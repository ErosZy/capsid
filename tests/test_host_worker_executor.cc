// WP-04 PR-06 (spec §8.1): the WorkerExecutor owns everything that talks
// to one capsid_worker process — the capsid_worker handle, the
// WorkerEventSource, the command queue, the event queue and the worker
// thread — and exposes only submit/cancel/grant, an event callback,
// health, inflight and stop/wait. capsid_worker_destroy runs exclusively
// on the executor's reaper thread, exactly once.
//
// PR-06 gate (spec §8.4): the SingleWorkerServer tests are the regression
// gate for the extraction; this test pins the NEW public contract that the
// extraction exists to provide:
//   - startup failure: a factory that rejects leaves no thread, no worker
//     and a stop_and_join() that is a safe no-op; a second start is
//     rejected;
//   - factory lifecycle: spawn/load/READY through the factory, a request
//     round-trip (begin → head/body/end with credit grants → inflight
//     returns to 0), the cancel path returns inflight to 0, and shutdown
//     exits the worker and reaps it;
//   - adopt: an already-READY worker (spawned/loaded through the C API
//     directly) can be handed over and serves a request;
//   - exactly-once destroy: after the exit path reaps the worker,
//     worker() is null and repeated stop_and_join() is a no-op.

#include "host/worker_executor.h"

#include "capsid/runtime.h"
#include "protocol.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <functional>
#include <iostream>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {

using capsid::host::Command;
using capsid::host::CommandType;
using capsid::host::WorkerEvent;
using capsid::host::WorkerExecutor;
using capsid::host::decode_worker_log_event;

[[noreturn]] void fail(const std::string& message) {
    std::cerr << "FAIL " << message << std::endl;
    std::exit(1);
}

void require(bool condition, const std::string& message) {
    if (!condition) {
        fail(message);
    }
}

const char* kBundle =
    "export default { async fetch(request) {"
    " return new Response('hello-executor'); } };\n";

void append_string16(std::vector<std::uint8_t>* output,
                     const std::string& value) {
    capsid::protocol::append_u16(
        output, static_cast<std::uint16_t>(value.size()));
    output->insert(output->end(), value.begin(), value.end());
}

void append_string32(std::vector<std::uint8_t>* output,
                     const std::string& value) {
    capsid::protocol::append_u32(
        output, static_cast<std::uint32_t>(value.size()));
    output->insert(output->end(), value.begin(), value.end());
}

capsid_event binding_log_event(const std::vector<std::uint8_t>& payload,
                               std::uint32_t flags =
                                   capsid::protocol::kFlagBindingLog) {
    capsid_event event = {};
    event.struct_size = sizeof(event);
    event.type = CAPSID_EVENT_LOG;
    event.flags = flags;
    event.request_id = 108;
    event.payload.data = payload.empty() ? nullptr : payload.data();
    event.payload.size = payload.size();
    return event;
}

void test_binding_log_decoder() {
    std::vector<std::uint8_t> payload;
    append_string16(&payload, "mongo");
    append_string16(&payload, "warn");
    append_string32(&payload, "visible");
    append_string32(
        &payload,
        "{\"password\":\"[REDACTED]\",\"safe\":\"yes\"}");
    WorkerEvent decoded;
    std::string error;
    require(decode_worker_log_event(
                binding_log_event(payload), &decoded, &error),
            "valid Binding log rejected: " + error);
    require(decoded.binding_log && decoded.binding_id == "mongo" &&
                decoded.log_level == "warn" && decoded.request_id == 108 &&
                decoded.text == "visible" &&
                decoded.log_fields_json.find("[REDACTED]") !=
                    std::string::npos,
            "Binding log envelope decoded incorrectly");

    const std::vector<std::uint8_t> empty;
    require(!decode_worker_log_event(
                binding_log_event(empty), &decoded, &error),
            "empty Binding log envelope accepted");
    require(!decode_worker_log_event(
                binding_log_event(payload, 0x80000000U), &decoded, &error),
            "unknown LOG flag accepted");

    std::vector<std::uint8_t> duplicate_fields;
    append_string16(&duplicate_fields, "mongo");
    append_string16(&duplicate_fields, "warn");
    append_string32(&duplicate_fields, "visible");
    append_string32(&duplicate_fields, "{\"x\":1,\"x\":2}");
    require(!decode_worker_log_event(
                binding_log_event(duplicate_fields), &decoded, &error),
            "duplicate Binding log field accepted");

    capsid_event ordinary = {};
    ordinary.struct_size = sizeof(ordinary);
    ordinary.type = CAPSID_EVENT_LOG;
    require(decode_worker_log_event(ordinary, &decoded, &error) &&
                !decoded.binding_log && decoded.text.empty(),
            "empty ordinary log did not decode safely");
    std::cout << "PASS: strict Binding log decoder" << std::endl;
}

// Wakes the waiting test thread whenever the executor queues a new event
// batch (the notifier runs on the worker thread).
class EventHarness {
public:
    explicit EventHarness(WorkerExecutor& executor) {
        executor.set_event_notifier([this] {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                signaled_ = true;
            }
            cv_.notify_all();
        });
    }

    // Drains the executor's event queue into `events` and returns when
    // `done` is satisfied; fails the test on a 15s deadline.
    void observe(
        WorkerExecutor& executor,
        std::vector<WorkerEvent>& events,
        const std::function<bool(const std::vector<WorkerEvent>&)>& done) {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(15);
        for (;;) {
            std::deque<WorkerEvent> batch = executor.drain_events();
            while (!batch.empty()) {
                events.push_back(std::move(batch.front()));
                batch.pop_front();
            }
            if (done(events)) {
                return;
            }
            std::unique_lock<std::mutex> lock(mutex_);
            if (signaled_) {
                // The notifier fired after the drain above: events are
                // queued, loop again to pick them up.
                signaled_ = false;
                continue;
            }
            if (!cv_.wait_until(lock, deadline,
                                [this] { return signaled_; })) {
                fail("event deadline expired");
            }
            signaled_ = false;
        }
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    bool signaled_ = false;
};

bool has_response_head_for(const std::vector<WorkerEvent>& events,
                           std::uint64_t request_id) {
    for (const WorkerEvent& event : events) {
        if (event.type == WorkerEvent::Type::kResponseHead &&
            event.request_id == request_id) {
            return true;
        }
    }
    return false;
}

bool has_fixed_response_head_for(
    const std::vector<WorkerEvent>& events,
    std::uint64_t request_id,
    std::uint32_t body_size) {
    for (const WorkerEvent& event : events) {
        if (event.type == WorkerEvent::Type::kResponseHead &&
            event.request_id == request_id && event.fixed_body &&
            event.fixed_body_size == body_size) {
            return true;
        }
    }
    return false;
}

// Submits a bodyless begin request and observes the round trip, granting
// response credit for every body frame exactly once. Returns when
// RESPONSE_END for `request_id` has arrived (or fails the test).
void begin_and_observe(WorkerExecutor& executor,
                       EventHarness& harness,
                       std::uint64_t request_id,
                       std::vector<WorkerEvent>& events) {
    Command begin;
    begin.type = CommandType::kBeginRequest;
    begin.request_id = request_id;
    begin.method = "GET";
    begin.url = "https://executor.invalid/hello";
    begin.end_request = true;  // bodyless fusion: begin + end in one frame
    executor.submit(std::move(begin));
    std::set<std::uint64_t> granted;
    harness.observe(executor, events,
                    [&](const std::vector<WorkerEvent>& all) {
                        for (const WorkerEvent& event : all) {
                            if (event.type ==
                                    WorkerEvent::Type::kResponseBody &&
                                granted.insert(event.request_id).second) {
                                Command grant;
                                grant.type =
                                    CommandType::kGrantResponseCredit;
                                grant.request_id = event.request_id;
                                grant.credit = static_cast<std::uint32_t>(
                                    event.body.size());
                                executor.submit(std::move(grant));
                            }
                        }
                        for (const WorkerEvent& event : all) {
                            if (event.type ==
                                    WorkerEvent::Type::kResponseEnd &&
                                event.request_id == request_id) {
                                return true;
                            }
                        }
                        return false;
                    });
}

void test_startup_failure() {
    WorkerExecutor executor;
    std::string error;
    bool factory_called = false;
    WorkerExecutor::WorkerFactory factory =
        [&factory_called](capsid_worker** out,
                          std::string* factory_error) -> bool {
        (void)out;
        factory_called = true;
        *factory_error = "injected spawn failure";
        return false;
    };
    require(!executor.start(factory, &error),
            "start must fail when the factory rejects");
    require(factory_called, "factory must run");
    require(!executor.available(),
            "no available worker after failed start");
    require(executor.inflight() == 0, "inflight 0 after failed start");
    // No thread was ever started: stop_and_join is a safe no-op and a
    // second start is rejected instead of double-owning a worker.
    executor.stop_and_join();
    require(!executor.start(factory, &error), "second start must fail");
    require(!executor.adopt(nullptr, &error), "adopt(nullptr) must fail");
    std::cout << "PASS: startup failure leaves nothing behind" << std::endl;
}

WorkerExecutor::WorkerFactory hello_factory(
    const std::string& worker_path,
    std::uint32_t initial_stream_window = 0) {
    const std::string bundle(kBundle);
    return [worker_path, initial_stream_window, bundle](capsid_worker** out,
                         std::string* factory_error) -> bool {
        capsid_worker_config config;
        capsid_worker_config_init(&config);
        config.worker_path = worker_path.c_str();
        config.request_timeout_ms = 2000;
        config.initial_stream_window = initial_stream_window;
        capsid_worker* worker = nullptr;
        if (capsid_worker_spawn(&config, &worker) != CAPSID_OK) {
            *factory_error = "worker spawn failed";
            return false;
        }
        if (capsid_worker_load_bundle(
                worker,
                reinterpret_cast<const std::uint8_t*>(bundle.data()),
                bundle.size()) != CAPSID_OK) {
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

WorkerExecutor::WorkerFactory bundle_factory(
    const std::string& worker_path,
    std::string bundle) {
    return [worker_path, bundle = std::move(bundle)](
               capsid_worker** out,
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
                worker,
                reinterpret_cast<const std::uint8_t*>(bundle.data()),
                bundle.size()) != CAPSID_OK ||
            capsid_worker_flush(worker) != CAPSID_OK) {
            capsid_worker_destroy(worker);
            *factory_error = "bundle startup failed";
            return false;
        }
        *out = worker;
        return true;
    };
}

void test_log_identity_is_host_owned(const std::string& worker_path) {
    WorkerExecutor executor;
    executor.set_log_identity("orders", "sha256:generation");
    EventHarness harness(executor);
    std::string error;
    require(executor.start(
                bundle_factory(
                    worker_path,
                    "console.log('identity-log');"
                    "export default { fetch() {"
                    " return new Response('ok'); } };\n"),
                &error),
            "log identity worker start: " + error);
    std::vector<WorkerEvent> events;
    harness.observe(
        executor, events,
        [](const std::vector<WorkerEvent>& observed) {
            for (const WorkerEvent& event : observed) {
                if (event.type == WorkerEvent::Type::kLog &&
                    event.text.find("identity-log") != std::string::npos) {
                    return true;
                }
            }
            return false;
        });
    bool verified = false;
    for (const WorkerEvent& event : events) {
        if (event.type == WorkerEvent::Type::kLog &&
            event.text.find("identity-log") != std::string::npos) {
            verified = event.application_id == "orders" &&
                       event.generation_digest == "sha256:generation";
        }
    }
    require(verified, "worker-controlled LOG lost Host-owned identity");
    executor.stop_and_join();
    std::cout << "PASS: LOG identity is attached by the Host" << std::endl;
}

void test_factory_ready_proof_is_not_compatibility_only(
    const std::string& worker_path) {
    WorkerExecutor::WorkerFactory factory = hello_factory(worker_path);
    factory.expected_ready.extended = true;
    factory.expected_ready.applied_feature_bits =
        CAPSID_SANDBOX_FEATURE_RLIMITS;
    factory.expected_ready.sandbox_profile_digest =
        "sha256:" + std::string(64, 'a');

    WorkerExecutor executor;
    std::string error;
    require(!executor.start(factory, &error),
            "compatibility-only READY was accepted for a Binding factory");
    require(error.find("sandbox proof") != std::string::npos,
            "Binding READY rejection lost its proof diagnostic: " + error);
    require(!executor.available(),
            "proof-mismatched replacement became available");
    std::cout << "PASS: factory pins the complete READY proof" << std::endl;
}

void test_factory_lifecycle(const std::string& worker_path) {
    WorkerExecutor executor;
    executor.set_metrics_enabled(true);
    EventHarness harness(executor);
    std::string error;
    require(executor.start(hello_factory(worker_path), &error),
            "start: " + error);
    require(executor.available(), "executor must be available after start");
    require(executor.inflight() == 0, "inflight starts at 0");

    // Request lifecycle: begin → head/body/end → inflight back to 0.
    std::vector<WorkerEvent> events;
    begin_and_observe(executor, harness, 7, events);
    require(has_response_head_for(events, 7),
            "missing RESPONSE_HEAD for request 7");
    require(has_fixed_response_head_for(events, 7, 14),
            "non-streamed response missing exact fixed-body metadata");
    require(executor.inflight() == 0, "inflight 0 after response end");

    // Owner acknowledgement of a Runtime-terminal response is local queue
    // retirement, not another command/wake or a redundant Runtime cancel.
    const auto submitted_before_retire =
        executor.metrics().commands_submitted.load(std::memory_order_relaxed);
    const auto batches_before_retire =
        executor.metrics().command_batches.load(std::memory_order_relaxed);
    executor.retire_terminal_request(7);
    require(executor.metrics().commands_submitted.load(
                std::memory_order_relaxed) == submitted_before_retire,
            "terminal retirement submitted a command");
    require(executor.metrics().command_batches.load(
                std::memory_order_relaxed) == batches_before_retire,
            "terminal retirement emitted a wake");

    // The next request proves that an old response-credit command, whether
    // still shared or already swapped locally, cannot poison the worker.
    events.clear();
    begin_and_observe(executor, harness, 9, events);
    require(has_response_head_for(events, 9),
            "worker unavailable after terminal retirement");
    executor.retire_terminal_request(9);

    // Cancel path: a begun request returns to inflight 0 at mark_canceled
    // (the host no longer counts it), and the cancel frame reaches the
    // worker.
    Command begin;
    begin.type = CommandType::kBeginRequest;
    begin.request_id = 8;
    begin.method = "GET";
    begin.url = "https://executor.invalid/hello";
    begin.end_request = true;
    executor.submit(std::move(begin));
    require(executor.inflight() == 1, "inflight 1 after begin");
    executor.mark_canceled(8);
    require(executor.inflight() == 0, "inflight 0 after mark_canceled");
    Command cancel;
    cancel.type = CommandType::kCancel;
    cancel.request_id = 8;
    executor.submit(std::move(cancel));

    // Shutdown: the worker exits and the reaper destroys it exactly once.
    executor.request_shutdown();
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (!executor.exited()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            fail("worker did not exit after request_shutdown");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    require(!executor.available(), "not available after exit");
    require(executor.worker() == nullptr, "worker reaped exactly once");
    executor.stop_and_join();
    executor.stop_and_join();  // idempotent: no second destroy
    std::cout << "PASS: factory lifecycle + exactly-once reap" << std::endl;
}

void test_fixed_response_requires_credit(const std::string& worker_path) {
    WorkerExecutor executor;
    EventHarness harness(executor);
    std::string error;
    require(executor.start(hello_factory(worker_path, 8), &error),
            "low-window start: " + error);

    std::vector<WorkerEvent> events;
    begin_and_observe(executor, harness, 10, events);
    require(has_response_head_for(events, 10),
            "low-window response missing head");
    require(!has_fixed_response_head_for(events, 10, 14),
            "fixed response exceeded available response credit");
    require(executor.inflight() == 0,
            "low-window response did not drain through ordinary credit");

    executor.stop_and_join();
    std::cout << "PASS: fixed response falls back below body credit"
              << std::endl;
}

void test_adopt_ready(const std::string& worker_path) {
    // Spawn + load + wait for READY through the C API directly, then hand
    // the live worker to a fresh executor (the §8.1 adopted-worker path).
    capsid_worker_config config;
    capsid_worker_config_init(&config);
    config.worker_path = worker_path.c_str();
    config.request_timeout_ms = 2000;
    capsid_worker* worker = nullptr;
    require(capsid_worker_spawn(&config, &worker) == CAPSID_OK, "spawn");
    require(capsid_worker_load_bundle(
                worker, reinterpret_cast<const std::uint8_t*>(kBundle),
                std::char_traits<char>::length(kBundle)) == CAPSID_OK,
            "load");
    require(capsid_worker_flush(worker) == CAPSID_OK, "flush");
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(15);
    for (;;) {
        const capsid_result flush = capsid_worker_flush(worker);
        require(flush == CAPSID_OK || flush == CAPSID_WOULD_BLOCK,
                "flush while waiting for READY");
        capsid_event event = {};
        event.struct_size = sizeof(event);
        const capsid_result result =
            capsid_worker_next_event(worker, &event);
        if (result == CAPSID_OK && event.type == CAPSID_EVENT_READY) {
            break;
        }
        if (result == CAPSID_OK &&
            (event.type == CAPSID_EVENT_ERROR ||
             event.type == CAPSID_EVENT_EXIT)) {
            fail("startup event before READY");
        }
        if (result != CAPSID_OK && result != CAPSID_WOULD_BLOCK) {
            fail("next event while waiting for READY");
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            fail("READY deadline expired");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    WorkerExecutor executor;
    EventHarness harness(executor);
    std::string error;
    require(executor.adopt(worker, &error), "adopt: " + error);
    require(executor.available(), "available after adopt");

    std::vector<WorkerEvent> events;
    begin_and_observe(executor, harness, 9, events);
    require(has_response_head_for(events, 9),
            "missing RESPONSE_HEAD for request 9");
    require(executor.inflight() == 0,
            "inflight 0 after adopted lifecycle");

    executor.stop_and_join();  // blocks until the worker thread reaped it
    require(executor.exited(), "exited after stop_and_join");
    require(executor.worker() == nullptr, "adopted worker reaped");
    std::cout << "PASS: adopted READY worker lifecycle" << std::endl;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        fail("expected capsid-worker path");
    }
    const std::string worker_path = argv[1];
    test_binding_log_decoder();
    test_log_identity_is_host_owned(worker_path);
    test_startup_failure();
    test_factory_ready_proof_is_not_compatibility_only(worker_path);
    test_factory_lifecycle(worker_path);
    test_fixed_response_requires_credit(worker_path);
    test_adopt_ready(worker_path);
    std::cout << "PASS: WorkerExecutor ownership contract (WP-04 §8.1)"
              << std::endl;
    return 0;
}
