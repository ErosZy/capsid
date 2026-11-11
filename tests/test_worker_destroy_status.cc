#include "capsid/runtime.h"
#include "worker_exit_assertions.h"

#if !defined(_WIN32)
#include <sys/wait.h>
#endif

#include <cerrno>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
using Clock = std::chrono::steady_clock;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct Worker {
    capsid_worker* value = nullptr;
    ~Worker() { capsid_worker_destroy(value); }
    capsid::WorkerExitStatus destroy() {
        auto* owned = value;
        value = nullptr;
        return capsid::destroy_worker_with_status(owned);
    }
};

void await_event(capsid_worker* worker, capsid_event_type type) {
    const auto deadline = Clock::now() + std::chrono::seconds(5);
    while (Clock::now() < deadline) {
        const auto flushed = capsid_worker_flush(worker);
        require(flushed == CAPSID_OK || flushed == CAPSID_WOULD_BLOCK, "flush");
        capsid_event event{};
        event.struct_size = sizeof(event);
        const auto next = capsid_worker_next_event(worker, &event);
        if (next == CAPSID_OK && event.type == type) {
            if (type == CAPSID_EVENT_EXIT)
                require(event.status == 0, "IPC EXIT status is unused, including abnormal exits");
            return;
        }
        require(next == CAPSID_WOULD_BLOCK ||
                    (next == CAPSID_OK && event.type == CAPSID_EVENT_LOG),
                "unexpected event while waiting for READY/EXIT");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    throw std::runtime_error("bounded READY/EXIT deadline");
}
}  // namespace

int main(int argc, char** argv) try {
    require(argc == 3, "expected mode and actual Worker path");
    const std::string mode(argv[1]);
    Worker worker;
    if (mode != "null") {
        capsid_worker_config config;
        capsid_worker_config_init(&config);
        config.worker_path = argv[2];
        config.strict_sandbox = 0;
        require(capsid_worker_spawn(&config, &worker.value) == CAPSID_OK, "spawn");
        const std::string source = "export default {fetch() { return new Response('ok'); }};";
        require(capsid_worker_load_bundle(worker.value,
                    reinterpret_cast<const std::uint8_t*>(source.data()), source.size()) == CAPSID_OK,
                "load actual Worker");
        await_event(worker.value, CAPSID_EVENT_READY);
        require((mode == "terminated" ? capsid_worker_terminate(worker.value)
                                      : capsid_worker_shutdown(worker.value)) == CAPSID_OK,
                "request exit");
        await_event(worker.value, CAPSID_EVENT_EXIT);
    }
#if !defined(_WIN32)
    if (mode == "reaped") {
        const auto pid = static_cast<pid_t>(capsid_worker_pid(worker.value));
        int status = 0;
        pid_t waited = 0;
        const auto deadline = Clock::now() + std::chrono::seconds(5);
        do {
            waited = waitpid(pid, &status, WNOHANG);
            require(waited >= 0 || errno == EINTR, "external reap failed");
            if (waited == pid) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } while (Clock::now() < deadline);
        require(waited == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0,
                "external waiter must independently observe exit zero");
    }
#endif
    const auto status = worker.destroy();
    std::cout << "WORKER_DESTROY_STATUS ";
    capsid_test::print_worker_exit(status);
    std::cout << std::endl;
    if (mode == "clean") {
        require(status.clean(), "real shutdown must retain OS exit zero");
    } else if (mode == "terminated") {
        require(status.observed && !status.clean() && !status.destroy_forced && status.wait_error == 0,
                "external termination must retain a non-clean OS result");
#if defined(_WIN32)
        require(status.exit_code == 1 && status.signal == 0, "TerminateProcess exit code");
#else
        require(status.signal == SIGKILL, "SIGKILL exit signal");
#endif
    } else if (mode == "null") {
        require(!status.observed && !status.clean() && !status.destroy_forced && status.wait_error == 0,
                "null worker must not invent exit zero");
#if !defined(_WIN32)
    } else if (mode == "reaped") {
        require(!status.observed && !status.clean() && !status.destroy_forced && status.wait_error == ECHILD,
                "already reaped child must remain unknown without signaling its PID");
#endif
    } else {
        throw std::runtime_error("unknown destroy-status mode");
    }
    return 0;
} catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << std::endl;
    return 1;
}
