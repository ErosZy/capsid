// Local capsid.json permissions for the single-worker / static-pool data
// planes (v0.1.3). See local_capsid_policy.h for the application boundary;
// this file is the loader: read (1 MiB cap) -> frozen schema validation ->
// extract the honored runtime settings (worker/request/pool/healthCheck/
// entry) -> parse -> reject valueFrom -> compile App permissions and
// Binding Manifest ∩ App grants through the same authoritative compilers
// -> build immutable runtime descriptors.

#include "host/local_capsid_policy.h"

#include "host/config.h"
#include "host/policy_compiler.h"
#include "win32_compat.h"

#if defined(_WIN32)
#include <io.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <jansson.h>

#include <cerrno>
#include <cstdint>
#include <string>
#include <vector>

namespace capsid::host {
namespace {

// The same per-file cap as the deployment-input safe-read boundary: a local
// capsid.json is an operator document, not a deployment artifact, but the
// budget is identical.
constexpr std::size_t kMaxLocalCapsidJsonBytes = 1024U * 1024U;

// Same secret key-id grammar as the managed provider
// ([A-Za-z0-9._-], no empty/dot components, <= 128 bytes). The managed
// implementation lives in the POSIX-only provider unit, so the Windows
// local data plane carries this copy instead of linking that unit.
constexpr std::size_t kMaxLocalSecretKeyIdBytes = 128U;
bool valid_local_secret_key_id(const std::string& key_id) {
    if (key_id.empty() || key_id.size() > kMaxLocalSecretKeyIdBytes) {
        return false;
    }
    for (const unsigned char c : key_id) {
        const bool alnum =
            (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9');
        if (!alnum && c != '.' && c != '_' && c != '-') {
            return false;
        }
    }
    return key_id.find("..") == std::string::npos;
}

// `entry` names one bundle file inside the capsid.json directory. It can
// never be absolute, a separator-led traversal (".." / "x/../y"), a drive
// path, or a directory component. The local data plane resolves it against
// the capsid.json directory, so the grammar is the containment boundary.
bool valid_local_entry_name(const std::string& entry) {
    if (entry.empty() || entry.size() > 255 || entry == "." ||
        entry == ".." || entry.find('/') != std::string::npos ||
        entry.find('\\') != std::string::npos ||
        entry.find(':') != std::string::npos ||
        entry.find('\0') != std::string::npos) {
        return false;
    }
    return entry.find("..") == std::string::npos;
}

enum class ReadOutcome { kOk, kMissing, kFailed };

// Platform stat-timestamp accessors for the post-read identity re-check.
#if defined(__APPLE__)
#define CAPSID_LOCAL_MTIME_SEC(st) ((st).st_mtimespec.tv_sec)
#define CAPSID_LOCAL_MTIME_NSEC(st) ((st).st_mtimespec.tv_nsec)
#define CAPSID_LOCAL_CTIME_SEC(st) ((st).st_ctimespec.tv_sec)
#define CAPSID_LOCAL_CTIME_NSEC(st) ((st).st_ctimespec.tv_nsec)
#elif defined(_WIN32)
#define CAPSID_LOCAL_MTIME_SEC(st) ((st).st_mtime)
#define CAPSID_LOCAL_MTIME_NSEC(st) 0
#define CAPSID_LOCAL_CTIME_SEC(st) ((st).st_ctime)
#define CAPSID_LOCAL_CTIME_NSEC(st) 0
#else
#define CAPSID_LOCAL_MTIME_SEC(st) ((st).st_mtim.tv_sec)
#define CAPSID_LOCAL_MTIME_NSEC(st) ((st).st_mtim.tv_nsec)
#define CAPSID_LOCAL_CTIME_SEC(st) ((st).st_ctim.tv_sec)
#define CAPSID_LOCAL_CTIME_NSEC(st) ((st).st_ctim.tv_nsec)
#endif

template <typename Stat>
bool same_identity(const Stat& before, const Stat& after) {
    return before.st_dev == after.st_dev && before.st_ino == after.st_ino &&
           before.st_size == after.st_size &&
           CAPSID_LOCAL_MTIME_SEC(before) == CAPSID_LOCAL_MTIME_SEC(after) &&
           CAPSID_LOCAL_MTIME_NSEC(before) ==
               CAPSID_LOCAL_MTIME_NSEC(after) &&
           CAPSID_LOCAL_CTIME_SEC(before) == CAPSID_LOCAL_CTIME_SEC(after) &&
           CAPSID_LOCAL_CTIME_NSEC(before) ==
               CAPSID_LOCAL_CTIME_NSEC(after);
}

// Reads the file through a descriptor opened without following symlinks:
// the policy document is the authority for what the worker may do, so the
// loader must not be redirected by a reparse point/symlink planted at the
// default ./capsid.json path, and a swap while reading is rejected by an
// identity re-check. On POSIX the file must also be owned by the invoking
// user. The managed 1 MiB document cap still applies. A missing file is
// distinguished from every other failure: the default ./capsid.json is
// allowed to be absent, an explicit --capsid-json is not.
ReadOutcome read_local_config_file(const std::string& path,
                                   std::vector<std::uint8_t>* out,
                                   std::string* error) {
    int fd = -1;
#if defined(_WIN32)
    // UTF-8 -> UTF-16 for CreateFileW, opened with
    // FILE_FLAG_OPEN_REPARSE_POINT so a symlink/junction is inspected
    // (and rejected) instead of followed.
    const int wide_size = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, path.c_str(), -1, nullptr, 0);
    if (wide_size <= 0) {
        *error = "cannot encode " + path;
        return ReadOutcome::kFailed;
    }
    std::wstring wide_path(static_cast<std::size_t>(wide_size - 1), L'\0');
    if (MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, path.c_str(), -1,
            &wide_path[0], wide_size) <= 0) {
        *error = "cannot encode " + path;
        return ReadOutcome::kFailed;
    }
    const HANDLE handle = CreateFileW(
        wide_path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const DWORD saved = GetLastError();
        if (saved == ERROR_FILE_NOT_FOUND || saved == ERROR_PATH_NOT_FOUND) {
            return ReadOutcome::kMissing;
        }
        *error = "cannot open " + path;
        return ReadOutcome::kFailed;
    }
    FILE_ATTRIBUTE_TAG_INFO attributes = {};
    if (!GetFileInformationByHandleEx(
            handle, FileAttributeTagInfo, &attributes,
            sizeof(attributes))) {
        CloseHandle(handle);
        *error = "cannot inspect " + path;
        return ReadOutcome::kFailed;
    }
    if ((attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        CloseHandle(handle);
        *error = path + " is not a regular file";
        return ReadOutcome::kFailed;
    }
    fd = _open_osfhandle(
        reinterpret_cast<intptr_t>(handle), _O_RDONLY | _O_BINARY);
    if (fd < 0) {
        CloseHandle(handle);
        *error = "cannot open " + path;
        return ReadOutcome::kFailed;
    }
#else
    // O_NONBLOCK: a FIFO planted at the path must not block the host;
    // O_NOFOLLOW: a symlink is rejected instead of redirecting the read.
    fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) {
        if (errno == ENOENT || errno == ENOTDIR) {
            return ReadOutcome::kMissing;
        }
        *error = (errno == ELOOP)
                     ? path + " is not a regular file"
                     : "cannot open " + path;
        return ReadOutcome::kFailed;
    }
#endif

#if defined(_WIN32)
    struct _stat64 before = {};
    const bool stat_ok = _fstat64(fd, &before) == 0;
    const bool regular = (before.st_mode & _S_IFREG) != 0;
#else
    struct stat before = {};
    const bool stat_ok = fstat(fd, &before) == 0;
    const bool regular = S_ISREG(before.st_mode);
    const bool owned = stat_ok && before.st_uid == geteuid();
#endif
    if (!stat_ok) {
        close(fd);
        *error = "cannot stat " + path;
        return ReadOutcome::kFailed;
    }
    if (!regular) {
        close(fd);
        *error = path + " is not a regular file";
        return ReadOutcome::kFailed;
    }
#if !defined(_WIN32)
    if (!owned) {
        close(fd);
        *error = path + " is not owned by the current user";
        return ReadOutcome::kFailed;
    }
#endif
    if (before.st_size < 0 ||
        static_cast<std::uint64_t>(before.st_size) >
            kMaxLocalCapsidJsonBytes) {
        close(fd);
        *error = path + " exceeds the 1 MiB capsid.json cap";
        return ReadOutcome::kFailed;
    }

    out->resize(static_cast<std::size_t>(before.st_size));
    std::size_t offset = 0;
    while (offset < out->size()) {
#if defined(_WIN32)
        // MSVC read() takes an unsigned int count; the 1 MiB cap keeps the
        // cast lossless.
        const ssize_t count = read(
            fd, out->data() + offset,
            static_cast<unsigned int>(out->size() - offset));
#else
        const ssize_t count = read(
            fd, out->data() + offset, out->size() - offset);
#endif
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            close(fd);
            *error = "cannot read " + path;
            return ReadOutcome::kFailed;
        }
        if (count == 0) {
            break;  // truncated mid-read; the identity check below rejects it
        }
        offset += static_cast<std::size_t>(count);
    }
    if (offset != out->size()) {
        close(fd);
        *error = path + " changed while being read";
        return ReadOutcome::kFailed;
    }

#if defined(_WIN32)
    struct _stat64 after = {};
    const bool recheck_ok = _fstat64(fd, &after) == 0;
#else
    struct stat after = {};
    const bool recheck_ok = fstat(fd, &after) == 0;
#endif
    close(fd);
    if (!recheck_ok || !same_identity(before, after)) {
        *error = path + " changed while being read";
        return ReadOutcome::kFailed;
    }
    return ReadOutcome::kOk;
}

// In local mode the document is its own authority: the intersection host
// mirrors the App request exactly, so compile_policy passes every request
// through while still performing the full normalization, grammar and
// ordering pipeline (rule ids, digests, canonical effective config).
HostPolicy permissive_host(const AppRequest& app) {
    HostPolicy host;
    host.module_allowlist = app.modules;
    for (const AppRequest::EnvRequest& request : app.env) {
        // Exact-name patterns: the frozen env grammar already ran.
        host.env_patterns.push_back(request.name);
    }
    host.fs_read_roots = app.fs_read;
    host.fetch_targets = app.fetch;
    host.storage_allowed = app.storage;
    host.storage_namespaces = app.storage_namespaces;
    host.stdio_allowed = app.stdio;
    host.stdio_streams = app.stdio_streams;
    // The single worker-count ceiling: exactly the requested pool, so the
    // pool check passes (local mode never imposes a separate ceiling).
    host.max_workers = app.workers;
    return host;
}

}  // namespace

bool load_local_capsid_policy(const std::string& path,
                              bool required,
                              const BindingRegistrySnapshot* binding_registry,
                              const std::string& secrets_root,
                              LocalCapsidPolicy* out,
                              std::string* error) {
    if (out == nullptr || error == nullptr) {
        return false;
    }
    out->present = false;
    out->policy = RuntimePolicy();
    out->bindings.clear();
    std::vector<std::uint8_t> bytes;
    const ReadOutcome outcome = read_local_config_file(path, &bytes, error);
    if (outcome == ReadOutcome::kMissing) {
        // A missing default file is the pre-v0.1.3 no-policy case; a
        // missing explicit --capsid-json is an operator error.
        if (!required) {
            return true;
        }
        *error = "cannot find " + path;
        return false;
    }
    if (outcome != ReadOutcome::kOk) {
        // The reader already set *error for every other failure.
        return false;
    }

    // 1. The frozen capsid/app-v1 schema boundary first: unknown fields,
    // duplicates, shapes, limits and the apiVersion contract.
    const std::string text(bytes.begin(), bytes.end());
    const ConfigValidationResult validated =
        validate_config_json(ConfigDocument::kApplication, text);
    if (!validated.ok) {
        *error = path + " rejected at " + validated.error.path + ": " +
                 validated.error.message;
        return false;
    }

    // 2. Non-permission fields this path now honors (v0.2.x): entry and
    // request.timeout live outside AppRequest and are read here; the
    // worker/request/pool/healthCheck sections are extracted from the
    // shared AppRequest parse in step 3. An un-honored shape still fails
    // loudly instead of being silently ignored.
    {
        json_error_t parse_error;
        json_t* root = json_loadb(
            reinterpret_cast<const char*>(bytes.data()), bytes.size(),
            JSON_REJECT_DUPLICATES, &parse_error);
        if (root == nullptr || !json_is_object(root)) {
            if (root) {
                json_decref(root);
            }
            *error = path + ": invalid capsid.json";
            return false;
        }
        json_t* entry = json_object_get(root, "entry");
        if (json_is_string(entry)) {
            const std::string value = json_string_value(entry);
            if (!valid_local_entry_name(value)) {
                *error = path + ": entry must be a plain file name "
                         "inside the capsid.json directory";
                json_decref(root);
                return false;
            }
            out->settings.has_entry = true;
            out->settings.entry = value;
        }
        json_t* request = json_object_get(root, "request");
        if (request != nullptr && json_is_object(request)) {
            json_t* timeout = json_object_get(request, "timeout");
            if (json_is_string(timeout)) {
                std::uint64_t timeout_ms = 0;
                if (!parse_duration_ms(json_string_value(timeout),
                                       &timeout_ms) ||
                    timeout_ms == 0) {
                    *error = path + ": invalid capsid.json request.timeout";
                    json_decref(root);
                    return false;
                }
                out->settings.has_request_timeout_ms = true;
                out->settings.request_timeout_ms = timeout_ms;
            }
        }
        // Queue presence: 0 is a meaningful value ("queueing disabled")
        // for the queue fields, so the document presence decides whether
        // the CLI or the document owns the knob.
        json_t* pool = json_object_get(root, "pool");
        if (pool != nullptr && json_is_object(pool)) {
            out->settings.has_queue_requests =
                json_object_get(pool, "queueRequests") != nullptr;
            out->settings.has_queue_header_bytes =
                json_object_get(pool, "queueHeaderBytes") != nullptr;
            out->settings.has_queue_timeout_ms =
                json_object_get(pool, "queueTimeout") != nullptr;
        }
        json_decref(root);
    }

    // 3. Map the validated document onto the App request through the same
    // parser managed mode uses.
    AppRequest app;
    std::string parse_error;
    if (!parse_app_request(bytes, &app, &parse_error)) {
        *error = path + ": " + parse_error;
        return false;
    }

    // 3b. Extract the honored runtime settings from the shared parse. The
    // request-window fields use the schema's 0-unset convention; the queue
    // fields carry their presence from step 2 because 0 means "queueing
    // disabled" there.
    {
        out->settings.max_inflight_per_worker = app.requests_per_worker;
        out->settings.has_max_inflight = app.requests_per_worker > 0;
        out->settings.max_streaming_inflight_per_worker =
            app.max_streaming_inflight_per_worker;
        out->settings.has_max_streaming_inflight =
            app.max_streaming_inflight_per_worker > 0;
        out->settings.stream_idle_timeout_ms = app.stream_idle_timeout_ms;
        out->settings.has_stream_idle_timeout_ms =
            app.stream_idle_timeout_ms > 0;
        out->settings.write_timeout_ms = app.write_timeout_ms;
        out->settings.has_write_timeout_ms = app.write_timeout_ms > 0;
        out->settings.queue_requests = app.queue_requests;
        out->settings.queue_header_bytes = app.queue_header_bytes;
        out->settings.queue_timeout_ms = app.queue_timeout_ms;
        out->settings.js_heap_bytes = app.js_heap_bytes;
        out->settings.process_address_bytes = app.process_address_bytes;
        out->settings.memory_bytes = app.memory_bytes;
        out->settings.file_descriptors = app.file_descriptors;
        out->settings.health_check = app.health_check;
        // worker.memoryMax is budget accounting on the managed path; the
        // local data plane has no per-worker cgroup budget to apply it
        // to, so accepting it would silently drop the capacity the
        // operator asked for.  Fail closed instead.
        if (app.memory_bytes > 0) {
            *error = path +
                     ": worker.memoryMax is not applicable in local mode";
            return false;
        }
    }

    // 4. env valueFrom resolves against an explicit --secrets-root: one
    // regular file per key id (the managed layout). Without a root the
    // entry is rejected — there is no implicit secret store on this path.
    if (!secrets_root.empty()) {
        for (AppRequest::EnvRequest& request : app.env) {
            if (!request.from_secret) {
                continue;
            }
            if (!valid_local_secret_key_id(request.secret_key_id)) {
                *error = path + ": invalid env valueFrom key id";
                return false;
            }
            std::vector<std::uint8_t> secret_bytes;
            const ReadOutcome secret_outcome = read_local_config_file(
                secrets_root + "/" + request.secret_key_id, &secret_bytes,
                error);
            if (secret_outcome != ReadOutcome::kOk) {
                *error = path + ": env valueFrom \"" +
                         request.secret_key_id + "\": " + *error;
                return false;
            }
            request.literal.assign(
                reinterpret_cast<const char*>(secret_bytes.data()),
                secret_bytes.size());
            request.from_secret = false;
        }
    } else {
        for (const AppRequest::EnvRequest& request : app.env) {
            if (request.from_secret) {
                *error = path + ": env valueFrom is unavailable in local "
                                "mode without --secrets-root";
                return false;
            }
        }
    }

    // 5. Local Binding development uses the production declaration and
    // compile path. The Registry remains Host-controlled and explicit;
    // declaring a Binding without --bindings-root fails closed. Binding
    // secrets need a provider and therefore cannot be honored by these
    // provider-less local modes.
    std::vector<AppBindingRequest> binding_requests;
    if (!parse_app_bindings(bytes, &binding_requests, &parse_error)) {
        *error = path + ": " + parse_error;
        return false;
    }
    if (!binding_requests.empty()) {
        if (binding_registry == nullptr) {
            *error = path +
                     ": bindings require an explicit Host Registry "
                     "(--bindings-root)";
            return false;
        }
        for (const AppBindingRequest& request : binding_requests) {
            if (!request.secrets.empty()) {
                *error = path + ": binding secrets are unavailable in "
                                "local mode (no secret provider): " +
                         request.id;
                return false;
            }
        }
        BindingCompileResult binding_result =
            compile_effective_bindings(*binding_registry, binding_requests);
        if (!binding_result.ok) {
            *error = path + ": " + binding_result.error;
            return false;
        }
        out->bindings = std::move(binding_result.bindings);
    }

    // 6. Compile through the authoritative compiler (normalization, rule
    // ids, digests, canonical effective config) with the permissive host.
    std::vector<EffectiveEnvEntry> resolved;
    resolved.reserve(app.env.size());
    for (const AppRequest::EnvRequest& request : app.env) {
        EffectiveEnvEntry entry;
        entry.name = request.name;
        entry.literal = request.literal;
        resolved.push_back(entry);
    }
    const PolicyCompileResult compiled =
        compile_policy(permissive_host(app), app, resolved);
    if (!compiled.ok) {
        *error = path + ": " + compiled.error;
        return false;
    }

    // 7. Runtime descriptors (capability + egress structs) for the spawn.
    std::vector<std::pair<std::string, std::string>> env_values;
    env_values.reserve(compiled.effective.env.size());
    for (const EffectiveEnvEntry& entry : compiled.effective.env) {
        env_values.emplace_back(entry.name, entry.literal);
    }
    out->policy = RuntimePolicy();
    if (!build_runtime_policy(compiled.effective, env_values, &out->policy,
                              error)) {
        *error = path + ": " + *error;
        return false;
    }
    out->present = true;
    return true;
}

}  // namespace capsid::host
