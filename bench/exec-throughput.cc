// Execution-throughput harness for the bytecode AOT rewriter benchmark
// (Step 8, G3/G5). Drives one fixture through the identical request in
// three modes over the public capsid_worker C ABI:
//   source  -> capsid_worker_load_bundle_named(source)
//   raw     -> capsid_worker_load_trusted_bytecode_named(unoptimized)
//   opt     -> capsid_worker_load_trusted_bytecode_named(rewritten)
// Each measured round is one request; the harness times begin-request ->
// RESPONSE_END with a steady clock and emits one JSON line per round:
//   {"mode":"opt","round":2,"ms":41.37,"status":200,"body_len":9,"body":"...","ok":true}
// With --expect-body the response body is validated byte-for-byte, so the
// benchmark doubles as a cross-mode correctness check (G1 continuity).
//
// Usage:
//   exec-throughput --worker <capsid-worker> --mode source|raw|opt \
//       --input <bundle> --source-name <file://...> \
//       [--rounds N] [--warmup N] [--timeout-seconds N] \
//       [--expect-body <text>]
#include "capsid/runtime.h"

#include "win32_compat.h"
#if defined(_WIN32)
#else
#include <sys/socket.h>
#endif
#include "win32_compat.h"
#if defined(_WIN32)
#else
#include <sys/wait.h>
#endif
#include "win32_compat.h"
#if defined(_WIN32)
#else
#include <unistd.h>
#endif

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace {

void fail(const std::string& message) {
    std::fprintf(stderr, "FAIL: %s\n", message.c_str());
    std::exit(1);
}

void require_result(capsid_result result, const char* operation) {
    if (result != CAPSID_OK) {
        fail(std::string(operation) + ": " + capsid_result_string(result));
    }
}

std::string read_file(const char* path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        fail(std::string("cannot open file: ") + path);
    }
    return std::string(std::istreambuf_iterator<char>(input),
                       std::istreambuf_iterator<char>());
}

std::string json_escape(const std::string& input) {
    static const char hex[] = "0123456789abcdef";
    std::string output;
    output.reserve(input.size());
    for (unsigned char ch : input) {
        switch (ch) {
        case '"': output += "\\\""; break;
        case '\\': output += "\\\\"; break;
        case '\b': output += "\\b"; break;
        case '\f': output += "\\f"; break;
        case '\n': output += "\\n"; break;
        case '\r': output += "\\r"; break;
        case '\t': output += "\\t"; break;
        default:
            if (ch < 0x20) {
                output += "\\u00";
                output += hex[ch >> 4];
                output += hex[ch & 0x0f];
            } else {
                output += static_cast<char>(ch);
            }
        }
    }
    return output;
}

capsid_worker* spawn_worker(const char* worker_path) {
    capsid_worker_config config;
    capsid_worker_config_init(&config);
    config.worker_path = worker_path;
    capsid_worker* worker = nullptr;
    require_result(capsid_worker_spawn(&config, &worker), "spawn worker");
    return worker;
}

void wait_for_ready(capsid_worker* worker) {
    const std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(30);
    for (;;) {
        const capsid_result flush = capsid_worker_flush(worker);
        if (flush != CAPSID_OK && flush != CAPSID_WOULD_BLOCK) {
            fail(std::string("startup flush: ") + capsid_result_string(flush));
        }
        capsid_event event = {};
        event.struct_size = sizeof(event);
        const capsid_result result = capsid_worker_next_event(worker, &event);
        if (result == CAPSID_OK) {
            if (event.type == CAPSID_EVENT_READY) {
                return;
            }
            if (event.type == CAPSID_EVENT_ERROR) {
                fail(std::string("worker startup error: ") +
                     std::string(reinterpret_cast<const char*>(event.payload.data),
                                 event.payload.size));
            }
            if (event.type == CAPSID_EVENT_EXIT) {
                fail("worker exited before READY");
            }
            continue;
        }
        if (result != CAPSID_WOULD_BLOCK) {
            fail(std::string("startup event: ") + capsid_result_string(result));
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            fail("timed out waiting for READY");
        }
        capsid_pollfd descriptor = {};
        descriptor.fd = capsid_worker_fd(worker);
        descriptor.events = POLLIN | (flush == CAPSID_WOULD_BLOCK ? POLLOUT : 0);
        capsid::win32::capsid_poll(&descriptor, 1, 50);
    }
}

// Runs one request, returning the wall time from begin_request to
// RESPONSE_END and the response body. Mirrors the differential test's
// request loop exactly (same event handling, same credit flow).
double run_request(capsid_worker* worker, const char* url,
                   int timeout_seconds, std::string* body_out) {
    const std::chrono::steady_clock::time_point begin =
        std::chrono::steady_clock::now();
    require_result(
        capsid_worker_begin_bodyless_request(
            worker, 1, "GET", url, NULL, 0),
        "begin bodyless request");
    std::string body;
    bool received_head = false;
    const std::chrono::steady_clock::time_point deadline =
        begin + std::chrono::seconds(timeout_seconds);
    for (;;) {
        const capsid_result flush = capsid_worker_flush(worker);
        if (flush != CAPSID_OK && flush != CAPSID_WOULD_BLOCK) {
            fail(std::string("request flush: ") + capsid_result_string(flush));
        }
        capsid_event event = {};
        event.struct_size = sizeof(event);
        const capsid_result result = capsid_worker_next_event(worker, &event);
        if (result == CAPSID_OK) {
            if (event.type == CAPSID_EVENT_REQUEST_CREDIT) {
                continue;
            }
            if (event.type == CAPSID_EVENT_RESPONSE_HEAD) {
                if (event.request_id != 1) {
                    fail("unexpected response head");
                }
                received_head = true;
                continue;
            }
            if (event.type == CAPSID_EVENT_RESPONSE_BODY) {
                if (!received_head || event.request_id != 1) {
                    fail("response body arrived before its head");
                }
                body.append(reinterpret_cast<const char*>(event.payload.data),
                            event.payload.size);
                require_result(
                    capsid_worker_grant_response_credit(
                        worker, event.request_id,
                        static_cast<std::uint32_t>(event.payload.size)),
                    "replenish response credit");
                continue;
            }
            if (event.type == CAPSID_EVENT_RESPONSE_END) {
                if (!received_head || event.request_id != 1) {
                    fail("unexpected response end");
                }
                *body_out = body;
                return std::chrono::duration<double, std::milli>(
                           std::chrono::steady_clock::now() - begin)
                    .count();
            }
            if (event.type == CAPSID_EVENT_ERROR) {
                fail(std::string("worker request error: ") +
                     std::string(reinterpret_cast<const char*>(event.payload.data),
                                 event.payload.size));
            }
            if (event.type == CAPSID_EVENT_EXIT) {
                fail("worker exited during request");
            }
            continue;
        }
        if (result != CAPSID_WOULD_BLOCK) {
            fail(std::string("request event: ") + capsid_result_string(result));
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            fail("timed out waiting for the response");
        }
        capsid_pollfd descriptor = {};
        descriptor.fd = capsid_worker_fd(worker);
        descriptor.events = POLLIN | (flush == CAPSID_WOULD_BLOCK ? POLLOUT : 0);
        capsid::win32::capsid_poll(&descriptor, 1, 50);
    }
}

// Profiling builds emit their exact-site dump while the worker runtime is
// being freed.  Abortive capsid_worker_destroy() intentionally gives a live
// child only a short natural-exit window, which can truncate that diagnostic
// JSON.  Follow the public graceful-stop protocol so EXIT is observed only
// after teardown (and therefore the dump) has completed.
void shutdown_worker(capsid_worker* worker) {
    require_result(capsid_worker_shutdown(worker), "shutdown worker");
    const std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(60);
    for (;;) {
        const capsid_result flush = capsid_worker_flush(worker);
        if (flush != CAPSID_OK && flush != CAPSID_WOULD_BLOCK) {
            fail(std::string("shutdown flush: ") + capsid_result_string(flush));
        }
        capsid_event event = {};
        event.struct_size = sizeof(event);
        const capsid_result result = capsid_worker_next_event(worker, &event);
        if (result == CAPSID_OK) {
            if (event.type == CAPSID_EVENT_EXIT) {
                return;
            }
            if (event.type == CAPSID_EVENT_ERROR) {
                fail(std::string("worker shutdown error: ") +
                     std::string(
                         reinterpret_cast<const char*>(event.payload.data),
                         event.payload.size));
            }
            continue;
        }
        if (result != CAPSID_WOULD_BLOCK) {
            fail(std::string("shutdown event: ") +
                 capsid_result_string(result));
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            fail("timed out waiting for graceful worker exit");
        }
        capsid_pollfd descriptor = {};
        descriptor.fd = capsid_worker_fd(worker);
        descriptor.events = POLLIN |
            (flush == CAPSID_WOULD_BLOCK ? POLLOUT : 0);
        capsid::win32::capsid_poll(&descriptor, 1, 50);
    }
}

const char* flag_value(int argc, char** argv, const char* name,
                       const char* fallback) {
    for (int i = 1; i < argc - 1; ++i) {
        if (std::strcmp(argv[i], name) == 0) {
            return argv[i + 1];
        }
    }
    return fallback;
}

bool has_flag(int argc, char** argv, const char* name) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], name) == 0) {
            return true;
        }
    }
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    const char* worker_path = flag_value(argc, argv, "--worker", nullptr);
    const char* mode = flag_value(argc, argv, "--mode", nullptr);
    const char* input_path = flag_value(argc, argv, "--input", nullptr);
    const char* source_name = flag_value(argc, argv, "--source-name", nullptr);
    const char* expect_body = flag_value(argc, argv, "--expect-body", nullptr);
    const char* url = flag_value(argc, argv, "--url",
                                 "https://example.test/sync");
    if (!worker_path || !mode || !input_path || !source_name) {
        fail("expected --worker --mode source|raw|opt --input --source-name");
    }
    const bool is_source = std::strcmp(mode, "source") == 0;
    if (!is_source && std::strcmp(mode, "raw") != 0 &&
        std::strcmp(mode, "opt") != 0) {
        fail("mode must be source, raw or opt");
    }
    int rounds = 5;
    int warmup = 1;
    int timeout_seconds = 30;
    if (has_flag(argc, argv, "--rounds")) {
        rounds = std::atoi(flag_value(argc, argv, "--rounds", "5"));
    }
    if (has_flag(argc, argv, "--warmup")) {
        warmup = std::atoi(flag_value(argc, argv, "--warmup", "1"));
    }
    if (has_flag(argc, argv, "--timeout-seconds")) {
        timeout_seconds =
            std::atoi(flag_value(argc, argv, "--timeout-seconds", "30"));
    }
    if (timeout_seconds <= 0) {
        fail("--timeout-seconds must be positive");
    }

    const std::string payload = read_file(input_path);
    const std::vector<std::uint8_t> bytes(payload.begin(), payload.end());

    capsid_worker* worker = spawn_worker(worker_path);
    if (is_source) {
        require_result(
            capsid_worker_load_bundle_named(
                worker, bytes.data(), bytes.size(), source_name),
            "load source bundle");
    } else {
        require_result(
            capsid_worker_load_trusted_bytecode_named(
                worker, bytes.data(), bytes.size(), source_name),
            "load trusted bytecode");
    }
    wait_for_ready(worker);

    for (int round = 1 - warmup; round <= rounds; ++round) {
        std::string body;
        const double ms = run_request(worker, url, timeout_seconds, &body);
        if (round <= 0) {
            continue;  // warmup, discarded
        }
        if (expect_body && body != expect_body) {
            fail(std::string("body mismatch: got '") + body + "' expected '" +
                 expect_body + "'");
        }
        const std::string escaped_body = json_escape(body);
        std::printf("{\"mode\":\"%s\",\"round\":%d,\"ms\":%.3f,"
                    "\"status\":200,\"body_len\":%zu,\"body\":\"%s\","
                    "\"ok\":true}\n",
                    mode, round, ms, body.size(), escaped_body.c_str());
    }
    shutdown_worker(worker);
    capsid_worker_destroy(worker);
    return 0;
}
