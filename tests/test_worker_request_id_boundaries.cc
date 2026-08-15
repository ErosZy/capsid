// Worker request IDs travel the full uint64 range across C, IPC, C++ and
// JS. Adjacent IDs above 2^53 collapse into the same JavaScript Number,
// and IDs near 2^64-1 wrap through int64. This test freezes the P0-1
// invariant: every transport response, LOG event and AUDIT record must
// carry the exact transport ID for boundaries the old bridge could not
// represent.
//
// Expected to FAIL on the pre-BigInt implementation (RED gate for PR-01):
// the concurrent 2^53 / 2^53+1 pair collides in the JS request map.

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

struct Response {
    bool saw_head = false;
    uint16_t status = 0;
    std::string body;
    bool ended = false;
};

struct Observations {
    // LOG message (request path) -> transport request id.
    std::map<std::string, uint64_t> log_ids;
    // (event.request_id, decoded record.request_id) per QUERY/stdio audit.
    std::vector<std::pair<uint64_t, uint64_t> > query_audits;
    std::map<uint64_t, Response> responses;
    std::set<uint64_t> remaining;
};

void observe(capsid_worker *worker, Observations *observations) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(15);
    while (!observations->remaining.empty()) {
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
        } else if (event.type == CAPSID_EVENT_RESPONSE_HEAD &&
                   observations->remaining.count(event.request_id) != 0) {
            Response &response = observations->responses[event.request_id];
            if (response.saw_head) {
                fail("duplicate response head for request " +
                     std::to_string(event.request_id));
            }
            response.saw_head = true;
            response.status = event.status;
        } else if (event.type == CAPSID_EVENT_RESPONSE_BODY &&
                   observations->remaining.count(event.request_id) != 0) {
            observations->responses[event.request_id].body.append(
                reinterpret_cast<const char *>(event.payload.data),
                event.payload.size);
            require_result(capsid_worker_grant_response_credit(
                               worker, event.request_id,
                               static_cast<uint32_t>(event.payload.size)),
                           "grant response credit");
        } else if (event.type == CAPSID_EVENT_RESPONSE_END &&
                   observations->remaining.count(event.request_id) != 0) {
            Response &response = observations->responses[event.request_id];
            if (!response.saw_head) {
                fail("response ended without head for request " +
                     std::to_string(event.request_id));
            }
            response.ended = true;
            observations->remaining.erase(event.request_id);
        } else if (event.type == CAPSID_EVENT_ERROR ||
                   event.type == CAPSID_EVENT_REQUEST_TIMEOUT ||
                   event.type == CAPSID_EVENT_EXIT) {
            fail("request event: " + payload(event));
        }
    }
}

void begin(capsid_worker *worker, uint64_t id, const char *path) {
    const std::string url = std::string("https://boundary.invalid/") + path;
    require_result(capsid_worker_begin_bodyless_request(
                       worker, id, "GET", url.c_str(), nullptr, 0),
                   "begin request");
}

void run_concurrent_pair(capsid_worker *worker,
                         uint64_t first_id,
                         const char *first_path,
                         uint64_t second_id,
                         const char *second_path,
                         const char *label) {
    Observations observations;
    observations.remaining.insert(first_id);
    observations.remaining.insert(second_id);
    begin(worker, first_id, first_path);
    begin(worker, second_id, second_path);
    observe(worker, &observations);

    const uint64_t ids[] = { first_id, second_id };
    const char *const paths[] = { first_path, second_path };
    for (size_t index = 0; index < 2; ++index) {
        const Response &response = observations.responses[ids[index]];
        if (!response.ended || !response.saw_head) {
            fail(std::string(label) + ": request " +
                 std::to_string(ids[index]) + " did not complete");
        }
        if (response.status != 200) {
            fail(std::string(label) + ": request " +
                 std::to_string(ids[index]) + " status " +
                 std::to_string(response.status));
        }
        if (response.body != paths[index]) {
            fail(std::string(label) + ": request " +
                 std::to_string(ids[index]) + " body '" + response.body +
                 "' != '" + paths[index] + "'");
        }
    }
    for (size_t index = 0; index < 2; ++index) {
        const uint64_t expected = ids[index];
        if (observations.log_ids[paths[index]] != expected) {
            fail(std::string(label) + ": LOG for '" + paths[index] +
                 "' carried request_id " +
                 std::to_string(observations.log_ids[paths[index]]) +
                 " instead of " + std::to_string(expected));
        }
    }
    size_t audits = 0;
    for (size_t index = 0; index < observations.query_audits.size(); ++index) {
        const uint64_t event_id = observations.query_audits[index].first;
        const uint64_t record_id = observations.query_audits[index].second;
        const bool belongs =
            event_id == first_id || event_id == second_id;
        if (!belongs || record_id != event_id) {
            fail(std::string(label) +
                 ": audit event/record request_id mismatch (" +
                 std::to_string(event_id) + "/" +
                 std::to_string(record_id) + ")");
        }
        audits += 1;
    }
    if (audits != 2) {
        fail(std::string(label) + ": expected 2 audit records, saw " +
             std::to_string(audits));
    }
}

void run_single(capsid_worker *worker,
                uint64_t id,
                const char *path,
                const char *label) {
    Observations observations;
    observations.remaining.insert(id);
    begin(worker, id, path);
    observe(worker, &observations);

    const Response &response = observations.responses[id];
    if (!response.ended || !response.saw_head) {
        fail(std::string(label) + ": request did not complete");
    }
    if (response.status != 200 || response.body != path) {
        fail(std::string(label) + ": response " +
             std::to_string(response.status) + " '" + response.body + "'");
    }
    if (observations.log_ids[path] != id) {
        fail(std::string(label) + ": LOG carried request_id " +
             std::to_string(observations.log_ids[path]) + " instead of " +
             std::to_string(id));
    }
    size_t audits = 0;
    for (size_t index = 0; index < observations.query_audits.size(); ++index) {
        if (observations.query_audits[index].first != id ||
            observations.query_audits[index].second != id) {
            fail(std::string(label) +
                 ": audit event/record request_id mismatch");
        }
        audits += 1;
    }
    if (audits != 1) {
        fail(std::string(label) + ": expected 1 audit record, saw " +
             std::to_string(audits));
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
    rule.rule_id = 90;
    capsid_capability_policy capability;
    capsid_capability_policy_init(&capability);
    capability.application_identity = "request-id-boundaries";
    capability.allowed_modules = modules;
    capability.allowed_module_count = 2;
    capability.rules = &rule;
    capability.rule_count = 1;

    capsid_worker_config config;
    capsid_worker_config_init(&config);
    config.worker_path = argv[1];
    config.strict_sandbox = 0;
    config.request_timeout_ms = 5000;
    config.capability_policy = &capability;

    capsid_worker *worker = nullptr;
    require_result(capsid_worker_spawn(&config, &worker), "spawn worker");

    const std::string bundle =
        "import { permissions } from 'capsid:permissions';\n"
        "import { stdio } from 'capsid:stdio';\n"
        "export default { async fetch(request) {\n"
        "  const name = new URL(request.url).pathname.slice(1);\n"
        "  await Promise.resolve();\n"
        "  permissions.query({ name: 'stdio', stream: 'stdout' });\n"
        "  stdio.write('stdout', name);\n"
        "  return new Response(name);\n"
        "} };\n";
    require_result(capsid_worker_load_bundle(
                       worker,
                       reinterpret_cast<const uint8_t *>(bundle.data()),
                       bundle.size()),
                   "load bundle");
    wait_ready(worker);

    // Adjacent IDs around 2^53: Number precision ends here, so the old
    // bridge collapses the pair into a single JS map key.
    const uint64_t k2Pow53 = UINT64_C(1) << 53;
    run_concurrent_pair(worker,
                        k2Pow53, "a53",
                        k2Pow53 + 1, "a53p1",
                        "2^53 pair");

    // Top of the uint64 range, single request.
    run_single(worker, UINT64_MAX, "max", "2^64-1 single");

    // Top of the range, concurrent with the next-lower ID.
    run_concurrent_pair(worker,
                        UINT64_MAX - 1, "maxm1",
                        UINT64_MAX, "max",
                        "2^64 pair");

    require_result(capsid_worker_shutdown(worker), "shutdown");
    capsid_worker_destroy(worker);
    std::cout << "PASS: request id boundaries stay exact through "
                 "transport, LOG and AUDIT"
              << std::endl;
    return 0;
}
