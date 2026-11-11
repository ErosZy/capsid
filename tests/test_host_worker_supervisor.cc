#include "host/worker_supervisor.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace {
void fail(const char* message) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::fflush(stderr);
    // A failing no-external-signal case has no cooperative cleanup path in
    // the old implementation. This fixture owns no Worker/child process;
    // terminate this failing driver rather than leaving its thread behind.
    std::_Exit(1);
}
}

int main(int argc, char** argv) {
    if (argc != 2) fail("one supervisor lifecycle case is required");
    const std::string mode = argv[1];
    const bool no_external = mode == "local-no-external";
    const bool destructor = mode == "destructor";
    if (mode != "local" && !no_external && !destructor && mode != "external")
        fail("unknown supervisor lifecycle case");

    std::atomic<bool> external_stop{false};
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false, joined = false;
    capsid::host::WorkerSupervisorOptions options;
    options.stop_requested = no_external ? nullptr : &external_stop;
    options.current_pool = [&] {
        std::lock_guard<std::mutex> lock(mutex);
        entered = true;
        changed.notify_all();
        return nullptr;
    };
    auto supervisor = std::make_unique<capsid::host::WorkerSupervisor>(std::move(options));
    {
        std::unique_lock<std::mutex> lock(mutex);
        if (!changed.wait_for(lock, std::chrono::seconds(2), [&] { return entered; }))
            fail("supervisor must enter its real provider before stopping");
    }
    if (mode == "external") {
        external_stop.store(true);
    } else if (!destructor) {
        supervisor->request_stop();
        supervisor->request_stop();  // Public local stop is idempotent.
    }
    std::thread waiter([&] {
        if (destructor) supervisor.reset();
        else supervisor->join();
        std::lock_guard<std::mutex> lock(mutex);
        joined = true;
        changed.notify_all();
    });
    bool stopped_in_time;
    {
        std::unique_lock<std::mutex> lock(mutex);
        stopped_in_time = changed.wait_for(lock, std::chrono::seconds(2), [&] { return joined; });
    }
    if (!stopped_in_time) {
        if (no_external) fail("local stop without an external signal must join");
        // Clean up the old failing thread before reporting its failed local
        // stop/destructor contract; this external change cannot count as PASS.
        external_stop.store(true);
    }
    waiter.join();
    if (!stopped_in_time) fail("local stop/destructor required an external cleanup signal");
    if (mode != "external" && external_stop.load())
        fail("local stop/destructor must leave the external signal false");
    if (!destructor) supervisor->join();  // A completed join is idempotent.
    std::printf("PASS: supervisor %s stops its entered thread with the expected signal\n", mode.c_str());
    return 0;
}
