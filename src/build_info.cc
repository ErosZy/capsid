// Library-side build identity.
//
// Every value, including the compatibility ID, comes from the generated
// build_identity.h (cmake/ComputeBuildIdentity.cmake); this translation
// unit never hashes or formats the canonical record itself, so the library,
// the worker and the bytecode compiler cannot drift apart.

#include "capsid/runtime.h"

#include <cstddef>
#include <cstring>

#include "build_identity.h"

namespace {

// The current ABI carries exactly this many leading bytes; the check below
// is a size negotiation, not a struct comparison (see the header comment on
// capsid_build_info).
constexpr std::size_t kBuildInfoStructSize = sizeof(capsid_build_info);

}  // namespace

void capsid_build_info_init(capsid_build_info *info) {
    if (info == nullptr) {
        return;
    }
    std::memset(info, 0, sizeof(*info));
    info->struct_size = sizeof(*info);
    info->version = CAPSID_BUILD_INFO_VERSION;
}

capsid_result capsid_runtime_build_info(capsid_build_info *out_info) {
    if (out_info == nullptr) {
        return CAPSID_INVALID_ARGUMENT;
    }
    // Callers must initialize the envelope first (or preserve struct_size
    // from an older ABI); a zeroed struct cannot be distinguished from an
    // omitted value.
    if (out_info->struct_size < kBuildInfoStructSize) {
        return CAPSID_INVALID_ARGUMENT;
    }

    out_info->struct_size = kBuildInfoStructSize;
    out_info->version = CAPSID_BUILD_INFO_VERSION;
    out_info->runtime_version = CAPSID_BUILD_RUNTIME_VERSION;
    out_info->abi_version = CAPSID_BUILD_ABI_VERSION;
    out_info->fetchrpc_version = CAPSID_BUILD_FETCHRPC_VERSION;
    out_info->quickjs_commit = CAPSID_BUILD_QUICKJS_COMMIT;
    out_info->txiki_overlay_key = CAPSID_BUILD_TXIKI_OVERLAY_KEY;
    out_info->txiki_overlay_manifest = CAPSID_BUILD_TXIKI_OVERLAY_MANIFEST;
    out_info->bytecode_compile_flags = CAPSID_BUILD_BYTECODE_COMPILE_FLAGS;
    out_info->target_architecture = CAPSID_BUILD_TARGET_ARCHITECTURE;
    out_info->endianness = CAPSID_BUILD_ENDIANNESS;
    out_info->pointer_width_bits = CAPSID_BUILD_POINTER_WIDTH_BITS;
    out_info->bytecode_format_identity =
        CAPSID_BUILD_BYTECODE_FORMAT_IDENTITY;
    out_info->capability_manifest_sha256 =
        CAPSID_BUILD_CAPABILITY_MANIFEST_SHA256;
    out_info->compatibility_id = CAPSID_BUILD_COMPATIBILITY_ID;
    return CAPSID_OK;
}
