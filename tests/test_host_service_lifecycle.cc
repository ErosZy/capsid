#include "host/service_lifecycle.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using capsid::host::ActiveServiceState;
using capsid::host::ActiveStateDocument;
using capsid::host::ActiveStateErrorCode;
using capsid::host::ActiveStatePersistResult;
using capsid::host::ActiveStateRecoveryAction;
using capsid::host::ActiveStateRecoveryResult;
using capsid::host::ExplicitDeployAction;
using capsid::host::ExplicitDeployPlan;
using capsid::host::ServiceLifecycleErrorCode;
using capsid::host::ServiceLifecyclePhase;
using capsid::host::ServiceLifecycleState;
using capsid::host::ServiceLifecycleTransition;
using capsid::host::ServiceRecoveryPlan;
using capsid::host::ServiceRouteDisposition;
using capsid::host::complete_service_retire_drain;
using capsid::host::enter_service_quarantine;
using capsid::host::kCrashBudgetExceededReason;
using capsid::host::plan_explicit_service_deploy;
using capsid::host::plan_service_recovery;
using capsid::host::request_service_retire;
using capsid::host::resolve_service_state_persistence;
using capsid::host::service_allows_automatic_replacement;
using capsid::host::service_route_disposition;

constexpr std::string_view kGenerationOne =
    "sha256:1111111111111111111111111111111111111111111111111111111111111111";
constexpr std::string_view kGenerationTwo =
    "sha256:2222222222222222222222222222222222222222222222222222222222222222";

[[noreturn]] void fail(const std::string& message) {
    std::cerr << "test-host-service-lifecycle: " << message << std::endl;
    std::exit(1);
}

void require(bool condition, const std::string& message) {
    if (!condition) {
        fail(message);
    }
}

ActiveStateDocument active_document(std::string_view version = "v2",
                                    std::string_view generation =
                                        kGenerationTwo) {
    ActiveStateDocument document;
    document.state = ActiveServiceState::kActive;
    document.application = "orders";
    document.version = std::string(version);
    document.generation = std::string(generation);
    return document;
}

ActiveStateDocument retired_document(std::string_view version = "v2",
                                     std::string_view generation =
                                         kGenerationTwo) {
    ActiveStateDocument document;
    document.state = ActiveServiceState::kRetired;
    document.application = "orders";
    document.previous_version = std::string(version);
    document.previous_generation = std::string(generation);
    return document;
}

ActiveStateDocument quarantined_document(std::string_view version = "v2",
                                         std::string_view generation =
                                             kGenerationTwo) {
    ActiveStateDocument document = active_document(version, generation);
    document.state = ActiveServiceState::kQuarantined;
    document.reason = std::string(kCrashBudgetExceededReason);
    return document;
}

ServiceLifecycleState lifecycle(ServiceLifecyclePhase phase) {
    return ServiceLifecycleState{phase, std::nullopt};
}

ServiceLifecycleState lifecycle(ServiceLifecyclePhase phase,
                                ActiveStateDocument document) {
    return ServiceLifecycleState{phase, std::move(document)};
}

bool same_document(const ActiveStateDocument& left,
                   const ActiveStateDocument& right) {
    return left.state == right.state &&
           left.application == right.application &&
           left.version == right.version &&
           left.generation == right.generation &&
           left.previous_version == right.previous_version &&
           left.previous_generation == right.previous_generation &&
           left.reason == right.reason;
}

void require_clean_success(const ServiceLifecycleTransition& result,
                           std::string_view label) {
    require(result.ok, std::string(label) + " failed: " +
                           result.error.message);
    require(result.error.code == ServiceLifecycleErrorCode::kNone &&
                result.error.path.empty() && result.error.message.empty(),
            std::string(label) + " succeeded with a stale error");
}

void require_transition_error(const ServiceLifecycleTransition& result,
                              ServiceLifecycleErrorCode code,
                              std::string_view path,
                              std::string_view label) {
    require(!result.ok, std::string(label) + " succeeded");
    require(result.error.code == code && result.error.path == path &&
                !result.error.message.empty(),
            std::string(label) + " returned the wrong error");
}

ActiveStatePersistResult committed_persistence() {
    ActiveStatePersistResult result;
    result.ok = true;
    result.active_name_replaced = true;
    return result;
}

ActiveStatePersistResult pre_rename_failure() {
    ActiveStatePersistResult result;
    result.error.code = ActiveStateErrorCode::kStorageError;
    result.error.path = "/active.json";
    result.error.message = "injected storage failure";
    return result;
}

ActiveStatePersistResult durability_uncertain() {
    ActiveStatePersistResult result;
    result.active_name_replaced = true;
    result.error.code = ActiveStateErrorCode::kDurabilityUncertain;
    result.error.path = "/active.json";
    result.error.message = "injected directory sync failure";
    return result;
}

void test_retired_or_quarantined_app_never_reactivates_on_restart() {
    ActiveStateRecoveryResult absent;
    absent.ok = true;
    absent.action = ActiveStateRecoveryAction::kNone;
    absent.stale_temp_cleanup_failed = true;
    const ServiceRecoveryPlan absent_plan = plan_service_recovery(absent);
    require(absent_plan.ok &&
                absent_plan.state.phase == ServiceLifecyclePhase::kAbsent &&
                !absent_plan.state.document.has_value() &&
                !absent_plan.start_pool &&
                !absent_plan.publish_active_after_ready &&
                absent_plan.stale_temp_cleanup_warning &&
                service_route_disposition(absent_plan.state) ==
                    ServiceRouteDisposition::kNotFound,
            "absent App recovery attempted to start or publish a pool");

    ActiveStateRecoveryResult active;
    active.ok = true;
    active.action = ActiveStateRecoveryAction::kActivate;
    active.document = active_document();
    active.stale_temp_cleanup_failed = true;
    const ServiceRecoveryPlan active_plan = plan_service_recovery(active);
    require(active_plan.ok &&
                active_plan.state.phase == ServiceLifecyclePhase::kActive &&
                active_plan.start_pool &&
                active_plan.publish_active_after_ready &&
                active_plan.stale_temp_cleanup_warning,
            "active recovery did not defer publication until pool readiness");

    ActiveStateRecoveryResult retired;
    retired.ok = true;
    retired.action = ActiveStateRecoveryAction::kKeepRetired;
    retired.document = retired_document();
    retired.stale_temp_cleanup_failed = true;
    const ServiceRecoveryPlan retired_plan = plan_service_recovery(retired);
    require(retired_plan.ok &&
                retired_plan.state.phase == ServiceLifecyclePhase::kRetired &&
                !retired_plan.start_pool &&
                !retired_plan.publish_active_after_ready &&
                service_route_disposition(retired_plan.state) ==
                    ServiceRouteDisposition::kNotFound &&
                !service_allows_automatic_replacement(retired_plan.state),
            "retired tombstone reactivated on restart");
    require(retired_plan.stale_temp_cleanup_warning,
            "retired recovery dropped the stale-temp cleanup warning");

    ActiveStateRecoveryResult quarantined;
    quarantined.ok = true;
    quarantined.action = ActiveStateRecoveryAction::kKeepQuarantined;
    quarantined.document = quarantined_document();
    quarantined.stale_temp_cleanup_failed = true;
    const ServiceRecoveryPlan quarantined_plan =
        plan_service_recovery(quarantined);
    require(quarantined_plan.ok &&
                quarantined_plan.state.phase ==
                    ServiceLifecyclePhase::kQuarantined &&
                !quarantined_plan.start_pool &&
                !quarantined_plan.publish_active_after_ready &&
                quarantined_plan.stale_temp_cleanup_warning &&
                service_route_disposition(quarantined_plan.state) ==
                    ServiceRouteDisposition::kUnavailable &&
                !service_allows_automatic_replacement(
                    quarantined_plan.state),
            "quarantined tombstone restarted its crash loop");
}

void test_recovery_action_document_mismatch_fails_closed() {
    ActiveStateRecoveryResult mismatch;
    mismatch.ok = true;
    mismatch.action = ActiveStateRecoveryAction::kActivate;
    mismatch.document = retired_document();
    const ServiceRecoveryPlan mismatch_plan = plan_service_recovery(mismatch);
    require(!mismatch_plan.ok &&
                mismatch_plan.state.phase ==
                    ServiceLifecyclePhase::kFailedClosed &&
                !mismatch_plan.start_pool &&
                !mismatch_plan.publish_active_after_ready &&
                mismatch_plan.error.code ==
                    ServiceLifecycleErrorCode::kInvalidRecovery &&
                mismatch_plan.error.path == "/recovery" &&
                service_route_disposition(mismatch_plan.state) ==
                    ServiceRouteDisposition::kUnavailable,
            "mismatched recovery action was not fail-closed");

    ActiveStateRecoveryResult upstream_error;
    upstream_error.error.code = ActiveStateErrorCode::kStorageError;
    upstream_error.error.message = "read failed";
    upstream_error.stale_temp_cleanup_failed = true;
    const ServiceRecoveryPlan error_plan =
        plan_service_recovery(upstream_error);
    require(!error_plan.ok &&
                error_plan.state.phase ==
                    ServiceLifecyclePhase::kFailedClosed &&
                !error_plan.start_pool &&
                error_plan.stale_temp_cleanup_warning &&
                error_plan.error.code ==
                    ServiceLifecycleErrorCode::kInvalidRecovery,
            "failed active.json recovery did not remain fail-closed");

    ActiveStateRecoveryResult invalid_action;
    invalid_action.ok = true;
    invalid_action.action = static_cast<ActiveStateRecoveryAction>(99);
    const ServiceRecoveryPlan invalid_plan =
        plan_service_recovery(invalid_action);
    require(!invalid_plan.ok && !invalid_plan.start_pool &&
                invalid_plan.state.phase ==
                    ServiceLifecyclePhase::kFailedClosed,
            "unknown recovery action started a pool");

    ActiveStateRecoveryResult malformed_active;
    malformed_active.ok = true;
    malformed_active.action = ActiveStateRecoveryAction::kActivate;
    malformed_active.document = active_document();
    malformed_active.document.generation = "sha256:bad";
    const ServiceRecoveryPlan malformed_plan =
        plan_service_recovery(malformed_active);
    require(!malformed_plan.ok && !malformed_plan.start_pool &&
                malformed_plan.state.phase ==
                    ServiceLifecyclePhase::kFailedClosed &&
                malformed_plan.error.code ==
                    ServiceLifecycleErrorCode::kInvalidRecovery,
            "malformed active recovery document started a pool");

    ActiveStateRecoveryResult contradictory_success;
    contradictory_success.ok = true;
    contradictory_success.action = ActiveStateRecoveryAction::kActivate;
    contradictory_success.document = active_document();
    contradictory_success.error.code = ActiveStateErrorCode::kStorageError;
    contradictory_success.error.message = "contradictory success";
    const ServiceRecoveryPlan contradictory_plan =
        plan_service_recovery(contradictory_success);
    require(!contradictory_plan.ok && !contradictory_plan.start_pool &&
                contradictory_plan.state.phase ==
                    ServiceLifecyclePhase::kFailedClosed &&
                contradictory_plan.error.code ==
                    ServiceLifecycleErrorCode::kInvalidRecovery,
            "successful recovery carrying an error started a pool");

    ActiveStateRecoveryResult nonempty_none;
    nonempty_none.ok = true;
    nonempty_none.action = ActiveStateRecoveryAction::kNone;
    nonempty_none.document.version = "hidden-state";
    const ServiceRecoveryPlan nonempty_none_plan =
        plan_service_recovery(nonempty_none);
    require(!nonempty_none_plan.ok && !nonempty_none_plan.start_pool &&
                nonempty_none_plan.state.phase ==
                    ServiceLifecyclePhase::kFailedClosed,
            "kNone recovery silently discarded a nonempty document");
}

void test_route_and_replacement_derivation_is_fail_closed() {
    const ServiceLifecycleState active =
        lifecycle(ServiceLifecyclePhase::kActive, active_document());
    require(service_route_disposition(active) ==
                ServiceRouteDisposition::kServeActive &&
                service_allows_automatic_replacement(active),
            "valid active state was not routable");
    require(service_route_disposition(
                lifecycle(ServiceLifecyclePhase::kAbsent)) ==
                ServiceRouteDisposition::kNotFound,
            "absent App did not map to not found");
    require(service_route_disposition(lifecycle(
                ServiceLifecyclePhase::kRetired, retired_document())) ==
                ServiceRouteDisposition::kNotFound,
            "retired App did not map to not found");
    require(service_route_disposition(lifecycle(
                ServiceLifecyclePhase::kQuarantined,
                quarantined_document())) ==
                ServiceRouteDisposition::kUnavailable,
            "quarantined App did not map to unavailable");
    require(service_route_disposition(lifecycle(
                ServiceLifecyclePhase::kRetiring, retired_document())) ==
                ServiceRouteDisposition::kNotFound,
            "committed retire drain did not map to not found");

    const ServiceLifecycleState malformed_active =
        lifecycle(ServiceLifecyclePhase::kActive);
    require(service_route_disposition(malformed_active) ==
                ServiceRouteDisposition::kUnavailable &&
                !service_allows_automatic_replacement(malformed_active),
            "malformed active state failed open");
    ActiveStateDocument malformed_document = active_document();
    malformed_document.generation = "sha256:bad";
    const ServiceLifecycleState malformed_active_document = lifecycle(
        ServiceLifecyclePhase::kActive, std::move(malformed_document));
    require(service_route_disposition(malformed_active_document) ==
                ServiceRouteDisposition::kUnavailable &&
                !service_allows_automatic_replacement(
                    malformed_active_document),
            "active state with malformed identifiers failed open");
    const ServiceLifecycleState invalid_phase{
        static_cast<ServiceLifecyclePhase>(99), active_document()};
    require(service_route_disposition(invalid_phase) ==
                ServiceRouteDisposition::kUnavailable &&
                !service_allows_automatic_replacement(invalid_phase),
            "unknown lifecycle phase failed open");
}

void test_retire_commit_rollback_and_drain() {
    const ActiveStateDocument source = active_document();
    const ServiceLifecycleTransition begun = request_service_retire(
        lifecycle(ServiceLifecyclePhase::kActive, source));
    require_clean_success(begun, "begin active retire");
    require(begun.state.phase == ServiceLifecyclePhase::kRetiring &&
                begun.state.document.has_value() &&
                same_document(*begun.state.document, source) &&
                begun.effects.persist_required &&
                begun.effects.stop_new_requests &&
                begun.effects.persist_document.state ==
                    ActiveServiceState::kRetired &&
                begun.effects.persist_document.previous_version == "v2" &&
                begun.effects.persist_document.previous_generation ==
                    kGenerationTwo &&
                service_route_disposition(begun.state) ==
                    ServiceRouteDisposition::kUnavailable &&
                !service_allows_automatic_replacement(begun.state),
            "retire did not stop routing before persistence");

    const ServiceLifecycleTransition rolled_back =
        resolve_service_state_persistence(begun.state, pre_rename_failure());
    require_transition_error(rolled_back,
                             ServiceLifecycleErrorCode::kPersistenceFailed,
                             "/persistence",
                             "pre-rename retire failure");
    require(rolled_back.state.phase == ServiceLifecyclePhase::kActive &&
                rolled_back.state.document.has_value() &&
                same_document(*rolled_back.state.document, source) &&
                service_route_disposition(rolled_back.state) ==
                    ServiceRouteDisposition::kServeActive &&
                service_allows_automatic_replacement(rolled_back.state) &&
                !rolled_back.effects.retry_persistence,
            "pre-rename retire failure did not restore active service");

    const ServiceLifecycleTransition committed =
        resolve_service_state_persistence(begun.state,
                                          committed_persistence());
    require_clean_success(committed, "commit retired tombstone");
    require(committed.state.phase == ServiceLifecyclePhase::kRetiring &&
                committed.state.document.has_value() &&
                committed.state.document->state ==
                    ActiveServiceState::kRetired &&
                committed.effects.begin_drain &&
                service_route_disposition(committed.state) ==
                    ServiceRouteDisposition::kNotFound,
            "committed retire did not remove routing before drain");

    const ServiceLifecycleTransition drained =
        complete_service_retire_drain(committed.state);
    require_clean_success(drained, "complete retired drain");
    require(drained.state.phase == ServiceLifecyclePhase::kRetired &&
                drained.state.document.has_value() &&
                drained.state.document->state ==
                    ActiveServiceState::kRetired,
            "retire drain did not reach retired state");
    const ServiceLifecycleTransition repeated =
        complete_service_retire_drain(drained.state);
    require_clean_success(repeated, "repeat retired drain completion");
    require(repeated.effects.idempotent &&
                repeated.state.phase == ServiceLifecyclePhase::kRetired,
            "repeated drain completion was not idempotent");
}

void test_quarantined_app_can_retire_without_becoming_active() {
    const ActiveStateDocument source = quarantined_document();
    const ServiceLifecycleTransition begun = request_service_retire(
        lifecycle(ServiceLifecyclePhase::kQuarantined, source));
    require_clean_success(begun, "begin quarantined retire");
    require(begun.state.phase == ServiceLifecyclePhase::kRetiring &&
                begun.effects.persist_required &&
                begun.effects.persist_document.state ==
                    ActiveServiceState::kRetired &&
                service_route_disposition(begun.state) ==
                    ServiceRouteDisposition::kUnavailable,
            "quarantined retire re-enabled routing");

    const ServiceLifecycleTransition failed =
        resolve_service_state_persistence(begun.state, pre_rename_failure());
    require_transition_error(failed,
                             ServiceLifecycleErrorCode::kPersistenceFailed,
                             "/persistence",
                             "quarantined retire persistence failure");
    require(failed.state.phase == ServiceLifecyclePhase::kQuarantined &&
                failed.state.document.has_value() &&
                same_document(*failed.state.document, source) &&
                service_route_disposition(failed.state) ==
                    ServiceRouteDisposition::kUnavailable &&
                !service_allows_automatic_replacement(failed.state),
            "failed quarantined retire restored active service");

    const ServiceLifecycleTransition committed =
        resolve_service_state_persistence(begun.state,
                                          committed_persistence());
    require_clean_success(committed, "commit quarantined retire");
    require(committed.state.document.has_value() &&
                committed.state.document->state ==
                    ActiveServiceState::kRetired &&
                committed.effects.begin_drain &&
                service_route_disposition(committed.state) ==
                    ServiceRouteDisposition::kNotFound,
            "quarantined retire did not commit its tombstone");
}

void test_retire_idempotency_and_rejections() {
    const ServiceLifecycleTransition retired = request_service_retire(
        lifecycle(ServiceLifecyclePhase::kRetired, retired_document()));
    require_clean_success(retired, "repeat retired request");
    require(retired.effects.idempotent &&
                !retired.effects.persist_required &&
                retired.state.phase == ServiceLifecyclePhase::kRetired,
            "repeated retire rewrote the tombstone");

    require_transition_error(
        request_service_retire(lifecycle(ServiceLifecyclePhase::kAbsent)),
        ServiceLifecycleErrorCode::kNotFound,
        "/state",
        "retire absent App");
    require_transition_error(
        request_service_retire(lifecycle(
            ServiceLifecyclePhase::kRetiring, active_document())),
        ServiceLifecycleErrorCode::kBusy,
        "/state",
        "concurrent retire");
    require_transition_error(
        request_service_retire(lifecycle(
            ServiceLifecyclePhase::kQuarantining, active_document())),
        ServiceLifecycleErrorCode::kBusy,
        "/state",
        "retire during quarantine commit");
    require_transition_error(
        request_service_retire(lifecycle(
            ServiceLifecyclePhase::kDurabilityUncertain,
            active_document())),
        ServiceLifecycleErrorCode::kDurabilityUncertain,
        "/state",
        "retire during durability incident");
}

void test_quarantine_persistence_never_restores_active() {
    const ActiveStateDocument source = active_document();
    const ServiceLifecycleTransition begun = enter_service_quarantine(
        lifecycle(ServiceLifecyclePhase::kActive, source));
    require_clean_success(begun, "begin quarantine");
    require(begun.state.phase == ServiceLifecyclePhase::kQuarantining &&
                begun.effects.stop_new_requests &&
                begun.effects.persist_required &&
                begun.effects.persist_document.state ==
                    ActiveServiceState::kQuarantined &&
                begun.effects.persist_document.reason ==
                    kCrashBudgetExceededReason &&
                service_route_disposition(begun.state) ==
                    ServiceRouteDisposition::kUnavailable &&
                !service_allows_automatic_replacement(begun.state),
            "quarantine did not fail closed before persistence");

    const ServiceLifecycleTransition failed =
        resolve_service_state_persistence(begun.state, pre_rename_failure());
    require_transition_error(failed,
                             ServiceLifecycleErrorCode::kPersistenceFailed,
                             "/persistence",
                             "pre-rename quarantine failure");
    require(failed.state.phase == ServiceLifecyclePhase::kQuarantining &&
                failed.state.document.has_value() &&
                same_document(*failed.state.document, source) &&
                failed.effects.retry_persistence &&
                failed.effects.persist_required &&
                failed.effects.stop_new_requests &&
                failed.effects.persist_document.state ==
                    ActiveServiceState::kQuarantined &&
                service_route_disposition(failed.state) ==
                    ServiceRouteDisposition::kUnavailable &&
                !service_allows_automatic_replacement(failed.state),
            "quarantine write failure resumed crash-loop traffic");

    const ServiceLifecycleTransition committed =
        resolve_service_state_persistence(begun.state,
                                          committed_persistence());
    require_clean_success(committed, "commit quarantine");
    require(committed.state.phase == ServiceLifecyclePhase::kQuarantined &&
                committed.state.document.has_value() &&
                committed.state.document->state ==
                    ActiveServiceState::kQuarantined &&
                committed.effects.begin_drain &&
                service_route_disposition(committed.state) ==
                    ServiceRouteDisposition::kUnavailable,
            "committed quarantine did not remain unavailable");
}

void test_durability_uncertainty_blocks_all_lifecycle_writes() {
    for (const ServiceLifecycleTransition& begun : {
             request_service_retire(lifecycle(
                 ServiceLifecyclePhase::kActive, active_document())),
             enter_service_quarantine(lifecycle(
                 ServiceLifecyclePhase::kActive, active_document()))}) {
        require(begun.ok, "failed to create persistence transition fixture");
        const ServiceLifecycleTransition uncertain =
            resolve_service_state_persistence(begun.state,
                                              durability_uncertain());
        require_transition_error(uncertain,
                                 ServiceLifecycleErrorCode::kDurabilityUncertain,
                                 "/persistence",
                                 "post-rename durability uncertainty");
        require(uncertain.state.phase ==
                    ServiceLifecyclePhase::kDurabilityUncertain &&
                    uncertain.effects.block_state_writes &&
                    uncertain.effects.stop_new_requests &&
                    service_route_disposition(uncertain.state) ==
                        ServiceRouteDisposition::kUnavailable &&
                    !service_allows_automatic_replacement(uncertain.state),
                "durability incident did not block routing and state writes");
    }

    ActiveStatePersistResult inconsistent_success = committed_persistence();
    inconsistent_success.active_name_replaced = false;
    const ServiceLifecycleTransition begun = request_service_retire(
        lifecycle(ServiceLifecyclePhase::kActive, active_document()));
    const ServiceLifecycleTransition inconsistent =
        resolve_service_state_persistence(begun.state, inconsistent_success);
    require_transition_error(inconsistent,
                             ServiceLifecycleErrorCode::kInvalidPersistenceResult,
                             "/persistence",
                             "success without replaced active name");
    require(inconsistent.state.phase ==
                ServiceLifecyclePhase::kDurabilityUncertain &&
                inconsistent.effects.block_state_writes,
            "inconsistent persistence result failed open");

    ActiveStatePersistResult success_with_error = committed_persistence();
    success_with_error.error.code = ActiveStateErrorCode::kStorageError;
    success_with_error.error.message = "contradictory success";
    const ServiceLifecycleTransition contradictory_success =
        resolve_service_state_persistence(begun.state, success_with_error);
    require_transition_error(
        contradictory_success,
        ServiceLifecycleErrorCode::kInvalidPersistenceResult,
        "/persistence",
        "successful persistence carrying an error");
    require(contradictory_success.state.phase ==
                ServiceLifecyclePhase::kDurabilityUncertain &&
                contradictory_success.effects.block_state_writes,
            "successful persistence with an error failed open");

    const ActiveStatePersistResult failure_without_error;
    const ServiceLifecycleTransition contradictory_failure =
        resolve_service_state_persistence(begun.state,
                                          failure_without_error);
    require_transition_error(
        contradictory_failure,
        ServiceLifecycleErrorCode::kInvalidPersistenceResult,
        "/persistence",
        "failed persistence without an error");
    require(contradictory_failure.state.phase ==
                ServiceLifecyclePhase::kDurabilityUncertain &&
                contradictory_failure.effects.block_state_writes,
            "failed persistence without an error guessed a rollback state");
}

void test_invalid_lifecycle_transitions_are_rejected() {
    require_transition_error(
        enter_service_quarantine(lifecycle(
            ServiceLifecyclePhase::kRetired, retired_document())),
        ServiceLifecycleErrorCode::kInvalidState,
        "/state",
        "quarantine retired App");
    require_transition_error(
        resolve_service_state_persistence(
            lifecycle(ServiceLifecyclePhase::kActive, active_document()),
            committed_persistence()),
        ServiceLifecycleErrorCode::kInvalidState,
        "/state",
        "persistence result without pending mutation");
    require_transition_error(
        complete_service_retire_drain(lifecycle(
            ServiceLifecyclePhase::kRetiring, active_document())),
        ServiceLifecycleErrorCode::kInvalidState,
        "/state",
        "retire drain before tombstone commit");
    require_transition_error(
        request_service_retire(lifecycle(
            ServiceLifecyclePhase::kActive, retired_document())),
        ServiceLifecycleErrorCode::kInvalidState,
        "/state",
        "active phase with retired document");
    require_transition_error(
        request_service_retire(ServiceLifecycleState{
            static_cast<ServiceLifecyclePhase>(99), active_document()}),
        ServiceLifecycleErrorCode::kInvalidState,
        "/state",
        "unknown lifecycle phase");

    const ServiceLifecycleTransition begun = request_service_retire(
        lifecycle(ServiceLifecyclePhase::kActive, active_document()));
    const ServiceLifecycleTransition committed =
        resolve_service_state_persistence(begun.state,
                                          committed_persistence());
    require(committed.ok, "failed to build committed retire fixture");
    require_transition_error(
        resolve_service_state_persistence(committed.state,
                                          committed_persistence()),
        ServiceLifecycleErrorCode::kInvalidState,
        "/state",
        "duplicate persistence resolution after retired commit");

    ActiveStateDocument malformed_pending = active_document();
    malformed_pending.generation = "sha256:bad";
    require_transition_error(
        resolve_service_state_persistence(
            lifecycle(ServiceLifecyclePhase::kRetiring,
                      std::move(malformed_pending)),
            committed_persistence()),
        ServiceLifecycleErrorCode::kInvalidState,
        "/state",
        "resolve malformed pending retire");

    ActiveStateDocument malformed_tombstone = retired_document();
    malformed_tombstone.previous_generation = "sha256:bad";
    require_transition_error(
        complete_service_retire_drain(
            lifecycle(ServiceLifecyclePhase::kRetiring,
                      std::move(malformed_tombstone))),
        ServiceLifecycleErrorCode::kInvalidState,
        "/state",
        "complete malformed retired tombstone drain");
}

void require_deploy(const ExplicitDeployPlan& plan,
                    bool ok,
                    ExplicitDeployAction action,
                    std::string_view label) {
    require(plan.ok == ok && plan.action == action,
            std::string(label) + " returned the wrong deploy action");
    if (ok) {
        require(plan.error.code == ServiceLifecycleErrorCode::kNone &&
                    plan.error.path.empty() && plan.error.message.empty(),
                std::string(label) + " succeeded with a stale error");
    } else {
        require(plan.error.code != ServiceLifecycleErrorCode::kNone &&
                    !plan.error.path.empty() && !plan.error.message.empty(),
                std::string(label) + " failed without a safe error");
    }
}

void test_explicit_deploy_idempotency_and_reactivation() {
    const ServiceLifecycleState active =
        lifecycle(ServiceLifecyclePhase::kActive, active_document());
    require_deploy(plan_explicit_service_deploy(
                       active, "orders", "v2", kGenerationTwo),
                   true,
                   ExplicitDeployAction::kAlreadyActive,
                   "exact active deploy");
    require_deploy(plan_explicit_service_deploy(
                       active, "orders", "v3", kGenerationOne),
                   true,
                   ExplicitDeployAction::kWarmAndActivate,
                   "new active Version deploy");
    require_deploy(plan_explicit_service_deploy(
                       active, "orders", "v2", kGenerationOne),
                   false,
                   ExplicitDeployAction::kVersionImmutabilityConflict,
                   "active Version remap");

    const ServiceLifecycleState retired =
        lifecycle(ServiceLifecyclePhase::kRetired, retired_document());
    const ExplicitDeployPlan retired_same = plan_explicit_service_deploy(
        retired, "orders", "v2", kGenerationTwo);
    require_deploy(retired_same,
                   true,
                   ExplicitDeployAction::kWarmAndActivate,
                   "same retired generation deploy");
    require(!retired_same.reset_instability_budget_after_activation,
            "retired deploy reset a nonexistent quarantine budget");
    require_deploy(plan_explicit_service_deploy(
                       retired, "orders", "v2", kGenerationOne),
                   false,
                   ExplicitDeployAction::kVersionImmutabilityConflict,
                   "retired Version remap");

    const ServiceLifecycleState quarantined = lifecycle(
        ServiceLifecyclePhase::kQuarantined, quarantined_document());
    const ExplicitDeployPlan quarantined_same = plan_explicit_service_deploy(
        quarantined, "orders", "v2", kGenerationTwo);
    require_deploy(quarantined_same,
                   true,
                   ExplicitDeployAction::kWarmAndActivate,
                   "same quarantined generation deploy");
    require(quarantined_same.reset_instability_budget_after_activation,
            "quarantined deploy did not defer budget reset to activation");
    require_deploy(plan_explicit_service_deploy(
                       quarantined, "orders", "v2", kGenerationOne),
                   false,
                   ExplicitDeployAction::kVersionImmutabilityConflict,
                   "quarantined Version remap");

    const ExplicitDeployPlan quarantined_new = plan_explicit_service_deploy(
        quarantined, "orders", "v3", kGenerationOne);
    require_deploy(quarantined_new,
                   true,
                   ExplicitDeployAction::kWarmAndActivate,
                   "new quarantined Version deploy");
    require(quarantined_new.reset_instability_budget_after_activation,
            "new Version deploy did not clear quarantine after activation");

    require_deploy(plan_explicit_service_deploy(
                       lifecycle(ServiceLifecyclePhase::kAbsent),
                       "orders",
                       "v1",
                       kGenerationOne),
                   true,
                   ExplicitDeployAction::kWarmAndActivate,
                   "absent App deploy");
}

void test_explicit_deploy_rejects_busy_uncertain_and_invalid_targets() {
    for (const ServiceLifecycleState& state : {
             lifecycle(ServiceLifecyclePhase::kRetiring, active_document()),
             lifecycle(ServiceLifecyclePhase::kQuarantining,
                       active_document())}) {
        require_deploy(plan_explicit_service_deploy(
                           state, "orders", "v3", kGenerationOne),
                       false,
                       ExplicitDeployAction::kRejectBusy,
                       "deploy during mutation");
    }
    require_deploy(plan_explicit_service_deploy(
                       lifecycle(ServiceLifecyclePhase::kDurabilityUncertain,
                                 active_document()),
                       "orders",
                       "v3",
                       kGenerationOne),
                   false,
                   ExplicitDeployAction::kRejectDurabilityUncertain,
                   "deploy during durability incident");
    require_deploy(plan_explicit_service_deploy(
                       lifecycle(ServiceLifecyclePhase::kFailedClosed),
                       "orders",
                       "v3",
                       kGenerationOne),
                   false,
                   ExplicitDeployAction::kRejectInvalidState,
                   "deploy from failed recovery");

    const ServiceLifecycleState active =
        lifecycle(ServiceLifecyclePhase::kActive, active_document());
    ActiveStateDocument malformed_existing_document = active_document();
    malformed_existing_document.generation = "sha256:bad";
    const ExplicitDeployPlan malformed_existing =
        plan_explicit_service_deploy(
            lifecycle(ServiceLifecyclePhase::kActive,
                      std::move(malformed_existing_document)),
            "orders",
            "v3",
            kGenerationOne);
    require_deploy(malformed_existing,
                   false,
                   ExplicitDeployAction::kRejectInvalidState,
                   "deploy from malformed lifecycle state");
    require(malformed_existing.error.code ==
                ServiceLifecycleErrorCode::kInvalidState &&
                malformed_existing.error.path == "/state",
            "malformed lifecycle state was reported as a deploy target error");

    const struct {
        std::string_view application;
        std::string_view version;
        std::string_view generation;
        std::string_view path;
    } invalid[] = {
        {"Orders", "v3", kGenerationOne, "/app"},
        {"orders", "../v3", kGenerationOne, "/version"},
        {"orders", "v3", "sha256:bad", "/generation"},
        {"billing", "v3", kGenerationOne, "/app"},
    };
    for (const auto& test : invalid) {
        const ExplicitDeployPlan plan = plan_explicit_service_deploy(
            active, test.application, test.version, test.generation);
        require_deploy(plan,
                       false,
                       ExplicitDeployAction::kRejectInvalidState,
                       "invalid deploy target");
        require(plan.error.code ==
                    ServiceLifecycleErrorCode::kInvalidDeployTarget,
                "invalid deploy target did not use kInvalidDeployTarget");
        require(plan.error.path == test.path,
                "invalid deploy target reported path '" + plan.error.path +
                    "' instead of '" + std::string(test.path) + "'");
    }
}

}  // namespace

int main() {
    test_invalid_lifecycle_transitions_are_rejected();
    test_recovery_action_document_mismatch_fails_closed();
    test_retired_or_quarantined_app_never_reactivates_on_restart();
    test_route_and_replacement_derivation_is_fail_closed();
    test_retire_commit_rollback_and_drain();
    test_quarantined_app_can_retire_without_becoming_active();
    test_retire_idempotency_and_rejections();
    test_quarantine_persistence_never_restores_active();
    test_durability_uncertainty_blocks_all_lifecycle_writes();
    test_explicit_deploy_idempotency_and_reactivation();
    test_explicit_deploy_rejects_busy_uncertain_and_invalid_targets();
    return 0;
}
