// WP-05 PR-09 §9.2: RoutingSnapshot / RoutingTable + GenerationPool
// adopt-create tests. The snapshot contract is pinned with REAL pools
// (create_adopted over pre-warmed workers): the router must pin the pool it
// found at request time even after a republish, a missing route is nullptr,
// and a drained pool still routes to the same pool — whose pick_worker then
// returns nullptr so the caller synthesizes 503.

#include "host/generation_pool.h"
#include "host/routing_snapshot.h"
#include "host/worker_event_source.h"

#include <signal.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using capsid::host::GenerationPool;
using capsid::host::GenerationPoolOptions;
using capsid::host::RoutingSnapshot;
using capsid::host::RoutingTable;
using capsid::host::WorkerExecutor;
using capsid::host::WorkerRecoveryPolicy;

[[noreturn]] void fail(const std::string& message) {
    std::cerr << "test-host-routing-snapshot: " << message << std::endl;
    std::exit(1);
}

void require(bool condition, const std::string& message) {
    if (!condition) {
        fail(message);
    }
}

const char* kBundle =
    "export default { async fetch(request) {"
    " return new Response('hello-snapshot'); } };\n";

// Spawn/load/flush the worker (NOT yet READY) — the executor's factory
// contract.
WorkerExecutor::WorkerFactory spawn_factory(const std::string& worker_path) {
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
            capsid_worker_destroy(worker);
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

// The Managed coordinator's warm-up: spawn + load + flush, then consume the
// READY handshake (including the compatibility check) BEFORE adopt. This
// mirrors managed_host.cc's warm-up loop exactly: next_event returns
// CAPSID_WOULD_BLOCK while READY is in flight, so the single WorkerEventSource
// adapter must wait before retrying (design review §4.3).
capsid_worker* warm_worker(const std::string& worker_path) {
    std::string error;
    capsid_worker* worker = nullptr;
    require(spawn_factory(worker_path)(&worker, &error),
            "warm worker spawn failed: " + error);
    capsid::host::WorkerEventSource event_source;
    event_source.set_worker(worker);
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(15);
    capsid_event event = {};
    event.struct_size = sizeof(event);
    for (;;) {
        const capsid_result flush = capsid_worker_flush(worker);
        if (flush != CAPSID_OK && flush != CAPSID_WOULD_BLOCK) {
            capsid_worker_destroy(worker);
            fail("warm worker flush failed before READY");
        }
        const capsid_result result = capsid_worker_next_event(worker, &event);
        if (result == CAPSID_OK) {
            if (event.type == CAPSID_EVENT_READY) {
                break;
            }
            if (event.type == CAPSID_EVENT_ERROR) {
                const std::string detail(
                    reinterpret_cast<const char*>(event.payload.data),
                    event.payload.size);
                capsid_worker_destroy(worker);
                fail("warm worker error before READY: " +
                     (detail.empty() ? "(empty)" : detail));
            }
            if (event.type == CAPSID_EVENT_EXIT) {
                capsid_worker_destroy(worker);
                fail("warm worker exited before READY");
            }
            continue;
        }
        if (result != CAPSID_WOULD_BLOCK) {
            capsid_worker_destroy(worker);
            fail("warm worker event error before READY");
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            capsid_worker_destroy(worker);
            fail("warm worker READY timeout");
        }
        event_source.wait(std::min(deadline, std::chrono::steady_clock::now() +
                                                 std::chrono::milliseconds(100)));
    }
    const std::string payload(
        reinterpret_cast<const char*>(event.payload.data), event.payload.size);
    capsid_build_info info;
    capsid_build_info_init(&info);
    require(capsid_runtime_build_info(&info) == CAPSID_OK &&
                info.compatibility_id != nullptr &&
                payload == info.compatibility_id,
            "warm worker compatibility ID mismatch");
    return worker;
}

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
                                   const std::string& application,
                                   std::uint32_t workers) {
    GenerationPoolOptions options;
    options.application_id = application;
    options.version = "v1";
    options.generation_digest =
        "sha256:" + std::string(64, application[0]);
    options.workers = workers;
    options.factory = spawn_factory(worker_path);
    options.recovery = test_policy();
    return options;
}

std::vector<capsid_worker*> warm_fleet(const std::string& worker_path,
                                       std::uint32_t count) {
    std::vector<capsid_worker*> workers;
    workers.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        workers.push_back(warm_worker(worker_path));
    }
    return workers;
}

void test_adopt_create_serves(const std::string& worker_path) {
    std::vector<capsid_worker*> warmed = warm_fleet(worker_path, 2);
    std::string error;
    std::shared_ptr<GenerationPool> pool = GenerationPool::create_adopted(
        pool_options(worker_path, "adoptapp", 2), std::move(warmed), &error);
    require(pool != nullptr, "adopt-create failed: " + error);
    require(pool->state() == GenerationPool::State::kActive,
            "adopted pool is not active");
    require(pool->ready_workers() == 2, "adopted fleet not fully READY");
    require(pool->application_id() == "adoptapp", "adopted identity");
    WorkerExecutor* executor = pool->pick_worker();
    require(executor != nullptr, "adopted pool cannot pick a worker");
    require(executor->available(), "picked worker not available");
    pool->stop_and_join();
    require(pool->state() == GenerationPool::State::kDead,
            "adopted pool did not drain");
    std::cout << "PASS: adopt-create serves and drains" << std::endl;
}

void test_adopt_create_rejects_mismatch(const std::string& worker_path) {
    // Two warmed workers, options.workers == 1: must fail closed and destroy
    // BOTH handed-in workers (nothing escapes).
    std::vector<capsid_worker*> warmed = warm_fleet(worker_path, 2);
    std::string error;
    std::shared_ptr<GenerationPool> pool = GenerationPool::create_adopted(
        pool_options(worker_path, "mismatch", 1), std::move(warmed), &error);
    require(pool == nullptr, "mismatched adopt-create was accepted");
    require(!error.empty(), "mismatched adopt-create gave no error");
    require(error.find("exactly") != std::string::npos,
            "mismatch error is not the contract error: '" + error + "'");

    // Missing factory: fail closed too.
    std::vector<capsid_worker*> warmed2 = warm_fleet(worker_path, 1);
    GenerationPoolOptions options = pool_options(worker_path, "nofactory", 1);
    options.factory = nullptr;
    pool = GenerationPool::create_adopted(options, std::move(warmed2), &error);
    require(pool == nullptr, "factory-less adopt-create was accepted");
    require(error.find("factory") != std::string::npos,
            "factory error is not the contract error: '" + error + "'");
    std::cout << "PASS: adopt-create mismatches fail closed" << std::endl;
}

void test_snapshot_pins_old_pool_after_republish(
    const std::string& worker_path) {
    std::string error;
    std::shared_ptr<GenerationPool> pool_a = GenerationPool::create_adopted(
        pool_options(worker_path, "app-a", 1),
        warm_fleet(worker_path, 1), &error);
    std::shared_ptr<GenerationPool> pool_b = GenerationPool::create_adopted(
        pool_options(worker_path, "app-b", 1),
        warm_fleet(worker_path, 1), &error);
    require(pool_a && pool_b, "adopt-create failed: " + error);

    RoutingTable table;
    auto first = RoutingSnapshot::build({{"app-a", pool_a},
                                         {"app-b", pool_b}});
    table.publish(first);
    require(table.generation() == 1, "generation did not advance");

    // A request "starts" on the first snapshot: it pins pool_a.
    std::shared_ptr<const RoutingSnapshot> pinned = table.load();
    std::shared_ptr<GenerationPool> pinned_a = pinned->find("app-a");
    require(pinned_a == pool_a, "snapshot did not find app-a");

    // Republish with app-a replaced by a new pool. The new load sees the
    // new pool; the request that pinned the old snapshot still holds the
    // OLD pool alive and serviceable.
    std::shared_ptr<GenerationPool> pool_a2 = GenerationPool::create_adopted(
        pool_options(worker_path, "app-a", 1),
        warm_fleet(worker_path, 1), &error);
    table.publish(RoutingSnapshot::build({{"app-a", pool_a2}}));
    require(table.generation() == 2, "republish did not advance");
    std::shared_ptr<const RoutingSnapshot> latest = table.load();
    require(latest->find("app-a") == pool_a2,
            "latest snapshot did not see the new pool");
    require(pinned_a->pick_worker() != nullptr,
            "pinned old pool became unserveable after republish");
    require(latest->find("app-b") == nullptr,
            "republished snapshot leaked the removed app");

    // A missing route is nullptr — the router's 503 point.
    require(pinned->find("missing") == nullptr,
            "missing route did not yield nullptr");

    // A drained pool still ROUTES to the same pool; pick_worker returns
    // nullptr so the session layer synthesizes 503.
    pool_b->request_drain();
    std::shared_ptr<GenerationPool> routed_b = pinned->find("app-b");
    require(routed_b == pool_b, "drain changed the route");
    require(routed_b->pick_worker() == nullptr,
            "drained pool still picked a worker");
    pool_a->stop_and_join();
    pool_a2->stop_and_join();
    pool_b->stop_and_join();
    std::cout << "PASS: snapshot pins old pool across republish" << std::endl;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        fail("expected capsid-worker path");
    }
    const std::string worker_path = argv[1];
    test_adopt_create_serves(worker_path);
    test_adopt_create_rejects_mismatch(worker_path);
    test_snapshot_pins_old_pool_after_republish(worker_path);
    std::cout << "PASS: RoutingSnapshot + adopt-create (WP-05 §9.2)" << std::endl;
    return 0;
}
