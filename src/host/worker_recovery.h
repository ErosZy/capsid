#ifndef CAPSID_HOST_WORKER_RECOVERY_H
#define CAPSID_HOST_WORKER_RECOVERY_H

#include "host/service_lifecycle.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace capsid::host {

// M0 keeps the rolling window bounded independently of config input. A
// max_events policy above this limit is invalid.
inline constexpr std::size_t kMaxTrackedInstabilityEvents = 1024U;

struct WorkerRecoveryPolicy {
    std::uint32_t max_events = 0;
    std::uint64_t window_ms = 0;
    std::uint64_t backoff_initial_ms = 0;
    std::uint64_t backoff_maximum_ms = 0;
    std::uint32_t jitter_basis_points = 0;
    std::uint64_t stable_reset_ms = 0;
    std::uint32_t replacements_concurrent_per_app = 0;
};

enum class WorkerInstabilityKind {
    kUnexpectedExit,
    kCgroupOom,
    kSynchronousCpuTimeout,
    kIpcFailure,
    kProtocolFailure,
    kHealthRecycle,
    kReplacementStartupFailure,
    kNormalDrain,
    kHostShutdown,
    kOperatorRetire,
};

enum class WorkerRecoveryErrorCode {
    kNone,
    kInvalidPolicy,
    kInvalidState,
    kClockRegression,
    kResourceLimit,
    kInvalidJitter,
    kInvalidRequest,
    kQueueFull,
};

struct WorkerRecoveryError {
    WorkerRecoveryErrorCode code = WorkerRecoveryErrorCode::kNone;
    std::string path;
    std::string message;
};

// Timestamps are monotonic milliseconds and sorted oldest first. The rolling
// window is (now - window_ms, now]: an event whose age is exactly window_ms
// has expired. replacement_attempts_since_stable advances only when this
// controller actually schedules a replacement.
struct GenerationRecoveryState {
    std::string application;
    std::string generation;
    std::vector<std::uint64_t> instability_events_ms;
    std::uint32_t replacement_attempts_since_stable = 0;
    std::uint64_t last_observed_ms = 0;
    bool has_last_observed_time = false;
};

struct WorkerInstabilityObservation {
    WorkerInstabilityKind kind = WorkerInstabilityKind::kUnexpectedExit;
    std::string worker_generation;
    std::uint64_t now_ms = 0;
    std::uint32_t ready_workers_after_removal = 0;
    std::uint32_t target_ready_workers = 0;
    std::uint32_t replacements_in_flight_for_app = 0;
    // The caller supplies the sampled signed offset. It must be within
    // +/- policy.jitter_basis_points. The base delay is capped before jitter.
    std::int32_t chosen_jitter_basis_points = 0;
};

enum class GenerationRecoveryDisposition {
    kIgnoredExpectedEvent,
    kStaleGeneration,
    kContinueActive,
    kBeginQuarantine,
    kUnavailable,
};

struct WorkerRecoveryDecision {
    bool ok = false;
    GenerationRecoveryState state;
    WorkerInstabilityKind instability_kind =
        WorkerInstabilityKind::kUnexpectedExit;
    GenerationRecoveryDisposition disposition =
        GenerationRecoveryDisposition::kUnavailable;
    bool event_counted = false;
    std::size_t events_in_window = 0;
    bool begin_quarantine = false;
    bool schedule_replacement = false;
    bool replacement_singleflight_exists = false;
    bool acquire_startup_permit = false;
    bool acquire_memory_permit = false;
    std::uint64_t replacement_delay_ms = 0;
    WorkerRecoveryError error;
};

// Records a worker/generation instability before making any retry or
// replacement decision. Only a complete ACTIVE lifecycle document matching
// state and worker_generation can count or schedule work. Exceeding max_events
// begins quarantine and suppresses replacement in the same result.
WorkerRecoveryDecision record_worker_instability(
    const GenerationRecoveryState& state,
    const WorkerRecoveryPolicy& policy,
    const ServiceLifecycleState& lifecycle,
    const WorkerInstabilityObservation& observation);

struct WorkerStabilityResult {
    bool ok = false;
    GenerationRecoveryState state;
    bool backoff_reset = false;
    WorkerRecoveryError error;
};

// Resets replacement backoff after one replacement worker has remained
// continuously READY for stable_reset_ms. It never clears crash-budget events.
WorkerStabilityResult observe_worker_stability(
    const GenerationRecoveryState& state,
    const WorkerRecoveryPolicy& policy,
    std::uint64_t continuously_ready_since_ms,
    std::uint64_t now_ms);

struct FailedRequestRetryContext {
    std::string method;
    bool response_head_sent = false;
    bool request_body_present_or_started = false;
    bool deadline_allows_retry = false;
    bool same_active_generation_worker_available = false;
    std::uint32_t retries_already_attempted = 0;
};

enum class FailedRequestAction {
    kRetryOnceSameGeneration,
    kSynthesize503,
    kAbortStartedResponse,
};

// A result that begins quarantine can never retry. Otherwise v1 allows one
// retry only for an eligible worker/IPC/protocol failure, GET/HEAD, no body,
// no response head, sufficient deadline and a READY worker from the same
// active generation.
FailedRequestAction decide_failed_request_action(
    const WorkerRecoveryDecision& recovery,
    const FailedRequestRetryContext& request);

enum class StartupPermitLane {
    kDeploy,
    kReplacement,
};

struct StartupPermitRequest {
    std::uint64_t ticket = 0;
    std::string application;
    std::string generation;
    StartupPermitLane lane = StartupPermitLane::kDeploy;
};

// Requests remain in enqueue order. Across Apps, a grant avoids serving the
// last-granted App again whenever another App is waiting; within the selected
// App it grants the oldest request. Lanes share permits but are counted
// separately. Exact replacement App/generation requests are singleflight.
struct FairStartupPermitQueue {
    std::vector<StartupPermitRequest> queued;
    std::string last_granted_application;
};

struct StartupPermitQueueResult {
    bool ok = false;
    FairStartupPermitQueue queue;
    bool joined_existing = false;
    std::size_t queued_deploys = 0;
    std::size_t queued_replacements = 0;
    WorkerRecoveryError error;
};

StartupPermitQueueResult enqueue_startup_permit_request(
    const FairStartupPermitQueue& queue,
    const StartupPermitRequest& request,
    std::size_t maximum_queued);

struct StartupPermitGrantResult {
    bool ok = false;
    FairStartupPermitQueue queue;
    std::optional<StartupPermitRequest> granted;
    std::size_t queued_deploys = 0;
    std::size_t queued_replacements = 0;
    WorkerRecoveryError error;
};

StartupPermitGrantResult grant_next_startup_permit(
    const FairStartupPermitQueue& queue,
    bool permit_available);

}  // namespace capsid::host

#endif
