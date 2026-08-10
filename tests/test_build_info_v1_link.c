/*
 * WP-07, spec §11.4: a caller compiled against the build-info v1 headers
 * (tests/fixtures/runtime_build_info_v1.h, a frozen snapshot) links
 * against the CURRENT library and negotiates with its smaller
 * struct_size. The library must accept the v1 envelope, fill exactly the
 * leading v1 fields, and never touch bytes beyond the caller's buffer.
 *
 * Usage: test-build-info-v1-link
 */
#include "fixtures/runtime_build_info_v1.h"

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

static int is_lower_hex(const char *value, size_t size) {
    if (value == NULL || strlen(value) != size) {
        return 0;
    }
    for (size_t i = 0; i < size; ++i) {
        const char c = value[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return 0;
        }
    }
    return 1;
}

int main(void) {
    /* Envelope rules the v1 caller knows: NULL and zeroed outputs are
     * rejected, the initializer stamps the v1 size. */
    CHECK(capsid_runtime_build_info(NULL) == CAPSID_INVALID_ARGUMENT);
    capsid_build_info zeroed = {0};
    CHECK(capsid_runtime_build_info(&zeroed) == CAPSID_INVALID_ARGUMENT);

    capsid_build_info info;
    capsid_build_info_init(&info);
    CHECK(info.struct_size == sizeof(info));
    /* The library's legacy initializer stamps a v1-sized envelope (it
     * cannot know this caller's buffer is smaller than the current
     * layout) with the CURRENT version: a v1 caller must treat versions
     * above its own conservatively. */
    CHECK(info.version == 2u); /* current CAPSID_BUILD_INFO_VERSION */

    /* The v1 caller succeeds against the new library: its struct_size is
     * the negotiated size actually used, and the library stamps its own
     * (newer) version so the v1 caller knows it must be conservative. */
    CHECK(capsid_runtime_build_info(&info) == CAPSID_OK);
    CHECK(info.struct_size == sizeof(info)); /* unchanged: only v1 fields fit */
    CHECK(info.version == 2u); /* current CAPSID_BUILD_INFO_VERSION */

    /* Every v1 field is filled. */
    CHECK(info.runtime_version != NULL && info.runtime_version[0] != '\0');
    CHECK(info.abi_version == 7u); /* CAPSID_ABI_VERSION, stable in v1 */
    CHECK(info.fetchrpc_version != 0);
    CHECK(is_lower_hex(info.quickjs_commit, 40));
    CHECK(is_lower_hex(info.txiki_overlay_key, 64));
    CHECK(is_lower_hex(info.txiki_overlay_manifest, 64));
    CHECK(info.bytecode_compile_flags != NULL &&
          info.bytecode_compile_flags[0] != '\0');
    CHECK(info.target_architecture != NULL &&
          info.target_architecture[0] != '\0');
    CHECK((strcmp(info.endianness, "little") == 0 ||
           strcmp(info.endianness, "big") == 0) &&
          info.pointer_width_bits == sizeof(void *) * 8);
    CHECK(info.bytecode_format_identity != NULL &&
          info.bytecode_format_identity[0] != '\0');
    CHECK(is_lower_hex(info.capability_manifest_sha256, 64));
    CHECK(info.compatibility_id != NULL &&
          strncmp(info.compatibility_id, "sha256:", 7) == 0 &&
          is_lower_hex(info.compatibility_id + 7, 64));

    /* Canary: the library must not touch memory beyond the v1 buffer even
     * though the current (v2) layout is larger. A library that demanded
     * struct_size >= sizeof(current struct) would have failed above; one
     * that wrote past the caller's buffer corrupts this canary. */
    {
        unsigned char raw[sizeof(capsid_build_info) + 64];
        memset(raw, 0, sizeof(raw));
        capsid_build_info *probe = (capsid_build_info *)raw;
        capsid_build_info_init(probe);
        memset(raw + sizeof(capsid_build_info), 0xAB, 64);
        CHECK(capsid_runtime_build_info(probe) == CAPSID_OK);
        CHECK(probe->version == 2u);
        CHECK(probe->compatibility_id != NULL);
        for (size_t i = 0; i < 64; ++i) {
            CHECK(raw[sizeof(capsid_build_info) + i] == 0xAB);
        }
    }

    printf("PASS: build_info_v1_link (%d checks)\n", g_checks);
    return 0;
}
