/* Original ABI v7 consumer (v0.3.0 remediation T01).
 *
 * Compiled against the frozen tests/compat/runtime_v7.h snapshot as
 * restored from the v0.2.2 release commit: the published struct layout
 * must stay byte-identical, a sentinel-framed config must initialize
 * without overrun, and the legacy spawn entry stays v3. The compile-time
 * size assertions below fail the build if anyone grows the frozen
 * layout again. */
#include "runtime_v7.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Frozen v0.2.2 capsid_worker_config size and alignment, recorded from
 * the release commit. Growing the struct breaks every consumer that
 * allocates it by value; protocol selection must use the separate
 * options descriptor instead. */
#define CAPSID_V7_CONFIG_FROZEN_SIZE 144u
#define CAPSID_V7_CONFIG_FROZEN_ALIGN 8u

typedef struct guarded_config {
    uint64_t before;
    capsid_worker_config config;
    uint64_t after;
} guarded_config;

_Static_assert(sizeof(capsid_worker_config) ==
                   CAPSID_V7_CONFIG_FROZEN_SIZE,
               "capsid_worker_config grew: ABI v7 layout is frozen");
_Static_assert(_Alignof(capsid_worker_config) ==
                   CAPSID_V7_CONFIG_FROZEN_ALIGN,
               "capsid_worker_config alignment changed");

int main(int argc, char **argv) {
    static const uint64_t before = UINT64_C(0x1122334455667788);
    static const uint64_t after = UINT64_C(0x8877665544332211);
    guarded_config guarded;
    memset(&guarded, 0xa5, sizeof(guarded));
    guarded.before = before;
    guarded.after = after;

    capsid_worker_config_init(&guarded.config);
    if (guarded.before != before || guarded.after != after) {
        fprintf(stderr, "FAIL: init overran the frozen struct\n");
        return 1;
    }
    if (guarded.config.struct_size != sizeof(capsid_worker_config) ||
        guarded.config.abi_version != CAPSID_ABI_VERSION ||
        guarded.config.sandbox_network_namespace_fd != -1 ||
        guarded.config.egress_reserved != 0) {
        fprintf(stderr, "FAIL: init defaults changed\n");
        return 1;
    }

    if (argc > 1) {
        guarded.config.worker_path = argv[1];
        /* Null out_worker exercises argument validation without
         * spawning; the frozen entry must reject it exactly as
         * before. */
        if (capsid_worker_spawn(&guarded.config, NULL) !=
                CAPSID_INVALID_ARGUMENT ||
            capsid_worker_spawn(NULL, NULL) != CAPSID_INVALID_ARGUMENT) {
            fprintf(stderr, "FAIL: spawn argument contract changed\n");
            return 1;
        }
        if (guarded.before != before || guarded.after != after) {
            fprintf(stderr, "FAIL: rejected spawn touched memory\n");
            return 1;
        }
    }

    printf("abi v7 original consumer: PASS\n");
    return 0;
}
