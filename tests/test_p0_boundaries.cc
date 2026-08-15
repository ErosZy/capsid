#include "capsid/runtime.h"

#include "win32_compat.h"

#if defined(__linux__)
#include <fcntl.h>
#if defined(_WIN32)
#include "win32_compat.h"
#else
#include <unistd.h>
#endif
#endif

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

namespace {

void fail(const std::string &message) {
    std::cerr << "FAIL: " << message << std::endl;
    std::exit(1);
}

void require(bool condition, const char *message) {
    if (!condition) {
        fail(message);
    }
}

void require_result(capsid_result result, const char *operation) {
    if (result != CAPSID_OK) {
        fail(std::string(operation) + ": " + capsid_result_string(result));
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

capsid_worker *spawn_loaded(const char *worker_path,
                          const std::string &bundle,
                          uint32_t max_header_bytes,
                          uint32_t max_queued_bytes) {
    capsid_worker_config config;
    capsid_worker_config_init(&config);
    config.worker_path = worker_path;
    config.strict_sandbox = 0;
    config.max_header_bytes = max_header_bytes;
    config.max_queued_bytes = max_queued_bytes;
    config.initial_stream_window = 1024;
    config.request_timeout_ms = 2000;

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

void wait_request_error(capsid_worker *worker, uint64_t request_id) {
    const std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
        pump(worker);
        capsid_event event;
        while (next_event(worker, &event)) {
            if (event.type == CAPSID_EVENT_REQUEST_CREDIT) {
                continue;
            }
            if (event.type == CAPSID_EVENT_ERROR &&
                event.request_id == request_id) {
                return;
            }
            if (event.type == CAPSID_EVENT_EXIT) {
                fail("worker exited instead of returning request error");
            }
        }
    }
    fail("request error timeout");
}

void test_strict_sandbox_fails_closed(const char *worker_path,
                                      const std::string &bundle) {
    capsid_worker_config config;
    capsid_worker_config_init(&config);
    config.worker_path = worker_path;
    config.strict_sandbox = 1;

    capsid_worker *worker = NULL;
    require_result(capsid_worker_spawn(&config, &worker), "strict spawn");
    require_result(
        capsid_worker_load_bundle(
            worker,
            reinterpret_cast<const uint8_t *>(bundle.data()),
            bundle.size()),
        "strict load");

    bool terminal = false;
    const std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
        pump(worker);
        capsid_event event;
        while (next_event(worker, &event)) {
            if (event.type == CAPSID_EVENT_READY) {
#if defined(__linux__)
                require(
                    (event.flags & CAPSID_SANDBOX_FEATURE_STRICT_BASE) ==
                        CAPSID_SANDBOX_FEATURE_STRICT_BASE,
                    "strict READY reports all mandatory sandbox features");
                terminal = true;
                break;
#else
                fail("strict sandbox silently accepted an unavailable policy");
#endif
            }
            if (event.type == CAPSID_EVENT_ERROR) {
                const std::string message(
                    reinterpret_cast<const char *>(event.payload.data),
                    event.payload.size);
#if defined(__linux__)
                fail(std::string("strict Linux sandbox failed: ") + message);
#else
                require(
                    message.find("strict sandbox is unavailable") !=
                        std::string::npos,
                    "strict sandbox error is explicit");
                terminal = true;
                break;
#endif
            }
        }
        if (terminal) {
            break;
        }
    }
    require(terminal, "strict sandbox produced no terminal startup event");
    capsid_worker_destroy(worker);
}

void test_bundle_enqueue_is_atomic(const char *worker_path) {
    capsid_worker_config config;
    capsid_worker_config_init(&config);
    config.worker_path = worker_path;
    config.strict_sandbox = 0;
    config.max_queued_bytes = 512;

    capsid_worker *worker = NULL;
    require_result(capsid_worker_spawn(&config, &worker), "atomic spawn");
    const std::vector<uint8_t> oversized(1024, 'x');
    require(
        capsid_worker_load_bundle(
            worker, &oversized[0], oversized.size()) ==
            CAPSID_WOULD_BLOCK,
        "oversized bundle must fail before partial enqueue");

    static const char valid_bundle[] =
        "export default { fetch() { return new Response('ok'); } };";
    require_result(
        capsid_worker_load_bundle(
            worker,
            reinterpret_cast<const uint8_t *>(valid_bundle),
            sizeof(valid_bundle) - 1),
        "load bundle after atomic rejection");
    wait_ready(worker);
    capsid_worker_destroy(worker);
}

void test_named_bundle_crosses_frame_boundary(const char *worker_path) {
    capsid_worker_config config;
    capsid_worker_config_init(&config);
    config.worker_path = worker_path;
    config.strict_sandbox = 0;

    std::string bundle =
        "export default { fetch() { return new Response('ok'); } };\n/*";
    bundle.append(70u * 1024u, 'x');
    bundle.append("*/");

    capsid_worker *worker = NULL;
    require_result(capsid_worker_spawn(&config, &worker), "named bundle spawn");
    require_result(
        capsid_worker_load_bundle_named(
            worker,
            reinterpret_cast<const uint8_t *>(bundle.data()),
            bundle.size(),
            "https://example.test/application.js"),
        "load named multi-frame bundle");
    wait_ready(worker);
    capsid_worker_destroy(worker);
}

void test_invalid_config_is_rejected(const char *worker_path) {
    capsid_worker_config config;
    capsid_worker_config_init(&config);
    config.worker_path = worker_path;
    config.strict_sandbox = 0;

    capsid_worker *worker = NULL;
    config.js_heap_limit =
        static_cast<uint64_t>(std::numeric_limits<int>::max()) + 1;
    require(
        capsid_worker_spawn(&config, &worker) == CAPSID_INVALID_ARGUMENT,
        "unrepresentable QuickJS heap limit rejected");

    capsid_worker_config_init(&config);
    config.worker_path = worker_path;
    config.strict_sandbox = 0;
    config.process_memory_limit = config.js_heap_limit - 1;
    require(
        capsid_worker_spawn(&config, &worker) == CAPSID_INVALID_ARGUMENT,
        "process memory limit below JS heap rejected");

    capsid_worker_config_init(&config);
    config.worker_path = worker_path;
    config.strict_sandbox = 0;
    config.max_header_bytes = 64u * 1024u + 1;
    require(
        capsid_worker_spawn(&config, &worker) == CAPSID_INVALID_ARGUMENT,
        "header limit beyond protocol capacity rejected");

    capsid_worker_config_init(&config);
    config.worker_path = worker_path;
    config.strict_sandbox = 0;
    config.max_queued_bytes = 4u * 1024u * 1024u + 1;
    require(
        capsid_worker_spawn(&config, &worker) == CAPSID_INVALID_ARGUMENT,
        "queue limit beyond parser capacity rejected");

    capsid_worker_config_init(&config);
    config.worker_path = worker_path;
    config.strict_sandbox = 2;
    require(
        capsid_worker_spawn(&config, &worker) == CAPSID_INVALID_ARGUMENT,
        "invalid strict sandbox boolean rejected");

    capsid_worker_config_init(&config);
    config.worker_path = worker_path;
    config.sandbox_required_features =
        CAPSID_SANDBOX_FEATURE_STRICT_BASE;
    require(
        capsid_worker_spawn(&config, &worker) == CAPSID_INVALID_ARGUMENT,
        "required sandbox features without strict mode rejected");

    capsid_worker_config_init(&config);
    config.worker_path = worker_path;
    config.strict_sandbox = 1;
    config.sandbox_required_features =
        CAPSID_SANDBOX_FEATURE_CGROUP_V2;
    require(
        capsid_worker_spawn(&config, &worker) == CAPSID_INVALID_ARGUMENT,
        "required cgroup feature without path rejected");

    capsid_worker_config_init(&config);
    config.worker_path = worker_path;
    config.strict_sandbox = 1;
    config.sandbox_cgroup_path = "relative/cgroup";
    require(
        capsid_worker_spawn(&config, &worker) == CAPSID_INVALID_ARGUMENT,
        "relative cgroup path rejected");

    capsid_worker_config_init(&config);
    config.worker_path = worker_path;
    config.sandbox_reserved = 1;
    require(
        capsid_worker_spawn(&config, &worker) == CAPSID_INVALID_ARGUMENT,
        "nonzero sandbox reserved field rejected");

    capsid_resource_limits limits;
    capsid_resource_limits_init(&limits);
    limits.enabled_fields = 1u << 31;
    capsid_worker_config_init(&config);
    config.worker_path = worker_path;
    config.resource_limits = &limits;
    require(
        capsid_worker_spawn(&config, &worker) == CAPSID_INVALID_ARGUMENT,
        "unknown resource limit field rejected");

    capsid_resource_limits_init(&limits);
    limits.reserved = 1;
    capsid_worker_config_init(&config);
    config.worker_path = worker_path;
    config.resource_limits = &limits;
    require(
        capsid_worker_spawn(&config, &worker) == CAPSID_INVALID_ARGUMENT,
        "nonzero resource limit reserved field rejected");

    capsid_resource_limits_init(&limits);
    limits.struct_size--;
    capsid_worker_config_init(&config);
    config.worker_path = worker_path;
    config.resource_limits = &limits;
    require(
        capsid_worker_spawn(&config, &worker) == CAPSID_INVALID_ARGUMENT,
        "undersized resource limit descriptor rejected");

    capsid_resource_limits_init(&limits);
    limits.enabled_fields = CAPSID_RESOURCE_LIMIT_FILE_DESCRIPTORS;
    limits.file_descriptors = 3;
    capsid_worker_config_init(&config);
    config.worker_path = worker_path;
    config.resource_limits = &limits;
    require(
        capsid_worker_spawn(&config, &worker) == CAPSID_INVALID_ARGUMENT,
        "file descriptor limit below IPC requirement rejected");

    capsid_resource_limits_init(&limits);
    limits.enabled_fields = CAPSID_RESOURCE_LIMIT_CGROUP_CPU_MAX;
    limits.cgroup_cpu_quota_us = 10000;
    limits.cgroup_cpu_period_us = 100000;
    capsid_worker_config_init(&config);
    config.worker_path = worker_path;
    config.resource_limits = &limits;
    require(
        capsid_worker_spawn(&config, &worker) == CAPSID_INVALID_ARGUMENT,
        "cgroup resource limits without strict cgroup rejected");

    capsid_resource_limits_init(&limits);
    limits.enabled_fields = CAPSID_RESOURCE_LIMIT_CGROUP_CPU_MAX;
    limits.cgroup_cpu_quota_us = 0;
    limits.cgroup_cpu_period_us = 100000;
    capsid_worker_config_init(&config);
    config.worker_path = worker_path;
    config.strict_sandbox = 1;
    config.sandbox_cgroup_path = "/sys/fs/cgroup/capsid-invalid";
    config.resource_limits = &limits;
    require(
        capsid_worker_spawn(&config, &worker) == CAPSID_INVALID_ARGUMENT,
        "zero cgroup CPU quota rejected before spawn");

    capsid_resource_limits_init(&limits);
    limits.enabled_fields = CAPSID_RESOURCE_LIMIT_CGROUP_CPU_MAX;
    limits.cgroup_cpu_quota_us = 10000;
    limits.cgroup_cpu_period_us = 999;
    capsid_worker_config_init(&config);
    config.worker_path = worker_path;
    config.strict_sandbox = 1;
    config.sandbox_cgroup_path = "/sys/fs/cgroup/capsid-invalid";
    config.resource_limits = &limits;
    require(
        capsid_worker_spawn(&config, &worker) == CAPSID_INVALID_ARGUMENT,
        "cgroup CPU period below kernel minimum rejected before spawn");

    capsid_resource_limits_init(&limits);
    limits.enabled_fields = CAPSID_RESOURCE_LIMIT_CGROUP_CPU_WEIGHT;
    limits.cgroup_cpu_weight = 10001;
    capsid_worker_config_init(&config);
    config.worker_path = worker_path;
    config.strict_sandbox = 1;
    config.sandbox_cgroup_path = "/sys/fs/cgroup/capsid-invalid";
    config.resource_limits = &limits;
    require(
        capsid_worker_spawn(&config, &worker) == CAPSID_INVALID_ARGUMENT,
        "cgroup CPU weight outside kernel range rejected before spawn");

    capsid_worker_config_init(&config);
    config.worker_path = worker_path;
    const std::string oversized_ca_path(4097, 'a');
    config.tls_ca_bundle_path = oversized_ca_path.c_str();
    require(
        capsid_worker_spawn(&config, &worker) == CAPSID_INVALID_ARGUMENT,
        "oversized TLS CA bundle path rejected");

    capsid_worker_config_init(&config);
    config.worker_path = worker_path;
    config.max_fetch_request_body_bytes =
        UINT64_C(9007199254740991) + 1;
    require(
        capsid_worker_spawn(&config, &worker) == CAPSID_INVALID_ARGUMENT,
        "Fetch request body limit beyond exact JS integer range rejected");

    capsid_worker_config_init(&config);
    config.worker_path = worker_path;
    config.max_fetch_response_body_bytes =
        UINT64_C(9007199254740991) + 1;
    require(
        capsid_worker_spawn(&config, &worker) == CAPSID_INVALID_ARGUMENT,
        "Fetch response body limit beyond exact JS integer range rejected");

    capsid_egress_policy egress_policy;
    capsid_egress_policy_init(&egress_policy);
    egress_policy.struct_size--;
    capsid_worker_config_init(&config);
    config.worker_path = worker_path;
    config.egress_policy = &egress_policy;
    require(
        capsid_worker_spawn(&config, &worker) == CAPSID_INVALID_ARGUMENT,
        "undersized egress policy descriptor rejected");

    capsid_egress_policy_init(&egress_policy);
    egress_policy.rule_count = 1;
    capsid_worker_config_init(&config);
    config.worker_path = worker_path;
    config.egress_policy = &egress_policy;
    require(
        capsid_worker_spawn(&config, &worker) == CAPSID_INVALID_ARGUMENT,
        "egress policy with missing rules rejected");

    capsid_egress_rule egress_rule;
    capsid_egress_rule_init(&egress_rule);
    egress_rule.action = CAPSID_EGRESS_ALLOW;
    egress_rule.target = "10.1.2.3/8";
    capsid_egress_policy_init(&egress_policy);
    egress_policy.rules = &egress_rule;
    egress_policy.rule_count = 1;
    capsid_worker_config_init(&config);
    config.worker_path = worker_path;
    config.egress_policy = &egress_policy;
    require(
        capsid_worker_spawn(&config, &worker) == CAPSID_INVALID_ARGUMENT,
        "non-canonical egress CIDR rejected before spawn");

    const std::string maximum_hostname =
        std::string(63, 'a') + "." +
        std::string(63, 'b') + "." +
        std::string(63, 'c') + "." +
        std::string(61, 'd');
    std::vector<capsid_egress_rule> oversized_rules(256);
    for (size_t index = 0; index < oversized_rules.size(); ++index) {
        capsid_egress_rule_init(&oversized_rules[index]);
        oversized_rules[index].action = CAPSID_EGRESS_ALLOW;
        oversized_rules[index].target = maximum_hostname.c_str();
    }
    capsid_egress_policy_init(&egress_policy);
    egress_policy.rules = &oversized_rules[0];
    egress_policy.rule_count =
        static_cast<uint32_t>(oversized_rules.size());
    capsid_worker_config_init(&config);
    config.worker_path = "/capsid/nonexistent-worker";
    config.egress_policy = &egress_policy;
    require(
        capsid_worker_spawn(&config, &worker) == CAPSID_INVALID_ARGUMENT,
        "oversized HELLO egress policy rejected before spawn");

    capsid_worker_config_init(&config);
    config.worker_path = worker_path;
    config.strict_sandbox = 1;
    config.sandbox_required_features =
        CAPSID_SANDBOX_FEATURE_NETWORK_NAMESPACE;
    require(
        capsid_worker_spawn(&config, &worker) == CAPSID_INVALID_ARGUMENT,
        "required network namespace without descriptor rejected");

#if defined(__linux__)
    const int network_namespace =
        open("/proc/self/ns/net", O_RDONLY | O_CLOEXEC);
    require(network_namespace >= 0, "open current network namespace");
    capsid_worker_config_init(&config);
    config.worker_path = worker_path;
    config.sandbox_network_namespace_fd = network_namespace;
    require(
        capsid_worker_spawn(&config, &worker) == CAPSID_INVALID_ARGUMENT,
        "network namespace descriptor without strict mode rejected");
    close(network_namespace);

    const int regular_file = open("/dev/null", O_RDONLY | O_CLOEXEC);
    require(regular_file >= 0, "open non-namespace descriptor");
    capsid_worker_config_init(&config);
    config.worker_path = worker_path;
    config.strict_sandbox = 1;
    config.sandbox_network_namespace_fd = regular_file;
    require(
        capsid_worker_spawn(&config, &worker) == CAPSID_INVALID_ARGUMENT,
        "non-network-namespace descriptor rejected");
    close(regular_file);
#endif
}

void test_inflight_limit(const char *worker_path,
                         const std::string &bundle) {
    capsid_worker_config config;
    capsid_worker_config_init(&config);
    config.worker_path = worker_path;
    config.strict_sandbox = 0;
    config.max_inflight_requests = 1;

    capsid_worker *worker = NULL;
    require_result(capsid_worker_spawn(&config, &worker), "inflight spawn");
    require_result(
        capsid_worker_load_bundle(
            worker,
            reinterpret_cast<const uint8_t *>(bundle.data()),
            bundle.size()),
        "inflight load");
    wait_ready(worker);
    require_result(
        capsid_worker_begin_request(
            worker, 80, "GET", "https://example.test/reuse", NULL, 0),
        "first inflight request");
    require(
        capsid_worker_begin_request(
            worker, 81, "GET", "https://example.test/reuse", NULL, 0) ==
            CAPSID_WOULD_BLOCK,
        "host enforces max inflight requests atomically");
    require_result(capsid_worker_cancel(worker, 80), "cancel inflight request");
    require_result(
        capsid_worker_begin_request(
            worker, 81, "GET", "https://example.test/reuse", NULL, 0),
        "request admitted after inflight cancellation");
    require_result(capsid_worker_cancel(worker, 81), "cancel admitted request");
    capsid_worker_destroy(worker);
}

void test_configured_limits(const char *worker_path,
                            const std::string &bundle) {
    require(
        bundle.size() + 1024 < 8192,
        "P0 fixture must fit below the oversized response chunk");
    const uint32_t max_queued_bytes =
        static_cast<uint32_t>(bundle.size() + 1024);
    capsid_worker *worker =
        spawn_loaded(worker_path, bundle, 512, max_queued_bytes);

    std::vector<uint8_t> large_value(600, 'v');
    static const uint8_t name[] = "x-large";
    capsid_header header = {};
    header.name.data = name;
    header.name.size = sizeof(name) - 1;
    header.value.data = &large_value[0];
    header.value.size = large_value.size();
    require(
        capsid_worker_begin_request(
            worker,
            90,
            "GET",
            "https://example.test/reuse",
            &header,
            1) == CAPSID_INVALID_ARGUMENT,
        "host enforces configured request header limit");

    require_result(
        capsid_worker_begin_request(
            worker,
            91,
            "GET",
            "https://example.test/large-header",
            NULL,
            0),
        "begin large response header");
    require_result(
        capsid_worker_end_request(worker, 91),
        "end large response header");
    wait_request_error(worker, 91);

    require_result(
        capsid_worker_begin_request(
            worker,
            92,
            "GET",
            "https://example.test/large-chunk",
            NULL,
            0),
        "begin large response chunk");
    require_result(
        capsid_worker_end_request(worker, 92),
        "end large response chunk");
    // Queue-saturation fix: max_queued_bytes is a wire-queue limit, not
    // a per-response size limit. A body larger than the queue segments
    // and must complete, never fail closed (design §2 contract #2).
    bool completed = false;
    size_t bytes_received = 0;
    const std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline && !completed) {
        pump(worker);
        capsid_event event;
        while (next_event(worker, &event)) {
            if (event.type == CAPSID_EVENT_REQUEST_CREDIT) {
                continue;
            }
            if (event.type == CAPSID_EVENT_RESPONSE_BODY) {
                bytes_received += event.payload.size;
                require_result(
                    capsid_worker_grant_response_credit(
                        worker,
                        92,
                        static_cast<uint32_t>(event.payload.size)),
                    "grant large-chunk credit");
            } else if (event.type == CAPSID_EVENT_RESPONSE_END &&
                       event.request_id == 92) {
                completed = true;
            } else if (event.type == CAPSID_EVENT_ERROR &&
                       event.request_id == 92) {
                fail("large body failed instead of segmenting");
            } else if (event.type == CAPSID_EVENT_EXIT) {
                fail("worker exited during large-chunk response");
            }
        }
    }
    require(completed, "large response chunk completed");
    require(bytes_received == 8192, "large response chunk delivered 8192 bytes");
    capsid_worker_destroy(worker);
}

void test_crash_then_respawn(const char *worker_path,
                             const std::string &bundle) {
    capsid_worker *worker =
        spawn_loaded(worker_path, bundle, 64u * 1024u, 4u * 1024u * 1024u);
    require_result(capsid_worker_terminate(worker), "terminate worker");

    bool exited = false;
    const std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
        capsid_event event = {};
        event.struct_size = sizeof(event);
        const capsid_result result = capsid_worker_next_event(worker, &event);
        if (result == CAPSID_OK && event.type == CAPSID_EVENT_EXIT) {
            exited = true;
            break;
        }
        if (result != CAPSID_OK && result != CAPSID_WOULD_BLOCK &&
            result != CAPSID_CLOSED) {
            fail("unexpected crash event result");
        }
        const int fd = capsid_worker_fd(worker);
        if (fd >= 0) {
            capsid_pollfd descriptor = {};
            descriptor.fd = fd;
            descriptor.events = POLLIN;
            capsid::win32::capsid_poll(&descriptor, 1, 10);
        }
    }
    require(exited, "worker crash must surface as EXIT");
    capsid_worker_destroy(worker);

    capsid_worker *replacement =
        spawn_loaded(worker_path, bundle, 64u * 1024u, 4u * 1024u * 1024u);
    capsid_worker_destroy(replacement);
}

}  // namespace

int main(int argc, char **argv) {
    if (argc != 3) {
        fail("expected worker path and fixture path");
    }
    const std::string bundle = read_file(argv[2]);
    test_strict_sandbox_fails_closed(argv[1], bundle);
    test_bundle_enqueue_is_atomic(argv[1]);
    test_named_bundle_crosses_frame_boundary(argv[1]);
    test_invalid_config_is_rejected(argv[1]);
    test_inflight_limit(argv[1], bundle);
    test_configured_limits(argv[1], bundle);
    test_crash_then_respawn(argv[1], bundle);
    return 0;
}
