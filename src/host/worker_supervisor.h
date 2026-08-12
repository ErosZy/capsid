#ifndef CAPSID_HOST_WORKER_SUPERVISOR_H
#define CAPSID_HOST_WORKER_SUPERVISOR_H

#include "host/generation_pool.h"
#include "host/managed_host.h"
#include "host/worker_recovery.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <random>
#include <string>
#include <thread>

namespace capsid::host {
class StructuredLog;
class MetricsRegistry;

// M2 item 5a (direction A): the per-App active-health probe scheduler.
// One thread per configured App drives the §7.4 probe loop against the
// App's generation pool. The pool is the ONLY recovery engine: the
// supervisor never touches a capsid_worker* and never reads the IPC
// event stream — probes go THROUGH the pool's executor (the sole event
// consumer; the probe command and its response events fold into the
// executor's probe_state()), verdicts are polled from probe_state(), and
// consecutive failed verdicts call pool->recycle_worker() (a
// kHealthRecycle instability that the pool records against the shared
// budget). The supervisor's only decisions are WHEN to probe and when
// the failures add up; the pool decides what happens next.
struct WorkerSupervisorOptions {
    // The App's managed coordinator — read-only here: the committed
    // healthCheck config (probe path and timeout) for the pool's
    // generation.
    ManagedHostOptions* managed_options = nullptr;
    // The App's recovery policy — used only for the probe-interval
    // jitter basis points.
    WorkerRecoveryPolicy policy;
    // The App's generation pool (direction A). nullptr when the App has
    // no pool (never deployed, quarantined, retired): nothing to probe.
    std::function<GenerationPool*()> current_pool;
    // Process-level stop signal: when set, the thread stops scheduling
    // probes and exits without counting anything (shutdown is never
    // instability).
    const std::atomic<bool>* stop_requested = nullptr;
    // M2 item 6 (design §7.4): the active health probe schedule.
    // active_health_interval_ms is the host-wide probe period (default
    // 30s; 0 disables probing entirely — the passive signal remains the
    // only recovery trigger). active_health_failures is the consecutive
    // failed probe verdicts that recycle the worker through the pool's
    // recovery chain as a kHealthRecycle instability event (§10.5.2).
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
    // (Re)arm the probe schedule for the executor the pool currently
    // hands out: clears the consecutive-failure streak and re-reads the
    // committed healthCheck config for the pool's generation.
    void rearm_probe(GenerationPool* pool, WorkerExecutor* executor);
    void probe_success();
    void probe_failure(WorkerExecutor* executor);
    void schedule_next_probe();
    bool sleep_interruptible(std::uint64_t milliseconds);
    bool stop_requested() const;
    std::uint64_t monotonic_ms() const;
    std::int32_t jitter_basis_points() const;

    WorkerSupervisorOptions options_;
    std::thread thread_;
    std::atomic<bool> stopped_ = false;
    // Only the supervisor thread touches these.
    WorkerExecutor* probe_target_ = nullptr;
    std::string anchored_generation_;
    HealthCheckConfig health_check_;
    // supervisor-side probe lifecycle: probe_in_flight_ is true between
    // submit_probe and the verdict (the executor's ProbeState mirrors it
    // on the worker thread; the supervisor must not read a command that
    // has not reached the worker yet as a skip).
    bool probe_in_flight_ = false;
    std::uint64_t probe_submitted_ms_ = 0;
    std::uint64_t probe_deadline_ms_ = 0;
    std::uint64_t next_probe_due_ms_ = 0;
    std::uint32_t consecutive_probe_failures_ = 0;
    mutable std::mt19937 jitter_rng_;
};

}  // namespace capsid::host

#endif
