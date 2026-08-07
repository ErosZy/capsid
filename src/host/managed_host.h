#ifndef CAPSID_HOST_MANAGED_HOST_H
#define CAPSID_HOST_MANAGED_HOST_H

struct capsid_worker;

#include "host/bytecode_attestation.h"
#include "host/policy_compiler.h"
#include "host/service_lifecycle.h"
#include "host/worker_recovery.h"

#include <atomic>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace capsid::host {

// M1D managed host coordinator. One active App, one worker per generation,
// one path listener. The deploy pipeline follows the frozen commit
// sequence: safe-read the version, select the artifact (source / trusted
// bytecode / compatibility fallback / fail closed), compile the policy and
// secrets, stage, fdatasync, COMPLETE, rename to the generation, fsync,
// write the immutable version mapping, spawn the worker, load, wait for
// READY, persist active.json, fsync, publish routing, drain the old worker.

struct ManagedHostOptions {
    // Pre-opened directory descriptors (safe-read boundary).
    int applications_root_fd = -1;
    // Pre-opened secret root dirfd; the App subdirectory is opened and
    // owner/mode verified by the coordinator before any secret read.
    int secret_root_template_fd = -1;
    std::string state_root;  // absolute path, created by the Host
    std::string application; // the single active App
    std::string worker_path; // absolute capsid-worker path
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
