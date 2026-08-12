// WP-04 §8.2/§8.3: GenerationPool — one generation of one App's worker
// fleet, composed of PR-06 WorkerExecutors. The WP-05 Managed data plane
// routes requests into a pool; this class owns the fleet machinery only
// (no listener, no sessions — those belong to the listener/router layer).
//
// §8.2 responsibilities:
//   - identity: application_id, version, generation_digest (the pool key);
//   - state machine: prepared → active → draining → dead;
//   - the fleet: vector<shared_ptr<WorkerExecutor>> slots plus the
//     generation's immutable effective limits;
//   - the pool inflight counter (the load signal for scheduling);
//   - least-loaded scheduling among READY workers: load = inflight +
//     pending-client-bytes (loader hook — the WP-05 session layer owns the
//     write buffers the pool cannot see) + unhealthy penalty (reserved:
//     below the session layer "unhealthy" only means exited, and exited
//     slots are already excluded from the READY set);
//   - requests pin shared_ptr<GenerationPool> (enable_shared_from_this):
//     a request begun on a slot stays there even when the slot is
//     replaced; the replaced executor is retired and kept alive until the
//     pool dies, so a pinned request never touches freed memory;
//   - draining rejects new requests (pick_worker → nullptr; the caller
//     answers 503). The per-executor shutdown grace (kShutdownGrace) is
//     the drain deadline: on expiry the Runtime cancels the remaining
//     work on the shutdown frame and the worker exits to the reaper
//     (WP-03 semantics).
//
// §8.3 responsibilities (replacement):
//   - any worker EXIT (poisoned or clean — the pool never distinguishes)
//     removes the slot from the READY set immediately; the pool serves at
//     N-1, and at 0 pick_worker returns nullptr so the caller synthesizes
//     503 — a dead worker is never routed to;
//   - the replacement respawns through the SAME factory (same artifact,
//     same effective config) under the worker_recovery controller: crash
//     budget (max_events begins quarantine and suppresses replacement),
//     exponential backoff with jitter, per-App singleflight, and the
//     process-global startup semaphore (FairStartupPermitQueue);
//   - a generation that is no longer active never starts a replacement;
//   - the READY replacement re-enters the fleet (N again), and continuous
//     stability resets the backoff (observe_worker_stability).

#ifndef CAPSID_HOST_GENERATION_POOL_H
#define CAPSID_HOST_GENERATION_POOL_H

#include "host/worker_executor.h"
#include "host/worker_recovery.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace capsid::host {

class StartupPermitCoordinator;
class StructuredLog;
class MetricsRegistry;

struct GenerationPoolOptions {
    // Identity (the generation key; application_id must satisfy the
    // active-state identifier contract [a-z0-9][a-z0-9._-]{0,62},
    // generation_digest "sha256:" + 64 hex).
    std::string application_id;
    std::string version;
    std::string generation_digest;
    // Target fleet size N. create() is a N→READY barrier: it returns only
    // after every slot's worker is READY (or fails and reaps everything).
    std::uint32_t workers = 1;
    // Spawns ONE worker (spawn/load/flush; READY handshake is the
    // executor's). Shared by every slot and every replacement, so a
    // replacement is by construction the same artifact and effective
    // config as the worker it replaces (§8.3). The factory must be
    // thread-safe if concurrent pools share it.
    WorkerExecutor::WorkerFactory factory;
    // Crash budget, backoff and startup-permit lane (§8.3). Requires
    // backoff_initial_ms > 0 and backoff_maximum_ms >= backoff_initial_ms
    // (create() rejects an invalid policy).
    WorkerRecoveryPolicy recovery;
    // The generation's immutable effective limits. The WP-05 session layer
    // enforces them per request; the pool records them as policy and
    // exposes them for diagnostics.
    std::uint64_t max_inflight_per_worker = 0;
    std::uint64_t max_queued_requests = 0;
    // §8.2 drain deadline for wait()/stop_and_join(): the whole-fleet
    // bound. Per-request cancellation is the executor's shutdown grace.
    std::chrono::milliseconds drain_deadline = std::chrono::seconds(10);
    // §8.2 load term "bytes awaiting client consumption": the pool owns no
    // client sockets, so the session layer supplies this per executor
    // (0 when unset). Keyed by the executor identity, NOT the slot index:
    // a replacement installs a different executor into the same slot, and
    // the session layer accounts per executor. Called under the pool mutex
    // (pick_worker), so the loader must never block on the pool.
    std::function<std::uint64_t(const WorkerExecutor*)> client_bytes_loader;
    // WP-05 §9.2: the pool is the sole drainer of every slot executor's
    // event queue, so events other consumers care about (the listener's
    // sessions) are forwarded here. Called on the pump thread, under the
    // pool mutex: the sink must be cheap and must never take a lock that
    // the pool could also take while the io thread holds it. kExit events
    // are forwarded too — the listener uses them to fail the requests
    // pinned to a dead worker (the executor emits no per-request failure
    // for a worker exit); the pool still handles the replacement.
    // Ordering: single producer (the pump), FIFO per executor, and kExit is
    // always the last event of its executor.
    std::function<void(const WorkerExecutor*, WorkerEvent)> event_sink;
    // §9.4 reaper-finished notification: called exactly once, on the pump
    // thread, after the pool entered kDead and its workers were reaped
    // (finalize_drain) — the "reaper completed" instant after which the
    // old pool's capacity count may be released. Must be cheap and
    // non-blocking (the ledger release runs here); never take the pool
    // mutex from the callback.
    std::function<void()> on_drain_complete;
    // Direction A (dual-engine resolution): the pool is the ONLY recovery
    // engine, so it owns the process-global fair startup-permit queue for
    // its replacement spawns (design §10.5.6). The Host wires one
    // coordinator instance shared by every pool and every deploy path;
    // null disables the queue (replacements start immediately).
    StartupPermitCoordinator* startup_permits = nullptr;
    // M2 item 7: the process-wide structured log and metrics registry
    // (design §12). Null disables event logging/metrics on this path.
    StructuredLog* log = nullptr;
    MetricsRegistry* metrics = nullptr;
    // Direction A lifecycle observers: the Host's worker map is a pure
    // observer (it never destroys workers — the executor's worker thread
    // is the sole reaper), so the pool reports worker identity changes
    // through these callbacks instead of exposing ownership.
    // on_worker_started fires when an executor became READY (create /
    // create_adopted / install_replacement — pool thread or spawn
    // thread, outside the pool mutex). on_worker_exited fires when an
    // executor's kExit is processed (pump thread, under the pool mutex).
    // on_quarantine fires when the crash budget is exhausted (pump or
    // spawn thread): the Host writes the quarantine tombstone; the pool
    // then requests its own shutdown (a quarantined generation never
    // spawns again). All three must be cheap and must never take the pool
    // mutex.
    std::function<void(const WorkerExecutor*)> on_worker_started;
    std::function<void(const WorkerExecutor*)> on_worker_exited;
    std::function<void()> on_quarantine;
};

// See the file comment for the full contract.
class GenerationPool : public std::enable_shared_from_this<GenerationPool> {
public:
    enum class State { kPrepared, kActive, kDraining, kDead };

    // N→READY barrier: spawns N workers through the factory, installs the
    // event notifier and starts the pump. Returns nullptr (with *error)
    // when any spawn fails — everything already started is reaped before
    // the call returns. The pool is kActive on success.
    static std::shared_ptr<GenerationPool> create(GenerationPoolOptions options,
                                                  std::string* error);

    // WP-05 §9.2: pool over an ALREADY-WARMED fleet. `warmed` is exactly
    // options.workers pre-warmed workers whose READY handshake (including
    // the compatibility check) was consumed by the adopter — the Managed
    // coordinator's warm-up. Each is wrapped via WorkerExecutor::adopt();
    // nothing is respawned. The factory stays REQUIRED for §8.3
    // replacements (same artifact, same effective config). On failure every
    // warmed worker is destroyed (adopted ones through their executors)
    // and nullptr is returned — no worker ever escapes.
    static std::shared_ptr<GenerationPool> create_adopted(
        GenerationPoolOptions options,
        std::vector<capsid_worker*> warmed,
        std::string* error);

    ~GenerationPool();  // request_drain() + wait()

    GenerationPool(const GenerationPool&) = delete;
    GenerationPool& operator=(const GenerationPool&) = delete;

    // ---- lifecycle -----------------------------------------------------

    // §8.2 drain: no new requests; every slot gets its shutdown command.
    // The pump reaps the fleet (executor threads exit; retired executors
    // are reaped too) and completes the drain when nothing is left.
    void request_drain();
    // Blocks until the pool is kDead (drain completed, every executor
    // thread joined, every replacement finished or abandoned). Idempotent.
    bool wait(std::string* error);
    // request_drain() + wait(). Safe on a never-created pool (nullptr
    // from create()) — the facade returns early.
    void stop_and_join();

    // ---- scheduling (§8.2) ---------------------------------------------

    // Least-loaded READY slot (tie: lowest slot index). Returns nullptr
    // when the generation is not kActive or no worker is READY — the
    // caller synthesizes 503 and must NOT route anywhere. The returned
    // executor stays alive (pool pinning) even if its slot is replaced.
    WorkerExecutor* pick_worker();

    // ---- direction A: active-health recycle -----------------------------

    // The first READY slot's executor — the active-health probe target
    // (v1 pools are single-worker; a multi-worker pool probes its lowest
    // READY slot). nullptr when the generation is not kActive or no
    // worker is READY. The returned executor stays pool-pinned.
    WorkerExecutor* current_worker();
    // M2 item 6 (§7.4) recycle entry: the supervisor probes the worker
    // THROUGH its executor and calls this on consecutive failed verdicts.
    // The pool records a kHealthRecycle instability (the shared budget —
    // §10.5.2), and either begins quarantine (on_quarantine + self
    // drain) or schedules a replacement AND requests the old worker's
    // shutdown — the worker is still ALIVE, so only the pool may retire
    // it (the executor's worker thread owns the destroy; the supervisor
    // never holds a capsid_worker*). Returns true when the recycle was
    // recorded (quarantine or replacement scheduled); false when the
    // target is not a READY slot of this pool (stale — the caller
    // re-syncs without counting).
    bool recycle_worker(WorkerExecutor* target);

    // ---- §9.2 event bridge ----------------------------------------------

    // Installs the listener's event sink on this pool: a mutex-guarded
    // swap, and the pump reads options_.event_sink under the same mutex,
    // so the sink never changes mid-drain. Exactly one consumer owns a
    // pool's fan-out at a time — the ManagedListener wires every pool in
    // its snapshot at start(), and the Managed coordinator wires pools it
    // activates after the listener is already running.
    // Precondition: the pool has NO in-flight requests (a request pinned
    // before wiring would lose its response events and hang) — wire before
    // the pool starts serving traffic.
    void set_event_sink(
        std::function<void(const WorkerExecutor*, WorkerEvent)> sink);

    // ---- diagnostics / load signal -------------------------------------

    // Total requests owned by the fleet (active slots + retired executors
    // still draining their pinned requests).
    std::uint64_t inflight() const;
    // Live READY capacity: N after a full fleet, N-1 while a replacement
    // is in flight, 0 when the fleet is exhausted.
    std::uint32_t ready_workers() const;
    // The generation's immutable per-worker inflight ceiling (0 = the
    // session layer imposes no cap beyond the worker's own limit).
    std::uint64_t max_inflight_per_worker() const {
        return options_.max_inflight_per_worker;
    }
    // The configured fleet size (the count the ledger reserved for this
    // pool; distinct from ready_workers(), which can drop below it while
    // the generation is reaping). PR-10 §9.4.
    std::uint32_t configured_workers() const { return options_.workers; }
    State state() const;
    std::string_view application_id() const;
    std::string_view version() const;
    std::string_view generation_digest() const;

private:
    explicit GenerationPool(GenerationPoolOptions options);
    // Direction A: the crash budget ran out. Emit the quarantine log and
    // metrics, fire the on_quarantine observer (the Host writes the
    // tombstone), and request this pool's own drain — a quarantined
    // generation never spawns again and its workers exit to the reaper.
    void begin_quarantine();
    // Record one instability through the recovery controller; on
    // quarantine the pool drains itself, on schedule_replacement the slot
    // is armed. Shared by handle_executor_exit and recycle_worker.
    // Caller holds mutex_. `reason` names the replacement for the
    // kWorkerReplaced log ("crashed" / "unhealthy").
    void record_instability(std::size_t slot,
                            WorkerInstabilityObservation observation,
                            const char* reason);

    void pump_loop();
    // kExit on `slot`: remove from the READY set, run the recovery
    // controller, schedule a replacement when the budget allows.
    void handle_executor_exit(std::size_t slot);
    // Spawns the replacement worker (startup permit → factory → READY →
    // slot swap). Runs on its own thread so the pump never blocks.
    void run_replacement(std::size_t slot);
    // Swap a READY replacement into `slot` (pump or replacement thread).
    void install_replacement(std::size_t slot,
                             std::shared_ptr<WorkerExecutor> replacement);
    // kDead + reap everything (slots and retired executors), outside the
    // pool mutex. Pump-only; called with the mutex released.
    void finalize_drain();
    bool fleet_exited() const;  // slots + retired, under mutex_
    // Any replacement scheduled (a due present) — the pump's no-deadline
    // wait predicate, under mutex_.
    bool due_pending_locked() const;

    GenerationPoolOptions options_;
    WorkerRecoveryPolicy policy_;          // validated copy
    GenerationRecoveryState recovery_state_;
    ServiceLifecycleState lifecycle_;      // kActive for the pool's life

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    struct Slot {
        std::shared_ptr<WorkerExecutor> executor;
        bool ready = false;                // READY set membership (§8.3)
        bool replacement_in_flight = false;
        // True once the pool itself requested this worker's shutdown: its
        // later EXIT is an expected lifecycle event (no crash budget, no
        // replacement) instead of an unexpected failure.
        bool shutdown_issued = false;
        // Direction A: the last instability's replacement reason, read by
        // install_replacement for the kWorkerReplaced message ("crashed"
        // for an unexpected exit, "unhealthy" for a health recycle).
        std::string replacement_reason;
    };
    std::vector<Slot> slots_;
    // Replaced (dead) executors with pinned requests: kept alive until
    // the pool dies so a pinned request never touches freed memory.
    std::vector<std::shared_ptr<WorkerExecutor>> retired_;
    // Slots covered by a scheduled-or-running replacement: the per-App
    // singleflight counter the recovery controller sees.
    std::uint64_t replacements_scheduled_ = 0;
    // Threads actually spawning (drives drain completion; the pump never
    // finalizes while one is running).
    std::uint64_t replacements_in_flight_ = 0;
    // Slot → spawn due time (monotonic ms) for the exponential backoff.
    std::vector<std::uint64_t> replacement_due_ms_;
    bool events_pending_ = false;
    // Atomic mirror of "state_ == kActive" for the startup-permit wait
    // abort predicate, which must not take the pool mutex (direction A:
    // a replacement thread abandons its permit wait when the drain
    // begins, so the pump's replacements_in_flight_ == 0 wait can never
    // deadlock behind a permit nobody will hand out).
    std::atomic<bool> active_flag_ = true;

    State state_ = State::kPrepared;
    std::uint64_t stable_since_ms_ = 0;   // continuous-READY base
    std::uint32_t ready_workers_locked() const;  // caller holds mutex_
    std::thread pump_thread_;
    std::vector<std::thread> replacement_threads_;
    std::once_flag wait_once_;
};

}  // namespace capsid::host

#endif
