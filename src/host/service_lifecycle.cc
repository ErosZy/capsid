// Service lifecycle control plane: the pure state machine for active,
// retiring/retired and quarantining/quarantined services.
//
// This slice never writes active.json, edits a Registry or starts/drains a
// worker; effects are commands for the later control-plane adapter. Every
// phase/document combination is validated first and unknown enums fail
// closed. App/Version/generation grammar is reused from
// encode_active_state_json() — it is never copied here.
//
// Crash counters, backoff and permit fairness are intentionally out of
// scope; they land in the merged M0.7-M0.9 work.

#include "host/service_lifecycle.h"

#include <string>
#include <string_view>

namespace capsid::host {
namespace {

using ErrorCode = ServiceLifecycleErrorCode;

void set_error(ServiceLifecycleError &error,
               ErrorCode code,
               std::string path,
               std::string message) {
    error.code = code;
    error.path = std::move(path);
    error.message = std::move(message);
}

// Whether a document is present and consistent with its phase. Every
// present document must also pass the full active-state contract (reusing
// encode_active_state_json) so a malformed identifier can never route,
// replace or start a pool. Malformed combinations fail closed everywhere.
bool document_matches_phase(ServiceLifecyclePhase phase,
                            const std::optional<ActiveStateDocument> &document) {
    if (!document.has_value()) {
        return phase == ServiceLifecyclePhase::kAbsent ||
               phase == ServiceLifecyclePhase::kFailedClosed;
    }
    const ActiveStateDocumentResult validated =
        encode_active_state_json(*document);
    if (!validated.ok) {
        return false;
    }
    switch (phase) {
    case ServiceLifecyclePhase::kAbsent:
        return false;
    case ServiceLifecyclePhase::kActive:
        return document->state == ActiveServiceState::kActive;
    case ServiceLifecyclePhase::kRetired:
        return document->state == ActiveServiceState::kRetired;
    case ServiceLifecyclePhase::kQuarantined:
        return document->state == ActiveServiceState::kQuarantined;
    // A pending retire retains its source active/quarantined document until
    // persistence resolves; after a committed tombstone it retains the
    // retired document while draining.
    case ServiceLifecyclePhase::kRetiring:
        return document->state == ActiveServiceState::kActive ||
               document->state == ActiveServiceState::kRetired ||
               document->state == ActiveServiceState::kQuarantined;
    // A pending quarantine retains its source active document.
    case ServiceLifecyclePhase::kQuarantining:
        return document->state == ActiveServiceState::kActive;
    case ServiceLifecyclePhase::kDurabilityUncertain:
    case ServiceLifecyclePhase::kFailedClosed:
        return true;
    }
    return false;
}

ActiveStateDocument retired_tombstone(const ActiveStateDocument &source) {
    ActiveStateDocument document;
    document.state = ActiveServiceState::kRetired;
    document.application = source.application;
    document.previous_version = source.version;
    document.previous_generation = source.generation;
    return document;
}

ActiveStateDocument quarantined_document(const ActiveStateDocument &source) {
    ActiveStateDocument document;
    document.state = ActiveServiceState::kQuarantined;
    document.application = source.application;
    document.version = source.version;
    document.generation = source.generation;
    document.reason = std::string(kCrashBudgetExceededReason);
    return document;
}

}  // namespace

ServiceRouteDisposition service_route_disposition(
    const ServiceLifecycleState &state) {
    if (!document_matches_phase(state.phase, state.document)) {
        return ServiceRouteDisposition::kUnavailable;
    }
    switch (state.phase) {
    case ServiceLifecyclePhase::kAbsent:
        return ServiceRouteDisposition::kNotFound;
    case ServiceLifecyclePhase::kActive:
        return ServiceRouteDisposition::kServeActive;
    case ServiceLifecyclePhase::kRetired:
        return ServiceRouteDisposition::kNotFound;
    case ServiceLifecyclePhase::kRetiring:
        // A committed retired tombstone removes routing before drain; a
        // pending retire still stops new requests pre-commit.
        return state.document->state == ActiveServiceState::kRetired
                   ? ServiceRouteDisposition::kNotFound
                   : ServiceRouteDisposition::kUnavailable;
    case ServiceLifecyclePhase::kQuarantining:
    case ServiceLifecyclePhase::kQuarantined:
    case ServiceLifecyclePhase::kDurabilityUncertain:
    case ServiceLifecyclePhase::kFailedClosed:
        return ServiceRouteDisposition::kUnavailable;
    }
    return ServiceRouteDisposition::kUnavailable;
}

bool service_allows_automatic_replacement(const ServiceLifecycleState &state) {
    // Only a complete, routable active state may be replaced automatically.
    return document_matches_phase(state.phase, state.document) &&
           state.phase == ServiceLifecyclePhase::kActive;
}

ServiceRecoveryPlan plan_service_recovery(
    const ActiveStateRecoveryResult &recovery) {
    ServiceRecoveryPlan plan;
    // The stale-temp cleanup warning is copied at the entry and propagates
    // on every return path: a stale temp file is administrative debt whether
    // recovery succeeds or fails.
    plan.stale_temp_cleanup_warning = recovery.stale_temp_cleanup_failed;
    if (!recovery.ok) {
        plan.error.code = ErrorCode::kInvalidRecovery;
        plan.error.path = "/recovery";
        plan.error.message = "active state recovery failed";
        plan.state.phase = ServiceLifecyclePhase::kFailedClosed;
        return plan;
    }
    // A successful recovery must be completely clean: any error fields
    // contradict the ok flag and fail closed as kInvalidRecovery. A
    // contradictory success must never start a pool.
    if (recovery.error.code != ActiveStateErrorCode::kNone ||
        !recovery.error.path.empty() || !recovery.error.message.empty()) {
        plan.error.code = ErrorCode::kInvalidRecovery;
        plan.error.path = "/recovery";
        plan.error.message = "recovery success carries a contradictory error";
        plan.state.phase = ServiceLifecyclePhase::kFailedClosed;
        return plan;
    }
    switch (recovery.action) {
    case ActiveStateRecoveryAction::kNone: {
        // A kNone action must carry a fully default document; hidden fields
        // must not be silently dropped.
        const ActiveStateDocument &document = recovery.document;
        if (document.state != ActiveServiceState::kActive ||
            !document.application.empty() || !document.version.empty() ||
            !document.generation.empty() ||
            !document.previous_version.empty() ||
            !document.previous_generation.empty() ||
            !document.reason.empty()) {
            plan.error.code = ErrorCode::kInvalidRecovery;
            plan.error.path = "/recovery";
            plan.error.message = "recovery action and document mismatch";
            plan.state.phase = ServiceLifecyclePhase::kFailedClosed;
            return plan;
        }
        plan.ok = true;
        plan.state.phase = ServiceLifecyclePhase::kAbsent;
        return plan;
    }
    case ActiveStateRecoveryAction::kActivate:
        if (recovery.document.state != ActiveServiceState::kActive ||
            !encode_active_state_json(recovery.document).ok) {
            plan.error.code = ErrorCode::kInvalidRecovery;
            plan.error.path = "/recovery";
            plan.error.message = "recovery action and document mismatch";
            plan.state.phase = ServiceLifecyclePhase::kFailedClosed;
            return plan;
        }
        plan.ok = true;
        plan.state.phase = ServiceLifecyclePhase::kActive;
        plan.state.document = recovery.document;
        plan.start_pool = true;
        plan.publish_active_after_ready = true;
        return plan;
    case ActiveStateRecoveryAction::kKeepRetired:
        if (recovery.document.state != ActiveServiceState::kRetired ||
            !encode_active_state_json(recovery.document).ok) {
            plan.error.code = ErrorCode::kInvalidRecovery;
            plan.error.path = "/recovery";
            plan.error.message = "recovery action and document mismatch";
            plan.state.phase = ServiceLifecyclePhase::kFailedClosed;
            return plan;
        }
        plan.ok = true;
        plan.state.phase = ServiceLifecyclePhase::kRetired;
        plan.state.document = recovery.document;
        return plan;
    case ActiveStateRecoveryAction::kKeepQuarantined:
        if (recovery.document.state != ActiveServiceState::kQuarantined ||
            !encode_active_state_json(recovery.document).ok) {
            plan.error.code = ErrorCode::kInvalidRecovery;
            plan.error.path = "/recovery";
            plan.error.message = "recovery action and document mismatch";
            plan.state.phase = ServiceLifecyclePhase::kFailedClosed;
            return plan;
        }
        plan.ok = true;
        plan.state.phase = ServiceLifecyclePhase::kQuarantined;
        plan.state.document = recovery.document;
        return plan;
    }
    plan.error.code = ErrorCode::kInvalidRecovery;
    plan.error.path = "/recovery";
    plan.error.message = "unknown recovery action";
    plan.state.phase = ServiceLifecyclePhase::kFailedClosed;
    return plan;
}

ServiceLifecycleTransition request_service_retire(
    const ServiceLifecycleState &state) {
    ServiceLifecycleTransition transition;
    if (!document_matches_phase(state.phase, state.document)) {
        set_error(transition.error, ErrorCode::kInvalidState, "/state",
                  "lifecycle phase and document are inconsistent");
        return transition;
    }
    switch (state.phase) {
    case ServiceLifecyclePhase::kRetired:
        transition.ok = true;
        transition.state = state;
        transition.effects.idempotent = true;
        return transition;
    case ServiceLifecyclePhase::kAbsent:
        set_error(transition.error, ErrorCode::kNotFound, "/state",
                  "cannot retire an absent App");
        return transition;
    case ServiceLifecyclePhase::kActive:
    case ServiceLifecyclePhase::kQuarantined: {
        transition.state.phase = ServiceLifecyclePhase::kRetiring;
        transition.state.document = state.document;
        transition.effects.persist_required = true;
        transition.effects.stop_new_requests = true;
        transition.effects.persist_document =
            retired_tombstone(*state.document);
        transition.ok = true;
        return transition;
    }
    case ServiceLifecyclePhase::kRetiring:
    case ServiceLifecyclePhase::kQuarantining:
        set_error(transition.error, ErrorCode::kBusy, "/state",
                  "a lifecycle mutation is already in progress");
        return transition;
    case ServiceLifecyclePhase::kDurabilityUncertain:
        set_error(transition.error, ErrorCode::kDurabilityUncertain, "/state",
                  "state writes are blocked by a durability incident");
        return transition;
    case ServiceLifecyclePhase::kFailedClosed:
        set_error(transition.error, ErrorCode::kInvalidState, "/state",
                  "cannot mutate a fail-closed service");
        return transition;
    }
    set_error(transition.error, ErrorCode::kInvalidState, "/state",
              "unknown lifecycle phase");
    return transition;
}

ServiceLifecycleTransition enter_service_quarantine(
    const ServiceLifecycleState &state) {
    ServiceLifecycleTransition transition;
    if (state.phase != ServiceLifecyclePhase::kActive ||
        !document_matches_phase(state.phase, state.document)) {
        set_error(transition.error, ErrorCode::kInvalidState, "/state",
                  "quarantine requires a complete active state");
        return transition;
    }
    transition.state.phase = ServiceLifecyclePhase::kQuarantining;
    transition.state.document = state.document;
    transition.effects.stop_new_requests = true;
    transition.effects.persist_required = true;
    transition.effects.persist_document = quarantined_document(*state.document);
    transition.ok = true;
    return transition;
}

ServiceLifecycleTransition resolve_service_state_persistence(
    const ServiceLifecycleState &state,
    const ActiveStatePersistResult &persistence) {
    ServiceLifecycleTransition transition;
    // Only a pending mutation may be resolved, and its source document must
    // pass the full active-state contract first (document_matches_phase runs
    // encode_active_state_json) so a malformed identifier can never be
    // committed through a persist result.
    if ((state.phase != ServiceLifecyclePhase::kRetiring &&
         state.phase != ServiceLifecyclePhase::kQuarantining) ||
        !document_matches_phase(state.phase, state.document)) {
        set_error(transition.error, ErrorCode::kInvalidState, "/state",
                  "persistence result without a pending mutation");
        return transition;
    }
    // The document is present and fully valid; now restrict the pending
    // source type. A committed retired tombstone (kRetiring with a retired
    // document) can never be resolved again.
    if (state.document->state == ActiveServiceState::kRetired ||
        (state.phase == ServiceLifecyclePhase::kQuarantining &&
         state.document->state != ActiveServiceState::kActive)) {
        set_error(transition.error, ErrorCode::kInvalidState, "/state",
                  "persistence result without a pending mutation");
        return transition;
    }

    // Legal persist results only:
    //   success:            ok && replaced && clean error
    //   pre-rename failure: !ok && !replaced && ordinary error
    //   durability unknown: !ok && replaced && kDurabilityUncertain
    // Every other combination fails closed into the durability incident.
    const bool clean_success =
        persistence.ok && persistence.active_name_replaced &&
        persistence.error.code == ActiveStateErrorCode::kNone &&
        persistence.error.path.empty() && persistence.error.message.empty();
    const bool pre_rename_failure =
        !persistence.ok && !persistence.active_name_replaced &&
        persistence.error.code != ActiveStateErrorCode::kNone &&
        persistence.error.code != ActiveStateErrorCode::kDurabilityUncertain;
    const bool durability_unknown =
        !persistence.ok && persistence.active_name_replaced &&
        persistence.error.code == ActiveStateErrorCode::kDurabilityUncertain;

    if (!clean_success && !pre_rename_failure && !durability_unknown) {
        // Self-contradictory result: the rename may or may not have landed.
        transition.state.phase = ServiceLifecyclePhase::kDurabilityUncertain;
        transition.state.document = state.document;
        transition.effects.block_state_writes = true;
        transition.effects.stop_new_requests = true;
        set_error(transition.error, ErrorCode::kInvalidPersistenceResult,
                  "/persistence",
                  "persistence result is self-contradictory");
        return transition;
    }

    if (clean_success) {
        if (state.phase == ServiceLifecyclePhase::kRetiring) {
            transition.state.phase = ServiceLifecyclePhase::kRetiring;
            transition.state.document = retired_tombstone(*state.document);
            transition.effects.begin_drain = true;
        } else {
            transition.state.phase = ServiceLifecyclePhase::kQuarantined;
            transition.state.document = quarantined_document(*state.document);
            transition.effects.begin_drain = true;
        }
        transition.ok = true;
        return transition;
    }

    if (durability_unknown) {
        transition.state.phase = ServiceLifecyclePhase::kDurabilityUncertain;
        transition.state.document = state.document;
        transition.effects.block_state_writes = true;
        transition.effects.stop_new_requests = true;
        set_error(transition.error, ErrorCode::kDurabilityUncertain,
                  "/persistence",
                  "active.json may have been replaced without durable sync");
        return transition;
    }

    // Pre-rename failure.
    if (state.phase == ServiceLifecyclePhase::kRetiring) {
        // Restore the source service exactly as it was before retire.
        const ActiveStateDocument &source = *state.document;
        transition.state.phase =
            source.state == ActiveServiceState::kQuarantined
                ? ServiceLifecyclePhase::kQuarantined
                : ServiceLifecyclePhase::kActive;
        transition.state.document = state.document;
    } else {
        // Quarantine stays fail-closed and retries persistence; it never
        // restores crash-loop traffic.
        transition.state.phase = ServiceLifecyclePhase::kQuarantining;
        transition.state.document = state.document;
        transition.effects.retry_persistence = true;
        transition.effects.persist_required = true;
        transition.effects.stop_new_requests = true;
        transition.effects.persist_document =
            quarantined_document(*state.document);
    }
    set_error(transition.error, ErrorCode::kPersistenceFailed, "/persistence",
              "active state persistence failed before the rename");
    return transition;
}

ServiceLifecycleTransition complete_service_retire_drain(
    const ServiceLifecycleState &state) {
    ServiceLifecycleTransition transition;
    if (state.phase == ServiceLifecyclePhase::kRetired &&
        document_matches_phase(state.phase, state.document)) {
        transition.ok = true;
        transition.state = state;
        transition.effects.idempotent = true;
        return transition;
    }
    // A committed retired tombstone must pass the full active-state contract
    // before drain completes — the retired state enum alone is not enough
    // (document_matches_phase runs encode_active_state_json).
    if (state.phase != ServiceLifecyclePhase::kRetiring ||
        !document_matches_phase(state.phase, state.document) ||
        state.document->state != ActiveServiceState::kRetired) {
        set_error(transition.error, ErrorCode::kInvalidState, "/state",
                  "retire drain requires a committed retired tombstone");
        return transition;
    }
    transition.state.phase = ServiceLifecyclePhase::kRetired;
    transition.state.document = state.document;
    transition.ok = true;
    return transition;
}

ExplicitDeployPlan plan_explicit_service_deploy(
    const ServiceLifecycleState &state,
    std::string_view application,
    std::string_view version,
    std::string_view generation) {
    ExplicitDeployPlan plan;

    // The current lifecycle state is validated first; a malformed phase or
    // document can never be deployed onto.
    if (!document_matches_phase(state.phase, state.document)) {
        set_error(plan.error, ErrorCode::kInvalidState, "/state",
                  "lifecycle phase and document are inconsistent");
        plan.action = ExplicitDeployAction::kRejectInvalidState;
        return plan;
    }

    // The deploy target is validated with the same App/Version/generation
    // contract as active.json by reusing the active-state encoder (the
    // grammar lives in exactly one place).
    ActiveStateDocument probe;
    probe.state = ActiveServiceState::kActive;
    probe.application = std::string(application);
    probe.version = std::string(version);
    probe.generation = std::string(generation);
    const ActiveStateDocumentResult validated = encode_active_state_json(probe);
    if (!validated.ok) {
        // The encoder's ActiveStateError carries the precise /app, /version
        // or /generation pointer; map it onto the deploy-target contract.
        plan.error.code = ErrorCode::kInvalidDeployTarget;
        plan.error.path = validated.error.path;
        plan.error.message = validated.error.message;
        plan.action = ExplicitDeployAction::kRejectInvalidState;
        return plan;
    }
    if (state.document.has_value() &&
        state.document->application != application) {
        set_error(plan.error, ErrorCode::kInvalidDeployTarget, "/app",
                  "deploy application differs from the service application");
        plan.action = ExplicitDeployAction::kRejectInvalidState;
        return plan;
    }

    switch (state.phase) {
    case ServiceLifecyclePhase::kAbsent:
        plan.ok = true;
        plan.action = ExplicitDeployAction::kWarmAndActivate;
        return plan;
    case ServiceLifecyclePhase::kActive:
        if (state.document->version == version &&
            state.document->generation == generation) {
            plan.ok = true;
            plan.action = ExplicitDeployAction::kAlreadyActive;
            return plan;
        }
        if (state.document->version == version) {
            // The same Version ID cannot be remapped to new content.
            set_error(plan.error, ErrorCode::kVersionImmutabilityConflict,
                      "/version",
                      "Version ID is already mapped to another generation");
            plan.action = ExplicitDeployAction::kVersionImmutabilityConflict;
            return plan;
        }
        plan.ok = true;
        plan.action = ExplicitDeployAction::kWarmAndActivate;
        return plan;
    case ServiceLifecyclePhase::kRetired:
        // Retired services must warm and activate even for the exact
        // previous generation, but the Version mapping stays immutable.
        if (state.document->previous_version == version &&
            state.document->previous_generation != generation) {
            set_error(plan.error, ErrorCode::kVersionImmutabilityConflict,
                      "/version",
                      "Version ID is already mapped to another generation");
            plan.action = ExplicitDeployAction::kVersionImmutabilityConflict;
            return plan;
        }
        plan.ok = true;
        plan.action = ExplicitDeployAction::kWarmAndActivate;
        return plan;
    case ServiceLifecyclePhase::kQuarantined:
        // Same-generation redeploy must still prewarm; the instability
        // budget resets only after successful activation.
        if (state.document->version == version &&
            state.document->generation != generation) {
            set_error(plan.error, ErrorCode::kVersionImmutabilityConflict,
                      "/version",
                      "Version ID is already mapped to another generation");
            plan.action = ExplicitDeployAction::kVersionImmutabilityConflict;
            return plan;
        }
        plan.ok = true;
        plan.action = ExplicitDeployAction::kWarmAndActivate;
        plan.reset_instability_budget_after_activation = true;
        return plan;
    case ServiceLifecyclePhase::kRetiring:
    case ServiceLifecyclePhase::kQuarantining:
        set_error(plan.error, ErrorCode::kBusy, "/state",
                  "a lifecycle mutation is already in progress");
        plan.action = ExplicitDeployAction::kRejectBusy;
        return plan;
    case ServiceLifecyclePhase::kDurabilityUncertain:
        set_error(plan.error, ErrorCode::kDurabilityUncertain, "/state",
                  "deploy is blocked by a durability incident");
        plan.action = ExplicitDeployAction::kRejectDurabilityUncertain;
        return plan;
    case ServiceLifecyclePhase::kFailedClosed:
        set_error(plan.error, ErrorCode::kInvalidState, "/state",
                  "cannot deploy a fail-closed service");
        plan.action = ExplicitDeployAction::kRejectInvalidState;
        return plan;
    }
    set_error(plan.error, ErrorCode::kInvalidState, "/state",
              "unknown lifecycle phase");
    plan.action = ExplicitDeployAction::kRejectInvalidState;
    return plan;
}

}  // namespace capsid::host
