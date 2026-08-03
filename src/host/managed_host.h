#ifndef CAPSID_HOST_MANAGED_HOST_H
#define CAPSID_HOST_MANAGED_HOST_H

#include "host/policy_compiler.h"

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
    int secret_root_template_fd = -1;  // dirfd of the secret root
    std::string state_root;            // absolute path, created by the Host
    std::string application;           // the single active App
    HostPolicy host_policy;
    std::vector<std::string> trusted_key_ids;  // allowed signing key ids
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
