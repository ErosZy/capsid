#ifndef CAPSID_FROZEN_RUNTIME_V7_H
#define CAPSID_FROZEN_RUNTIME_V7_H

/*
 * Frozen consumer-side ABI v7 snapshot.
 *
 * Do not include the live public header here and do not add newer symbols.
 * This file proves that a binary built from the v7 layout still links and
 * that initializers do not write beyond the old structure.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CAPSID_ABI_VERSION 7u

typedef struct capsid_worker capsid_worker;
typedef struct capsid_resource_limits capsid_resource_limits;
typedef struct capsid_egress_policy capsid_egress_policy;
typedef struct capsid_capability_policy capsid_capability_policy;

typedef enum capsid_result {
    CAPSID_OK = 0,
    CAPSID_WOULD_BLOCK = 1,
    CAPSID_CLOSED = 2,
    CAPSID_INVALID_ARGUMENT = 3,
    CAPSID_PROTOCOL_ERROR = 4,
    CAPSID_SYSTEM_ERROR = 5,
    CAPSID_CHILD_ERROR = 6
} capsid_result;

typedef struct capsid_worker_config {
    uint32_t struct_size;
    uint32_t abi_version;
    const char *worker_path;
    uint64_t js_heap_limit;
    uint64_t process_memory_limit;
    uint64_t request_timeout_ms;
    uint32_t js_stack_size;
    uint32_t max_inflight_requests;
    uint32_t max_header_bytes;
    uint32_t max_queued_bytes;
    uint32_t initial_stream_window;
    uint8_t strict_sandbox;
    uint8_t reserved[7];
    const char *tls_ca_bundle_path;
    uint64_t max_fetch_request_body_bytes;
    uint64_t max_fetch_response_body_bytes;
    uint32_t sandbox_required_features;
    uint32_t sandbox_reserved;
    const char *sandbox_cgroup_path;
    const capsid_resource_limits *resource_limits;
    const capsid_egress_policy *egress_policy;
    const capsid_capability_policy *capability_policy;
    int32_t sandbox_network_namespace_fd;
    uint32_t egress_reserved;
} capsid_worker_config;

void capsid_worker_config_init(capsid_worker_config *config);
const char *capsid_result_string(capsid_result result);
capsid_result capsid_worker_spawn(
    const capsid_worker_config *config,
    capsid_worker **out_worker);
uint32_t capsid_recommended_worker_count(void);

#ifdef __cplusplus
}
#endif

#endif
