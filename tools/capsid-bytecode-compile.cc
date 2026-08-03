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
#include <string>
#include <unistd.h>
#include <vector>

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
    bool ok = json_object_set_new(root, "schema", json_string(kAttestationSchema)) == 0 &&
              json_object_set_new(root, "application", json_string(application.c_str())) == 0 &&
              json_object_set_new(root, "version", json_string(version.c_str())) == 0 &&
              json_object_set_new(root, "sourceName", json_string(source_name.c_str())) == 0 &&
              json_object_set_new(root, "sourceSha256", json_string(source_sha256.c_str())) == 0 &&
              json_object_set_new(root, "bytecodeSha256", json_string(bytecode_sha256.c_str())) == 0 &&
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
        source_sha256,
        bytecode_sha256,
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

// All-or-none output: every target must be absent; all three are written to
// temp files first, then renamed into place. Any failure removes every temp
// and every renamed target, so a failed run leaves no partial official set.
struct AtomicOutputs {
    const char* bytecode_path;
    const char* attestation_path;
    const char* message_path;
    const std::vector<std::uint8_t>& bytecode;
    const std::string& attestation;
    const std::vector<std::uint8_t>& message;
};

bool write_temp(const char* temp_path,
                const void* data,
                std::size_t size) {
    std::FILE* handle = std::fopen(temp_path, "wb");
    if (handle == nullptr) {
        return false;
    }
    const bool written =
        size == 0 || std::fwrite(data, 1, size, handle) == size;
    if (std::fclose(handle) != 0 || !written) {
        std::remove(temp_path);
        return false;
    }
    return true;
}

bool commit_outputs(const AtomicOutputs& outputs) {
    const std::string temp_bytecode =
        std::string(outputs.bytecode_path) + ".tmp." +
        std::to_string(static_cast<long long>(getpid()));
    const std::string temp_attestation =
        std::string(outputs.attestation_path) + ".tmp." +
        std::to_string(static_cast<long long>(getpid()));
    const std::string temp_message =
        std::string(outputs.message_path) + ".tmp." +
        std::to_string(static_cast<long long>(getpid()));

    // Existing outputs are never overwritten; check all three up front.
    for (const char* path : { outputs.bytecode_path, outputs.attestation_path,
                              outputs.message_path }) {
        std::FILE* probe = std::fopen(path, "rb");
        if (probe != nullptr) {
            std::fclose(probe);
            std::fprintf(stderr,
                         "capsid-bytecode-compile: refusing to overwrite %s\n",
                         path);
            return false;
        }
    }

    bool ok = write_temp(temp_bytecode.c_str(),
                         outputs.bytecode.data(), outputs.bytecode.size()) &&
              write_temp(temp_attestation.c_str(),
                         outputs.attestation.data(), outputs.attestation.size()) &&
              write_temp(temp_message.c_str(),
                         outputs.message.data(), outputs.message.size());
    if (ok) {
        ok = std::rename(temp_bytecode.c_str(), outputs.bytecode_path) == 0 &&
             std::rename(temp_attestation.c_str(), outputs.attestation_path) == 0 &&
             std::rename(temp_message.c_str(), outputs.message_path) == 0;
    }
    if (!ok) {
        std::remove(temp_bytecode.c_str());
        std::remove(temp_attestation.c_str());
        std::remove(temp_message.c_str());
        std::remove(outputs.bytecode_path);
        std::remove(outputs.attestation_path);
        std::remove(outputs.message_path);
        std::fputs("capsid-bytecode-compile: output commit failed; "
                   "no partial files left behind\n",
                   stderr);
    }
    return ok;
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
    for (int index = 1; index < argc; index += 2) {
        const char* flag = argv[index];
        if (index + 1 >= argc) {
            fail_usage();
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

    const AtomicOutputs outputs = {
        bytecode_out.c_str(),
        attestation_out.c_str(),
        message_out.c_str(),
        bytecode,
        attestation,
        message,
    };
    return commit_outputs(outputs) ? 0 : 1;
}
