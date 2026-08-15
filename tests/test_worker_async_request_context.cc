// Promise continuations must keep the request identity of the handler that
// created them. A microtask, a nested Promise, a 0ms timer and a 20ms
// timer all resume with executing_request_id_ cleared on the old bridge,
// so their LOG frames, audit events and decoded audit records all fall
// back to request_id 0 while the transport RESPONSE_END keeps the real ID.
//
// WP-02 gate (spec §6.2): the token is captured at job ENQUEUE time inside
// the request scope. Microtask and nested-promise continuations are enqueued
// inside the request chain, so they keep the exact transport ID — asserted
// at three layers (LOG frame, audit event, decoded audit record). Two
// requests interleave so events cannot be attributed by arrival order alone.
//
// WP-03 (spec §7.2): TJSAsyncContextHooks wire timers, HTTP clients and
// webcrypto ops to the request context. A timer callback fires outside any
// QuickJS job, so without the hooks it would run in worker scope; the hooks
// capture the creating request's context at resource creation and resume
// the callback inside it — the 0ms/20ms timer and digest continuations now
// keep exact ids, asserted at the same three layers.

#include "capsid/runtime.h"

#include "win32_compat.h"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <vector>

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

void wait_io(capsid_worker *worker, bool writable) {
    capsid_pollfd descriptor = {};
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

std::string decode_log(const capsid_event &event) {
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

struct Observations {
    // LOG message (path) -> transport request id.
    std::map<std::string, uint64_t> log_ids;
    // QUERY/stdio audit (event.request_id, record.request_id) pairs.
    std::vector<std::pair<uint64_t, uint64_t> > query_audits;
    std::set<uint64_t> ended;
};

void observe(capsid_worker *worker,
             const std::set<uint64_t> &wanted,
             Observations *observations,
             bool allow_timeout_terminal) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(15);
    for (;;) {
        size_t completed = 0;
        for (uint64_t id : wanted) {
            completed += observations->ended.count(id);
        }
        if (completed == wanted.size()) {
            return;
        }
        const capsid_event event = next_event(worker, deadline);
        if (event.type == CAPSID_EVENT_LOG) {
            observations->log_ids[decode_log(event)] = event.request_id;
        } else if (event.type == CAPSID_EVENT_AUDIT) {
            capsid_audit_record audit;
            capsid_audit_record_init(&audit);
            require_result(capsid_audit_record_decode(&event, &audit),
                           "decode audit");
            if (audit.stage == CAPSID_AUDIT_STAGE_QUERY &&
                bytes(audit.capability) == "stdio") {
                observations->query_audits.push_back(
                    std::make_pair(event.request_id, audit.request_id));
            }
        } else if (event.type == CAPSID_EVENT_RESPONSE_BODY) {
            require_result(capsid_worker_grant_response_credit(
                               worker, event.request_id,
                               static_cast<uint32_t>(event.payload.size)),
                           "grant response credit");
        } else if (event.type == CAPSID_EVENT_RESPONSE_END &&
                   wanted.count(event.request_id) != 0) {
            observations->ended.insert(event.request_id);
        } else if (event.type == CAPSID_EVENT_REQUEST_TIMEOUT &&
                   allow_timeout_terminal &&
                   wanted.count(event.request_id) != 0) {
            // WP-02 boundary (spec §7.1): the timer continuation runs
            // outside any QuickJS job, captures no token, and the strict
            // response bridges reject it — the request fails closed at
            // the transport level with a timeout error. WP-03 wires the
            // resource context and turns this into a normal response.
            observations->ended.insert(event.request_id);
        } else if (event.type == CAPSID_EVENT_ERROR ||
                   event.type == CAPSID_EVENT_REQUEST_TIMEOUT ||
                   event.type == CAPSID_EVENT_EXIT) {
            fail("request event: " + payload(event));
        }
    }
}

void begin(capsid_worker *worker, uint64_t id, const char *path) {
    const std::string url = std::string("https://context.invalid/") + path;
    require_result(capsid_worker_begin_bodyless_request(
                       worker, id, "GET", url.c_str(), nullptr, 0),
                   "begin request");
}

void require_log(const Observations &observations,
                 const std::string &message,
                 uint64_t expected) {
    std::map<std::string, uint64_t>::const_iterator it =
        observations.log_ids.find(message);
    if (it == observations.log_ids.end()) {
        fail("missing LOG for '" + message + "'");
    }
    if (it->second != expected) {
        fail("LOG for '" + message + "' carried request_id " +
             std::to_string(it->second) + " instead of " +
             std::to_string(expected));
    }
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
    rule.rule_id = 91;
    capsid_capability_policy capability;
    capsid_capability_policy_init(&capability);
    capability.application_identity = "async-context-audit";
    capability.allowed_modules = modules;
    capability.allowed_module_count = 2;
    capability.rules = &rule;
    capability.rule_count = 1;

    capsid_worker_config config;
    capsid_worker_config_init(&config);
    config.worker_path = argv[1];
    config.strict_sandbox = 0;
    // 1500ms: the timer-based requests fail closed at the transport level
    // (the strict response bridges reject the token-less continuation), and
    // the timeout error must land well inside the 15s observe window.
    config.request_timeout_ms = 1500;
    config.capability_policy = &capability;

    capsid_worker *worker = nullptr;
    require_result(capsid_worker_spawn(&config, &worker), "spawn worker");

    const std::string bundle =
        "import { permissions } from 'capsid:permissions';\n"
        "import { stdio } from 'capsid:stdio';\n"
        "async function nested() {\n"
        "  await Promise.resolve();\n"
        "  return true;\n"
        "}\n"
        "async function run(name) {\n"
        "  if (name === 'micro') await Promise.resolve();\n"
        "  else if (name === 'nested') await nested();\n"
        "  else if (name === 'digest') {\n"
        "    await crypto.subtle.digest('SHA-256',\n"
        "        new TextEncoder().encode(name));\n"
        "  } else await new Promise(resolve => setTimeout(resolve, "
        "name === 'slow' ? 20 : 0));\n"
        "  permissions.query({ name: 'stdio', stream: 'stdout' });\n"
        "  stdio.write('stdout', name);\n"
        "}\n"
        "export default { async fetch(request) {\n"
        "  const name = new URL(request.url).pathname.slice(1);\n"
        "  await run(name);\n"
        "  return new Response(name);\n"
        "} };\n";
    require_result(capsid_worker_load_bundle(
                       worker,
                       reinterpret_cast<const uint8_t *>(bundle.data()),
                       bundle.size()),
                   "load bundle");
    wait_ready(worker);

    // Sequential: microtask continuation, then a nested Promise chain, then
    // a webcrypto digest continuation (its callback fires from the uv work
    // queue, outside any QuickJS job). Strict phase: any error/timeout
    // event fails the test.
    Observations observations;
    begin(worker, 41, "micro");
    observe(worker, { 41 }, &observations, false);
    begin(worker, 42, "nested");
    observe(worker, { 42 }, &observations, false);
    begin(worker, 45, "digest");
    observe(worker, { 45 }, &observations, false);

    // Interleaved: 20ms timer and 0ms timer overlap, so events cannot be
    // attributed by order of arrival. WP-03 (spec §7.2): timer callbacks
    // fire outside any QuickJS job but resume in the captured request
    // context, so both complete normally with their exact IDs.
    begin(worker, 43, "slow");
    begin(worker, 44, "fast");
    observe(worker, { 43, 44 }, &observations, true);

    // LOG frame ownership. Microtask, nested-promise, digest and timer
    // continuations are all created inside the request scope, so the
    // captured token pins the exact transport ID at every layer.
    require_log(observations, "micro", 41);
    require_log(observations, "nested", 42);
    require_log(observations, "digest", 45);
    require_log(observations, "slow", 43);
    require_log(observations, "fast", 44);

    // Audit event and decoded audit record ownership: every continuation
    // keeps its exact ID — nothing may fall back to worker-scope id 0.
    const size_t expected_audits = 5;
    size_t audits = 0;
    std::map<uint64_t, size_t> per_request;
    for (size_t index = 0; index < observations.query_audits.size(); ++index) {
        const uint64_t event_id = observations.query_audits[index].first;
        const uint64_t record_id = observations.query_audits[index].second;
        if (record_id != event_id) {
            fail("audit event request_id " + std::to_string(event_id) +
                 " != decoded record request_id " +
                 std::to_string(record_id));
        }
        per_request[event_id] += 1;
        audits += 1;
    }
    if (audits != expected_audits) {
        fail("expected " + std::to_string(expected_audits) +
             " audit records, saw " + std::to_string(audits));
    }
    if (per_request[41] != 1 || per_request[42] != 1 ||
        per_request[45] != 1 ||
        per_request[0] != 0 ||
        per_request[43] != 1 || per_request[44] != 1) {
        fail("audit attribution: 41=" + std::to_string(per_request[41]) +
             " 42=" + std::to_string(per_request[42]) +
             " 45=" + std::to_string(per_request[45]) +
             " 0=" + std::to_string(per_request[0]) +
             " 43=" + std::to_string(per_request[43]) +
             " 44=" + std::to_string(per_request[44]));
    }

    require_result(capsid_worker_shutdown(worker), "shutdown");
    capsid_worker_destroy(worker);
    std::cout << "PASS: microtask, nested-promise, digest and timer "
                 "continuations keep their request identity (WP-03)"
              << std::endl;
    return 0;
}
