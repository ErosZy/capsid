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
#include <unistd.h>
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
      activate_pool_(options.activate_pool),
      activate_generation_(options.activate_generation),
      retire_worker_(options.retire_worker),
      external_stop_(options.external_stop) {
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
        // After a stop the executor rejects submissions synchronously.
        if (stopping_ || (external_stop_ != nullptr &&
                          external_stop_->load())) {
            outcome.error = "admin operation rejected during shutdown";
            if (status != nullptr) {
                status->state = OperationState::kFailed;
                status->error = outcome.error;
            }
            return outcome;
        }
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
                return stopping_ || !queue_.empty() ||
                       (external_stop_ != nullptr &&
                        external_stop_->load());
            });
            // Re-check the stop condition after the wake-up: the signal
            // may have arrived while this thread was waiting.
            const bool stop_requested =
                stopping_ || (external_stop_ != nullptr &&
                              external_stop_->load());
            if (stop_requested) {
                // Cancel path: queued operations are recorded as redacted
                // Failed and dropped, the capacity slots they held are
                // released, and the loop exits. The empty queue is never
                // dereferenced.
                for (PendingOperation& queued : queue_) {
                    OperationStatus& entry = registry_[queued.operation_id];
                    entry.state = OperationState::kFailed;
                    entry.error = queued.retire ? "operation failed"
                                                : "deployment failed";
                }
                pending_count_ -= queue_.size();
                queue_.clear();
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
        DeployOutcome outcome;
        try {
            if (pending.retire) {
                outcome = inner_->retire(pending.application, &recorded);
                succeeded = outcome.ok;
                if (succeeded) {
                    // The owner reclaims the drained pool after a
                    // successful retire. A throwing callback is caught
                    // below and fails the public operation (never
                    // std::terminate).
                    if (retire_worker_) {
                        retire_worker_(pending.application);
                    }
                }
            } else {
                outcome = inner_->deploy(pending.application,
                                         pending.version, &recorded);
                succeeded = outcome.ok;
                if (succeeded && !outcome.workers.empty()) {
                    // Ownership handoff: the whole pool moves to exactly
                    // one callback. When configured, the GENERATION hook
                    // (PR-09c §9.3) is used for EVERY pool size — the
                    // fleet plus the §8.3 replacement factory and the
                    // generation identity, so the data plane wires a pool
                    // even for a single-worker App. Otherwise a
                    // multi-worker pool MUST go through the atomic
                    // activate_pool — the legacy activate_worker would
                    // claim just one process of a bigger pool — while the
                    // single-worker pool keeps the legacy path. A true
                    // return transfers ownership of every worker; anything
                    // else (missing callback, false return, exception)
                    // leaves the pool unclaimed for the tail cleanup to
                    // recycle and marks the public operation as a redacted
                    // Failed.
                    bool activated = false;
                    if (activate_generation_) {
                        activated = activate_generation_(
                            pending.application, outcome.workers,
                            outcome.generation_factory, outcome.version,
                            outcome.generation_digest);
                    } else if (outcome.workers.size() > 1) {
                        if (activate_pool_) {
                            activated = activate_pool_(
                                pending.application, outcome.workers);
                        }
                    } else if (activate_worker_) {
                        activated = activate_worker_(
                            pending.application, outcome.workers[0]);
                    }
                    if (activated) {
                        // Ownership transferred: the pool is no longer ours
                        // (the tail cleanup would double-free).
                        outcome.workers.clear();
                        outcome.worker = nullptr;
                    } else {
                        succeeded = false;
                    }
                }
            }
        } catch (const std::exception&) {
            // A background exception must become a redacted Failed state,
            // never std::terminate.
            succeeded = false;
        } catch (...) {
            succeeded = false;
        }
        if (!succeeded && !outcome.workers.empty()) {
            // An exception or an unclaimed pool must still not leak: every
            // worker of the failed deploy's own new pool is recycled.
            for (capsid_worker* worker : outcome.workers) {
                capsid_worker_destroy(worker);
            }
            outcome.workers.clear();
            outcome.worker = nullptr;
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
    // The permit is bound to the active App slot: a replacement deploy of
    // an App that already holds its slot does not acquire again, and the
    // global capacity is consumed BEFORE any spawn or durable activation.
    bool newly_acquired = true;
    if (capacity != nullptr) {
        newly_acquired = capacity->acquire(application);
        if (!newly_acquired && !capacity->holds(application)) {
            DeployOutcome outcome;
            outcome.error = "worker capacity exceeded";
            if (status != nullptr) {
                status->state = OperationState::kFailed;
                status->error = outcome.error;
            }
            return outcome;
        }
    }
    DeployOutcome outcome = managed_deploy(options, version, status);
    if (capacity != nullptr) {
        if (outcome.ok && !outcome.workers.empty()) {
            // The activated pool HOLDS the slot until its retire; a
            // replacement keeps its existing slot.
            capacity->record_success(application);
        } else if (newly_acquired) {
            // Only a permit acquired BY THIS DEPLOY is returned when it
            // settles without a live worker; a failed replacement leaves
            // the old holder's slot untouched.
            capacity->release(application);
        }
    }
    return outcome;
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
    DeployOutcome outcome = managed_retire(options, status);
    if (outcome.ok && capacity != nullptr) {
        // The retired worker's slot returns to the global pool.
        capacity->release(application);
    }
    return outcome;
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
