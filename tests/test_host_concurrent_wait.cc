// M2 concurrent-wait CONTRACT test (not a RED regression gate): two
// threads calling wait() on the same object must both return true within
// a bounded window, neither may crash (double-join is UB), and both
// observe the object fully stopped after returning.
//
// Scope note: this test cannot distinguish call_once from an early-return
// implementation — request_stop() running first collapses the join window
// to ~zero (verified: restoring the old early-return implementation still
// passes 10/10). call_once's value is public-API contract correctness
// (a concurrent caller must BLOCK until the joins complete), not a
// reproducible-bug fix.

#include "host/single_worker_server.h"
#include "host/static_pool_server.h"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

[[noreturn]] void fail(const std::string& message) {
    std::cerr << "FAIL: " << message << std::endl;
    std::exit(1);
}

void require(bool condition, const std::string& message) {
    if (!condition) {
        fail(message);
    }
}

capsid::host::SingleWorkerServerOptions worker_options(
    const char* worker_path) {
    capsid::host::SingleWorkerServerOptions options;
    options.worker_path = worker_path;
    options.source_bundle_path = "concurrent-wait-inline";
    options.source_name = "file://orders/v1/bundle.mjs";
    options.application = "orders";
    options.listen_address = "127.0.0.1";
    options.listen_port = 0;
    options.public_scheme = "http";
    options.public_authority = "public.example";
    options.request_timeout_ms = 5000;
    options.initial_stream_window = 64U * 1024U;
    options.strict_sandbox = false;
    options.ready_fd = -1;
    options.write_ready_record = false;
    return options;
}

const std::vector<std::uint8_t>& fixture_bundle() {
    static const std::string source =
        "export default { fetch: () => new Response('concurrent-wait-ok') };";
    static const std::vector<std::uint8_t> bundle(source.begin(), source.end());
    return bundle;
}

void test_concurrent_shard_wait(const char* worker_path) {
    capsid::host::SingleWorkerServer server(worker_options(worker_path));
    std::string error;
    require(server.start(fixture_bundle(), &error),
            "concurrent shard wait: start failed: " + error);

    // Stop first: wait() blocks until the server is stopped, so without a
    // stop request both callers would legitimately block forever. The
    // concurrency under test is two callers racing INTO wait() at once —
    // the first owns the joins, the second must block until they complete
    // and may not return early with a "stopped" claim.
    server.request_stop();
    bool first_ok = false;
    bool second_ok = false;
    const auto began = std::chrono::steady_clock::now();
    std::thread first([&] {
        std::string wait_error;
        first_ok = server.wait(&wait_error);
    });
    std::thread second([&] {
        std::string wait_error;
        second_ok = server.wait(&wait_error);
    });
    first.join();
    second.join();
    require(std::chrono::steady_clock::now() - began <
                std::chrono::seconds(5),
            "concurrent shard wait exceeded its bounded window");
    require(first_ok && second_ok, "a concurrent shard wait() failed");
    // The server must be fully stopped: worker_available is false and the
    // facade teardown (destructor) must not crash on the joined threads.
    require(!server.worker_available(),
            "concurrent wait returned while the worker was still available");
}

void test_concurrent_pool_wait(const char* worker_path) {
    capsid::host::StaticPoolServerOptions options;
    options.workers = 2;
    options.worker_options = worker_options(worker_path);
    capsid::host::StaticPoolServer pool(std::move(options));
    std::string error;
    require(pool.start(fixture_bundle(), &error),
            "concurrent pool wait: start failed: " + error);
    require(pool.active_workers() == 2,
            "concurrent pool wait fixture did not activate two shards");

    pool.request_stop();
    bool first_ok = false;
    bool second_ok = false;
    const auto began = std::chrono::steady_clock::now();
    std::thread first([&] {
        std::string wait_error;
        first_ok = pool.wait(&wait_error);
    });
    std::thread second([&] {
        std::string wait_error;
        second_ok = pool.wait(&wait_error);
    });
    first.join();
    second.join();
    require(std::chrono::steady_clock::now() - began <
                std::chrono::seconds(5),
            "concurrent pool wait exceeded its bounded window");
    require(first_ok && second_ok, "a concurrent pool wait() failed");
    require(pool.active_workers() == 0,
            "concurrent pool wait returned before the pool was stopped");
}

}  // namespace

int main(int argc, char** argv) {
    require(argc == 3, "expected mode and capsid-worker path");
    const std::string mode = argv[1];
    if (mode == "shard") {
        test_concurrent_shard_wait(argv[2]);
    } else if (mode == "pool") {
        test_concurrent_pool_wait(argv[2]);
    } else {
        fail("unknown concurrent-wait mode");
    }
    std::cout << "PASS" << std::endl;
    return 0;
}
