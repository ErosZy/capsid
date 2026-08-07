#ifndef CAPSID_HOST_WORKER_SUPERVISOR_H
#define CAPSID_HOST_WORKER_SUPERVISOR_H

#include "host/managed_host.h"
#include "host/worker_recovery.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <random>
#include <string>
#include <thread>

struct capsid_worker;

namespace capsid::host {
class StartupPermitCoordinator;

// M2 item 5a: the per-App worker supervisor. One thread per configured
// App owns the observation of its current worker's IPC stream and the
// replacement/quarantine decisions derived from it (design §10.5). The
// thread never holds a decision function while touching the worker map:
// the map is the single source of truth for "which worker is current",
// and every action the supervisor takes is gated by an identity check
// against it, so a deploy or retire that races the observation wins and
// the supervisor simply re-syncs.
struct WorkerSupervisorOptions {
    // The App's managed coordinator (used for managed_recover respawns
    // and the managed_quarantine tombstone).
    ManagedHostOptions* managed_options = nullptr;
    // The App's recovery policy (recovery.* in host.json).
    WorkerRecoveryPolicy policy;
    // Worker-map ownership closures. current_worker returns the map entry
    // for the App (nullptr when none). publish_worker replaces the entry,
    // destroying the previous worker. remove_if_current destroys and
    // erases the entry ONLY when it is exactly the given worker (a raced
    // deploy's live worker is never destroyed by the supervisor).
    // discard_worker destroys a worker the supervisor itself spawned but
    // could not publish.
    std::function<capsid_worker*(const std::string&)> current_worker;
    std::function<void(const std::string&, capsid_worker*)> publish_worker;
    std::function<void(const std::string&, capsid_worker*)> remove_if_current;
    std::function<void(capsid_worker*)> discard_worker;
    // Process-level stop signal: when set, the thread stops observing and
    // exits without counting anything (shutdown is never instability).
    const std::atomic<bool>* stop_requested = nullptr;
    // M2 item 5b: the process-global fair startup-permit queue (design
    // §10.5.6). A replacement waits for its grant behind every queued
    // request from other Apps, so a crash-looping App cannot persistently
    // queue ahead of another App's deploy. The grant is held across the
    // respawn. Null disables the queue (replacements start immediately).
    StartupPermitCoordinator* startup_permits = nullptr;
};

class WorkerSupervisor {
public:
    explicit WorkerSupervisor(WorkerSupervisorOptions options);
    ~WorkerSupervisor();

    WorkerSupervisor(const WorkerSupervisor&) = delete;
    WorkerSupervisor& operator=(const WorkerSupervisor&) = delete;

    // Idempotent: the destructor joins as well, so explicit stop/join is
    // only needed to control shutdown ordering.
    void request_stop();
    void join();

private:
    void run();
    void handle_worker_gone(capsid_worker* worker);
    void attempt_replacement(capsid_worker* worker, const WorkerRecoveryDecision& decision);
    void quarantine(capsid_worker* worker);
    void remove_current(capsid_worker* worker);
    bool sleep_interruptible(std::uint64_t milliseconds);
    bool stop_requested() const;
    std::uint64_t monotonic_ms() const;
    std::int32_t jitter_basis_points() const;

    WorkerSupervisorOptions options_;
    std::thread thread_;
    std::atomic<bool> stopped_ = false;
    // Only the supervisor thread touches these.
    capsid_worker* observed_ = nullptr;
    GenerationRecoveryState recovery_state_;
    bool recovery_anchored_ = false;
    std::uint64_t ready_since_ms_ = 0;
    bool tracking_stability_ = false;
    mutable std::mt19937 jitter_rng_;
};

}  // namespace capsid::host

#endif
