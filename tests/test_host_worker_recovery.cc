#include "host/managed_admin_backend.h"
#include "host/worker_recovery.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace {

using namespace capsid::host;

constexpr std::string_view kGenerationOne =
    "sha256:1111111111111111111111111111111111111111111111111111111111111111";
constexpr std::string_view kGenerationTwo =
    "sha256:2222222222222222222222222222222222222222222222222222222222222222";

[[noreturn]] void fail(const std::string& message) {
    std::cerr << "test-host-worker-recovery: " << message << std::endl;
    std::exit(1);
}

void require(bool condition, const std::string& message) {
    if (!condition) {
        fail(message);
    }
}

WorkerRecoveryPolicy policy(std::uint32_t max_events = 5) {
    WorkerRecoveryPolicy value;
    value.max_events = max_events;
    value.window_ms = 60000;
    value.backoff_initial_ms = 250;
    value.backoff_maximum_ms = 30000;
    value.jitter_basis_points = 2000;
    value.stable_reset_ms = 60000;
    value.replacements_concurrent_per_app = 1;
    return value;
}

ActiveStateDocument active_document(
    std::string_view generation = kGenerationTwo) {
    ActiveStateDocument document;
    document.state = ActiveServiceState::kActive;
    document.application = "orders";
    document.version = "v2";
    document.generation = std::string(generation);
    return document;
}

ServiceLifecycleState lifecycle(
    ServiceLifecyclePhase phase = ServiceLifecyclePhase::kActive,
    std::string_view generation = kGenerationTwo) {
    return ServiceLifecycleState{phase, active_document(generation)};
}

GenerationRecoveryState recovery_state(
    std::string_view generation = kGenerationTwo) {
    GenerationRecoveryState state;
    state.application = "orders";
    state.generation = std::string(generation);
    return state;
}

WorkerInstabilityObservation observation(
    WorkerInstabilityKind kind,
    std::uint64_t now_ms,
    std::int32_t jitter_basis_points = 0) {
    WorkerInstabilityObservation value;
    value.kind = kind;
    value.worker_generation = std::string(kGenerationTwo);
    value.now_ms = now_ms;
    value.target_ready_workers = 1;
    value.chosen_jitter_basis_points = jitter_basis_points;
    return value;
}

void require_clean(const WorkerRecoveryDecision& result,
                   std::string_view label) {
    require(result.ok, std::string(label) + " failed: " +
                           result.error.message);
    require(result.error.code == WorkerRecoveryErrorCode::kNone &&
                result.error.path.empty() && result.error.message.empty(),
            std::string(label) + " succeeded with a stale error");
}

void require_error(const WorkerRecoveryDecision& result,
                   WorkerRecoveryErrorCode code,
                   std::string_view label) {
    require(!result.ok, std::string(label) + " succeeded");
    require(result.error.code == code && !result.error.path.empty() &&
                !result.error.message.empty() && !result.event_counted &&
                !result.begin_quarantine && !result.schedule_replacement &&
                !result.acquire_startup_permit &&
                !result.acquire_memory_permit,
            std::string(label) + " did not fail atomically");
}

void test_replacement_requires_a_deficit_and_old_generation_is_stale() {
    WorkerInstabilityObservation no_deficit = observation(
        WorkerInstabilityKind::kUnexpectedExit, 1);
    no_deficit.ready_workers_after_removal = 1;
    no_deficit.target_ready_workers = 1;
    const WorkerRecoveryDecision unnecessary = record_worker_instability(
        recovery_state(), policy(), lifecycle(), no_deficit);
    require_clean(unnecessary, "failure without a pool deficit");
    require(unnecessary.event_counted &&
                unnecessary.disposition ==
                    GenerationRecoveryDisposition::kContinueActive &&
                !unnecessary.schedule_replacement &&
                !unnecessary.replacement_singleflight_exists &&
                !unnecessary.acquire_startup_permit &&
                !unnecessary.acquire_memory_permit &&
                unnecessary.replacement_delay_ms == 0 &&
                unnecessary.state.replacement_attempts_since_stable == 0,
            "full pool scheduled an unnecessary replacement");

    GenerationRecoveryState old_state = recovery_state(kGenerationOne);
    WorkerInstabilityObservation old_worker = observation(
        WorkerInstabilityKind::kUnexpectedExit, 2);
    old_worker.worker_generation = std::string(kGenerationOne);
    const WorkerRecoveryDecision stale = record_worker_instability(
        old_state, policy(), lifecycle(), old_worker);
    require_clean(stale, "old generation after active switch");
    require(stale.disposition ==
                GenerationRecoveryDisposition::kStaleGeneration &&
                !stale.event_counted && !stale.schedule_replacement,
            "old generation after active switch was not ignored as stale");
}

void test_rolling_budget_counts_only_instability_events() {
    GenerationRecoveryState state = recovery_state();
    const WorkerRecoveryPolicy test_policy = policy(2);

    for (const WorkerInstabilityKind kind : {
             WorkerInstabilityKind::kNormalDrain,
             WorkerInstabilityKind::kHostShutdown,
             WorkerInstabilityKind::kOperatorRetire}) {
        const WorkerRecoveryDecision ignored = record_worker_instability(
            state, test_policy, lifecycle(), observation(kind, 0));
        require_clean(ignored, "expected lifecycle event");
        require(ignored.disposition ==
                    GenerationRecoveryDisposition::kIgnoredExpectedEvent &&
                    !ignored.event_counted &&
                    ignored.state.instability_events_ms.empty() &&
                    !ignored.schedule_replacement,
                "expected lifecycle event consumed crash budget");
        state = ignored.state;
    }

    WorkerRecoveryDecision first = record_worker_instability(
        state,
        test_policy,
        lifecycle(),
        observation(WorkerInstabilityKind::kUnexpectedExit, 0, -2000));
    require_clean(first, "first instability");
    require(first.event_counted && first.events_in_window == 1 &&
                first.disposition ==
                    GenerationRecoveryDisposition::kContinueActive &&
                first.schedule_replacement &&
                first.acquire_startup_permit &&
                first.acquire_memory_permit &&
                first.replacement_delay_ms == 200 &&
                first.state.replacement_attempts_since_stable == 1,
            "first instability did not schedule initial replacement");

    WorkerRecoveryDecision second = record_worker_instability(
        first.state,
        test_policy,
        lifecycle(),
        observation(WorkerInstabilityKind::kReplacementStartupFailure,
                    59999));
    require_clean(second, "second instability");
    require(second.events_in_window == 2 && second.schedule_replacement &&
                second.replacement_delay_ms == 500 &&
                second.state.replacement_attempts_since_stable == 2,
            "replacement failure did not double backoff");

    // At exactly window_ms the event at t=0 has expired.
    WorkerRecoveryDecision boundary = record_worker_instability(
        second.state,
        test_policy,
        lifecycle(),
        observation(WorkerInstabilityKind::kIpcFailure, 60000));
    require_clean(boundary, "window boundary instability");
    require(boundary.events_in_window == 2 &&
                !boundary.begin_quarantine &&
                boundary.replacement_delay_ms == 1000,
            "rolling window boundary was not half-open");

    WorkerRecoveryDecision exceeded = record_worker_instability(
        boundary.state,
        test_policy,
        lifecycle(),
        observation(WorkerInstabilityKind::kProtocolFailure, 60001));
    require_clean(exceeded, "budget-exceeding instability");
    require(exceeded.event_counted && exceeded.events_in_window == 3 &&
                exceeded.disposition ==
                    GenerationRecoveryDisposition::kBeginQuarantine &&
                exceeded.begin_quarantine &&
                !exceeded.schedule_replacement &&
                !exceeded.replacement_singleflight_exists &&
                !exceeded.acquire_startup_permit &&
                !exceeded.acquire_memory_permit &&
                exceeded.replacement_delay_ms == 0,
            "budget exceed did not suppress replacement atomically");
}

void test_quarantine_is_decided_before_request_retry() {
    const WorkerRecoveryPolicy test_policy = policy(1);
    const WorkerRecoveryDecision under_budget = record_worker_instability(
        recovery_state(),
        test_policy,
        lifecycle(),
        observation(WorkerInstabilityKind::kUnexpectedExit, 10));
    require_clean(under_budget, "under-budget failure");

    FailedRequestRetryContext eligible;
    eligible.method = "GET";
    eligible.deadline_allows_retry = true;
    eligible.same_active_generation_worker_available = true;
    require(decide_failed_request_action(under_budget, eligible) ==
                FailedRequestAction::kRetryOnceSameGeneration,
            "eligible GET was not retried once");
    eligible.method = "HEAD";
    require(decide_failed_request_action(under_budget, eligible) ==
                FailedRequestAction::kRetryOnceSameGeneration,
            "eligible HEAD was not retried once");

    const std::array<FailedRequestRetryContext, 5> ineligible{{
        FailedRequestRetryContext{"POST", false, false, true, true, 0},
        FailedRequestRetryContext{"GET", false, true, true, true, 0},
        FailedRequestRetryContext{"GET", false, false, false, true, 0},
        FailedRequestRetryContext{"GET", false, false, true, false, 0},
        FailedRequestRetryContext{"GET", false, false, true, true, 1},
    }};
    for (const FailedRequestRetryContext& request : ineligible) {
        require(decide_failed_request_action(under_budget, request) ==
                    FailedRequestAction::kSynthesize503,
                "ineligible request was retried");
    }

    FailedRequestRetryContext started = eligible;
    started.method = "GET";
    started.response_head_sent = true;
    require(decide_failed_request_action(under_budget, started) ==
                FailedRequestAction::kAbortStartedResponse,
            "started response was replaced with a new HTTP head");

    const WorkerRecoveryDecision exceeded = record_worker_instability(
        under_budget.state,
        test_policy,
        lifecycle(),
        observation(WorkerInstabilityKind::kProtocolFailure, 11));
    require_clean(exceeded, "quarantine-triggering failure");
    require(exceeded.begin_quarantine &&
                decide_failed_request_action(exceeded, eligible) ==
                    FailedRequestAction::kSynthesize503,
            "quarantine-triggering failure retried to a residual worker");
    require(decide_failed_request_action(exceeded, started) ==
                FailedRequestAction::kAbortStartedResponse,
            "quarantine rewrote an already-started response");

    const WorkerRecoveryDecision already_quarantining =
        record_worker_instability(
            exceeded.state,
            test_policy,
            lifecycle(ServiceLifecyclePhase::kQuarantining),
            observation(WorkerInstabilityKind::kUnexpectedExit, 12));
    require_clean(already_quarantining, "failure while quarantining");
    require(already_quarantining.disposition ==
                GenerationRecoveryDisposition::kUnavailable &&
                !already_quarantining.event_counted &&
                !already_quarantining.schedule_replacement &&
                decide_failed_request_action(already_quarantining, eligible) ==
                    FailedRequestAction::kSynthesize503,
            "QUARANTINING allowed retry or replacement");

    const WorkerRecoveryDecision health = record_worker_instability(
        recovery_state(),
        policy(),
        lifecycle(),
        observation(WorkerInstabilityKind::kHealthRecycle, 20));
    require_clean(health, "health recycle");
    require(decide_failed_request_action(health, eligible) ==
                FailedRequestAction::kSynthesize503,
            "health recycle was treated as a retryable inflight failure");
}

void test_backoff_jitter_cap_and_stable_reset() {
    const WorkerRecoveryPolicy test_policy = policy(10);
    WorkerRecoveryDecision first = record_worker_instability(
        recovery_state(),
        test_policy,
        lifecycle(),
        observation(WorkerInstabilityKind::kUnexpectedExit, 0, -2000));
    require_clean(first, "negative-jitter backoff");
    require(first.replacement_delay_ms == 200,
            "negative jitter was not applied to initial backoff");

    WorkerRecoveryDecision second = record_worker_instability(
        first.state,
        test_policy,
        lifecycle(),
        observation(WorkerInstabilityKind::kCgroupOom, 1));
    require_clean(second, "second backoff");
    require(second.replacement_delay_ms == 500,
            "second scheduled replacement did not double backoff");

    GenerationRecoveryState capped_state = second.state;
    capped_state.replacement_attempts_since_stable = 20;
    WorkerRecoveryDecision capped = record_worker_instability(
        capped_state,
        test_policy,
        lifecycle(),
        observation(WorkerInstabilityKind::kSynchronousCpuTimeout,
                    2,
                    2000));
    require_clean(capped, "capped positive-jitter backoff");
    require(capped.replacement_delay_ms == 36000,
            "maximum base backoff was not capped before jitter");

    const std::vector<std::uint64_t> budget_before =
        capped.state.instability_events_ms;
    const WorkerStabilityResult early = observe_worker_stability(
        capped.state, test_policy, 2, 60001);
    require(early.ok && !early.backoff_reset &&
                early.state.replacement_attempts_since_stable == 21,
            "backoff reset before continuous stability threshold");
    const WorkerStabilityResult reset = observe_worker_stability(
        early.state, test_policy, 2, 60002);
    require(reset.ok && reset.backoff_reset &&
                reset.state.replacement_attempts_since_stable == 0 &&
                reset.state.instability_events_ms == budget_before &&
                reset.error.code == WorkerRecoveryErrorCode::kNone,
            "stable worker did not reset only the backoff state");

    const WorkerStabilityResult regressed = observe_worker_stability(
        reset.state, test_policy, 2, 60001);
    require(!regressed.ok &&
                regressed.error.code ==
                    WorkerRecoveryErrorCode::kClockRegression &&
                !regressed.backoff_reset,
            "regressed fake clock changed backoff state");

    const WorkerStabilityResult future_ready_since = observe_worker_stability(
        recovery_state(), test_policy, 2, 1);
    require(!future_ready_since.ok &&
                future_ready_since.error.code ==
                    WorkerRecoveryErrorCode::kClockRegression &&
                !future_ready_since.backoff_reset,
            "future READY timestamp was accepted");

    GenerationRecoveryState invalid_identity = recovery_state();
    invalid_identity.application = "Orders";
    const WorkerStabilityResult invalid_stability = observe_worker_stability(
        invalid_identity, test_policy, 0, 60000);
    require(!invalid_stability.ok &&
                invalid_stability.error.code ==
                    WorkerRecoveryErrorCode::kInvalidState,
            "stability reset accepted an invalid generation identity");
}

void test_invalid_or_stale_recovery_input_fails_closed() {
    WorkerInstabilityObservation stale_observation = observation(
        WorkerInstabilityKind::kUnexpectedExit, 1);
    stale_observation.worker_generation = std::string(kGenerationOne);
    const WorkerRecoveryDecision stale = record_worker_instability(
        recovery_state(), policy(), lifecycle(), stale_observation);
    require_clean(stale, "stale worker failure");
    require(stale.disposition ==
                GenerationRecoveryDisposition::kStaleGeneration &&
                !stale.event_counted && !stale.schedule_replacement,
            "stale generation consumed active budget");

    GenerationRecoveryState malformed_state = recovery_state();
    malformed_state.generation = "sha256:bad";
    require_error(record_worker_instability(
                      malformed_state,
                      policy(),
                      lifecycle(),
                      observation(WorkerInstabilityKind::kUnexpectedExit, 1)),
                  WorkerRecoveryErrorCode::kInvalidState,
                  "malformed stale recovery generation");

    WorkerInstabilityObservation malformed_worker = observation(
        WorkerInstabilityKind::kUnexpectedExit, 1);
    malformed_worker.worker_generation = "sha256:bad";
    require_error(record_worker_instability(
                      recovery_state(),
                      policy(),
                      lifecycle(),
                      malformed_worker),
                  WorkerRecoveryErrorCode::kInvalidState,
                  "malformed worker generation");

    ActiveStateDocument malformed = active_document();
    malformed.generation = "sha256:bad";
    require_error(record_worker_instability(
                      recovery_state(),
                      policy(),
                      ServiceLifecycleState{
                          ServiceLifecyclePhase::kActive,
                          std::move(malformed)},
                      observation(WorkerInstabilityKind::kUnexpectedExit, 1)),
                  WorkerRecoveryErrorCode::kInvalidState,
                  "malformed active lifecycle");

    WorkerRecoveryPolicy invalid_policy = policy();
    invalid_policy.max_events = 0;
    require_error(record_worker_instability(
                      recovery_state(),
                      invalid_policy,
                      lifecycle(),
                      observation(WorkerInstabilityKind::kUnexpectedExit, 1)),
                  WorkerRecoveryErrorCode::kInvalidPolicy,
                  "zero crash budget");

    WorkerInstabilityObservation invalid_kind = observation(
        static_cast<WorkerInstabilityKind>(99), 1);
    require_error(record_worker_instability(
                      recovery_state(), policy(), lifecycle(), invalid_kind),
                  WorkerRecoveryErrorCode::kInvalidState,
                  "unknown instability kind");

    WorkerInstabilityObservation invalid_jitter = observation(
        WorkerInstabilityKind::kUnexpectedExit, 1, 2001);
    require_error(record_worker_instability(
                      recovery_state(), policy(), lifecycle(), invalid_jitter),
                  WorkerRecoveryErrorCode::kInvalidJitter,
                  "out-of-range jitter");

    WorkerInstabilityObservation joined = observation(
        WorkerInstabilityKind::kUnexpectedExit, 1);
    joined.replacements_in_flight_for_app = 1;
    const WorkerRecoveryDecision singleflight = record_worker_instability(
        recovery_state(), policy(), lifecycle(), joined);
    require_clean(singleflight, "replacement singleflight");
    require(singleflight.event_counted &&
                singleflight.replacement_singleflight_exists &&
                !singleflight.schedule_replacement &&
                singleflight.state.replacement_attempts_since_stable == 0,
            "existing replacement did not suppress another spawn");

    GenerationRecoveryState unsorted = recovery_state();
    unsorted.instability_events_ms = {2, 1};
    require_error(record_worker_instability(
                      unsorted,
                      policy(),
                      lifecycle(),
                      observation(WorkerInstabilityKind::kUnexpectedExit, 3)),
                  WorkerRecoveryErrorCode::kInvalidState,
                  "unsorted instability state");

    GenerationRecoveryState future_event = recovery_state();
    future_event.instability_events_ms = {4};
    require_error(record_worker_instability(
                      future_event,
                      policy(),
                      lifecycle(),
                      observation(WorkerInstabilityKind::kUnexpectedExit, 3)),
                  WorkerRecoveryErrorCode::kClockRegression,
                  "future-dated instability event");

    GenerationRecoveryState oversized = recovery_state();
    oversized.instability_events_ms.assign(
        kMaxTrackedInstabilityEvents + 1, 1);
    require_error(record_worker_instability(
                      oversized,
                      policy(),
                      lifecycle(),
                      observation(WorkerInstabilityKind::kUnexpectedExit, 2)),
                  WorkerRecoveryErrorCode::kResourceLimit,
                  "oversized instability state");

    WorkerRecoveryPolicy overflow_policy = policy();
    overflow_policy.backoff_initial_ms =
        std::numeric_limits<std::uint64_t>::max();
    overflow_policy.backoff_maximum_ms =
        std::numeric_limits<std::uint64_t>::max();
    overflow_policy.jitter_basis_points = 10000;
    require_error(record_worker_instability(
                      recovery_state(),
                      overflow_policy,
                      lifecycle(),
                      observation(WorkerInstabilityKind::kUnexpectedExit, 1)),
                  WorkerRecoveryErrorCode::kInvalidPolicy,
                  "overflowing backoff policy");

    GenerationRecoveryState saturated_attempts = recovery_state();
    saturated_attempts.replacement_attempts_since_stable =
        std::numeric_limits<std::uint32_t>::max();
    const WorkerRecoveryDecision saturated = record_worker_instability(
        saturated_attempts,
        policy(),
        lifecycle(),
        observation(WorkerInstabilityKind::kUnexpectedExit, 1));
    require_clean(saturated, "saturated replacement attempt counter");
    require(saturated.schedule_replacement &&
                saturated.state.replacement_attempts_since_stable ==
                    std::numeric_limits<std::uint32_t>::max(),
            "replacement attempt counter wrapped to zero");

    WorkerRecoveryPolicy maximum_budget = policy(
        static_cast<std::uint32_t>(kMaxTrackedInstabilityEvents));
    GenerationRecoveryState full_budget = recovery_state();
    full_budget.instability_events_ms.assign(
        kMaxTrackedInstabilityEvents, 1);
    const WorkerRecoveryDecision bounded_quarantine =
        record_worker_instability(
            full_budget,
            maximum_budget,
            lifecycle(),
            observation(WorkerInstabilityKind::kUnexpectedExit, 2));
    require_clean(bounded_quarantine, "maximum tracked crash budget");
    require(bounded_quarantine.begin_quarantine &&
                bounded_quarantine.events_in_window ==
                    kMaxTrackedInstabilityEvents + 1 &&
                bounded_quarantine.state.instability_events_ms.size() <=
                    kMaxTrackedInstabilityEvents,
            "budget-exceeding result escaped the hard state bound");
}

StartupPermitRequest startup_request(std::uint64_t ticket,
                                     std::string_view application,
                                     StartupPermitLane lane,
                                     std::string_view generation =
                                         kGenerationTwo) {
    StartupPermitRequest request;
    request.ticket = ticket;
    request.application = std::string(application);
    request.generation = std::string(generation);
    request.lane = lane;
    return request;
}

void require_queue_success(const StartupPermitQueueResult& result,
                           std::string_view label) {
    require(result.ok &&
                result.error.code == WorkerRecoveryErrorCode::kNone &&
                result.error.path.empty() && result.error.message.empty(),
            std::string(label) + " failed");
}

void test_startup_permits_are_fair_by_app_and_counted_by_lane() {
    FairStartupPermitQueue queue;
    StartupPermitQueueResult enqueued = enqueue_startup_permit_request(
        queue,
        startup_request(1, "app-a", StartupPermitLane::kReplacement),
        3);
    require_queue_success(enqueued, "enqueue first replacement");
    enqueued = enqueue_startup_permit_request(
        enqueued.queue,
        startup_request(2, "app-a", StartupPermitLane::kDeploy),
        3);
    require_queue_success(enqueued, "enqueue same-App deploy");
    enqueued = enqueue_startup_permit_request(
        enqueued.queue,
        startup_request(3, "app-b", StartupPermitLane::kDeploy),
        3);
    require_queue_success(enqueued, "enqueue other-App deploy");
    require(enqueued.queued_replacements == 1 &&
                enqueued.queued_deploys == 2,
            "startup lanes were not counted separately");

    const StartupPermitGrantResult unavailable =
        grant_next_startup_permit(enqueued.queue, false);
    require(unavailable.ok && !unavailable.granted.has_value() &&
                unavailable.queue.queued.size() == 3,
            "unavailable permit consumed a request");

    StartupPermitGrantResult first =
        grant_next_startup_permit(enqueued.queue, true);
    require(first.ok && first.granted.has_value() &&
                first.granted->ticket == 1 &&
                first.granted->lane == StartupPermitLane::kReplacement,
            "first startup grant did not preserve FIFO");

    enqueued = enqueue_startup_permit_request(
        first.queue,
        startup_request(4, "app-a", StartupPermitLane::kReplacement),
        3);
    require_queue_success(enqueued, "reenqueue crash-loop App");
    StartupPermitGrantResult second =
        grant_next_startup_permit(enqueued.queue, true);
    require(second.ok && second.granted.has_value() &&
                second.granted->ticket == 3 &&
                second.granted->application == "app-b",
            "crash-loop App starved another App's deploy");
    StartupPermitGrantResult third =
        grant_next_startup_permit(second.queue, true);
    require(third.ok && third.granted.has_value() &&
                third.granted->ticket == 2,
            "per-App queue did not preserve its oldest request");
    StartupPermitGrantResult fourth =
        grant_next_startup_permit(third.queue, true);
    require(fourth.ok && fourth.granted.has_value() &&
                fourth.granted->ticket == 4 && fourth.queue.queued.empty(),
            "final replacement was not granted");

    FairStartupPermitQueue singleflight_queue;
    StartupPermitQueueResult original = enqueue_startup_permit_request(
        singleflight_queue,
        startup_request(10, "app-a", StartupPermitLane::kReplacement),
        2);
    StartupPermitQueueResult joined = enqueue_startup_permit_request(
        original.queue,
        startup_request(11, "app-a", StartupPermitLane::kReplacement),
        2);
    require_queue_success(joined, "join replacement singleflight");
    require(joined.joined_existing && joined.queue.queued.size() == 1 &&
                joined.queue.queued.front().ticket == 10 &&
                joined.queued_replacements == 1,
            "replacement singleflight created duplicate queue entries");

    const StartupPermitQueueResult duplicate_ticket =
        enqueue_startup_permit_request(
            original.queue,
            startup_request(10, "app-b", StartupPermitLane::kDeploy),
            2);
    require(!duplicate_ticket.ok &&
                duplicate_ticket.error.code ==
                    WorkerRecoveryErrorCode::kInvalidRequest &&
                duplicate_ticket.queue.queued.size() == 1,
            "duplicate startup ticket changed the queue");

    const StartupPermitQueueResult full = enqueue_startup_permit_request(
        joined.queue,
        startup_request(12, "app-b", StartupPermitLane::kDeploy),
        1);
    require(!full.ok &&
                full.error.code == WorkerRecoveryErrorCode::kQueueFull &&
                full.queue.queued.size() == 1,
            "full startup queue accepted another request");

    const StartupPermitQueueResult invalid_app =
        enqueue_startup_permit_request(
            FairStartupPermitQueue{},
            startup_request(20, "App-A", StartupPermitLane::kDeploy),
            1);
    require(!invalid_app.ok &&
                invalid_app.error.code ==
                    WorkerRecoveryErrorCode::kInvalidRequest,
            "invalid App ID entered the startup queue");

    FairStartupPermitQueue malformed_queue;
    malformed_queue.queued.push_back(startup_request(
        30,
        "app-a",
        static_cast<StartupPermitLane>(99)));
    const StartupPermitGrantResult malformed_grant =
        grant_next_startup_permit(malformed_queue, true);
    require(!malformed_grant.ok && !malformed_grant.granted.has_value() &&
                malformed_grant.error.code ==
                    WorkerRecoveryErrorCode::kInvalidState &&
                malformed_grant.queue.queued.size() == 1,
            "malformed existing queue granted a permit");
    const StartupPermitQueueResult malformed_enqueue =
        enqueue_startup_permit_request(
            malformed_queue,
            startup_request(31, "app-b", StartupPermitLane::kDeploy),
            2);
    require(!malformed_enqueue.ok &&
                malformed_enqueue.error.code ==
                    WorkerRecoveryErrorCode::kInvalidState &&
                malformed_enqueue.queue.queued.size() == 1,
            "enqueue accepted malformed existing queue state");

    FairStartupPermitQueue malformed_fairness_state;
    malformed_fairness_state.last_granted_application = "App-A";
    malformed_fairness_state.queued.push_back(startup_request(
        40, "app-b", StartupPermitLane::kDeploy));
    const StartupPermitGrantResult malformed_fairness_grant =
        grant_next_startup_permit(malformed_fairness_state, true);
    require(!malformed_fairness_grant.ok &&
                !malformed_fairness_grant.granted.has_value() &&
                malformed_fairness_grant.error.code ==
                    WorkerRecoveryErrorCode::kInvalidState &&
                malformed_fairness_grant.queue.queued.size() == 1 &&
                malformed_fairness_grant.queue.last_granted_application ==
                    "App-A",
            "malformed last-granted App changed permit fairness state");
}

// M2 item 5b: the coordinator wires the pure FairStartupPermitQueue into
// the two startup paths. The queue decides ORDER and fairness; the
// coordinator decides timing: an idle queue grants the first request
// immediately, a release hands the grant to the next waiter, the queue
// limit rejects fail-closed, and the stop signal interrupts a wait and
// withdraws the request so shutdown never leaves a stale holder.

void test_startup_permit_coordinator_grants_idle_request_immediately() {
    std::atomic<bool> stop{false};
    StartupPermitCoordinator coordinator(&stop, 8);
    StartupPermitRequest request;
    request.application = "app-a";
    request.generation = std::string(kGenerationTwo);
    request.lane = StartupPermitLane::kDeploy;
    require(coordinator.enqueue_and_wait(request),
            "idle queue did not grant the first request");
    coordinator.release_grant();
}

void test_startup_permit_coordinator_hands_grant_to_next_waiter() {
    std::atomic<bool> stop{false};
    StartupPermitCoordinator coordinator(&stop, 8);
    StartupPermitRequest first;
    first.application = "app-a";
    first.generation = std::string(kGenerationTwo);
    first.lane = StartupPermitLane::kDeploy;
    require(coordinator.enqueue_and_wait(first),
            "first App did not get the idle permit");
    std::atomic<bool> waiting_started{false};
    std::atomic<bool> waiting_granted{false};
    std::thread waiting([&] {
        waiting_started.store(true);
        StartupPermitRequest second;
        second.application = "app-b";
        second.generation = std::string(kGenerationTwo);
        second.lane = StartupPermitLane::kDeploy;
        waiting_granted.store(coordinator.enqueue_and_wait(second));
    });
    while (!waiting_started.load()) {
        std::this_thread::yield();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    require(!waiting_granted.load(),
            "waiting App was granted before the holder released");
    coordinator.release_grant();
    waiting.join();
    require(waiting_granted.load(),
            "waiting App was not granted after the release");
    coordinator.release_grant();
}

void test_startup_permit_coordinator_queue_limit_rejects_fail_closed() {
    std::atomic<bool> stop{false};
    StartupPermitCoordinator coordinator(&stop, 1);
    StartupPermitRequest first;
    first.application = "app-a";
    first.generation = std::string(kGenerationTwo);
    first.lane = StartupPermitLane::kDeploy;
    require(coordinator.enqueue_and_wait(first),
            "first startup did not hold the idle permit");
    std::atomic<bool> waiting_started{false};
    std::atomic<bool> waiting_granted{false};
    std::thread waiting([&] {
        waiting_started.store(true);
        StartupPermitRequest second;
        second.application = "app-b";
        second.generation = std::string(kGenerationTwo);
        second.lane = StartupPermitLane::kDeploy;
        waiting_granted.store(coordinator.enqueue_and_wait(second));
    });
    while (!waiting_started.load()) {
        std::this_thread::yield();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    StartupPermitRequest third;
    third.application = "app-c";
    third.generation = std::string(kGenerationTwo);
    third.lane = StartupPermitLane::kDeploy;
    require(!coordinator.enqueue_and_wait(third),
            "queue limit did not reject the overflow startup");
    coordinator.release_grant();
    waiting.join();
    require(waiting_granted.load(),
            "queued startup was not granted after the release");
    coordinator.release_grant();
}

void test_startup_permit_coordinator_replacement_singleflight_rejects() {
    std::atomic<bool> stop{false};
    StartupPermitCoordinator coordinator(&stop, 8);
    StartupPermitRequest first;
    first.application = "app-a";
    first.generation = std::string(kGenerationTwo);
    first.lane = StartupPermitLane::kDeploy;
    require(coordinator.enqueue_and_wait(first),
            "first startup did not hold the idle permit");
    std::atomic<bool> waiting_started{false};
    std::atomic<bool> waiting_granted{false};
    std::thread waiting([&] {
        waiting_started.store(true);
        StartupPermitRequest second;
        second.application = "app-a";
        second.generation = std::string(kGenerationTwo);
        second.lane = StartupPermitLane::kReplacement;
        waiting_granted.store(coordinator.enqueue_and_wait(second));
    });
    while (!waiting_started.load()) {
        std::this_thread::yield();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    // An exact (App, generation) replacement is already queued: the second
    // request joins it, adds no queue entry and is rejected.
    StartupPermitRequest duplicate;
    duplicate.application = "app-a";
    duplicate.generation = std::string(kGenerationTwo);
    duplicate.lane = StartupPermitLane::kReplacement;
    require(!coordinator.enqueue_and_wait(duplicate),
            "duplicate replacement joined the queue instead of rejecting");
    coordinator.release_grant();
    waiting.join();
    require(waiting_granted.load(),
            "queued replacement was not granted after the release");
    coordinator.release_grant();
}

void test_startup_permit_coordinator_stop_interrupts_and_withdraws() {
    std::atomic<bool> stop{false};
    StartupPermitCoordinator coordinator(&stop, 8);
    StartupPermitRequest first;
    first.application = "app-a";
    first.generation = std::string(kGenerationTwo);
    first.lane = StartupPermitLane::kDeploy;
    require(coordinator.enqueue_and_wait(first),
            "first startup did not hold the idle permit");
    std::atomic<bool> waiting_started{false};
    std::atomic<bool> waiting_done{false};
    std::atomic<bool> waiting_result{true};
    std::thread waiting([&] {
        waiting_started.store(true);
        StartupPermitRequest second;
        second.application = "app-b";
        second.generation = std::string(kGenerationTwo);
        second.lane = StartupPermitLane::kDeploy;
        waiting_result.store(coordinator.enqueue_and_wait(second));
        waiting_done.store(true);
    });
    while (!waiting_started.load()) {
        std::this_thread::yield();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    stop.store(true);
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(5);
    while (!waiting_done.load() &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    require(waiting_done.load(),
            "stop did not interrupt the queued wait in bounded time");
    require(!waiting_result.load(),
            "interrupted wait reported a granted permit");
    waiting.join();
    // The withdrawn request must not occupy the queue: the next request is
    // granted immediately.
    coordinator.release_grant();
    StartupPermitRequest next;
    next.application = "app-c";
    next.generation = std::string(kGenerationTwo);
    next.lane = StartupPermitLane::kDeploy;
    require(coordinator.enqueue_and_wait(next),
            "withdrawn request still occupied the queue");
    coordinator.release_grant();
}

}  // namespace

int main() {
    test_replacement_requires_a_deficit_and_old_generation_is_stale();
    test_rolling_budget_counts_only_instability_events();
    test_quarantine_is_decided_before_request_retry();
    test_backoff_jitter_cap_and_stable_reset();
    test_invalid_or_stale_recovery_input_fails_closed();
    test_startup_permits_are_fair_by_app_and_counted_by_lane();
    test_startup_permit_coordinator_grants_idle_request_immediately();
    test_startup_permit_coordinator_hands_grant_to_next_waiter();
    test_startup_permit_coordinator_queue_limit_rejects_fail_closed();
    test_startup_permit_coordinator_replacement_singleflight_rejects();
    test_startup_permit_coordinator_stop_interrupts_and_withdraws();
    return 0;
}
