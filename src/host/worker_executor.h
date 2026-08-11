// WP-04 §8.1: the WorkerExecutor owns everything that talks to one
// capsid_worker process.
//
// The extraction boundary (spec §8.1): the executor exclusively owns the
// capsid_worker handle, the WorkerEventSource, the command queue, the event
// queue and the worker thread. It accepts an already-READY adopted worker
// or a factory spawn/load/READY, and exposes only submit/cancel/grant, an
// event callback, health, inflight and stop/wait. capsid_worker_destroy
// runs exclusively on the executor's owner/reaper thread (the worker
// thread's exit path), exactly once.
//
// The SingleWorkerServer composes this class; the pool (PR-07) composes
// one per worker.
//
// Thread model:
//   - the owner thread (the SingleWorkerServer io thread) calls submit(),
//     mark_canceled(), drain_events(), request_shutdown() and stop_and_join();
//   - the worker thread runs worker_thread_main() and owns the destroy;
//   - the two threads communicate through two mutex-protected queues; the
//     worker thread copies every event payload before posting it, so
//     payload/header views never outlive the next next_event call.
//
// Inflight accounting: every begun request id is inserted at submit() and
// erased exactly once at its terminal point (RESPONSE_END, REQUEST_TIMEOUT,
// mark_canceled from the owner's cancel path, or worker exit). inflight()
// is the number of requests the worker currently owns — the §8.2 load
// signal for pool scheduling.

#ifndef CAPSID_HOST_WORKER_EXECUTOR_H
#define CAPSID_HOST_WORKER_EXECUTOR_H

#include "capsid/runtime.h"
#include "host/request_normalization.h"
#include "host/worker_event_source.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace capsid::host {

constexpr auto kReadyTimeout = std::chrono::seconds(10);
constexpr auto kShutdownGrace = std::chrono::seconds(2);

enum class CommandType {
    kBeginRequest,
    kWriteRequest,
    kEndRequest,
    kCancel,
    kGrantResponseCredit,
    kShutdown,
};

// A command from the owner thread to the worker thread. Strings are copied
// so the worker thread never reads owner-thread-owned buffers.
struct Command {
    CommandType type = CommandType::kBeginRequest;
    std::uint64_t request_id = 0;
    std::string method;
    std::string url;
    std::vector<NormalizedPublicRequestHeader> headers;
    std::vector<std::uint8_t> body;
    std::uint32_t credit = 0;
    // kBeginRequest only: the request has no body, so the kRequestEnd frame
    // is sent back-to-back with the head in the same flush. A separate end
    // command could land in a different IPC read than its begin, letting the
    // Runtime complete a synchronous handler response (and erase the request)
    // before the end frame arrives — which it rejects as an invalid frame
    // and kills the worker.
    bool end_request = false;
};

// An event decoded from the capsid_event protocol on the worker thread and
// handed to the owner thread through drain_events().
struct WorkerEvent {
    enum class Type {
        kRequestCredit,
        kResponseHead,
        kResponseBody,
        kResponseEnd,
        kLog,
        kError,
        kExit,
        kRequestTimeout,
        kRequestFailure,
    };
    Type type = Type::kLog;
    std::uint64_t request_id = 0;
    std::uint32_t credit = 0;
    std::uint16_t status = 0;
    bool fixed_body = false;
    std::uint32_t fixed_body_size = 0;
    std::vector<std::pair<std::string, std::string>> headers;
    std::vector<std::uint8_t> body;
    std::string text;
};

class WorkerExecutor {
public:
    // Produces a prepared (spawned, bundle loaded and flushed, not yet
    // READY) worker. On failure the factory must destroy the worker it
    // created and return false with *error set: ownership transfers to the
    // executor only on success, and from that moment destroy() runs
    // exclusively on the executor's reaper thread.
    using WorkerFactory =
        std::function<bool(capsid_worker** worker, std::string* error)>;
    // Invoked (worker thread or owner thread, never under the executor's
    // mutex) whenever new events are queued, so the owner can schedule its
    // drain. Set once before start()/adopt().
    using EventNotifier = std::function<void()>;

    WorkerExecutor() = default;
    ~WorkerExecutor();  // stop_and_join(); the worker thread owns destroy()
    WorkerExecutor(const WorkerExecutor&) = delete;
    WorkerExecutor& operator=(const WorkerExecutor&) = delete;

    // ---- lifecycle -----------------------------------------------------

    // Spawn/load/READY through the factory (the READY handshake includes
    // the compatibility-ID check). On failure the executor is left in a
    // stopped state: no thread, no worker, stop_and_join() a safe no-op.
    bool start(const WorkerFactory& factory, std::string* error);
    // Takes ownership of an already-READY worker whose READY handshake was
    // consumed by the adopter (the startup service). Skips the handshake.
    bool adopt(capsid_worker* worker, std::string* error);

    // Queues the Runtime shutdown frame at most once (idempotent across
    // request_stop, SIGTERM, start-failure and the destructor): a duplicate
    // would trigger a Runtime double shutdown.
    void request_shutdown();
    // request_shutdown() + join the worker thread; idempotent. Blocks until
    // the reaper finished, so callers never leave a joinable thread behind.
    void stop_and_join();

    // ---- command direction (owner thread) ------------------------------

    void submit(Command command);
    // Tombstone: no further frame for this request may reach the IPC
    // channel — the Runtime erased it (RESPONSE_END / REQUEST_TIMEOUT), or
    // the owner cancelled it. Called by the owner synchronously before the
    // matching kCancel is submitted, and by the worker thread when the
    // Runtime reports the request terminal. Also releases the inflight slot
    // exactly once (idempotent).
    void mark_canceled(std::uint64_t request_id);

    // ---- event direction (worker thread → owner thread) ----------------

    void set_event_notifier(EventNotifier notifier);
    // Swaps the event queue out under the mutex. The owner drains all
    // events in one call; the notifier fires on every empty→non-empty
    // transition and for the always-last kExit event.
    std::deque<WorkerEvent> drain_events();

    // ---- health / capacity ---------------------------------------------

    // READY and not exited (the owner's pool capacity signal).
    bool available() const;
    bool exited() const;
    // Requests the worker currently owns (relaxed read; see the class
    // comment for the accounting).
    std::uint64_t inflight() const;

    // ---- diagnostics ----------------------------------------------------

    // CAPSID_HOST_IPC_METRICS=1 only; zero overhead in headline runs.
    struct Metrics {
        std::atomic<std::uint64_t> commands_submitted = 0;
        std::atomic<std::uint64_t> command_batches = 0;  // wake-pipe writes
        std::atomic<std::uint64_t> commands_executed = 0;
        std::atomic<std::uint64_t> flush_calls = 0;
        std::atomic<std::uint64_t> flush_eagain = 0;
        std::atomic<std::uint64_t> events_queued = 0;
        std::atomic<std::uint64_t> grant_commands = 0;
        std::atomic<std::uint64_t> credit_bytes_granted = 0;
        std::atomic<std::uint64_t> command_queue_high_water = 0;
        std::atomic<std::uint64_t> event_queue_high_water = 0;
    };
    Metrics& metrics();
    // Set before start()/adopt() by the owner (which owns the env parsing).
    void set_metrics_enabled(bool enabled);
    // The live worker handle for diagnostics; nullptr once the reaper
    // destroyed it (or while the destroy is in flight). Ownership always
    // stays with the executor.
    capsid_worker* worker() const;

private:
    void worker_thread_main();
    bool wait_for_ready();
    bool execute_command(Command command);
    bool batch_flush();
    bool handle_worker_protocol_event(const capsid_event& event);
    void queue_worker_event(WorkerEvent event);
    void queue_worker_exit_event();
    bool report_runtime_failure(std::uint64_t request_id,
                                capsid_result result,
                                const char* operation);

    Metrics metrics_;
    // Atomic: set by the owner before start()/adopt(), but read from the
    // command path while a concurrent pool stop may be mid-start (TSan gate
    // PR-06: submit() vs set_metrics_enabled() race).
    std::atomic<bool> metrics_enabled_ = false;
    EventNotifier notifier_;
    // Exactly-once lifecycle gates.
    std::atomic<bool> started_ = false;
    std::atomic<bool> shutdown_requested_ = false;
    // Set before the blocking destroy so worker() never returns a pointer
    // into destroyed memory.
    std::atomic<bool> destroying_ = false;

    // Bridge between the owner thread and the worker thread. Mutable: the
    // const accessors (available/exited/worker) read the state under the
    // lock while the worker thread writes it.
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Command> commands_;
    std::deque<WorkerEvent> events_;
    // Tombstones for requests the Runtime already erased (worker thread)
    // or the owner cancelled (owner thread): any queued frame for them is
    // dropped before it reaches the IPC channel.
    std::set<std::uint64_t> canceled_;
    // Inflight accounting: every begun request id, erased exactly once at
    // its terminal point. Guards the inflight_ counter against double
    // decrements (a cancel racing an already-queued RESPONSE_END).
    std::set<std::uint64_t> inflight_ids_;
    std::atomic<std::uint64_t> inflight_ = 0;
    bool ready_ = false;
    bool ready_match_ = false;
    bool exited_ = false;
    bool exit_event_queued_ = false;
    capsid_worker* worker_ = nullptr;
    WorkerEventSource source_;
    std::optional<std::chrono::steady_clock::time_point> shutdown_deadline_;
    std::thread worker_thread_;
};

}  // namespace capsid::host

#endif
