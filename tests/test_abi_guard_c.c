/*
 * WP-06, spec §10.4: a pure C caller compiles and links against the
 * Runtime without any C++ exception crossing the boundary. Exercises the
 * new ABI surface (CAPSID_OUT_OF_MEMORY / CAPSID_INTERNAL_ERROR /
 * capsid_last_error) plus a complete spawn→request→destroy cycle.
 *
 * Usage: test-abi-guard-c [worker_path]
 * (without a worker path the failure-semantics checks still run)
 */
#include "capsid/runtime.h"

#include <stdio.h>
#include <string.h>

static int g_checks = 0;

#define CHECK(condition)                                          \
    do {                                                          \
        if (!(condition)) {                                       \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__,         \
                    __LINE__, #condition);                        \
            return 1;                                             \
        }                                                         \
        ++g_checks;                                               \
    } while (0)

int main(int argc, char **argv) {
    const char *worker_path = argc > 1 ? argv[1] : NULL;

    /* Result strings cover the WP-06 enumerators and unknown values. */
    CHECK(capsid_result_string(CAPSID_OK) != NULL);
    CHECK(strcmp(capsid_result_string(CAPSID_OUT_OF_MEMORY),
                 "out of memory") == 0);
    CHECK(strcmp(capsid_result_string(CAPSID_INTERNAL_ERROR),
                 "internal error") == 0);
    CHECK(strcmp(capsid_result_string((capsid_result)99),
                 "unknown result") == 0);

    /* Fresh thread: no error recorded. */
    CHECK(capsid_last_error() == NULL);

    /* Invalid argument is a return code; the guard clears the error slot. */
    {
        capsid_worker *null_worker = (capsid_worker *)1;
        CHECK(capsid_worker_spawn(NULL, &null_worker) ==
              CAPSID_INVALID_ARGUMENT);
        CHECK(capsid_last_error() == NULL);
    }

    /* A complete spawn→request→destroy cycle from C, no exceptions. */
    if (worker_path != NULL) {
        capsid_worker_config config;
        capsid_worker_config_init(&config);
        config.worker_path = worker_path;
        capsid_worker *worker = NULL;
        CHECK(capsid_worker_spawn(&config, &worker) == CAPSID_OK);
        CHECK(worker != NULL);
        CHECK(capsid_worker_pid(worker) > 0);
        CHECK(capsid_worker_fd(worker) >= 0);
        CHECK(capsid_last_error() == NULL);

        CHECK(capsid_worker_begin_request(
                  worker, 1, "GET", "http://example.test/",
                  NULL, 0) == CAPSID_OK);
        CHECK(capsid_worker_end_request(worker, 1) == CAPSID_OK);
        CHECK(capsid_last_error() == NULL);

        /* Availability helpers are callable from C with sane values.
         * CPU topology is only reported on Linux; other platforms
         * legitimately return 0. */
#if defined(__linux__)
        CHECK(capsid_recommended_worker_count() >= 1);
        CHECK(capsid_available_cpu_count() >= 1);
        CHECK(capsid_available_cpu_at(0, &(uint32_t){0}) == CAPSID_OK);
#endif

        capsid_worker_destroy(worker);
        CHECK(capsid_last_error() == NULL);
    }

    /* Repeated destroy of a null handle is a no-op. */
    capsid_worker_destroy(NULL);
    capsid_worker_destroy(NULL);

    printf("PASS: abi_guard_c (%d checks)\n", g_checks);
    return 0;
}
