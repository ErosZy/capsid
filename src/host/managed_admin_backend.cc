// M1D managed Admin adapters. See managed_admin_backend.h.

#include "host/managed_admin_backend.h"

#include "capsid/runtime.h"

#include <atomic>
#include <condition_variable>
#include <map>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>

namespace capsid::host {
namespace {

// Unique public operation ID: prefix + pid + a process-wide atomic counter.
std::string admin_operation_id() {
    static std::atomic<std::uint64_t> counter{0};
    const std::uint64_t id = counter.fetch_add(1, std::memory_order_relaxed);
    std::ostringstream out;
    out << "op-admin-" << static_cast<long long>(getpid()) << "-" << id;
    return out.str();
}

}  // namespace

AsyncAdminBackend::AsyncAdminBackend(
    AdminBackend* inner, const AsyncAdminBackendOptions& options)
    : inner_(inner),
      max_pending_(options.max_pending_operations == 0
                       ? 1
                       : options.max_pending_operations),
      activate_worker_(options.activate_worker),
      retire_worker_(options.retire_worker) {
    worker_ = std::thread(&AsyncAdminBackend::worker_loop, this);
}

AsyncAdminBackend::~AsyncAdminBackend() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    condition_.notify_all();
    worker_.join();
}

DeployOutcome AsyncAdminBackend::submit(
    bool retire, const std::string& application, const std::string& version,
    OperationStatus* status) {
    DeployOutcome outcome;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // max_pending_ bounds RUNNING + queued operations (the count is
        // released only when the worker finishes); a full pool is
        // fail-closed.
        if (pending_count_ >= max_pending_) {
            outcome.error = "admin operation capacity exceeded";
            if (status != nullptr) {
                status->state = OperationState::kFailed;
                status->error = outcome.error;
            }
            return outcome;
        }
        PendingOperation pending;
        pending.retire = retire;
        pending.application = application;
        pending.version = version;
        pending.operation_id = admin_operation_id();
        // The public ID is captured BEFORE the move into the queue: the
        // moved-from object must never be read.
        const std::string public_id = pending.operation_id;
        OperationStatus recorded;
        recorded.operation_id = public_id;
        recorded.version = version;
        recorded.state = OperationState::kValidating;
        registry_[public_id] = recorded;
        pending_count_ += 1;
        queue_.push_back(std::move(pending));
        outcome.ok = true;
        outcome.operation_id = public_id;
        if (status != nullptr) {
            *status = recorded;
        }
    }
    condition_.notify_one();
    return outcome;
}

void AsyncAdminBackend::worker_loop() {
    for (;;) {
        PendingOperation pending;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            condition_.wait(lock, [&]() {
                return stopping_ || !queue_.empty();
            });
            if (stopping_ && queue_.empty()) {
                return;
            }
            pending = std::move(queue_.front());
            queue_.erase(queue_.begin());
        }
        OperationStatus recorded;
        recorded.operation_id = pending.operation_id;
        recorded.version = pending.version;
        recorded.state = OperationState::kValidating;
        bool succeeded = false;
        capsid_worker* claimed_worker = nullptr;
        try {
            if (pending.retire) {
                DeployOutcome outcome = inner_->retire(pending.application,
                                                       &recorded);
                succeeded = outcome.ok;
                if (succeeded) {
                    // The owner reclaims the drained worker after a
                    // successful retire. A throwing callback is caught
                    // below and fails the public operation (never
                    // std::terminate).
                    if (retire_worker_) {
                        retire_worker_(pending.application);
                    }
                }
            } else {
                DeployOutcome outcome = inner_->deploy(
                    pending.application, pending.version, &recorded);
                succeeded = outcome.ok;
                claimed_worker = outcome.worker;
                if (succeeded && claimed_worker != nullptr) {
                    // Explicit ownership handoff: activate_worker returns
                    // true only when it took ownership of the worker.
                    bool activated = false;
                    if (activate_worker_) {
                        activated = activate_worker_(pending.application,
                                                     claimed_worker);
                    }
                    if (!activated) {
                        // Nobody claimed the worker: destroy it and mark
                        // the public operation as a redacted Failed.
                        capsid_worker_destroy(claimed_worker);
                        claimed_worker = nullptr;
                        succeeded = false;
                    }
                }
            }
        } catch (const std::exception&) {
            succeeded = false;
        } catch (...) {
            // A background exception must become a redacted Failed state,
            // never std::terminate.
            succeeded = false;
        }
        if (!succeeded && claimed_worker != nullptr) {
            // An exception after a successful handoff claim must still not
            // leak the worker.
            capsid_worker_destroy(claimed_worker);
            claimed_worker = nullptr;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        // The task is no longer running: the capacity slot opens for the
        // next submission.
        pending_count_ -= 1;
        // The public operation ID is never overwritten by an inner ID; the
        // registry keeps the submitted identity and only the state/error
        // fields reflect the outcome.
        OperationStatus& entry = registry_[pending.operation_id];
        if (succeeded) {
            entry.state = OperationState::kActive;
        } else {
            entry.state = OperationState::kFailed;
            // Static redaction: inner errors never enter the registry.
            entry.error = pending.retire ? "operation failed"
                                         : "deployment failed";
        }
    }
}

DeployOutcome AsyncAdminBackend::deploy(const std::string& application,
                                        const std::string& version,
                                        OperationStatus* status) {
    return submit(false, application, version, status);
}

DeployOutcome AsyncAdminBackend::retire(const std::string& application,
                                        OperationStatus* status) {
    return submit(true, application, std::string(), status);
}

OperationStatus AsyncAdminBackend::operation_status(
    const std::string& operation_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::map<std::string, OperationStatus>::const_iterator found =
        registry_.find(operation_id);
    if (found == registry_.end()) {
        OperationStatus status;
        status.operation_id = operation_id;
        status.state = OperationState::kFailed;
        status.error = "operation not found";
        return status;
    }
    return found->second;
}

std::string AsyncAdminBackend::app_status(const std::string& application) {
    if (inner_ == nullptr) {
        return "{\"active\":false}";
    }
    return inner_->app_status(application);
}

ManagedAdminBackend::ManagedAdminBackend(
    const std::vector<ManagedHostOptions*>& applications)
    : applications_(applications) {}

ManagedHostOptions* ManagedAdminBackend::find_application(
    const std::string& application) {
    for (ManagedHostOptions* options : applications_) {
        if (options != nullptr && options->application == application) {
            return options;
        }
    }
    return nullptr;
}

DeployOutcome ManagedAdminBackend::deploy(const std::string& application,
                                          const std::string& version,
                                          OperationStatus* status) {
    ManagedHostOptions* options = find_application(application);
    if (options == nullptr) {
        DeployOutcome outcome;
        outcome.error = "unknown application";
        if (status != nullptr) {
            status->state = OperationState::kFailed;
            status->error = outcome.error;
        }
        return outcome;
    }
    return managed_deploy(options, version, status);
}

DeployOutcome ManagedAdminBackend::retire(const std::string& application,
                                          OperationStatus* status) {
    ManagedHostOptions* options = find_application(application);
    if (options == nullptr) {
        DeployOutcome outcome;
        outcome.error = "unknown application";
        if (status != nullptr) {
            status->state = OperationState::kFailed;
            status->error = outcome.error;
        }
        return outcome;
    }
    return managed_retire(options, status);
}

OperationStatus ManagedAdminBackend::operation_status(
    const std::string& operation_id) {
    if (applications_.empty() || applications_.front() == nullptr) {
        OperationStatus status;
        status.operation_id = operation_id;
        status.state = OperationState::kFailed;
        status.error = "operation not found";
        return status;
    }
    // The operation registry is process-global; any configured App's
    // options can query it.
    return managed_operation_status(*applications_.front(), operation_id);
}

std::string ManagedAdminBackend::app_status(const std::string& application) {
    ManagedHostOptions* options = find_application(application);
    if (options == nullptr) {
        // Fail closed: an unconfigured App must not masquerade as a legal
        // inactive App. The Admin dispatcher's guarded call converts this
        // into a redacted 500.
        throw std::runtime_error("unknown application");
    }
    return managed_app_status(*options);
}

}  // namespace capsid::host
