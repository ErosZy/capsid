// WP-04 PR-07 (spec §8.2/§8.3): GenerationPool — one generation of one
// App's worker fleet — and worker replacement.
//
// PR-07 gate (spec §8.4): N→N-1→N replacement, retire/drain races with a
// replacement in flight, poisoned EXIT never misjudged as healthy, new
// requests only ever enter READY workers (0 ready → pick_worker() returns
// nullptr — the caller's 503 point), and the SingleWorkerServer suite as
// the extraction regression gate (run separately).
//
// The pool's pump owns every slot's event drain, so this test observes
// request completion through the pool's inflight counter (begin + an
// upfront credit grant → RESPONSE_END → inflight returns to 0) instead of
// reading events directly. Worker death is injected with SIGKILL — the
// poisoned-exit variety, identical to the admission suite's fault
// injection — by scanning /proc for this test process's worker children
// (Linux only; the kill-based tests skip elsewhere).

#include "host/generation_pool.h"

#include "capsid/runtime.h"

#if defined(_WIN32)
#include "win32_compat.h"
#else
#include <dirent.h>
#endif
#include <signal.h>
#if defined(_WIN32)
#include "win32_compat.h"
#else
#include <unistd.h>
#endif

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

using capsid::host::Command;
using capsid::host::CommandType;
using capsid::host::GenerationPool;
using capsid::host::GenerationPoolOptions;
using capsid::host::WorkerExecutor;
using capsid::host::WorkerRecoveryPolicy;

[[noreturn]] void fail(const std::string& message) {
    std::cerr << "FAIL " << message << std::endl;
    std::exit(1);
}

void require(bool condition, const std::string& message) {
    if (!condition) {
        fail(message);
    }
}

// Polls `predicate` until it holds or the deadline expires; returns whether
// it held. Never sleeps longer than 10ms per poll.
bool wait_for(const std::function<bool()>& predicate,
              std::chrono::milliseconds timeout) {
    const auto deadline =
        std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return predicate();
}

const char* kBundle =
    "export default { async fetch(request) {"
    " return new Response('hello-pool'); } };\n";

WorkerExecutor::WorkerFactory hello_factory(const std::string& worker_path) {
    return [worker_path](capsid_worker** out,
                         std::string* factory_error) -> bool {
        capsid_worker_config config;
        capsid_worker_config_init(&config);
        config.worker_path = worker_path.c_str();
        config.request_timeout_ms = 2000;
        capsid_worker* worker = nullptr;
        if (capsid_worker_spawn(&config, &worker) != CAPSID_OK) {
            *factory_error = "worker spawn failed";
            return false;
        }
        if (capsid_worker_load_bundle(
                worker, reinterpret_cast<const std::uint8_t*>(kBundle),
                std::char_traits<char>::length(kBundle)) != CAPSID_OK) {
            capsid_worker_destroy(worker);  // the factory cleans its own
            *factory_error = "bundle load failed";
            return false;
        }
        if (capsid_worker_flush(worker) != CAPSID_OK) {
            capsid_worker_destroy(worker);
            *factory_error = "bundle flush failed";
            return false;
        }
        *out = worker;
        return true;
    };
}

// Fast, deterministic recovery: 20ms initial backoff, no jitter, budget of
// two unexpected exits before quarantine, no stability reset during a test.
WorkerRecoveryPolicy test_policy() {
    WorkerRecoveryPolicy policy;
    policy.max_events = 2;
    policy.window_ms = 60000;
    policy.backoff_initial_ms = 20;
    policy.backoff_maximum_ms = 1000;
    policy.jitter_basis_points = 0;
    policy.stable_reset_ms = 60000;
    policy.replacements_concurrent_per_app = 1;
    return policy;
}

GenerationPoolOptions pool_options(const std::string& worker_path,
                                   std::uint32_t workers) {
    GenerationPoolOptions options;
    options.application_id = "demopool";
    options.version = "v1";
    options.generation_digest =
        "sha256:" + std::string(64, 'a');  // valid generation identifier
    options.workers = workers;
    options.factory = hello_factory(worker_path);
    options.recovery = test_policy();
    return options;
}

// Submits a bodyless begin plus an upfront credit grant and waits until the
// pool's inflight returns to 0 — the response completed end-to-end. The
// pump owns the event drain, so the inflight counter is the only observable
// completion signal at this layer.
void round_trip(GenerationPool& pool, std::uint64_t request_id) {
    WorkerExecutor* worker = pool.pick_worker();
    require(worker != nullptr,
            "round_trip: no READY worker to serve the request");
    Command begin;
    begin.type = CommandType::kBeginRequest;
    begin.request_id = request_id;
    begin.method = "GET";
    begin.url = "https://pool.invalid/hello";
    begin.end_request = true;  // bodyless fusion: begin + end in one frame
    worker->submit(std::move(begin));
    Command grant;
    grant.type = CommandType::kGrantResponseCredit;
    grant.request_id = request_id;
    grant.credit = 4096;  // covers the whole 15-byte body upfront
    worker->submit(std::move(grant));
    require(wait_for([&] { return pool.inflight() == 0; },
                     std::chrono::seconds(15)),
            "round trip did not complete within 15s");
}

#if defined(__linux__)
// The pool spawns the workers as OUR direct children; scan /proc for the
// children of this test process (same idiom as the admission suite).
pid_t find_worker_child_pid() {
    const pid_t self = getpid();
    DIR* directory = opendir("/proc");
    require(directory != nullptr, "cannot open /proc to find the worker");
    pid_t found = -1;
    struct dirent* entry = nullptr;
    while ((entry = readdir(directory)) != nullptr) {
        if (entry->d_name[0] < '0' || entry->d_name[0] > '9') {
            continue;
        }
        char path[320];  // NAME_MAX (255) + "/proc/" + "/stat" + NUL
        std::snprintf(path, sizeof(path), "/proc/%s/stat", entry->d_name);
        FILE* file = std::fopen(path, "r");
        if (file == nullptr) {
            continue;
        }
        char comm[256];
        long ppid = -1;
        // Format: pid (comm) state ppid ... — see the admission suite for
        // the scan-format notes.
        const int scanned = std::fscanf(
            file, "%*d %255[^)]%*c %*c %ld", comm, &ppid);
        std::fclose(file);
        if (scanned == 2 && ppid == static_cast<long>(self)) {
            found = static_cast<pid_t>(std::strtol(entry->d_name, nullptr, 10));
            break;
        }
    }
    closedir(directory);
    return found;
}

std::size_t worker_child_count() {
    const pid_t self = getpid();
    DIR* directory = opendir("/proc");
    require(directory != nullptr, "cannot open /proc to count workers");
    std::size_t count = 0;
    struct dirent* entry = nullptr;
    while ((entry = readdir(directory)) != nullptr) {
        if (entry->d_name[0] < '0' || entry->d_name[0] > '9') {
            continue;
        }
        char path[320];
        std::snprintf(path, sizeof(path), "/proc/%s/stat", entry->d_name);
        FILE* file = std::fopen(path, "r");
        if (file == nullptr) {
            continue;
        }
        char comm[256];
        long ppid = -1;
        const int scanned = std::fscanf(
            file, "%*d %255[^)]%*c %*c %ld", comm, &ppid);
        std::fclose(file);
        if (scanned == 2 && ppid == static_cast<long>(self)) {
            ++count;
        }
    }
    closedir(directory);
    return count;
}

void kill_worker_child() {
    const pid_t worker_pid = find_worker_child_pid();
    require(worker_pid > 0, "cannot find the live worker child process");
    require(kill(worker_pid, SIGKILL) == 0, "cannot SIGKILL the worker");
}
#endif  // __linux__

void test_create_and_drain(const std::string& worker_path) {
    std::string error;
    std::shared_ptr<GenerationPool> pool =
        GenerationPool::create(pool_options(worker_path, 2), &error);
    require(pool != nullptr, "create N=2: " + error);
    // N→READY barrier: both workers READY, identity wired, active state.
    require(pool->application_id() == "demopool", "application_id");
    require(pool->version() == "v1", "version");
    require(pool->generation_digest() == "sha256:" + std::string(64, 'a'),
            "generation_digest");
    require(pool->state() == GenerationPool::State::kActive, "active");
    require(pool->ready_workers() == 2, "both workers READY");
    require(pool->inflight() == 0, "inflight starts at 0");
    // Least-loaded with a tie breaks to the lowest slot: slot 0.
    WorkerExecutor* first = pool->pick_worker();
    require(first != nullptr, "pick returns a worker");
    round_trip(*pool, 10);
    // Draining rejects new requests: pick goes null before the drain even
    // completes, and wait() reaps everything.
    pool->request_drain();
    require(pool->pick_worker() == nullptr,
            "draining pool rejects new requests");
    require(pool->wait(&error), "wait: " + error);
    require(pool->state() == GenerationPool::State::kDead, "dead");
    require(pool->ready_workers() == 0, "no ready workers after drain");
    require(pool->pick_worker() == nullptr,
            "dead pool never routes");
    require(pool->inflight() == 0, "inflight 0 after drain");
    std::cout << "PASS: N→READY barrier, least-loaded pick, drain/reap"
              << std::endl;
}

void test_startup_failure_reaps(const std::string& worker_path) {
    // The factory rejects the SECOND spawn: the pool must abort the
    // barrier, reap the first worker and leave nothing behind.
    std::string error;
    GenerationPoolOptions options = pool_options(worker_path, 2);
    int spawn_calls = 0;
    options.factory = [&](capsid_worker** out, std::string* factory_error) {
        ++spawn_calls;
        if (spawn_calls == 2) {
            *factory_error = "injected second-spawn failure";
            return false;
        }
        return hello_factory(worker_path)(out, factory_error);
    };
    std::shared_ptr<GenerationPool> pool =
        GenerationPool::create(options, &error);
    require(pool == nullptr, "create must fail on a partial barrier");
    require(spawn_calls == 2, "the factory must be called exactly twice");
    require(error.find("2/2") != std::string::npos,
            "error names the failing slot: " + error);
#if defined(__linux__)
    // The reaped first worker's process must be gone.
    require(worker_child_count() == 0,
            "no worker child survives a failed barrier");
#endif
    std::cout << "PASS: startup failure reaps the started workers"
              << std::endl;
}

#if defined(__linux__)
void test_n_to_n_minus_1_to_n(const std::string& worker_path) {
    std::string error;
    std::shared_ptr<GenerationPool> pool =
        GenerationPool::create(pool_options(worker_path, 2), &error);
    require(pool != nullptr, "create N=2: " + error);
    round_trip(*pool, 20);
    // Fault injection: SIGKILL one worker (poisoned exit, exactly what the
    // audit's worker-death path exercises).
    kill_worker_child();
    // §8.3: EXIT removes the worker from the READY set immediately — the
    // pool serves at N-1.
    require(wait_for([&] { return pool->ready_workers() == 1; },
                     std::chrono::seconds(15)),
            "ready set must drop to N-1 after the kill");
    require(pool->state() == GenerationPool::State::kActive,
            "the generation stays active at N-1");
    round_trip(*pool, 21);  // the survivor serves
    // Replacement: the fleet returns to N (§8.3), then serves again.
    require(wait_for([&] { return pool->ready_workers() == 2; },
                     std::chrono::seconds(15)),
            "replacement must bring the fleet back to N");
    round_trip(*pool, 22);
    pool->request_drain();
    require(pool->wait(&error), "wait: " + error);
    std::cout << "PASS: N→N-1→N replacement" << std::endl;
}

void test_zero_ready_503_point(const std::string& worker_path) {
    std::string error;
    std::shared_ptr<GenerationPool> pool =
        GenerationPool::create(pool_options(worker_path, 1), &error);
    require(pool != nullptr, "create N=1: " + error);
    WorkerExecutor* original = pool->pick_worker();
    require(original != nullptr, "initial pick returns the worker");
    round_trip(*pool, 30);
    kill_worker_child();
    require(wait_for([&] { return pool->ready_workers() == 0; },
                     std::chrono::seconds(15)),
            "ready set must drop to 0 after the kill");
    // §8.3: at 0 ready the pool routes nowhere (the caller's 503 point) and
    // never serves the dead worker; the generation is still active, so a
    // replacement is due.
    require(pool->pick_worker() == nullptr, "pick must be null at 0 ready");
    require(pool->state() == GenerationPool::State::kActive,
            "active with a replacement pending");
    // The replacement comes READY and becomes the ONLY routing target —
    // new requests enter the replacement, never the dead worker.
    require(wait_for([&] { return pool->ready_workers() == 1; },
                     std::chrono::seconds(15)),
            "replacement must reach READY");
    WorkerExecutor* replacement = pool->pick_worker();
    require(replacement != nullptr, "pick returns the replacement");
    require(replacement != original,
            "new requests only enter the replacement, never the dead "
            "worker (the retired executor is a different object)");
    round_trip(*pool, 31);  // the replacement serves
    pool->request_drain();
    require(pool->wait(&error), "wait: " + error);
    std::cout << "PASS: 0-ready 503 point, replacement takes the traffic"
              << std::endl;
}

void test_crash_budget_quarantine(const std::string& worker_path) {
    std::string error;
    std::shared_ptr<GenerationPool> pool =
        GenerationPool::create(pool_options(worker_path, 1), &error);
    require(pool != nullptr, "create N=1: " + error);
    // Two kills fit the budget (max_events=2): each is replaced.
    for (int round = 0; round < 2; ++round) {
        require(wait_for([&] { return pool->ready_workers() == 1; },
                         std::chrono::seconds(15)),
                "worker must be READY before the next kill");
        kill_worker_child();
        require(wait_for([&] { return pool->ready_workers() == 0; },
                         std::chrono::seconds(15)),
                "kill must drop the ready set");
        require(wait_for([&] { return pool->ready_workers() == 1; },
                         std::chrono::seconds(15)),
                "within-budget exit must be replaced");
    }
    // The third exit exceeds the budget: quarantine suppresses the
    // replacement — a bounded negative probe (well beyond the 20ms backoff
    // plus a full spawn) must observe no recovery.
    require(wait_for([&] { return pool->ready_workers() == 1; },
                     std::chrono::seconds(15)),
            "worker READY before the budget-exceeding kill");
    kill_worker_child();
    require(wait_for([&] { return pool->ready_workers() == 0; },
                     std::chrono::seconds(15)),
            "kill must drop the ready set");
    require(!wait_for([&] { return pool->ready_workers() == 1; },
                      std::chrono::milliseconds(1500)),
            "quarantine must suppress the replacement after the budget "
            "is exhausted");
    require(pool->pick_worker() == nullptr, "quarantined pool routes nowhere");
    pool->request_drain();
    require(pool->wait(&error), "wait: " + error);
    std::cout << "PASS: crash budget begins quarantine (no replacement)"
              << std::endl;
}

void test_drain_during_replacement(const std::string& worker_path) {
    // Host-shutdown vs replacement race (§8.4): kill the worker, then stop
    // the pool while the replacement is still in flight. Whichever
    // interleaving wins, the pool must drain completely without a crash or
    // a hung join.
    std::string error;
    GenerationPoolOptions options = pool_options(worker_path, 1);
    // Slow down the replacement's spawn so the drain lands mid-spawn with
    // high probability (deterministic timing control for the race, not a
    // pass-forcing sleep).
    options.recovery.backoff_initial_ms = 1;
    std::atomic<bool> slow_spawn{false};
    const std::string slow_path = worker_path;
    options.factory = [&](capsid_worker** out, std::string* factory_error) {
        if (slow_spawn.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
        }
        const bool ok = hello_factory(slow_path)(out, factory_error);
        return ok;
    };
    std::shared_ptr<GenerationPool> pool = GenerationPool::create(options, &error);
    require(pool != nullptr, "create N=1: " + error);
    round_trip(*pool, 40);
    slow_spawn.store(true, std::memory_order_relaxed);
    kill_worker_child();
    require(wait_for([&] { return pool->ready_workers() == 0; },
                     std::chrono::seconds(15)),
            "ready set must drop to 0 after the kill");
    // The replacement is scheduled with a 1ms backoff — by the time the
    // drain lands it is either mid-spawn or about to be. Both are the
    // documented race.
    pool->request_drain();
    require(pool->wait(&error), "drain must complete with a replacement "
                                "in flight: " + error);
    require(pool->state() == GenerationPool::State::kDead, "dead");
    require(pool->ready_workers() == 0, "no worker survives the drain");
    std::cout << "PASS: host shutdown vs replacement race drains cleanly"
              << std::endl;
}
#endif  // __linux__

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        fail("expected capsid-worker path");
    }
    const std::string worker_path = argv[1];
    test_create_and_drain(worker_path);
    test_startup_failure_reaps(worker_path);
#if defined(__linux__)
    test_n_to_n_minus_1_to_n(worker_path);
    test_zero_ready_503_point(worker_path);
    test_crash_budget_quarantine(worker_path);
    test_drain_during_replacement(worker_path);
#else
    std::cout << "SKIP: kill-injection tests need /proc (Linux)" << std::endl;
#endif
    std::cout << "PASS: GenerationPool fleet + replacement (WP-04 §8.2/§8.3)"
              << std::endl;
    return 0;
}
