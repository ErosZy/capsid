// WP-06, spec §10.4: no C++ exception may cross an extern "C" boundary.
//
// A controllable global operator new failure countdown exercises every
// allocation site of the guarded entry points (spawn, policy copy, frame
// encode, request begin, load bundle, ...). Each failure point must yield
// CAPSID_OUT_OF_MEMORY (or CAPSID_INTERNAL_ERROR for injected
// non-bad_alloc failures) with a non-empty capsid_last_error(), never a
// crash, never a leaked child.
//
// Usage: test-abi-guard [worker_path]   (worker_path enables the
// success-path scenarios; the failure-injection scenarios run without it)
#include "capsid/runtime.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <stdexcept>
#include <string>

#if defined(__linux__)
#include <errno.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

// Global allocation-failure countdown. g_fail_countdown >= 0 arms it:
// the allocation at which it reaches 0 fails (with bad_alloc, or with a
// runtime_error when g_inject_runtime_error is set).
long g_fail_countdown = -1;
bool g_inject_runtime_error = false;

}  // namespace

void *operator new(std::size_t size) {
    if (g_fail_countdown >= 0) {
        if (g_fail_countdown == 0) {
            if (g_inject_runtime_error) {
                // Disarm before constructing: the exception object itself
                // allocates, which would recurse into this allocator.
                g_fail_countdown = -1;
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

void *operator new[](std::size_t size) {
    return ::operator new(size);
}

void *operator new(std::size_t size, const std::nothrow_t &) noexcept {
    // Own the nothrow variants too: without these definitions the sanitizer
    // runtime provides them, and the later delete → std::free would trip
    // ASan's alloc-dealloc-mismatch (nothrow-new memory freed as malloc).
    return std::malloc(size ? size : 1);
}

void *operator new[](std::size_t size, const std::nothrow_t &) noexcept {
    return std::malloc(size ? size : 1);
}

void operator delete(void *memory) noexcept {
    std::free(memory);
}

void operator delete[](void *memory) noexcept {
    std::free(memory);
}

void operator delete(void *memory, std::size_t) noexcept {
    std::free(memory);
}

void operator delete[](void *memory, std::size_t) noexcept {
    std::free(memory);
}

void operator delete(void *memory, const std::nothrow_t &) noexcept {
    std::free(memory);
}

void operator delete[](void *memory, const std::nothrow_t &) noexcept {
    std::free(memory);
}

void operator delete(void *memory, std::size_t,
                     const std::nothrow_t &) noexcept {
    std::free(memory);
}

void operator delete[](void *memory, std::size_t,
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

static capsid_worker_config default_config(const char *worker_path) {
    capsid_worker_config config;
    capsid_worker_config_init(&config);
    config.worker_path = worker_path;
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

// Scenario 1 (§10.4): spawn fails on its very first allocation. The
// pre-fork portion of spawn allocates nothing (all C calls), so the first
// failure point is the worker object or the hello frame — both require a
// real worker binary.
static int scenario_spawn_first_allocation_fails(
    const char *worker_path) {
    disarm_failures();
    CHECK(capsid_last_error() == NULL);
    const capsid_worker_config config = default_config(worker_path);
    arm_failure_at(0);
    capsid_worker *worker = reinterpret_cast<capsid_worker *>(1);
    const capsid_result result = capsid_worker_spawn(&config, &worker);
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
    for (long fail_at = 0; fail_at < 64; ++fail_at) {
        arm_failure_at(fail_at);
        capsid_worker *worker = reinterpret_cast<capsid_worker *>(1);
        const capsid_result result =
            capsid_worker_spawn(&config, &worker);
        disarm_failures();
        if (result == CAPSID_OUT_OF_MEMORY) {
            CHECK(worker == NULL);
            CHECK(capsid_last_error() != NULL);
        } else {
            CHECK(result == CAPSID_OK);
            CHECK(worker != NULL);
            capsid_worker_destroy(worker);
        }
    }
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
    capsid_worker_spawn(NULL, &scratch);
    CHECK(capsid_last_error() == NULL);
    capsid_worker *worker = NULL;
    if (worker_path) {
        const capsid_worker_config spawn_config =
            default_config(worker_path);
        CHECK(capsid_worker_spawn(&spawn_config, &worker) == CAPSID_OK);
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
        // Without a worker binary the pre-fork portion of spawn has no
        // allocation sites (all C calls), so the injected failure never
        // fires and posix_spawn fails: the boundary must still be safe —
        // a return code, no exception, no error slot.
        const capsid_worker_config config = default_config(NULL);
        g_fail_countdown = 0;
        g_inject_runtime_error = true;
        capsid_worker *failed_worker =
            reinterpret_cast<capsid_worker *>(1);
        const capsid_result result =
            capsid_worker_spawn(&config, &failed_worker);
        disarm_failures();
        CHECK(result == CAPSID_CHILD_ERROR);
        CHECK(capsid_last_error() == NULL);
    }
    return 0;
}

// Scenario 4 (§10.4): request-path entry points fail cleanly on OOM and
// destroy still reaps the child afterwards.
static int scenario_request_path_oom(const char *worker_path) {
    disarm_failures();
    const capsid_worker_config config = default_config(worker_path);
    capsid_worker *worker = NULL;
    CHECK(capsid_worker_spawn(&config, &worker) == CAPSID_OK);
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
    CHECK(capsid_worker_spawn(&config, &worker) == CAPSID_OK);
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
static int scenario_no_zombie_children() {
    disarm_failures();
    const capsid_worker_config config = default_config(NULL);
    for (int round = 0; round < 8; ++round) {
        arm_failure_at(static_cast<long>(round));
        capsid_worker *worker = reinterpret_cast<capsid_worker *>(1);
        const capsid_result result =
            capsid_worker_spawn(&config, &worker);
        disarm_failures();
        if (result != CAPSID_OUT_OF_MEMORY) {
            CHECK(result == CAPSID_OK);
            capsid_worker_destroy(worker);
        }
        CHECK(worker == NULL || result == CAPSID_OK);
    }
    capsid_worker_destroy(NULL);
    // Wait for any SIGCHLD to be handled; then no child may remain.
    for (int attempt = 0; attempt < 40; ++attempt) {
        int status = 0;
        const pid_t leftover = waitpid(-1, &status, WNOHANG);
        if (leftover == -1 && errno == ECHILD) {
            return 0;
        }
        usleep(5000);
    }
    CHECK(false && "zombie children remain after failure injection");
    return 1;
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
        CHECK(capsid_worker_spawn(&config, &worker) == CAPSID_OK);
        CHECK(capsid_last_error() == NULL);
        capsid_worker_destroy(worker);
        // destroy is not guarded: the slot keeps whatever the last
        // guarded call left (NULL here).
        CHECK(capsid_last_error() == NULL);
    }

    // An invalid-argument failure is a return code, not an exception; the
    // guard clears the slot, so last_error stays NULL.
    capsid_worker *null_worker = reinterpret_cast<capsid_worker *>(1);
    CHECK(capsid_worker_spawn(NULL, &null_worker) ==
          CAPSID_INVALID_ARGUMENT);
    CHECK(capsid_last_error() == NULL);
    return 0;
}

int main(int argc, char **argv) {
    const char *worker_path =
        argc > 1 ? argv[1] : NULL;
    int failed = 0;
    failed += scenario_result_string_and_error_semantics(worker_path);
    if (worker_path) {
        // Spawn OOM scenarios need a real worker binary: the pre-fork
        // portion of spawn allocates nothing, and the first failure points
        // (worker object, hello frame) only exist once the child is up.
        failed += scenario_spawn_first_allocation_fails(worker_path);
        failed += scenario_internal_error_injection(worker_path);
        failed += scenario_spawn_allocation_sweep(worker_path);
        failed += scenario_request_path_oom(worker_path);
        failed += scenario_destroy_reaps_after_shutdown_oom(worker_path);
    } else {
        failed += scenario_internal_error_injection(NULL);
    }
#if defined(__linux__)
    failed += scenario_no_zombie_children();
#endif
    if (failed != 0) {
        std::fprintf(stderr, "FAILED: %d scenario(s)\n", failed);
        return 1;
    }
    std::printf("PASS: abi_guard (%ld checks)\n", g_checks);
    return 0;
}
