// capsid-bytecode-compile: first-party QuickJS module-bytecode compiler for
// the trusted-bytecode deployment path (M1D-1).
//
// Frozen CLI:
//   --source <bundle.mjs>        input module source
//   --source-name <exact-name>   module name; must equal the worker's
//                                --source-name expectation
//   --application <AppId>        attestation claim
//   --version <VersionId>        attestation claim
//   --key-id <KeyId>             attestation claim (offline signing key id)
//   --bytecode-out <bundle.qjsb> compiled module bytecode
//   --attestation-out <bytecode.json>
//   --signing-message-out <message.bin>
//
// The compiler links the same QuickJS (the txiki overlay) as the worker, so
// the emitted bytecode is loadable by the release artifact's runtime. It
// never receives a private key and never signs: --signing-message-out is the
// exact frozen byte string the offline pipeline signs, and the host-side
// verifier (verify_bytecode_attestation) re-derives the same bytes.
//
// Output contract:
//   - the three outputs are all-or-none (no partial official files);
//   - existing outputs are never overwritten;
//   - identical inputs produce identical bytecode, attestation and message.

#include <jansson.h>
#include <openssl/evp.h>
#include <quickjs.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "bytecode_rewriter/bytecode_rewriter.h"
#include "win32_compat.h"
#if defined(_WIN32)
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#if defined(__linux__)
#include <sys/syscall.h>
#endif

#include "build_identity.h"

namespace {

constexpr std::size_t kMaxSourceBytes = 16U * 1024U * 1024U;
constexpr const char* kAttestationSchema = "capsid-bytecode-v1";

void fail_usage() {
    std::fputs(
        "usage: capsid-bytecode-compile\n"
        "  --source <bundle.mjs> --source-name <exact-name>\n"
        "  --application <AppId> --version <VersionId> --key-id <KeyId>\n"
        "  --bytecode-out <bundle.qjsb> --attestation-out <bytecode.json>\n"
        "  --signing-message-out <message.bin>\n"
        "  --print-compatibility-id   (alone; never mixed with the above)\n",
        stderr);
}

bool read_source(const char* path, std::vector<std::uint8_t>* out) {
    std::FILE* handle = std::fopen(path, "rb");
    if (handle == nullptr) {
        std::fprintf(stderr, "capsid-bytecode-compile: cannot open source: %s\n",
                     path);
        return false;
    }
    out->clear();
    std::uint8_t buffer[64 * 1024];
    std::size_t total = 0;
    for (;;) {
        const std::size_t read_size =
            std::fread(buffer, 1, sizeof(buffer), handle);
        if (read_size == 0) {
            if (std::ferror(handle)) {
                std::fclose(handle);
                std::fprintf(stderr,
                             "capsid-bytecode-compile: source read failed: %s\n",
                             path);
                return false;
            }
            break;
        }
        total += read_size;
        if (total > kMaxSourceBytes) {
            std::fclose(handle);
            std::fprintf(stderr,
                         "capsid-bytecode-compile: source exceeds %zu bytes\n",
                         kMaxSourceBytes);
            return false;
        }
        out->insert(out->end(), buffer, buffer + read_size);
    }
    std::fclose(handle);
    return true;
}

bool sha256_hex(const std::uint8_t* data,
                std::size_t size,
                std::string* out) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_size = 0;
    if (EVP_Digest(data, size, digest, &digest_size, EVP_sha256(), nullptr) !=
            1 ||
        digest_size != 32) {
        return false;
    }
    static constexpr char kHex[] = "0123456789abcdef";
    out->clear();
    out->reserve(64);
    for (unsigned int i = 0; i < digest_size; ++i) {
        out->push_back(kHex[digest[i] >> 4]);
        out->push_back(kHex[digest[i] & 0x0f]);
    }
    return true;
}

// Module loader for the compile-only eval. Import-free bundles never
// reach it; mirrors the worker's loader contract (a missing module fails
// the compile). The worker resolves imports at load time from its own
// policy, so the compiler only needs the loader slot installed.
JSModuleDef* compile_module_loader(JSContext* ctx,
                                   const char* module_name,
                                   void*) {
    JS_ThrowReferenceError(ctx, "module is unavailable: %s", module_name);
    return nullptr;
}

// QuickJS compile: same module-compile path as the worker's trusted
// bytecode load (JS_Eval compile-only, then JS_WriteObject bytecode). The
// source-name is the module filename, so the module name matches the
// worker's bundle-name check.
bool compile_module(const std::string& source_name,
                    const std::vector<std::uint8_t>& source,
                    std::vector<std::uint8_t>* bytecode) {
    JSRuntime* runtime = JS_NewRuntime();
    if (runtime == nullptr) {
        return false;
    }
    JSContext* ctx = JS_NewContext(runtime);
    if (ctx == nullptr) {
        JS_FreeRuntime(runtime);
        return false;
    }
    JS_SetModuleLoaderFunc(runtime, nullptr, compile_module_loader, nullptr);
    // The lexer lookahead wants a readable NUL sentinel past input_len
    // (same as the worker's load path); the sentinel is not part of the
    // source length or the source hash.
    std::string source_with_sentinel(source.begin(), source.end());
    source_with_sentinel.push_back('\0');
    JSValue module = JS_Eval(
        ctx,
        source_with_sentinel.data(),
        source.size(),
        source_name.c_str(),
        JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    if (JS_IsException(module)) {
        JS_FreeValue(ctx, module);
        JS_FreeContext(ctx);
        JS_FreeRuntime(runtime);
        std::fputs("capsid-bytecode-compile: module compile failed\n", stderr);
        return false;
    }
    std::size_t size = 0;
    std::uint8_t* data = JS_WriteObject(ctx, &size, module, JS_WRITE_OBJ_BYTECODE);
    JS_FreeValue(ctx, module);
    if (data == nullptr) {
        JS_FreeContext(ctx);
        JS_FreeRuntime(runtime);
        std::fputs("capsid-bytecode-compile: bytecode serialization failed\n",
                   stderr);
        return false;
    }
    bytecode->assign(data, data + size);
    js_free(ctx, data);
    JS_FreeContext(ctx);
    JS_FreeRuntime(runtime);
    return true;
}

// Canonical single-line attestation with the frozen field order. The
// verifier's strict parser (parse_attestation_json) accepts exactly these
// keys in any order, but the compiler emits the canonical order so repeated
// compiles are byte-identical.
bool build_attestation(const std::string& application,
                       const std::string& version,
                       const std::string& source_name,
                       const std::string& source_sha256,
                       const std::string& bytecode_sha256,
                       const std::string& key_id,
                       std::string* out) {
    json_t* root = json_object();
    if (root == nullptr) {
        return false;
    }
    // Digest claims carry the frozen "sha256:" prefix the verifier's strict
    // parser requires.
    const std::string source_digest = "sha256:" + source_sha256;
    const std::string bytecode_digest = "sha256:" + bytecode_sha256;
    bool ok = json_object_set_new(root, "schema", json_string(kAttestationSchema)) == 0 &&
              json_object_set_new(root, "application", json_string(application.c_str())) == 0 &&
              json_object_set_new(root, "version", json_string(version.c_str())) == 0 &&
              json_object_set_new(root, "sourceName", json_string(source_name.c_str())) == 0 &&
              json_object_set_new(root, "sourceSha256", json_string(source_digest.c_str())) == 0 &&
              json_object_set_new(root, "bytecodeSha256", json_string(bytecode_digest.c_str())) == 0 &&
              json_object_set_new(root, "compatibilityId",
                                  json_string(CAPSID_BUILD_COMPATIBILITY_ID)) == 0 &&
              json_object_set_new(root, "keyId", json_string(key_id.c_str())) == 0;
    if (!ok) {
        json_decref(root);
        return false;
    }
    char* dump = json_dumps(root, JSON_COMPACT | JSON_PRESERVE_ORDER);
    json_decref(root);
    if (dump == nullptr) {
        return false;
    }
    *out = dump;
    std::free(dump);
    return true;
}

// Frozen signed message: the domain string including its terminating NUL,
// then each claim as a 32-bit big-endian length and UTF-8 bytes, in the
// schema/application/version/sourceName/sourceSha256/bytecodeSha256/
// compatibilityId/keyId order. This is byte-identical to what
// verify_bytecode_attestation re-derives before signature verification.
bool build_signing_message(const std::string& application,
                           const std::string& version,
                           const std::string& source_name,
                           const std::string& source_sha256,
                           const std::string& bytecode_sha256,
                           const std::string& key_id,
                           std::vector<std::uint8_t>* out) {
    static constexpr char kDomain[] = "capsid-bytecode-attestation-v1\0";
    out->assign(reinterpret_cast<const std::uint8_t*>(kDomain),
                reinterpret_cast<const std::uint8_t*>(kDomain) +
                    sizeof(kDomain) - 1);
    const std::string fields[] = {
        kAttestationSchema,
        application,
        version,
        source_name,
        "sha256:" + source_sha256,
        "sha256:" + bytecode_sha256,
        CAPSID_BUILD_COMPATIBILITY_ID,
        key_id,
    };
    for (const std::string& field : fields) {
        const std::uint32_t size = static_cast<std::uint32_t>(field.size());
        out->push_back(static_cast<std::uint8_t>(size >> 24));
        out->push_back(static_cast<std::uint8_t>(size >> 16));
        out->push_back(static_cast<std::uint8_t>(size >> 8));
        out->push_back(static_cast<std::uint8_t>(size));
        out->insert(out->end(), field.begin(), field.end());
    }
    return true;
}

// Publish safety (M1D audit):
//   - temps are created in the target directory with O_CREAT|O_EXCL|
//     O_NOFOLLOW|O_CLOEXEC — a pre-placed temp symlink or concurrent
//     creation cannot be followed or truncated;
//   - the final publish is no-replace (renameat2 RENAME_NOREPLACE on
//     Linux, linkat+unlinkat elsewhere): an existing target is never
//     overwritten, even by a concurrent creator;
//   - rollback unlinks only the targets THIS invocation actually renamed;
//     pre-existing or concurrently created files are never removed;
//   - three independent output paths cannot be power-loss atomic; that
//     atomicity belongs to the deploy pipeline's staging generation
//     directory. This tool guarantees process-level all-or-none only.
struct OutputTarget {
    std::string dir;   // directory of the output path
    std::string name;  // final basename
    std::string temp;  // temp basename (name + ".tmp." + suffix)
    const void* data = nullptr;
    std::size_t size = 0;
};

bool split_output_path(const std::string& path, OutputTarget* target,
                       const std::string& suffix) {
    const std::string::size_type slash = path.find_last_of("/\\");
    target->dir = slash == std::string::npos ? "." : path.substr(0, slash);
    target->name =
        slash == std::string::npos ? path : path.substr(slash + 1);
    if (target->name.empty() || target->name == "." || target->name == ".." ||
        target->name.find('/') != std::string::npos ||
        target->name.find('\\') != std::string::npos) {
        return false;
    }
    target->temp = target->name + ".tmp." + suffix;
    return true;
}

#if defined(_WIN32)
// Windows output pipeline. CREATE_NEW is the O_CREAT|O_EXCL equivalent;
// FILE_FLAG_OPEN_REPARSE_POINT rejects symlink temps; FlushFileBuffers is
// fsync; MoveFileEx without MOVEFILE_REPLACE_EXISTING is the
// rename-without-replace contract (ERROR_ALREADY_EXISTS on a present
// final name); DeleteFileW is unlink.
std::wstring widen_output_path(const std::string& value) {
    const int wide_size =
        MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
    if (wide_size <= 0) {
        return std::wstring();
    }
    std::wstring wide(static_cast<std::size_t>(wide_size - 1), L'\0');
    if (MultiByteToWideChar(
            CP_UTF8, 0, value.c_str(), -1, &wide[0], wide_size) <= 0) {
        return std::wstring();
    }
    return wide;
}

std::wstring output_path(const OutputTarget& target,
                         const std::string& name) {
    return widen_output_path(
        target.dir + (target.dir.empty() ? "" : "\\") + name);
}

bool write_temp_file(const OutputTarget& target, std::string* error) {
    const std::wstring temp_path = output_path(target, target.temp);
    const HANDLE handle = CreateFileW(
        temp_path.c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        *error = "cannot create temporary output";
        return false;
    }
    FILE_ATTRIBUTE_TAG_INFO attributes = {};
    if (GetFileInformationByHandleEx(
            handle, FileAttributeTagInfo, &attributes,
            sizeof(attributes)) != 0 &&
        (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        CloseHandle(handle);
        (void)DeleteFileW(temp_path.c_str());
        *error = "cannot create temporary output";
        return false;
    }
    const std::uint8_t* data =
        static_cast<const std::uint8_t*>(target.data);
    std::size_t offset = 0;
    bool ok = true;
    while (offset < target.size) {
        DWORD written = 0;
        const DWORD chunk = target.size - offset > MAXDWORD
            ? MAXDWORD
            : static_cast<DWORD>(target.size - offset);
        if (!WriteFile(handle, data + offset, chunk, &written, nullptr) ||
            written == 0) {
            ok = false;
            break;
        }
        offset += static_cast<std::size_t>(written);
    }
    if (ok) {
        ok = FlushFileBuffers(handle) != 0;
    }
    CloseHandle(handle);
    if (!ok) {
        (void)DeleteFileW(temp_path.c_str());
        *error = "cannot write temporary output";
        return false;
    }
    return true;
}

// No-replace publish: MoveFileEx refuses to replace an existing final
// name (MOVEFILE_REPLACE_EXISTING is intentionally absent).
bool publish_no_replace(const OutputTarget& target, std::string* error) {
    const std::wstring temp_path = output_path(target, target.temp);
    const std::wstring final_path = output_path(target, target.name);
    if (MoveFileExW(temp_path.c_str(), final_path.c_str(),
                    MOVEFILE_WRITE_THROUGH)) {
        return true;
    }
    const DWORD saved = GetLastError();
    if (saved == ERROR_ALREADY_EXISTS || saved == ERROR_FILE_EXISTS) {
        *error = "output already exists";
    }
    return false;
}

bool remove_output_file(const OutputTarget& target,
                        const std::string& name) {
    const std::wstring path = output_path(target, name);
    return DeleteFileW(path.c_str()) != 0;
}
#else
bool write_temp_file(const OutputTarget& target, std::string* error) {
    const int dir_fd = open(target.dir.c_str(),
                            O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dir_fd < 0) {
        *error = "cannot open output directory";
        return false;
    }
    // O_EXCL + O_NOFOLLOW: the temp must not pre-exist and must not be a
    // symlink; a concurrent creator loses the race cleanly.
    const int fd = openat(dir_fd, target.temp.c_str(),
                          O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW |
                              O_CLOEXEC,
                          0600);
    if (fd < 0) {
        close(dir_fd);
        *error = "cannot create temporary output";
        return false;
    }
    const std::uint8_t* data =
        static_cast<const std::uint8_t*>(target.data);
    std::size_t offset = 0;
    bool ok = true;
    while (offset < target.size) {
        const ssize_t written =
            write(fd, data + offset, target.size - offset);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            ok = false;
            break;
        }
        offset += static_cast<std::size_t>(written);
    }
    if (ok && fsync(fd) != 0) {
        ok = false;
    }
    close(fd);
    if (!ok) {
        unlinkat(dir_fd, target.temp.c_str(), 0);
        close(dir_fd);
        *error = "cannot write temporary output";
        return false;
    }
    close(dir_fd);
    return true;
}

// No-replace publish. Linux uses renameat2(RENAME_NOREPLACE); other
// platforms use linkat + unlinkat (the link fails with EEXIST when the
// target exists, so the final name is never overwritten).
bool publish_no_replace(const OutputTarget& target, std::string* error) {
    const int dir_fd = open(target.dir.c_str(),
                            O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dir_fd < 0) {
        *error = "cannot open output directory";
        return false;
    }
    bool published = false;
#if defined(__linux__)
    // glibc >= 2.28 provides renameat2 directly; fall back to the syscall
    // on older libcs (RENAME_NOREPLACE is not exposed by <stdio.h>).
    if (renameat2(dir_fd, target.temp.c_str(), dir_fd, target.name.c_str(),
                  RENAME_NOREPLACE) == 0) {
        published = true;
    }
#else
    if (linkat(dir_fd, target.temp.c_str(), dir_fd, target.name.c_str(), 0) ==
            0) {
        unlinkat(dir_fd, target.temp.c_str(), 0);
        published = true;
    }
#endif
    if (!published && errno == EEXIST) {
        *error = "output already exists";
        close(dir_fd);
        return false;
    }
    close(dir_fd);
    return published;
}

bool remove_output_file(const OutputTarget& target,
                        const std::string& name) {
    const int dir_fd = open(target.dir.c_str(),
                            O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dir_fd < 0) {
        return false;
    }
    const bool removed = unlinkat(dir_fd, name.c_str(), 0) == 0;
    close(dir_fd);
    return removed;
}
#endif

struct AtomicOutputs {
    OutputTarget bytecode;
    OutputTarget attestation;
    OutputTarget message;
};

bool commit_outputs(const AtomicOutputs& outputs) {
    std::string error;
    bool ok =
        write_temp_file(outputs.bytecode, &error) &&
        write_temp_file(outputs.attestation, &error) &&
        write_temp_file(outputs.message, &error);
    // Publish all three; remember which finals THIS invocation renamed so
    // rollback never removes a pre-existing or concurrent file.
    bool published_bytecode = false;
    bool published_attestation = false;
    bool published_message = false;
    if (ok) {
        published_bytecode = publish_no_replace(outputs.bytecode, &error);
        ok = published_bytecode;
    }
    if (ok) {
        published_attestation = publish_no_replace(outputs.attestation, &error);
        ok = published_attestation;
    }
    if (ok) {
        published_message = publish_no_replace(outputs.message, &error);
        ok = published_message;
    }
    if (!ok) {
        std::fprintf(stderr,
                     "capsid-bytecode-compile: output commit failed (%s); "
                     "rolled back this invocation's files\n",
                     error.c_str());
    }
    // Rollback: temps always; finals only if this invocation renamed them.
    if (!ok || !published_bytecode) {
        (void)remove_output_file(outputs.bytecode, outputs.bytecode.temp);
    }
    if (published_bytecode && !ok) {
        (void)remove_output_file(outputs.bytecode, outputs.bytecode.name);
    }
    if (!ok || !published_attestation) {
        (void)remove_output_file(outputs.attestation,
                                 outputs.attestation.temp);
    }
    if (published_attestation && !ok) {
        (void)remove_output_file(outputs.attestation,
                                 outputs.attestation.name);
    }
    if (!ok || !published_message) {
        (void)remove_output_file(outputs.message, outputs.message.temp);
    }
    if (published_message && !ok) {
        (void)remove_output_file(outputs.message, outputs.message.name);
    }
    return ok;
}

// Formal grammar for the attestation claims (M1D audit). Rejects NUL,
// control characters, path separators and empty/oversized values.
bool valid_claim(const std::string& value,
                 std::size_t min,
                 std::size_t max,
                 bool allow_slash) {
    if (value.size() < min || value.size() > max) {
        return false;
    }
    for (const char c : value) {
        const unsigned char byte = static_cast<unsigned char>(c);
        if (byte == 0 || byte < 0x20 || byte == 0x7f) {
            return false;
        }
        if (!allow_slash && (c == '/' || c == '\\')) {
            return false;
        }
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--print-compatibility-id") == 0) {
        std::fputs(CAPSID_BUILD_COMPATIBILITY_ID, stdout);
        std::fputc('\n', stdout);
        return 0;
    }

    std::string source_path;
    std::string source_name;
    std::string application;
    std::string version;
    std::string key_id;
    std::string bytecode_out;
    std::string attestation_out;
    std::string message_out;
    std::set<std::string> seen_flags;
    for (int index = 1; index < argc; index += 2) {
        const char* flag = argv[index];
        if (index + 1 >= argc) {
            fail_usage();
            return 2;
        }
        if (!seen_flags.insert(flag).second) {
            std::fprintf(stderr,
                         "capsid-bytecode-compile: duplicate argument: %s\n",
                         flag);
            return 2;
        }
        const std::string value = argv[index + 1];
        if (std::strcmp(flag, "--source") == 0) {
            source_path = value;
        } else if (std::strcmp(flag, "--source-name") == 0) {
            source_name = value;
        } else if (std::strcmp(flag, "--application") == 0) {
            application = value;
        } else if (std::strcmp(flag, "--version") == 0) {
            version = value;
        } else if (std::strcmp(flag, "--key-id") == 0) {
            key_id = value;
        } else if (std::strcmp(flag, "--bytecode-out") == 0) {
            bytecode_out = value;
        } else if (std::strcmp(flag, "--attestation-out") == 0) {
            attestation_out = value;
        } else if (std::strcmp(flag, "--signing-message-out") == 0) {
            message_out = value;
        } else {
            std::fprintf(stderr,
                         "capsid-bytecode-compile: unknown argument: %s\n",
                         flag);
            fail_usage();
            return 2;
        }
    }
    if (source_path.empty() || source_name.empty() || application.empty() ||
        version.empty() || key_id.empty() || bytecode_out.empty() ||
        attestation_out.empty() || message_out.empty()) {
        fail_usage();
        return 2;
    }
    // Claim grammar (M1D audit): application/version/key-id use the frozen
    // identifier grammar; sourceName is a module filename without path
    // separators. The three output paths must be distinct.
    // sourceName is a module identifier (URI-style, e.g. file:///app/...);
    // application/version/key-id are plain identifiers without separators.
    if (!valid_claim(application, 1, 64, false) ||
        !valid_claim(version, 1, 128, false) ||
        !valid_claim(key_id, 1, 128, false) ||
        !valid_claim(source_name, 1, 256, true)) {
        std::fputs("capsid-bytecode-compile: invalid claim value\n", stderr);
        return 2;
    }
    if (bytecode_out == attestation_out ||
        bytecode_out == message_out ||
        attestation_out == message_out) {
        std::fputs("capsid-bytecode-compile: output paths must be distinct\n",
                   stderr);
        return 2;
    }

    std::vector<std::uint8_t> source;
    if (!read_source(source_path.c_str(), &source)) {
        return 1;
    }
    std::string source_sha256;
    if (!sha256_hex(source.data(), source.size(), &source_sha256)) {
        return 1;
    }
    std::vector<std::uint8_t> bytecode;
    if (!compile_module(source_name, source, &bytecode)) {
        return 1;
    }
    // Bytecode AOT rewriter (docs/bytecode-aot-rewriter.md): pure
    // post-serialization rewrite, deterministic, fail-closed. The
    // attestation/signing flow below then covers the optimized bytes.
    {
        std::string opt_error;
        std::vector<std::uint8_t> optimized;
        if (!capsid::bytecode::rewrite(bytecode, &optimized,
                                        capsid::bytecode::kPassAll, true,
                                        &opt_error)) {
            std::fprintf(stderr, "%s\n", opt_error.c_str());
            return 1;
        }
        bytecode.swap(optimized);
    }
    std::string bytecode_sha256;
    if (!sha256_hex(bytecode.data(), bytecode.size(), &bytecode_sha256)) {
        return 1;
    }
    std::string attestation;
    if (!build_attestation(application, version, source_name, source_sha256,
                           bytecode_sha256, key_id, &attestation)) {
        return 1;
    }
    std::vector<std::uint8_t> message;
    if (!build_signing_message(application, version, source_name, source_sha256,
                               bytecode_sha256, key_id, &message)) {
        return 1;
    }

    // Deterministic temp suffix (pid only): the publish-safety RED can
    // pre-place a temp symlink at the exact temp path of the child it
    // forks. O_EXCL|O_NOFOLLOW then rejects it.
    const std::string suffix =
#if defined(_WIN32)
        std::to_string(static_cast<long long>(capsid::win32::getpid()));
#else
        std::to_string(static_cast<long long>(getpid()));
#endif
    AtomicOutputs outputs;
    if (!split_output_path(bytecode_out, &outputs.bytecode, suffix) ||
        !split_output_path(attestation_out, &outputs.attestation, suffix) ||
        !split_output_path(message_out, &outputs.message, suffix)) {
        std::fputs("capsid-bytecode-compile: invalid output path\n", stderr);
        return 2;
    }
    outputs.bytecode.data = bytecode.data();
    outputs.bytecode.size = bytecode.size();
    outputs.attestation.data = attestation.data();
    outputs.attestation.size = attestation.size();
    outputs.message.data = message.data();
    outputs.message.size = message.size();
    return commit_outputs(outputs) ? 0 : 1;
}
