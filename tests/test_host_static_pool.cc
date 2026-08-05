// Frozen M2 static-pool state-machine RED suite.
//
// This first batch deliberately freezes only the two ownership boundaries
// needed before queueing and load selection can be implemented:
//   * a fixed pool cannot activate until every configured worker is READY;
//   * a worker's owner shard is immutable and only that shard may see it in
//     its local READY scheduling set.

#if __has_include("host/static_pool.h")
#include "host/static_pool.h"
#define CAPSID_HAS_STATIC_POOL 1
#else
#define CAPSID_HAS_STATIC_POOL 0
#endif

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace {

[[noreturn]] void fail(const std::string& message) {
    std::cerr << "FAIL: " << message << std::endl;
    std::exit(1);
}

#if CAPSID_HAS_STATIC_POOL

using capsid::host::PoolWorkerId;
using capsid::host::StaticPoolState;

void require(bool condition, const std::string& message) {
    if (!condition) {
        fail(message);
    }
}

void require_workers(std::vector<PoolWorkerId> actual,
                     std::vector<PoolWorkerId> expected,
                     const std::string& message) {
    std::sort(actual.begin(), actual.end());
    std::sort(expected.begin(), expected.end());
    require(actual == expected, message);
}

void test_activates_only_when_all_ready() {
    StaticPoolState empty(0);
    require(!empty.can_activate(),
            "zero-target pool claimed it could activate");
    require(!empty.activate(), "zero-target pool activated");
    require(!empty.active(), "invalid zero-target activation changed state");

    StaticPoolState pool(3);
    require(pool.target_workers() == 3, "target worker count was not retained");
    require(!pool.active(), "new pool started active");
    require(!pool.can_activate(), "empty pool claimed it could activate");

    require(pool.register_starting(101, 0), "worker 101 was not registered");
    require(pool.register_starting(102, 1), "worker 102 was not registered");
    require(pool.register_starting(103, 0), "worker 103 was not registered");
    require(!pool.register_starting(104, 1),
            "pool accepted more workers than its fixed target");
    require(pool.registered_workers() == 3,
            "registered worker count did not reach the fixed target");

    require(pool.mark_ready(101), "worker 101 did not become READY");
    require(pool.mark_ready(102), "worker 102 did not become READY");
    require(pool.ready_workers() == 2, "READY count is wrong before activation");
    require(!pool.can_activate(), "partially READY pool claimed activation readiness");
    require(!pool.activate(), "partially READY pool activated");
    require(!pool.active(), "failed early activation changed pool state");

    require(!pool.mark_ready(999), "unknown worker was accepted as READY");
    require(!pool.mark_ready(102), "duplicate READY transition was accepted");
    require(pool.ready_workers() == 2,
            "invalid READY transitions changed the READY count");

    require(pool.mark_ready(103), "final worker did not become READY");
    require(pool.can_activate(), "fully READY fixed pool cannot activate");
    require(pool.activate(), "fully READY fixed pool did not activate");
    require(pool.active(), "successful activation was not retained");
    require(!pool.activate(), "pool accepted a duplicate activation transition");
    require(pool.active(), "duplicate activation attempt cleared active state");
}

void test_preserves_owner_shard() {
    StaticPoolState pool(3);
    require(pool.register_starting(201, 0), "worker 201 was not registered");
    require(pool.register_starting(202, 1), "worker 202 was not registered");

    // Re-registering an existing worker, especially on another shard, is not
    // a transfer operation. Ownership is assigned once during bootstrap.
    require(!pool.register_starting(201, 1),
            "worker ownership migrated through duplicate registration");
    const std::optional<std::uint32_t> owner = pool.owner_shard(201);
    require(owner.has_value() && *owner == 0,
            "duplicate registration changed the worker owner shard");

    require(pool.register_starting(203, 0), "worker 203 was not registered");
    require(pool.mark_ready(201), "worker 201 did not become READY");
    require(pool.mark_ready(202), "worker 202 did not become READY");
    require(pool.mark_ready(203), "worker 203 did not become READY");

    require_workers(pool.ready_workers_for_shard(0), {201, 203},
                    "shard 0 scheduling set contains foreign workers");
    require_workers(pool.ready_workers_for_shard(1), {202},
                    "shard 1 scheduling set contains foreign workers");
    require_workers(pool.ready_workers_for_shard(2), {},
                    "empty shard borrowed workers from another shard");

    require(pool.activate(), "fully READY owner-shard fixture did not activate");
    require_workers(pool.ready_workers_for_shard(1), {202},
                    "activation changed immutable shard ownership");
}

#endif

}  // namespace

int main(int argc, char** argv) {
#if !CAPSID_HAS_STATIC_POOL
    (void)argc;
    (void)argv;
    fail("host/static_pool.h is not implemented");
#else
    require(argc == 2, "expected exactly one test mode");
    const std::string mode = argv[1];
    if (mode == "all-ready") {
        test_activates_only_when_all_ready();
    } else if (mode == "owner-shard") {
        test_preserves_owner_shard();
    } else {
        fail("unknown test mode: " + mode);
    }
    std::cout << "PASS: " << mode << std::endl;
    return 0;
#endif
}
