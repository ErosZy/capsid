#include "runtime_v7.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef struct guarded_config {
    uint64_t before;
    capsid_worker_config config;
    uint64_t after;
} guarded_config;

int main(void) {
    static const uint64_t before = UINT64_C(0x1122334455667788);
    static const uint64_t after = UINT64_C(0x8877665544332211);
    guarded_config guarded;
    memset(&guarded, 0xa5, sizeof(guarded));
    guarded.before = before;
    guarded.after = after;

    capsid_worker_config_init(&guarded.config);
    if (guarded.before != before ||
        guarded.after != after ||
        guarded.config.struct_size != sizeof(capsid_worker_config) ||
        guarded.config.abi_version != CAPSID_ABI_VERSION ||
        guarded.config.sandbox_network_namespace_fd != -1 ||
        guarded.config.egress_reserved != 0) {
        return 1;
    }
    if (capsid_worker_spawn(&guarded.config, NULL) !=
            CAPSID_INVALID_ARGUMENT ||
        capsid_worker_spawn(NULL, NULL) != CAPSID_INVALID_ARGUMENT ||
        capsid_result_string(CAPSID_OK) == NULL ||
        capsid_recommended_worker_count() == 0) {
        return 2;
    }
    return 0;
}
