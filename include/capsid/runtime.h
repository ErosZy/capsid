#ifndef CAPSID_RUNTIME_H
#define CAPSID_RUNTIME_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CAPSID_ABI_VERSION 7u
#define CAPSID_CAPABILITY_POLICY_VERSION_1 1u
#define CAPSID_CAPABILITY_POLICY_VERSION 2u
#define CAPSID_MEMORY_METRICS_VERSION 1u
#define CAPSID_BUILD_INFO_VERSION 2u

#define CAPSID_RESOURCE_UNLIMITED UINT64_MAX
#define CAPSID_RESOURCE_PIDS_UNLIMITED UINT32_MAX

/*
 * Host-only sandbox policy/status bits. They are never exposed to application
 * JavaScript. CAPSID_EVENT_READY reports the features actually applied in
 * capsid_event.flags.
 */
typedef enum capsid_sandbox_feature {
    CAPSID_SANDBOX_FEATURE_RLIMITS = 1u << 0,
    CAPSID_SANDBOX_FEATURE_NO_NEW_PRIVS = 1u << 1,
    CAPSID_SANDBOX_FEATURE_SECCOMP = 1u << 2,
    CAPSID_SANDBOX_FEATURE_LANDLOCK = 1u << 3,
    CAPSID_SANDBOX_FEATURE_USER_NAMESPACE = 1u << 4,
    CAPSID_SANDBOX_FEATURE_MOUNT_NAMESPACE = 1u << 5,
    CAPSID_SANDBOX_FEATURE_IPC_NAMESPACE = 1u << 6,
    CAPSID_SANDBOX_FEATURE_UTS_NAMESPACE = 1u << 7,
    CAPSID_SANDBOX_FEATURE_CGROUP_V2 = 1u << 8,
    CAPSID_SANDBOX_FEATURE_NETWORK_NAMESPACE = 1u << 9,
    CAPSID_SANDBOX_FEATURE_STRICT_BASE =
        CAPSID_SANDBOX_FEATURE_RLIMITS |
        CAPSID_SANDBOX_FEATURE_NO_NEW_PRIVS |
        CAPSID_SANDBOX_FEATURE_SECCOMP |
        CAPSID_SANDBOX_FEATURE_LANDLOCK,
    CAPSID_SANDBOX_FEATURE_ALL =
        CAPSID_SANDBOX_FEATURE_STRICT_BASE |
        CAPSID_SANDBOX_FEATURE_USER_NAMESPACE |
        CAPSID_SANDBOX_FEATURE_MOUNT_NAMESPACE |
        CAPSID_SANDBOX_FEATURE_IPC_NAMESPACE |
        CAPSID_SANDBOX_FEATURE_UTS_NAMESPACE |
        CAPSID_SANDBOX_FEATURE_CGROUP_V2 |
        CAPSID_SANDBOX_FEATURE_NETWORK_NAMESPACE
} capsid_sandbox_feature;

typedef struct capsid_worker capsid_worker;

typedef enum capsid_result {
    CAPSID_OK = 0,
    CAPSID_WOULD_BLOCK = 1,
    CAPSID_CLOSED = 2,
    CAPSID_INVALID_ARGUMENT = 3,
    CAPSID_PROTOCOL_ERROR = 4,
    CAPSID_SYSTEM_ERROR = 5,
    CAPSID_CHILD_ERROR = 6,
    /* WP-06 additions to ABI v7: no exception may cross an extern "C"
     * boundary. CAPSID_OUT_OF_MEMORY is a std::bad_alloc caught by the
     * ABI guard; CAPSID_INTERNAL_ERROR is any other C++ exception. */
    CAPSID_OUT_OF_MEMORY = 7,
    CAPSID_INTERNAL_ERROR = 8
} capsid_result;

/*
 * Thread-local error detail for the last Capsid API call on this thread.
 * Points into a fixed-size buffer owned by the Runtime: never free it, and
 * treat it as valid only until the next Capsid API call on the same
 * thread. Returns NULL when the last call succeeded (or never ran), and
 * on OOM paths only static text is stored — the mechanism never allocates.
 */
const char *capsid_last_error(void);

/*
 * Immutable identity of the Runtime/QuickJS bytecode toolchain plus the
 * provenance of the exact build being linked. String pointers returned by
 * capsid_runtime_build_info() have process lifetime and must not be freed.
 * compatibility_id is "sha256:" followed by 64 lowercase hexadecimal
 * digits and covers the bytecode-compatibility record v2 below in the
 * documented order. build_id is "sha256:" plus 64 lowercase hexadecimal
 * digits and covers the build-provenance record v1 minus its buildId line.
 *
 * This structure is additive to ABI v7. Version 2 (CAPSID_BUILD_INFO_VERSION
 * 2) appended the provenance fields after compatibility_id. Callers
 * compiled against the version-1 headers pass the smaller v1 struct_size
 * and still succeed: capsid_runtime_build_info() fills exactly the leading
 * fields that fit and never touches memory beyond the caller's buffer.
 * Callers must initialize the envelope first (capsid_build_info_init or
 * memset + struct_size) and preserve struct_size for size negotiation.
 */
typedef struct capsid_build_info {
    uint32_t struct_size;
    uint32_t version;
    const char *runtime_version;
    uint32_t abi_version;
    uint32_t fetchrpc_version;
    const char *quickjs_commit;
    const char *txiki_overlay_key;
    const char *txiki_overlay_manifest;
    const char *bytecode_compile_flags;
    const char *target_architecture;
    const char *endianness;
    uint32_t pointer_width_bits;
    const char *bytecode_format_identity;
    const char *capability_manifest_sha256;
    const char *compatibility_id;
    /* Build-info v2 (WP-07, spec §11.3/§11.4): provenance of the exact
     * linked build. capsid_commit is 40 lowercase hex on a git checkout,
     * "unknown" when the commit could not be resolved; capsid_tree_clean
     * is 1 when git status --porcelain was empty at configure time;
     * provenance_dirty is 1 whenever release packaging must reject this
     * build (non-Release configure, unresolved commit, or dirty tree).
     * build_feature_flags is one canonical ASCII string:
     *   "lto=ON|OFF asan=... ubsan=... tsan=... mimalloc=... host=... worker=..." */
    const char *build_id;
    const char *capsid_commit;
    uint32_t capsid_tree_clean;
    uint32_t provenance_dirty;
    const char *compiler_id;
    const char *compiler_version;
    const char *target_triple;
    const char *cmake_build_type;
    const char *build_feature_flags;
} capsid_build_info;

/*
 * Host-only resource controls. enabled_fields distinguishes an omitted value
 * from an explicit zero (for example, memory.swap.max = 0).
 */
typedef enum capsid_resource_limit_field {
    CAPSID_RESOURCE_LIMIT_FILE_DESCRIPTORS = 1u << 0,
    CAPSID_RESOURCE_LIMIT_CGROUP_CPU_MAX = 1u << 1,
    CAPSID_RESOURCE_LIMIT_CGROUP_CPU_WEIGHT = 1u << 2,
    CAPSID_RESOURCE_LIMIT_CGROUP_MEMORY_HIGH = 1u << 3,
    CAPSID_RESOURCE_LIMIT_CGROUP_MEMORY_MAX = 1u << 4,
    CAPSID_RESOURCE_LIMIT_CGROUP_MEMORY_SWAP_MAX = 1u << 5,
    CAPSID_RESOURCE_LIMIT_CGROUP_PIDS_MAX = 1u << 6,
    CAPSID_RESOURCE_LIMIT_ALL =
        CAPSID_RESOURCE_LIMIT_FILE_DESCRIPTORS |
        CAPSID_RESOURCE_LIMIT_CGROUP_CPU_MAX |
        CAPSID_RESOURCE_LIMIT_CGROUP_CPU_WEIGHT |
        CAPSID_RESOURCE_LIMIT_CGROUP_MEMORY_HIGH |
        CAPSID_RESOURCE_LIMIT_CGROUP_MEMORY_MAX |
        CAPSID_RESOURCE_LIMIT_CGROUP_MEMORY_SWAP_MAX |
        CAPSID_RESOURCE_LIMIT_CGROUP_PIDS_MAX
} capsid_resource_limit_field;

typedef struct capsid_resource_limits {
    uint32_t struct_size;
    uint32_t enabled_fields;
    uint32_t file_descriptors;
    /* Linux cgroup v2 cpu.weight: 1..10000. */
    uint32_t cgroup_cpu_weight;
    /*
     * Linux cgroup v2 cpu.max, in microseconds. quota may be
     * CAPSID_RESOURCE_UNLIMITED; period must be 1000..1000000.
     */
    uint64_t cgroup_cpu_quota_us;
    uint64_t cgroup_cpu_period_us;
    /*
     * Linux cgroup v2 byte limits. CAPSID_RESOURCE_UNLIMITED writes "max".
     * An explicit zero is valid when the corresponding bit is enabled.
     */
    uint64_t cgroup_memory_high_bytes;
    uint64_t cgroup_memory_max_bytes;
    uint64_t cgroup_memory_swap_max_bytes;
    /* CAPSID_RESOURCE_PIDS_UNLIMITED writes "max". */
    uint32_t cgroup_pids_max;
    uint32_t reserved;
} capsid_resource_limits;

/*
 * Host-only direct-egress policy. A NULL policy is deny-all. The descriptor
 * and every rule are copied synchronously by capsid_worker_spawn().
 *
 * Targets are exact ASCII hostnames, "*.example.com" DNS-label wildcards,
 * numeric IP addresses, or canonical IPv4/IPv6 CIDRs. A 0/0 port pair means
 * any port. Explicit deny rules always win. Resolved loopback, link-local,
 * private, unique-local, multicast, unspecified, and other protected
 * addresses require an explicit matching CIDR allow even when the policy's
 * default action is allow.
 */
typedef enum capsid_egress_action {
    CAPSID_EGRESS_DENY = 0,
    CAPSID_EGRESS_ALLOW = 1
} capsid_egress_action;

typedef struct capsid_egress_rule {
    uint32_t struct_size;
    capsid_egress_action action;
    const char *target;
    uint16_t port_start;
    uint16_t port_end;
    /* Stable host-provided audit identifier. Zero assigns index + 1. */
    uint32_t rule_id;
    uint32_t reserved;
} capsid_egress_rule;

typedef struct capsid_egress_policy {
    uint32_t struct_size;
    capsid_egress_action default_action;
    const capsid_egress_rule *rules;
    uint32_t rule_count;
    uint32_t reserved;
} capsid_egress_policy;

/*
 * Host-only capability policy. This is an immutable worker-startup snapshot:
 * capsid_worker_spawn() validates and synchronously copies every descriptor,
 * module name, rule, identity, and nested network policy.
 *
 * Build availability, module visibility, and operation permission are
 * independent gates. A policy can name a known optional module that was not
 * compiled; importing it then reports unavailable rather than granting it.
 */
typedef enum capsid_permission_name {
    CAPSID_PERMISSION_NONE = 0,
    CAPSID_PERMISSION_READ = 1,
    CAPSID_PERMISSION_WRITE = 2,
    CAPSID_PERMISSION_NET = 3,
    CAPSID_PERMISSION_ENV = 4,
    CAPSID_PERMISSION_SYS = 5,
    CAPSID_PERMISSION_FFI = 6,
    CAPSID_PERMISSION_RAW_SOCKET = 7,
    CAPSID_PERMISSION_STDIO = 8,
    CAPSID_PERMISSION_STORAGE = 9,
    CAPSID_PERMISSION_ENGINE = 10
} capsid_permission_name;

typedef enum capsid_permission_action {
    CAPSID_PERMISSION_DENY = 0,
    CAPSID_PERMISSION_ALLOW = 1
} capsid_permission_action;

typedef enum capsid_permission_state {
    CAPSID_PERMISSION_STATE_DENIED = 0,
    CAPSID_PERMISSION_STATE_GRANTED = 1,
    CAPSID_PERMISSION_STATE_PARTIAL = 2,
    CAPSID_PERMISSION_STATE_UNAVAILABLE = 3
} capsid_permission_state;

typedef struct capsid_permission_rule {
    uint32_t struct_size;
    capsid_permission_action action;
    capsid_permission_name permission;
    /*
     * NULL means the entire boolean capability. Path permissions use an
     * absolute lexical scope, env supports exact names and one trailing '*',
     * and sys/stdio/storage/engine use fixed resource names.
     * CAPSID_PERMISSION_NET must use capsid_capability_policy.net_policy.
     */
    const char *resource;
    /* Nonzero and unique within the capability policy. */
    uint32_t rule_id;
    uint32_t reserved;
} capsid_permission_rule;

/*
 * Explicit host-provided environment snapshot entry. Values are never read
 * from the worker process environment. The descriptor and both strings are
 * copied synchronously by capsid_worker_spawn().
 */
typedef struct capsid_env_entry {
    uint32_t struct_size;
    const char *name;
    const char *value;
    uint32_t reserved;
} capsid_env_entry;

typedef struct capsid_capability_policy {
    uint32_t struct_size;
    uint32_t version;
    const char *application_identity;
    const char *const *allowed_modules;
    uint32_t allowed_module_count;
    const capsid_permission_rule *rules;
    uint32_t rule_count;
    /*
     * The operation-level policy for standard fetch and future client
     * transports. If both this and capsid_worker_config.egress_policy are
     * provided, both must allow the operation.
     */
    const capsid_egress_policy *net_policy;
    uint32_t reserved;
    /*
     * Version 2: immutable values exposed by capsid:env. Every entry must be
     * covered by an effective CAPSID_PERMISSION_ENV allow rule, and the module
     * itself must be present in allowed_modules.
     */
    const capsid_env_entry *env_entries;
    uint32_t env_entry_count;
    uint32_t env_reserved;
} capsid_capability_policy;

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
    /*
     * Linux strict mode always requires CAPSID_SANDBOX_FEATURE_STRICT_BASE.
     * Other platforms fail closed until an equivalent policy is implemented.
     */
    uint8_t strict_sandbox;
    uint8_t reserved[7];
    const char *tls_ca_bundle_path;
    uint64_t max_fetch_request_body_bytes;
    uint64_t max_fetch_response_body_bytes;
    /*
     * Additional fail-closed Linux requirements. This field must be zero when
     * strict_sandbox is zero. Namespaces are installed inside the worker.
     */
    uint32_t sandbox_required_features;
    uint32_t sandbox_reserved;
    /*
     * Absolute path to a delegated cgroup v2 directory. The host library
     * applies enabled controller limits, verifies them, and attaches the
     * child before HELLO. Non-NULL implies CAPSID_SANDBOX_FEATURE_CGROUP_V2.
     */
    const char *sandbox_cgroup_path;
    /*
     * Optional typed resource controls. The descriptor is read synchronously
     * by capsid_worker_spawn() and is never exposed to JavaScript.
     */
    const capsid_resource_limits *resource_limits;
    /*
     * Immutable direct-fetch network policy. NULL denies every outbound
     * Fetch. The native HTTP client remains internal to the worker.
     */
    const capsid_egress_policy *egress_policy;
    /*
     * Optional immutable module/capability policy. NULL preserves the legacy
     * module-deny behavior and leaves direct fetch governed only by
     * egress_policy. When non-NULL, its net policy is an additional gate.
     */
    const capsid_capability_policy *capability_policy;
    /*
     * Linux network namespace descriptor prepared by the host. The namespace
     * must already contain the intended routing and firewall policy. -1
     * disables it. A nonnegative descriptor requires strict_sandbox and adds
     * CAPSID_SANDBOX_FEATURE_NETWORK_NAMESPACE to the required feature set.
     */
    int32_t sandbox_network_namespace_fd;
    uint32_t egress_reserved;
} capsid_worker_config;

/*
 * FetchRPC v4 capability bits (docs/fetchrpc-v4.md §1.2). Effective
 * capabilities are the conjunction with the worker's READY reply.
 * Requested through capsid_protocol_options (spawn_ex); the published
 * capsid_worker_config layout is frozen at the ABI v7 shape.
 */
#define CAPSID_PROTOCOL_CAP_DUPLEX_CHANNEL (1u << 0)
#define CAPSID_PROTOCOL_CAP_UPGRADE (1u << 1)
#define CAPSID_PROTOCOL_CAP_HALF_CLOSE (1u << 2)
#define CAPSID_PROTOCOL_CAP_CHANNEL_LAYOUT_A2 (1u << 3)
#define CAPSID_PROTOCOL_CAP_CHANNEL_CONFIRM (1u << 4)
/* Atomic final WebSocket Close; requires CHANNEL_CONFIRM. */
#define CAPSID_PROTOCOL_CAP_CHANNEL_CLOSE_FINAL (1u << 5)
/* Optional outbound Upgrade fetch metadata; requires UPGRADE, CHANNEL_CONFIRM
 * and CHANNEL_CLOSE_FINAL. A peer must announce actual metadata/body support. */
#define CAPSID_PROTOCOL_CAP_FETCH_UPGRADE (1u << 6)
/* Retained incoming I/O ownership after transport EOF. Requires UPGRADE
 * and CHANNEL_CONFIRM. Produces CONNECTION_LIFETIME events only when
 * explicitly negotiated; existing descriptors and event layout are unchanged. */
#define CAPSID_PROTOCOL_CAP_CONNECTION_LIFETIME (1u << 7)

/*
 * Protocol options descriptor for capsid_worker_spawn_ex. This is a
 * new, separately versioned descriptor: it never extends the frozen
 * capsid_worker_config layout. Two-way size negotiation: the caller
 * states its buffer size to ..._init and again at spawn; the library
 * reads only fields the caller's size covers, and unknown descriptor
 * versions are rejected instead of guessed.
 */
typedef struct capsid_protocol_options {
    uint32_t struct_size;
    /* Descriptor layout version, independent of CAPSID_ABI_VERSION. */
    uint32_t descriptor_version;
    /* Wire version: 3 (default) or 4. */
    uint16_t wire_version;
    uint16_t reserved16;
    /* CAPSID_PROTOCOL_CAP_* bitset; unknown bits are rejected. */
    uint32_t capabilities;
    /*
     * Optional channel limits (zero selects the deployment default).
     * Enforced from v4 connections that announce the A2 layout; ignored
     * (must be zero-validated only for sanity) on pure v4 carriage.
     */
    uint32_t max_channels;
    uint32_t reserved32;
    uint64_t max_message_bytes;
    uint64_t reassembly_budget_bytes;
    uint64_t send_budget_bytes;
    uint32_t close_timeout_ms;
    uint32_t dial_timeout_ms;
    uint32_t idle_timeout_ms;
    uint32_t per_channel_queue_bytes;
} capsid_protocol_options;

/* First descriptor layout version. */
#define CAPSID_PROTOCOL_DESCRIPTOR_VERSION 1u

/*
 * Negotiated protocol state of a spawned worker, via
 * capsid_worker_get_protocol_info. Same two-way size rules.
 */
typedef struct capsid_protocol_info {
    uint32_t struct_size;
    uint32_t descriptor_version;
    uint16_t wire_version;
    uint16_t reserved16;
    uint32_t host_capabilities;
    uint32_t worker_capabilities;
    uint32_t effective_capabilities;
} capsid_protocol_info;

typedef struct capsid_bytes {
    const uint8_t *data;
    size_t size;
} capsid_bytes;

typedef struct capsid_header {
    capsid_bytes name;
    capsid_bytes value;
} capsid_header;

typedef enum capsid_audit_stage {
    CAPSID_AUDIT_STAGE_BUILD = 1,
    CAPSID_AUDIT_STAGE_MODULE = 2,
    CAPSID_AUDIT_STAGE_OPERATION = 3,
    CAPSID_AUDIT_STAGE_QUERY = 4
} capsid_audit_stage;

typedef enum capsid_audit_decision {
    CAPSID_AUDIT_DENY = 0,
    CAPSID_AUDIT_ALLOW = 1,
    CAPSID_AUDIT_UNAVAILABLE = 2,
    CAPSID_AUDIT_PARTIAL = 3
} capsid_audit_decision;

typedef struct capsid_audit_record {
    uint32_t struct_size;
    uint32_t version;
    capsid_audit_stage stage;
    capsid_audit_decision decision;
    uint64_t worker_id;
    uint64_t request_id;
    uint32_t rule_id;
    uint32_t policy_version;
    capsid_bytes application_identity;
    capsid_bytes module;
    capsid_bytes capability;
    capsid_bytes resource_kind;
    capsid_bytes resource;
    capsid_bytes manifest_hash;
} capsid_audit_record;

/*
 * Host-only snapshot returned on explicit request. Computing it walks the
 * QuickJS heap, so it is intended for diagnostics and regression gates, not
 * per-request telemetry. It is never exposed to application JavaScript.
 */
typedef struct capsid_memory_metrics {
    uint32_t struct_size;
    uint32_t version;
    uint64_t malloc_size;
    uint64_t malloc_limit;
    uint64_t memory_used_size;
    uint64_t atom_count;
    uint64_t atom_size;
    uint64_t string_count;
    uint64_t string_size;
    uint64_t object_count;
    uint64_t object_size;
    uint64_t property_count;
    uint64_t property_size;
    uint64_t shape_count;
    uint64_t shape_size;
    uint64_t js_function_count;
    uint64_t js_function_size;
    uint64_t js_function_code_size;
    uint64_t binary_object_count;
    uint64_t binary_object_size;
} capsid_memory_metrics;

typedef enum capsid_event_type {
    CAPSID_EVENT_NONE = 0,
    CAPSID_EVENT_READY = 1,
    CAPSID_EVENT_REQUEST_CREDIT = 2,
    CAPSID_EVENT_RESPONSE_HEAD = 3,
    CAPSID_EVENT_RESPONSE_BODY = 4,
    CAPSID_EVENT_RESPONSE_END = 5,
    CAPSID_EVENT_LOG = 6,
    CAPSID_EVENT_ERROR = 7,
    /* IPC peer closed. Unused fields, including status, are zero; this
     * event does not carry the operating system process exit status. */
    CAPSID_EVENT_EXIT = 8,
    CAPSID_EVENT_REQUEST_TIMEOUT = 9,
    CAPSID_EVENT_AUDIT = 10,
    CAPSID_EVENT_MEMORY_METRICS = 11,
    /*
     * FetchRPC v4 A2 channel events (T07). Delivered only on workers
     * spawned with capsid_worker_spawn_ex at wire version 4 and the
     * A2/DUPLEX capabilities; a v3 worker can never produce them.
     * event.payload carries the raw A2 frame payload; decode it with
     * capsid_channel_event_decode. capsid_event itself is NOT extended.
     */
    CAPSID_EVENT_CHANNEL_DIAL = 12,       /* worker requests an outbound dial */
    CAPSID_EVENT_CHANNEL_DATA = 13,       /* worker->host message fragment */
    CAPSID_EVENT_CHANNEL_CREDIT = 14,     /* worker granted host credit */
    CAPSID_EVENT_CHANNEL_HALF_CLOSE = 15, /* worker closed its send side */
    CAPSID_EVENT_CHANNEL_CLOSE = 16,      /* worker closed the channel */
    CAPSID_EVENT_CHANNEL_CANCEL = 17,     /* worker cancelled the channel */
    CAPSID_EVENT_CHANNEL_ACK = 18,        /* result committed or terminal confirmed */
    /* credit is the validated total number of retained local incoming I/O
     * owners. payload is the 8-byte v4 lifetime record. Network reservations
     * are accounted separately; request_id and flags are zero. */
    CAPSID_EVENT_CONNECTION_LIFETIME = 19
} capsid_event_type;

/*
 * CAPSID_EVENT_RESPONSE_HEAD flag: `credit` carries the exact byte length
 * of a small complete non-streamed response body. The body/end events still
 * obey ordinary response credit; this is a representation hint, not a bypass
 * of flow control. Larger bodies use the pipelined representation and leave
 * the flag clear.
 */
#define CAPSID_RESPONSE_HEAD_FLAG_FIXED_BODY 1u
/* A 101 Upgrade is an implicit HTTP terminal. credit carries the assigned
 * channel id; payload remains the original borrowed header block. No
 * RESPONSE_END follows. The channel lives independently of request_id. */
#define CAPSID_RESPONSE_HEAD_FLAG_UPGRADE 32u

typedef struct capsid_event {
    uint32_t struct_size;
    capsid_event_type type;
    uint64_t request_id;
    /*
     * CAPSID_EVENT_READY uses capsid_sandbox_feature bits and carries the
     * worker compatibility ID as non-NUL-terminated ASCII in payload.
     */
    uint32_t flags;
    uint32_t status;
    uint32_t credit;
    capsid_bytes payload;
} capsid_event;

/* ------------------------------------------------------------------ */
/* FetchRPC v4 A2 channel ABI (frozen 2026-10-12, GLM plan T07).       */
/* All channel entry points: require capsid_worker_spawn_ex with wire  */
/* version 4 and the negotiated CAPSID_PROTOCOL_CAP_DUPLEX_CHANNEL |   */
/* ..._CHANNEL_LAYOUT_A2 bits; return CAPSID_PROTOCOL_ERROR when the   */
/* connection did not negotiate them, and CAPSID_WOULD_BLOCK only      */
/* where documented. Inputs are copied synchronously: no entry point   */
/* borrows caller memory past its return.                              */

/* capsid_worker_channel_send flags (frame flags on the wire). */
#define CAPSID_CHANNEL_FLAG_START 1u
#define CAPSID_CHANNEL_FLAG_END 2u
#define CAPSID_CHANNEL_FLAG_TEXT 4u

/* capsid_worker_channel_cancel reasons (contract 3.2 cancel table). */
#define CAPSID_CANCEL_REASON_LOCAL 0u
#define CAPSID_CANCEL_REASON_DISCONNECT 1u
#define CAPSID_CANCEL_REASON_TIMEOUT 2u
#define CAPSID_CANCEL_REASON_SHUTDOWN 3u
#define CAPSID_CANCEL_REASON_RESOURCE_LIMITS 4u
#define CAPSID_CANCEL_REASON_PROTOCOL_VIOLATION 5u

#define CAPSID_CHANNEL_ACK_DIAL_RESULT 0u
#define CAPSID_CHANNEL_ACK_CANCEL 1u
#define CAPSID_CHANNEL_ACK_TRANSPORT_CLOSE 2u

/* First capsid_channel_event layout version. */
#define CAPSID_CHANNEL_EVENT_VERSION 1u

/*
 * Typed view over a CAPSID_EVENT_CHANNEL_* event. `message`, `url`,
 * and `diagnostic` point INTO the capsid_event payload: they are
 * borrowed and valid until the next capsid_worker_next_event call on
 * the same worker. channel_id/dial_id/credit/close_kind/code are
 * decoded copies. Initialize with capsid_channel_event_init; decode
 * with capsid_channel_event_decode. The caller passes its buffer size;
 * the decoder writes only that prefix and stamps the output version.
 */
typedef struct capsid_channel_event {
    uint32_t struct_size;
    uint32_t version;
    /* One of CAPSID_EVENT_CHANNEL_DIAL..ACK (mirrors the event). */
    uint32_t kind;
    uint32_t channel_id;
    uint64_t dial_id;
    /* capsid_protocol owner kind (open/dial events; else 0). */
    uint32_t owner_kind;
    /* Close events: contract close kind + code (0 when absent). */
    uint32_t close_kind;
    uint16_t code;
    uint16_t reserved16;
    /* Credit events: the granted amount. */
    uint64_t credit;
    /* DATA events: borrowed fragment bytes (after the id prefix). */
    capsid_bytes message;
    /* DIAL events: borrowed target URL. */
    capsid_bytes url;
    /* CLOSE events: borrowed reason bytes. */
    capsid_bytes diagnostic;
} capsid_channel_event;

capsid_result capsid_channel_event_init_sized(capsid_channel_event *event,
                                              uint32_t caller_size);
/* The initializer's size is evaluated in the caller's translation unit. */
static inline void capsid_channel_event_init(capsid_channel_event *event) {
    (void)capsid_channel_event_init_sized(event, (uint32_t)sizeof(*event));
}
capsid_result capsid_channel_event_decode(const capsid_event *event,
                                          capsid_channel_event *out,
                                          uint32_t caller_size);
/* CANCEL reason is a scalar getter; the v1 typed view keeps its layout. */
capsid_result capsid_channel_cancel_reason(const capsid_event *event,
                                            uint32_t *out_reason);
capsid_result capsid_channel_ack_kind(const capsid_event *event,
                                       uint32_t *out_kind);
/* CONFIRM capability: stop the channel's producer and drain/cancel its IO
 * before acknowledging CANCEL/transport close. Use channel_id=0 and a
 * nonzero dial_id for pending-dial cancellation; assigned identities use
 * a nonzero channel_id and dial_id=0. WOULD_BLOCK consumes no state; retry
 * with control priority. A dial ACCEPT remains provisional until the pump
 * delivers ACK_DIAL_RESULT, so no channel DATA may be sent before it.
 * The event and typed-view layouts remain unchanged. */
capsid_result capsid_worker_channel_confirm(capsid_worker *worker,
                                              uint32_t channel_id,
                                              uint64_t dial_id,
                                              uint32_t kind);

/*
 * Sends one DATA fragment (flags: CAPSID_CHANNEL_FLAG_*). The message
 * must fit the negotiated per-message cap; fragments are credit-bound
 * per direction. The bytes are copied before returning.
 */
capsid_result capsid_worker_channel_send(capsid_worker *worker,
                                         uint32_t channel_id,
                                         const uint8_t *data,
                                         size_t size,
                                         uint32_t flags);
/* Grants the worker `credit` additional send bytes on the channel. */
capsid_result capsid_worker_channel_grant(capsid_worker *worker,
                                          uint32_t channel_id,
                                          uint64_t credit);
/* Half-closes the host's send side (requires the HALF_CLOSE bit). */
capsid_result capsid_worker_channel_half_close(capsid_worker *worker,
                                               uint32_t channel_id);
/* Close handshake; code/reason follow the contract close matrix.
 * code == 0 with an empty reason encodes an empty Close body. */
capsid_result capsid_worker_channel_close(capsid_worker *worker,
                                          uint32_t channel_id,
                                          uint16_t code,
                                          const char *reason);
/* Explicit close kind: 0 initiated, 1 observed network Close, 2 transport,
 * 3 final (send Close then stop transport, requires CHANNEL_CLOSE_FINAL).
 * Transport codes are synthetic diagnostics and must not go on the network. */
capsid_result capsid_worker_channel_close_ex(capsid_worker *worker,
                                             uint32_t channel_id,
                                             uint32_t kind,
                                             uint16_t code,
                                             const char *reason);
/* Counted UTF-8 reason; preserves embedded NUL bytes permitted by RFC 6455.
 * Input is copied synchronously. The original C-string APIs remain valid. */
capsid_result capsid_worker_channel_close_bytes(capsid_worker *worker,
                                                uint32_t channel_id,
                                                uint32_t kind,
                                                uint16_t code,
                                                const uint8_t *reason,
                                                size_t reason_size);
/* Terminal cancel (reason: CAPSID_CANCEL_REASON_*). */
capsid_result capsid_worker_channel_cancel(capsid_worker *worker,
                                           uint32_t channel_id,
                                           uint32_t reason);
/*
 * Reserves an inbound Upgrade for the next begin_request (or
 * begin_bodyless_request) using request_id. That call sends its normal
 * head with the upgrade flag and A2 tail. A queue refusal preserves the
 * reservation for retry. The worker's 101 commits an independent channel;
 * Both begin variants mark an upgrade bodyless and ended; callers must not
 * write a request body or issue end_request again for this handshake.
 * a non-101 response releases it. Cancel channel_id to abort a reservation.
 * Host-assigned ids must be announced in increasing order on the IPC stream.
 */
capsid_result capsid_worker_upgrade_begin(capsid_worker *worker,
                                          uint64_t request_id,
                                          uint32_t channel_id,
                                          uint64_t max_message_bytes);
/* Accepts a worker dial: assigns channel_id at the accepted cap. */
capsid_result capsid_worker_dial_accept(capsid_worker *worker,
                                        uint64_t dial_id,
                                        uint32_t channel_id,
                                        uint64_t max_message_bytes,
                                        const char *selected_protocol);
/* Additive ABI-v7 descriptor for a negotiated FETCH_UPGRADE result.
 * Initialize every field (including struct_size and version) before use.
 * Names/values are counted HTTP ByteString octets, with duplicate headers
 * preserved in order. All input bytes are copied before the call returns.
 * A non-101 response uses the channel for Host-to-Worker binary body chunks;
 * end its body with CLOSE_FINAL without a WebSocket code or reason. */
#define CAPSID_FETCH_UPGRADE_RESPONSE_VERSION 1u
typedef struct capsid_fetch_upgrade_response {
    uint32_t struct_size;
    uint32_t version;
    uint32_t status;
    uint32_t redirected;  /* 0 or 1 */
    capsid_bytes status_text;
    capsid_bytes url;
    const capsid_header *headers;
    size_t header_count;
} capsid_fetch_upgrade_response;

/* Requires the negotiated FETCH_UPGRADE capability and matching fetch OPEN.
 * Ordinary dial_accept remains the constructor-WebSocket operation.
 * Queue refusal preserves the pending dial and assigned id for retry. */
capsid_result capsid_worker_dial_accept_http(capsid_worker *worker,
                                             uint64_t dial_id,
                                             uint32_t channel_id,
                                             uint64_t max_message_bytes,
                                             const capsid_fetch_upgrade_response *response);
/* Rejects a worker dial (contract REJECT reason codes 0..9). */
capsid_result capsid_worker_dial_reject(capsid_worker *worker,
                                        uint64_t dial_id,
                                        uint32_t reason_code,
                                        const char *diagnostic);


void capsid_resource_limits_init(capsid_resource_limits *limits);
void capsid_egress_rule_init(capsid_egress_rule *rule);
void capsid_egress_policy_init(capsid_egress_policy *policy);
void capsid_permission_rule_init(capsid_permission_rule *rule);
void capsid_env_entry_init(capsid_env_entry *entry);
void capsid_capability_policy_init(capsid_capability_policy *policy);
void capsid_audit_record_init(capsid_audit_record *record);
void capsid_memory_metrics_init(capsid_memory_metrics *metrics);
/*
 * Initializes the build-info envelope for size negotiation. Stamps the
 * CALLER's struct size — which only the caller's own header knows — so
 * this is an inline: callers compiled against the current headers use it
 * directly. Callers compiled against the build-info v1 headers (which
 * declare it as a plain function) link the library's legacy symbol of the
 * same name instead, which stamps a v1-sized envelope and never writes
 * past a v1 buffer. In both cases the version stamped is the current
 * CAPSID_BUILD_INFO_VERSION; a caller from an older ABI must treat newer
 * versions conservatively.
 */
#ifndef CAPSID_BUILD_INFO_INIT_IMPLEMENTATION
static inline void capsid_build_info_init(capsid_build_info *info) {
    if (info == NULL) {
        return;
    }
    info->struct_size = (uint32_t)sizeof(*info);
    info->version = CAPSID_BUILD_INFO_VERSION;
}
#endif
void capsid_worker_config_init(capsid_worker_config *config);
const char *capsid_result_string(capsid_result result);

/*
 * Returns the library-side build identity and provenance. struct_size is a
 * size negotiation: the caller announces its buffer size and only the
 * leading fields that fit are written. Callers compiled against the
 * build-info v1 headers pass the v1 size and receive the v1 fields;
 * anything below the v1 size is CAPSID_INVALID_ARGUMENT.
 *
 * The compatibility ID is computed from this exact canonical UTF-8 record
 * (compatibility record v2, spec §11.2), including the final newline:
 *
 * schema=capsid-bytecode-compatibility-v2
 * quickjsCommit=<quickjs_commit>
 * txikiOverlayManifest=<txiki_overlay_manifest>
 * bytecodeCompileFlags=<bytecode_compile_flags>
 * targetArchitecture=<target_architecture>
 * endianness=<endianness>
 * pointerWidthBits=<pointer_width_bits decimal>
 * bytecodeFormatIdentity=<bytecode_format_identity>
 *
 * Hash the record with SHA-256 and prefix its lowercase hexadecimal digest
 * with "sha256:". No locale-dependent formatting or JSON canonicalization is
 * involved.
 *
 * The build ID (build_id) is computed from the provenance record v1
 * (spec §11.3) minus its final buildId line, in this order:
 *
 * schema=capsid-build-provenance-v1
 * capsidCommit=<capsid_commit>
 * capsidTreeClean=<true|false>
 * runtimeVersion=<runtime_version>
 * abiVersion=<abi_version decimal>
 * fetchRpcVersion=<fetchrpc_version decimal>
 * compatibilityId=<compatibility_id>
 * capabilityManifestSha256=<capability_manifest_sha256>
 * compilerId=<compiler_id>
 * compilerVersion=<compiler_version>
 * targetTriple=<target_triple>
 * cmakeBuildType=<cmake_build_type>
 * featureFlags=<build_feature_flags>
 * dependencyOverlayKey=<txiki_overlay_key>
 */
capsid_result capsid_runtime_build_info(capsid_build_info *out_info);

capsid_result capsid_worker_spawn(const capsid_worker_config *config, capsid_worker **out_worker);

/*
 * Protocol-selected spawn. options == NULL is exactly the published v3
 * behavior of capsid_worker_spawn. With options the wire version,
 * requested capabilities, and (for the A2 layout) channel limits are
 * negotiated: every option field is validated BEFORE any process,
 * socketpair, cgroup, or handle is created, so a rejected spawn leaves
 * no resources behind and sets *out_worker to NULL.
 *
 * The options descriptor is copied synchronously; callers may free it
 * immediately. Two-way size negotiation: capsid_protocol_options_init
 * records the caller's buffer size, and this entry point reads only the
 * fields that size covers. Unknown descriptor versions, nonzero
 * reserved fields, unknown capability bits, and unsatisfiable
 * dependency combinations are CAPSID_INVALID_ARGUMENT.
 */
capsid_result capsid_worker_spawn_ex(const capsid_worker_config *config,
                                     const capsid_protocol_options *options,
                                     capsid_worker **out_worker);

/*
 * Initializes a protocol options descriptor in the caller's buffer.
 * caller_size is the size of the caller's allocation; the function
 * writes at most min(caller_size, library-known size) bytes and
 * records caller_size in struct_size. A caller_size smaller than the
 * fixed prefix (through per_channel_queue_bytes) is
 * CAPSID_INVALID_ARGUMENT and writes nothing. Defaults: descriptor
 * version 1, wire version 3, no capabilities, zero limits (deployment
 * defaults).
 */
capsid_result capsid_protocol_options_init(capsid_protocol_options *options,
                                           uint32_t caller_size);

/*
 * Negotiated protocol state after the worker's READY. Fills the
 * caller's capsid_protocol_info buffer (same two-way size rules;
 * caller_size is the caller's allocation size). Returns
 * CAPSID_WOULD_BLOCK until READY has been observed on this worker.
 */
capsid_result capsid_worker_get_protocol_info(const capsid_worker *worker,
                                              capsid_protocol_info *out_info,
                                              uint32_t caller_size);
/*
 * Abortive cleanup (spec §13.3): closes the IPC channel and terminates the
 * worker process immediately — in-flight requests are dropped without
 * warning and the worker gets no chance to flush application state. The
 * child is always reaped before destroy returns; it allocates nothing and
 * never throws. A host that wants a graceful stop must run the explicit
 * sequence capsid_worker_shutdown → capsid_worker_flush → poll
 * capsid_worker_next_event until CAPSID_EVENT_EXIT / CAPSID_CLOSED →
 * capsid_worker_destroy. Terminate-on-deadline is the only non-abortive
 * escape: the Host's normal stop is graceful, and only a shutdown deadline
 * turns into capsid_worker_terminate.
 */
void capsid_worker_destroy(capsid_worker *worker);
int capsid_worker_fd(const capsid_worker *worker);
int64_t capsid_worker_pid(const capsid_worker *worker);

/*
 * Host execution topology. The recommended count is bounded by the calling
 * process CPU affinity and, on Linux cgroup v2, cpu.max. Available CPU
 * indices reflect the calling process affinity. Affinity is host-only and
 * never observable by application JavaScript.
 *
 * Frozen conservative fallbacks (WP-06, spec §10.2): if querying the host
 * topology raises an internal error, capsid_recommended_worker_count()
 * and capsid_available_cpu_count() return 1 and capsid_available_cpu_at()
 * returns CAPSID_INTERNAL_ERROR; in all three cases capsid_last_error()
 * is set on the calling thread.
 */
uint32_t capsid_recommended_worker_count(void);
uint32_t capsid_available_cpu_count(void);
capsid_result capsid_available_cpu_at(uint32_t index, uint32_t *out_cpu);
capsid_result capsid_worker_set_cpu_affinity(capsid_worker *worker, uint32_t cpu);

capsid_result capsid_worker_load_bundle(capsid_worker *worker, const uint8_t *bundle, size_t bundle_size);
capsid_result capsid_worker_load_bundle_named(capsid_worker *worker,
                                          const uint8_t *bundle,
                                          size_t bundle_size,
                                          const char *source_name);

/*
 * Binding v1 (docs/binding-technical-design.md §6): additive, versioned
 * descriptor API. Every field is copied before the call returns; the
 * descriptor may only be loaded before capsid_worker_load_bundle* — after
 * the bundle call the sequence is sealed and further loads fail with
 * CAPSID_INVALID_ARGUMENT. Secret values never appear in logs, digests or
 * audit output.
 */
#define CAPSID_BINDING_DESCRIPTOR_VERSION 1u

typedef struct capsid_binding_secret {
    const char *key;
    capsid_bytes value;
} capsid_binding_secret;

typedef struct capsid_binding_policy {
    // Manifest permission modules (the Binding Runtime's module grants;
    // the App never repeats them).
    const char *const *modules;
    uint32_t module_count;
    const char *const *net_rules;
    uint32_t net_rule_count;
    const char *const *fs_read;
    uint32_t fs_read_count;
    const char *const *fs_write;
    uint32_t fs_write_count;
    const char *const *env;
    uint32_t env_count;
    const char *const *stdio;
    uint32_t stdio_count;
} capsid_binding_policy;

typedef struct capsid_sandbox_requirements {
    const char *const *profiles;
    uint32_t profile_count;
} capsid_sandbox_requirements;

typedef struct capsid_binding_descriptor {
    uint32_t struct_size;
    uint32_t version;
    const char *binding_name;
    capsid_bytes source;
    capsid_bytes config_json;
    const capsid_binding_secret *secrets;
    uint32_t secret_count;
    const capsid_binding_policy *policy;
    const capsid_sandbox_requirements *sandbox;
} capsid_binding_descriptor;

capsid_result capsid_worker_load_binding(
    capsid_worker *worker,
    const capsid_binding_descriptor *binding);

/*
 * Loads QuickJS bytecode produced by the exact same trusted Capsid/QuickJS
 * build. QuickJS bytecode is not a portable or hardened input format:
 * corrupted, incompatible, or attacker-controlled bytes may cause memory
 * corruption. Hosts must never pass tenant-provided or otherwise untrusted
 * data to this function. source_name must exactly match the module name
 * embedded when the bytecode was compiled.
 */
capsid_result capsid_worker_load_trusted_bytecode_named(
    capsid_worker *worker,
    const uint8_t *bytecode,
    size_t bytecode_size,
    const char *source_name);
capsid_result capsid_worker_begin_request(capsid_worker *worker,
                                      uint64_t request_id,
                                      const char *method,
                                      const char *url,
                                      const capsid_header *headers,
                                      size_t header_count);
// Optimized begin for GET/HEAD requests: sets the END_REQUEST flag on the
// RequestHead frame so the worker skips the initial request-direction credit
// and marks request_ended immediately. Saves one frame per bodyless request.
capsid_result capsid_worker_begin_bodyless_request(capsid_worker *worker,
                                      uint64_t request_id,
                                      const char *method,
                                      const char *url,
                                      const capsid_header *headers,
                                      size_t header_count);
capsid_result capsid_worker_write_request(capsid_worker *worker,
                                      uint64_t request_id,
                                      const uint8_t *data,
                                      size_t size);
capsid_result capsid_worker_end_request(capsid_worker *worker, uint64_t request_id);
capsid_result capsid_worker_grant_response_credit(capsid_worker *worker, uint64_t request_id, uint32_t credit);
capsid_result capsid_worker_cancel(capsid_worker *worker, uint64_t request_id);
/*
 * Queues one host-only heap snapshot request. The response is delivered as
 * CAPSID_EVENT_MEMORY_METRICS and decoded with capsid_memory_metrics_decode().
 */
capsid_result capsid_worker_request_memory_metrics(capsid_worker *worker);
capsid_result capsid_worker_flush(capsid_worker *worker);
capsid_result capsid_worker_next_event(capsid_worker *worker, capsid_event *event);
/*
 * Graceful stop (spec §13.3), used only as part of the explicit sequence
 * shutdown → flush → drain EXIT → destroy: queues the worker-side shutdown
 * frame; the worker flushes its state and exits, after which next_event
 * reports CAPSID_EVENT_EXIT and the channel closes. A caller that abandons
 * the drain must still run capsid_worker_destroy (abortive) to reap the
 * child.
 */
capsid_result capsid_worker_shutdown(capsid_worker *worker);
/*
 * Abortive signal: SIGKILLs the worker process without a graceful frame.
 * Use only when a shutdown deadline expired; the worker must still be
 * reaped with capsid_worker_destroy afterwards.
 */
capsid_result capsid_worker_terminate(capsid_worker *worker);

/*
 * Response header views point into event->payload and remain valid only until
 * the next capsid_worker_next_event call for the same worker.
 */
capsid_result capsid_response_header_count(const capsid_event *event, size_t *out_count);
capsid_result capsid_response_header_at(const capsid_event *event,
                                    size_t index,
                                    capsid_header *out_header);
capsid_result capsid_response_status_text(const capsid_event *event,
                                      capsid_bytes *out_status_text);

capsid_result capsid_audit_record_decode(
    const capsid_event *event,
    capsid_audit_record *out_record);
capsid_result capsid_memory_metrics_decode(
    const capsid_event *event,
    capsid_memory_metrics *out_metrics);

#ifdef __cplusplus
}
#endif

#endif
