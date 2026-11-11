// WP-06, spec §10.4: no C++ exception may cross an extern "C" boundary.
//
// A controllable global operator new countdown sweeps every allocation
// observed in a successful spawn for the selected configuration. Request,
// bundle, event and shutdown scenarios also exercise their first failure.
// Each injected allocation failure must yield
// CAPSID_OUT_OF_MEMORY (or CAPSID_INTERNAL_ERROR for injected
// non-bad_alloc failures) with a non-empty capsid_last_error(), never a
// crash, never a leaked child.
//
// Usage: test-abi-guard [worker_path [configuration]]
// A worker path enables spawn sweeps and request scenarios. Without it,
// controlled missing-executable and allocator checks run independently.
#include "capsid/runtime.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <stdexcept>
#include <string>
#include <array>
#include <exception>

#if defined(_WIN32)
#include <windows.h>
#endif

#if defined(__linux__)
#include <errno.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

// Keep allocator probe entry points observable under optimization.
// Inlining their malloc/free implementation into callers also makes GCC
// diagnose a false new/free mismatch for the intentional replacements.
#if defined(_MSC_VER)
#define CAPSID_TEST_NOINLINE __declspec(noinline)
#elif defined(__GNUC__)
#define CAPSID_TEST_NOINLINE __attribute__((noinline))
#else
#define CAPSID_TEST_NOINLINE
#endif

namespace {

// Global allocation-failure countdown. g_fail_countdown >= 0 arms it:
// the allocation at which it reaches 0 fails (with bad_alloc, or with a
// runtime_error when g_inject_runtime_error is set).
long g_fail_countdown = -1;
bool g_inject_runtime_error = false;
bool g_runtime_error_raised = false;
bool g_runtime_error_swallowed = false;
long g_allocation_count = 0;
bool g_count_allocations = false;
enum class ConfigurationCase {
    Default, EgressEmpty, EgressRules, CapabilityEmpty, PermissionRule,
    CapabilitySnapshot, V4Carriage, V4Channels, SnapshotV4Channels
};
ConfigurationCase g_configuration_case = ConfigurationCase::Default;


}  // namespace

CAPSID_TEST_NOINLINE void *operator new(std::size_t size) {
    if (g_count_allocations) ++g_allocation_count;
    if (g_fail_countdown >= 0) {
        if (g_fail_countdown == 0) {
            if (g_inject_runtime_error) {
                // Disarm before constructing: the exception object itself
                // allocates, which would recurse into this allocator.
                g_fail_countdown = -1;
                g_runtime_error_raised = true;
                throw std::runtime_error(
                    "injected non-bad_alloc failure");
            }
            throw std::bad_alloc();
        }
        --g_fail_countdown;
    }
    if (void *memory = std::malloc(size ? size : 1)) {
        return memory;
    }
    throw std::bad_alloc();
}

CAPSID_TEST_NOINLINE void *operator new[](std::size_t size) {
    return ::operator new(size);
}

CAPSID_TEST_NOINLINE void *operator new(std::size_t size, const std::nothrow_t &) noexcept {
    // Own the nothrow variants too: without these definitions the sanitizer
    // runtime provides them, and the later delete → std::free would trip
    // ASan's alloc-dealloc-mismatch (nothrow-new memory freed as malloc).
    try {
        return ::operator new(size);
    } catch (...) {
        if (g_inject_runtime_error && g_runtime_error_raised) {
            g_runtime_error_swallowed = true;
        }
        return nullptr;
    }
}

CAPSID_TEST_NOINLINE void *operator new[](std::size_t size, const std::nothrow_t &) noexcept {
    return ::operator new(size, std::nothrow);
}

CAPSID_TEST_NOINLINE void operator delete(void *memory) noexcept {
    std::free(memory);
}

CAPSID_TEST_NOINLINE void operator delete[](void *memory) noexcept {
    std::free(memory);
}

CAPSID_TEST_NOINLINE void operator delete(void *memory, std::size_t) noexcept {
    std::free(memory);
}

CAPSID_TEST_NOINLINE void operator delete[](void *memory, std::size_t) noexcept {
    std::free(memory);
}

CAPSID_TEST_NOINLINE void operator delete(void *memory, const std::nothrow_t &) noexcept {
    std::free(memory);
}

CAPSID_TEST_NOINLINE void operator delete[](void *memory, const std::nothrow_t &) noexcept {
    std::free(memory);
}

CAPSID_TEST_NOINLINE void operator delete(void *memory, std::size_t,
                     const std::nothrow_t &) noexcept {
    std::free(memory);
}

CAPSID_TEST_NOINLINE void operator delete[](void *memory, std::size_t,
                       const std::nothrow_t &) noexcept {
    std::free(memory);
}

static long g_checks = 0;

#define CHECK(condition)                                                  \
    do {                                                                  \
        if (!(condition)) {                                               \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                         #condition);                                     \
            return 1;                                                     \
        }                                                                 \
        ++g_checks;                                                       \
    } while (0)


#if defined(__linux__)
static bool open_descriptor_count(std::size_t *count) {
    DIR *directory = opendir("/proc/self/fd");
    if (!directory) return false;
    std::size_t observed = 0;
    errno = 0;
    while (const dirent *entry = readdir(directory)) {
        if (std::strcmp(entry->d_name, ".") != 0 &&
            std::strcmp(entry->d_name, "..") != 0) ++observed;
    }
    const int read_error = errno;
    const int close_result = closedir(directory);
    if (read_error != 0 || close_result != 0) return false;
    *count = observed;
    return true;
}

static bool no_remaining_linux_children() {
    int status = 0;
    errno = 0;
    const pid_t leftover = waitpid(-1, &status, WNOHANG);
    if (leftover == -1 && errno == ECHILD) return true;
    // A reaped zombie is a test failure, not a repair followed by success.
    std::fprintf(stderr, "FAIL: child remains after cleanup: pid=%ld errno=%d\n",
        static_cast<long>(leftover), errno);
    return false;
}
#endif

static capsid_result configured_spawn(const capsid_worker_config *config,
                                     capsid_worker **worker) {
    const bool carriage = g_configuration_case == ConfigurationCase::V4Carriage;
    const bool channels = g_configuration_case == ConfigurationCase::V4Channels ||
        g_configuration_case == ConfigurationCase::SnapshotV4Channels;
    if (!carriage && !channels) return capsid_worker_spawn(config, worker);
    capsid_protocol_options options;
    const capsid_result initialized =
        capsid_protocol_options_init(&options, sizeof(options));
    if (initialized != CAPSID_OK) return initialized;
    options.wire_version = 4;
    if (channels) {
        options.capabilities =
            CAPSID_PROTOCOL_CAP_DUPLEX_CHANNEL | CAPSID_PROTOCOL_CAP_UPGRADE |
            CAPSID_PROTOCOL_CAP_HALF_CLOSE | CAPSID_PROTOCOL_CAP_CHANNEL_LAYOUT_A2 |
            CAPSID_PROTOCOL_CAP_CHANNEL_CONFIRM | CAPSID_PROTOCOL_CAP_CHANNEL_CLOSE_FINAL |
            CAPSID_PROTOCOL_CAP_FETCH_UPGRADE | CAPSID_PROTOCOL_CAP_CONNECTION_LIFETIME;
    }
    return capsid_worker_spawn_ex(config, &options, worker);
}

static capsid_worker_config default_config(const char *worker_path) {
    capsid_worker_config config;
    capsid_worker_config_init(&config);
    config.worker_path = worker_path;
    static const capsid_egress_policy empty_egress = [] {
        capsid_egress_policy policy;
        capsid_egress_policy_init(&policy);
        return policy;
    }();
    static const std::array<capsid_egress_rule, 2> egress_rules = [] {
        std::array<capsid_egress_rule, 2> rules{};
        for (auto &rule : rules) capsid_egress_rule_init(&rule);
        rules[0].action = CAPSID_EGRESS_DENY;
        rules[0].target = "api.example.test";
        rules[0].port_start = rules[0].port_end = 443;
        rules[0].rule_id = 73;
        rules[1].action = CAPSID_EGRESS_ALLOW;
        rules[1].target = "mirror.example.test";
        rules[1].port_start = rules[1].port_end = 8443;
        rules[1].rule_id = 74;
        return rules;
    }();
    static const capsid_egress_policy nonempty_egress = [] {
        capsid_egress_policy policy;
        capsid_egress_policy_init(&policy);
        policy.rules = egress_rules.data();
        policy.rule_count = static_cast<uint32_t>(egress_rules.size());
        return policy;
    }();
    static const capsid_capability_policy empty_capability = [] {
        capsid_capability_policy policy;
        capsid_capability_policy_init(&policy);
        policy.application_identity = "abi-guard";
        return policy;
    }();
    static const capsid_permission_rule permission_rule = [] {
        capsid_permission_rule rule;
        capsid_permission_rule_init(&rule);
        rule.action = CAPSID_PERMISSION_DENY;
        rule.permission = CAPSID_PERMISSION_ENV;
        rule.resource = "CAPSID_ABI_*";
        rule.rule_id = 23;
        return rule;
    }();
    static const capsid_capability_policy single_rule_capability = [] {
        capsid_capability_policy policy = empty_capability;
        policy.rules = &permission_rule;
        policy.rule_count = 1;
        return policy;
    }();
    static const char *const modules[] = {"capsid:env", "capsid:system"};
    static const std::array<capsid_permission_rule, 2> snapshot_rules = [] {
        std::array<capsid_permission_rule, 2> rules{};
        for (auto &rule : rules) capsid_permission_rule_init(&rule);
        rules[0].action = CAPSID_PERMISSION_ALLOW;
        rules[0].permission = CAPSID_PERMISSION_ENV;
        rules[0].resource = "CAPSID_ABI_*";
        rules[0].rule_id = 23;
        rules[1].action = CAPSID_PERMISSION_DENY;
        rules[1].permission = CAPSID_PERMISSION_SYS;
        rules[1].resource = "loadAvg";
        rules[1].rule_id = 24;
        return rules;
    }();
    static const std::array<capsid_env_entry, 2> snapshot_entries = [] {
        std::array<capsid_env_entry, 2> entries{};
        for (auto &entry : entries) capsid_env_entry_init(&entry);
        entries[0].name = "CAPSID_ABI_ONE";
        entries[0].value = "one";
        entries[1].name = "CAPSID_ABI_TWO";
        entries[1].value = "two";
        return entries;
    }();
    static const capsid_capability_policy snapshot_capability = [] {
        capsid_capability_policy policy = empty_capability;
        policy.allowed_modules = modules;
        policy.allowed_module_count = 2;
        policy.rules = snapshot_rules.data();
        policy.rule_count = static_cast<uint32_t>(snapshot_rules.size());
        policy.env_entries = snapshot_entries.data();
        policy.env_entry_count = static_cast<uint32_t>(snapshot_entries.size());
        return policy;
    }();
    switch (g_configuration_case) {
        case ConfigurationCase::EgressEmpty: config.egress_policy = &empty_egress; break;
        case ConfigurationCase::EgressRules: config.egress_policy = &nonempty_egress; break;
        case ConfigurationCase::CapabilityEmpty: config.capability_policy = &empty_capability; break;
        case ConfigurationCase::PermissionRule: config.capability_policy = &single_rule_capability; break;
        case ConfigurationCase::CapabilitySnapshot:
        case ConfigurationCase::SnapshotV4Channels: config.capability_policy = &snapshot_capability; break;
        default: break;
    }

    return config;
}

static void arm_failure_at(long allocation_index) {
    g_fail_countdown = allocation_index;
    g_inject_runtime_error = false;
}

static void disarm_failures() {
    g_fail_countdown = -1;
    g_inject_runtime_error = false;
}

// Scenario 1 (§10.4): reject the first actual C++ allocation, whether
// platform setup allocates before child creation or the first allocation
// occurs while constructing the worker or its hello frame.
static int scenario_spawn_first_allocation_fails(
    const char *worker_path) {
    disarm_failures();
    CHECK(capsid_last_error() == NULL);
    const capsid_worker_config config = default_config(worker_path);
    arm_failure_at(0);
    capsid_worker *worker = reinterpret_cast<capsid_worker *>(1);
    const capsid_result result = configured_spawn(&config, &worker);
    disarm_failures();
    CHECK(result == CAPSID_OUT_OF_MEMORY);
    CHECK(worker == NULL);
    const char *detail = capsid_last_error();
    CHECK(detail != NULL);
    CHECK(detail[0] != '\0');
    // Repeated destroy of an unused pointer is a no-op.
    capsid_worker_destroy(NULL);
    capsid_worker_destroy(NULL);
    return 0;
}

// Scenario 2 (§10.4): sweep every allocation site of spawn. Each point
// either fails cleanly (OOM, no worker) or succeeds (worker is destroyed).
static int scenario_spawn_allocation_sweep(const char *worker_path) {
    const capsid_worker_config config = default_config(worker_path);
    // Measure the successful path, then fail every observed allocation
    // and require the first allocation beyond that path to succeed.
    disarm_failures();
    g_allocation_count = 0;
    g_count_allocations = true;
    capsid_worker *baseline = NULL;
    const capsid_result baseline_result =
        configured_spawn(&config, &baseline);
    g_count_allocations = false;
    const long allocation_sites = g_allocation_count;
    CHECK(baseline_result == CAPSID_OK);
    CHECK(baseline != NULL);
    capsid_worker_destroy(baseline);
    CHECK(allocation_sites > 0 && allocation_sites <= 512);
#if defined(_WIN32)
    DWORD baseline_handles = 0;
    CHECK(GetProcessHandleCount(GetCurrentProcess(), &baseline_handles));
#elif defined(__linux__)
    std::size_t baseline_descriptors = 0;
    CHECK(open_descriptor_count(&baseline_descriptors));
    CHECK(no_remaining_linux_children());
#endif
    for (long fail_at = 0; fail_at <= allocation_sites; ++fail_at) {
        arm_failure_at(fail_at);
        capsid_worker *worker = reinterpret_cast<capsid_worker *>(1);
        const capsid_result result =
            configured_spawn(&config, &worker);
        disarm_failures();
        if (fail_at < allocation_sites) {
            CHECK(result == CAPSID_OUT_OF_MEMORY);
            CHECK(worker == NULL);
            CHECK(capsid_last_error() != NULL);
        } else {
            CHECK(result == CAPSID_OK);
            CHECK(worker != NULL);
            capsid_worker_destroy(worker);
        }
#if defined(_WIN32)
        DWORD handles = 0;
        CHECK(GetProcessHandleCount(GetCurrentProcess(), &handles));
        if (handles != baseline_handles) {
            std::fprintf(stderr,
                "allocation %ld leaked handles: before=%lu after=%lu\n",
                fail_at, baseline_handles, handles);
        }
        CHECK(handles == baseline_handles);
#elif defined(__linux__)
        std::size_t descriptors = 0;
        CHECK(open_descriptor_count(&descriptors));
        if (descriptors != baseline_descriptors) {
            std::fprintf(stderr,
                "allocation %ld leaked descriptors: before=%zu after=%zu\n",
                fail_at, baseline_descriptors, descriptors);
        }
        CHECK(descriptors == baseline_descriptors);
        CHECK(no_remaining_linux_children());
#endif
    }
    // Exercise the same measured sites with a non-bad_alloc exception.
    // The nothrow operator may swallow only the injected exception at its
    // own allocation; all other sites must reach the ABI internal-error guard.
    for (long fail_at = 0; fail_at < allocation_sites; ++fail_at) {
        arm_failure_at(fail_at);
        g_inject_runtime_error = true;
        g_runtime_error_raised = false;
        g_runtime_error_swallowed = false;
        capsid_worker *worker = reinterpret_cast<capsid_worker *>(1);
        const capsid_result result = configured_spawn(&config, &worker);
        const bool injected = g_runtime_error_raised;
        const bool swallowed = g_runtime_error_swallowed;
        disarm_failures();
        if (!injected || result != (swallowed ? CAPSID_OUT_OF_MEMORY : CAPSID_INTERNAL_ERROR)) {
            std::fprintf(stderr,
                "internal-error site %ld: injected=%d swallowed=%d result=%d\n",
                fail_at, injected, swallowed, static_cast<int>(result));
        }
        CHECK(injected);
        CHECK(result == (swallowed ? CAPSID_OUT_OF_MEMORY : CAPSID_INTERNAL_ERROR));
        CHECK(worker == NULL);
        CHECK(capsid_last_error() != NULL && capsid_last_error()[0] != '\0');
#if defined(_WIN32)
        DWORD handles = 0;
        CHECK(GetProcessHandleCount(GetCurrentProcess(), &handles));
        if (handles != baseline_handles) {
            std::fprintf(stderr,
                "internal-error site %ld leaked handles: before=%lu after=%lu\n",
                fail_at, baseline_handles, handles);
        }
        CHECK(handles == baseline_handles);
#elif defined(__linux__)
        std::size_t descriptors = 0;
        CHECK(open_descriptor_count(&descriptors));
        CHECK(descriptors == baseline_descriptors);
        CHECK(no_remaining_linux_children());
#endif
    }
    std::printf("PASS: internal-error allocation sweep (%ld failure points)\n", allocation_sites);
    std::printf("PASS: spawn allocation sweep (%ld failure points + success)\n",
        allocation_sites);
    return 0;
}

// Scenario 3 (§10.4): non-bad_alloc exceptions become
// CAPSID_INTERNAL_ERROR with detail, never a crash. Injected on the
// request path: spawn's nothrow worker allocation would swallow the
// exception, so the guarded vector allocation of begin_request is the
// clean injection point.
static int scenario_internal_error_injection(const char *worker_path) {
    disarm_failures();
    // Clear any error slot left by a previous scenario through a guarded
    // call (INVALID_ARGUMENT is a return code, not an exception).
    capsid_worker *scratch = NULL;
    configured_spawn(NULL, &scratch);
    CHECK(capsid_last_error() == NULL);
    capsid_worker *worker = NULL;
    if (worker_path) {
        const capsid_worker_config spawn_config =
            default_config(worker_path);
        CHECK(configured_spawn(&spawn_config, &worker) == CAPSID_OK);
        CHECK(worker != NULL);
        g_fail_countdown = 0;
        g_inject_runtime_error = true;
        const capsid_result result = capsid_worker_begin_request(
            worker, 7, "GET", "http://example.test/", NULL, 0);
        disarm_failures();
        CHECK(result == CAPSID_INTERNAL_ERROR);
        CHECK(capsid_last_error() != NULL);
        capsid_worker_destroy(worker);
    } else {
        // An empty executable path guarantees no child can launch. Some
        // Debug libraries allocate metadata before reaching the OS call;
        // require the exact result corresponding to observed injection.
        const capsid_worker_config config = default_config("");
        g_fail_countdown = 0;
        g_inject_runtime_error = true;
        capsid_worker *failed_worker =
            reinterpret_cast<capsid_worker *>(1);
        const capsid_result result =
            configured_spawn(&config, &failed_worker);
        const bool injected = g_fail_countdown < 0;
        disarm_failures();
        CHECK(failed_worker == NULL);
        if (injected) {
            CHECK(result == CAPSID_INTERNAL_ERROR);
            CHECK(capsid_last_error() != NULL);
        } else {
            CHECK(result == CAPSID_CHILD_ERROR);
            CHECK(capsid_last_error() == NULL);
        }
    }
    return 0;
}

// Scenario 4 (§10.4): request-path entry points fail cleanly on OOM and
// destroy still reaps the child afterwards.
static int scenario_request_path_oom(const char *worker_path) {
    disarm_failures();
    const capsid_worker_config config = default_config(worker_path);
    capsid_worker *worker = NULL;
    CHECK(configured_spawn(&config, &worker) == CAPSID_OK);
    CHECK(worker != NULL);
    CHECK(capsid_last_error() == NULL);

    // begin_request: frame encode + payload allocation.
    arm_failure_at(0);
    const capsid_result begin_result = capsid_worker_begin_request(
        worker, 1, "GET", "http://example.test/", NULL, 0);
    disarm_failures();
    CHECK(begin_result == CAPSID_OUT_OF_MEMORY);
    CHECK(capsid_last_error() != NULL);

    // begin_bodyless_request: same guard.
    arm_failure_at(0);
    const capsid_result bodyless_result =
        capsid_worker_begin_bodyless_request(
            worker, 2, "GET", "http://example.test/", NULL, 0);
    disarm_failures();
    CHECK(bodyless_result == CAPSID_OUT_OF_MEMORY);
    CHECK(capsid_last_error() != NULL);

    // write_request: before the worker grants request-direction credit it
    // returns WOULD_BLOCK without allocating; the chunked allocation path
    // (same queue_chunked) is exercised by the load_bundle case below.
    CHECK(capsid_worker_begin_request(
              worker, 3, "POST", "http://example.test/",
              NULL, 0) == CAPSID_OK);
    CHECK(capsid_last_error() == NULL);
    const uint8_t body[] = "hello";
    CHECK(capsid_worker_write_request(
              worker, 3, body, sizeof(body) - 1) ==
          CAPSID_WOULD_BLOCK);
    CHECK(capsid_last_error() == NULL);

    // end_request / grant_response_credit / cancel /
    // request_memory_metrics: queue_frame allocation.
    arm_failure_at(0);
    const capsid_result end_result =
        capsid_worker_end_request(worker, 3);
    disarm_failures();
    CHECK(end_result == CAPSID_OUT_OF_MEMORY);
    CHECK(capsid_last_error() != NULL);

    arm_failure_at(0);
    const capsid_result credit_result =
        capsid_worker_grant_response_credit(worker, 3, 4096);
    disarm_failures();
    CHECK(credit_result == CAPSID_OUT_OF_MEMORY);
    CHECK(capsid_last_error() != NULL);

    arm_failure_at(0);
    const capsid_result cancel_result =
        capsid_worker_cancel(worker, 3);
    disarm_failures();
    CHECK(cancel_result == CAPSID_OUT_OF_MEMORY);
    CHECK(capsid_last_error() != NULL);

    arm_failure_at(0);
    const capsid_result metrics_result =
        capsid_worker_request_memory_metrics(worker);
    disarm_failures();
    CHECK(metrics_result == CAPSID_OUT_OF_MEMORY);
    CHECK(capsid_last_error() != NULL);

    // load_bundle: chunked frame batch allocation.
    arm_failure_at(0);
    const capsid_result bundle_result = capsid_worker_load_bundle(
        worker, body, sizeof(body) - 1);
    disarm_failures();
    CHECK(bundle_result == CAPSID_OUT_OF_MEMORY);
    CHECK(capsid_last_error() != NULL);

    // next_event: parser may allocate while draining worker output; the
    // guard must contain that too.
    arm_failure_at(0);
    capsid_event event;
    std::memset(&event, 0, sizeof(event));
    event.struct_size = sizeof(event);
    const capsid_result event_result =
        capsid_worker_next_event(worker, &event);
    disarm_failures();
    CHECK(event_result == CAPSID_OUT_OF_MEMORY ||
          event_result == CAPSID_WOULD_BLOCK);
    if (event_result == CAPSID_OUT_OF_MEMORY) {
        CHECK(capsid_last_error() != NULL);
    }

    // destroy after failures: must reap the child, not leak it.
    capsid_worker_destroy(worker);
    return 0;
}

// Scenario 5 (§10.4): shutdown OOM still reaps the child inside destroy.
static int scenario_destroy_reaps_after_shutdown_oom(
    const char *worker_path) {
    disarm_failures();
    const capsid_worker_config config = default_config(worker_path);
    capsid_worker *worker = NULL;
    CHECK(configured_spawn(&config, &worker) == CAPSID_OK);
    CHECK(worker != NULL);
    // Everything inside destroy's graceful path fails to allocate; the
    // unconditional close/TERM/KILL/reap must still happen.
    arm_failure_at(0);
    capsid_worker_destroy(worker);
    disarm_failures();
    return 0;
}

#if defined(__linux__)
// Scenario 6 (Linux, §10.4): after spawn failures and destroys there are
// no zombie children left for the test process.
static int scenario_no_zombie_children(const char *worker_path) {
    disarm_failures();
    const capsid_worker_config config = default_config(worker_path ? worker_path : "");
    for (int round = 0; round < 8; ++round) {
        arm_failure_at(static_cast<long>(round));
        capsid_worker *worker = reinterpret_cast<capsid_worker *>(1);
        const capsid_result result =
            configured_spawn(&config, &worker);
        disarm_failures();
        if (result == CAPSID_OUT_OF_MEMORY) {
            CHECK(worker == NULL);
        } else if (worker_path) {
            CHECK(result == CAPSID_OK);
            CHECK(worker != NULL);
            capsid_worker_destroy(worker);
        } else {
            CHECK(result == CAPSID_CHILD_ERROR);
            CHECK(worker == NULL);
        }
    }
    capsid_worker_destroy(NULL);
    CHECK(no_remaining_linux_children());
    return 0;
}
#endif

// Scenario 7: capsid_result_string covers the new enumerators and unknown
// values; capsid_last_error semantics on success paths.
static int scenario_result_string_and_error_semantics(
    const char *worker_path) {
    disarm_failures();
    CHECK(std::strcmp(capsid_result_string(CAPSID_OUT_OF_MEMORY),
                      "out of memory") == 0);
    CHECK(std::strcmp(capsid_result_string(CAPSID_INTERNAL_ERROR),
                      "internal error") == 0);
    CHECK(std::strcmp(capsid_result_string(
                          static_cast<capsid_result>(99)),
                      "unknown result") == 0);
    CHECK(capsid_last_error() == NULL);

    // A successful guarded call clears the error slot.
    if (worker_path) {
        const capsid_worker_config config =
            default_config(worker_path);
        capsid_worker *worker = NULL;
        CHECK(configured_spawn(&config, &worker) == CAPSID_OK);
        CHECK(capsid_last_error() == NULL);
        capsid_worker_destroy(worker);
        // destroy is not guarded: the slot keeps whatever the last
        // guarded call left (NULL here).
        CHECK(capsid_last_error() == NULL);
    }

    // An invalid-argument failure is a return code, not an exception; the
    // guard clears the slot, so last_error stays NULL.
    capsid_worker *null_worker = reinterpret_cast<capsid_worker *>(1);
    CHECK(configured_spawn(NULL, &null_worker) ==
          CAPSID_INVALID_ARGUMENT);
    CHECK(capsid_last_error() == NULL);
    return 0;
}


static int scenario_nothrow_injector_controls() {
    arm_failure_at(0);
    void *memory = ::operator new(8, std::nothrow);
    disarm_failures();
    const bool rejected_single = memory == NULL;
    ::operator delete(memory, std::nothrow);
    CHECK(rejected_single);
    arm_failure_at(0);
    memory = ::operator new[](8, std::nothrow);
    disarm_failures();
    const bool rejected_array = memory == NULL;
    ::operator delete[](memory, std::nothrow);
    CHECK(rejected_array);
    return 0;
}

#if defined(_WIN32)
static HANDLE g_child_lifetime_job = nullptr;

static bool own_child_lifetime() {
    // Keep this handle until process exit so an assertion or terminate
    // failure also closes the private test's complete worker tree.
    const HANDLE job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!job) return false;
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation,
            &limits, sizeof(limits)) ||
        !AssignProcessToJobObject(job, GetCurrentProcess())) {
        CloseHandle(job);
        return false;
    }
    g_child_lifetime_job = job;
    return true;
}

static bool owned_process_count(DWORD *count) {
    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION information = {};
    if (!g_child_lifetime_job || !QueryInformationJobObject(g_child_lifetime_job,
            JobObjectBasicAccountingInformation, &information, sizeof(information), nullptr)) return false;
    *count = information.ActiveProcesses;
    return true;
}

static bool no_remaining_child_processes(bool report = true) {
    // A system snapshot's parent PID can refer to an unrelated older process
    // after PID reuse. The private job tracks this test's complete child tree.
    DWORD count = 0;
    if (!owned_process_count(&count)) return false;
    if (count != 1 && report) std::fprintf(stderr,
        "private test job still contains %lu processes (expected only the test)\n", count);
    return count == 1;
}

static bool wait_for_child_accounting(DWORD timeout_ms, bool report = true) {
    // Process exit has its own handle wait. Job accounting can publish the
    // resulting removal later; keep its bounded observation separate.
    const ULONGLONG started = GetTickCount64();
    for (;;) {
        DWORD count = 0;
        if (!owned_process_count(&count)) return false;
        if (count == 1) return true;
        if (GetTickCount64() - started >= timeout_ms) {
            if (report) std::fprintf(stderr,
                "private test job still contains %lu processes after %lu ms\n", count, timeout_ms);
            return false;
        }
        Sleep(1);
    }
}

static int scenario_child_lifetime_controls() {
    CHECK(no_remaining_child_processes());
    wchar_t executable[32768] = {};
    const DWORD length = GetModuleFileNameW(nullptr, executable, 32768);
    CHECK(length > 0 && length < 32768);
    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION child = {};
    // Never run the child's main(): this real, suspended child exists only
    // to prove that the cleanup check rejects an actual owned live process.
    CHECK(CreateProcessW(executable, nullptr, nullptr, nullptr, FALSE,
        CREATE_SUSPENDED, nullptr, nullptr, &startup, &child));
    BOOL member = FALSE;
    const BOOL membership_checked = IsProcessInJob(child.hProcess, g_child_lifetime_job, &member);
    DWORD count = 0;
    const bool counted = owned_process_count(&count);
    const bool rejected = !no_remaining_child_processes(false);
    const bool bounded_rejected = !wait_for_child_accounting(20, false);
    const BOOL terminated = TerminateProcess(child.hProcess, 0);
    const DWORD waited = terminated ? WaitForSingleObject(child.hProcess, 5000) : WAIT_FAILED;
    DWORD exit_code = STILL_ACTIVE;
    const BOOL exit_checked = waited == WAIT_OBJECT_0 && GetExitCodeProcess(child.hProcess, &exit_code);
    CloseHandle(child.hThread);
    CloseHandle(child.hProcess);
    CHECK(membership_checked && member);
    CHECK(counted && count == 2);
    CHECK(rejected);
    CHECK(bounded_rejected);
    CHECK(terminated && waited == WAIT_OBJECT_0);
    CHECK(exit_checked && exit_code == 0);
    CHECK(wait_for_child_accounting(5000));
    CHECK(no_remaining_child_processes());
    std::printf("PASS: private-job child ownership controls (live child rejected; exited child released)\n");
    return 0;
}
#endif

int main(int argc, char **argv) {
    struct Mode {
        const char *name;
        ConfigurationCase configuration;
    };
    static const Mode modes[] = {
        {"default", ConfigurationCase::Default},
        {"egress-empty", ConfigurationCase::EgressEmpty},
        {"egress-rules", ConfigurationCase::EgressRules},
        {"capability-empty", ConfigurationCase::CapabilityEmpty},
        {"permission-rule", ConfigurationCase::PermissionRule},
        {"capability-snapshot", ConfigurationCase::CapabilitySnapshot},
        {"v4-carriage", ConfigurationCase::V4Carriage},
        {"v4-channels", ConfigurationCase::V4Channels},
        {"snapshot-v4-channels", ConfigurationCase::SnapshotV4Channels}
    };
    if (argc > 3) {
        std::fprintf(stderr, "FAIL: expected [worker_path [configuration]]\n");
        return 1;
    }
    const char *selected = argc > 2 ? argv[2] : "default";
    bool known = false;
    for (const auto &mode : modes) {
        if (std::strcmp(selected, mode.name) != 0) continue;
        g_configuration_case = mode.configuration;
        known = true;
        break;
    }
    if (!known) {
        std::fprintf(stderr, "FAIL: unknown allocation configuration: %s\n", selected);
        return 1;
    }
    std::printf("ABI allocation configuration: %s\n", selected);

    std::set_terminate([] {
        std::fprintf(stderr,
            "FAIL: allocation injection terminated instead of returning an ABI error (countdown=%ld)\n",
            g_fail_countdown);
        std::fflush(stderr);
        std::_Exit(1);
    });
#if defined(_WIN32)
    CHECK(own_child_lifetime());
#endif

    const char *worker_path =
        argc > 1 ? argv[1] : NULL;
    int failed = 0;
#if defined(_WIN32)
    failed += scenario_child_lifetime_controls();
#endif
    failed += scenario_nothrow_injector_controls();
    failed += scenario_result_string_and_error_semantics(worker_path);
    if (worker_path) {
        // The complete spawn allocation path includes the real child,
        // worker construction and hello frame. Without an explicit worker
        // path only the controlled missing-executable cases run.
        failed += scenario_spawn_first_allocation_fails(worker_path);
        failed += scenario_internal_error_injection(worker_path);
        failed += scenario_spawn_allocation_sweep(worker_path);
        failed += scenario_request_path_oom(worker_path);
        failed += scenario_destroy_reaps_after_shutdown_oom(worker_path);
    } else {
        failed += scenario_internal_error_injection(NULL);
    }
#if defined(__linux__)
    failed += scenario_no_zombie_children(worker_path);
    CHECK(no_remaining_linux_children());
#endif
#if defined(_WIN32)
    CHECK(no_remaining_child_processes());
#endif
    if (failed != 0) {
        std::fprintf(stderr, "FAILED: %d scenario(s)\n", failed);
        return 1;
    }
    std::printf("PASS: abi_guard (%ld checks)\n", g_checks);
    return 0;
}
