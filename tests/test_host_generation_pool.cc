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
// injection — using /proc on Linux and native Toolhelp/TerminateProcess
// on Windows to observe and terminate this test process's exact worker children.

#include "host/generation_pool.h"
#include "host/generation_reclaimer.h"
#include "worker_exit_assertions.h"

#include "capsid/runtime.h"

#if defined(_WIN32)
#include "win32_compat.h"
#else
#include <dirent.h>
#include <sys/wait.h>
#endif
#include <signal.h>
#if defined(_WIN32)
#include <tlhelp32.h>
#include <cwchar>
#endif
#if defined(_WIN32)
#include "win32_compat.h"
#else
#include <unistd.h>
#endif

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
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

#if defined(_WIN32)
std::wstring native_worker_path;
unsigned native_injected_kills = 0;

std::wstring native_full_path(const std::string& path) {
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        path.c_str(), -1, nullptr, 0);
    require(count > 0, "worker path is valid UTF-8");
    std::vector<wchar_t> input(static_cast<std::size_t>(count));
    require(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        path.c_str(), -1, input.data(), count) == count, "convert worker path");
    std::vector<wchar_t> full(32768);
    const DWORD length = GetFullPathNameW(input.data(), static_cast<DWORD>(full.size()), full.data(), nullptr);
    require(length > 0 && length < full.size(), "resolve full worker path");
    return std::wstring(full.data(), length);
}

std::vector<DWORD> native_worker_children() {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    require(snapshot != INVALID_HANDLE_VALUE, "create native process snapshot");
    PROCESSENTRY32W entry{};
    entry.dwSize = static_cast<DWORD>(sizeof(entry));
    BOOL more = Process32FirstW(snapshot, &entry);
    require(more != FALSE, "read first native process snapshot entry");
    std::vector<DWORD> result;
    for (; more; more = Process32NextW(snapshot, &entry)) {
        if (entry.th32ParentProcessID != GetCurrentProcessId()) continue;
        HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
            FALSE, entry.th32ProcessID);
        if (!process) {
            require(GetLastError() == ERROR_INVALID_PARAMETER,
                "open owned child process for native observation");
            continue;
        }
        const DWORD state = WaitForSingleObject(process, 0);
        require(state == WAIT_OBJECT_0 || state == WAIT_TIMEOUT, "observe owned child liveness");
        if (state == WAIT_TIMEOUT) {
            std::vector<wchar_t> image(32768);
            DWORD length = static_cast<DWORD>(image.size());
            const BOOL queried = QueryFullProcessImageNameW(process, 0, image.data(), &length);
            require(queried != FALSE, "read owned live child image path");
            if (_wcsicmp(image.data(), native_worker_path.c_str()) == 0)
                result.push_back(entry.th32ProcessID);
        }
        require(CloseHandle(process) != FALSE, "close native child observation handle");
    }
    require(GetLastError() == ERROR_NO_MORE_FILES, "native process snapshot completed");
    require(CloseHandle(snapshot) != FALSE, "close native process snapshot handle");
    return result;
}

std::size_t worker_child_count() { return native_worker_children().size(); }

void native_no_worker_children() {
    require(wait_for([] { return worker_child_count() == 0; }, std::chrono::seconds(3)),
        "no owned live worker child survives the completed pool scenario");
}

void native_observer_controls() {
    native_no_worker_children();
    std::vector<wchar_t> command(native_worker_path.begin(), native_worker_path.end());
    command.insert(command.begin(), L'"');
    command.push_back(L'"'); command.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = static_cast<DWORD>(sizeof(startup));
    PROCESS_INFORMATION child{};
    require(CreateProcessW(native_worker_path.c_str(), command.data(), nullptr, nullptr, FALSE,
        CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, nullptr, &startup, &child) != FALSE,
        "create real suspended worker as independent native observer witness");
    const auto observed = native_worker_children();
    require(observed.size() == 1 && observed.front() == child.dwProcessId,
        "snapshot finds exactly the independently created owned worker");
    require(WaitForSingleObject(child.hProcess, 0) == WAIT_TIMEOUT,
        "independent process witness is live before termination");
    constexpr DWORD expected_exit = 0x00cafe01;
    require(TerminateProcess(child.hProcess, expected_exit) != FALSE,
        "terminate only the exact independently owned suspended child");
    require(WaitForSingleObject(child.hProcess, 3000) == WAIT_OBJECT_0,
        "independent native child termination completes");
    DWORD exit_code = 0;
    require(GetExitCodeProcess(child.hProcess, &exit_code) != FALSE && exit_code == expected_exit,
        "native observer sees the exact independent exit code");
    require(CloseHandle(child.hThread) != FALSE && CloseHandle(child.hProcess) != FALSE,
        "close independent child handles");
    native_no_worker_children();
    std::cout << "PASS: native Windows process observer positive/live/dead/ownership controls" << std::endl;
}

void kill_worker_child() {
    const auto children = native_worker_children();
    require(!children.empty(), "find a live owned worker for native Windows kill injection");
    HANDLE process = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
        FALSE, children.front());
    require(process != nullptr, "open exact owned worker for native kill injection");
    require(WaitForSingleObject(process, 0) == WAIT_TIMEOUT,
        "the exact worker is live before native kill injection");
    constexpr DWORD expected_exit = 0x00cafe02;
    require(TerminateProcess(process, expected_exit) != FALSE, "native worker kill injection succeeds");
    require(WaitForSingleObject(process, 3000) == WAIT_OBJECT_0, "native killed worker actually exits");
    DWORD exit_code = 0;
    require(GetExitCodeProcess(process, &exit_code) != FALSE && exit_code == expected_exit,
        "native worker reports the exact injected abnormal exit code");
    require(CloseHandle(process) != FALSE, "close native kill-injection observation handle");
    ++native_injected_kills;
    std::cout << "NATIVE_POOL_KILL pid=" << children.front() << " exit=" << exit_code << std::endl;
}

#elif defined(__linux__)
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
#if defined(__linux__) || defined(_WIN32)
    // The reaped first worker's process must be gone.
    require(worker_child_count() == 0,
            "no worker child survives a failed barrier");
#endif
    std::cout << "PASS: startup failure reaps the started workers"
              << std::endl;
}

#if defined(__linux__) || defined(_WIN32)
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

void test_notification_does_not_own_pool(const std::string& worker_path, bool adopted) {
    struct Gate {
        std::mutex mutex;
        std::condition_variable cv;
        std::atomic<bool> armed{false};
        bool entered = false;
        bool released = false;
    };
    const auto gate = std::make_shared<Gate>();
    auto options = pool_options(worker_path, 1);
    auto lifetime = std::make_shared<int>(1);
    const std::weak_ptr<int> factory_lifetime = lifetime;
    options.factory = [factory = hello_factory(worker_path), lifetime](
        capsid_worker** out, std::string* error) { return factory(out, error); };
    lifetime.reset();
    options.on_executor_notify = [gate] {
        if (!gate->armed.exchange(false)) return;
        std::unique_lock<std::mutex> lock(gate->mutex);
        gate->entered = true;
        gate->cv.notify_all();
        gate->cv.wait(lock, [&] { return gate->released; });
    };
    std::string error;
    std::shared_ptr<GenerationPool> pool;
    if (adopted) {
        capsid_worker* warmed = nullptr;
        require(options.factory(&warmed, &error), "warm notification worker: " + error);
        require(wait_for([&] {
            capsid_event event = {};
            event.struct_size = sizeof(event);
            const auto result = capsid_worker_next_event(warmed, &event);
            require(result == CAPSID_OK || result == CAPSID_WOULD_BLOCK, "warm READY event");
            return result == CAPSID_OK && event.type == CAPSID_EVENT_READY;
        }, std::chrono::seconds(5)), "warm notification worker READY");
        pool = GenerationPool::create_adopted(std::move(options), {warmed}, &error);
    } else pool = GenerationPool::create(std::move(options), &error);
    require(pool != nullptr, "notification test pool: " + error);
    const std::weak_ptr<GenerationPool> weak_pool = pool;
    gate->armed.store(true);
    pool->current_worker()->submit_probe("https://pool.invalid/hello");
    {
        std::unique_lock<std::mutex> lock(gate->mutex);
        require(gate->cv.wait_for(lock, std::chrono::seconds(5),
                                 [&] { return gate->entered; }),
                "executor must reach the paused notification");
    }
    // If the notifier owns a shared_ptr, dropping the control-plane owner
    // would move destruction onto the executor thread, which then waits
    // for its own exit. Detect that ownership while the ordering is fixed.
    const bool notifier_owns_pool = pool.use_count() != 1;
    if (notifier_owns_pool) {
        { std::lock_guard<std::mutex> lock(gate->mutex); gate->released = true; }
        gate->cv.notify_all();
        pool->stop_and_join();
        pool.reset();
        require(false, "executor notification must not retain the pool's last owner");
    }
    std::atomic<bool> destroyed{false};
    std::thread release([owned = std::move(pool), &destroyed]() mutable {
        owned.reset();
        destroyed.store(true);
    });
    const bool destruction_started = wait_for([&] { return weak_pool.expired(); },
                                               std::chrono::seconds(5));
    { std::lock_guard<std::mutex> lock(gate->mutex); gate->released = true; }
    gate->cv.notify_all();
    require(wait_for([&] { return destroyed.load(); }, std::chrono::seconds(5)),
            "abandoned pool must reap and join its paused executor");
    release.join();
    require(destruction_started, "external owner starts pool destruction");
    require(factory_lifetime.expired(), "abandoned pool releases its factory");
    std::cout << "PASS: " << (adopted ? "adopted" : "created")
              << " notification vs final owner release reaps pool/factory"
              << std::endl;
}

#if defined(__linux__) || defined(_WIN32)
void test_final_owner_during_replacement(const std::string& worker_path) {
    struct Gate {
        std::mutex mutex;
        std::condition_variable cv;
        bool entered = false;
        bool released = false;
    };
    const auto gate = std::make_shared<Gate>();
    auto options = pool_options(worker_path, 1);
    auto calls = std::make_shared<std::atomic<unsigned>>(0);
    options.factory = [factory = hello_factory(worker_path), gate, calls](
        capsid_worker** out, std::string* error) {
        if (calls->fetch_add(1) != 0) {
            std::unique_lock<std::mutex> lock(gate->mutex);
            gate->entered = true; gate->cv.notify_all();
            gate->cv.wait(lock, [&] { return gate->released; });
        }
        return factory(out, error);
    };
    std::string error;
    auto pool = GenerationPool::create(std::move(options), &error);
    require(pool != nullptr, "replacement release test: " + error);
    kill_worker_child();
    {
        std::unique_lock<std::mutex> lock(gate->mutex);
        require(gate->cv.wait_for(lock, std::chrono::seconds(5), [&] { return gate->entered; }),
                "replacement must reach the paused factory");
    }
    const bool producer_owns_pool = pool.use_count() != 1;
    if (producer_owns_pool) {
        { std::lock_guard<std::mutex> lock(gate->mutex); gate->released = true; }
        gate->cv.notify_all(); pool->stop_and_join(); pool.reset();
        require(false, "replacement producer must not own the pool it must join");
    }
    const std::weak_ptr<GenerationPool> weak_pool = pool;
    std::atomic<bool> destroyed{false};
    std::thread release([owned = std::move(pool), &destroyed]() mutable {
        owned.reset(); destroyed.store(true);
    });
    const bool started = wait_for([&] { return weak_pool.expired(); }, std::chrono::seconds(5));
    { std::lock_guard<std::mutex> lock(gate->mutex); gate->released = true; }
    gate->cv.notify_all();
    require(wait_for([&] { return destroyed.load(); }, std::chrono::seconds(5)),
            "abandoned pool must join the replacement producer");
    release.join();
    require(started, "control-plane owner destroys the replacing pool");
    std::cout << "PASS: final owner during replacement reaps and joins" << std::endl;
}
#endif


void test_idle_generation_reclamation(const std::string& worker_path, bool adopted, bool stop_pending) {
    struct Gate {
        std::mutex mutex;
        std::condition_variable cv;
        bool entered = false;
        bool release = false;
        std::thread::id callback_thread;
        std::thread::id factory_release_thread;
        std::atomic<unsigned> factory_releases{0};
    };
    struct Lifetime {
        std::shared_ptr<Gate> gate;
        ~Lifetime() {
            gate->factory_release_thread = std::this_thread::get_id();
            gate->factory_releases.fetch_add(1);
        }
    };
    const auto gate = std::make_shared<Gate>();
    capsid::host::GenerationReclaimer reclaimer;
    auto options = pool_options(worker_path, 1);
    auto lifetime = std::make_shared<Lifetime>(); lifetime->gate = gate;
    const std::weak_ptr<Lifetime> factory_lifetime = lifetime;
    options.factory = [factory = hello_factory(worker_path), lifetime](
        capsid_worker** out, std::string* error) { return factory(out, error); };
    lifetime.reset();
    std::shared_ptr<GenerationPool> parked;
    const auto weak_box = std::make_shared<std::weak_ptr<GenerationPool>>();
    options.on_drain_complete = [&, gate, weak_box] {
        auto pin = weak_box->lock();
        require(pin != nullptr, "drain hook pins the parked generation");
        parked.reset();
        reclaimer.enqueue(std::move(pin));
        std::unique_lock<std::mutex> lock(gate->mutex);
        gate->callback_thread = std::this_thread::get_id();
        gate->entered = true; gate->cv.notify_all();
        gate->cv.wait(lock, [&] { return gate->release; });
    };
    std::string error;
    std::shared_ptr<GenerationPool> pool;
    if (adopted) {
        capsid_worker* warmed = nullptr;
        require(options.factory(&warmed, &error), "warm idle reclamation worker: " + error);
        require(wait_for([&] {
            capsid_event event = {}; event.struct_size = sizeof(event);
            const auto result = capsid_worker_next_event(warmed, &event);
            require(result == CAPSID_OK || result == CAPSID_WOULD_BLOCK, "idle reclamation READY event");
            return result == CAPSID_OK && event.type == CAPSID_EVENT_READY;
        }, std::chrono::seconds(5)), "idle reclamation warmed worker READY");
        pool = GenerationPool::create_adopted(std::move(options), {warmed}, &error);
    } else pool = GenerationPool::create(std::move(options), &error);
    require(pool != nullptr, "idle reclamation pool: " + error);
    *weak_box = pool; parked = pool;
    const std::weak_ptr<GenerationPool> weak_pool = pool;
    pool->request_drain(); pool.reset();
    {
        std::unique_lock<std::mutex> lock(gate->mutex);
        require(gate->cv.wait_for(lock, std::chrono::seconds(5), [&] { return gate->entered; }),
                "completion hook must reach the controlled pause");
    }
    require(!weak_pool.expired() && !factory_lifetime.expired(),
            "reclaimer pins pool and factory while the pump callback is running");
    std::atomic<bool> shutdown_done{false};
    std::thread shutdown;
    if (stop_pending) shutdown = std::thread([&] { reclaimer.stop_and_join(); shutdown_done.store(true); });
    { std::lock_guard<std::mutex> lock(gate->mutex); gate->release = true; }
    gate->cv.notify_all();
    // No further enqueue, sweep, deploy or retire occurs before this check.
    const bool reclaimed_while_idle = wait_for([&] {
        return weak_pool.expired() && factory_lifetime.expired() && gate->factory_releases.load() == 1;
    }, std::chrono::seconds(3));
    // Clean up the legacy implementation too before reporting the RED result.
    reclaimer.stop_and_join();
    if (shutdown.joinable()) shutdown.join();
    require(reclaimed_while_idle, "last retired pool and factory must be reclaimed while the host is idle");
    require(gate->factory_releases.load() == 1, "factory is released exactly once");
    require(gate->factory_release_thread != gate->callback_thread, "factory release runs outside the pump thread");
    if (stop_pending) require(shutdown_done.load(), "shutdown joins queued reclamation");
    std::cout << "PASS: idle reclamation " << (adopted ? "adopted" : "created")
              << (stop_pending ? " during shutdown" : " without further admin work") << std::endl;
}

void test_replacement_observer_before_drain(const std::string& worker_path) {
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<const WorkerExecutor*> started;
    std::vector<capsid::WorkerExitStatus> exits;
    bool entered = false, release = false, replacement_exit = false, drained = false;
    auto options = pool_options(worker_path, 1);
    options.on_worker_started = [&](const WorkerExecutor* executor) {
        std::unique_lock<std::mutex> lock(mutex);
        started.push_back(executor);
        if (started.size() == 2) {
            entered = true;
            cv.notify_all();
            cv.wait(lock, [&] { return release; });
        }
    };
    options.on_worker_exited = [&](const WorkerExecutor* executor) {
        std::lock_guard<std::mutex> lock(mutex);
        if (started.size() == 2 && started.back() == executor) replacement_exit = true;
        cv.notify_all();
    };
    options.on_drain_complete = [&] {
        std::lock_guard<std::mutex> lock(mutex);
        for (const auto* executor : started) exits.push_back(executor->exit_status());
        drained = true;
        cv.notify_all();
    };
    std::string error;
    auto pool = GenerationPool::create(std::move(options), &error);
    require(pool != nullptr, "observer drain pool: " + error);
    require(pool->recycle_worker(pool->current_worker()), "recycle starts replacement");
    {
        std::unique_lock<std::mutex> lock(mutex);
        require(cv.wait_for(lock, std::chrono::seconds(10), [&] { return entered; }),
                "replacement observer reaches controlled pause");
    }
    pool->request_drain();
    bool drained_before_observer;
    {
        std::unique_lock<std::mutex> lock(mutex);
        require(cv.wait_for(lock, std::chrono::seconds(5), [&] { return replacement_exit; }),
                "pump processes replacement EXIT during observer pause");
        drained_before_observer = cv.wait_for(lock, std::chrono::seconds(1), [&] { return drained; });
        release = true;
    }
    cv.notify_all();
    pool->stop_and_join();
    require(!drained_before_observer, "drain completion must wait for the replacement observer");
    require(drained && started.size() == 2 && exits.size() == 2,
            "both started workers reach drain completion");
    for (const auto& status : exits) {
        require(capsid_test::natural_exit(status, 0), "observer drain actual OS exit is natural zero");
        std::cout << "OBSERVER_DRAIN_EXIT "; capsid_test::print_worker_exit(status); std::cout << '\n';
    }
    std::cout << "PASS: replacement observer remains live until return" << std::endl;
}

struct CompletedFleet {
    std::mutex mutex;
    std::vector<std::pair<const WorkerExecutor*, std::int64_t>> started;
    std::vector<capsid::WorkerExitStatus> exits;
    void wire(GenerationPoolOptions& options) {
        options.on_worker_started = [this](const auto* executor) {
            std::lock_guard<std::mutex> lock(mutex);
            started.emplace_back(executor, executor->process_id());
        };
        options.on_drain_complete = [this] {
            std::lock_guard<std::mutex> lock(mutex);
            for (const auto& item : started) exits.push_back(item.first->exit_status());
        };
    }
    void verify() {
        require(started.size() == 2 && exits.size() == 2, "both real executors reach drain completion");
        for (std::size_t i = 0; i < exits.size(); ++i) {
            require(started[i].second > 0 && capsid_test::natural_exit(exits[i], 0), "actual natural OS exit zero");
#if !defined(_WIN32)
            int status = 0; pid_t result;
            do { result = waitpid(static_cast<pid_t>(started[i].second), &status, WNOHANG); } while (result < 0 && errno == EINTR);
            require(result == -1 && errno == ECHILD, "sole product reaper collected the exact child");
#endif
            std::cout << "QUARANTINE_POOL_EXIT {\"pid\":" << started[i].second << ",\"status\":";
            capsid_test::print_worker_exit(exits[i]); std::cout << "}\n";
        }
    }
};

void test_quarantine_callback_releases_pool(const std::string& worker_path) {
    std::mutex mutex;
    std::condition_variable cv;
    bool entered = false, release = false, drained = false;
    auto options = pool_options(worker_path, 1);
    CompletedFleet observed; observed.wire(options);
    options.recovery.max_events = 1;
    options.on_quarantine = [&] {
        std::unique_lock<std::mutex> lock(mutex);
        entered = true; cv.notify_all();
        cv.wait(lock, [&] { return release; });
    };
    std::string error;
    auto pool = GenerationPool::create(std::move(options), &error);
    require(pool != nullptr, "quarantine lock-order pool: " + error);
    auto* original = pool->current_worker();
    require(pool->recycle_worker(original), "first recycle starts replacement");
    require(wait_for([&] { return original->exited() && pool->ready_workers() == 1 &&
                                  pool->current_worker() != original; }, std::chrono::seconds(10)),
            "first replacement is real and original is reaped");
    auto* replacement = pool->current_worker();
    std::thread recycle([&] { require(pool->recycle_worker(replacement), "quarantine recycle accepted"); });
    {
        std::unique_lock<std::mutex> lock(mutex);
        require(cv.wait_for(lock, std::chrono::seconds(10), [&] { return entered; }),
                "quarantine callback reaches the controlled pause");
    }
    std::thread drain([&] {
        pool->request_drain();
        { std::lock_guard<std::mutex> lock(mutex); drained = true; }
        cv.notify_all();
    });
    bool returned;
    {
        std::unique_lock<std::mutex> lock(mutex);
        returned = cv.wait_for(lock, std::chrono::seconds(1), [&] { return drained; });
        release = true;
    }
    cv.notify_all(); recycle.join(); drain.join(); pool->stop_and_join();
    require(returned, "quarantine callback must not hold the pool mutex needed by a deployment drain");
    observed.verify();
    std::cout << "PASS: deployment drain proceeds while quarantine callback is paused" << std::endl;
}

void test_retired_exit_before_completion(const std::string& worker_path) {
    std::mutex mutex;
    std::condition_variable cv;
    bool entered = false, release = false, drained = false;
    std::atomic<bool> hold{false}, claimed{false};
    std::thread::id original_thread;
    WorkerExecutor* original = nullptr;
    auto options = pool_options(worker_path, 1);
    CompletedFleet observed; observed.wire(options);
    options.on_executor_notify = [&] {
        std::unique_lock<std::mutex> lock(mutex);
        if (!hold.load()) { original_thread = std::this_thread::get_id(); cv.notify_all(); return; }
        if (std::this_thread::get_id() != original_thread || claimed.load() || original->available() || original->exited()) return;
        claimed.store(true);
        entered = true; cv.notify_all();
        cv.wait(lock, [&] { return release; });
    };
    const auto observe_completion = options.on_drain_complete;
    options.on_drain_complete = [&, observe_completion] {
        observe_completion();
        std::lock_guard<std::mutex> lock(mutex); drained = true; cv.notify_all();
    };
    std::string error;
    auto pool = GenerationPool::create(std::move(options), &error);
    require(pool != nullptr, "retired-exit pool: " + error);
    original = pool->current_worker();
    {
        std::unique_lock<std::mutex> lock(mutex);
        require(cv.wait_for(lock, std::chrono::seconds(10), [&] { return original_thread != std::thread::id{}; }),
                "observe original worker notifier thread before holding its exit");
        hold.store(true);
    }
    require(pool->recycle_worker(original), "recycle the exact controlled worker");
    {
        std::unique_lock<std::mutex> lock(mutex);
        require(cv.wait_for(lock, std::chrono::seconds(10), [&] { return entered; }), "original pauses before actual reap");
    }
    require(wait_for([&] { return pool->ready_workers() == 1 && pool->current_worker() != original; },
                     std::chrono::seconds(10)), "replacement installed while retired executor is held");
    require(!original->exit_status().observed && !original->exited(), "retired OS status is still unknown at the barrier");
    pool->request_drain();
    bool completed_early;
    {
        std::unique_lock<std::mutex> lock(mutex);
        completed_early = cv.wait_for(lock, std::chrono::seconds(1), [&] { return drained; });
        release = true;
    }
    cv.notify_all(); pool->stop_and_join();
    require(!completed_early, "drain completion must wait for every retired executor's actual reap");
    require(drained, "drain completes after the retired executor is released");
    observed.verify();
    std::cout << "PASS: retired executor reap precedes drain completion" << std::endl;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2 && argc != 3) {
        fail("expected capsid-worker path and optional --reclamation");
    }
    const std::string worker_path = argv[1];
#if defined(_WIN32)
    native_worker_path = native_full_path(worker_path);
    native_observer_controls();
#endif
    if (argc == 3) {
        if (std::string(argv[2]) == "--portable") {
            test_notification_does_not_own_pool(worker_path, false);
#if defined(_WIN32)
    native_no_worker_children();
#endif
            test_notification_does_not_own_pool(worker_path, true);
#if defined(_WIN32)
    native_no_worker_children();
#endif
            test_create_and_drain(worker_path);
#if defined(_WIN32)
    native_no_worker_children();
#endif
            test_startup_failure_reaps(worker_path);
#if defined(_WIN32)
    native_no_worker_children();
#endif
            std::cout << "PASS: GenerationPool portable create/drain, notifier lifetime and startup-failure checks" << std::endl;
            return 0;
        }
        if (std::string(argv[2]) == "--quarantine-lock") {
            test_quarantine_callback_releases_pool(worker_path); return 0;
        }
        if (std::string(argv[2]) == "--retired-exit") {
            test_retired_exit_before_completion(worker_path); return 0;
        }
        if (std::string(argv[2]) == "--observer-drain") {
            test_replacement_observer_before_drain(worker_path);
            return 0;
        }
        require(std::string(argv[2]) == "--reclamation", "known pool test selector");
        test_idle_generation_reclamation(worker_path, false, false);
        test_idle_generation_reclamation(worker_path, true, false);
        test_idle_generation_reclamation(worker_path, true, true);
        return 0;
    }
    test_notification_does_not_own_pool(worker_path, false);
#if defined(_WIN32)
    native_no_worker_children();
#endif
    test_notification_does_not_own_pool(worker_path, true);
#if defined(_WIN32)
    native_no_worker_children();
#endif
    test_create_and_drain(worker_path);
#if defined(_WIN32)
    native_no_worker_children();
#endif
    test_startup_failure_reaps(worker_path);
#if defined(_WIN32)
    native_no_worker_children();
#endif
#if defined(__linux__) || defined(_WIN32)
    test_final_owner_during_replacement(worker_path);
#if defined(_WIN32)
    native_no_worker_children();
#endif
    test_n_to_n_minus_1_to_n(worker_path);
#if defined(_WIN32)
    native_no_worker_children();
#endif
    test_zero_ready_503_point(worker_path);
#if defined(_WIN32)
    native_no_worker_children();
#endif
    test_crash_budget_quarantine(worker_path);
#if defined(_WIN32)
    native_no_worker_children();
#endif
    test_drain_during_replacement(worker_path);
#if defined(_WIN32)
    native_no_worker_children();
#endif
#if defined(_WIN32)
    require(native_injected_kills == 7, "all seven actual native Windows worker kills executed");
#endif
    std::cout << "PASS: GenerationPool fleet + replacement (WP-04 §8.2/§8.3)"
              << std::endl;
    return 0;
#else
    // The kill-injection subtests above need /proc pid scan + SIGKILL and
    // are compiled out on non-Linux. Report the missing coverage with the
    // CTest skip code instead of a green pass that asserted nothing (the
    // remaining scenarios above did run).
    std::cerr << "SKIP: kill-injection scenarios need /proc (Linux); "
                 "the create/drain and startup-failure scenarios ran"
              << std::endl;
    return 77;
#endif
}
