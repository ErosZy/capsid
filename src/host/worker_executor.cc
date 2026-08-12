// WorkerExecutor implementation — see worker_executor.h. Extracted from
// single_worker_server.cc (WP-04 §8.1): everything that talks to one
// capsid_worker process. The owner thread submits commands and drains
// events; the worker thread owns the capsid_worker exclusively and reaps
// it (capsid_worker_destroy) in its exit path — the only destroy site.

#include "host/worker_executor.h"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace capsid::host {

namespace {

using SteadyClock = std::chrono::steady_clock;

// M2 item 6 (design §7.4): the fixed small probe response cap. A probe
// body beyond this fails the verdict regardless of status; bodies under
// the worker's initial stream window flow without any credit grant.
inline constexpr std::size_t kProbeResponseBodyCap = 4096;

void write_stderr(std::string_view message) {
    std::fwrite(message.data(), 1, message.size(), stderr);
    std::fputc('\n', stderr);
}

}  // namespace

WorkerExecutor::~WorkerExecutor() {
    stop_and_join();
}

bool WorkerExecutor::start(const WorkerFactory& factory, std::string* error) {
    if (started_.exchange(true)) {
        if (error != nullptr) {
            *error = "executor already started";
        }
        return false;
    }
    capsid_worker* worker = nullptr;
    if (!factory(&worker, error)) {
        // The factory destroyed its own worker on failure (it never handed
        // ownership over), so there is nothing to reap and no thread to
        // join; stop_and_join() stays a safe no-op.
        return false;
    }
    worker_ = worker;
    source_.set_worker(worker);

    // The worker thread takes exclusive ownership of every further Runtime
    // API call (READY arrives through next_event).
    worker_thread_ = std::thread([this] { worker_thread_main(); });
    if (!wait_for_ready()) {
        // The worker thread owns the destroy: its exit path is the reaper,
        // and the bounded terminate backstop forces the blocking destroy to
        // finish promptly. The shutdown command wakes the blocking wait.
        stop_and_join();
        if (error != nullptr) {
            *error = "worker did not become READY";
        }
        return false;
    }
    return true;
}

bool WorkerExecutor::adopt(capsid_worker* worker, std::string* error) {
    if (started_.exchange(true)) {
        if (error != nullptr) {
            *error = "executor already started";
        }
        return false;
    }
    if (worker == nullptr) {
        if (error != nullptr) {
            *error = "adopt requires a live worker";
        }
        return false;
    }
    // The adopter consumed the READY handshake (including the compatibility
    // check); ownership transfers here, and from now on destroy runs only
    // on this executor's reaper thread.
    worker_ = worker;
    source_.set_worker(worker);
    {
        std::unique_lock<std::mutex> lock(mutex_);
        ready_ = true;
        ready_match_ = true;
    }
    worker_thread_ = std::thread([this] { worker_thread_main(); });
    return true;
}

void WorkerExecutor::request_shutdown() {
    Command shutdown;
    shutdown.type = CommandType::kShutdown;
    if (!shutdown_requested_.exchange(true)) {
        submit(std::move(shutdown));
    }
}

void WorkerExecutor::stop_and_join() {
    request_shutdown();
    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }
}

void WorkerExecutor::submit(Command command) {
    if (metrics_enabled_.load(std::memory_order_relaxed)) {
        metrics_.commands_submitted.fetch_add(1, std::memory_order_relaxed);
    }
    const bool counts_inflight =
        command.type == CommandType::kBeginRequest;
    const std::uint64_t request_id = command.request_id;
    std::unique_lock<std::mutex> lock(mutex_);
    if (counts_inflight && inflight_ids_.insert(request_id).second) {
        inflight_.fetch_add(1, std::memory_order_relaxed);
    }
    const bool was_empty = commands_.empty();
    commands_.push_back(std::move(command));
    if (was_empty) {
        if (metrics_enabled_.load(std::memory_order_relaxed)) {
            metrics_.command_batches.fetch_add(1,
                                               std::memory_order_relaxed);
        }
        // Wake the worker thread out of its blocking wait. The byte is
        // written on the empty→non-empty transition only: a push into a
        // non-empty queue is already covered by an outstanding byte (or the
        // worker is draining), so the wake pipe cannot fill.
        source_.wake();
    }
    if (metrics_enabled_.load(std::memory_order_relaxed)) {
        {
            const std::uint64_t size =
                static_cast<std::uint64_t>(commands_.size());
            std::uint64_t hw = metrics_.command_queue_high_water.load(
                std::memory_order_relaxed);
            while (hw < size &&
                   !metrics_.command_queue_high_water.compare_exchange_weak(
                       hw, size, std::memory_order_relaxed)) {
            }
        }
    }
    cv_.notify_one();
}

void WorkerExecutor::submit_probe(const std::string& url) {
    Command probe;
    probe.type = CommandType::kProbeRequest;
    probe.url = url;
    probe.probe_id = next_probe_id_.fetch_add(1, std::memory_order_relaxed);
    {
        std::unique_lock<std::mutex> lock(mutex_);
        // A fresh probe: any earlier verdict is stale (the supervisor
        // consumed or cancelled it). The worker thread clears the rest on
        // execute.
        probe_in_flight_ = true;
        probe_complete_ = false;
    }
    submit(std::move(probe));
}

void WorkerExecutor::cancel_probe() {
    Command cancel;
    cancel.type = CommandType::kProbeCancel;
    submit(std::move(cancel));
}

ProbeState WorkerExecutor::probe_state() const {
    std::unique_lock<std::mutex> lock(mutex_);
    ProbeState state;
    state.in_flight = probe_in_flight_;
    state.complete = probe_complete_;
    state.healthy = probe_healthy_;
    state.status = probe_status_;
    state.body_bytes = probe_body_bytes_;
    return state;
}

void WorkerExecutor::mark_canceled(std::uint64_t request_id) {
    std::unique_lock<std::mutex> lock(mutex_);
    canceled_.insert(request_id);
    // The inflight slot is released exactly once per begun request: the
    // first terminal point (owner cancel, or the worker-side RESPONSE_END /
    // REQUEST_TIMEOUT) wins; a cancel racing an already-queued RESPONSE_END
    // cannot double-count.
    if (inflight_ids_.erase(request_id) == 1) {
        inflight_.fetch_sub(1, std::memory_order_relaxed);
    }
}

void WorkerExecutor::set_event_notifier(EventNotifier notifier) {
    notifier_ = std::move(notifier);
}

std::deque<WorkerEvent> WorkerExecutor::drain_events() {
    std::unique_lock<std::mutex> lock(mutex_);
    std::deque<WorkerEvent> local;
    local.swap(events_);
    return local;
}

bool WorkerExecutor::available() const {
    std::unique_lock<std::mutex> lock(mutex_);
    return ready_ && !exited_;
}

bool WorkerExecutor::exited() const {
    std::unique_lock<std::mutex> lock(mutex_);
    return exited_;
}

std::uint64_t WorkerExecutor::inflight() const {
    return inflight_.load(std::memory_order_relaxed);
}

WorkerExecutor::Metrics& WorkerExecutor::metrics() {
    return metrics_;
}

void WorkerExecutor::set_metrics_enabled(bool enabled) {
    // Relaxed: the flag is a metrics gate, and the store is ordered against
    // the mutex the readers hold (or is set before any thread exists).
    metrics_enabled_.store(enabled, std::memory_order_relaxed);
}

capsid_worker* WorkerExecutor::worker() const {
    std::unique_lock<std::mutex> lock(mutex_);
    // destroying_ gates the window between the destroy call and the
    // worker_ = nullptr store: a diagnostic snapshot must never touch
    // freed memory.
    return destroying_ ? nullptr : worker_;
}

void WorkerExecutor::worker_thread_main() {
    for (;;) {
        // Drain command wake bytes before swapping: a command pushed before
        // this drain is either already in the queue (the swap picks it up)
        // or was pushed after the swap (its wake byte is still in the pipe
        // and wakes the poll below). Draining first keeps both orderings
        // safe.
        source_.drain_wake_bytes();
        {
            std::unique_lock<std::mutex> lock(mutex_);
            std::deque<Command> local;
            local.swap(commands_);
            if (shutdown_deadline_.has_value() &&
                SteadyClock::now() >= *shutdown_deadline_) {
                if (worker_) {
                    capsid_worker_terminate(worker_);
                }
            }
            lock.unlock();
            for (Command& command : local) {
                if (!execute_command(std::move(command))) {
                    // A closed or wedged worker: fail every pending request
                    // on the io thread and reap below.
                    queue_worker_exit_event();
                    goto worker_exit;
                }
            }
            // Batch flush: every queued frame reaches the worker in one
            // write syscall instead of one per command.
            if (!batch_flush()) {
                queue_worker_exit_event();
                goto worker_exit;
            }
        }

        // Drain every pending event; next_event returns WOULD_BLOCK when the
        // IPC channel is idle.
        bool worker_exited = false;
        for (;;) {
            // struct_size is the size-negotiation contract of the Runtime
            // API: a value-uninitialized capsid_event reads as garbage and
            // next_event rejects it, so zero the struct and state its size.
            capsid_event event = {};
            event.struct_size = sizeof(event);
            const capsid_result result =
                capsid_worker_next_event(worker_, &event);
            if (result == CAPSID_OK) {
                if (!handle_worker_protocol_event(event)) {
                    worker_exited = true;
                    break;
                }
                continue;
            }
            if (result == CAPSID_WOULD_BLOCK) {
                break;
            }
            // The IPC channel is closed or corrupt: the worker is gone.
            queue_worker_exit_event();
            worker_exited = true;
            break;
        }
        if (worker_exited) {
            goto worker_exit;
        }

        // Block until the worker IPC descriptor is readable, a command wake
        // arrives, or the shutdown deadline passes. The owner thread's wake
        // byte keeps command pickup latency at poll latency instead of a
        // polling period; the worker thread never busy-polls.
        std::optional<SteadyClock::time_point> until;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (shutdown_deadline_.has_value()) {
                until = shutdown_deadline_;
            }
        }
        if (!source_.wait(until)) {
            queue_worker_exit_event();
            goto worker_exit;
        }
    }

worker_exit:
    // The worker thread remains the sole owner up to and including the
    // blocking destroy (§8.1: capsid_worker_destroy runs only on the
    // owner/reaper thread). destroying_ gates worker() during the destroy
    // window so a diagnostic snapshot can never touch freed memory.
    {
        std::unique_lock<std::mutex> lock(mutex_);
        destroying_ = true;
        // A worker that died mid-probe leaves no verdict: the supervisor
        // sees in_flight=false/complete=false and treats the probe as
        // failed (the pool owns the instability decision).
        probe_in_flight_ = false;
        probe_complete_ = false;
    }
    capsid_worker_destroy(worker_);
    {
        std::unique_lock<std::mutex> lock(mutex_);
        worker_ = nullptr;
        destroying_ = false;
        exited_ = true;
        // Every request still owned by this worker is gone with it; the
        // pool sees inflight 0 and routes new work to a replacement.
        inflight_ids_.clear();
    }
    inflight_.store(0, std::memory_order_relaxed);
    cv_.notify_all();
    // The exit event (and its notifier) fired before the blocking destroy
    // above; this second notify carries the exited_ flip. An owner whose
    // wait predicate reads exited() state — the generation pool's
    // drain-completion check — would otherwise miss the flip: it woke on
    // the exit event, saw exited_ still false, went back to sleep, and no
    // later event ever woke it.
    if (notifier_) {
        notifier_();
    }
}

// A Runtime call failed for a reason other than a full queue: the owner and
// the Runtime disagree about the request's state (WOULD_BLOCK on admission,
// INVALID_ARGUMENT for a frame the Runtime no longer accepts). The request
// fails closed on the client side instead of leaving the owner and the
// Runtime silently out of sync. Returns false when the worker itself is
// gone, which takes the exit path and fails every pending request.
bool WorkerExecutor::report_runtime_failure(std::uint64_t request_id,
                                            capsid_result result,
                                            const char* operation) {
    if (result == CAPSID_CLOSED) {
        return false;
    }
    write_stderr(std::string("capsid-host: worker ") + operation +
                 " failed: " + capsid_result_string(result) + " (request " +
                 std::to_string(request_id) + ")");
    WorkerEvent failure;
    failure.type = WorkerEvent::Type::kRequestFailure;
    failure.request_id = request_id;
    queue_worker_event(std::move(failure));
    return true;
}

bool WorkerExecutor::execute_command(Command command) {
    if (metrics_enabled_.load(std::memory_order_relaxed)) {
        metrics_.commands_executed.fetch_add(1, std::memory_order_relaxed);
    }
    // Commands for a cancelled request are dropped before execution: the
    // worker may have already expired the request (and erased its state), so
    // a late write/end/grant frame would be rejected as an invalid IPC frame
    // and kill the worker. The tombstone entry is removed again when the
    // matching cancel reaches the worker thread, so the set stays bounded by
    // the cancels still in flight.
    if (command.type != CommandType::kBeginRequest &&
        command.type != CommandType::kShutdown &&
        command.type != CommandType::kCancel &&
        command.type != CommandType::kProbeRequest &&
        command.type != CommandType::kProbeCancel) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (canceled_.count(command.request_id) != 0) {
            return true;
        }
    }
    switch (command.type) {
    case CommandType::kBeginRequest: {
        std::vector<capsid_header> headers;
        headers.reserve(command.headers.size());
        for (const auto& header : command.headers) {
            headers.push_back(capsid_header{
                {reinterpret_cast<const std::uint8_t*>(header.name.data()),
                 header.name.size()},
                {reinterpret_cast<const std::uint8_t*>(header.value.data()),
                 header.value.size()},
            });
        }
        const capsid_header* header_ptr =
            headers.empty() ? nullptr : headers.data();
        capsid_result result;
        if (command.end_request) {
            result = capsid_worker_begin_bodyless_request(
                worker_, command.request_id, command.method.c_str(),
                command.url.c_str(), header_ptr, headers.size());
        } else {
            result = capsid_worker_begin_request(
                worker_, command.request_id, command.method.c_str(),
                command.url.c_str(), header_ptr, headers.size());
        }
        if (result != CAPSID_OK) {
            return report_runtime_failure(command.request_id, result,
                                          "begin_request");
        }
        break;
    }
    case CommandType::kWriteRequest: {
        const capsid_result result = capsid_worker_write_request(
            worker_, command.request_id, command.body.data(),
            command.body.size());
        if (result == CAPSID_WOULD_BLOCK) {
            // The Runtime did not consume the chunk (credit or write queue
            // full); retry the same chunk before any later request-body
            // chunk. Nothing was queued, so no flush is needed.
            std::unique_lock<std::mutex> lock(mutex_);
            commands_.push_front(std::move(command));
            return true;
        }
        if (result != CAPSID_OK) {
            return report_runtime_failure(command.request_id, result,
                                          "write_request");
        }
        break;
    }
    case CommandType::kEndRequest: {
        const capsid_result result =
            capsid_worker_end_request(worker_, command.request_id);
        if (result != CAPSID_OK) {
            return report_runtime_failure(command.request_id, result,
                                          "end_request");
        }
        break;
    }
    case CommandType::kCancel: {
        const capsid_result result =
            capsid_worker_cancel(worker_, command.request_id);
        if (result == CAPSID_CLOSED) {
            return false;
        }
        if (result != CAPSID_OK) {
            // Cancel is best-effort: the Runtime already forgot the request
            // or the channel is wedged. Log and continue; the request is
            // gone from the owner either way.
            write_stderr(std::string("capsid-host: worker cancel failed: ") +
                         capsid_result_string(result) + " (request " +
                         std::to_string(command.request_id) + ")");
        } else {
            // The cancel reached the worker: the Runtime has forgotten this
            // request, so no further frame for it can be valid. The tombstone
            // entry is no longer needed; dropping it here bounds the set to
            // the cancels still in flight.
            std::unique_lock<std::mutex> lock(mutex_);
            canceled_.erase(command.request_id);
        }
        break;
    }
    case CommandType::kGrantResponseCredit: {
        if (metrics_enabled_.load(std::memory_order_relaxed)) {
            metrics_.grant_commands.fetch_add(1, std::memory_order_relaxed);
            metrics_.credit_bytes_granted.fetch_add(
                command.credit, std::memory_order_relaxed);
        }
        const capsid_result result = capsid_worker_grant_response_credit(
            worker_, command.request_id, command.credit);
        if (result == CAPSID_WOULD_BLOCK) {
            std::unique_lock<std::mutex> lock(mutex_);
            commands_.push_front(std::move(command));
            return true;
        }
        if (result != CAPSID_OK) {
            // Grants are only submitted for requests whose response has not
            // ended (see the write completion guard) and dropped by the
            // tombstone once a request ends or is cancelled, so any other
            // rejection is a genuine owner/Runtime state mismatch.
            return report_runtime_failure(command.request_id, result,
                                          "grant_response_credit");
        }
        break;
    }
    case CommandType::kShutdown:
        capsid_worker_shutdown(worker_);
        {
            std::unique_lock<std::mutex> lock(mutex_);
            shutdown_deadline_ = SteadyClock::now() + kShutdownGrace;
        }
        break;
    case CommandType::kProbeRequest: {
        // M2 item 6: one bodyless GET against the App's healthCheck path.
        // The probe id is owner-allocated from the reserved high range, so
        // it never collides with a data-plane id; the response events are
        // folded into probe_state() in handle_worker_protocol_event and
        // never reach the pool's event queue.
        const capsid_result result = capsid_worker_begin_bodyless_request(
            worker_, command.probe_id, "GET", command.url.c_str(), nullptr,
            0);
        {
            std::unique_lock<std::mutex> lock(mutex_);
            probe_id_ = command.probe_id;
            probe_in_flight_ = true;
            probe_complete_ = false;
            probe_status_ = 0;
            probe_body_bytes_ = 0;
        }
        if (result != CAPSID_OK) {
            // A failed probe delivery is a failed verdict (the supervisor
            // sees complete=false with in_flight=false after the clear
            // below). Never queue a kRequestFailure for the probe — it is
            // not a data-plane request and the listener would reject it.
            std::unique_lock<std::mutex> lock(mutex_);
            probe_in_flight_ = false;
            lock.unlock();
            if (result == CAPSID_CLOSED) {
                return false;  // the worker is gone; the exit path reaps it
            }
            write_stderr(std::string("capsid-host: worker probe failed: ") +
                         capsid_result_string(result));
        }
        break;
    }
    case CommandType::kProbeCancel: {
        std::unique_lock<std::mutex> lock(mutex_);
        const bool outstanding = probe_in_flight_;
        probe_in_flight_ = false;
        probe_complete_ = false;
        const std::uint64_t probe_id = probe_id_;
        lock.unlock();
        if (outstanding) {
            // Best-effort: the Runtime may already have forgotten the
            // probe (a terminal event raced the cancel).
            (void)capsid_worker_cancel(worker_, probe_id);
        }
        break;
    }
    }
    // Frames were queued; the caller (worker thread main loop) batches the
    // flush across all commands of this batch.
    return true;
}

// batch_flush sends every queued frame that the current command batch
// produced. It is called once per command drain, not per command, so a
// batch of N GET requests produces one write syscall instead of N.
bool WorkerExecutor::batch_flush() {
    const capsid_result result = capsid_worker_flush(worker_);
    if (result == CAPSID_OK || result == CAPSID_WOULD_BLOCK) {
        // WOULD_BLOCK is fine — the frames are in the worker's input buffer;
        // the next batch's poll/wait will drain and flush more.
        return true;
    }
    write_stderr(std::string("capsid-host: worker batch flush failed: ") +
                 capsid_result_string(result));
    return false;
}

bool WorkerExecutor::handle_worker_protocol_event(const capsid_event& event) {
    // M2 item 6: fold probe response events into probe_state() on the
    // worker thread. The probe id is from the reserved high range, so no
    // data-plane event can match it; a terminal verdict or the fixed body
    // cap completes the probe without a data-plane event.
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (probe_in_flight_ && event.request_id == probe_id_) {
            switch (event.type) {
            case CAPSID_EVENT_RESPONSE_HEAD:
                probe_status_ = static_cast<std::int32_t>(event.status);
                break;
            case CAPSID_EVENT_RESPONSE_BODY:
                probe_body_bytes_ += event.payload.size;
                if (probe_body_bytes_ > kProbeResponseBodyCap) {
                    // §7.4: the fixed small response cap is a protocol
                    // failure; the verdict fails and the supervisor
                    // cancels the outstanding request.
                    probe_complete_ = true;
                    probe_healthy_ = false;
                }
                break;
            case CAPSID_EVENT_RESPONSE_END:
                probe_complete_ = true;
                probe_healthy_ = probe_status_ >= 200 &&
                                 probe_status_ <= 299 &&
                                 probe_body_bytes_ <= kProbeResponseBodyCap;
                break;
            case CAPSID_EVENT_REQUEST_TIMEOUT:
                probe_complete_ = true;
                probe_healthy_ = false;
                break;
            default:
                break;
            }
            return true;
        }
    }
    switch (event.type) {
    case CAPSID_EVENT_READY: {
        std::string payload(
            reinterpret_cast<const char*>(event.payload.data),
            event.payload.size);
        capsid_build_info info;
        capsid_build_info_init(&info);
        const capsid_result result = capsid_runtime_build_info(&info);
        bool match = false;
        if (result == CAPSID_OK && info.compatibility_id != nullptr) {
            match = payload == info.compatibility_id;
        }
        std::unique_lock<std::mutex> lock(mutex_);
        ready_ = true;
        ready_match_ = match;
        cv_.notify_all();
        return true;
    }
    case CAPSID_EVENT_REQUEST_CREDIT: {
        WorkerEvent worker_event;
        worker_event.type = WorkerEvent::Type::kRequestCredit;
        worker_event.request_id = event.request_id;
        worker_event.credit = event.credit;
        queue_worker_event(std::move(worker_event));
        return true;
    }
    case CAPSID_EVENT_RESPONSE_HEAD: {
        WorkerEvent worker_event;
        worker_event.type = WorkerEvent::Type::kResponseHead;
        worker_event.request_id = event.request_id;
        worker_event.status = static_cast<std::uint16_t>(event.status);
        worker_event.fixed_body =
            (event.flags & CAPSID_RESPONSE_HEAD_FLAG_FIXED_BODY) != 0;
        worker_event.fixed_body_size =
            worker_event.fixed_body ? event.credit : 0;
        std::size_t count = 0;
        if (capsid_response_header_count(&event, &count) == CAPSID_OK) {
            worker_event.headers.reserve(count);
            for (std::size_t index = 0; index < count; ++index) {
                capsid_header header;
                if (capsid_response_header_at(&event, index, &header) !=
                    CAPSID_OK) {
                    continue;
                }
                worker_event.headers.emplace_back(
                    std::string(reinterpret_cast<const char*>(
                                    header.name.data),
                                header.name.size),
                    std::string(reinterpret_cast<const char*>(
                                    header.value.data),
                                header.value.size));
            }
        }
        queue_worker_event(std::move(worker_event));
        return true;
    }
    case CAPSID_EVENT_RESPONSE_BODY: {
        WorkerEvent worker_event;
        worker_event.type = WorkerEvent::Type::kResponseBody;
        worker_event.request_id = event.request_id;
        worker_event.body.assign(event.payload.data,
                                 event.payload.data + event.payload.size);
        queue_worker_event(std::move(worker_event));
        return true;
    }
    case CAPSID_EVENT_RESPONSE_END: {
        // The Runtime erased this request when it sent RESPONSE_END, so any
        // command still queued for it (a response-credit grant most likely)
        // would be rejected by the Runtime ABI as an invalid frame and logged
        // as an internal state error. Mark the request terminal here, on the
        // worker thread, before the event is handed to the owner thread: the
        // drop check in execute_command then covers the window until the
        // owner's kResponseEnd handler inserts the same tombstone and the
        // kCancel marker removes it again. The inflight slot is released
        // exactly once.
        mark_canceled(event.request_id);
        WorkerEvent worker_event;
        worker_event.type = WorkerEvent::Type::kResponseEnd;
        worker_event.request_id = event.request_id;
        queue_worker_event(std::move(worker_event));
        return true;
    }
    case CAPSID_EVENT_LOG: {
        WorkerEvent worker_event;
        worker_event.type = WorkerEvent::Type::kLog;
        worker_event.text.assign(
            reinterpret_cast<const char*>(event.payload.data),
            event.payload.size);
        queue_worker_event(std::move(worker_event));
        return true;
    }
    case CAPSID_EVENT_ERROR: {
        WorkerEvent worker_event;
        worker_event.type = WorkerEvent::Type::kError;
        worker_event.request_id = event.request_id;
        worker_event.text.assign(
            reinterpret_cast<const char*>(event.payload.data),
            event.payload.size);
        queue_worker_event(std::move(worker_event));
        return true;
    }
    case CAPSID_EVENT_REQUEST_TIMEOUT: {
        // Mark the request cancelled on the worker thread before the timeout
        // event is delivered: the worker has already erased the request, so
        // any late command for it must be dropped before it reaches the IPC
        // channel or the worker rejects it as an invalid frame.
        mark_canceled(event.request_id);
        WorkerEvent worker_event;
        worker_event.type = WorkerEvent::Type::kRequestTimeout;
        worker_event.request_id = event.request_id;
        queue_worker_event(std::move(worker_event));
        return true;
    }
    case CAPSID_EVENT_EXIT:
        queue_worker_exit_event();
        return false;
    default:
        // AUDIT and MEMORY_METRICS are not part of the M1A data plane.
        return true;
    }
}

void WorkerExecutor::queue_worker_event(WorkerEvent event) {
    bool need_notify = false;
    {
        // The event-queue size must be read under the mutex: this function
        // runs on both the owner thread and the worker thread, and the other
        // side may be mutating events_ concurrently.
        std::unique_lock<std::mutex> lock(mutex_);
        if (metrics_enabled_.load(std::memory_order_relaxed)) {
            metrics_.events_queued.fetch_add(1, std::memory_order_relaxed);
            {
                const std::uint64_t size =
                    static_cast<std::uint64_t>(events_.size() + 1);
                std::uint64_t hw = metrics_.event_queue_high_water.load(
                    std::memory_order_relaxed);
                while (hw < size &&
                       !metrics_.event_queue_high_water.compare_exchange_weak(
                           hw, size, std::memory_order_relaxed)) {
                }
            }
        }
        need_notify = events_.empty();
        events_.push_back(std::move(event));
    }
    // Only notify when the queue was empty — the owner drains everything
    // in one call. New events that arrive while the drain is running will
    // see the (now-empty) queue and schedule their own notify.
    if (need_notify && notifier_) {
        notifier_();
    }
}

void WorkerExecutor::queue_worker_exit_event() {
    bool notify = false;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!exit_event_queued_) {
            WorkerEvent exit_event;
            exit_event.type = WorkerEvent::Type::kExit;
            events_.push_back(std::move(exit_event));
            exit_event_queued_ = true;
            // The exit event is always the last one, so a notify is needed
            // even if the queue was non-empty — the owner may already be
            // draining earlier queued events and won't see this one without
            // a notify.
            notify = true;
        }
    }
    if (notify && notifier_) {
        notifier_();
    }
}

bool WorkerExecutor::wait_for_ready() {
    std::unique_lock<std::mutex> lock(mutex_);
    const bool notified = cv_.wait_for(lock, kReadyTimeout, [this] {
        return ready_ || exited_;
    });
    if (!notified) {
        write_stderr("capsid-host: worker did not become READY in time");
        return false;
    }
    if (exited_) {
        write_stderr("capsid-host: worker exited before READY");
        return false;
    }
    if (!ready_match_) {
        write_stderr("capsid-host: worker compatibility ID mismatch");
        return false;
    }
    return true;
}

}  // namespace capsid::host
