// WorkerExecutor implementation — see worker_executor.h. Extracted from
// single_worker_server.cc (WP-04 §8.1): everything that talks to one
// capsid_worker process. The owner thread submits commands and drains
// events; the worker thread owns the capsid_worker exclusively and reaps
// it (capsid_worker_destroy) in its exit path — the only destroy site.

#include "host/worker_executor.h"
#include <stdexcept>
#include "host/websocket_dialer.h"
#include "client_channel_state.h"

#include "host/binding_compile.h"
#include "host/config.h"

#include <jansson.h>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
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

bool decode_worker_log_event(const capsid_event& event,
                             WorkerEvent* output,
                             std::string* error) {
    if (output == nullptr || error == nullptr ||
        event.type != CAPSID_EVENT_LOG) {
        return false;
    }
    *output = WorkerEvent();
    output->type = WorkerEvent::Type::kLog;
    output->request_id = event.request_id;
    error->clear();
    if (event.flags == 0) {
        if (event.payload.size != 0 && event.payload.data == nullptr) {
            *error = "invalid application log payload";
            return false;
        }
        if (event.payload.size != 0) {
            output->text.assign(
                reinterpret_cast<const char*>(event.payload.data),
                event.payload.size);
        }
        return true;
    }
    if (event.flags != capsid::protocol::kFlagBindingLog ||
        event.payload.size == 0 || event.payload.data == nullptr) {
        *error = "invalid Binding log flags or payload";
        return false;
    }
    const std::uint8_t* cursor = event.payload.data;
    const std::uint8_t* end = cursor + event.payload.size;
    const auto read_string16 = [&cursor, end](std::string* value) {
        std::uint16_t size = 0;
        if (!capsid::protocol::read_u16(&cursor, end, &size) ||
            static_cast<std::size_t>(end - cursor) < size) {
            return false;
        }
        value->assign(reinterpret_cast<const char*>(cursor), size);
        cursor += size;
        return true;
    };
    const auto read_string32 = [&cursor, end](std::string* value) {
        std::uint32_t size = 0;
        if (!capsid::protocol::read_u32(&cursor, end, &size) ||
            static_cast<std::size_t>(end - cursor) < size) {
            return false;
        }
        value->assign(reinterpret_cast<const char*>(cursor), size);
        cursor += size;
        return true;
    };
    if (!read_string16(&output->binding_id) ||
        !read_string16(&output->log_level) ||
        !read_string32(&output->text) ||
        !read_string32(&output->log_fields_json) || cursor != end ||
        !valid_binding_id(output->binding_id) ||
        (output->log_level != "debug" && output->log_level != "info" &&
         output->log_level != "warn" && output->log_level != "error") ||
        output->text.size() > 16U * 1024U ||
        output->log_fields_json.size() > 16U * 1024U ||
        output->log_fields_json.size() < 2 ||
        output->log_fields_json.front() != '{' ||
        output->log_fields_json.back() != '}') {
        *error = "invalid Binding log envelope";
        return false;
    }
    json_error_t json_error = {};
    json_t* fields = json_loadb(
        output->log_fields_json.data(), output->log_fields_json.size(),
        JSON_REJECT_DUPLICATES | JSON_DECODE_ANY, &json_error);
    if (fields == nullptr || !json_is_object(fields)) {
        if (fields != nullptr) {
            json_decref(fields);
        }
        *error = "invalid Binding log fields";
        return false;
    }
    char* canonical_fields = json_dumps(
        fields, JSON_COMPACT | JSON_ENSURE_ASCII | JSON_SORT_KEYS);
    json_decref(fields);
    if (canonical_fields == nullptr) {
        *error = "cannot canonicalize Binding log fields";
        return false;
    }
    output->log_fields_json.assign(canonical_fields);
    std::free(canonical_fields);
    if (output->log_fields_json.size() > 16U * 1024U) {
        *error = "canonical Binding log fields exceed the size limit";
        return false;
    }
    output->binding_log = true;
    return true;
}

WorkerExecutor::WorkerExecutor() = default;
void WorkerExecutor::set_channel_budget(std::shared_ptr<ServiceChannelBudget> budget) {
    if (started_) throw std::logic_error("channel budget must be set before worker startup");
    channel_budget_ = std::move(budget);
}
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
    expected_ready_ = factory.expected_ready;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        worker_ = worker;
        process_id_.store(capsid_worker_pid(worker));
    }
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
            std::unique_lock<std::mutex> lock(mutex_);
            *error = ready_error_.empty()
                ? "worker did not become READY"
                : ready_error_;
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
    source_.set_worker(worker);
    {
        std::unique_lock<std::mutex> lock(mutex_);
        worker_ = worker;
        process_id_.store(capsid_worker_pid(worker));
        capsid_protocol_info info;
        if (capsid_worker_get_protocol_info(worker, &info, sizeof(info)) == CAPSID_OK)
            effective_channel_caps_ = info.effective_capabilities;
        capsid::client_channel_limits(worker,&channel_limits_);
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
#ifdef CAPSID_HOST_NETWORKING
    if(outbound_) outbound_->stop_and_join();
#endif
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
        inflight_.fetch_add(1, std::memory_order_acq_rel);
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

bool WorkerExecutor::try_submit_channel(Command command) {
    const size_t limit = command.type == CommandType::kChannelData ? 16u*1024u*1024u - 65536 : 16u*1024u*1024u;
    size_t charge = 1024;
    const auto add_charge = [&](size_t bytes) {
        if (bytes > limit - charge) return false;
        charge += bytes; return true;
    };
    if (!add_charge(command.body.size()) || !add_charge(command.close_reason.size()) ||
        !add_charge(command.http_response.url.size()) || !add_charge(command.http_response.status_text.size())) return false;
    for (const auto &header : command.http_response.headers)
        if (!add_charge(header.first.size()) || !add_charge(header.second.size()) || !add_charge(64)) return false;
    std::unique_lock<std::mutex> lock(mutex_);
    if (exited_ || charge > limit || channel_command_bytes_ > limit - charge) return false;
    const bool was_empty = commands_.empty();
    command.channel_charge = charge;
    commands_.push_back(std::move(command));
    channel_command_bytes_ += charge;
    if (was_empty) source_.wake();
    cv_.notify_one();
    return true;
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
        inflight_.fetch_sub(1, std::memory_order_acq_rel);
    }
}

void WorkerExecutor::retire_terminal_request(std::uint64_t request_id) {
    std::unique_lock<std::mutex> lock(mutex_);
    canceled_.insert(request_id);
    if (inflight_ids_.erase(request_id) == 1) {
        inflight_.fetch_sub(1, std::memory_order_acq_rel);
    }
    // Anything still in the shared queue has not reached the Runtime and is
    // stale now. A command already swapped into the worker's local batch is
    // protected by canceled_ until that batch reaches the retirement point
    // below.
    commands_.erase(
        std::remove_if(commands_.begin(), commands_.end(),
                       [request_id](const Command& command) {
                           return command.request_id == request_id;
                       }),
        commands_.end());
    retire_pending_.insert(request_id);
}

void WorkerExecutor::set_event_notifier(EventNotifier notifier) {
    notifier_ = std::move(notifier);
}

void WorkerExecutor::set_log_identity(std::string application_id,
                                      std::string generation_digest) {
    if (started_.load(std::memory_order_relaxed)) {
        return;
    }
    log_application_id_ = std::move(application_id);
    log_generation_digest_ = std::move(generation_digest);
}

std::deque<WorkerEvent> WorkerExecutor::drain_events() {
    std::unique_lock<std::mutex> lock(mutex_);
    std::deque<WorkerEvent> local;
    local.swap(events_);
    for (const auto &event:local) channel_event_bytes_ -= event.channel_charge;
    return local;
}

bool WorkerExecutor::available() const {
    std::unique_lock<std::mutex> lock(mutex_);
    return ready_ && !exit_event_queued_ && !destroying_ && !exited_;
}

bool WorkerExecutor::exited() const {
    std::unique_lock<std::mutex> lock(mutex_);
    return exited_;
}

std::uint64_t WorkerExecutor::inflight() const {
    return inflight_.load(std::memory_order_acquire);
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
    // This accessor only observes publication. A diagnostic reader must
    // use snapshot_client_metrics(), which holds the owner lock through
    // the actual reads instead of borrowing this pointer.
    return destroying_ ? nullptr : worker_;
}

bool WorkerExecutor::snapshot_client_metrics(capsid::ClientIpcMetrics* metrics) {
    if (metrics == nullptr) {
        return false;
    }
    std::unique_lock<std::mutex> lock(mutex_);
    if (worker_ != nullptr && !destroying_) {
        return capsid::client_ipc_metrics_snapshot(worker_, metrics);
    }
    return final_client_metrics_valid_ &&
        capsid::client_ipc_metrics_drain(&final_client_metrics_, metrics);
}

capsid::WorkerExitStatus WorkerExecutor::exit_status() const {
    std::unique_lock<std::mutex> lock(mutex_);
    return exit_status_;
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
            while (!local.empty()) {
                Command command = std::move(local.front()); local.pop_front();
                const size_t charge = command.channel_charge;
                command_would_block_ = false;
                if (!execute_command(std::move(command))) {
                    // A closed or wedged worker: fail every pending request
                    // on the io thread and reap below.
                    queue_worker_exit_event();
                    goto worker_exit;
                }
                if (command_would_block_) {
                    std::unique_lock<std::mutex> retry_lock(mutex_);
                    commands_.insert(commands_.begin()+1, std::make_move_iterator(local.begin()), std::make_move_iterator(local.end()));
                    break;
                }
                if (charge) {
                    std::unique_lock<std::mutex> charge_lock(mutex_);
                    channel_command_bytes_ -= charge;
                }
            }
            // Batch flush: every queued frame reaches the worker in one
            // write syscall instead of one per command.
            if (!batch_flush()) {
                queue_worker_exit_event();
                goto worker_exit;
            }
            if (notifier_) notifier_(); // channel capacity may have returned without a byte event
            // The owner has observed these Runtime-terminal requests and
            // promised not to submit another frame for them. Clearing after
            // the local batch also covers stale commands that were swapped
            // out before the acknowledgement.
            {
                std::unique_lock<std::mutex> retire_lock(mutex_);
                for (const std::uint64_t request_id : retire_pending_) {
                    canceled_.erase(request_id);
                }
                retire_pending_.clear();
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
        const bool waited = ipc_write_pending_
            ? source_.wait_writable_or_wake(until) != WorkerEventSource::WakeResult::kError
            : source_.wait(until);
        if (!waited) {
            queue_worker_exit_event();
            goto worker_exit;
        }
    }

worker_exit:
#ifdef CAPSID_HOST_NETWORKING
    if(outbound_) outbound_->request_stop();
#endif
    // The worker thread remains the sole owner up to and including the
    // blocking destroy (§8.1: capsid_worker_destroy runs only on the
    // owner/reaper thread). Snapshot and unpublication share the same
    // lock. The abortive destroy only closes/signals/reaps: every IPC
    // increment has happened before this final transfer.
    capsid_worker* retired_worker = nullptr;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        destroying_ = true;
        retired_worker = worker_;
        final_client_metrics_valid_ = capsid::client_ipc_metrics_snapshot(
            retired_worker, &final_client_metrics_);
        worker_ = nullptr;
        // A worker that died mid-probe leaves no verdict: the supervisor
        // sees in_flight=false/complete=false and treats the probe as
        // failed (the pool owns the instability decision).
        probe_in_flight_ = false;
        probe_complete_ = false;
    }
    source_.set_worker(nullptr);
    const capsid::WorkerExitStatus status =
        capsid::destroy_worker_with_status(retired_worker);
    {
        std::unique_lock<std::mutex> lock(mutex_);
        exit_status_ = status;
        worker_ = nullptr;
        destroying_ = false;
        exited_ = true;
        // Every request still owned by this worker is gone with it; the
        // pool sees inflight 0 and routes new work to a replacement.
        inflight_ids_.clear();
    }
    retained_contexts_.store(0, std::memory_order_relaxed);
    publish_channels(0);
    inflight_.store(0, std::memory_order_release);
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
    // and kill the worker. An explicit owner cancel removes its tombstone
    // when kCancel reaches this thread; a Runtime-terminal request removes
    // it at the safe post-batch retirement point.
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
        if (command.upgrade_request && command.channel_id == 0) {
            if (next_channel_id_ == 0) return report_runtime_failure(command.request_id, CAPSID_INVALID_ARGUMENT, "channel id exhaustion");
            command.channel_id = next_channel_id_++;
            const auto reserved = capsid_worker_upgrade_begin(worker_, command.request_id, command.channel_id, command.channel_max_message);
            if (reserved != CAPSID_OK) return report_runtime_failure(command.request_id, reserved, "upgrade reserve");
        }
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
        if (result == CAPSID_WOULD_BLOCK) {
            std::unique_lock<std::mutex> lock(mutex_);
            commands_.push_front(std::move(command)); command_would_block_ = true;
            return true;
        }
        if (result != CAPSID_OK) {
            if (command.upgrade_request) (void)capsid_worker_channel_cancel(worker_,command.channel_id,CAPSID_CANCEL_REASON_LOCAL);
            return report_runtime_failure(command.request_id, result,
                                          "begin_request");
        }
        if (command.upgrade_request) {
            live_channel_ids_.insert(command.channel_id);
            publish_channels(live_channel_ids_.size());
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
            command_would_block_ = true;
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
            command_would_block_ = true;
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
    case CommandType::kChannelData:
    case CommandType::kChannelGrant:
    case CommandType::kChannelClose:
    case CommandType::kChannelCancel:
    case CommandType::kChannelConfirm: {
        capsid::ChannelEntry entry;
        if (!command.channel_id && command.type==CommandType::kChannelConfirm) {
            bool cancelled=false;
            if(!capsid::client_pending_dial(worker_,command.dial_id,&entry,&cancelled) || !cancelled) return true;
        } else if (!capsid::client_channel_snapshot(worker_, command.channel_id, &entry)) return true;
        const auto &host = entry.direction[0];
        const auto &peer = entry.direction[1];
        if (command.type != CommandType::kChannelConfirm &&
            (host.close_sent || host.terminal_sent || entry.phase == capsid::ChannelPhase::Cancelling)) return true;
        capsid_result result = CAPSID_INVALID_ARGUMENT;
        switch (command.type) {
        case CommandType::kChannelData:
            result = capsid_worker_channel_send(worker_, command.channel_id, command.body.data(), command.body.size(), command.channel_flags); break;
        case CommandType::kChannelGrant:
            result = capsid_worker_channel_grant(worker_, command.channel_id, command.channel_credit); break;
        case CommandType::kChannelClose:
            result = capsid_worker_channel_close_bytes(worker_, command.channel_id, command.close_kind, command.close_code,
                reinterpret_cast<const uint8_t *>(command.close_reason.data()),command.close_reason.size()); break;
        case CommandType::kChannelCancel:
            result = capsid_worker_channel_cancel(worker_, command.channel_id, command.channel_flags); break;
        case CommandType::kChannelConfirm:
            if (command.channel_id && (!peer.terminal_sent || host.terminal_ack_sent)) return true;
            result = capsid_worker_channel_confirm(worker_, command.channel_id, command.dial_id, command.channel_flags); break;
        default: break;
        }
        if (result == CAPSID_WOULD_BLOCK) {
            std::unique_lock<std::mutex> lock(mutex_);
            commands_.push_front(std::move(command)); command_would_block_ = true;
            return true;
        }
        if (result != CAPSID_OK) return false;
        refresh_channels();
        break;
    }
    case CommandType::kDialAccept:
    case CommandType::kDialReject: {
        capsid::ChannelEntry entry; bool cancelled=false,unresolved=false;
        if(!capsid::client_pending_dial(worker_,command.dial_id,&entry,&cancelled,&unresolved) || cancelled || !unresolved) return true;
        uint32_t rejection=8;
        const bool capacity=live_channel_ids_.size()>=channel_limits_.max_channels ||
            capsid::client_dial_must_reject(worker_,command.dial_id,&rejection);
        if(command.type==CommandType::kDialAccept && (capacity || !next_channel_id_)) {
            command.type=CommandType::kDialReject; command.channel_flags=rejection;
            command.close_reason="WebSocket worker channel capacity exhausted";
        }
        const bool accepting=command.type==CommandType::kDialAccept;
        const uint32_t assigned=accepting ? next_channel_id_ : 0;
        capsid_result result;
        if (accepting && command.fetch_upgrade) {
            std::vector<capsid_header> headers; headers.reserve(command.http_response.headers.size());
            for (const auto &header : command.http_response.headers) headers.push_back({
                {reinterpret_cast<const uint8_t *>(header.first.data()),header.first.size()},
                {reinterpret_cast<const uint8_t *>(header.second.data()),header.second.size()}});
            capsid_fetch_upgrade_response response{}; response.struct_size=sizeof(response);
            response.version=CAPSID_FETCH_UPGRADE_RESPONSE_VERSION; response.status=command.http_response.status;
            response.redirected=command.http_response.redirected;
            response.status_text={reinterpret_cast<const uint8_t *>(command.http_response.status_text.data()),command.http_response.status_text.size()};
            response.url={reinterpret_cast<const uint8_t *>(command.http_response.url.data()),command.http_response.url.size()};
            response.headers=headers.data(); response.header_count=headers.size();
            result=capsid_worker_dial_accept_http(worker_,command.dial_id,assigned,command.channel_max_message,&response);
        } else if (accepting) result=capsid_worker_dial_accept(worker_,command.dial_id,assigned,command.channel_max_message,command.close_reason.c_str());
        else result=capsid_worker_dial_reject(worker_,command.dial_id,command.channel_flags,command.close_reason.c_str());
        if(result==CAPSID_WOULD_BLOCK) {
            std::unique_lock<std::mutex> lock(mutex_); commands_.push_front(std::move(command)); command_would_block_=true; return true;
        }
        if(result!=CAPSID_OK) return false;
        if(accepting) { ++next_channel_id_; live_channel_ids_.insert(assigned); outbound_channel_ids_.insert(assigned); }
        WorkerEvent ready; ready.type=WorkerEvent::Type::kChannelReady; ready.outbound=true;
        ready.dial_id=command.dial_id; ready.channel_id=assigned; queue_worker_event(std::move(ready));
        refresh_channels(); break;
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
    ipc_write_pending_ = result == CAPSID_WOULD_BLOCK;
    if (result == CAPSID_OK || result == CAPSID_WOULD_BLOCK) {
        // WOULD_BLOCK is fine — the frames are in the worker's input buffer;
        // the next batch's poll/wait will drain and flush more.
        return true;
    }
    write_stderr(std::string("capsid-host: worker batch flush failed: ") +
                 capsid_result_string(result));
    return false;
}

void WorkerExecutor::publish_channels(std::uint64_t count) {
    channels_.store(count, std::memory_order_relaxed);
    connection_owners_.store(count+retained_contexts_.load(std::memory_order_relaxed),std::memory_order_release);
}

void WorkerExecutor::refresh_channels() {
    for (auto it = live_channel_ids_.begin(); it != live_channel_ids_.end();) {
        if (!capsid::client_channel_snapshot(worker_, *it, nullptr)) {
            quarantined_channel_ids_.erase(*it); outbound_channel_ids_.erase(*it); it = live_channel_ids_.erase(it);
        }
        else ++it;
    }
    size_t pending_count=0;
    for(auto it=outbound_dial_ids_.begin();it!=outbound_dial_ids_.end();) {
        capsid::ChannelEntry entry;
        if(!capsid::client_pending_dial(worker_,*it,&entry)) it=outbound_dial_ids_.erase(it);
        else { if(!entry.channel_id) ++pending_count; ++it; }
    }
    publish_channels(live_channel_ids_.size()+pending_count);
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
        // GLM T08: capture the negotiated channel capability set (the
        // client already split and stripped the v4 tail; the typed
        // getter is the only reader). Zero = channels unavailable.
        {
            capsid_protocol_info protocol_info;
            if (capsid_worker_get_protocol_info(
                    worker_, &protocol_info, sizeof(protocol_info)) ==
                CAPSID_OK) {
                effective_channel_caps_ =
                    protocol_info.effective_capabilities;
                capsid::client_channel_limits(worker_,&channel_limits_);
            }
        }
        // §4.3: the payload is the 71-byte compatibility id plus, for
        // workers with bindings, the sandbox proof. The id prefix must
        // match exactly; a malformed extension fails the ready match.
        capsid::WorkerReadyProof proof;
        std::string compat_id;
        std::string proof_error;
        const std::vector<uint8_t> ready_payload(
            event.payload.data,
            event.payload.data + event.payload.size);
        const bool parsed = capsid::parse_ready_proof(
            ready_payload, &compat_id, &proof, &proof_error);
        capsid_build_info info;
        capsid_build_info_init(&info);
        const capsid_result result = capsid_runtime_build_info(&info);
        bool match = false;
        std::string ready_error = proof_error;
        if (parsed && result == CAPSID_OK &&
            info.compatibility_id != nullptr) {
            match = capsid::host::verify_worker_ready(
                ready_payload,
                info.compatibility_id,
                expected_ready_,
                &ready_error);
        }
        std::unique_lock<std::mutex> lock(mutex_);
        ready_ = true;
        ready_match_ = match;
        ready_error_ = match ? std::string() : ready_error;
        ready_proof_ = proof;
        cv_.notify_all();
        return true;
    }
    case CAPSID_EVENT_CONNECTION_LIFETIME:
        retained_contexts_.store(event.credit, std::memory_order_relaxed);
        publish_channels(channels_.load(std::memory_order_relaxed));
        if (notifier_) notifier_();
        return true;
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
        worker_event.upgrade = (event.flags & CAPSID_RESPONSE_HEAD_FLAG_UPGRADE) != 0;
        worker_event.channel_id = worker_event.upgrade ? event.credit : 0;
        if (worker_event.upgrade) mark_canceled(event.request_id);
        refresh_channels();
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
    case CAPSID_EVENT_CHANNEL_DIAL: {
#ifdef CAPSID_HOST_NETWORKING
        if(!outbound_) {
            capsid::EgressPolicy authority;
            if(!capsid::client_egress_snapshot(worker_,&authority)) return false;
            WebSocketDialerOptions options; options.budget = channel_budget_;
            outbound_=std::make_unique<WebSocketDialer>(std::move(authority),channel_limits_,
                [this](Command command) { return try_submit_channel(std::move(command)); },
                [this](size_t bytes) { std::unique_lock<std::mutex> lock(mutex_); channel_event_bytes_-=bytes; },std::move(options));
        }
#endif
        capsid::protocol::ChannelOpenPayload open; std::string error;
        if(!capsid::protocol::decode_channel_open(event.payload.data,event.payload.size,&open,&error,effective_channel_caps())) return false;
        outbound_dial_ids_.insert(open.dial_id);
        WorkerEvent output; output.type=WorkerEvent::Type::kChannelDial; output.outbound=true;
        output.dial_id=open.dial_id; output.channel_max_message=open.max_message_bytes;
        output.channel_kind=open.channel_kind; output.http_request=std::move(open.http_request);
        output.text=std::move(open.url); output.protocols=std::move(open.protocols);
        output.dial_refused=capsid::client_dial_must_reject(worker_,open.dial_id,&output.channel_flags);
        if (output.channel_kind==capsid::protocol::kChannelKindFetchUpgrade) {
            size_t charge=output.text.size()+1024;
            for (const auto &header : output.http_request.headers) charge+=header.first.size()+header.second.size()+64;
            bool exhausted;
            { std::unique_lock<std::mutex> lock(mutex_);
              exhausted=charge>16u*1024u*1024u || channel_event_bytes_>16u*1024u*1024u-charge;
              if (!exhausted) {channel_event_bytes_+=charge; output.channel_charge=charge;}
            }
            if (exhausted) {
                Command rejected; rejected.type=CommandType::kDialReject; rejected.dial_id=open.dial_id;
                rejected.channel_flags=8; rejected.close_reason="HTTP upgrade event queue exhausted";
                return execute_command(std::move(rejected));
            }
        }
#ifndef CAPSID_HOST_NETWORKING
        Command rejected; rejected.type=CommandType::kDialReject; rejected.dial_id=open.dial_id;
        rejected.channel_flags=8; rejected.close_reason="Host WebSocket networking is unavailable";
        if(!execute_command(std::move(rejected))) return false;
#endif
        queue_worker_event(std::move(output)); refresh_channels(); return true;
    }
    case CAPSID_EVENT_CHANNEL_DATA:
    case CAPSID_EVENT_CHANNEL_CREDIT:
    case CAPSID_EVENT_CHANNEL_CLOSE:
    case CAPSID_EVENT_CHANNEL_CANCEL:
    case CAPSID_EVENT_CHANNEL_ACK: {
        capsid_channel_event view; capsid_channel_event_init(&view);
        if (capsid_channel_event_decode(&event, &view, sizeof(view)) != CAPSID_OK) return false;
        WorkerEvent output;
        output.channel_id = view.channel_id;
        output.dial_id = view.dial_id;
        output.outbound=outbound_channel_ids_.count(view.channel_id) || outbound_dial_ids_.count(view.dial_id);
        output.channel_flags = event.flags;
        output.channel_credit = view.credit;
        output.close_kind = view.close_kind;
        output.close_code = view.code;
        if (view.diagnostic.size) output.text.assign(reinterpret_cast<const char *>(view.diagnostic.data), view.diagnostic.size);
        if (event.type == CAPSID_EVENT_CHANNEL_DATA) {
            if (quarantined_channel_ids_.count(view.channel_id)) return true;
            bool exhausted = false;
            { std::unique_lock<std::mutex> lock(mutex_);
              const size_t charge = view.message.size + 1024;
              exhausted = charge > 16u*1024u*1024u || channel_event_bytes_ > 16u*1024u*1024u - charge;
              if (!exhausted) channel_event_bytes_ += charge;
            }
            if (exhausted) {
                // Stop only this producer. The owner's bounded terminal
                // event will close its network socket and release its pin.
                Command cancel; cancel.type = CommandType::kChannelCancel;
                cancel.channel_id = view.channel_id; cancel.channel_flags = CAPSID_CANCEL_REASON_RESOURCE_LIMITS;
                quarantined_channel_ids_.insert(view.channel_id);
                if (!execute_command(std::move(cancel))) return false;
                output.type = WorkerEvent::Type::kChannelCancel;
            } else {
                output.type = WorkerEvent::Type::kChannelData;
                output.channel_charge = view.message.size + 1024;
                output.body.assign(view.message.data, view.message.data + view.message.size);
            }
        } else if (event.type == CAPSID_EVENT_CHANNEL_CREDIT) output.type = WorkerEvent::Type::kChannelCredit;
        else if (event.type == CAPSID_EVENT_CHANNEL_CLOSE) output.type = WorkerEvent::Type::kChannelClose;
        else if (event.type == CAPSID_EVENT_CHANNEL_CANCEL) output.type = WorkerEvent::Type::kChannelCancel;
        else { output.type = WorkerEvent::Type::kChannelAck; if (capsid_channel_ack_kind(&event, &output.ack_kind) != CAPSID_OK) return false; }
#ifndef CAPSID_HOST_NETWORKING
        if(output.outbound && event.type==CAPSID_EVENT_CHANNEL_CANCEL && !view.channel_id) {
            Command confirmation; confirmation.type=CommandType::kChannelConfirm;
            confirmation.dial_id=view.dial_id; confirmation.channel_flags=1;
            if(!execute_command(std::move(confirmation))) return false;
        }
#endif
        refresh_channels();
        queue_worker_event(std::move(output));
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
        // owner's kResponseEnd handler acknowledges the terminal event. The
        // worker thread removes the tombstone only after its current command
        // batch has drained. The inflight slot is released exactly once.
        mark_canceled(event.request_id);
        WorkerEvent worker_event;
        worker_event.type = WorkerEvent::Type::kResponseEnd;
        worker_event.request_id = event.request_id;
        queue_worker_event(std::move(worker_event));
        return true;
    }
    case CAPSID_EVENT_LOG: {
        WorkerEvent worker_event;
        std::string log_error;
        if (!decode_worker_log_event(event, &worker_event, &log_error)) {
            write_stderr(std::string("capsid-host: ") + log_error);
            return false;
        }
        worker_event.application_id = log_application_id_;
        worker_event.generation_digest = log_generation_digest_;
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
    if(event.outbound) {
#ifdef CAPSID_HOST_NETWORKING
        if(outbound_) outbound_->post(std::move(event));
#else
        std::unique_lock<std::mutex> lock(mutex_); channel_event_bytes_-=event.channel_charge;
#endif
        return;
    }
    bool need_notify = false;
    {
        // The event-queue size must be read under the mutex: this function
        // runs on both the owner thread and the worker thread, and the other
        // side may be mutating events_ concurrently.
        std::unique_lock<std::mutex> lock(mutex_);
        if (event.type == WorkerEvent::Type::kChannelCredit) {
            for (auto &queued:events_) if (queued.type==event.type && queued.channel_id==event.channel_id &&
                    event.channel_credit <= std::numeric_limits<std::uint64_t>::max()-queued.channel_credit) {
                queued.channel_credit += event.channel_credit; return;
            }
        }
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
        write_stderr("capsid-host: " + (ready_error_.empty()
            ? std::string("worker compatibility ID mismatch") : ready_error_));
        return false;
    }
    return true;
}

}  // namespace capsid::host
