#ifndef CAPSID_HOST_MANAGED_ADMIN_BACKEND_H
#define CAPSID_HOST_MANAGED_ADMIN_BACKEND_H

#include "host/admin_api.h"
#include "host/managed_host.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
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

// A deploy's target generation is unknown until the artifact is COMPLETE,
// but a startup-permit request must carry a valid generation. Deploys use
// the current active generation, or this zero probe when the App has
// never been active; the field only participates in replacement
// singleflight matching and identifier validation — never in fairness.
inline constexpr std::string_view kStartupPermitProbeGeneration =
    "sha256:0000000000000000000000000000000000000000000000000000000000000000";

// M2 item 5b: the process-global fair startup-permit coordinator (design
// §10.5.6). A single FairStartupPermitQueue instance is shared across all
// Apps and both startup paths (deploy and replacement); a grant is held
// by exactly one caller at a time and handed to the next waiter when
// released. The pure queue decides ORDER and fairness; the coordinator
// decides TIMING: an idle queue grants the first request immediately, a
// release awards the next grant, and the stop signal interrupts a wait
// and withdraws the request (shutdown is never startup).
//
// WorkerCapacityPermit still decides CONCURRENCY: an App that cannot
// acquire capacity never reaches this queue, and a granted permit is
// consumed by a single spawn/READY window (the holder releases it when
// the operation settles).
class StartupPermitCoordinator {
public:
    StartupPermitCoordinator(const std::atomic<bool>* stop_requested,
                             std::size_t maximum_queued)
        : maximum_queued_(maximum_queued),
          stop_requested_(stop_requested) {}

    // Enqueues the request and blocks until its grant is awarded. The
    // first request while the queue is idle is granted immediately.
    // Returns false — and grants nothing — when the queue is full, the
    // request is invalid, an exact (App, generation) replacement is
    // already queued (singleflight), or the stop signal fired while
    // waiting (the request is withdrawn from the queue in that case).
    bool enqueue_and_wait(const StartupPermitRequest& request) {
        std::unique_lock<std::mutex> lock(mutex_);
        StartupPermitRequest mine = request;
        mine.ticket = next_ticket_++;
        const StartupPermitQueueResult queued =
            enqueue_startup_permit_request(queue_, mine, maximum_queued_);
        if (!queued.ok) {
            return false;  // queue full or invalid request: fail closed
        }
        queue_ = queued.queue;
        if (queued.joined_existing) {
            // A same (App, generation) replacement is already queued; in
            // v1 a single supervisor thread per App drives replacement
            // serially, so a duplicate is unreachable and rejected.
            return false;
        }
        const std::uint64_t my_ticket = mine.ticket;
        if (!granted_ticket_.has_value()) {
            // Queue idle: the first request is granted immediately.
            const StartupPermitGrantResult granted =
                grant_next_startup_permit(queue_, true);
            if (granted.ok && granted.granted.has_value()) {
                queue_ = granted.queue;
                granted_ticket_ = granted.granted->ticket;
            }
        }
        // Poll-stop wait: the process-wide stop signal has no attached
        // condition variable, so the wait re-checks it every slice.
        while (granted_ticket_ != my_ticket) {
            if (stop_requested_ != nullptr &&
                stop_requested_->load(std::memory_order_relaxed)) {
                break;
            }
            condition_.wait_for(lock, std::chrono::milliseconds(100));
        }
        if (granted_ticket_ != my_ticket) {
            // Stop fired while waiting: withdraw the request so the queue
            // is not left holding a waiter nobody will service.
            queue_.queued.erase(
                std::remove_if(
                    queue_.queued.begin(), queue_.queued.end(),
                    [&](const StartupPermitRequest& item) {
                        return item.ticket == my_ticket;
                    }),
                queue_.queued.end());
            return false;
        }
        // The permit stays marked as granted to THIS request until
        // release_grant() hands it to the next waiter: erasing the mark
        // here would let a concurrent enqueue steal the permit while the
        // holder is still starting its worker.
        return true;
    }

    // Releases the held grant and awards the next one; the next waiter
    // wakes. Must be called exactly once per successful enqueue_and_wait.
    void release_grant() {
        std::lock_guard<std::mutex> lock(mutex_);
        const StartupPermitGrantResult next =
            grant_next_startup_permit(queue_, true);
        if (next.ok) {
            queue_ = next.queue;
            granted_ticket_ = next.granted.has_value()
                                  ? std::make_optional(
                                        next.granted->ticket)
                                  : std::nullopt;
        }
        condition_.notify_all();
    }

private:
    std::uint64_t next_ticket_ = 1;
    std::size_t maximum_queued_;
    const std::atomic<bool>* stop_requested_;
    FairStartupPermitQueue queue_;
    // The ticket currently holding the permit (nullopt when idle). A
    // ticket granted to a waiter that stops before claiming it is
    // released by that waiter itself.
    std::optional<std::uint64_t> granted_ticket_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
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
    // Optional process-global fair startup-permit coordinator (design
    // §10.5.6). A deploy joins the shared queue after acquiring capacity
    // and holds its grant across the whole pipeline. Null disables the
    // queue (deploys start immediately).
    StartupPermitCoordinator* startup_permits = nullptr;
};

}  // namespace capsid::host

#endif
