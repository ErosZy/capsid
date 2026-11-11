#include "host/worker_supervisor.h"
#include "worker_exit_assertions.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace {
using namespace capsid::host;
using namespace std::chrono_literals;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

// The same fixture also builds against the former borrowed-pointer
// provider, so its RED result tests runtime behavior rather than a compile
// error caused by the ownership interface change.
template <typename Provider>
auto selected_pool(const Provider&, const std::shared_ptr<GenerationPool>& pool) {
    if constexpr (std::is_pointer_v<std::invoke_result_t<Provider>>) {
        return pool.get();
    } else {
        return pool;
    }
}

struct HealthReadGate {
    std::mutex mutex;
    std::condition_variable cv;
    unsigned entered = 0;
    unsigned released = 0;
    bool all_released = false;
    bool timed_out = false;
    std::vector<std::string> generations;

    HealthCheckConfig read(const std::string& generation) {
        std::unique_lock<std::mutex> lock(mutex);
        generations.push_back(generation);
        const unsigned ticket = ++entered;
        cv.notify_all();
        if (!cv.wait_for(lock, 10s, [&] { return all_released || released >= ticket; })) {
            timed_out = true;
        }
        return HealthCheckConfig{true, "/health", 2000};
    }
    void wait(unsigned ticket) {
        std::unique_lock<std::mutex> lock(mutex);
        require(cv.wait_for(lock, 5s, [&] { return entered >= ticket; }),
                "supervisor must enter the controlled health reader");
        require(!timed_out, "health reader barrier must not expire");
    }
    void release(unsigned ticket) {
        std::lock_guard<std::mutex> lock(mutex);
        released = ticket;
        cv.notify_all();
    }
    void release_all() {
        std::lock_guard<std::mutex> lock(mutex);
        all_released = true;
        cv.notify_all();
    }
};

struct Observation {
    std::mutex mutex;
    std::condition_variable cv;
    const WorkerExecutor* started = nullptr;
    std::vector<capsid::WorkerExitStatus> exits;
    std::string response;
    unsigned heads = 0, ends = 0, errors = 0;
    unsigned status = 0;
};

struct ExitNotifyGate {
    std::mutex mutex;
    std::condition_variable cv;
    bool entered = false, released = false, joined = false, timed_out = false;
    void pause() {
        std::unique_lock<std::mutex> lock(mutex);
        entered = true;
        cv.notify_all();
        if (!cv.wait_for(lock, 10s, [&] { return released; })) timed_out = true;
    }
    void release() {
        std::lock_guard<std::mutex> lock(mutex);
        released = true;
        cv.notify_all();
    }
};

struct PoolFixture {
    std::shared_ptr<Observation> observation = std::make_shared<Observation>();
    std::shared_ptr<GenerationPool> pool;
    std::weak_ptr<GenerationPool> weak_pool;
    std::weak_ptr<WorkerExecutor> weak_executor;
    std::string generation;
    std::string identity;

    PoolFixture(const std::string& worker_path, char name,
                const std::shared_ptr<ExitNotifyGate>& exit_gate = {}) : identity(1, name) {
        generation = "sha256:" + std::string(64, name);
        // Each process maintains its own probe counter. A later ordinary
        // request observes what its real fetch handler executed, including
        // a probe that was queued before the request but completed earlier.
        const std::string bundle = "let probes=0; export default {fetch(request){"
            "const path=new URL(request.url).pathname;"
            "if(path==='/health'){++probes;return new Response('healthy');}"
            "if(path==='/status')return new Response('" + identity + ":'+probes);"
            "throw new Error('unexpected request '+path);}};";
        GenerationPoolOptions options;
        options.application_id = "supervisor-test";
        options.version = "v1";
        options.generation_digest = generation;
        options.workers = 1;
        options.recovery.max_events = 2;
        options.recovery.window_ms = 60000;
        options.recovery.backoff_initial_ms = 20;
        options.recovery.backoff_maximum_ms = 1000;
        options.recovery.jitter_basis_points = 0;
        options.recovery.stable_reset_ms = 60000;
        options.recovery.replacements_concurrent_per_app = 1;
        options.factory = [worker_path, bundle](capsid_worker** out, std::string* error) {
            capsid_worker_config config;
            capsid_worker_config_init(&config);
            config.worker_path = worker_path.c_str();
            config.request_timeout_ms = 2000;
            capsid_worker* worker = nullptr;
            if (capsid_worker_spawn(&config, &worker) != CAPSID_OK) {
                *error = "real Worker spawn failed";
                return false;
            }
            if (capsid_worker_load_bundle(worker,
                    reinterpret_cast<const std::uint8_t*>(bundle.data()), bundle.size()) != CAPSID_OK ||
                capsid_worker_flush(worker) != CAPSID_OK) {
                capsid_worker_destroy(worker);
                *error = "real Worker bundle load/flush failed";
                return false;
            }
            *out = worker;
            return true;
        };
        const auto observed = observation;
        if (exit_gate) {
            // An active worker's EXIT may schedule recovery before its
            // final notifier. Keep that replacement outside this test's
            // controlled exit interval; drain cancels it before any spawn.
            options.recovery.backoff_initial_ms = 60000;
            options.recovery.backoff_maximum_ms = 60000;
            options.on_executor_notify = [observed, exit_gate] {
                const WorkerExecutor* executor;
                {
                    std::lock_guard<std::mutex> lock(observed->mutex);
                    executor = observed->started;
                }
                if (executor != nullptr && executor->exit_status().observed) exit_gate->pause();
            };
        }
        options.on_worker_started = [observed](const WorkerExecutor* executor) {
            std::lock_guard<std::mutex> lock(observed->mutex);
            observed->started = executor;
        };
        options.on_drain_complete = [observed] {
            // finalize_drain still pins every reaped executor while this
            // callback reads the status recorded by its sole OS reaper.
            std::lock_guard<std::mutex> lock(observed->mutex);
            observed->exits.push_back(observed->started->exit_status());
            observed->cv.notify_all();
        };
        options.event_sink = [observed](const WorkerExecutor*, WorkerEvent event) {
            if (event.request_id != 101) return;
            std::lock_guard<std::mutex> lock(observed->mutex);
            switch (event.type) {
                case WorkerEvent::Type::kResponseHead:
                    ++observed->heads;
                    observed->status = event.status;
                    break;
                case WorkerEvent::Type::kResponseBody:
                    observed->response.append(event.body.begin(), event.body.end());
                    break;
                case WorkerEvent::Type::kResponseEnd: ++observed->ends; break;
                case WorkerEvent::Type::kError:
                case WorkerEvent::Type::kRequestFailure:
                case WorkerEvent::Type::kRequestTimeout: ++observed->errors; break;
                default: break;
            }
            observed->cv.notify_all();
        };
        std::string error;
        pool = GenerationPool::create(std::move(options), &error);
        require(pool != nullptr, error.c_str());
        weak_pool = pool;
        weak_executor = pool->current_worker_snapshot();
        require(!weak_executor.expired(), "real Worker must cross READY");
    }
    ~PoolFixture() {
        if (pool) pool->stop_and_join();
    }
    void expect_probes(unsigned count) {
        auto executor = pool->current_worker_snapshot();
        require(executor != nullptr, "status request requires the same live Worker");
        Command begin;
        begin.type = CommandType::kBeginRequest;
        begin.request_id = 101;
        begin.method = "GET";
        begin.url = "http://supervisor-test/status";
        begin.end_request = true;
        executor->submit(std::move(begin));
        Command credit;
        credit.type = CommandType::kGrantResponseCredit;
        credit.request_id = 101;
        credit.credit = 4096;
        executor->submit(std::move(credit));
        std::unique_lock<std::mutex> lock(observation->mutex);
        require(observation->cv.wait_for(lock, 5s, [&] {
                    return observation->ends != 0 || observation->errors != 0;
                }), "real status response must reach its terminal event");
        std::printf("STATUS generation=%s body=%s expected=%u\n",
                    identity.c_str(), observation->response.c_str(), count);
        require(observation->heads == 1 && observation->status == 200 &&
                observation->ends == 1 && observation->errors == 0,
                "status requires one 200 head and one successful response end");
        require(observation->response == identity + ":" + std::to_string(count),
                "Worker executed an unexpected number of health probes");
    }
    void expect_reaped() {
        if (pool) pool->stop_and_join();
        std::lock_guard<std::mutex> lock(observation->mutex);
        require(observation->exits.size() == 1, "exactly one real Worker must be reaped");
        capsid_test::print_worker_exit(observation->exits.front());
        std::printf("\n");
        require(capsid_test::natural_exit(observation->exits.front(), 0),
                "actual Worker OS exit must be natural zero");
    }
};

struct SupervisorFixture {
    HealthReadGate& gate;
    ManagedHostOptions managed;
    std::atomic<bool> external_stop{false};
    std::mutex selection_mutex;
    PoolFixture* selected;
    std::unique_ptr<WorkerSupervisor> supervisor;

    SupervisorFixture(HealthReadGate& reader, PoolFixture& first) : gate(reader), selected(&first) {
        managed.application = "supervisor-test";
        WorkerSupervisorOptions options;
        options.managed_options = &managed;
        options.policy.jitter_basis_points = 0;
        options.active_health_interval_ms = 60000;
        options.active_health_failures = 1;
        options.stop_requested = &external_stop;
        options.current_pool = [&] {
            std::lock_guard<std::mutex> lock(selection_mutex);
            return selected_pool(decltype(WorkerSupervisorOptions::current_pool){}, selected->pool);
        };
        options.health_check_reader = [&](const std::string& generation) { return gate.read(generation); };
        supervisor = std::make_unique<WorkerSupervisor>(std::move(options));
    }
    ~SupervisorFixture() { stop(); }
    void stop() {
        supervisor->request_stop();
        gate.release_all();
        supervisor->join();
    }
};

void run_exit_notifier(const std::string& worker) {
    auto gate = std::make_shared<ExitNotifyGate>();
    PoolFixture fixture(worker, 'a', gate);
    auto executor_pin = fixture.pool->current_worker_snapshot();
    struct DrainThread {
        ExitNotifyGate& gate;
        std::thread thread;
        ~DrainThread() {
            gate.release();
            if (thread.joinable()) thread.join();
        }
    } drain{*gate, {}};
    executor_pin->request_shutdown();
    {
        std::unique_lock<std::mutex> lock(gate->mutex);
        require(gate->cv.wait_for(lock, 5s, [&] { return gate->entered; }),
                "real executor must pause in its notifier after OS reap");
    }
    drain.thread = std::thread([&] {
        fixture.pool->stop_and_join();
        std::lock_guard<std::mutex> lock(gate->mutex);
        gate->joined = true;
        gate->cv.notify_all();
    });
    bool completed_before_notifier;
    {
        std::unique_lock<std::mutex> lock(gate->mutex);
        completed_before_notifier = gate->cv.wait_for(lock, 1s, [&] { return gate->joined; });
    }
    gate->release();
    drain.thread.join();
    fixture.expect_reaped();
    require(!gate->timed_out, "exit notifier barrier must not expire");
    require(!completed_before_notifier,
            "pool wait must join the final executor notifier despite an external executor lease");
    require(!fixture.weak_executor.expired(), "external executor lease remains valid after the join");
    executor_pin.reset();
    require(fixture.weak_executor.expired(), "last external lease releases the joined executor");
}

void run(const std::string& worker, const std::string& mode) {
    if (mode == "exit-notifier") {
        run_exit_notifier(worker);
        return;
    }
    HealthReadGate gate;
    PoolFixture first(worker, 'a');
    if (mode == "stale-pool") {
        PoolFixture second(worker, 'b');
        SupervisorFixture runner(gate, first);
        gate.wait(1);
        {
            std::lock_guard<std::mutex> lock(runner.selection_mutex);
            runner.selected = &second;
        }
        gate.release(1);
        gate.wait(2);
        first.expect_probes(0);
        runner.stop();
        second.expect_probes(0);
        first.expect_reaped();
        second.expect_reaped();
        require(gate.generations == std::vector<std::string>{first.generation, second.generation},
                "re-arm must read each selected generation's own configuration");
        return;
    }
    SupervisorFixture runner(gate, first);
    gate.wait(1);
    if (mode == "pool-lease") {
        // Fail safely on the borrowed implementation BEFORE dropping its
        // only owner; cleanup can still release/join the paused thread.
        require(first.weak_pool.use_count() >= 2,
                "supervisor must retain the pool across a blocked config read");
        require(first.weak_executor.use_count() >= 2,
                "supervisor must retain the executor independently of its pool");
        {
            std::lock_guard<std::mutex> lock(runner.selection_mutex);
            first.pool.reset();
        }
        require(!first.weak_pool.expired() && !first.weak_executor.expired(),
                "dropping the published owner must leave both supervisor leases alive");
        runner.stop();
        require(first.weak_pool.expired() && first.weak_executor.expired(),
                "joined supervisor must release the retired pool and executor");
    } else if (mode == "executor-lease") {
        require(first.weak_executor.use_count() >= 2,
                "supervisor must pin an executor before its pool can clear the slots");
        first.pool->stop_and_join();
        require(first.pool->state() == GenerationPool::State::kDead,
                "real pool drain must finish during the blocked config read");
        require(!first.weak_executor.expired(),
                "completed drain must not free the supervisor's executor");
        runner.stop();
        require(first.weak_executor.expired(), "join must release the drained executor lease");
    } else if (mode == "stop-local" || mode == "stop-external") {
        if (mode == "stop-local") runner.supervisor->request_stop();
        else runner.external_stop.store(true);
        gate.release_all();
        runner.supervisor->join();
        first.expect_probes(0);
    } else if (mode == "active-probe") {
        gate.release_all();
        auto executor = first.pool->current_worker_snapshot();
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (!executor->probe_state().complete && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(5ms);
        }
        const auto verdict = executor->probe_state();
        require(verdict.complete && verdict.healthy && verdict.status == 200,
                "configured current generation must execute a real successful probe");
        runner.stop();
        first.expect_probes(1);
    } else {
        throw std::runtime_error("unknown supervisor ownership case");
    }
    first.expect_reaped();
    require(!gate.timed_out, "controlled reader must finish without a barrier timeout");
}
}  // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 3, "Worker path and one ownership case are required");
        run(argv[1], argv[2]);
        std::printf("PASS: real supervisor ownership %s\n", argv[2]);
        return 0;
    } catch (const std::exception& error) {
        // All fixture destructors release the reader and join/drain before
        // failure reaches main. A failed contract never becomes a skip.
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
