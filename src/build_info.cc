// Library-side build identity and provenance.
//
// Every value, including the compatibility ID and the build ID, comes from
// the generated build_identity.h (cmake/ComputeBuildIdentity.cmake); this
// translation unit never hashes or formats the canonical records itself, so
// the library, the worker and the bytecode compiler cannot drift apart.
//
// capsid_runtime_build_info() performs a true size negotiation (spec §11.4):
// the caller announces its buffer size through struct_size and receives
// exactly the leading fields that fit. Callers compiled against the
// build-info v1 headers (CAPSID_BUILD_INFO_VERSION 1) pass the smaller v1
// struct size and still succeed with the leading v1 fields filled; memory
// beyond the caller's buffer is never touched.
//
// The initializer is an inline in the current headers (it must stamp the
// CALLER's struct size); this translation unit exports the legacy symbol
// of the same name that v1-header callers link against.

// Skip the header's inline definition so this TU can own the external
// symbol; the struct and every macro still come from runtime.h.
#define CAPSID_BUILD_INFO_INIT_IMPLEMENTATION
#include "capsid/runtime.h"

#include <cstddef>
#include <cstring>

#include "build_identity.h"

namespace {

// The v1 layout (ABI v7, build-info version 1) ended with
// compatibility_id; anything smaller cannot carry even the v1 fields.
constexpr std::size_t kBuildInfoV1Size =
    offsetof(capsid_build_info, compatibility_id) + sizeof(const char *);

// The current layout is bigger by the appended v2 provenance fields.
constexpr std::size_t kBuildInfoStructSize = sizeof(capsid_build_info);

}  // namespace

// Legacy initializer for callers compiled against the build-info v1
// headers. Their buffer is the v1 size and the current layout is larger,
// so only the two envelope words are written with a v1-sized struct_size;
// the full-struct memset of a v1-era init would overflow their buffer.
// capsid_runtime_build_info() fills every field the caller's struct_size
// admits and stamps the current version. The header no longer declares
// this name for current callers, so the C linkage must be spelled out.
extern "C" void capsid_build_info_init(capsid_build_info *info) {
    if (info == nullptr) {
        return;
    }
    info->struct_size = static_cast<uint32_t>(kBuildInfoV1Size);
    info->version = CAPSID_BUILD_INFO_VERSION;
}

// Write a field only when the caller's buffer holds the whole field;
// otherwise leave the caller's bytes untouched. Writing in struct order
// with these guards is exactly "fill the leading fields that fit".
#define CAPSID_BI_WRITE_PTR(member, value)                                     \
    do {                                                                       \
        if (offsetof(capsid_build_info, member) + sizeof(const char *) <=      \
            out_info->struct_size) {                                           \
            out_info->member = (value);                                        \
        }                                                                      \
    } while (0)

#define CAPSID_BI_WRITE_U32(member, value)                                     \
    do {                                                                       \
        if (offsetof(capsid_build_info, member) + sizeof(uint32_t) <=          \
            out_info->struct_size) {                                           \
            out_info->member = (value);                                        \
        }                                                                      \
    } while (0)

capsid_result capsid_runtime_build_info(capsid_build_info *out_info) {
    if (out_info == nullptr) {
        return CAPSID_INVALID_ARGUMENT;
    }
    // The envelope must be initialized by the caller (or preserved from an
    // older ABI); a zeroed struct cannot be distinguished from an omitted
    // value. A struct_size below the v1 layout is not a known caller ABI.
    if (out_info->struct_size < kBuildInfoV1Size) {
        return CAPSID_INVALID_ARGUMENT;
    }

    // struct_size is the negotiated size actually written: the caller's
    // buffer, capped at the current full layout. version is stamped by the
    // library, so a v1 caller learns it is linked against a newer library.
    out_info->struct_size =
        out_info->struct_size < kBuildInfoStructSize
            ? out_info->struct_size
            : kBuildInfoStructSize;
    out_info->version = CAPSID_BUILD_INFO_VERSION;

    CAPSID_BI_WRITE_PTR(runtime_version, CAPSID_BUILD_RUNTIME_VERSION);
    CAPSID_BI_WRITE_U32(abi_version, CAPSID_BUILD_ABI_VERSION);
    CAPSID_BI_WRITE_U32(fetchrpc_version, CAPSID_BUILD_FETCHRPC_VERSION);
    CAPSID_BI_WRITE_PTR(quickjs_commit, CAPSID_BUILD_QUICKJS_COMMIT);
    CAPSID_BI_WRITE_PTR(txiki_overlay_key, CAPSID_BUILD_TXIKI_OVERLAY_KEY);
    CAPSID_BI_WRITE_PTR(txiki_overlay_manifest,
                        CAPSID_BUILD_TXIKI_OVERLAY_MANIFEST);
    CAPSID_BI_WRITE_PTR(bytecode_compile_flags,
                        CAPSID_BUILD_BYTECODE_COMPILE_FLAGS);
    CAPSID_BI_WRITE_PTR(target_architecture,
                        CAPSID_BUILD_TARGET_ARCHITECTURE);
    CAPSID_BI_WRITE_PTR(endianness, CAPSID_BUILD_ENDIANNESS);
    CAPSID_BI_WRITE_U32(pointer_width_bits, CAPSID_BUILD_POINTER_WIDTH_BITS);
    CAPSID_BI_WRITE_PTR(bytecode_format_identity,
                        CAPSID_BUILD_BYTECODE_FORMAT_IDENTITY);
    CAPSID_BI_WRITE_PTR(capability_manifest_sha256,
                        CAPSID_BUILD_CAPABILITY_MANIFEST_SHA256);
    CAPSID_BI_WRITE_PTR(compatibility_id, CAPSID_BUILD_COMPATIBILITY_ID);
    CAPSID_BI_WRITE_PTR(build_id, CAPSID_BUILD_BUILD_ID);
    CAPSID_BI_WRITE_PTR(capsid_commit, CAPSID_BUILD_CAPSID_COMMIT);
    CAPSID_BI_WRITE_U32(capsid_tree_clean, CAPSID_BUILD_TREE_CLEAN);
    CAPSID_BI_WRITE_U32(provenance_dirty, CAPSID_BUILD_PROVENANCE_DIRTY);
    CAPSID_BI_WRITE_PTR(compiler_id, CAPSID_BUILD_COMPILER_ID);
    CAPSID_BI_WRITE_PTR(compiler_version, CAPSID_BUILD_COMPILER_VERSION);
    CAPSID_BI_WRITE_PTR(target_triple, CAPSID_BUILD_TARGET_TRIPLE);
    CAPSID_BI_WRITE_PTR(cmake_build_type, CAPSID_BUILD_TYPE);
    CAPSID_BI_WRITE_PTR(build_feature_flags, CAPSID_BUILD_FEATURE_FLAGS);
    return CAPSID_OK;
}

#undef CAPSID_BI_WRITE_PTR
#undef CAPSID_BI_WRITE_U32
