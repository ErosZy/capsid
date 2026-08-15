#ifndef CAPSID_HOST_MANAGED_HOST_H
#define CAPSID_HOST_MANAGED_HOST_H

struct capsid_worker;

#include "host/activation_transaction.h"
#include "host/bytecode_attestation.h"
#include "host/binding_compile.h"
#include "host/policy_compiler.h"
#include "host/service_lifecycle.h"
#include "host/worker_capacity_ledger.h"
#include "host/worker_executor.h"
#include "host/worker_recovery.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace capsid::host {

class StructuredLog;
class MetricsRegistry;

// M1D managed host coordinator. One active App, one worker per generation,
// one path listener. The deploy pipeline follows the frozen commit
// sequence: safe-read the version, select the artifact (source / trusted
// bytecode / compatibility fallback / fail closed), compile the policy and
// secrets, stage, fdatasync, COMPLETE, rename to the generation, fsync,
// write the immutable version mapping, spawn the worker, load, wait for
// READY, persist active.json, fsync, publish routing, drain the old worker.

// Declared BEFORE ManagedHostOptions: the §9.3 transaction callback fields
// use it in their std::function signatures, and std::function instantiated
// with an incomplete parameter type cannot be called once the type is
// completed (GCC 13).
struct DeployOutcome {
    bool ok = false;
    std::string operation_id;
    std::string error;  // static text
    // The whole owning pool: every warmed worker (READY + compatibility
    // verified), all owned by the caller after a successful deploy; the
    // caller wires the data plane and drains the previous pool. The pool
    // is atomic — on any failure it is empty and no worker escapes.
    std::vector<capsid_worker*> workers;
    // Legacy single-worker entry. Only valid when workers.size() == 1,
    // where it aliases workers[0]; null for any other pool size, so an
    // old consumer can never be handed just one worker of a bigger pool.
    capsid_worker* worker = nullptr;
    // PR-09c (§8.3/§9.3) generation handoff: the §8.3 replacement factory
    // plus the generation identity. A data-plane pool adopted over
    // `workers` spawns replacements through this factory — same artifact,
    // same effective config, no re-read of the upload directory — and the
    // pool's application/version/digest identity comes from these fields.
    // Set on every successful warm (deploy, idempotent redeploy, recovery);
    // always null on failure.
    WorkerExecutor::WorkerFactory generation_factory;
    std::string version;
    std::string generation_digest;
    // Binding v1 §6: the committed immutable Binding snapshot; worker
    // replacement reloads from this, never from a re-read of bindingsRoot.
    std::vector<capsid::host::EffectiveBinding> bindings;
};

struct ManagedHostOptions {
    // Pre-opened directory descriptors (safe-read boundary).
    int applications_root_fd = -1;
    // Pre-opened secret root dirfd; the App subdirectory is opened and
    // owner/mode verified by the coordinator before any secret read.
    int secret_root_template_fd = -1;
    std::string state_root;  // absolute path, created by the Host
    std::string application; // the single active App
    std::string worker_path; // absolute capsid-worker path
    // Binding v1 §2.1: the scanned bindingsRoot snapshot root. Empty
    // disables bindings for this Host.
    std::string bindings_root;
    HostPolicy host_policy;
    // Trusted Ed25519 keys for bytecode attestation verification.
    std::vector<capsid::host::TrustedBytecodeKey> trusted_keys;
    // The runtime compatibility ID the worker must report at READY.
    std::string runtime_compatibility_id;
    // Optional process-level stop signal. When set and fired, the worker
    // READY handshake aborts promptly (spawned worker destroyed, operation
    // fails) instead of waiting out its 15-second deadline, so a SIGTERM
    // shutdown can cancel a genuinely running deploy.
    const std::atomic<bool>* stop_requested = nullptr;
    // M2 item 5a: the crash-budget/backoff policy the worker supervisor
    // decides against. Populated by the Host from recovery.*; the pure
    // decision functions fail closed on an invalid policy.
    WorkerRecoveryPolicy recovery_policy;
    // §9.3 activation transaction (PR-10). The three activation callbacks
    // must be configured together (or none): the coordinator runs prepare
    // BEFORE the active-state persist (may fail — a failed prepare must
    // already have destroyed the warmed workers it was handed), commit
    // after a successful persist (must never fail: atomic publication +
    // drain signal only, see activation_transaction.h), and abort when
    // the persist failed. On the transactional path the coordinator owns
    // no workers after prepare: the plan does.
    std::function<std::unique_ptr<ActivationPlan>(
        const std::string& application, const DeployOutcome& prepared,
        std::string* error)>
        prepare_activation;
    std::function<void(ActivationPlan* plan)> commit_activation;  // noexcept
    std::function<void(ActivationPlan* plan)> abort_activation;   // noexcept
    // §9.3 retire transaction: prepare_retire before the tombstone
    // persist (may fail), commit_retire after it (noexcept — drain
    // signal + ledger category switch), abort_retire when it failed.
    std::function<std::unique_ptr<RetirePlan>(
        const std::string& application, std::string* error)>
        prepare_retire;
    std::function<void(RetirePlan* plan)> commit_retire;  // noexcept
    std::function<void(RetirePlan* plan)> abort_retire;   // noexcept
    // §9.4 process-global weighted capacity ledger (one object shared by
    // every App's options). The coordinator reserves the target pool
    // count before ANY spawn and settles the reserve on the transaction
    // commit; null disables the gate.
    WorkerCapacityLedger* ledger = nullptr;
    // M2 item 7: the process-wide structured log and metrics registry
    // (design §12). Null disables event logging/metrics on this path.
    StructuredLog* log = nullptr;
    MetricsRegistry* metrics = nullptr;
};

enum class OperationState {
    kValidating,
    kStaging,
    kWarming,
    kActivating,
    kActive,
    kFailed,
};

struct OperationStatus {
    std::string operation_id;
    OperationState state = OperationState::kValidating;
    std::string version;
    std::string error;  // static text; never secret values or paths
};

enum class DeployErrorCode {
    kNone,
    kInvalidRequest,
    kArtifactSelectionFailed,
    kPolicyCompileFailed,
    kSecretFailed,
    kStagingFailed,
    kWorkerFailed,
    kVersionConflict,
    kConcurrentMutation,
};

// Runs the deploy pipeline for app/version. On success the active
// generation is persisted and the worker is warm; the caller then
// publishes the data-plane routing.
DeployOutcome managed_deploy(ManagedHostOptions* options,
                             const std::string& version,
                             OperationStatus* status);

// Retire the active App: persist the retired tombstone (no worker
// restart). Returns the operation id.
DeployOutcome managed_retire(ManagedHostOptions* options,
                             OperationStatus* status);

// M2 item 5a: persist the crash-budget quarantine tombstone (state
// quarantined + CRASH_BUDGET_EXCEEDED) for the CURRENT active document,
// mirroring the retire tombstone path. Idempotent: an already-quarantined
// or retired App is a successful no-op (the App is already not serving).
// The caller (the worker supervisor) stops automatic replacement and
// removes the dead worker from the routing map after this returns.
DeployOutcome managed_quarantine(ManagedHostOptions* options,
                                 OperationStatus* status);

// M2 item 5a: reads the current App lifecycle (active-state document +
// phase) for the supervisor's decision input, through the same verified
// dirfd walk and strict recovery rules as boot recovery. ok=false when
// the state cannot be read at all.
struct ManagedLifecycleSnapshot {
    bool ok = false;
    ServiceLifecycleState state;
};
ManagedLifecycleSnapshot managed_read_lifecycle(ManagedHostOptions* options);

// M2 item 6 (design §7.4): read the active health probe config from the
// committed generation's capsid.json (re-read on every re-anchor, so a
// redeployed healthCheck takes effect with the new generation). Fails
// closed: any read/parse failure yields configured=false and the App
// keeps passive signals only.
HealthCheckConfig managed_read_health_check(ManagedHostOptions* options,
                                            const std::string& generation);

// Startup recovery: load active.json if present; validate the COMPLETE
// generation (artifacts, policy, trusted keys, identity); spawn and warm
// the worker; publish routing. Missing active.json means no active App.
DeployOutcome managed_recover(ManagedHostOptions* options,
                              OperationStatus* status);

// Reads the current operation status by id.
OperationStatus managed_operation_status(
    const ManagedHostOptions& options,
    const std::string& operation_id);

// Reads the App status (active version, generation digest, worker pid).
std::string managed_app_status(const ManagedHostOptions& options);

}  // namespace capsid::host

#endif
