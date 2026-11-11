#ifndef CAPSID_HOST_GENERATION_RECLAIMER_H
#define CAPSID_HOST_GENERATION_RECLAIMER_H
#include "host/generation_pool.h"
#include <condition_variable>
#include <deque>
#include <exception>
#include <memory>
#include <mutex>
#include <thread>

namespace capsid::host {
// Completed pools must be joined and released away from their pump thread,
// including the last retired generation when no further admin work arrives.
// The host must join all producers before stop_and_join().
class GenerationReclaimer {
public:
    GenerationReclaimer() : thread_([this] { run(); }) {}
    ~GenerationReclaimer() { stop_and_join(); }
    GenerationReclaimer(const GenerationReclaimer&) = delete;
    GenerationReclaimer& operator=(const GenerationReclaimer&) = delete;

    void enqueue(std::shared_ptr<GenerationPool> pool) {
        if (!pool) return;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopping_) std::terminate(); // producer lifetime contract
            pending_.push_back(std::move(pool));
        }
        cv_.notify_one();
    }

    void stop_and_join() {
        std::call_once(stop_once_, [this] {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                stopping_ = true;
            }
            cv_.notify_one();
            thread_.join();
        });
    }

private:
    void run() {
        for (;;) {
            std::shared_ptr<GenerationPool> pool;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [this] { return stopping_ || !pending_.empty(); });
                if (pending_.empty()) return;
                pool = std::move(pending_.front());
                pending_.pop_front();
            }
            // This reference outlives the pump's completion hook. Its final
            // release therefore cannot move the joining destructor to the pump.
            pool->stop_and_join();
            pool.reset();
        }
    }
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::shared_ptr<GenerationPool>> pending_;
    bool stopping_ = false;
    std::once_flag stop_once_;
    std::thread thread_;
};
} // namespace capsid::host

#endif // CAPSID_HOST_GENERATION_RECLAIMER_H
