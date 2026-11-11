#include "host/quarantine_dispatcher.h"
#include "host/structured_log.h"

#include <algorithm>
#include <exception>
#include <utility>

namespace capsid::host {
QuarantineRequest::QuarantineRequest(ManagedHostOptions* options,
    std::string generation, std::function<bool()> still_current)
    : options_(options), generation_(std::move(generation)),
      still_current_(std::move(still_current)) {}

DeployOutcome QuarantineRequest::result() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return result_;
}

void QuarantineRequest::finish(DeployOutcome outcome) {
    if (!outcome.ok && options_ && options_->log) {
        options_->log->log(LogLane::kControl,
            {.event = log_events::kQuarantine, .app = options_->application,
             .generation = generation_, .result = "fail", .message = outcome.error});
    }
    { std::lock_guard<std::mutex> lock(mutex_); result_ = std::move(outcome); }
    completed.store(true);
}

void QuarantineRequest::run() {
    DeployOutcome outcome;
    try {
        OperationStatus status;
        outcome = managed_quarantine(options_, generation_, &status, still_current_);
    } catch (const std::exception&) {
        outcome.error = "quarantine persistence failed";
    }
    finish(std::move(outcome));
}

QuarantineDispatcher::QuarantineDispatcher() : thread_([this] { run(); }) {}
QuarantineDispatcher::~QuarantineDispatcher() { stop_and_join(); }

bool QuarantineDispatcher::enqueue(const std::shared_ptr<QuarantineRequest>& request) {
    if (!request) return false;
    if (request->submitted_.exchange(true)) return true;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!stopping_) {
            // At most one live request per pool. Prune weak entries on
            // every submission so aborted pools cannot grow a backlog
            // while the consumer is waiting for a running deployment.
            std::erase_if(pending_, [](const auto& entry) { return entry.expired(); });
            pending_.emplace_back(request);
            cv_.notify_one();
            return true;
        }
    }
    DeployOutcome failed;
    failed.error = "quarantine dispatcher stopped";
    request->finish(std::move(failed));
    return false;
}

void QuarantineDispatcher::run() {
    for (;;) {
        std::shared_ptr<QuarantineRequest> request;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return stopping_ || !pending_.empty(); });
            if (pending_.empty()) return;
            request = pending_.front().lock(); pending_.pop_front();
        }
        if (request) request->run();
    }
}

void QuarantineDispatcher::stop_and_join() {
    std::call_once(stop_once_, [this] {
        { std::lock_guard<std::mutex> lock(mutex_); stopping_ = true; }
        cv_.notify_one(); thread_.join();
    });
}
} // namespace capsid::host
