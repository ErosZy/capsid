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

#include "graceful_worker_exit.h"
#include "win32_compat.h"
#if !defined(_WIN32)
#include <unistd.h>
#endif

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
        capsid_pollfd descriptor = {};
        descriptor.fd = fd;
        descriptor.events =
            POLLIN | (flush == CAPSID_WOULD_BLOCK ? POLLOUT : 0);
        capsid::win32::capsid_poll(&descriptor, 1, 10);
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
    // Terminal arrival order (end/error), for ordering assertions.
    std::vector<uint64_t> completion_order;
    // Per-request terminal count: each request must produce exactly one.
    std::map<uint64_t, uint32_t> terminal_counts;
    std::map<uint64_t, uint64_t> pending_grant_bytes;
};

// Observe every phase through the same counters; a terminal received while
// credit is withheld remains part of the final exactly-once assertions.
void collect_event(Collector &collector, const capsid_event &event) {
    switch (event.type) {
        case CAPSID_EVENT_RESPONSE_HEAD:
            collector.head_frames += 1;
            break;
        case CAPSID_EVENT_RESPONSE_BODY:
            collector.body_frames += 1;
            collector.outcomes[event.request_id].body.append(
                reinterpret_cast<const char *>(event.payload.data), event.payload.size);
            break;
        case CAPSID_EVENT_RESPONSE_END:
            collector.end_frames += 1;
            collector.outcomes[event.request_id].ended = true;
            collector.terminal_counts[event.request_id] += 1;
            collector.completion_order.push_back(event.request_id);
            collector.pending_grant_bytes[event.request_id] = 0;
            break;
        case CAPSID_EVENT_ERROR:
        case CAPSID_EVENT_REQUEST_TIMEOUT:
            collector.error_frames += 1;
            collector.outcomes[event.request_id].errored = true;
            collector.terminal_counts[event.request_id] += 1;
            collector.completion_order.push_back(event.request_id);
            collector.pending_grant_bytes[event.request_id] = 0;
            break;
        case CAPSID_EVENT_EXIT:
            collector.saw_exit = true;
            break;
        default:
            break;
    }
}

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
                int timeout_s,
                Collector collector = Collector()) {
    std::set<uint64_t> pending_ids(ids.begin(), ids.end());
    for (const auto &entry : collector.terminal_counts) {
        if (entry.second != 0) pending_ids.erase(entry.first);
    }
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
                const capsid_result result =
                    capsid_worker_grant_response_credit(
                        worker, entry.first,
                        static_cast<uint32_t>(entry.second) + 65536u);
                entry.second = 0;
                require_result(result, "grant deferred credit");
            }
        }
    };
    const std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(timeout_s);
    bool first_pass = true;
    while (std::chrono::steady_clock::now() < deadline &&
           (first_pass || !pending_ids.empty())) {
        first_pass = false;
        pump(worker);
        capsid_event event;
        while (next_event(worker, &event)) {
            collect_event(collector, event);
            switch (event.type) {
                case CAPSID_EVENT_REQUEST_CREDIT:
                    continue;
                case CAPSID_EVENT_RESPONSE_HEAD:
                    continue;
                case CAPSID_EVENT_RESPONSE_BODY: {
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
                    pending_ids.erase(event.request_id);
                    break;
                case CAPSID_EVENT_ERROR:
                case CAPSID_EVENT_REQUEST_TIMEOUT:
                    pending_ids.erase(event.request_id);
                    break;
                case CAPSID_EVENT_EXIT:
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

// #9: an error message larger than the whole queue must still deliver
// a terminal (truncated to fit one frame) instead of wedging forever.
void test_huge_error_message(const char *worker_path,
                             const std::string &bundle) {
    capsid_worker *worker =
        spawn_with(worker_path, bundle, 4096, 4096, 2000);
    require_result(
        capsid_worker_begin_request(
            worker, 91, "GET",
            "https://example.test/error-huge-message", NULL, 0),
        "begin huge error message");
    require_result(
        capsid_worker_end_request(worker, 91),
        "end huge error message");
    Collector collector = drive(worker, { 91 }, GrantMode::kImmediate, 2);
    expect_error(collector, 91);
    capsid_worker_destroy(worker);
}

// #10: a real partial-write-then-error response (stream error after a
// large chunk) still produces exactly one terminal within 2 s.
void test_error_after_partial_write(const char *worker_path,
                                    const std::string &bundle) {
    capsid_worker *worker =
        spawn_with(worker_path, bundle, 4096, 4096, 2000);
    require_result(
        capsid_worker_begin_request(
            worker, 92, "GET",
            "https://example.test/error-after-write-real", NULL, 0),
        "begin error-after-write-real");
    require_result(
        capsid_worker_end_request(worker, 92),
        "end error-after-write-real");
    Collector collector = drive(worker, { 92 }, GrantMode::kImmediate, 2);
    expect_error(collector, 92);
    require(collector.body_frames > 0,
            "partial write happened before the error");
    require(collector.terminal_counts[92] == 1,
            "error-after-write produced exactly one terminal");
    capsid_worker_destroy(worker);
}

// #11: timeout under a saturated queue fires the cancel path exactly
// once; the terminal is delivered exactly once (no re-cancel on later
// timer ticks).
//
// A 1 MiB wire queue and 64 response bodies provide the pressure input.
// The dedicated timeout writer fixture lets the first body from each
// response reach the host, then returns EAGAIN for 1.5 s. Its independent
// queue observer requires the timeout terminal to remain blocked behind
// a full wire queue across later timer ticks, then enter the queue once.
// The ordinary Worker runs the same public exactly-once/abort assertions;
// kernel buffering alone does not prove that its wire queue saturated.
void test_timeout_once_under_saturation(const char *worker_path,
                                        const std::string &bundle) {
    capsid_worker *worker =
        spawn_with(worker_path, bundle, 1024u * 1024u, 16u * 1024u, 300);
    std::vector<uint64_t> ids;
    require_result(
        capsid_worker_begin_request(
            worker, 92, "GET", "https://example.test/hang", NULL, 0),
        "begin hang for timeout");
    require_result(
        capsid_worker_end_request(worker, 92),
        "end hang for timeout");
    ids.push_back(92);
    for (uint64_t id = 93; id < 93 + 64; ++id) {
        require_result(
            capsid_worker_begin_request(
                worker, id, "GET", "https://example.test/chunk-65537",
                NULL, 0),
            "begin saturation for timeout");
        require_result(
            capsid_worker_end_request(worker, id),
            "end saturation for timeout");
        ids.push_back(id);
    }

    // Phase A: read until every request produced its first frame, then
    // release deferred credits (per request 65536 + accumulated) so the
    // worker can fill its 1 MiB outbound buffer.
    const std::chrono::steady_clock::time_point first_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(3);
    std::set<uint64_t> seen_first;
    bool fired = false;
    std::map<uint64_t, uint64_t> held;
    Collector phase_a;
    while (std::chrono::steady_clock::now() < first_deadline && !fired) {
        pump(worker);
        capsid_event event;
        while (next_event(worker, &event)) {
            collect_event(phase_a, event);
            if (event.type == CAPSID_EVENT_RESPONSE_BODY) {
                seen_first.insert(event.request_id);
                if (fired) {
                    require_result(
                        capsid_worker_grant_response_credit(
                            worker, event.request_id,
                            static_cast<uint32_t>(event.payload.size)),
                        "grant phase C");
                } else {
                    held[event.request_id] += event.payload.size;
                }
            }
        }
        if (!fired && seen_first.size() >= 64) {
            fired = true;
            for (std::map<uint64_t, uint64_t>::const_iterator it =
                     held.begin();
                 it != held.end();
                 ++it) {
                // The ABI rejects credit after a terminal has retired the
                // request. Keep that terminal and omit only its stale grant.
                if (phase_a.terminal_counts.count(it->first) != 0) {
                    held[it->first] = 0;
                    continue;
                }
                require_result(
                    capsid_worker_grant_response_credit(
                        worker, it->first,
                        static_cast<uint32_t>(it->second) + 65536u),
                    "grant deferred");
                held[it->first] = 0;
            }
        }
    }
    require(fired, "all saturation requests produced first frames");

    // Publish the queued WINDOW_UPDATE frames before withholding reads.
    // Otherwise Phase B leaves the worker blocked on credit rather than
    // exercising its outbound wire queue.
    bool credits_flushed = false;
    while (std::chrono::steady_clock::now() < first_deadline && !credits_flushed) {
        const capsid_result result = capsid_worker_flush(worker);
        if (result == CAPSID_OK) {
            credits_flushed = true;
        } else if (result == CAPSID_WOULD_BLOCK) {
            capsid_pollfd descriptor = {};
            descriptor.fd = capsid_worker_fd(worker);
            descriptor.events = POLLOUT;
            capsid::win32::capsid_poll(&descriptor, 1, 10);
        } else {
            require_result(result, "flush deferred response credit");
        }
    }
    require(credits_flushed, "deferred credits reached the Worker before Phase B");

    // Phase B: stop reading for 1.5 s. The worker keeps appending
    // (credits were released), its socket and outbound buffer fill to
    // the 1 MiB wire limit, and the hang request times out at ~300 ms
    // with its error terminal wedged behind the full queue — several
    // timer ticks pass, enough for a re-cancel bug to inflate the
    // abort counter.
    const std::chrono::steady_clock::time_point sat_deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
    while (std::chrono::steady_clock::now() < sat_deadline) {
        const int fd = capsid_worker_fd(worker);
        capsid_pollfd descriptor = {};
        descriptor.fd = fd;
        descriptor.events = 0;  // never readable: keep the queue full
        capsid::win32::capsid_poll(&descriptor, 1, 10);
    }

    // Phase C: read + grant everything, then probe the abort counter.
    Collector collector = drive(worker, ids, GrantMode::kImmediate, 15, phase_a);
    require(collector.outcomes.size() == ids.size(),
            "every observed outcome belongs to the submitted requests");
    require(!collector.saw_exit, "worker must not exit");
    const auto hang = collector.outcomes.find(92);
    if (hang == collector.outcomes.end() || !hang->second.errored) {
        std::fprintf(stderr,
                     "timeout saturation diagnostics: outcomes=%zu head=%llu "
                     "body=%llu end=%llu error=%llu hang_terminals=%u\n",
                     collector.outcomes.size(),
                     static_cast<unsigned long long>(collector.head_frames),
                     static_cast<unsigned long long>(collector.body_frames),
                     static_cast<unsigned long long>(collector.end_frames),
                     static_cast<unsigned long long>(collector.error_frames),
                     collector.terminal_counts[92]);
    }
    require(hang != collector.outcomes.end() && hang->second.errored,
            "hang request errored via deadline");
    // The 64 saturation requests also hit the 300 ms deadline (they are
    // wedged behind the full queue) — each gets exactly one error
    // terminal; the hang request must also produce exactly one.
    require(collector.terminal_counts[92] == 1,
            "hang request produced exactly one terminal");
    for (uint64_t id = 93; id < 157; ++id) {
        require(collector.terminal_counts[id] == 1,
                "saturation request produced exactly one terminal");
    }

    require_result(
        capsid_worker_begin_request(
            worker, 158, "GET", "https://example.test/hang-count",
            NULL, 0),
        "begin hang-count probe");
    require_result(
        capsid_worker_end_request(worker, 158),
        "end hang-count probe");
    Collector probe = drive(worker, { 158 }, GrantMode::kImmediate, 3);
    const auto count = probe.outcomes.find(158);
    require(count != probe.outcomes.end() && count->second.ended,
            "hang-count probe completed");
    require(count->second.body.size() == 4 && count->second.body[0] == '1',
            "cancel bridge delivered exactly once");
    require(capsid_test::shutdown_and_wait(worker, 5000).clean(),
            "timeout pressure Worker exited cleanly with its fixture verdict");
    capsid_worker_destroy(worker);
}

// #12: with a small queue, the small response completes before the
// large response and within a bounded number of pump passes (true
// round-robin, not begin()-first ordering).
void test_small_completes_first(const char *worker_path,
                                const std::string &bundle) {
    capsid_worker *worker =
        spawn_with(worker_path, bundle, 8192, 4096, 2000);
    require_result(
        capsid_worker_begin_request(
            worker, 95, "GET", "https://example.test/chunk-65537",
            NULL, 0),
        "begin large for ordering");
    require_result(
        capsid_worker_end_request(worker, 95),
        "end large for ordering");
    require_result(
        capsid_worker_begin_request(
            worker, 96, "GET", "https://example.test/small", NULL, 0),
        "begin small for ordering");
    require_result(
        capsid_worker_end_request(worker, 96),
        "end small for ordering");
    Collector collector = drive(worker, { 95, 96 }, GrantMode::kImmediate, 2);
    const auto large = collector.outcomes.find(95);
    const auto small = collector.outcomes.find(96);
    require(large != collector.outcomes.end() && small != collector.outcomes.end(),
            "both outcomes exist");
    require(small->second.ended, "small response completed");
    require(small->second.body == "small-ok",
            "small response content correct");
    // True round-robin: the small response's terminal arrives before
    // the large response's, regardless of request id order.
    require(collector.completion_order.size() == 2,
            "both terminals arrived");
    require(collector.completion_order[0] == 96,
            "small response terminal arrived first");
    capsid_worker_destroy(worker);
}

// #13: call-time snapshot semantics: a plain (non-bytes) stream keeps
// the chunk writable after enqueue, so the application mutates the
// array after the write call; the response must carry the bytes as
// they were at the write call (0x55). Without the snapshot the later
// mutation (0xaa) would leak into the response, so this test is RED
// when the JS_NewUint8ArrayCopy snapshot is removed.
void test_pending_mutation_snapshot(const char *worker_path,
                                    const std::string &bundle) {
    capsid_worker *worker =
        spawn_with(worker_path, bundle, 4096, 1024, 2000);
    require_result(
        capsid_worker_begin_request(
            worker, 97, "GET",
            "https://example.test/mutate-after-write", NULL, 0),
        "begin mutation test");
    require_result(
        capsid_worker_end_request(worker, 97),
        "end mutation test");
    Collector collector = drive(worker, { 97 }, GrantMode::kImmediate, 5);
    const auto it = collector.outcomes.find(97);
    require(it != collector.outcomes.end() && it->second.ended,
            "mutation response completed");
    require(it->second.body.size() == 40000,
            "mutation response size");
    for (size_t i = 0; i < 20000; ++i) {
        if (static_cast<uint8_t>(it->second.body[i]) != 0x55) {
            fail("first chunk must carry enqueue-time bytes (0x55)");
        }
    }
    for (size_t i = 20000; i < 40000; ++i) {
        if (static_cast<uint8_t>(it->second.body[i]) != 0x66) {
            fail("second chunk content wrong");
        }
    }
    capsid_worker_destroy(worker);
}

// #14: sustained large-response concurrency completes correctly and
// leaves the worker usable. (The old variant injected a
// CAPSID_TEST_PARTIAL_WRITE env hook that the worker never received and
// asserted RSS; the exact physical high-water is covered by
// test_outbound_buffer instead.)
void test_partial_write_high_water(const char *worker_path,
                                   const std::string &bundle) {
    capsid_worker *worker =
        spawn_with(worker_path, bundle, 4u * 1024u * 1024u, 16384, 5000);
    std::vector<uint64_t> ids;
    for (uint64_t id = 98; id < 98 + 16; ++id) {
        require_result(
            capsid_worker_begin_request(
                worker, id, "GET", "https://example.test/chunk-65537",
                NULL, 0),
            "begin stress request");
        require_result(
            capsid_worker_end_request(worker, id),
            "end stress request");
        ids.push_back(id);
    }
    Collector collector = drive(worker, ids, GrantMode::kImmediate, 10);
    require(!collector.saw_exit, "worker must not exit");
    for (uint64_t id : ids) {
        expect_body(collector, id, 65537, 0x52);
    }
    capsid_worker_destroy(worker);
}

// #15/#16: the untouched-string final shortcut is an internal detail.
// Once application code observes or replaces Response.body, bootstrap must
// use the normal stream path and preserve the application-visible bytes.
void test_accessed_string_body_falls_back(const char *worker_path,
                                          const std::string &bundle) {
    capsid_worker *worker =
        spawn_with(worker_path, bundle, 64u * 1024u, 16384, 2000);
    require_result(
        capsid_worker_begin_request(
            worker, 114, "GET",
            "https://example.test/string-body-accessed", NULL, 0),
        "begin accessed string body");
    require_result(
        capsid_worker_end_request(worker, 114),
        "end accessed string body");
    Collector collector = drive(worker, { 114 }, GrantMode::kImmediate, 3);
    const auto it = collector.outcomes.find(114);
    require(it != collector.outcomes.end() && it->second.ended,
            "accessed string response completed");
    require(it->second.body == "accessed-ok",
            "accessed string body preserved");
    capsid_worker_destroy(worker);
}

void test_replaced_string_body_falls_back(const char *worker_path,
                                          const std::string &bundle) {
    capsid_worker *worker =
        spawn_with(worker_path, bundle, 64u * 1024u, 16384, 2000);
    require_result(
        capsid_worker_begin_request(
            worker, 115, "GET",
            "https://example.test/string-body-replaced", NULL, 0),
        "begin replaced string body");
    require_result(
        capsid_worker_end_request(worker, 115),
        "end replaced string body");
    Collector collector = drive(worker, { 115 }, GrantMode::kImmediate, 3);
    const auto it = collector.outcomes.find(115);
    require(it != collector.outcomes.end() && it->second.ended,
            "replaced string response completed");
    require(it->second.body == "replacement-ok",
            "replacement stream body preserved");
    capsid_worker_destroy(worker);
}

// #17: untouched strings take the native final path without changing the
// Encoding Standard's UTF-8 semantics. In particular, isolated UTF-16
// surrogates become U+FFFD, valid pairs remain one scalar, and embedded NUL
// bytes are length-delimited rather than truncated.
void test_native_string_final_utf8(const char *worker_path,
                                   const std::string &bundle) {
    capsid_worker *worker =
        spawn_with(worker_path, bundle, 64u * 1024u, 16384, 2000);
    struct Probe {
        uint64_t id;
        const char *path;
        std::string expected;
    };
    const Probe probes[] = {
        { 116, "/string-native-ascii", "ascii-fast" },
        { 117, "/string-native-unicode",
          std::string("A\xc3\xa9\xe4\xb8\xad\xf0\x9f\x98\x80Z", 11) },
        { 118, "/string-native-surrogates",
          std::string("\xef\xbf\xbd" "A" "\xef\xbf\xbd"
                      "\xf0\x9f\x98\x80", 11) },
        { 119, "/string-native-nul", std::string("a\0b", 3) },
        { 120, "/string-materialized-surrogates",
          std::string("\xef\xbf\xbd" "A" "\xef\xbf\xbd"
                      "\xf0\x9f\x98\x80", 11) },
    };
    std::vector<uint64_t> ids;
    for (const Probe &probe : probes) {
        const std::string url =
            std::string("https://example.test") + probe.path;
        require_result(
            capsid_worker_begin_request(
                worker, probe.id, "GET", url.c_str(), NULL, 0),
            "begin native string probe");
        require_result(
            capsid_worker_end_request(worker, probe.id),
            "end native string probe");
        ids.push_back(probe.id);
    }
    Collector collector = drive(worker, ids, GrantMode::kImmediate, 3);
    for (const Probe &probe : probes) {
        const auto it = collector.outcomes.find(probe.id);
        require(it != collector.outcomes.end() && it->second.ended,
                "native string response completed");
        require(!it->second.errored, "native string response not errored");
        require(it->second.body == probe.expected,
                "native string UTF-8 bytes preserved");
    }
    capsid_worker_destroy(worker);
}

// #18: establish sixteen producers before starting the deadline subject.
// The original 64-request case remains #11. This case isolates the full
// wire queue from the cost of dispatching 64 ASan-instrumented handlers.
// Each producer contributes a real 64 KiB pump quantum to the 1 MiB queue.
void test_established_timeout_pressure(const char *worker_path,
                                       const std::string &bundle) {
    capsid_worker *worker =
        spawn_with(worker_path, bundle, 1024u * 1024u, 16u * 1024u, 300);
#if defined(_WIN32)
    // The controlled writer cannot piggyback ACKs on body output. Publish
    // this test's small input frames without Nagle batching delays.
    const SOCKET input_socket = static_cast<SOCKET>(_get_osfhandle(capsid_worker_fd(worker)));
    const int no_delay = 1;
    int observed_no_delay = 0;
    int option_size = sizeof(observed_no_delay);
    require(setsockopt(input_socket, IPPROTO_TCP, TCP_NODELAY,
                      reinterpret_cast<const char *>(&no_delay), sizeof(no_delay)) == 0,
            "disable batching on the private pressure input socket");
    require(getsockopt(input_socket, IPPROTO_TCP, TCP_NODELAY,
                      reinterpret_cast<char *>(&observed_no_delay), &option_size) == 0 &&
            observed_no_delay == 1, "observe unbatched private pressure input");
    std::fprintf(stderr, "QUEUE_FIXTURE input_nodelay=%d\n", observed_no_delay);
#endif
    std::vector<uint64_t> ids{92};
    for (uint64_t id = 93; id < 109; ++id) {
        require_result(capsid_worker_begin_request(
            worker, id, "GET", "https://example.test/timeout-pressure-body", NULL, 0),
            "begin pressure producer");
        require_result(capsid_worker_end_request(worker, id), "end pressure producer");
        ids.push_back(id);
    }
    const auto first_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    Collector phase_a;
    std::set<uint64_t> seen_first;
    while (std::chrono::steady_clock::now() < first_deadline && seen_first.size() != 16) {
        pump(worker);
        capsid_event event;
        while (next_event(worker, &event)) {
            collect_event(phase_a, event);
            if (event.type == CAPSID_EVENT_RESPONSE_BODY) seen_first.insert(event.request_id);
        }
    }
    require(seen_first.size() == 16, "all sixteen producers established their first windows");
    for (uint64_t id = 93; id < 109; ++id) {
        require(phase_a.outcomes[id].body.size() == 16384 && phase_a.terminal_counts[id] == 0,
                "producer still live after its original initial window");
    }
    require_result(capsid_worker_begin_request(
        worker, 92, "GET", "https://example.test/hang", NULL, 0), "begin pressure hang");
    require_result(capsid_worker_end_request(worker, 92), "end pressure hang");
    for (uint64_t id = 93; id < 109; ++id) {
        require_result(capsid_worker_grant_response_credit(worker, id, 128u * 1024u),
                       "grant established producers enough for a full native pump pass");
    }
    bool flushed = false;
    while (std::chrono::steady_clock::now() < first_deadline && !flushed) {
        const capsid_result result = capsid_worker_flush(worker);
        if (result == CAPSID_OK) flushed = true;
        else if (result == CAPSID_WOULD_BLOCK) {
            capsid_pollfd descriptor = {};
            descriptor.fd = capsid_worker_fd(worker); descriptor.events = POLLOUT;
            capsid::win32::capsid_poll(&descriptor, 1, 10);
        } else require_result(result, "publish pressure input and credit");
    }
    require(flushed, "pressure input reached the Worker");
    const auto held_until = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
    while (std::chrono::steady_clock::now() < held_until) {
        capsid_pollfd descriptor = {};
        descriptor.fd = capsid_worker_fd(worker);
        capsid::win32::capsid_poll(&descriptor, 1, 10);
    }
    Collector collector = drive(worker, ids, GrantMode::kImmediate, 15, phase_a);
    require(!collector.saw_exit && collector.outcomes.size() == ids.size(),
            "all submitted pressure requests survived to a terminal");
    for (uint64_t id : ids) require(collector.terminal_counts[id] == 1,
                                  "each pressure request produced exactly one terminal");
    require(collector.outcomes[92].errored && !collector.outcomes[92].ended,
            "hang reached its unchanged request deadline");
    require_result(capsid_worker_begin_request(
        worker, 158, "GET", "https://example.test/hang-count", NULL, 0), "begin abort-count probe");
    require_result(capsid_worker_end_request(worker, 158), "end abort-count probe");
    Collector probe = drive(worker, {158}, GrantMode::kImmediate, 3);
    require(probe.outcomes[158].ended && !probe.outcomes[158].errored &&
            probe.outcomes[158].body == "1111" && probe.terminal_counts[158] == 1,
            "abort bridge delivered once and the same Worker remains usable");
    require(capsid_test::shutdown_and_wait(worker, 5000).clean(),
            "established pressure Worker exited cleanly with its fixture verdict");
    capsid_worker_destroy(worker);
}

}  // namespace

// Optional argv[3]: run only the numbered test (1..18). Without it the
// original seventeen cases run in order. Case 18 requires the explicit
// controlled-writer selector used by worker_response_timeout_queue_pressure.
// A failing test stops the run, so the
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
    run_sel(only, argv[1], bundle, 9, test_huge_error_message);
    run_sel(only, argv[1], bundle, 10, test_error_after_partial_write);
    run_sel(only, argv[1], bundle, 11, test_timeout_once_under_saturation);
    run_sel(only, argv[1], bundle, 12, test_small_completes_first);
    run_sel(only, argv[1], bundle, 13, test_pending_mutation_snapshot);
    run_sel(only, argv[1], bundle, 14, test_partial_write_high_water);
    run_sel(only, argv[1], bundle, 15, test_accessed_string_body_falls_back);
    run_sel(only, argv[1], bundle, 16, test_replaced_string_body_falls_back);
    run_sel(only, argv[1], bundle, 17, test_native_string_final_utf8);
    if (only == "18") {
        test_established_timeout_pressure(argv[1], bundle);
    }
    std::printf("all queue-saturation RED tests passed\n");
    return 0;
}
