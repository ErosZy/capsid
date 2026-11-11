#include "win32_compat.h"
#include "host/process_snapshot.h"
#include "host/routing_snapshot.h"
#include "worker_exit_assertions.h"

#if !defined(_WIN32)
#include <sys/wait.h>
#endif
#include <atomic>
#include <cerrno>
#include <chrono>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
using namespace capsid::host;
using Clock = std::chrono::steady_clock;
void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
bool wait_for(const std::function<bool()>& predicate) {
    const auto deadline = Clock::now() + std::chrono::seconds(15);
    while (Clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return predicate();
}

struct Fleet {
    std::mutex mutex;
    std::vector<const WorkerExecutor*> started;
    std::vector<std::int64_t> pids;
    std::vector<capsid::WorkerExitStatus> exits;
#if defined(_WIN32)
    std::vector<HANDLE> handles;
#endif
    std::shared_ptr<GenerationPool> pool;
    Fleet(const char* worker_path, const std::string& app, const std::string& version) {
        GenerationPoolOptions options;
        options.application_id = app;
        options.version = version;
        options.generation_digest = "sha256:" + std::string(64, version.back());
        options.workers = 1;
        options.recovery.max_events = 4;
        options.recovery.window_ms = 60000;
        options.recovery.backoff_initial_ms = 20;
        options.recovery.backoff_maximum_ms = 100;
        options.recovery.stable_reset_ms = 60000;
        options.recovery.jitter_basis_points = 0;
        options.recovery.replacements_concurrent_per_app = 1;
        options.factory = [worker_path, version](capsid_worker** out, std::string* error) {
            capsid_worker_config config;
            capsid_worker_config_init(&config);
            config.worker_path = worker_path;
            config.strict_sandbox = 0;
            if (capsid_worker_spawn(&config, out) != CAPSID_OK) {
                *error = "spawn real observation Worker"; return false;
            }
            const std::string bundle = "export default {fetch(){return new Response('" + version + "');}};";
            if (capsid_worker_load_bundle(*out, reinterpret_cast<const std::uint8_t*>(bundle.data()),
                                         bundle.size()) != CAPSID_OK || capsid_worker_flush(*out) != CAPSID_OK) {
                capsid_worker_destroy(*out); *out = nullptr;
                *error = "load observation bundle"; return false;
            }
            return true;
        };
        options.on_worker_started = [this](const WorkerExecutor* executor) {
            std::lock_guard<std::mutex> lock(mutex);
            started.push_back(executor);
            pids.push_back(executor->process_id());
#if defined(_WIN32)
            handles.push_back(OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
                                          FALSE, static_cast<DWORD>(pids.back())));
#endif
        };
        options.on_drain_complete = [this] {
            std::lock_guard<std::mutex> lock(mutex);
            for (std::size_t i = 0; i < started.size(); ++i) {
                require(started[i]->process_id() == pids[i], "cached identity survives the actual reaper");
                exits.push_back(started[i]->exit_status());
            }
        };
        std::string error;
        pool = GenerationPool::create(std::move(options), &error);
        require(pool != nullptr, "create observation fleet: " + error);
    }
    ~Fleet() {
        pool->stop_and_join();
#if defined(_WIN32)
        for (auto handle : handles) if (handle) CloseHandle(handle);
#endif
    }
    std::int64_t pid() { std::lock_guard<std::mutex> lock(mutex); return pids.back(); }
    void verify() {
        pool->stop_and_join();
        require(exits.size() == pids.size(), "every observed Worker has an actual exit status");
        for (std::size_t i = 0; i < pids.size(); ++i) {
            require(capsid_test::natural_exit(exits[i], 0), "observation Worker exits naturally with OS code zero");
#if defined(_WIN32)
            require(handles[i] && WaitForSingleObject(handles[i], 0) == WAIT_OBJECT_0, "independent OS handle exited");
            DWORD code = STILL_ACTIVE;
            require(GetExitCodeProcess(handles[i], &code) && code == 0, "independent OS exit code zero");
#else
            int status = 0; pid_t result;
            do { result = waitpid(static_cast<pid_t>(pids[i]), &status, WNOHANG); } while (result < 0 && errno == EINTR);
            require(result == -1 && errno == ECHILD, "sole product reaper already collected this exact Worker");
#endif
            std::cout << "PROCESS_OBSERVATION_EXIT {\"pid\":" << pids[i] << ",\"status\":";
            capsid_test::print_worker_exit(exits[i]); std::cout << "}\n";
        }
    }
};

void response(GenerationPool& pool, const std::string& expected) {
    auto* executor = pool.pick_worker();
    require(executor != nullptr, "published worker is schedulable");
    struct Reply { std::mutex mutex; unsigned heads = 0, ends = 0; bool failed = false; std::string body; };
    auto reply = std::make_shared<Reply>();
    pool.set_event_sink([reply, executor](const WorkerExecutor* source, WorkerEvent event) {
        if (source != executor) return;
        std::lock_guard<std::mutex> lock(reply->mutex);
        using T = WorkerEvent::Type;
        if (event.type == T::kExit || event.type == T::kError) reply->failed = true;
        if (event.request_id != 1) return;
        if (event.type == T::kResponseHead) { ++reply->heads; if (event.status != 200) reply->failed = true; }
        else if (event.type == T::kResponseBody) {
            if (reply->heads != 1 || reply->ends || reply->body.size() + event.body.size() > 4096) reply->failed = true;
            else reply->body.append(event.body.begin(), event.body.end());
        } else if (event.type == T::kResponseEnd) { ++reply->ends; if (reply->heads != 1) reply->failed = true; }
        else if (event.type == T::kRequestFailure || event.type == T::kRequestTimeout) reply->failed = true;
    });
    Command begin;
    begin.type = CommandType::kBeginRequest; begin.request_id = 1;
    begin.method = "GET"; begin.url = "https://observation.invalid/"; begin.end_request = true;
    executor->submit(std::move(begin));
    Command credit;
    credit.type = CommandType::kGrantResponseCredit; credit.request_id = 1; credit.credit = 4096;
    executor->submit(std::move(credit));
    const bool complete = wait_for([&] { std::lock_guard<std::mutex> lock(reply->mutex); return reply->ends || reply->failed; });
    pool.set_event_sink({});
    std::lock_guard<std::mutex> lock(reply->mutex);
    require(complete && !reply->failed && reply->heads == 1 && reply->ends == 1 && reply->body == expected,
            "actual complete 200 response from the observed generation");
    std::cout << "PROCESS_OBSERVATION_RESPONSE " << expected << '\n';
}

void publication(const char* worker) {
    auto routing = std::make_shared<RoutingTable>();
    auto observe = process_snapshot_provider([routing] { return static_cast<pid_t>(routing->worker_process_id()); });
    require(observe().worker_pid == 0, "empty route has no observed process");
    Fleet old(worker, "alpha", "v1");
    auto old_route = RoutingSnapshot::build({{"alpha", old.pool}});
    routing->publish(old_route);
    require(observe().worker_pid == static_cast<std::uint64_t>(old.pid()), "committed route selects old process");
    response(*old.pool, "v1");
    Fleet aborted(worker, "alpha", "v2");
    const auto aborted_snapshot = RoutingSnapshot::build({{"alpha", aborted.pool}});
    require(observe().worker_pid == static_cast<std::uint64_t>(old.pid()), "prepared generation cannot change metrics");
    aborted.verify();
    require(observe().worker_pid == static_cast<std::uint64_t>(old.pid()), "aborted generation cannot clear serving metrics");
    require(aborted_snapshot->find("alpha") == aborted.pool, "aborted view was built but never published");
    Fleet next(worker, "alpha", "v3");
    Fleet other(worker, "beta", "v4");
    auto next_route = RoutingSnapshot::build({{"alpha", next.pool}, {"beta", other.pool}});
    std::atomic<bool> stop{false}, invalid{false};
    std::atomic<unsigned> samples{0};
    const auto old_pid = static_cast<std::uint64_t>(old.pid()), next_pid = static_cast<std::uint64_t>(next.pid());
    std::thread reader([&] {
        while (!stop.load()) {
            const auto pid = observe().worker_pid;
            if (pid != old_pid && pid != next_pid) invalid.store(true);
            ++samples;
        }
    });
    require(wait_for([&] { return samples.load() != 0; }), "concurrent metrics reader actually runs");
    for (unsigned i = 0; i < 200; ++i) routing->publish(i % 2 ? old_route : next_route);
    routing->publish(next_route); stop.store(true); reader.join();
    require(!invalid.load(), "concurrent render selects only a pinned published generation");
    old.verify();
    require(observe().worker_pid == next_pid, "old generation drain cannot clear the committed replacement");
    response(*next.pool, "v3");
    routing->publish(RoutingSnapshot::build({{"alpha", nullptr}, {"beta", other.pool}}));
    next.verify();
    require(observe().worker_pid == static_cast<std::uint64_t>(other.pid()), "retire falls back to another live App");
    response(*other.pool, "v4");
    other.verify();
    require(observe().worker_pid == 0, "dead published pool cannot supply a stale PID");
    routing->publish(RoutingSnapshot::build({{"alpha", nullptr}, {"beta", nullptr}}));
    require(observe().worker_pid == 0, "tombstones have no process metrics");
}

void replacement(const char* worker) {
    Fleet fleet(worker, "alpha", "v1");
    auto routing = std::make_shared<RoutingTable>();
    routing->publish(RoutingSnapshot::build({{"alpha", fleet.pool}}));
    auto observe = process_snapshot_provider([routing] { return static_cast<pid_t>(routing->worker_process_id()); });
    auto* old = fleet.pool->current_worker();
    const auto old_pid = old->process_id();
    require(fleet.pool->recycle_worker(old), "health recycle accepted");
    require(wait_for([&] {
        std::lock_guard<std::mutex> lock(fleet.mutex);
        return fleet.pids.size() == 2 && fleet.pool->ready_workers() == 1 && old->exited();
    }), "replacement READY and original actually reaped");
    require(fleet.pid() != old_pid && old->process_id() == old_pid, "replacement has distinct immutable process identity");
    require(observe().worker_pid == static_cast<std::uint64_t>(fleet.pid()), "committed pool observes its live replacement");
    response(*fleet.pool, "v1");
    fleet.verify();
    require(observe().worker_pid == 0, "completed pool cannot retain replacement PID");
}
} // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 3, "expected mode and real Worker path");
        const std::string mode = argv[1];
        if (mode == "publication") publication(argv[2]);
        else if (mode == "replacement") replacement(argv[2]);
        else throw std::runtime_error("unknown observation scenario");
        std::cout << "PASS: process observation " << mode << std::endl;
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << std::endl; return 1;
    }
}
