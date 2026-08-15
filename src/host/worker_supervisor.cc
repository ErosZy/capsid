#include "host/worker_supervisor.h"

#include "host/managed_admin_backend.h"
#include "host/metrics.h"
#include "host/structured_log.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <thread>

namespace capsid::host {

namespace {

// M2 item 7 (design §12.2): single write path for every supervisor event.
// Null log (unit fixtures without the process-wide instance) is a no-op.
void emit_log(StructuredLog* log, LogLane lane, LogFields fields) {
    if (log != nullptr) {
        log->log(lane, std::move(fields));
    }
}

// A probe schedule marker meaning "never due" (unconfigured App, or
// probing disabled by a zero host interval).
inline constexpr std::uint64_t kProbeDisabled =
    std::numeric_limits<std::uint64_t>::max();

// A submitted probe that the executor's worker thread never began (its
// ProbeState shows neither in_flight nor complete) within this window is
// a SKIP — the probe command never reached the Runtime (execution
// failure) — not a verdict (§7.4: a skip neither succeeds nor fails; the
// schedule simply re-arms). The window absorbs the submit→execute
// latency on the worker thread.
inline constexpr std::uint64_t kProbeSubmitSkipWindowMs = 250;

}  // namespace

// M2 item 5a (direction A): the per-App active-health probe scheduler.
// The pool is the only recovery engine; this thread only decides when to
// probe and when consecutive failures add up to a recycle. Three
// invariants hold for the whole loop:
//
// 1. The pool (through current_pool) is the single source of truth for
//    "which worker is current". Every probe action is gated by an
//    executor identity check against it, so a deploy, retire or
//    replacement that races the probe always wins and the supervisor
//    simply re-syncs (a fresh executor re-arms the schedule).
// 2. Probes never touch the IPC stream directly: submit_probe /
//    cancel_probe / probe_state on the executor are the whole interface
//    (the executor's worker thread is the sole event consumer).
// 3. Shutdown is never instability: the stop signal is checked at every
//    gate, and nothing observed after stop counts toward any budget.

WorkerSupervisor::WorkerSupervisor(WorkerSupervisorOptions options)
    : options_(std::move(options)) {
    thread_ = std::thread(&WorkerSupervisor::run, this);
}

WorkerSupervisor::~WorkerSupervisor() {
    request_stop();
    join();
}

void WorkerSupervisor::request_stop() {
    stopped_.store(true, std::memory_order_relaxed);
}

void WorkerSupervisor::join() {
    if (thread_.joinable()) {
        thread_.join();
    }
}

bool WorkerSupervisor::stop_requested() const {
    return options_.stop_requested != nullptr &&
           options_.stop_requested->load(std::memory_order_relaxed);
}

std::uint64_t WorkerSupervisor::monotonic_ms() const {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

std::int32_t WorkerSupervisor::jitter_basis_points() const {
    const std::int32_t limit =
        static_cast<std::int32_t>(options_.policy.jitter_basis_points);
    std::uniform_int_distribution<std::int32_t> distribution(-limit, limit);
    return distribution(jitter_rng_);
}

bool WorkerSupervisor::sleep_interruptible(std::uint64_t milliseconds) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(milliseconds);
    while (std::chrono::steady_clock::now() < deadline) {
        if (stop_requested()) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return true;
}

void WorkerSupervisor::run() {
    for (;;) {
        if (stop_requested()) {
            return;
        }
        GenerationPool* pool = options_.current_pool();
        if (pool == nullptr) {
            // No pool (never deployed, quarantined, retired): nothing to
            // probe.
            probe_target_ = nullptr;
            probe_in_flight_ = false;
            consecutive_probe_failures_ = 0;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        WorkerExecutor* executor = pool->current_worker();
        if (executor == nullptr) {
            // No READY worker (a replacement in flight, or draining):
            // re-sync when one appears. A probe target the pool already
            // removed counts nothing — the pool recorded its instability
            // and owns the outcome.
            probe_target_ = nullptr;
            probe_in_flight_ = false;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        if (executor != probe_target_) {
            // A new worker (deploy, replacement, or recycle): re-arm the
            // probe schedule — a fresh worker starts a fresh failure
            // streak.
            rearm_probe(pool, executor);
        }
        // §7.4 probe state machine: submit → verdict → schedule. The
        // executor's ProbeState mirrors the worker-thread fold of the
        // probe's response events.
        const ProbeState state = executor->probe_state();
        if (probe_in_flight_) {
            if (state.complete) {
                probe_in_flight_ = false;
                if (state.healthy) {
                    probe_success();
                } else {
                    probe_failure(executor);
                }
            } else if (!state.in_flight) {
                // The probe never began on the worker thread within the
                // submit window: a skip, not a verdict (§7.4). The next
                // schedule re-arms the probe.
                if (monotonic_ms() - probe_submitted_ms_ >=
                    kProbeSubmitSkipWindowMs) {
                    probe_in_flight_ = false;
                    schedule_next_probe();
                }
            } else if (monotonic_ms() > probe_deadline_ms_) {
                // The verdict never arrived in time: cancel so the worker
                // frees its response slot; a failed verdict.
                executor->cancel_probe();
                probe_in_flight_ = false;
                probe_failure(executor);
            }
        } else if (next_probe_due_ms_ != kProbeDisabled &&
                   monotonic_ms() >= next_probe_due_ms_) {
            // Fire the probe THROUGH the executor's probe channel. The
            // URL mirrors the data-plane shape — an absolute URL with the
            // App id as the authority — so the app's fetch handler
            // observes the probe exactly like a real request. A bare
            // relative path would not construct (the worker's URL parser
            // requires a scheme).
            std::string url = "http://" +
                              options_.managed_options->application +
                              (health_check_.path[0] == '/' ? "" : "/") +
                              health_check_.path;
            executor->submit_probe(url);
            probe_in_flight_ = true;
            probe_submitted_ms_ = monotonic_ms();
            probe_deadline_ms_ = probe_submitted_ms_ + health_check_.timeout_ms;
        }
        // The verdict may have recycled the worker just observed (the
        // pool retired it and is spawning a replacement): never touch a
        // worker the pool no longer hands out.
        if (pool->current_worker() != executor) {
            probe_target_ = nullptr;
            probe_in_flight_ = false;
            continue;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

void WorkerSupervisor::rearm_probe(GenerationPool* pool,
                                   WorkerExecutor* executor) {
    probe_target_ = executor;
    probe_in_flight_ = false;
    consecutive_probe_failures_ = 0;
    health_check_ = HealthCheckConfig();
    anchored_generation_ = std::string(pool->generation_digest());
    // The config is re-read from the committed generation on every arm,
    // so a redeployed healthCheck takes effect with the new generation;
    // an App without a readable lifecycle snapshot is never probed.
#if !defined(_WIN32)
    // managed_read_health_check lives in the managed coordinator, which
    // is not built on Windows (see docs/windows.md); managed_options is
    // never wired there, so the default (unconfigured) probe applies.
    if (options_.managed_options != nullptr) {
        health_check_ = managed_read_health_check(
            options_.managed_options, anchored_generation_);
    }
#endif
    // Every new worker gets at least one initial check: the initial
    // probe is due immediately, in parallel with the pool's stability
    // window.
    next_probe_due_ms_ =
        (health_check_.configured && options_.active_health_interval_ms > 0)
            ? monotonic_ms()
            : kProbeDisabled;
}

void WorkerSupervisor::probe_success() {
    consecutive_probe_failures_ = 0;
    schedule_next_probe();
}

void WorkerSupervisor::probe_failure(WorkerExecutor* executor) {
    ++consecutive_probe_failures_;
    LogFields fields;
    fields.level = "warn";
    fields.event = log_events::kHealthProbe;
    fields.app = options_.managed_options->application;
    fields.generation = anchored_generation_;
    fields.result = "failed";
    fields.message = "health probe failed (" +
                     std::to_string(consecutive_probe_failures_) +
                     " consecutive)";
    emit_log(options_.log, LogLane::kApp, std::move(fields));
    if (options_.active_health_failures > 0 &&
        consecutive_probe_failures_ >= options_.active_health_failures) {
        // §7.4: consecutive failed verdicts (timeout, non-2xx, protocol
        // error or oversized body) make the worker UNHEALTHY: recycle it
        // through the pool as a kHealthRecycle instability event,
        // counting against the shared budget (§10.5.2). The pool retires
        // the target (it leaves the READY set immediately) and schedules
        // the replacement; the next loop re-syncs to the fresh worker.
        GenerationPool* pool = options_.current_pool();
        if (pool != nullptr && pool->recycle_worker(executor)) {
            probe_target_ = nullptr;
            probe_in_flight_ = false;
            consecutive_probe_failures_ = 0;
            return;
        }
        // The recycle was stale (the pool already moved on): fall through
        // to the normal schedule; the next loop re-syncs and re-arms.
    }
    schedule_next_probe();
}

// Next probe due after the interval, jittered with the policy's
// basis-point range (same scaling convention as the recovery backoff).
void WorkerSupervisor::schedule_next_probe() {
    const std::int64_t interval =
        static_cast<std::int64_t>(options_.active_health_interval_ms);
    const std::int64_t scaled =
        interval * (10000 + jitter_basis_points());
    next_probe_due_ms_ = monotonic_ms() +
                         static_cast<std::uint64_t>(scaled / 10000);
}

}  // namespace capsid::host
