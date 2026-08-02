// Worker crash budget, replacement backoff and startup-permit fairness: the
// pure control-plane slice for recovering unstable generations.
//
// This slice never spawns workers or reads a clock; the caller passes a
// monotonic now_ms and a sampled jitter. Every identifier/document check
// reuses the active-state contract from active_state.cc — no grammar is
// copied here. Unknown enums, malformed lifecycle state, unsorted or
// oversized event lists and clock regressions all fail closed atomically.
// Crash budgets and the permit queue are bounded (kMaxTrackedInstabilityEvents)
// so a hostile or buggy caller can never grow state without limit.

#include "host/worker_recovery.h"

#include <cstdint>
#include <limits>
#include <string>
#include <utility>

namespace capsid::host {
namespace {

using ErrorCode = WorkerRecoveryErrorCode;

void set_error(WorkerRecoveryError &error,
               ErrorCode code,
               std::string path,
               std::string message) {
    error.code = code;
    error.path = std::move(path);
    error.message = std::move(message);
}

bool valid_policy(const WorkerRecoveryPolicy &policy) {
    if (policy.max_events == 0U ||
        policy.max_events > kMaxTrackedInstabilityEvents) {
        return false;
    }
    if (policy.window_ms == 0U) {
        return false;
    }
    if (policy.backoff_initial_ms == 0U ||
        policy.backoff_maximum_ms < policy.backoff_initial_ms) {
        return false;
    }
    // Jitter is expressed in basis points; more than +-100% is meaningless.
    if (policy.jitter_basis_points > 10000U) {
        return false;
    }
    if (policy.stable_reset_ms == 0U) {
        return false;
    }
    if (policy.replacements_concurrent_per_app == 0U) {
        return false;
    }
    // The jittered product of the maximum base backoff must fit the signed
    // scaling used in replacement_delay_ms(); a policy that could overflow is
    // rejected instead of silently truncating the delay.
    const std::uint64_t max_factor = 10000U + policy.jitter_basis_points;
    const std::uint64_t max_scaled = static_cast<std::uint64_t>(
        std::numeric_limits<std::int64_t>::max());
    if (policy.backoff_maximum_ms > max_scaled / max_factor) {
        return false;
    }
    return true;
}

// Validates the lifecycle document with the full active-state contract and
// then checks the phase/document pairing exactly like the service lifecycle
// slice: a malformed identifier can never count events or schedule work.
bool lifecycle_consistent(const ServiceLifecycleState &lifecycle) {
    if (!lifecycle.document.has_value()) {
        return lifecycle.phase == ServiceLifecyclePhase::kAbsent ||
               lifecycle.phase == ServiceLifecyclePhase::kFailedClosed;
    }
    if (!encode_active_state_json(*lifecycle.document).ok) {
        return false;
    }
    switch (lifecycle.phase) {
    case ServiceLifecyclePhase::kAbsent:
        return false;
    case ServiceLifecyclePhase::kActive:
        return lifecycle.document->state == ActiveServiceState::kActive;
    case ServiceLifecyclePhase::kRetired:
        return lifecycle.document->state == ActiveServiceState::kRetired;
    case ServiceLifecyclePhase::kQuarantined:
        return lifecycle.document->state == ActiveServiceState::kQuarantined;
    // A pending retire retains its source active/quarantined document until
    // persistence resolves; after a committed tombstone it retains the
    // retired document while draining.
    case ServiceLifecyclePhase::kRetiring:
        return lifecycle.document->state == ActiveServiceState::kActive ||
               lifecycle.document->state == ActiveServiceState::kRetired ||
               lifecycle.document->state == ActiveServiceState::kQuarantined;
    // A pending quarantine retains its source active document.
    case ServiceLifecyclePhase::kQuarantining:
        return lifecycle.document->state == ActiveServiceState::kActive;
    case ServiceLifecyclePhase::kDurabilityUncertain:
    case ServiceLifecyclePhase::kFailedClosed:
        return true;
    }
    return false;
}

bool events_sorted(const std::vector<std::uint64_t> &events) {
    for (std::size_t i = 1; i < events.size(); ++i) {
        if (events[i] < events[i - 1]) {
            return false;
        }
    }
    return true;
}

bool instability_kind_is_expected(WorkerInstabilityKind kind) {
    return kind == WorkerInstabilityKind::kNormalDrain ||
           kind == WorkerInstabilityKind::kHostShutdown ||
           kind == WorkerInstabilityKind::kOperatorRetire;
}

// App/generation grammar lives in exactly one place: probe the active-state
// encoder with a minimal but valid document.
bool request_identifiers_valid(std::string_view application,
                               std::string_view generation) {
    ActiveStateDocument probe;
    probe.state = ActiveServiceState::kActive;
    probe.application = std::string(application);
    probe.version = "v0";
    probe.generation = std::string(generation);
    return encode_active_state_json(probe).ok;
}

inline constexpr std::string_view kProbeGeneration =
    "sha256:0000000000000000000000000000000000000000000000000000000000000000";

bool valid_application_id(std::string_view application) {
    return request_identifiers_valid(application, kProbeGeneration);
}

bool request_is_valid(const StartupPermitRequest &request) {
    return (request.lane == StartupPermitLane::kDeploy ||
            request.lane == StartupPermitLane::kReplacement) &&
           request_identifiers_valid(request.application, request.generation);
}

// Atomic guard for caller-supplied queues: every entry must be a valid
// request, tickets must be unique across the queue, the replacement
// singleflight invariant must hold (at most one queued replacement per
// App/generation), and a non-empty last_granted_application must be a valid
// App ID (it would otherwise participate in fairness selection). A malformed
// existing queue fails closed with kInvalidState and stays untouched.
bool queue_invariants_ok(const FairStartupPermitQueue &queue) {
    if (!queue.last_granted_application.empty() &&
        !valid_application_id(queue.last_granted_application)) {
        return false;
    }
    for (std::size_t i = 0; i < queue.queued.size(); ++i) {
        const StartupPermitRequest &request = queue.queued[i];
        if (!request_is_valid(request)) {
            return false;
        }
        for (std::size_t j = 0; j < i; ++j) {
            const StartupPermitRequest &other = queue.queued[j];
            if (other.ticket == request.ticket) {
                return false;
            }
            if (other.lane == StartupPermitLane::kReplacement &&
                request.lane == StartupPermitLane::kReplacement &&
                other.application == request.application &&
                other.generation == request.generation) {
                return false;
            }
        }
    }
    return true;
}

// Exponential backoff capped before jitter is applied: the cap is on the
// base delay, and jitter may push the final delay above it. valid_policy()
// guarantees the jittered product cannot overflow the signed scaling.
std::uint64_t replacement_delay_ms(const WorkerRecoveryPolicy &policy,
                                   std::uint32_t attempts,
                                   std::int32_t jitter_basis_points) {
    std::uint64_t base = policy.backoff_initial_ms;
    const std::uint64_t cap = policy.backoff_maximum_ms;
    for (std::uint32_t i = 0; i < attempts && base < cap; ++i) {
        if (base > cap / 2) {
            base = cap;
            break;
        }
        base *= 2;
    }
    const std::int64_t scaled =
        static_cast<std::int64_t>(base) * (10000 + jitter_basis_points);
    return static_cast<std::uint64_t>(scaled / 10000);
}

}  // namespace

WorkerRecoveryDecision record_worker_instability(
    const GenerationRecoveryState &state,
    const WorkerRecoveryPolicy &policy,
    const ServiceLifecycleState &lifecycle,
    const WorkerInstabilityObservation &observation) {
    WorkerRecoveryDecision decision;
    decision.state = state;
    decision.instability_kind = observation.kind;

    if (!valid_policy(policy)) {
        set_error(decision.error, ErrorCode::kInvalidPolicy, "/policy",
                  "recovery policy is invalid");
        return decision;
    }
    if (!lifecycle_consistent(lifecycle)) {
        set_error(decision.error, ErrorCode::kInvalidState, "/state",
                  "lifecycle phase and document are inconsistent");
        return decision;
    }
    // The observation kind must be a known enum value; anything else fails
    // closed before any branch can be taken.
    switch (observation.kind) {
    case WorkerInstabilityKind::kUnexpectedExit:
    case WorkerInstabilityKind::kCgroupOom:
    case WorkerInstabilityKind::kSynchronousCpuTimeout:
    case WorkerInstabilityKind::kIpcFailure:
    case WorkerInstabilityKind::kProtocolFailure:
    case WorkerInstabilityKind::kHealthRecycle:
    case WorkerInstabilityKind::kReplacementStartupFailure:
    case WorkerInstabilityKind::kNormalDrain:
    case WorkerInstabilityKind::kHostShutdown:
    case WorkerInstabilityKind::kOperatorRetire:
        break;
    default:
        set_error(decision.error, ErrorCode::kInvalidState, "/kind",
                  "unknown instability kind");
        return decision;
    }
    const std::int32_t jitter = observation.chosen_jitter_basis_points;
    const std::int32_t jitter_limit =
        static_cast<std::int32_t>(policy.jitter_basis_points);
    if (jitter < -jitter_limit || jitter > jitter_limit) {
        set_error(decision.error, ErrorCode::kInvalidJitter, "/jitter",
                  "chosen jitter exceeds the policy jitter");
        return decision;
    }
    if (!events_sorted(state.instability_events_ms)) {
        set_error(decision.error, ErrorCode::kInvalidState, "/state",
                  "instability events are not sorted");
        return decision;
    }
    if (state.instability_events_ms.size() > kMaxTrackedInstabilityEvents) {
        set_error(decision.error, ErrorCode::kResourceLimit, "/state",
                  "instability event state exceeds the tracked limit");
        return decision;
    }
    // Future-dated events are a clock anomaly and fail closed; they are never
    // silently trimmed by the window.
    for (const std::uint64_t event_ms : state.instability_events_ms) {
        if (event_ms > observation.now_ms) {
            set_error(decision.error, ErrorCode::kClockRegression, "/state",
                      "instability event lies in the future");
            return decision;
        }
    }
    if (state.has_last_observed_time &&
        observation.now_ms < state.last_observed_ms) {
        set_error(decision.error, ErrorCode::kClockRegression, "/state",
                  "monotonic clock regressed");
        return decision;
    }
    // The state's own App/generation must pass the active-state contract
    // before any ok flag, timestamp update or stale judgment can happen: a
    // malformed generation is not a stale generation.
    if (!request_identifiers_valid(state.application, state.generation)) {
        set_error(decision.error, ErrorCode::kInvalidState, "/state",
                  "recovery state identity is invalid");
        return decision;
    }

    // A consistent but non-active lifecycle has no work to recover; it is a
    // clean, uncounted unavailable result.
    if (lifecycle.phase != ServiceLifecyclePhase::kActive) {
        decision.ok = true;
        decision.state.last_observed_ms = observation.now_ms;
        decision.state.has_last_observed_time = true;
        decision.disposition = GenerationRecoveryDisposition::kUnavailable;
        return decision;
    }

    // The recovery state must belong to this App; a state for another App is
    // an internal error. A state for an older generation of the same App is
    // simply stale (checked after ok is set below).
    if (state.application != lifecycle.document->application) {
        set_error(decision.error, ErrorCode::kInvalidState, "/state",
                  "recovery state does not match the active application");
        return decision;
    }
    // Only a worker failure that would count validates worker_generation: a
    // malformed value is an observation error, while a valid value from a
    // different generation is a clean stale result. Expected lifecycle
    // events never carry a worker generation and are exempt.
    if (!instability_kind_is_expected(observation.kind) &&
        observation.worker_generation != lifecycle.document->generation &&
        !request_identifiers_valid(state.application,
                                   observation.worker_generation)) {
        set_error(decision.error, ErrorCode::kInvalidState, "/worker-generation",
                  "worker generation is malformed");
        return decision;
    }

    decision.ok = true;
    decision.state.last_observed_ms = observation.now_ms;
    decision.state.has_last_observed_time = true;

    // Normal drain, host shutdown and operator retire consume no budget and
    // schedule no replacement.
    if (instability_kind_is_expected(observation.kind)) {
        decision.disposition =
            GenerationRecoveryDisposition::kIgnoredExpectedEvent;
        return decision;
    }
    // The recovery state predates the active generation: the whole state is
    // a clean stale result, never an error.
    if (state.generation != lifecycle.document->generation) {
        decision.disposition = GenerationRecoveryDisposition::kStaleGeneration;
        return decision;
    }
    // A worker from a different (already validated) generation cannot spend
    // this generation's budget; it is a clean stale result, not an error.
    if (observation.worker_generation != lifecycle.document->generation) {
        decision.disposition = GenerationRecoveryDisposition::kStaleGeneration;
        return decision;
    }

    // The rolling window is (now - window_ms, now]: an event whose age is
    // exactly window_ms has expired. Future-dated events were already
    // rejected above, so the age computation cannot underflow.
    std::vector<std::uint64_t> window;
    window.reserve(state.instability_events_ms.size() + 1);
    for (const std::uint64_t event_ms : state.instability_events_ms) {
        if (observation.now_ms - event_ms < policy.window_ms) {
            window.push_back(event_ms);
        }
    }
    window.push_back(observation.now_ms);
    decision.event_counted = true;
    decision.events_in_window = window.size();
    // The logical count may exceed the hard cap (the 1025th event triggers
    // quarantine), but the persisted timestamps stay within the hard bound:
    // keep the newest events, which are exactly the ones the next window
    // trim would keep anyway.
    if (window.size() > kMaxTrackedInstabilityEvents) {
        window.erase(
            window.begin(),
            window.end() -
                static_cast<std::vector<std::uint64_t>::difference_type>(
                    kMaxTrackedInstabilityEvents));
    }
    decision.state.instability_events_ms = std::move(window);

    // The budget is decided before any retry/replacement work: exceeding
    // max_events begins quarantine and suppresses replacement in the same
    // result, never retry, never replacement.
    if (decision.events_in_window > policy.max_events) {
        decision.disposition =
            GenerationRecoveryDisposition::kBeginQuarantine;
        decision.begin_quarantine = true;
        return decision;
    }

    // A replacement is only needed when the pool has a capacity deficit; a
    // full pool still counts the event but must not spawn. This is not a
    // singleflight join — no replacement exists, one is simply not needed.
    if (observation.ready_workers_after_removal >=
        observation.target_ready_workers) {
        decision.disposition = GenerationRecoveryDisposition::kContinueActive;
        return decision;
    }

    // One in-flight replacement per App is enough; joining it counts the
    // event but must not schedule a second spawn or advance the backoff.
    if (observation.replacements_in_flight_for_app >=
        policy.replacements_concurrent_per_app) {
        decision.replacement_singleflight_exists = true;
        decision.disposition = GenerationRecoveryDisposition::kContinueActive;
        return decision;
    }

    // Only an actually scheduled replacement advances the attempt counter,
    // and the counter saturates instead of wrapping back to zero.
    decision.replacement_delay_ms = replacement_delay_ms(
        policy, decision.state.replacement_attempts_since_stable, jitter);
    decision.schedule_replacement = true;
    decision.acquire_startup_permit = true;
    decision.acquire_memory_permit = true;
    if (decision.state.replacement_attempts_since_stable !=
        std::numeric_limits<std::uint32_t>::max()) {
        ++decision.state.replacement_attempts_since_stable;
    }
    decision.disposition = GenerationRecoveryDisposition::kContinueActive;
    return decision;
}

WorkerStabilityResult observe_worker_stability(
    const GenerationRecoveryState &state,
    const WorkerRecoveryPolicy &policy,
    std::uint64_t continuously_ready_since_ms,
    std::uint64_t now_ms) {
    WorkerStabilityResult result;
    result.state = state;
    if (!valid_policy(policy)) {
        set_error(result.error, ErrorCode::kInvalidPolicy, "/policy",
                  "recovery policy is invalid");
        return result;
    }
    // Without a lifecycle to compare against, the state's own App/generation
    // identity must pass the active-state contract.
    if (!request_identifiers_valid(state.application, state.generation)) {
        set_error(result.error, ErrorCode::kInvalidState, "/state",
                  "recovery state identity is invalid");
        return result;
    }
    if (!events_sorted(state.instability_events_ms)) {
        set_error(result.error, ErrorCode::kInvalidState, "/state",
                  "instability events are not sorted");
        return result;
    }
    if (state.instability_events_ms.size() > kMaxTrackedInstabilityEvents) {
        set_error(result.error, ErrorCode::kResourceLimit, "/state",
                  "instability event state exceeds the tracked limit");
        return result;
    }
    // Future-dated events are a clock anomaly and fail closed.
    for (const std::uint64_t event_ms : state.instability_events_ms) {
        if (event_ms > now_ms) {
            set_error(result.error, ErrorCode::kClockRegression, "/state",
                      "instability event lies in the future");
            return result;
        }
    }
    if (state.has_last_observed_time && now_ms < state.last_observed_ms) {
        set_error(result.error, ErrorCode::kClockRegression, "/state",
                  "monotonic clock regressed");
        return result;
    }
    // READY can only be observed in the past; a future READY timestamp is a
    // clock anomaly, not a zero-length stability interval.
    if (continuously_ready_since_ms > now_ms) {
        set_error(result.error, ErrorCode::kClockRegression, "/state",
                  "READY timestamp lies in the future");
        return result;
    }
    result.state.last_observed_ms = now_ms;
    result.state.has_last_observed_time = true;
    result.ok = true;
    // Continuous stability resets only the backoff; crash-budget events stay.
    if (now_ms - continuously_ready_since_ms >= policy.stable_reset_ms) {
        result.state.replacement_attempts_since_stable = 0;
        result.backoff_reset = true;
    }
    return result;
}

FailedRequestAction decide_failed_request_action(
    const WorkerRecoveryDecision &recovery,
    const FailedRequestRetryContext &request) {
    // A response head that already left the gateway must never be rewritten.
    if (request.response_head_sent) {
        return FailedRequestAction::kAbortStartedResponse;
    }
    // Quarantine, failed decisions and non-active dispositions never retry.
    // v1 retries only worker/IPC/protocol failures; OOM, CPU timeouts and
    // health recycles are resources the gateway must not amplify.
    if (!recovery.ok || recovery.begin_quarantine ||
        recovery.disposition !=
            GenerationRecoveryDisposition::kContinueActive) {
        return FailedRequestAction::kSynthesize503;
    }
    switch (recovery.instability_kind) {
    case WorkerInstabilityKind::kUnexpectedExit:
    case WorkerInstabilityKind::kIpcFailure:
    case WorkerInstabilityKind::kProtocolFailure:
        break;
    default:
        return FailedRequestAction::kSynthesize503;
    }
    // One retry, GET/HEAD, no body, no response head yet, enough deadline and
    // a READY worker from the same active generation.
    if ((request.method != "GET" && request.method != "HEAD") ||
        request.request_body_present_or_started ||
        !request.deadline_allows_retry ||
        !request.same_active_generation_worker_available ||
        request.retries_already_attempted > 0) {
        return FailedRequestAction::kSynthesize503;
    }
    return FailedRequestAction::kRetryOnceSameGeneration;
}

StartupPermitQueueResult enqueue_startup_permit_request(
    const FairStartupPermitQueue &queue,
    const StartupPermitRequest &request,
    std::size_t maximum_queued) {
    StartupPermitQueueResult result;
    result.queue = queue;
    // The caller-supplied queue is validated first and stays untouched: a
    // malformed existing queue fails closed as kInvalidState.
    if (!queue_invariants_ok(queue)) {
        set_error(result.error, ErrorCode::kInvalidState, "/queue",
                  "existing permit queue is malformed");
        return result;
    }
    // The new request itself uses the request-level error contract.
    if (request.lane != StartupPermitLane::kDeploy &&
        request.lane != StartupPermitLane::kReplacement) {
        set_error(result.error, ErrorCode::kInvalidRequest, "/request",
                  "unknown startup permit lane");
        return result;
    }
    if (!request_identifiers_valid(request.application, request.generation)) {
        set_error(result.error, ErrorCode::kInvalidRequest, "/request",
                  "invalid App ID or generation");
        return result;
    }
    // Tickets are unique across the whole queue, not just per App.
    for (const StartupPermitRequest &queued : queue.queued) {
        if (queued.ticket == request.ticket) {
            set_error(result.error, ErrorCode::kInvalidRequest, "/ticket",
                      "duplicate startup permit ticket");
            return result;
        }
    }
    // Exact replacement App/generation requests are singleflight: a
    // replacement request joins an existing replacement for the same
    // App/generation (deploy requests never join), and joining adds no queue
    // entry, so it cannot hit the queue limit.
    if (request.lane == StartupPermitLane::kReplacement) {
        for (const StartupPermitRequest &queued : queue.queued) {
            if (queued.lane == StartupPermitLane::kReplacement &&
                queued.application == request.application &&
                queued.generation == request.generation) {
                result.joined_existing = true;
                break;
            }
        }
    }
    if (!result.joined_existing) {
        if (queue.queued.size() >= maximum_queued) {
            set_error(result.error, ErrorCode::kQueueFull, "/queue",
                      "startup permit queue is full");
            return result;
        }
        result.queue.queued.push_back(request);
    }
    result.ok = true;
    for (const StartupPermitRequest &queued : result.queue.queued) {
        if (queued.lane == StartupPermitLane::kDeploy) {
            ++result.queued_deploys;
        } else {
            ++result.queued_replacements;
        }
    }
    return result;
}

StartupPermitGrantResult grant_next_startup_permit(
    const FairStartupPermitQueue &queue,
    bool permit_available) {
    StartupPermitGrantResult result;
    result.queue = queue;
    // The caller-supplied queue is validated before anything is granted; a
    // malformed queue fails closed as kInvalidState and stays untouched.
    if (!queue_invariants_ok(queue)) {
        set_error(result.error, ErrorCode::kInvalidState, "/queue",
                  "existing permit queue is malformed");
        return result;
    }
    if (permit_available && !queue.queued.empty()) {
        // Across Apps, avoid serving the last-granted App again whenever
        // another App is waiting; within the selected App the oldest request
        // (queue order) wins. If every waiting request belongs to the
        // last-granted App, the head of the queue is granted.
        std::size_t index = 0;
        for (std::size_t i = 0; i < queue.queued.size(); ++i) {
            if (queue.queued[i].application != queue.last_granted_application) {
                index = i;
                break;
            }
        }
        result.granted = queue.queued[index];
        result.queue.queued.erase(result.queue.queued.begin() + index);
        result.queue.last_granted_application = result.granted->application;
    }
    result.ok = true;
    for (const StartupPermitRequest &queued : result.queue.queued) {
        if (queued.lane == StartupPermitLane::kDeploy) {
            ++result.queued_deploys;
        } else {
            ++result.queued_replacements;
        }
    }
    return result;
}

}  // namespace capsid::host
