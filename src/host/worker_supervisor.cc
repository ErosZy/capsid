#include "host/worker_supervisor.h"

#include "capsid/runtime.h"

#include <poll.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>

namespace capsid::host {

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
            capsid_event event;
            const capsid_result result = capsid_worker_next_event(worker, &event);
            if (result == CAPSID_OK &&
                event.type != CAPSID_EVENT_EXIT &&
                event.type != CAPSID_EVENT_ERROR) {
                // Consumable event: worker log or protocol noise. Managed
                // mode has no data plane, so only logs are surfaced.
                if (event.type == CAPSID_EVENT_LOG) {
                    std::fprintf(
                        stderr,
                        "capsid-host: [%s] worker log: %.*s\n",
                        options_.managed_options->application.c_str(),
                        static_cast<int>(event.payload.size),
                        reinterpret_cast<const char*>(event.payload.data));
                }
                continue;
            }
            // EXIT, ERROR, EOF or an unexpected read failure.
            handle_worker_gone(worker);
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
        std::fprintf(stderr,
                     "capsid-host: [%s] cannot read lifecycle after worker "
                     "exit; stopping automatic recovery\n",
                     options_.managed_options->application.c_str());
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
        std::fprintf(stderr,
                     "capsid-host: [%s] instability decision failed: %s; "
                     "stopping automatic recovery\n",
                     options_.managed_options->application.c_str(),
                     decision.error.message.c_str());
        remove_current(worker);
        observed_ = nullptr;
        return;
    }
    if (decision.disposition ==
        GenerationRecoveryDisposition::kBeginQuarantine) {
        quarantine(worker);
        return;
    }
    if (decision.disposition == GenerationRecoveryDisposition::kContinueActive) {
        if (decision.schedule_replacement) {
            attempt_replacement(worker, decision);
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

void WorkerSupervisor::attempt_replacement(capsid_worker* worker,
                                           const WorkerRecoveryDecision& first) {
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
        // The backoff elapsed. The map and the active document must still
        // name the crashed worker's generation; anything else means a
        // deploy or tombstone raced us and owns the outcome.
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
            remove_current(worker);
            observed_ = nullptr;
            return;
        }
        capsid::host::OperationStatus status;
        const DeployOutcome recovered =
            managed_recover(options_.managed_options, &status);
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
                std::fprintf(stderr,
                             "capsid-host: [%s] replaced crashed worker\n",
                             options_.managed_options->application.c_str());
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
        // Spawn/load/READY failure counts toward the crash budget (§10.5).
        // The lifecycle snapshot above anchors the decision.
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
            std::fprintf(stderr,
                         "capsid-host: [%s] replacement failure decision "
                         "failed: %s; stopping automatic recovery\n",
                         options_.managed_options->application.c_str(),
                         next.error.message.c_str());
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
        std::fprintf(stderr,
                     "capsid-host: [%s] quarantine tombstone write failed: "
                     "%s; automatic recovery stopped\n",
                     options_.managed_options->application.c_str(),
                     outcome.error.c_str());
        return;
    }
    std::fprintf(stderr,
                 "capsid-host: [%s] quarantined: crash budget exceeded\n",
                 options_.managed_options->application.c_str());
}

void WorkerSupervisor::remove_current(capsid_worker* worker) {
    options_.remove_if_current(options_.managed_options->application, worker);
    observed_ = nullptr;
}

}  // namespace capsid::host
