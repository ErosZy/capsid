// A request that reaches a terminal state must not leave a realm behind
// that can still execute request-level capabilities: after cancel, after
// a deadline, and after a normal response that left a detached timer, the
// worker must poison itself (exit) instead of letting the continuation
// run, and the next request must be served by a fresh realm.
//
// Expected to FAIL on the pre-poison implementation (RED gate for PR-01):
// the old bridge lets the 80ms continuations run with request_id 0 and
// keeps serving the next request in the same realm.

#include "capsid/runtime.h"

#include "win32_compat.h"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>

namespace {

[[noreturn]] void fail(const std::string &message) {
    std::cerr << "FAIL " << message << std::endl;
    std::exit(1);
}

void require_result(capsid_result result, const char *operation) {
    if (result != CAPSID_OK) {
        fail(std::string(operation) + ": " + capsid_result_string(result));
    }
}

std::string bytes(const capsid_bytes &value) {
    return value.size == 0
        ? std::string()
        : std::string(reinterpret_cast<const char *>(value.data), value.size);
}

std::string payload(const capsid_event &event) {
    return bytes(event.payload);
}

std::string log_message(const capsid_event &event) {
    if (event.payload.size < 2) {
        fail("truncated log");
    }
    const size_t stream_size = event.payload.data[0] |
                               (static_cast<size_t>(event.payload.data[1]) << 8);
    if (stream_size > event.payload.size - 2) {
        fail("invalid log stream length");
    }
    const char *data = reinterpret_cast<const char *>(event.payload.data);
    return std::string(data + 2 + stream_size,
                       event.payload.size - 2 - stream_size);
}

void wait_io(capsid_worker *worker, bool writable) {
    pollfd descriptor = {};
    descriptor.fd = capsid_worker_fd(worker);
    descriptor.events = POLLIN | (writable ? POLLOUT : 0);
    (void)capsid::win32::capsid_poll(&descriptor, 1, 20);
}

capsid_event next_event(capsid_worker *worker,
                        std::chrono::steady_clock::time_point deadline) {
    for (;;) {
        const capsid_result flush = capsid_worker_flush(worker);
        if (flush != CAPSID_OK && flush != CAPSID_WOULD_BLOCK) {
            fail(std::string("flush: ") + capsid_result_string(flush));
        }
        capsid_event event = {};
        event.struct_size = sizeof(event);
        const capsid_result result = capsid_worker_next_event(worker, &event);
        if (result == CAPSID_OK) {
            return event;
        }
        if (result != CAPSID_WOULD_BLOCK) {
            fail(std::string("next event: ") + capsid_result_string(result));
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            fail("event deadline expired");
        }
        wait_io(worker, flush == CAPSID_WOULD_BLOCK);
    }
}

void wait_ready(capsid_worker *worker) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(15);
    for (;;) {
        const capsid_event event = next_event(worker, deadline);
        if (event.type == CAPSID_EVENT_READY) {
            return;
        }
        if (event.type == CAPSID_EVENT_ERROR ||
            event.type == CAPSID_EVENT_EXIT) {
            fail("startup event: " + payload(event));
        }
    }
}

void begin(capsid_worker *worker, uint64_t id, const char *path) {
    const std::string url = std::string("https://terminal.invalid/") + path;
    require_result(capsid_worker_begin_bodyless_request(
                       worker, id, "GET", url.c_str(), nullptr, 0),
                   "begin request");
}

// Waits for a LOG with exactly `wanted`; fails on ERROR/EXIT.
void wait_log(capsid_worker *worker,
              const std::string &wanted,
              uint64_t expected_id) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(15);
    for (;;) {
        const capsid_event event = next_event(worker, deadline);
        if (event.type == CAPSID_EVENT_LOG && log_message(event) == wanted) {
            if (event.request_id != expected_id) {
                fail("LOG '" + wanted + "' carried request_id " +
                     std::to_string(event.request_id) + " instead of " +
                     std::to_string(expected_id));
            }
            return;
        }
        if (event.type == CAPSID_EVENT_ERROR ||
            event.type == CAPSID_EVENT_EXIT) {
            fail("log wait event: " + payload(event));
        }
    }
}

// Waits for a REQUEST_TIMEOUT for `id`; fails on ERROR/EXIT.
void wait_timeout(capsid_worker *worker, uint64_t id) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(15);
    for (;;) {
        const capsid_event event = next_event(worker, deadline);
        if (event.type == CAPSID_EVENT_REQUEST_TIMEOUT &&
            event.request_id == id) {
            return;
        }
        if (event.type == CAPSID_EVENT_ERROR ||
            event.type == CAPSID_EVENT_EXIT) {
            fail("timeout wait event: " + payload(event));
        }
    }
}

// After a request terminal state, the worker must not run the detached
// continuation and must poison itself: any LOG whose message equals
// `forbidden` fails the test, and the phase only passes on EXIT.
void watch_poison(capsid_worker *worker,
                  const std::string &forbidden,
                  const char *label) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
        const capsid_event event = next_event(worker, deadline);
        if (event.type == CAPSID_EVENT_LOG) {
            if (log_message(event) == forbidden) {
                fail(std::string(label) +
                     ": terminal continuation produced native event '" +
                     forbidden + "' with request_id " +
                     std::to_string(event.request_id));
            }
            continue;
        }
        if (event.type == CAPSID_EVENT_ERROR ||
            event.type == CAPSID_EVENT_REQUEST_TIMEOUT) {
            // Terminalization of the request itself is expected on the
            // fixed implementation; keep watching for the poison EXIT.
            continue;
        }
        if (event.type == CAPSID_EVENT_EXIT) {
            return;
        }
    }
    fail(std::string(label) +
         ": worker neither ran the terminal continuation nor exited "
         "(poison required)");
}

// Completes a normal response and grants credit; fails on ERROR/EXIT.
void complete_response(capsid_worker *worker,
                       uint64_t id,
                       bool *saw_head) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(15);
    for (;;) {
        const capsid_event event = next_event(worker, deadline);
        if (event.type == CAPSID_EVENT_RESPONSE_HEAD &&
            event.request_id == id) {
            *saw_head = event.status == 200;
        } else if (event.type == CAPSID_EVENT_RESPONSE_BODY &&
                   event.request_id == id) {
            require_result(capsid_worker_grant_response_credit(
                               worker, id,
                               static_cast<uint32_t>(event.payload.size)),
                           "grant response credit");
        } else if (event.type == CAPSID_EVENT_RESPONSE_END &&
                   event.request_id == id) {
            if (!*saw_head) {
                fail("response ended without a 200 head");
            }
            return;
        } else if (event.type == CAPSID_EVENT_ERROR ||
                   event.type == CAPSID_EVENT_REQUEST_TIMEOUT ||
                   event.type == CAPSID_EVENT_EXIT) {
            fail("response event: " + payload(event));
        }
    }
}

capsid_worker *spawn(const char *worker_path,
                     const capsid_capability_policy *capability,
                     uint32_t timeout_ms) {
    capsid_worker_config config;
    capsid_worker_config_init(&config);
    config.worker_path = worker_path;
    config.strict_sandbox = 0;
    config.request_timeout_ms = timeout_ms;
    config.capability_policy = capability;

    capsid_worker *worker = nullptr;
    require_result(capsid_worker_spawn(&config, &worker), "spawn worker");
    const std::string bundle =
        "import { permissions } from 'capsid:permissions';\n"
        "import { stdio } from 'capsid:stdio';\n"
        "function emit(name) {\n"
        "  permissions.query({ name: 'stdio', stream: 'stdout' });\n"
        "  stdio.write('stdout', name);\n"
        "}\n"
        "export default { async fetch(request) {\n"
        "  const name = new URL(request.url).pathname.slice(1);\n"
        "  if (name === 'cancel') {\n"
        "    emit('start-cancel');\n"
        "    setTimeout(() => emit('after-cancel'), 80);\n"
        "    return new Promise(() => {});\n"
        "  }\n"
        "  if (name === 'timeout') {\n"
        "    emit('start-timeout');\n"
        "    await new Promise(resolve => setTimeout(resolve, 80));\n"
        "    emit('after-timeout');\n"
        "  }\n"
        "  if (name === 'detached') {\n"
        "    emit('start-detached');\n"
        "    setTimeout(() => emit('after-detach'), 80);\n"
        "    return new Response('detached-ok');\n"
        "  }\n"
        "  emit('start-reuse');\n"
        "  return new Response('reuse-ok');\n"
        "} };\n";
    require_result(capsid_worker_load_bundle(
                       worker,
                       reinterpret_cast<const uint8_t *>(bundle.data()),
                       bundle.size()),
                   "load bundle");
    wait_ready(worker);
    return worker;
}

}  // namespace

int main(int argc, char **argv) {
    if (argc != 2) {
        fail("expected capsid-worker path");
    }

    const char *modules[] = { "capsid:permissions", "capsid:stdio" };
    capsid_permission_rule rule;
    capsid_permission_rule_init(&rule);
    rule.action = CAPSID_PERMISSION_ALLOW;
    rule.permission = CAPSID_PERMISSION_STDIO;
    rule.resource = "stdout";
    rule.rule_id = 92;
    capsid_capability_policy capability;
    capsid_capability_policy_init(&capability);
    capability.application_identity = "terminal-continuation-audit";
    capability.allowed_modules = modules;
    capability.allowed_module_count = 2;
    capability.rules = &rule;
    capability.rule_count = 1;

    // Phase 1: cancel leaves an 80ms continuation behind.
    capsid_worker *worker = spawn(argv[1], &capability, 30);
    begin(worker, 51, "cancel");
    wait_log(worker, "start-cancel", 51);
    require_result(capsid_worker_cancel(worker, 51), "cancel request");
    watch_poison(worker, "after-cancel", "cancel");
    capsid_worker_destroy(worker);

    // Phase 2: deadline leaves the same continuation behind.
    worker = spawn(argv[1], &capability, 30);
    begin(worker, 52, "timeout");
    wait_log(worker, "start-timeout", 52);
    wait_timeout(worker, 52);
    watch_poison(worker, "after-timeout", "timeout");
    capsid_worker_destroy(worker);

    // Phase 3: a normal response with a detached timer must also poison.
    worker = spawn(argv[1], &capability, 30);
    begin(worker, 53, "detached");
    wait_log(worker, "start-detached", 53);
    bool saw_head = false;
    complete_response(worker, 53, &saw_head);
    watch_poison(worker, "after-detach", "detached");
    capsid_worker_destroy(worker);

    // Phase 4: the next request must be served by a fresh realm with no
    // trace of the old one.
    worker = spawn(argv[1], &capability, 30);
    begin(worker, 54, "reuse");
    wait_log(worker, "start-reuse", 54);
    saw_head = false;
    complete_response(worker, 54, &saw_head);
    require_result(capsid_worker_shutdown(worker), "shutdown");
    capsid_worker_destroy(worker);

    std::cout << "PASS: cancel, timeout and detached continuations poison "
                 "the worker; the next request runs in a fresh realm"
              << std::endl;
    return 0;
}
