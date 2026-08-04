// RED for the queue-saturation activity fix (docs/queue-saturation-activity-fix.md).
//
// Frozen before the implementation. Each test asserts the post-fix
// contract; the current implementation fails at least #1, #2 and #3
// deterministically (size > max_queued_bytes raises RangeError, and
// 64 concurrent 65537-byte responses cross the shared 4 MiB pending
// budget), and hangs on #5 under saturation.
//
// Run: test-response-queue-saturation <capsid-worker> <fixture.js>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <poll.h>
#include <unistd.h>

#include "capsid/runtime.h"

namespace {

[[noreturn]] void fail(const std::string &message) {
    std::fprintf(stderr, "FAIL: %s\n", message.c_str());
    std::exit(1);
}

#define require(condition, message)                                          \
    do {                                                                     \
        if (!(condition)) {                                                  \
            fail(std::string(message) + " (" #condition ")");                \
        }                                                                    \
    } while (0)

void require_result(capsid_result result, const std::string &operation) {
    if (result != CAPSID_OK) {
        fail(operation + ": " + capsid_result_string(result));
    }
}

std::string read_file(const char *path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        fail(std::string("cannot open fixture: ") + path);
    }
    return std::string(std::istreambuf_iterator<char>(input),
                       std::istreambuf_iterator<char>());
}

void pump(capsid_worker *worker) {
    const capsid_result flush = capsid_worker_flush(worker);
    if (flush != CAPSID_OK && flush != CAPSID_WOULD_BLOCK &&
        flush != CAPSID_CLOSED) {
        fail(std::string("flush: ") + capsid_result_string(flush));
    }
    const int fd = capsid_worker_fd(worker);
    if (fd >= 0) {
        struct pollfd descriptor = {};
        descriptor.fd = fd;
        descriptor.events =
            POLLIN | (flush == CAPSID_WOULD_BLOCK ? POLLOUT : 0);
        poll(&descriptor, 1, 10);
    }
}

bool next_event(capsid_worker *worker, capsid_event *event) {
    std::memset(event, 0, sizeof(*event));
    event->struct_size = sizeof(*event);
    const capsid_result result = capsid_worker_next_event(worker, event);
    if (result == CAPSID_OK) {
        return true;
    }
    if (result == CAPSID_WOULD_BLOCK || result == CAPSID_CLOSED) {
        return false;
    }
    fail(std::string("next event: ") + capsid_result_string(result));
    return false;
}

void wait_ready(capsid_worker *worker) {
    const std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        pump(worker);
        capsid_event event;
        while (next_event(worker, &event)) {
            if (event.type == CAPSID_EVENT_READY) {
                return;
            }
            if (event.type == CAPSID_EVENT_ERROR ||
                event.type == CAPSID_EVENT_EXIT) {
                fail("worker failed before READY");
            }
        }
    }
    fail("READY timeout");
}

capsid_worker *spawn_with(const char *worker_path,
                          const std::string &bundle,
                          uint32_t max_queued_bytes,
                          uint32_t initial_window,
                          uint32_t request_timeout_ms) {
    capsid_worker_config config;
    capsid_worker_config_init(&config);
    config.worker_path = worker_path;
    config.strict_sandbox = 0;
    config.max_header_bytes = 512;
    config.max_queued_bytes = max_queued_bytes;
    config.initial_stream_window = initial_window;
    config.request_timeout_ms = request_timeout_ms;

    capsid_worker *worker = NULL;
    require_result(capsid_worker_spawn(&config, &worker), "spawn");
    require_result(
        capsid_worker_load_bundle(
            worker,
            reinterpret_cast<const uint8_t *>(bundle.data()),
            bundle.size()),
        "load bundle");
    wait_ready(worker);
    return worker;
}

// Per-request outcome under test.
struct Outcome {
    bool ended = false;
    bool errored = false;
    bool exited = false;
    std::string body;
};

struct Collector {
    std::map<uint64_t, Outcome> outcomes;
    bool saw_exit = false;
    uint64_t head_frames = 0;
    uint64_t body_frames = 0;
    uint64_t end_frames = 0;
    uint64_t error_frames = 0;
    std::map<uint64_t, uint64_t> pending_grant_bytes;
};

enum class GrantMode {
    kImmediate,  // reimburse every frame as it arrives (fast host)
    kDeferredOnce,  // hold credits until every request produced a first
                    // frame (forcing worker-side pending accumulation),
                    // then release everything at once
};

// Drives the event loop until every request in `ids` has an end or an
// error (or `deadline` passes).
Collector drive(capsid_worker *worker,
                const std::vector<uint64_t> &ids,
                GrantMode mode,
                int timeout_s) {
    Collector collector;
    std::set<uint64_t> pending_ids(ids.begin(), ids.end());
    std::set<uint64_t> seen_first_frame;
    const size_t expected_ids = ids.size();
    bool deferred_fired = false;
    const auto fire_deferred = [&]() {
        if (deferred_fired) {
            return;
        }
        deferred_fired = true;
        // Release every held frame, plus a generous window so each
        // request can drain its remaining body without another stall.
        for (auto &entry : collector.pending_grant_bytes) {
            if (entry.second > 0) {
                require_result(
                    capsid_worker_grant_response_credit(
                        worker, entry.first,
                        static_cast<uint32_t>(entry.second) + 65536u),
                    "grant deferred credit");
                entry.second = 0;
            }
        }
    };
    const std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(timeout_s);
    while (std::chrono::steady_clock::now() < deadline &&
           !pending_ids.empty()) {
        pump(worker);
        capsid_event event;
        while (next_event(worker, &event)) {
            switch (event.type) {
                case CAPSID_EVENT_REQUEST_CREDIT:
                    continue;
                case CAPSID_EVENT_RESPONSE_HEAD:
                    collector.head_frames += 1;
                    continue;
                case CAPSID_EVENT_RESPONSE_BODY: {
                    collector.body_frames += 1;
                    Outcome &outcome = collector.outcomes[event.request_id];
                    outcome.body.append(
                        reinterpret_cast<const char *>(event.payload.data),
                        event.payload.size);
                    seen_first_frame.insert(event.request_id);
                    if (mode == GrantMode::kImmediate || deferred_fired) {
                        collector.pending_grant_bytes[event.request_id] +=
                            event.payload.size;
                        require_result(
                            capsid_worker_grant_response_credit(
                                worker, event.request_id,
                                static_cast<uint32_t>(
                                    collector.pending_grant_bytes
                                        [event.request_id])),
                            "grant response credit");
                        collector.pending_grant_bytes[event.request_id] = 0;
                    } else {
                        collector.pending_grant_bytes[event.request_id] +=
                            event.payload.size;
                    }
                    break;
                }
                case CAPSID_EVENT_RESPONSE_END:
                    collector.end_frames += 1;
                    collector.outcomes[event.request_id].ended = true;
                    pending_ids.erase(event.request_id);
                    break;
                case CAPSID_EVENT_ERROR:
                    collector.error_frames += 1;
                    collector.outcomes[event.request_id].errored = true;
                    pending_ids.erase(event.request_id);
                    break;
                case CAPSID_EVENT_EXIT:
                    collector.saw_exit = true;
                    pending_ids.clear();
                    break;
                default:
                    break;
            }
        }
        // Deferred mode: once every request has produced a first frame,
        // release the credits even if no new frame arrived this round
        // (the worker is waiting on credit, not on a frame).
        if (mode == GrantMode::kDeferredOnce &&
            !deferred_fired &&
            seen_first_frame.size() >= expected_ids) {
            fire_deferred();
        }
    }
    return collector;
}

void expect_body(const Collector &collector,
                 uint64_t id,
                 size_t size,
                 uint8_t byte) {
    const auto it = collector.outcomes.find(id);
    require(it != collector.outcomes.end(), "outcome exists");
    require(it->second.ended, "response ended");
    require(!it->second.errored, "response not errored");
    require(it->second.body.size() == size, "body size matches");
    for (size_t i = 0; i < size; ++i) {
        if (static_cast<uint8_t>(it->second.body[i]) != byte) {
            fail("body content mismatch");
        }
    }
}

void expect_error(const Collector &collector, uint64_t id) {
    const auto it = collector.outcomes.find(id);
    require(it != collector.outcomes.end(), "outcome exists");
    require(it->second.errored, "response errored");
}

// #1: a single response larger than the whole queue completes by
// segmented transfer. 20000 > max_queued 4096 -> the current
// implementation raises RangeError at the first write.
void test_body_larger_than_queue(const char *worker_path,
                                 const std::string &bundle) {
    capsid_worker *worker =
        spawn_with(worker_path, bundle, 4096, 4096, 2000);
    require_result(
        capsid_worker_begin_request(
            worker, 11, "GET", "https://example.test/chunk-20000", NULL, 0),
        "begin chunk-20000");
    require_result(
        capsid_worker_end_request(worker, 11),
        "end chunk-20000");
    Collector collector = drive(worker, { 11 }, GrantMode::kImmediate, 5);
    expect_body(collector, 11, 20000, 0x53);
    capsid_worker_destroy(worker);
}

// #2: a single write chunk larger than the queue segments instead of
// being rejected. 8193 > max_queued 4096.
void test_single_chunk_over_queue(const char *worker_path,
                                  const std::string &bundle) {
    capsid_worker *worker =
        spawn_with(worker_path, bundle, 4096, 4096, 2000);
    require_result(
        capsid_worker_begin_request(
            worker, 12, "GET", "https://example.test/chunk-8193", NULL, 0),
        "begin chunk-8193");
    require_result(
        capsid_worker_end_request(worker, 12),
        "end chunk-8193");
    Collector collector = drive(worker, { 12 }, GrantMode::kImmediate, 5);
    expect_body(collector, 12, 8193, 0x51);
    capsid_worker_destroy(worker);
}

// #3: 64 concurrent 65537-byte responses all complete. The shared
// 4 MiB pending budget is crossed when the host withholds credit
// (initial window 512): 64 x (65537 - 512) = 4,161,600 >
// 4,194,304 - 65025. The current implementation raises RangeError on
// the last request.
void test_64x65537_all_complete(const char *worker_path,
                                const std::string &bundle) {
    capsid_worker *worker =
        spawn_with(worker_path, bundle, 4u * 1024u * 1024u, 512, 5000);
    std::vector<uint64_t> ids;
    for (uint64_t id = 21; id < 21 + 64; ++id) {
        require_result(
            capsid_worker_begin_request(
                worker, id, "GET", "https://example.test/chunk-65537",
                NULL, 0),
            "begin 65537 request");
        require_result(
            capsid_worker_end_request(worker, id),
            "end 65537 request");
        ids.push_back(id);
    }
    Collector collector = drive(worker, ids, GrantMode::kDeferredOnce, 15);
    require(!collector.saw_exit, "worker must not exit");
    for (uint64_t id : ids) {
        expect_body(collector, id, 65537, 0x52);
    }
    capsid_worker_destroy(worker);
}

// #4 (control): 64 x 65536 stays fully healthy; the fix must not
// regress it.
void test_64x65536_all_complete(const char *worker_path,
                                const std::string &bundle) {
    capsid_worker *worker =
        spawn_with(worker_path, bundle, 4u * 1024u * 1024u, 512, 5000);
    std::vector<uint64_t> ids;
    for (uint64_t id = 41; id < 41 + 64; ++id) {
        require_result(
            capsid_worker_begin_request(
                worker, id, "GET", "https://example.test/chunk-65536",
                NULL, 0),
            "begin 65536 request");
        require_result(
            capsid_worker_end_request(worker, id),
            "end 65536 request");
        ids.push_back(id);
    }
    Collector collector = drive(worker, ids, GrantMode::kDeferredOnce, 15);
    require(!collector.saw_exit, "worker must not exit");
    for (uint64_t id : ids) {
        expect_body(collector, id, 65536, 0x52);
    }
    capsid_worker_destroy(worker);
}

// #5: with the output queue saturated by a large response, an
// application that errors still receives its terminal within 1 second
// (no 20 s+ hang). The erroring request throws before any body write
// (/error-immediate); the large request keeps the queue under pressure.
void test_error_after_write_terminal(const char *worker_path,
                                     const std::string &bundle) {
    capsid_worker *worker =
        spawn_with(worker_path, bundle, 4096, 4096, 2000);
    require_result(
        capsid_worker_begin_request(
            worker, 51, "GET", "https://example.test/chunk-20000",
            NULL, 0),
        "begin saturation request");
    require_result(
        capsid_worker_end_request(worker, 51),
        "end saturation request");
    require_result(
        capsid_worker_begin_request(
            worker, 52, "GET", "https://example.test/error-immediate",
            NULL, 0),
        "begin error request");
    require_result(
        capsid_worker_end_request(worker, 52),
        "end error request");
    Collector collector = drive(worker, { 51, 52 }, GrantMode::kImmediate, 1);
    expect_error(collector, 52);
    capsid_worker_destroy(worker);
}

// #6: a small response is not starved behind a large one.
void test_small_not_starved(const char *worker_path,
                            const std::string &bundle) {
    capsid_worker *worker =
        spawn_with(worker_path, bundle, 8192, 4096, 2000);
    require_result(
        capsid_worker_begin_request(
            worker, 61, "GET", "https://example.test/chunk-65537",
            NULL, 0),
        "begin large");
    require_result(
        capsid_worker_end_request(worker, 61),
        "end large");
    require_result(
        capsid_worker_begin_request(
            worker, 62, "GET", "https://example.test/small", NULL, 0),
        "begin small");
    require_result(
        capsid_worker_end_request(worker, 62),
        "end small");
    Collector collector = drive(worker, { 61, 62 }, GrantMode::kImmediate, 2);
    const auto small = collector.outcomes.find(62);
    require(small != collector.outcomes.end() && small->second.ended,
            "small response completed");
    require(!small->second.errored, "small response not errored");
    require(small->second.body.size() == 8 &&
                small->second.body == "small-ok",
            "small response content correct");
    capsid_worker_destroy(worker);
}

// #7: cancelling a request whose body is still pending must not leak
// or UAF (ASan gate), and the worker stays usable afterwards.
void test_cancel_pending(const char *worker_path,
                         const std::string &bundle) {
    capsid_worker *worker =
        spawn_with(worker_path, bundle, 4096, 4096, 2000);
    require_result(
        capsid_worker_begin_request(
            worker, 71, "GET", "https://example.test/chunk-20000",
            NULL, 0),
        "begin cancel target");
    require_result(
        capsid_worker_end_request(worker, 71),
        "end cancel target");
    // Let the worker start producing body frames, then cancel.
    const std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    bool saw_body = false;
    while (std::chrono::steady_clock::now() < deadline && !saw_body) {
        pump(worker);
        capsid_event event;
        while (next_event(worker, &event)) {
            if (event.type == CAPSID_EVENT_REQUEST_CREDIT) {
                continue;
            }
            if (event.type == CAPSID_EVENT_RESPONSE_BODY &&
                event.request_id == 71) {
                saw_body = true;
            }
            if (event.type == CAPSID_EVENT_EXIT) {
                fail("worker exited during cancel test");
            }
        }
    }
    require(saw_body, "cancel target produced body");
    require_result(capsid_worker_cancel(worker, 71), "cancel pending body");
    // Worker must remain usable.
    require_result(
        capsid_worker_begin_request(
            worker, 72, "GET", "https://example.test/small", NULL, 0),
        "begin after cancel");
    require_result(
        capsid_worker_end_request(worker, 72),
        "end after cancel");
    Collector collector = drive(worker, { 72 }, GrantMode::kImmediate, 3);
    const auto small = collector.outcomes.find(72);
    require(small != collector.outcomes.end() && small->second.ended,
            "small response completed after cancel");
    require(small->second.body == "small-ok",
            "small response content correct after cancel");
    capsid_worker_destroy(worker);
}

// #8: the worker still serves requests after the stress scenario.
void test_reusable_after_stress(const char *worker_path,
                                const std::string &bundle) {
    capsid_worker *worker =
        spawn_with(worker_path, bundle, 8192, 4096, 2000);
    require_result(
        capsid_worker_begin_request(
            worker, 81, "GET", "https://example.test/chunk-20000",
            NULL, 0),
        "begin stress");
    require_result(
        capsid_worker_end_request(worker, 81),
        "end stress");
    Collector stressed = drive(worker, { 81 }, GrantMode::kImmediate, 5);
    expect_body(stressed, 81, 20000, 0x53);

    require_result(
        capsid_worker_begin_request(
            worker, 82, "GET", "https://example.test/small", NULL, 0),
        "begin after stress");
    require_result(
        capsid_worker_end_request(worker, 82),
        "end after stress");
    Collector after = drive(worker, { 82 }, GrantMode::kImmediate, 3);
    const auto small = after.outcomes.find(82);
    require(small != after.outcomes.end() && small->second.ended,
            "small response completed after stress");
    require(small->second.body == "small-ok",
            "small response content correct after stress");
    capsid_worker_destroy(worker);
}

}  // namespace

// Optional argv[3]: run only the numbered test (1..8). Without it the
// whole suite runs in order; a failing test stops the run, so the
// selector is how the RED matrix is captured per test.
void run_sel(const std::string &only,
             const char *worker_path,
             const std::string &bundle,
             int number,
             void (*test)(const char *, const std::string &)) {
    if (only.empty() || only == std::to_string(number)) {
        test(worker_path, bundle);
    }
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fail("expected worker path and fixture path");
    }
    const std::string bundle = read_file(argv[2]);
    const std::string only = argc > 3 ? argv[3] : "";
    run_sel(only, argv[1], bundle, 1, test_body_larger_than_queue);
    run_sel(only, argv[1], bundle, 2, test_single_chunk_over_queue);
    run_sel(only, argv[1], bundle, 3, test_64x65537_all_complete);
    run_sel(only, argv[1], bundle, 4, test_64x65536_all_complete);
    run_sel(only, argv[1], bundle, 5, test_error_after_write_terminal);
    run_sel(only, argv[1], bundle, 6, test_small_not_starved);
    run_sel(only, argv[1], bundle, 7, test_cancel_pending);
    run_sel(only, argv[1], bundle, 8, test_reusable_after_stress);
    std::printf("all queue-saturation RED tests passed\n");
    return 0;
}
