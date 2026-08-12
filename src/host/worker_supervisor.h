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
struct capsid_event;

namespace capsid::host {
class StartupPermitCoordinator;
class StructuredLog;
class MetricsRegistry;

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
    // M2 item 6 (design §7.4): the active health probe schedule.
    // active_health_interval_ms is the host-wide probe period (default
    // 30s; 0 disables probing entirely — the passive signal remains the
    // only recovery trigger). active_health_failures is the consecutive
    // failed probe verdicts that recycle the worker through the item-5a
    // replacement chain as a kHealthRecycle instability event (§10.5.2).
    // An unconfigured App is never probed regardless of these values.
    std::uint64_t active_health_interval_ms = 0;
    std::uint32_t active_health_failures = 0;
    // M2 item 7: the process-wide structured log and metrics registry
    // (design §12). Null disables event logging/metrics on this path.
    StructuredLog* log = nullptr;
    MetricsRegistry* metrics = nullptr;
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
    // M2 item 6: the probe-driven recycle path — the same decision chain
    // as handle_worker_gone, but the worker is still ALIVE: consecutive
    // failed probe verdicts reclassify it as unhealthy (§7.4) and each
    // recycle counts against the shared instability budget (§10.5.2).
    void handle_unhealthy_worker(capsid_worker* worker);
    void attempt_replacement(capsid_worker* worker,
                             const WorkerRecoveryDecision& decision,
                             const char* replaced_message);
    void quarantine(capsid_worker* worker);
    void remove_current(capsid_worker* worker);
    bool sleep_interruptible(std::uint64_t milliseconds);
    bool stop_requested() const;
    std::uint64_t monotonic_ms() const;
    std::int32_t jitter_basis_points() const;
    // M2 item 6: probe machinery. All state lives on the supervisor
    // thread inside run()'s poll loop.
    void start_probe(capsid_worker* worker);
    void handle_probe_event(capsid_worker* worker,
                            const capsid_event& event);
    void probe_success();
    void probe_failure(capsid_worker* worker);
    void schedule_next_probe();

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
    // M2 item 6: active health probe state. probe_worker_ names the
    // worker the current config/state belongs to; the loop re-arms
    // whenever the map hands out a different worker (a replacement
    // publish, a re-anchor, or a redeployed healthCheck).
    capsid_worker* probe_worker_ = nullptr;
    HealthCheckConfig health_check_;
    bool probe_in_flight_ = false;
    std::uint64_t probe_request_id_ = 0;
    std::uint64_t probe_deadline_ms_ = 0;
    std::int32_t probe_status_ = 0;
    std::uint64_t probe_body_bytes_ = 0;
    std::uint64_t next_probe_due_ms_ = 0;
    std::uint32_t consecutive_probe_failures_ = 0;
};

}  // namespace capsid::host

#endif
