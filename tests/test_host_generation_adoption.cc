// Real-process ownership checks for the Managed pre-warmed fleet handoff.
// Observe only children spawned here; never scan for or reap another worker.
#include "win32_compat.h"
#include "host/generation_pool.h"
#include "worker_exit_assertions.h"

#if !defined(_WIN32)
#include <signal.h>
#include <sys/wait.h>
#endif

#include <cerrno>
#include <chrono>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {
using capsid::host::GenerationPool;
using capsid::host::GenerationPoolOptions;
using capsid::host::WorkerExecutor;
using Clock = std::chrono::steady_clock;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

class Child {
public:
    explicit Child(const char* worker_path) {
        capsid_worker_config config;
        capsid_worker_config_init(&config);
        config.worker_path = worker_path;
        config.strict_sandbox = 0;
        require(capsid_worker_spawn(&config, &worker_) == CAPSID_OK, "spawn actual Worker");
        pid_ = capsid_worker_pid(worker_);
#if defined(_WIN32)
        handle_ = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION |
                                  PROCESS_TERMINATE, FALSE, static_cast<DWORD>(pid_));
        if (!handle_) {
            capsid_worker_destroy(std::exchange(worker_, nullptr));
            throw std::runtime_error("open independent child observation handle");
        }
#endif
    }

    ~Child() {
        // Before transfer, ordinary RAII owns the C handle. Afterwards use
        // only the OS identity: the pool may already have freed the handle.
        capsid_worker_destroy(std::exchange(worker_, nullptr));
        cleanup_survivor();
#if defined(_WIN32)
        CloseHandle(handle_);
#endif
    }

    Child(const Child&) = delete;
    Child& operator=(const Child&) = delete;

    void warm() {
        const std::string bundle =
            "export default {fetch() {return new Response('adopted');}};";
        require(capsid_worker_load_bundle(worker_,
                    reinterpret_cast<const std::uint8_t*>(bundle.data()), bundle.size()) == CAPSID_OK,
                "load actual Worker");
        const auto deadline = Clock::now() + std::chrono::seconds(5);
        while (Clock::now() < deadline) {
            const auto flushed = capsid_worker_flush(worker_);
            require(flushed == CAPSID_OK || flushed == CAPSID_WOULD_BLOCK, "warm flush");
            capsid_event event{};
            event.struct_size = sizeof(event);
            const auto next = capsid_worker_next_event(worker_, &event);
            if (next == CAPSID_OK && event.type == CAPSID_EVENT_READY) return;
            require(next == CAPSID_WOULD_BLOCK ||
                        (next == CAPSID_OK && event.type == CAPSID_EVENT_LOG), "warm READY event");
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        throw std::runtime_error("bounded warm READY deadline");
    }

    capsid_worker* transfer() { return std::exchange(worker_, nullptr); }

    bool reclaimed() {
#if defined(_WIN32)
        const auto waited = WaitForSingleObject(handle_, 0);
        require(waited == WAIT_OBJECT_0 || waited == WAIT_TIMEOUT, "observe child exit");
        DWORD code = STILL_ACTIVE;
        require(GetExitCodeProcess(handle_, &code) != 0, "query actual OS exit code");
        std::cout << "{\"pid\":" << pid_ << ",\"exited\":"
                  << (waited == WAIT_OBJECT_0 ? "true" : "false")
                  << ",\"exitCode\":" << code << "}";
        return waited == WAIT_OBJECT_0;
#else
        int status = 0;
        pid_t waited;
        do { waited = waitpid(static_cast<pid_t>(pid_), &status, WNOHANG); }
        while (waited < 0 && errno == EINTR);
        const int error = waited < 0 ? errno : 0;
        require(waited >= 0 || error == ECHILD, "observe owned child reap");
        // A zombie is a leak too. If this probe reaps it, record a failure;
        // ECHILD alone proves the product already performed its own reap.
        if (waited == static_cast<pid_t>(pid_)) reaped_by_probe_ = true;
        const bool reaped = waited < 0 && error == ECHILD;
        std::cout << "{\"pid\":" << pid_ << ",\"reapedByProduct\":"
                  << (reaped ? "true" : "false")
                  << ",\"reapedByProbe\":" << (reaped_by_probe_ ? "true" : "false") << "}";
        return reaped;
#endif
    }

private:
    void cleanup_survivor() noexcept {
#if defined(_WIN32)
        if (handle_ && WaitForSingleObject(handle_, 0) == WAIT_TIMEOUT) {
            TerminateProcess(handle_, 1);
            WaitForSingleObject(handle_, 2000);
        }
#else
        if (reaped_by_probe_) return;
        int status = 0;
        pid_t waited;
        do { waited = waitpid(static_cast<pid_t>(pid_), &status, WNOHANG); }
        while (waited < 0 && errno == EINTR);
        if (waited != 0) return;
        kill(static_cast<pid_t>(pid_), SIGKILL);
        do { waited = waitpid(static_cast<pid_t>(pid_), &status, 0); }
        while (waited < 0 && errno == EINTR);
#endif
    }

    capsid_worker* worker_ = nullptr;
    std::int64_t pid_ = -1;
#if defined(_WIN32)
    HANDLE handle_ = nullptr;
#else
    bool reaped_by_probe_ = false;
#endif
};

void run(const std::string& mode, const char* worker_path) {
    const bool success = mode == "success";
    const bool null_case = mode == "null-first" || mode == "null-middle" || mode == "null-last";
    require(success || null_case || mode == "count" || mode == "zero" ||
                mode == "identity" || mode == "policy" || mode == "factory", "known scenario");
    const std::size_t count = success || null_case ? 3 : 2;
    const std::size_t null_index = mode == "null-first" ? 0 : mode == "null-middle" ? 1 : 2;
    std::vector<std::unique_ptr<Child>> children;
    std::vector<capsid_worker*> warmed(count, nullptr);
    for (std::size_t i = 0; i < count; ++i) {
        if (!(null_case && i == null_index)) {
            auto child = std::make_unique<Child>(worker_path);
            child->warm();
            // Transfer only after every fixture is ready, so a preparation
            // failure still destroys each handle through its original owner.
            children.push_back(std::move(child));
        }
    }
    GenerationPoolOptions options;
    options.application_id = mode == "identity" ? "" : "adoption-cleanup";
    options.version = "v1";
    options.generation_digest = "sha256:" + std::string(64, 'a');
    options.workers = mode == "zero" ? 0U : mode == "count" ? 3U : static_cast<std::uint32_t>(count);
    std::size_t spawn_calls = 0;
    if (mode != "factory") {
        options.factory = [&](capsid_worker**, std::string* error) {
            ++spawn_calls;
            if (error) *error = "adoption must not respawn";
            return false;
        };
    }
    options.recovery.max_events = mode == "policy" ? 0U : 2U;
    options.recovery.window_ms = 60000;
    options.recovery.backoff_initial_ms = 20;
    options.recovery.backoff_maximum_ms = 1000;
    options.recovery.stable_reset_ms = 60000;
    options.recovery.replacements_concurrent_per_app = 1;
    std::vector<const WorkerExecutor*> adopted;
    options.on_worker_started = [&](const WorkerExecutor* worker) { adopted.push_back(worker); };
    std::size_t drain_callbacks = 0;
    std::vector<capsid::WorkerExitStatus> exits;
    options.on_drain_complete = [&] {
        ++drain_callbacks;
        // Executors still exist at this callback; the sole reaper already
        // published their actual OS status. No independent waiter races it.
        for (const auto* worker : adopted) exits.push_back(worker->exit_status());
    };

    std::string error;
    std::size_t child_index = 0;
    for (std::size_t i = 0; i < count; ++i)
        if (!(null_case && i == null_index)) warmed[i] = children[child_index++]->transfer();
    const auto started = Clock::now();
    auto pool = GenerationPool::create_adopted(std::move(options), std::move(warmed), &error);
    const auto returned_ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count();
    if (pool) pool->stop_and_join();
    bool all_reclaimed = true;
    std::cout << "GENERATION_ADOPTION {\"mode\":\"" << mode << "\",\"returnedPool\":"
              << (pool ? "true" : "false") << ",\"adopted\":" << adopted.size()
              << ",\"respawnCalls\":" << spawn_calls << ",\"returnMs\":" << returned_ms
              << ",\"children\":[";
    for (std::size_t i = 0; i < children.size(); ++i) {
        if (i) std::cout << ',';
        if (!children[i]->reclaimed()) all_reclaimed = false;
    }
    std::cout << "],\"workerExits\":[";
    for (std::size_t i = 0; i < exits.size(); ++i) {
        if (i) std::cout << ',';
        capsid_test::print_worker_exit(exits[i]);
    }
    std::cout << "]}" << std::endl;
    require(static_cast<bool>(pool) == success, "only a complete fleet can succeed: " + error);
    require(spawn_calls == 0, "pre-warmed adoption must not spawn a replacement");
    require(adopted.size() == (success ? count : null_case ? null_index : 0), "exact adopted prefix");
    require(returned_ms < 5000, "adoption/rollback must return within five seconds");
    require(all_reclaimed, "every transferred Worker must already be exited/reaped on return");
    if (success) {
        require(drain_callbacks == 1 && exits.size() == count, "one completed fleet drain");
        for (const auto& status : exits) require(status.clean(), "natural Managed drain must preserve actual OS exit zero");
        pool->stop_and_join();
        require(drain_callbacks == 1, "idempotent drain must not notify twice");
    } else {
        require(!error.empty(), "failure retains its diagnostic");
        require(drain_callbacks == 0, "failed adoption never publishes a completed active generation");
    }
}
}  // namespace

int main(int argc, char** argv) try {
    require(argc == 3, "expected scenario and actual Worker path");
    run(argv[1], argv[2]);
    return 0;
} catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << std::endl;
    return 1;
}
