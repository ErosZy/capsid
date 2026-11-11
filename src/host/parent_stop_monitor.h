#ifndef CAPSID_HOST_PARENT_STOP_MONITOR_H
#define CAPSID_HOST_PARENT_STOP_MONITOR_H

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace capsid::host {

// Optional one-shot parent control: a byte or EOF requests normal shutdown.
// Owns a non-inheritable duplicate; cancellation never blocks in a pipe read.
class ParentStopMonitor {
public:
    static std::unique_ptr<ParentStopMonitor> create(int fd, std::string* error);
    ~ParentStopMonitor();
    ParentStopMonitor(const ParentStopMonitor&) = delete;
    ParentStopMonitor& operator=(const ParentStopMonitor&) = delete;
    bool start(std::function<void()> request_stop, std::string* error);
    void cancel();
    // Read after cancel() joins the watcher.
    const std::string& error() const { return error_; }

private:
    explicit ParentStopMonitor(int fd) : fd_(fd) {}
    void watch(std::function<void()> request_stop);
    int fd_;
    std::atomic<bool> cancelled_{false};
    std::mutex mutex_;
    std::condition_variable cv_;
    std::thread thread_;
    std::string error_;
};

}  // namespace capsid::host
#endif
