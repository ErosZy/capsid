#ifndef CAPSID_HOST_MANAGED_ADMIN_BACKEND_H
#define CAPSID_HOST_MANAGED_ADMIN_BACKEND_H

#include "host/admin_api.h"
#include "host/managed_host.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

struct capsid_worker;

namespace capsid::host {

struct AsyncAdminBackendOptions {
    // Bound on concurrently running plus queued operations. A full pool
    // rejects new submissions immediately.
    std::size_t max_pending_operations = 8;
    // Process-level stop signal (SIGTERM shutdown). When it fires, queued
    // operations are cancelled (recorded as redacted Failed) and the
    // worker loop stops taking new work; a running managed deploy
    // observes it through the coordinator's own stop_requested field.
    const std::atomic<bool>* external_stop = nullptr;
    // PR-10 (§9.3): worker ownership no longer hands off through this
    // layer. The activation/retire transactions live on the coordinator
    // (ManagedHostOptions prepare/commit/abort), which publishes the
    // routing and drains the old generation INSIDE the deploy/retire
    // operation. The Async backend is purely the bounded executor: a
    // successful deploy/retire is Active, period. A successful deploy
    // whose outcome still carries workers means the coordinator was
    // configured WITHOUT the transaction callbacks (only the legacy
    // direct-call path can produce it); there is no publisher here, so
    // the Async backend recycles the unclaimed pool and fails the public
    // operation closed rather than leak it.
};

// Bounded background executor between the Admin HTTP layer and a blocking
// coordinator backend. deploy/retire are queued onto a single worker
// thread: a public operation ID is generated and returned immediately with
// kValidating progress, the registry is mutex-protected and observable
// through operation_status (non-terminal states transition to Active or
// Failed), and the pool is bounded (full -> immediate rejection). Inner
// failures and exceptions are recorded as redacted Failed states; the
// destructor stops the worker and joins it (never detaches).
class AsyncAdminBackend final : public AdminBackend {
public:
    AsyncAdminBackend(AdminBackend* inner,
                      const AsyncAdminBackendOptions& options);
    ~AsyncAdminBackend() override;

    AsyncAdminBackend(const AsyncAdminBackend&) = delete;
    AsyncAdminBackend& operator=(const AsyncAdminBackend&) = delete;

    DeployOutcome deploy(const std::string& application,
                         const std::string& version,
                         OperationStatus* status) override;
    DeployOutcome retire(const std::string& application,
                         OperationStatus* status) override;
    OperationStatus operation_status(
        const std::string& operation_id) override;
    std::string app_status(const std::string& application) override;

private:
    struct PendingOperation {
        bool retire = false;
        std::string application;
        std::string version;
        std::string operation_id;
    };
    void worker_loop();
    DeployOutcome submit(bool retire, const std::string& application,
                         const std::string& version,
                         OperationStatus* status);

    AdminBackend* inner_;
    std::size_t max_pending_;
    const std::atomic<bool>* external_stop_;
    std::mutex mutex_;
    std::condition_variable condition_;
    std::vector<PendingOperation> queue_;
    // Running + queued operations (released when the worker finishes).
    std::size_t pending_count_ = 0;
    bool stopping_ = false;
    std::thread worker_;
    // operation_id -> last recorded status; running and terminal states
    // both live here so operation_status can observe progress.
    std::map<std::string, OperationStatus> registry_;
};

// Maps an exact App ID to its ManagedHostOptions and drives the REAL
// managed_* coordinator functions (managed_deploy / managed_retire /
// managed_operation_status / managed_app_status). An unknown App fails
// closed with a static error.
class ManagedAdminBackend final : public AdminBackend {
public:
    explicit ManagedAdminBackend(
        const std::vector<ManagedHostOptions*>& applications);

    DeployOutcome deploy(const std::string& application,
                         const std::string& version,
                         OperationStatus* status) override;
    DeployOutcome retire(const std::string& application,
                         OperationStatus* status) override;
    OperationStatus operation_status(
        const std::string& operation_id) override;
    std::string app_status(const std::string& application) override;

private:
    ManagedHostOptions* find_application(const std::string& application);

    std::vector<ManagedHostOptions*> applications_;
};

}  // namespace capsid::host

#endif
