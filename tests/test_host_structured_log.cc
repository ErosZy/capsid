// M2 item 7 structured-log RED suite (design §12.2).
//
// Locks the two-lane contract: every line is one JSON object with the
// fixed field set; the app lane is bounded and drops on overflow WITH a
// count (the log-drop metric); the control lane is bounded and NEVER
// drops — an overflowing control lane writes through to the sink
// synchronously, so CRASH_BUDGET_EXCEEDED, quarantine/retired
// transitions, retire drain timeouts and admin authorization failures
// always land even under a stalled sink.

#if __has_include("host/structured_log.h")
#include "host/structured_log.h"
#define CAPSID_HAS_STRUCTURED_LOG 1
#else
#define CAPSID_HAS_STRUCTURED_LOG 0
#endif

#include <chrono>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <mutex>
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

#if CAPSID_HAS_STRUCTURED_LOG

using capsid::host::LogFields;
using capsid::host::LogLane;
using capsid::host::StructuredLog;
using capsid::host::encode_log_line;

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

int mode_encode_line_json() {
    // One line per event, fixed fields present, empty fields omitted,
    // special characters escaped so the line stays single-line JSON.
    LogFields fields;
    fields.level = "warn";
    fields.event = "recovery_decision";
    fields.app = "orders";
    fields.generation = "sha256:ab";
    fields.binding = "mongo";
    fields.request_id = "108";
    fields.result = "quarantine";
    fields.fields = "{\"safe\":\"visible\",\"password\":\"[REDACTED]\"}";
    fields.duration_ms = 42;
    fields.message = "line one\n\"quoted\" \\ tail";
    const std::string line = encode_log_line(fields, 1700000000123ULL);
    require(contains(line, "{\"timestamp\":\"1700000000123\""),
            "encode omitted the timestamp: " + line);
    require(contains(line, "\"level\":\"warn\""), "encode omitted level");
    require(contains(line, "\"event\":\"recovery_decision\""),
            "encode omitted event");
    require(contains(line, "\"app\":\"orders\""), "encode omitted app");
    require(contains(line, "\"generation\":\"sha256:ab\""),
            "encode omitted generation");
    require(contains(line, "\"binding\":\"mongo\""),
            "encode omitted Binding identity");
    require(contains(line, "\"request_id\":\"108\""),
            "encode omitted request identity");
    require(contains(
                line,
                "\"fields\":{\"safe\":\"visible\","
                "\"password\":\"[REDACTED]\"}"),
            "encode did not preserve structured fields: " + line);
    require(!contains(line, "\"fields\":\"{"),
            "encode double-encoded structured fields");
    require(contains(line, "\"result\":\"quarantine\""),
            "encode omitted result");
    require(contains(line, "\"duration_ms\":42"),
            "encode omitted duration_ms");
    require(contains(line, "\"message\":\"line one\\n\\\"quoted\\\" \\\\ tail\""),
            "encode did not escape the message: " + line);
    require(line.find('\n') == std::string::npos,
            "encode emitted a multi-line record");
    require(!contains(line, "\"version\""),
            "encode emitted an empty version field");
    require(!contains(line, "\"worker_id\""),
            "encode emitted an empty worker_id field");
    std::cout << "PASS" << std::endl;
    return 0;
}

int mode_app_lane_drops_and_counts() {
    // A stalled sink + an over-capacity app lane: events drop, the drop
    // counter rises, and every accepted event is either delivered or
    // counted. The writer thread never blocks the caller.
    std::mutex sink_mutex;
    std::vector<std::string> received;
    StructuredLog log(
        [&](const std::string& line) {
            {
                std::lock_guard<std::mutex> lock(sink_mutex);
                received.push_back(line);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        },
        /*app_capacity=*/8, /*control_capacity=*/4);
    constexpr std::size_t kTotal = 200;
    LogFields fields;
    fields.event = "app_log";
    fields.app = "orders";
    for (std::size_t index = 0; index < kTotal; ++index) {
        fields.message = "event " + std::to_string(index);
        log.log(LogLane::kApp, fields);
    }
    log.flush();
    std::size_t delivered = 0;
    {
        std::lock_guard<std::mutex> lock(sink_mutex);
        delivered = received.size();
    }
    require(log.accepted_events() == kTotal,
            "accepted count mismatches submitted events");
    require(delivered + log.dropped_app_events() == kTotal,
            "app lane lost events without counting them: delivered=" +
                std::to_string(delivered) +
                " dropped=" + std::to_string(log.dropped_app_events()));
    require(log.dropped_app_events() > 0,
            "app lane overflow was never counted");
    std::cout << "PASS" << std::endl;
    return 0;
}

int mode_control_lane_never_drops() {
    // The same stalled sink with an over-capacity control lane: every
    // control event lands (write-through), nothing is dropped or counted.
    std::mutex sink_mutex;
    std::vector<std::string> received;
    StructuredLog log(
        [&](const std::string& line) {
            {
                std::lock_guard<std::mutex> lock(sink_mutex);
                received.push_back(line);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        },
        /*app_capacity=*/8, /*control_capacity=*/4);
    constexpr std::size_t kTotal = 200;
    LogFields fields;
    fields.event = "quarantine";
    fields.app = "orders";
    fields.result = "crash_budget_exceeded";
    for (std::size_t index = 0; index < kTotal; ++index) {
        fields.message = "control " + std::to_string(index);
        log.log(LogLane::kControl, fields);
    }
    log.flush();
    std::size_t delivered = 0;
    {
        std::lock_guard<std::mutex> lock(sink_mutex);
        delivered = received.size();
    }
    require(delivered == kTotal,
            "control lane dropped an event: delivered=" +
                std::to_string(delivered) +
                " expected=" + std::to_string(kTotal));
    require(log.dropped_app_events() == 0,
            "control overflow was wrongly counted as an app drop");
    require(log.accepted_events() == kTotal,
            "control accepted count mismatches submitted events");
    std::cout << "PASS" << std::endl;
    return 0;
}

int mode_control_precedes_app_backlog() {
    // A control event never waits behind an app-log backlog: with a
    // stalled sink and a full app queue, the control event still arrives
    // promptly (bounded by the write-through, not by the backlog).
    std::mutex sink_mutex;
    std::vector<std::string> received;
    StructuredLog log(
        [&](const std::string& line) {
            {
                std::lock_guard<std::mutex> lock(sink_mutex);
                received.push_back(line);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        },
        /*app_capacity=*/8, /*control_capacity=*/4);
    LogFields app_fields;
    app_fields.event = "app_log";
    app_fields.app = "orders";
    for (std::size_t index = 0; index < 8; ++index) {
        app_fields.message = "app " + std::to_string(index);
        log.log(LogLane::kApp, app_fields);
    }
    LogFields control_fields;
    control_fields.event = "admin_auth";
    control_fields.app = "orders";
    control_fields.result = "denied";
    log.log(LogLane::kControl, control_fields);
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(100);
    bool control_landed = false;
    for (;;) {
        {
            std::lock_guard<std::mutex> lock(sink_mutex);
            for (const std::string& line : received) {
                if (contains(line, "\"event\":\"admin_auth\"")) {
                    control_landed = true;
                }
            }
        }
        if (control_landed) {
            break;
        }
        require(std::chrono::steady_clock::now() < deadline,
                "control event waited behind the app backlog");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    log.flush();
    std::cout << "PASS" << std::endl;
    return 0;
}

int mode_app_lane_is_fifo() {
    std::mutex sink_mutex;
    std::vector<std::string> received;
    StructuredLog log([&](const std::string& line) {
        std::lock_guard<std::mutex> lock(sink_mutex);
        received.push_back(line);
    });
    for (std::size_t index = 0; index < 3; ++index) {
        LogFields fields;
        fields.event = "app_log";
        fields.message = "fifo-" + std::to_string(index);
        log.log(LogLane::kApp, std::move(fields));
    }
    log.flush();
    require(received.size() == 3, "FIFO test lost an app event");
    for (std::size_t index = 0; index < received.size(); ++index) {
        require(contains(received[index], "fifo-" + std::to_string(index)),
                "app lane is not FIFO at index " + std::to_string(index));
    }
    std::cout << "PASS" << std::endl;
    return 0;
}

int mode_flush_idle_writer() {
    for (unsigned round = 0; round < 256; ++round) {
        StructuredLog log([](const std::string&) {
            fail("an idle logger must not fabricate an event");
        });
        std::this_thread::yield();
        log.flush();
        log.flush();
        require(log.accepted_events() == 0, "empty flush changes accepted count");
    }
    std::cout << "PASS" << std::endl;
    return 0;
}

int mode_flush_after_final_event() {
    for (unsigned round = 0; round < 256; ++round) {
        std::atomic<unsigned> delivered{0};
        StructuredLog log([&](const std::string& line) {
            require(contains(line, "\"event\":\"shutdown\"") && line.back() == '\n',
                    "final shutdown record must be delivered intact");
            delivered.fetch_add(1);
        });
        log.log(LogLane::kControl, {.event = "shutdown"});
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (delivered.load() == 0) {
            require(std::chrono::steady_clock::now() < deadline,
                    "final event must reach the real writer before flush");
            std::this_thread::yield();
        }
        log.flush();
        require(delivered.load() == 1 && log.accepted_events() == 1,
                "flush must preserve the single final event");
    }
    std::cout << "PASS" << std::endl;
    return 0;
}

int mode_concurrent_flush_joins_writer() {
    std::mutex gate;
    std::condition_variable ready;
    bool sink_entered = false, release_sink = false;
    unsigned flush_entered = 0, flush_returned = 0, delivered = 0;
    StructuredLog log([&](const std::string&) {
        std::unique_lock<std::mutex> lock(gate);
        if (!sink_entered) {
            sink_entered = true;
            ready.notify_all();
            ready.wait(lock, [&] { return release_sink; });
        }
        ++delivered;
    });
    log.log(LogLane::kControl, {.event = "startup"});
    {
        std::unique_lock<std::mutex> lock(gate);
        require(ready.wait_for(lock, std::chrono::seconds(2), [&] { return sink_entered; }),
                "real writer must enter the gated sink");
    }
    log.log(LogLane::kApp, {.event = "app_log"});
    log.log(LogLane::kControl, {.event = "shutdown"});
    const auto flush = [&] {
        {
            std::lock_guard<std::mutex> lock(gate);
            ++flush_entered;
            ready.notify_all();
        }
        log.flush();
        {
            std::lock_guard<std::mutex> lock(gate);
            ++flush_returned;
            ready.notify_all();
        }
    };
    std::thread first(flush), second(flush);
    {
        std::unique_lock<std::mutex> lock(gate);
        require(ready.wait_for(lock, std::chrono::seconds(2), [&] { return flush_entered == 2; }),
                "both concurrent flush callers must start");
        require(!ready.wait_for(lock, std::chrono::milliseconds(50), [&] { return flush_returned != 0; }),
                "every flush must wait for the writer, including concurrent callers");
        release_sink = true;
        ready.notify_all();
    }
    first.join(); second.join();
    require(flush_returned == 2 && delivered == 3 && log.accepted_events() == 3,
            "both flush calls must join the writer after both lanes drain");
    log.flush();
    std::cout << "PASS" << std::endl;
    return 0;
}

#endif  // CAPSID_HAS_STRUCTURED_LOG

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: test-host-structured-log <mode>" << std::endl;
        return 1;
    }
    const std::string mode = argv[1];
#if CAPSID_HAS_STRUCTURED_LOG
    if (mode == "structured_log_emits_single_line_json") {
        return mode_encode_line_json();
    }
    if (mode == "structured_log_app_lane_drops_and_counts") {
        return mode_app_lane_drops_and_counts();
    }
    if (mode == "structured_log_control_lane_never_drops") {
        return mode_control_lane_never_drops();
    }
    if (mode == "structured_log_control_precedes_app_backlog") {
        return mode_control_precedes_app_backlog();
    }
    if (mode == "structured_log_app_lane_is_fifo") {
        return mode_app_lane_is_fifo();
    }
    if (mode == "structured_log_flush_idle_writer") return mode_flush_idle_writer();
    if (mode == "structured_log_flush_after_final_event") return mode_flush_after_final_event();
    if (mode == "structured_log_concurrent_flush_joins_writer") return mode_concurrent_flush_joins_writer();
#else
    if (mode == "structured_log_emits_single_line_json" ||
        mode == "structured_log_app_lane_drops_and_counts" ||
        mode == "structured_log_control_lane_never_drops" ||
        mode == "structured_log_control_precedes_app_backlog" ||
        mode == "structured_log_app_lane_is_fifo" ||
        mode == "structured_log_flush_idle_writer" ||
        mode == "structured_log_flush_after_final_event" ||
        mode == "structured_log_concurrent_flush_joins_writer") {
        fail("structured log component is not implemented");
    }
#endif
    std::cerr << "unknown mode: " << mode << std::endl;
    return 1;
}
