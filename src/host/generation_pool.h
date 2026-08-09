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
    // client sockets, so the session layer supplies this per slot (0 when
    // unset).
    std::function<std::uint64_t(std::size_t slot)> client_bytes_loader;
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

    // ---- diagnostics / load signal -------------------------------------

    // Total requests owned by the fleet (active slots + retired executors
    // still draining their pinned requests).
    std::uint64_t inflight() const;
    // Live READY capacity: N after a full fleet, N-1 while a replacement
    // is in flight, 0 when the fleet is exhausted.
    std::uint32_t ready_workers() const;
    State state() const;
    std::string_view application_id() const;
    std::string_view version() const;
    std::string_view generation_digest() const;

private:
    explicit GenerationPool(GenerationPoolOptions options);
    // The process-global startup semaphore (§8.3). The struct is defined
    // in the .cc; the static member is the only entry so the private
    // nested type is never named outside the class.
    struct StartupSemaphore;
    static StartupSemaphore& startup_semaphore();

    void pump_loop();
    // kExit on `slot`: remove from the READY set, run the recovery
    // controller, schedule a replacement when the budget allows.
    void handle_executor_exit(std::size_t slot);
    // Spawns the replacement worker (semaphore permit → factory → READY →
    // slot swap). Runs on its own thread so the pump never blocks.
    void run_replacement(std::size_t slot);
    // Swap a READY replacement into `slot` (pump or replacement thread).
    void install_replacement(std::size_t slot,
                             std::shared_ptr<WorkerExecutor> replacement);
    // kDead + reap everything (slots and retired executors), outside the
    // pool mutex. Pump-only; called with the mutex released.
    void finalize_drain();
    bool fleet_exited() const;  // slots + retired, under mutex_

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
    // Atomic mirror of "state_ == kActive" for the semaphore wait
    // predicate, which must not take the pool mutex.
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
