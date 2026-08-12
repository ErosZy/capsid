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
    CAPSID_EVENT_EXIT = 8,
    CAPSID_EVENT_REQUEST_TIMEOUT = 9,
    CAPSID_EVENT_AUDIT = 10,
    CAPSID_EVENT_MEMORY_METRICS = 11
} capsid_event_type;

/*
 * CAPSID_EVENT_RESPONSE_HEAD flag: `credit` carries the exact byte length
 * of a small complete non-streamed response body. The body/end events still
 * obey ordinary response credit; this is a representation hint, not a bypass
 * of flow control. Larger bodies use the pipelined representation and leave
 * the flag clear.
 */
#define CAPSID_RESPONSE_HEAD_FLAG_FIXED_BODY 1u

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
