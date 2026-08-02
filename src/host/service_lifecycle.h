#ifndef CAPSID_HOST_SERVICE_LIFECYCLE_H
#define CAPSID_HOST_SERVICE_LIFECYCLE_H

#include "host/active_state.h"

#include <optional>
#include <string>
#include <string_view>

namespace capsid::host {

// In-memory control-plane state. A pending retire retains its source active
// or quarantined document until persistence resolves; after a committed
// retired tombstone it retains the retired document until drain completes.
// A pending quarantine retains its source active document.
enum class ServiceLifecyclePhase {
    kAbsent,
    kActive,
    kRetiring,
    kRetired,
    kQuarantining,
    kQuarantined,
    kDurabilityUncertain,
    kFailedClosed,
};

struct ServiceLifecycleState {
    ServiceLifecyclePhase phase = ServiceLifecyclePhase::kAbsent;
    std::optional<ActiveStateDocument> document;
};

enum class ServiceRouteDisposition {
    kServeActive,
    kNotFound,
    kUnavailable,
};

enum class ServiceLifecycleErrorCode {
    kNone,
    kInvalidState,
    kInvalidRecovery,
    kNotFound,
    kBusy,
    kPersistenceFailed,
    kDurabilityUncertain,
    kInvalidPersistenceResult,
    kInvalidDeployTarget,
    kVersionImmutabilityConflict,
};

struct ServiceLifecycleError {
    ServiceLifecycleErrorCode code = ServiceLifecycleErrorCode::kNone;
    std::string path;
    std::string message;
};

// Effects are commands for the later control-plane adapter. This pure slice
// never writes active.json, edits a Registry, or starts/drains a worker.
struct ServiceLifecycleEffects {
    bool persist_required = false;
    ActiveStateDocument persist_document;
    bool stop_new_requests = false;
    bool begin_drain = false;
    bool retry_persistence = false;
    bool block_state_writes = false;
    bool idempotent = false;
};

struct ServiceLifecycleTransition {
    // false means the requested operation failed or was rejected. state and
    // effects still describe the mandatory rollback/fail-closed response to
    // an already-started persistence operation.
    bool ok = false;
    ServiceLifecycleState state;
    ServiceLifecycleEffects effects;
    ServiceLifecycleError error;
};

// Invalid/malformed states always derive fail-closed behavior.
ServiceRouteDisposition service_route_disposition(
    const ServiceLifecycleState& state);
bool service_allows_automatic_replacement(
    const ServiceLifecycleState& state);

struct ServiceRecoveryPlan {
    bool ok = false;
    ServiceLifecycleState state;
    // Only an M0.4 kActivate result may start a pool. It is not published to
    // routing until that recovered pool is READY/healthy.
    bool start_pool = false;
    bool publish_active_after_ready = false;
    bool stale_temp_cleanup_warning = false;
    ServiceLifecycleError error;
};

// Translates the strict M0.4 recovery result. Retired/quarantined/absent and
// every malformed/error result start no worker and publish no active route.
ServiceRecoveryPlan plan_service_recovery(
    const ActiveStateRecoveryResult& recovery);

// Starts explicit retire. Active and quarantined states first become
// kRetiring and stop new requests, then request a retired tombstone write.
// Retired is an idempotent success; absent is not found; transient and
// durability-uncertain states reject the operation.
ServiceLifecycleTransition request_service_retire(
    const ServiceLifecycleState& state);

// Starts crash-budget quarantine from an active generation. Routing and
// automatic replacement stop before the quarantined state is persisted.
ServiceLifecycleTransition enter_service_quarantine(
    const ServiceLifecycleState& state);

// Resolves persist_active_state() for kRetiring/kQuarantining:
// - committed retire publishes the retired tombstone and begins drain;
// - pre-rename retire failure restores its active/quarantined source;
// - pre-rename quarantine failure stays fail-closed and requests retry;
// - rename followed by uncertain durability enters kDurabilityUncertain and
//   blocks later state writes.
// Inconsistent persist results also fail closed as kInvalidPersistenceResult.
ServiceLifecycleTransition resolve_service_state_persistence(
    const ServiceLifecycleState& state,
    const ActiveStatePersistResult& persistence);

// A committed retired tombstone remains kRetiring while its old pool drains.
// Completion moves it to kRetired. Repeated completion is idempotent.
ServiceLifecycleTransition complete_service_retire_drain(
    const ServiceLifecycleState& state);

enum class ExplicitDeployAction {
    kAlreadyActive,
    kWarmAndActivate,
    kRejectBusy,
    kRejectDurabilityUncertain,
    kRejectInvalidState,
    kVersionImmutabilityConflict,
};

struct ExplicitDeployPlan {
    bool ok = false;
    ExplicitDeployAction action = ExplicitDeployAction::kRejectInvalidState;
    // This is a post-activation effect. Planning or warming must not clear a
    // quarantined generation's instability budget early.
    bool reset_instability_budget_after_activation = false;
    ServiceLifecycleError error;
};

// Inputs are validated with the same App/Version/generation contract as
// active.json. An active exact generation is idempotent. Retired and
// quarantined states, including the exact previous/current generation, must
// warm and atomically activate rather than taking an idempotent shortcut.
ExplicitDeployPlan plan_explicit_service_deploy(
    const ServiceLifecycleState& state,
    std::string_view application,
    std::string_view version,
    std::string_view generation);

}  // namespace capsid::host

#endif
