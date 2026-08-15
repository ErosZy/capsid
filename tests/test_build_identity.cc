#include "capsid/runtime.h"

#include <openssl/evp.h>

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include "win32_compat.h"
#include <string>
#include <sys/types.h>
#if defined(_WIN32)
#include "win32_compat.h"
#else
#include <sys/wait.h>
#endif
#if defined(_WIN32)
#include "win32_compat.h"
#else
#include <unistd.h>
#endif

namespace {

[[noreturn]] void fail(const std::string& message) {
    std::cerr << "test-build-identity: " << message << std::endl;
    std::exit(1);
}

void require(bool condition, const std::string& message) {
    if (!condition) {
        fail(message);
    }
}

bool is_lower_hex(const char* value, std::size_t size) {
    if (value == nullptr || std::strlen(value) != size) {
        return false;
    }
    for (std::size_t i = 0; i < size; ++i) {
        const char c = value[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return false;
        }
    }
    return true;
}

bool is_sha256_id(const char* value) {
    return value != nullptr && std::strncmp(value, "sha256:", 7) == 0 &&
           is_lower_hex(value + 7, 64);
}

std::string sha256_id(const std::string& bytes) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_size = 0;
    require(EVP_Digest(bytes.data(), bytes.size(), digest, &digest_size,
                       EVP_sha256(), nullptr) == 1,
            "OpenSSL could not hash the compatibility record");
    require(digest_size == 32, "SHA-256 returned an unexpected digest size");
    static constexpr char kHex[] = "0123456789abcdef";
    std::string result = "sha256:";
    result.reserve(71);
    for (unsigned int i = 0; i < digest_size; ++i) {
        result.push_back(kHex[digest[i] >> 4]);
        result.push_back(kHex[digest[i] & 0x0f]);
    }
    return result;
}

// Compatibility record v2 (spec §11.2): exactly the fields that change
// whether one build's QuickJS bytecode reads in another build.
std::string canonical_record(const capsid_build_info& info) {
    return std::string("schema=capsid-bytecode-compatibility-v2\n") +
           "quickjsCommit=" + info.quickjs_commit + "\n" +
           "txikiOverlayManifest=" + info.txiki_overlay_manifest + "\n" +
           "bytecodeCompileFlags=" + info.bytecode_compile_flags + "\n" +
           "targetArchitecture=" + info.target_architecture + "\n" +
           "endianness=" + info.endianness + "\n" +
           "pointerWidthBits=" + std::to_string(info.pointer_width_bits) +
           "\n" + "bytecodeFormatIdentity=" +
           info.bytecode_format_identity + "\n";
}

// Provenance record v1 (spec §11.3) minus the final buildId line, in the
// fixed order documented in runtime.h. build_id is the SHA-256 of exactly
// these bytes; recomputing it here proves no record field is missing or
// misordered (spec §11.4: the test must detect field absence).
std::string canonical_provenance(const capsid_build_info& info) {
    return std::string("schema=capsid-build-provenance-v1\n") +
           "capsidCommit=" + info.capsid_commit + "\n" +
           "capsidTreeClean=" +
           (info.capsid_tree_clean == 1 ? "true" : "false") + "\n" +
           "runtimeVersion=" + info.runtime_version + "\n" +
           "abiVersion=" + std::to_string(info.abi_version) + "\n" +
           "fetchRpcVersion=" + std::to_string(info.fetchrpc_version) +
           "\n" + "compatibilityId=" + info.compatibility_id + "\n" +
           "capabilityManifestSha256=" + info.capability_manifest_sha256 +
           "\n" + "compilerId=" + info.compiler_id + "\n" +
           "compilerVersion=" + info.compiler_version + "\n" +
           "targetTriple=" + info.target_triple + "\n" +
           "cmakeBuildType=" + info.cmake_build_type + "\n" +
           "featureFlags=" + info.build_feature_flags + "\n" +
           "dependencyOverlayKey=" + info.txiki_overlay_key + "\n";
}

// Every compile/feature flag key the provenance record promises must be
// present in the canonical feature-flags string — a missing key means the
// build identity silently dropped a controlled build difference.
bool feature_flags_complete(const char* flags) {
    if (flags == nullptr) {
        return false;
    }
    for (const char* key : { "lto=", "asan=", "ubsan=", "tsan=",
                             "mimalloc=", "host=", "worker=" }) {
        if (std::strstr(flags, key) == nullptr) {
            return false;
        }
    }
    return true;
}

capsid_build_info read_library_build_info() {
    capsid_build_info invalid = {};
    require(capsid_runtime_build_info(nullptr) == CAPSID_INVALID_ARGUMENT,
            "NULL build-info output was accepted");
    require(capsid_runtime_build_info(&invalid) == CAPSID_INVALID_ARGUMENT,
            "uninitialized build-info output was accepted");

    capsid_build_info info;
    capsid_build_info_init(&info);
    require(info.struct_size == sizeof(info) &&
                info.version == CAPSID_BUILD_INFO_VERSION,
            "build-info initializer returned the wrong envelope");
    require(capsid_runtime_build_info(&info) == CAPSID_OK,
            "library build info was unavailable");
    require(info.version == CAPSID_BUILD_INFO_VERSION &&
                info.abi_version == CAPSID_ABI_VERSION &&
                info.fetchrpc_version != 0,
            "library build info reported incompatible numeric versions");
    require(info.runtime_version != nullptr && info.runtime_version[0] != 0 &&
                is_lower_hex(info.quickjs_commit, 40) &&
                is_lower_hex(info.txiki_overlay_key, 64) &&
                is_lower_hex(info.txiki_overlay_manifest, 64) &&
                info.bytecode_compile_flags != nullptr &&
                info.bytecode_compile_flags[0] != 0 &&
                info.target_architecture != nullptr &&
                info.target_architecture[0] != 0 &&
                info.bytecode_format_identity != nullptr &&
                info.bytecode_format_identity[0] != 0 &&
                is_lower_hex(info.capability_manifest_sha256, 64),
            "library build info contains an empty or malformed identity field");
    require(is_sha256_id(info.build_id),
            "library build ID is not lowercase sha256");
    require(sha256_id(canonical_provenance(info)) == info.build_id,
            "library build ID does not cover the canonical provenance "
            "record; a provenance field is missing or misordered");
    require(info.capsid_commit != nullptr &&
                (is_lower_hex(info.capsid_commit, 40) ||
                 std::strcmp(info.capsid_commit, "unknown") == 0),
            "capsid commit must be 40 lowercase hex or \"unknown\"");
    require(info.capsid_tree_clean == 0 || info.capsid_tree_clean == 1,
            "capsid_tree_clean is not a boolean");
    require(info.provenance_dirty == 0 || info.provenance_dirty == 1,
            "provenance_dirty is not a boolean");
    const bool commit_known = is_lower_hex(info.capsid_commit, 40);
    const bool release_grade = std::strcmp(info.cmake_build_type, "Release") == 0 &&
                               info.capsid_tree_clean == 1 && commit_known;
    require(info.provenance_dirty == (release_grade ? 0 : 1),
            "provenance_dirty disagrees with build type/commit/tree state");
    require(info.compiler_id != nullptr && info.compiler_id[0] != 0 &&
                info.compiler_version != nullptr &&
                info.compiler_version[0] != 0 &&
                info.target_triple != nullptr && info.target_triple[0] != 0 &&
                info.cmake_build_type != nullptr &&
                feature_flags_complete(info.build_feature_flags),
            "library build provenance is missing toolchain or feature "
            "information");
#ifdef CAPSID_TEST_EXPECTED_QUICKJS_COMMIT
    require(std::strcmp(info.quickjs_commit,
                        CAPSID_TEST_EXPECTED_QUICKJS_COMMIT) == 0,
            "build identity carries the txiki.js commit instead of the "
            "locked QuickJS gitlink");
#endif
    require((std::strcmp(info.endianness, "little") == 0 ||
             std::strcmp(info.endianness, "big") == 0) &&
                info.pointer_width_bits == sizeof(void*) * 8,
            "library build info does not describe the running architecture");
    require(is_sha256_id(info.compatibility_id),
            "library compatibility ID is not lowercase sha256");
    require(sha256_id(canonical_record(info)) == info.compatibility_id,
            "library compatibility ID does not cover the canonical record");
    return info;
}

std::string read_child_stdout(const char* executable, const char* argument) {
#if defined(_WIN32)
    std::string command = std::string("\"") + executable + "\" \"" +
                          argument + "\"";
    FILE* stream = _popen(command.c_str(), "rb");
    require(stream != nullptr, "could not spawn compiler identity probe");
    std::string output;
    char buffer[256];
    for (;;) {
        const size_t count = std::fread(buffer, 1, sizeof(buffer), stream);
        if (count > 0) {
            output.append(buffer, count);
            continue;
        }
        break;
    }
    require(_pclose(stream) == 0, "compiler identity probe failed");
    while (!output.empty() &&
           (output.back() == '\n' || output.back() == '\r')) {
        output.pop_back();
    }
    return output;
#else
    int descriptors[2];
    require(pipe(descriptors) == 0, "could not create compiler output pipe");
    const pid_t child = fork();
    require(child >= 0, "could not fork compiler identity probe");
    if (child == 0) {
        close(descriptors[0]);
        if (dup2(descriptors[1], STDOUT_FILENO) < 0) {
            _exit(126);
        }
        close(descriptors[1]);
        execl(executable, executable, argument, static_cast<char*>(nullptr));
        _exit(127);
    }
    close(descriptors[1]);
    std::string output;
    char buffer[256];
    for (;;) {
        const ssize_t count = read(descriptors[0], buffer, sizeof(buffer));
        if (count > 0) {
            output.append(buffer, static_cast<std::size_t>(count));
            continue;
        }
        if (count < 0 && errno == EINTR) {
            continue;
        }
        require(count == 0, "could not read compiler identity output");
        break;
    }
    close(descriptors[0]);
    int status = 0;
    require(waitpid(child, &status, 0) == child && WIFEXITED(status) &&
                WEXITSTATUS(status) == 0,
            "compiler identity probe failed");
    while (!output.empty() &&
           (output.back() == '\n' || output.back() == '\r')) {
        output.pop_back();
    }
    return output;
#endif
}

std::string read_worker_identity(const char* worker_path) {
    capsid_worker_config config;
    capsid_worker_config_init(&config);
    config.worker_path = worker_path;
    capsid_worker* worker = nullptr;
    require(capsid_worker_spawn(&config, &worker) == CAPSID_OK &&
                worker != nullptr,
            "could not spawn identity worker");

    static constexpr char kBundle[] =
        "export default { fetch() { return new Response('ok'); } };";
    require(capsid_worker_load_bundle_named(
                worker, reinterpret_cast<const std::uint8_t*>(kBundle),
                sizeof(kBundle) - 1, "bundle.mjs") == CAPSID_OK &&
                capsid_worker_flush(worker) == CAPSID_OK,
            "could not load identity worker bundle");

    std::string identity;
    for (int attempt = 0; attempt < 100 && identity.empty(); ++attempt) {
        capsid_event event = {};
        event.struct_size = sizeof(event);
        const capsid_result next = capsid_worker_next_event(worker, &event);
        if (next == CAPSID_WOULD_BLOCK) {
            capsid_pollfd descriptor = {capsid_worker_fd(worker), POLLIN, 0};
            require(capsid::win32::capsid_poll(&descriptor, 1, 100) >= 0 || errno == EINTR,
                    "worker identity poll failed");
            continue;
        }
        require(next == CAPSID_OK, "worker closed before READY identity");
        require(event.type != CAPSID_EVENT_ERROR &&
                    event.type != CAPSID_EVENT_EXIT,
                "worker rejected the identity probe bundle");
        if (event.type == CAPSID_EVENT_READY) {
            require(event.payload.data != nullptr && event.payload.size == 71,
                    "READY did not carry a sha256 compatibility ID");
            identity.assign(
                reinterpret_cast<const char*>(event.payload.data),
                event.payload.size);
        }
    }
    capsid_worker_destroy(worker);
    require(!identity.empty(), "worker did not become READY");
    return identity;
}

}  // namespace

int main(int argc, char** argv) {
    capsid_build_info_init(nullptr);
    const capsid_build_info info = read_library_build_info();
    if (argc == 1) {
        return 0;
    }
    require(argc == 3, "expected optional <worker> <compiler> arguments");
    const std::string compiler_identity =
        read_child_stdout(argv[2], "--print-compatibility-id");
    const std::string worker_identity = read_worker_identity(argv[1]);
    require(compiler_identity == info.compatibility_id,
            "compiler compatibility ID differs from the library");
    require(worker_identity == info.compatibility_id,
            "worker compatibility ID differs from the library");
    return 0;
}
