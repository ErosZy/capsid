#include "host/worker_supervisor.h"

#include "capsid/runtime.h"
#include "host/managed_admin_backend.h"
#include "host/metrics.h"
#include "host/structured_log.h"

#include <poll.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
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

void count_event(MetricsRegistry* metrics,
                 const std::string& event,
                 const std::string& app,
                 const std::string& generation) {
    if (metrics != nullptr) {
        metrics->count_worker_event(event, app, generation);
    }
}

}  // namespace

// M2 item 6 (design §7.4): the fixed small probe response cap. A probe
// body beyond this fails the verdict regardless of status. Bodies under
// the worker's initial stream window flow without any credit grant; a
// larger body would stall the worker anyway and hit the probe timeout.
inline constexpr std::size_t kProbeResponseBodyCap = 4096;

// A probe schedule marker meaning "never due" (unconfigured App, or
// probing disabled by a zero host interval).
inline constexpr std::uint64_t kProbeDisabled =
    std::numeric_limits<std::uint64_t>::max();

// M2 item 5a: the per-App supervisor thread. It owns the observation of the
// current worker's IPC stream and every decision derived from it (design
// §10.5): replacement with backoff, and quarantine when the crash budget is
// exhausted. Three invariants hold for the whole loop:
//
// 1. The worker map (managed by the Host, exposed through the option
//    closures) is the single source of truth for "which worker is current".
//    Every action is gated by an identity check against it, so a deploy or
//    retire that races the observation always wins and the supervisor
//    re-syncs instead of destroying a fresh worker.
// 2. The worker_recovery functions stay pure decision procedures; the
//    supervisor never calls them while holding map or lifecycle state.
// 3. Shutdown is never instability: the stop signal is checked at every
//    gate, and nothing observed or attempted after stop counts toward any
//    budget.

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
        capsid_worker* worker =
            options_.current_worker(options_.managed_options->application);
        if (worker == nullptr) {
            // No current worker (never deployed, or quarantined/retired):
            // nothing to observe.
            observed_ = nullptr;
            recovery_anchored_ = false;
            tracking_stability_ = false;
            probe_worker_ = nullptr;
            probe_in_flight_ = false;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        if (worker != observed_) {
            // A newly published worker: the deploy/boot-recovery path (or
            // our own replacement) owns the anchor. Re-anchor the recovery
            // state before the next decision; a generation change clears
            // the crash budget and backoff exactly when the new worker
            // appears (matching the post-activation reset of the explicit
            // deploy state machine).
            observed_ = worker;
            const ManagedLifecycleSnapshot snapshot =
                managed_read_lifecycle(options_.managed_options);
            if (!snapshot.ok ||
                snapshot.state.phase != ServiceLifecyclePhase::kActive) {
                recovery_anchored_ = false;
                tracking_stability_ = false;
                ready_since_ms_ = monotonic_ms();
                continue;
            }
            if (!recovery_anchored_ ||
                recovery_state_.generation !=
                    snapshot.state.document->generation) {
                GenerationRecoveryState fresh;
                fresh.application = options_.managed_options->application;
                fresh.generation = snapshot.state.document->generation;
                fresh.last_observed_ms = monotonic_ms();
                fresh.has_last_observed_time = true;
                recovery_state_ = std::move(fresh);
            }
            recovery_anchored_ = true;
            // A worker that just became READY is by definition continuously
            // READY from now on; arm the stability window.
            ready_since_ms_ = monotonic_ms();
            tracking_stability_ = true;
        }
        // M2 item 6 (design §7.4): (re)arm the active health probe for a
        // NEW worker. This is keyed on the worker identity, not the
        // anchor branch above, so it also fires after our own replacement
        // publish (the map hands out a worker probe_worker_ does not yet
        // name). The config is re-read from the committed generation on
        // every arm, so a redeployed healthCheck takes effect with the
        // new generation; an unanchored worker is never probed.
        if (worker != probe_worker_) {
            probe_worker_ = worker;
            probe_in_flight_ = false;
            consecutive_probe_failures_ = 0;
            health_check_ = HealthCheckConfig();
            if (recovery_anchored_) {
                health_check_ = managed_read_health_check(
                    options_.managed_options, recovery_state_.generation);
            }
            // Every new worker gets at least one initial check: the
            // initial probe is due immediately, in parallel with the 5a
            // stability window.
            next_probe_due_ms_ =
                (recovery_anchored_ && health_check_.configured &&
                 options_.active_health_interval_ms > 0)
                    ? monotonic_ms()
                    : kProbeDisabled;
        }
        // In-flight probe deadline: a verdict still pending past the
        // App's healthCheck.timeout is a failed verdict (§7.4 gives the
        // probe its own timeout, independent of the 100 ms poll slice).
        // The request is cancelled so the worker frees its response slot.
        if (probe_in_flight_ &&
            monotonic_ms() > probe_deadline_ms_) {
            // Cancel the request so the worker frees its response slot;
            // the cancel frame must be flushed like any other command
            // (the supervisor thread is the only command sender here).
            (void)capsid_worker_cancel(worker, probe_request_id_);
            (void)capsid_worker_flush(worker);
            probe_in_flight_ = false;
            probe_failure(worker);
        } else if (!probe_in_flight_ &&
                   next_probe_due_ms_ != kProbeDisabled &&
                   monotonic_ms() >= next_probe_due_ms_) {
            start_probe(worker);
        }
        // A probe verdict may have recycled the worker just observed:
        // attempt_replacement's publish destroys it once the replacement
        // is READY (the 5a crash path never falls through to the fd with
        // a destroyed worker — its event branch continues immediately).
        // Re-sync from the map, never touch a worker the map no longer
        // names.
        if (options_.current_worker(options_.managed_options->application) !=
            worker) {
            continue;
        }
        const int fd = capsid_worker_fd(worker);
        if (fd < 0) {
            // No IPC channel: nothing observable. Treat as an exit.
            handle_worker_gone(worker);
            continue;
        }
        struct pollfd pfd;
        pfd.fd = fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        const int polled = poll(&pfd, 1, 100);
        if (polled > 0 && (pfd.revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
            // Drain the parser, not one event per poll: a single read may
            // deliver several frames (a log followed by the probe's head,
            // body and end in one write), and poll cannot fire again once
            // the socket has been drained into the parser — events would
            // sit unread and every probe would fail by deadline.
            for (;;) {
                capsid_event event = {};
                event.struct_size = sizeof(event);
                const capsid_result result =
                    capsid_worker_next_event(worker, &event);
                if (result == CAPSID_WOULD_BLOCK) {
                    break;
                }
                if (result != CAPSID_OK ||
                    event.type == CAPSID_EVENT_EXIT ||
                    event.type == CAPSID_EVENT_ERROR) {
                    // EXIT, ERROR, EOF or an unexpected read failure.
                    handle_worker_gone(worker);
                    break;
                }
                // Consumable event: worker log or protocol noise. Managed
                // mode has no data plane, so only logs are surfaced.
                if (event.type == CAPSID_EVENT_LOG) {
                    // Payload layout: string16(level) + message (the
                    // worker's little-endian two-byte length prefix).
                    const uint8_t* cursor = event.payload.data;
                    const uint8_t* end = cursor + event.payload.size;
                    std::string level;
                    if (event.payload.size >= 2) {
                        const std::size_t level_size =
                            static_cast<std::size_t>(cursor[0]) |
                            (static_cast<std::size_t>(cursor[1]) << 8);
                        cursor += 2;
                        if (level_size <=
                            static_cast<std::size_t>(end - cursor)) {
                            level.assign(
                                reinterpret_cast<const char*>(cursor),
                                level_size);
                            cursor += level_size;
                        }
                    }
                    // §12.2: runtime LOG forwarding lives in the bounded
                    // app lane (droppable + counted); the message payload
                    // is the worker's own sanitized text.
                    LogFields fields;
                    fields.level = level.empty() ? "info" : level;
                    fields.event = log_events::kAppLog;
                    fields.app = options_.managed_options->application;
                    fields.message.assign(
                        reinterpret_cast<const char*>(cursor),
                        static_cast<std::size_t>(end - cursor));
                    emit_log(options_.log, LogLane::kApp, std::move(fields));
                }
                if (probe_in_flight_ &&
                    event.request_id == probe_request_id_) {
                    handle_probe_event(worker, event);
                    // A failed verdict may have recycled the worker
                    // through attempt_replacement's publish, which
                    // destroys it once the replacement is READY: never
                    // touch it again in this drain.
                    if (options_.current_worker(
                            options_.managed_options->application) !=
                        worker) {
                        break;
                    }
                }
            }
            continue;
        }
        // Poll timeout: the worker has remained continuously READY through
        // this 100 ms slice. Once the stability window has fully elapsed,
        // clear the backoff (never the crash budget).
        if (tracking_stability_ && recovery_anchored_) {
            const std::uint64_t now = monotonic_ms();
            if (now - ready_since_ms_ >= options_.policy.stable_reset_ms) {
                const WorkerStabilityResult stability =
                    observe_worker_stability(recovery_state_,
                                             options_.policy,
                                             ready_since_ms_,
                                             now);
                if (stability.ok) {
                    recovery_state_ = stability.state;
                }
                // The window for this worker has elapsed either way; the
                // next re-sync arms the next window.
                tracking_stability_ = false;
            }
        }
    }
}

// M2 item 6: fires one bodyless GET against the App's healthCheck.path
// (design §7.4). The verdict comes back through the poll loop's event
// dispatch; a body under the worker's initial stream window flows without
// any credit grant. CAPSID_WOULD_BLOCK means the IPC command queue is
// full — the only busy source in managed mode (there is no data plane) —
// and the probe is SKIPPED: not counted as a success, and it does not
// reset the consecutive-failure count (§7.4).
void WorkerSupervisor::start_probe(capsid_worker* worker) {
    // The probe URL mirrors the data-plane shape — an absolute URL with
    // the App id as the authority — so the app's fetch handler observes
    // the probe exactly like a real request. A bare relative path would
    // not construct (the worker's URL parser requires a scheme).
    std::string url = "http://" +
                      options_.managed_options->application +
                      (health_check_.path[0] == '/' ? "" : "/") +
                      health_check_.path;
    const capsid_result result = capsid_worker_begin_bodyless_request(
        worker, ++probe_request_id_, "GET", url.c_str(), nullptr, 0);
    if (result == CAPSID_OK) {
        // The data plane flushes commands from its io thread; here the
        // supervisor thread must flush the probe frame itself or the
        // worker never sees it (its responses would then never arrive).
        const capsid_result flushed = capsid_worker_flush(worker);
        if (flushed != CAPSID_OK) {
            // The probe could not be delivered: the worker is effectively
            // unreachable. Fail the verdict.
            probe_failure(worker);
            return;
        }
        probe_in_flight_ = true;
        probe_deadline_ms_ = monotonic_ms() + health_check_.timeout_ms;
        probe_status_ = 0;
        probe_body_bytes_ = 0;
        return;
    }
    if (result == CAPSID_WOULD_BLOCK) {
        // §7.4: the only busy source in managed mode. A skip is neither a
        // success nor a failure verdict; the schedule simply re-arms.
        LogFields fields;
        fields.level = "warn";
        fields.event = log_events::kHealthProbe;
        fields.app = options_.managed_options->application;
        fields.generation = recovery_state_.generation;
        fields.result = "skipped";
        fields.message = "health probe skipped (worker busy)";
        emit_log(options_.log, LogLane::kApp, std::move(fields));
        count_event(options_.metrics, "busy",
                    options_.managed_options->application,
                    recovery_state_.generation);
        schedule_next_probe();
        return;
    }
    // Any other send failure is itself a protocol-level failed verdict.
    probe_failure(worker);
}

void WorkerSupervisor::handle_probe_event(capsid_worker* worker,
                                          const capsid_event& event) {
    switch (event.type) {
    case CAPSID_EVENT_RESPONSE_HEAD:
        probe_status_ = static_cast<std::int32_t>(event.status);
        break;
    case CAPSID_EVENT_RESPONSE_BODY:
        probe_body_bytes_ += event.payload.size;
        if (probe_body_bytes_ > kProbeResponseBodyCap) {
            // §7.4: the fixed small response cap is a protocol failure;
            // stop the body and fail the verdict.
            (void)capsid_worker_cancel(worker, probe_request_id_);
            (void)capsid_worker_flush(worker);
            probe_in_flight_ = false;
            probe_failure(worker);
        }
        break;
    case CAPSID_EVENT_RESPONSE_END:
        probe_in_flight_ = false;
        if (probe_status_ >= 200 && probe_status_ <= 299 &&
            probe_body_bytes_ <= kProbeResponseBodyCap) {
            probe_success();
        } else {
            probe_failure(worker);
        }
        break;
    case CAPSID_EVENT_REQUEST_TIMEOUT:
        probe_in_flight_ = false;
        probe_failure(worker);
        break;
    default:
        break;
    }
}

void WorkerSupervisor::probe_success() {
    consecutive_probe_failures_ = 0;
    schedule_next_probe();
}

void WorkerSupervisor::probe_failure(capsid_worker* worker) {
    ++consecutive_probe_failures_;
    LogFields fields;
    fields.level = "warn";
    fields.event = log_events::kHealthProbe;
    fields.app = options_.managed_options->application;
    fields.generation = recovery_state_.generation;
    fields.result = "failed";
    fields.message = "health probe failed (" +
                     std::to_string(consecutive_probe_failures_) +
                     " consecutive)";
    emit_log(options_.log, LogLane::kApp, std::move(fields));
    if (options_.active_health_failures > 0 &&
        consecutive_probe_failures_ >= options_.active_health_failures) {
        // §7.4: consecutive failed verdicts (timeout, non-2xx, protocol
        // error or oversized body) make the worker UNHEALTHY: recycle it
        // through the item-5a chain as a kHealthRecycle instability
        // event, counting against the shared budget (§10.5.2).
        handle_unhealthy_worker(worker);
        return;
    }
    schedule_next_probe();
}

// Next probe due after the interval, jittered with the policy's
// basis-point range (same scaling convention as the 5a backoff).
void WorkerSupervisor::schedule_next_probe() {
    const std::int64_t interval =
        static_cast<std::int64_t>(options_.active_health_interval_ms);
    const std::int64_t scaled =
        interval * (10000 + jitter_basis_points());
    next_probe_due_ms_ = monotonic_ms() +
                         static_cast<std::uint64_t>(scaled / 10000);
}

// The probe-driven recycle decision: identical gates and decision chain
// to handle_worker_gone, but the worker is still ALIVE — the map still
// names it, so the identity checks hold and the replacement publish
// destroys it exactly like a crashed worker. The only difference is the
// instability kind: kHealthRecycle counts toward the crash budget
// (§10.5.2: an active-health recycle is budgeted like a crash).
void WorkerSupervisor::handle_unhealthy_worker(capsid_worker* worker) {
    if (stop_requested()) {
        observed_ = nullptr;
        return;
    }
    if (options_.current_worker(options_.managed_options->application) !=
        worker) {
        observed_ = nullptr;
        return;
    }
    const ManagedLifecycleSnapshot snapshot =
        managed_read_lifecycle(options_.managed_options);
    if (!snapshot.ok) {
        LogFields fields;
        fields.level = "error";
        fields.event = log_events::kRecoveryDecision;
        fields.app = options_.managed_options->application;
        fields.generation = recovery_state_.generation;
        fields.result = "error";
        fields.message = "cannot read lifecycle after health probe failure; "
                         "stopping automatic recovery";
        emit_log(options_.log, LogLane::kControl, std::move(fields));
        remove_current(worker);
        observed_ = nullptr;
        return;
    }
    if (!recovery_anchored_ ||
        snapshot.state.phase != ServiceLifecyclePhase::kActive ||
        snapshot.state.document->generation != recovery_state_.generation) {
        // The active document moved while the probe was failing: the
        // deploy owns the outcome. Clean up, never count.
        remove_current(worker);
        observed_ = nullptr;
        return;
    }
    WorkerInstabilityObservation observation;
    observation.kind = WorkerInstabilityKind::kHealthRecycle;
    observation.worker_generation = recovery_state_.generation;
    observation.now_ms = monotonic_ms();
    observation.ready_workers_after_removal = 0;
    observation.target_ready_workers = 1;
    observation.replacements_in_flight_for_app = 0;
    observation.chosen_jitter_basis_points = jitter_basis_points();
    const WorkerRecoveryDecision decision = record_worker_instability(
        recovery_state_, options_.policy, snapshot.state, observation);
    recovery_state_ = decision.state;
    if (!decision.ok) {
        LogFields fields;
        fields.level = "error";
        fields.event = log_events::kRecoveryDecision;
        fields.app = options_.managed_options->application;
        fields.generation = recovery_state_.generation;
        fields.result = "error";
        fields.message = "instability decision failed: " +
                         decision.error.message;
        emit_log(options_.log, LogLane::kControl, std::move(fields));
        remove_current(worker);
        observed_ = nullptr;
        return;
    }
    count_event(options_.metrics, "unhealthy",
                options_.managed_options->application,
                recovery_state_.generation);
    // §12.1 recovery family: the remaining instability budget after this
    // counted event, and the backoff the decision chose.
    if (options_.metrics != nullptr) {
        const std::uint64_t remaining =
            decision.events_in_window >= options_.policy.max_events
                ? 0
                : static_cast<std::uint64_t>(
                      options_.policy.max_events -
                      decision.events_in_window);
        options_.metrics->set_recovery_instability_budget_remaining(
            options_.managed_options->application, remaining);
        options_.metrics->set_recovery_backoff_ms(
            options_.managed_options->application,
            decision.replacement_delay_ms);
    }
    if (decision.disposition ==
        GenerationRecoveryDisposition::kBeginQuarantine) {
        quarantine(worker);
        return;
    }
    if (decision.disposition == GenerationRecoveryDisposition::kContinueActive) {
        if (decision.schedule_replacement) {
            attempt_replacement(worker, decision,
                                "replaced unhealthy worker");
            return;
        }
        // Pool full or singleflight already present: nothing to replace.
        remove_current(worker);
        observed_ = nullptr;
        return;
    }
    // kIgnoredExpectedEvent / kStaleGeneration / kUnavailable: not counted.
    remove_current(worker);
    observed_ = nullptr;
}

void WorkerSupervisor::handle_worker_gone(capsid_worker* worker) {
    if (stop_requested()) {
        observed_ = nullptr;
        return;
    }
    // Identity check against the map: if a deploy/retire replaced the
    // entry, the exit belongs to the old worker and the deploy owns the
    // outcome — never counted, never quarantined.
    if (options_.current_worker(options_.managed_options->application) !=
        worker) {
        observed_ = nullptr;
        return;
    }
    const ManagedLifecycleSnapshot snapshot =
        managed_read_lifecycle(options_.managed_options);
    if (!snapshot.ok) {
        LogFields fields;
        fields.level = "error";
        fields.event = log_events::kRecoveryDecision;
        fields.app = options_.managed_options->application;
        fields.generation = recovery_state_.generation;
        fields.result = "error";
        fields.message =
            "cannot read lifecycle after worker exit; "
            "stopping automatic recovery";
        emit_log(options_.log, LogLane::kControl, std::move(fields));
        remove_current(worker);
        observed_ = nullptr;
        return;
    }
    if (!recovery_anchored_ ||
        snapshot.state.phase != ServiceLifecyclePhase::kActive ||
        snapshot.state.document->generation != recovery_state_.generation) {
        // The active document moved while the crash was being observed: the
        // deploy owns the outcome. Clean up, never count.
        remove_current(worker);
        observed_ = nullptr;
        return;
    }
    WorkerInstabilityObservation observation;
    observation.kind = WorkerInstabilityKind::kUnexpectedExit;
    observation.worker_generation = recovery_state_.generation;
    observation.now_ms = monotonic_ms();
    observation.ready_workers_after_removal = 0;
    observation.target_ready_workers = 1;
    observation.replacements_in_flight_for_app = 0;
    observation.chosen_jitter_basis_points = jitter_basis_points();
    const WorkerRecoveryDecision decision = record_worker_instability(
        recovery_state_, options_.policy, snapshot.state, observation);
    recovery_state_ = decision.state;
    if (!decision.ok) {
        LogFields fields;
        fields.level = "error";
        fields.event = log_events::kRecoveryDecision;
        fields.app = options_.managed_options->application;
        fields.generation = recovery_state_.generation;
        fields.result = "error";
        fields.message =
            "instability decision failed: " + decision.error.message;
        emit_log(options_.log, LogLane::kControl, std::move(fields));
        remove_current(worker);
        observed_ = nullptr;
        return;
    }
    count_event(options_.metrics, "crash",
                options_.managed_options->application,
                recovery_state_.generation);
    if (options_.metrics != nullptr) {
        const std::uint64_t remaining =
            decision.events_in_window >= options_.policy.max_events
                ? 0
                : static_cast<std::uint64_t>(
                      options_.policy.max_events -
                      decision.events_in_window);
        options_.metrics->set_recovery_instability_budget_remaining(
            options_.managed_options->application, remaining);
        options_.metrics->set_recovery_backoff_ms(
            options_.managed_options->application,
            decision.replacement_delay_ms);
    }
    if (decision.disposition ==
        GenerationRecoveryDisposition::kBeginQuarantine) {
        quarantine(worker);
        return;
    }
    if (decision.disposition == GenerationRecoveryDisposition::kContinueActive) {
        if (decision.schedule_replacement) {
            attempt_replacement(worker, decision, "replaced crashed worker");
            return;
        }
        // Pool full or singleflight already present: nothing to replace.
        // Remove the dead worker and re-sync (v1 has one worker per App,
        // so this path is only reachable on an invalid policy).
        remove_current(worker);
        observed_ = nullptr;
        return;
    }
    // kIgnoredExpectedEvent / kStaleGeneration / kUnavailable: not counted.
    remove_current(worker);
    observed_ = nullptr;
}

void WorkerSupervisor::attempt_replacement(
    capsid_worker* worker, const WorkerRecoveryDecision& first,
    const char* replaced_message) {
    WorkerRecoveryDecision decision = first;
    for (;;) {
        if (!sleep_interruptible(decision.replacement_delay_ms)) {
            observed_ = nullptr;
            return;
        }
        if (stop_requested()) {
            observed_ = nullptr;
            return;
        }
        // M2 item 5b: the replacement joins the process-global fair
        // startup-permit queue (design §10.5.6) and holds its grant
        // across the respawn, so a crash-looping App cannot persistently
        // queue ahead of another App's deploy. grant_held is true exactly
        // between a successful enqueue_and_wait and release_grant.
        bool grant_held = false;
        if (options_.startup_permits != nullptr &&
            decision.acquire_startup_permit) {
            StartupPermitRequest request;
            request.application = options_.managed_options->application;
            request.generation = recovery_state_.generation;
            request.lane = StartupPermitLane::kReplacement;
            if (!options_.startup_permits->enqueue_and_wait(request)) {
                if (stop_requested()) {
                    observed_ = nullptr;
                    return;
                }
                // The queue rejected the replacement (queue full): the
                // startup could not be scheduled. Count it as a startup
                // failure below — the same re-decision as a failed
                // respawn, which may begin quarantine.
            } else {
                grant_held = true;
                // §12.1 recovery family: a replacement permit was granted.
                if (options_.metrics != nullptr) {
                    options_.metrics->count_recovery_startup_permit_grant(
                        options_.managed_options->application);
                }
            }
        }
        // The grant (or the queue rejection) elapsed. The map and the
        // active document must still name the crashed worker's
        // generation; anything else means a deploy or tombstone raced us
        // and owns the outcome.
        if (options_.current_worker(options_.managed_options->application) !=
            worker) {
            if (grant_held) {
                options_.startup_permits->release_grant();
            }
            observed_ = nullptr;
            return;
        }
        const ManagedLifecycleSnapshot snapshot =
            managed_read_lifecycle(options_.managed_options);
        if (!snapshot.ok ||
            snapshot.state.phase != ServiceLifecyclePhase::kActive ||
            !recovery_anchored_ ||
            snapshot.state.document->generation != recovery_state_.generation) {
            if (grant_held) {
                options_.startup_permits->release_grant();
            }
            remove_current(worker);
            observed_ = nullptr;
            return;
        }
        capsid::host::OperationStatus status;
        const DeployOutcome recovered =
            managed_recover(options_.managed_options, &status);
        if (grant_held) {
            // The permit was consumed by this spawn/READY window; hand it
            // to the next waiter regardless of the outcome.
            options_.startup_permits->release_grant();
        }
        if (recovered.ok && recovered.worker != nullptr) {
            // The replacement is READY against the same committed
            // generation. Publish only if the map still holds the crashed
            // worker; otherwise our spawn lost the race and the worker we
            // created is destroyed directly, never entering the map.
            if (options_.current_worker(options_.managed_options->application) ==
                worker) {
                options_.publish_worker(options_.managed_options->application,
                                        recovered.worker);
                observed_ = recovered.worker;
                ready_since_ms_ = monotonic_ms();
                tracking_stability_ = true;
                recovery_state_.last_observed_ms = ready_since_ms_;
                recovery_state_.has_last_observed_time = true;
                // §12.2: a replacement publish is a process-lifecycle
                // event (control lane, never dropped).
                LogFields fields;
                fields.event = log_events::kWorkerReplaced;
                fields.app = options_.managed_options->application;
                fields.generation = recovery_state_.generation;
                fields.result = "replaced";
                fields.message = replaced_message;
                emit_log(options_.log, LogLane::kControl, std::move(fields));
                count_event(options_.metrics, "replacement",
                            options_.managed_options->application,
                            recovery_state_.generation);
            } else {
                options_.discard_worker(recovered.worker);
                observed_ = nullptr;
            }
            return;
        }
        if (recovered.ok) {
            // The active document no longer names an activatable generation
            // (a retire/quarantine tombstone landed during the backoff).
            remove_current(worker);
            observed_ = nullptr;
            return;
        }
        if (stop_requested()) {
            observed_ = nullptr;
            return;
        }
        // Spawn/load/READY failure (or a rejected queue entry) counts
        // toward the crash budget (§10.5). The lifecycle snapshot above
        // anchors the decision.
        WorkerInstabilityObservation observation;
        observation.kind = WorkerInstabilityKind::kReplacementStartupFailure;
        observation.worker_generation = recovery_state_.generation;
        observation.now_ms = monotonic_ms();
        observation.ready_workers_after_removal = 0;
        observation.target_ready_workers = 1;
        observation.replacements_in_flight_for_app = 0;
        observation.chosen_jitter_basis_points = jitter_basis_points();
        const WorkerRecoveryDecision next = record_worker_instability(
            recovery_state_, options_.policy, snapshot.state, observation);
        recovery_state_ = next.state;
        if (!next.ok) {
            LogFields fields;
            fields.level = "error";
            fields.event = log_events::kRecoveryDecision;
            fields.app = options_.managed_options->application;
            fields.generation = recovery_state_.generation;
            fields.result = "error";
            fields.message = "replacement failure decision failed: " +
                             next.error.message;
            emit_log(options_.log, LogLane::kControl, std::move(fields));
            remove_current(worker);
            observed_ = nullptr;
            return;
        }
        if (next.disposition ==
            GenerationRecoveryDisposition::kBeginQuarantine) {
            quarantine(worker);
            return;
        }
        if (!next.schedule_replacement) {
            remove_current(worker);
            observed_ = nullptr;
            return;
        }
        // One more attempt with the advanced backoff.
        decision = next;
    }
}

void WorkerSupervisor::quarantine(capsid_worker* worker) {
    if (stop_requested()) {
        observed_ = nullptr;
        return;
    }
    // The tombstone must quarantine the generation the decision was made
    // against. A generation move between the decision and this write means
    // a deploy owns the outcome; the fresh generation has a fresh budget.
    if (options_.current_worker(options_.managed_options->application) !=
        worker) {
        observed_ = nullptr;
        return;
    }
    const ManagedLifecycleSnapshot snapshot =
        managed_read_lifecycle(options_.managed_options);
    if (!snapshot.ok ||
        snapshot.state.phase != ServiceLifecyclePhase::kActive ||
        !recovery_anchored_ ||
        snapshot.state.document->generation != recovery_state_.generation) {
        observed_ = nullptr;
        return;
    }
    capsid::host::OperationStatus status;
    const DeployOutcome outcome =
        managed_quarantine(options_.managed_options, &status);
    // Destroy the dead worker after the tombstone is durable: a crash in
    // the window between leaves a quarantined document, which boot recovery
    // honors (kKeepQuarantined never resurrects).
    remove_current(worker);
    observed_ = nullptr;
    tracking_stability_ = false;
    if (!outcome.ok) {
        // §12.2: CRASH_BUDGET_EXCEEDED and the quarantine transition are
        // control-plane events — the control lane never drops them.
        LogFields fields;
        fields.level = "error";
        fields.event = log_events::kQuarantine;
        fields.app = options_.managed_options->application;
        fields.generation = recovery_state_.generation;
        fields.result = "error";
        fields.message = "quarantine tombstone write failed: " +
                         outcome.error + "; automatic recovery stopped";
        emit_log(options_.log, LogLane::kControl, std::move(fields));
        return;
    }
    LogFields fields;
    fields.event = log_events::kQuarantine;
    fields.app = options_.managed_options->application;
    fields.generation = recovery_state_.generation;
    fields.result = "crash_budget_exceeded";
    fields.message = "quarantined: crash budget exceeded";
    emit_log(options_.log, LogLane::kControl, std::move(fields));
    if (options_.metrics != nullptr) {
        options_.metrics->count_recovery_quarantine(
            options_.managed_options->application);
    }
}

void WorkerSupervisor::remove_current(capsid_worker* worker) {
    options_.remove_if_current(options_.managed_options->application, worker);
    observed_ = nullptr;
}

}  // namespace capsid::host
