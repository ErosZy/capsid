#ifndef CAPSID_HOST_QUARANTINE_DISPATCHER_H
#define CAPSID_HOST_QUARANTINE_DISPATCHER_H

#include "host/managed_host.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace capsid::host {

// Owned by the pool, not by the pending queue. A failed activation can
// destroy its pool while the management thread is waiting for the App
// operation lock; no task ever pins a pool or waits for a pool mutex.
class QuarantineRequest {
public:
    QuarantineRequest(ManagedHostOptions* options, std::string generation,
                      std::function<bool()> still_current);
    std::atomic<bool> completed{false};
    DeployOutcome result() const;
private:
    friend class QuarantineDispatcher;
    void run();
    void finish(DeployOutcome outcome);
    ManagedHostOptions* options_;
    const std::string generation_;
    const std::function<bool()> still_current_;
    mutable std::mutex mutex_;
    DeployOutcome result_;
    std::atomic<bool> submitted_{false};
};

class QuarantineDispatcher {
public:
    QuarantineDispatcher();
    ~QuarantineDispatcher();
    QuarantineDispatcher(const QuarantineDispatcher&) = delete;
    QuarantineDispatcher& operator=(const QuarantineDispatcher&) = delete;
    bool enqueue(const std::shared_ptr<QuarantineRequest>& request);
    // Join producers first. Pending live requests are completed, not
    // dropped on shutdown; expired aborted-generation requests are skipped.
    void stop_and_join();
private:
    void run();
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::weak_ptr<QuarantineRequest>> pending_;
    bool stopping_ = false;
    std::once_flag stop_once_;
    std::thread thread_;
};
} // namespace capsid::host
#endif
