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
    // Explicit worker ownership handoff. When a managed deploy succeeds
    // with a non-null worker, activate_worker(application, worker) is
    // called with the owning pointer. A true return transfers ownership to
    // the callback (the Async backend must neither destroy nor leak it); a
    // missing callback, a false return or an exception destroys the
    // unclaimed worker and marks the public operation as a redacted
    // Failed. retire_worker(application) is invoked after a successful
    // retire so the owner can reclaim and destroy the drained worker.
    std::function<bool(const std::string&, capsid_worker*)> activate_worker;
    // Atomic POOL ownership handoff. A deploy of a fixed N>1 pool warms
    // the whole pool before Active is reported; activate_pool(application,
    // workers) then receives the entire owning pool. A true return
    // transfers ownership of every worker to the callback (the Async
    // backend must neither destroy nor leak any of them); a missing
    // callback, a false return or an exception destroys the whole pool and
    // marks the public operation as a redacted Failed. A multi-worker pool
    // NEVER falls back to the legacy activate_worker: an old single-worker
    // callback would claim just one process of a bigger pool.
    std::function<bool(const std::string&,
                       std::vector<capsid_worker*>)> activate_pool;
    // PR-09c (§9.3) GENERATION ownership handoff: invoked when the
    // coordinator returns a warmed generation — the whole fleet, the §8.3
    // replacement factory, and the version + digest identity — so the data
    // plane can adopt a pool in place (spawn replacements through the same
    // artifact/config, label the pool with the generation identity). When
    // set it is preferred over activate_pool_/activate_worker_ for EVERY
    // pool size: a single-worker App is a one-worker pool, not a bare
    // worker. A true return transfers ownership of every worker (the Async
    // backend must neither destroy nor leak any of them); a missing
    // callback, a false return or an exception destroys the whole pool and
    // marks the public operation as a redacted Failed.
    std::function<bool(const std::string& application,
                       std::vector<capsid_worker*> workers,
                       const WorkerExecutor::WorkerFactory& factory,
                       const std::string& version,
                       const std::string& generation_digest)>
        activate_generation;
    std::function<void(const std::string&)> retire_worker;
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
    std::function<bool(const std::string&, capsid_worker*)> activate_worker_;
    std::function<bool(const std::string&,
                       std::vector<capsid_worker*>)> activate_pool_;
    std::function<bool(const std::string&, std::vector<capsid_worker*>,
                       const WorkerExecutor::WorkerFactory&,
                       const std::string&, const std::string&)>
        activate_generation_;
    std::function<void(const std::string&)> retire_worker_;
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

// Process-global worker permit (capacity.workersTotal). The permit is
// bound to an ACTIVE App slot: a replacement deploy of an App that already
// holds the slot does not acquire again, a failed replacement leaves the
// old holder's slot untouched, and only a newly acquired permit is
// returned when the deploy settles without a live worker.
class WorkerCapacityPermit {
public:
    explicit WorkerCapacityPermit(int capacity) : remaining_(capacity) {}

    // Acquires the permit for `application`. Returns true when the permit
    // was newly acquired (the caller must release it if the operation
    // settles without a live worker); returns false when the App already
    // holds its slot (replacement) or when the global capacity is
    // exhausted.
    bool acquire(const std::string& application) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (holders_.find(application) != holders_.end()) {
            return false;  // replacement of an existing slot
        }
        int remaining = remaining_.load();
        while (remaining > 0 &&
               !remaining_.compare_exchange_weak(remaining,
                                                 remaining - 1)) {
        }
        if (remaining <= 0) {
            return false;  // capacity exhausted
        }
        holders_.insert(application);
        return true;
    }

    // Records a live worker for the App (already-held slots are kept).
    void record_success(const std::string& application) {
        std::lock_guard<std::mutex> lock(mutex_);
        holders_.insert(application);
    }

    // Returns the slot: the App's activated worker no longer holds the
    // permit (retire) or a newly acquired permit that settled without a
    // worker.
    void release(const std::string& application) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (holders_.erase(application) > 0) {
            remaining_.fetch_add(1);
        }
    }

    bool holds(const std::string& application) {
        std::lock_guard<std::mutex> lock(mutex_);
        return holders_.find(application) != holders_.end();
    }

private:
    std::atomic<int> remaining_;
    std::mutex mutex_;
    std::set<std::string> holders_;
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

public:
    // Optional process-global worker permit (capacity.workersTotal). A
    // deploy consumes the App's slot BEFORE any spawn or durable
    // activation; the slot stays bound to the active worker until its
    // retire. Null disables the gate.
    WorkerCapacityPermit* capacity = nullptr;
};

}  // namespace capsid::host

#endif
