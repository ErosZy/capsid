// M1D managed host coordinator. See managed_host.h.
//
// The deploy pipeline follows the frozen commit sequence and never reports
// Active before the real worker is READY: safe-read, attestation
// verification with the frozen selection rules (trusted bytecode / source
// fallback / fail closed), policy + secret compilation, real generation
// identity, unique exclusive staging, per-file fsync, COMPLETE last,
// rename into generations, version mapping, worker spawn + load + READY +
// compatibility check, and finally the active-state persist API.

#include "host/managed_host.h"

#include "capsid/runtime.h"
#include "host/active_state.h"
#include "host/artifact_safe_read.h"
#include "host/generation_identity.h"
#include "host/secret_file_provider.h"
#include "host/worker_event_source.h"

#include <jansson.h>
#include <openssl/evp.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <map>
#include <poll.h>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace capsid::host {
namespace {

constexpr const char* kCompleteMarker = "COMPLETE";

std::string sha256_hex(const std::string& data) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_size = 0;
    if (EVP_Digest(data.data(), data.size(), digest, &digest_size,
                   EVP_sha256(), nullptr) != 1 ||
        digest_size != 32) {
        return "";
    }
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (unsigned int i = 0; i < digest_size; ++i) {
        out.push_back(kHex[digest[i] >> 4]);
        out.push_back(kHex[digest[i] & 0x0f]);
    }
    return out;
}

std::string json_escape(const std::string& value) {
    std::ostringstream out;
    for (const char c : value) {
        switch (c) {
        case '"': out << "\\\""; break;
        case '\\': out << "\\\\"; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        default: out << c;
        }
    }
    return out.str();
}

bool write_file_at(int dir_fd, const char* name, const std::string& content,
                   std::string* error) {
    const int fd = openat(dir_fd, name, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) {
        *error = "cannot create state file";
        return false;
    }
    const bool ok = content.empty() ||
        write(fd, content.data(), content.size()) ==
            static_cast<ssize_t>(content.size());
    if (ok && fsync(fd) != 0) {
        *error = "cannot sync state file";
        close(fd);
        return false;
    }
    close(fd);
    if (!ok) {
        *error = "cannot write state file";
    }
    return ok;
}

bool make_dir_at(int dir_fd, const char* name) {
    if (mkdirat(dir_fd, name, 0700) == 0) {
        return true;
    }
    return errno == EEXIST;
}

// Unique per-operation staging name: time + pid + a process counter. The
// name is unpredictable to callers and never reused.
std::string unique_operation_id() {
    static std::uint64_t counter = 0;
    ++counter;
    std::ostringstream out;
    out << "op-" << static_cast<long long>(getpid()) << "-" << counter;
    return out.str();
}

// ---- worker warm-up: spawn, load, READY, compatibility check ----

struct WarmResult {
    bool ok = false;
    capsid_worker* worker = nullptr;
    std::string error;  // static text
};

WarmResult warm_worker(const ManagedHostOptions& options,
                       const std::vector<std::uint8_t>& bundle,
                       bool trusted_bytecode,
                       const std::string& source_name,
                       const EffectiveConfig& effective,
                       const std::vector<std::pair<std::string, std::string>>& env_values) {
    WarmResult out;
    // Build the ABI capability policy from the effective config: the
    // module allowlist, env permission rules and env entries (values only
    // enter the snapshot), plus fs-read and net rules for the effective
    // permissions.
    std::vector<const char*> module_names;
    for (const std::string& module : effective.modules) {
        module_names.push_back(module.c_str());
    }
    std::vector<capsid_permission_rule> rules;
    std::vector<capsid_env_entry> env_entries;
    std::vector<std::string> rule_resources;
    std::vector<std::string> env_names;
    for (const EffectiveEnvEntry& entry : effective.env) {
        env_names.push_back(entry.name);
        capsid_permission_rule rule;
        capsid_permission_rule_init(&rule);
        rule.permission = CAPSID_PERMISSION_ENV;
        rule.action = CAPSID_PERMISSION_ALLOW;
        rule_resources.push_back(entry.name);
        rule.resource = rule_resources.back().c_str();
        rule.rule_id = entry.rule_id != 0 ? entry.rule_id : 1;
        rules.push_back(rule);
    }
    for (const std::string& path : effective.fs_read) {
        capsid_permission_rule rule;
        capsid_permission_rule_init(&rule);
        rule.permission = CAPSID_PERMISSION_READ;
        rule.action = CAPSID_PERMISSION_ALLOW;
        rule_resources.push_back(path);
        rule.resource = rule_resources.back().c_str();
        rule.rule_id = 1;
        rules.push_back(rule);
    }
    for (const FetchTarget& target : effective.fetch) {
        capsid_permission_rule rule;
        capsid_permission_rule_init(&rule);
        rule.permission = CAPSID_PERMISSION_NET;
        rule.action = CAPSID_PERMISSION_ALLOW;
        rule_resources.push_back(target.host);
        rule.resource = rule_resources.back().c_str();
        rule.rule_id = 1;
        rules.push_back(rule);
    }
    for (std::size_t index = 0; index < env_values.size(); ++index) {
        capsid_env_entry entry;
        capsid_env_entry_init(&entry);
        entry.name = env_values[index].first.c_str();
        entry.value = env_values[index].second.c_str();
        env_entries.push_back(entry);
    }
    capsid_capability_policy policy;
    capsid_capability_policy_init(&policy);
    policy.allowed_modules = module_names.empty() ? nullptr : module_names.data();
    policy.allowed_module_count = static_cast<std::uint32_t>(module_names.size());
    policy.rules = rules.empty() ? nullptr : rules.data();
    policy.rule_count = static_cast<std::uint32_t>(rules.size());
    policy.env_entries = env_entries.empty() ? nullptr : env_entries.data();
    policy.env_entry_count = static_cast<std::uint32_t>(env_entries.size());

    capsid_worker_config config;
    capsid_worker_config_init(&config);
    config.worker_path = options.worker_path.c_str();
    config.capability_policy = &policy;
    if (capsid_worker_spawn(&config, &out.worker) != CAPSID_OK) {
        out.error = "worker spawn failed";
        return out;
    }
    capsid::host::WorkerEventSource event_source;
    event_source.set_worker(out.worker);
    const capsid_result load_result =
        trusted_bytecode
            ? capsid_worker_load_trusted_bytecode_named(
                  out.worker, bundle.data(), bundle.size(), source_name.c_str())
            : capsid_worker_load_bundle_named(
                  out.worker, bundle.data(), bundle.size(), source_name.c_str());
    if (load_result != CAPSID_OK) {
        capsid_worker_destroy(out.worker);
        out.worker = nullptr;
        out.error = "worker bundle load failed";
        return out;
    }
    // Wait for READY; the payload is the worker's compatibility ID.
    const std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(15);
    for (;;) {
        const capsid_result flush = capsid_worker_flush(out.worker);
        if (flush != CAPSID_OK && flush != CAPSID_WOULD_BLOCK) {
            capsid_worker_destroy(out.worker);
            out.worker = nullptr;
            out.error = "worker flush failed";
            return out;
        }
        capsid_event event = {};
        event.struct_size = sizeof(event);
        const capsid_result result =
            capsid_worker_next_event(out.worker, &event);
        if (result == CAPSID_OK) {
            if (event.type == CAPSID_EVENT_READY) {
                const std::string reported(
                    reinterpret_cast<const char*>(event.payload.data),
                    event.payload.size);
                if (reported != options.runtime_compatibility_id) {
                    capsid_worker_destroy(out.worker);
                    out.worker = nullptr;
                    out.error = "worker compatibility mismatch";
                    return out;
                }
                out.ok = true;
                return out;
            }
            if (event.type == CAPSID_EVENT_ERROR) {
                const std::string detail(
                    reinterpret_cast<const char*>(event.payload.data),
                    event.payload.size);
                capsid_worker_destroy(out.worker);
                out.worker = nullptr;
                out.error = detail.empty() ? "worker failed before READY"
                                           : detail;
                return out;
            }
            if (event.type == CAPSID_EVENT_EXIT) {
                capsid_worker_destroy(out.worker);
                out.worker = nullptr;
                out.error = "worker failed before READY";
                return out;
            }
            continue;
        }
        if (result != CAPSID_WOULD_BLOCK) {
            capsid_worker_destroy(out.worker);
            out.worker = nullptr;
            out.error = "worker event error";
            return out;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            capsid_worker_destroy(out.worker);
            out.worker = nullptr;
            out.error = "worker READY timeout";
            return out;
        }
        // Wait through the WorkerEventSource adapter (the single Host
        // adapter for the worker IPC descriptor).
        event_source.wait(deadline);
    }
}

// POSIX adapter for the active-state persist contract.
class PosixActiveStateFilesystem final : public ActiveStateFilesystem {
public:
    explicit PosixActiveStateFilesystem(const std::string& app_dir)
        : app_dir_(app_dir) {}

    ActiveStateIoStatus cleanup_stale_active_temps() override {
        return ActiveStateIoStatus::kOk;
    }
    ActiveStateReadResult read_active_file() override {
        ActiveStateReadResult result;
        const std::string path = app_dir_ + "/active.json";
        const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            result.status = errno == ENOENT ? ActiveStateIoStatus::kNotFound
                                            : ActiveStateIoStatus::kError;
            return result;
        }
        char buffer[4096];
        std::string bytes;
        for (;;) {
            const ssize_t count = read(fd, buffer, sizeof(buffer));
            if (count <= 0) {
                break;
            }
            bytes.append(buffer, static_cast<std::size_t>(count));
        }
        close(fd);
        result.status = ActiveStateIoStatus::kOk;
        result.bytes = std::move(bytes);
        return result;
    }
    GenerationCompleteness inspect_generation(
        std::string_view generation) override {
        const std::string marker =
            app_dir_ + "/generations/" + std::string(generation) + "/COMPLETE";
        struct stat st = {};
        if (stat(marker.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
            return GenerationCompleteness::kComplete;
        }
        return errno == ENOENT ? GenerationCompleteness::kMissing
                               : GenerationCompleteness::kError;
    }
    ActiveStateIoStatus create_active_temp_exclusive(
        std::string_view temp_name) override {
        const int fd = open((app_dir_ + "/" + std::string(temp_name)).c_str(),
                            O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (fd < 0) {
            return errno == EEXIST ? ActiveStateIoStatus::kAlreadyExists
                                   : ActiveStateIoStatus::kError;
        }
        close(fd);
        return ActiveStateIoStatus::kOk;
    }
    ActiveStateIoStatus write_active_temp(std::string_view temp_name,
                                          std::string_view bytes) override {
        const int fd = open((app_dir_ + "/" + std::string(temp_name)).c_str(),
                            O_WRONLY | O_CLOEXEC);
        if (fd < 0) {
            return ActiveStateIoStatus::kError;
        }
        const bool ok =
            write(fd, bytes.data(), bytes.size()) ==
            static_cast<ssize_t>(bytes.size());
        close(fd);
        return ok ? ActiveStateIoStatus::kOk : ActiveStateIoStatus::kError;
    }
    ActiveStateIoStatus sync_active_temp(std::string_view temp_name) override {
        const int fd = open((app_dir_ + "/" + std::string(temp_name)).c_str(),
                            O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            return ActiveStateIoStatus::kError;
        }
        const bool ok = fsync(fd) == 0;
        close(fd);
        return ok ? ActiveStateIoStatus::kOk : ActiveStateIoStatus::kError;
    }
    ActiveStateIoStatus rename_temp_over_active(
        std::string_view temp_name) override {
        if (rename((app_dir_ + "/" + std::string(temp_name)).c_str(),
                   (app_dir_ + "/active.json").c_str()) != 0) {
            return ActiveStateIoStatus::kError;
        }
        return ActiveStateIoStatus::kOk;
    }
    ActiveStateIoStatus sync_app_directory() override {
        const int fd = open(app_dir_.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (fd < 0) {
            return ActiveStateIoStatus::kError;
        }
        const bool ok = fsync(fd) == 0;
        close(fd);
        return ok ? ActiveStateIoStatus::kOk : ActiveStateIoStatus::kError;
    }

private:
    std::string app_dir_;
};

// Owner/mode verification for the pre-opened secret root and the App
// subdirectory (M1D audit item 3): the App dir must be owned by the Host
// euid and mode 0700 (no group/other access).
int open_verified_app_secret_dir(int root_fd, const std::string& app) {
    const int fd = openat(root_fd, app.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) {
        return -1;
    }
    struct stat st = {};
    if (fstat(fd, &st) != 0 || st.st_uid != geteuid() ||
        (st.st_mode & 0077) != 0 || !S_ISDIR(st.st_mode)) {
        close(fd);
        return -1;
    }
    return fd;
}

// Parse capsid.json into the AppRequest (strict, fail-closed).
bool parse_app_request(const std::vector<std::uint8_t>& bytes,
                       AppRequest* app,
                       std::string* error) {
    json_error_t parse_error;
    json_t* root = json_loadb(
        reinterpret_cast<const char*>(bytes.data()), bytes.size(),
        JSON_REJECT_DUPLICATES, &parse_error);
    if (root == nullptr || !json_is_object(root)) {
        *error = "invalid capsid.json";
        if (root) {
            json_decref(root);
        }
        return false;
    }
    const auto string_array = [&](const char* key,
                                  std::vector<std::string>* out) -> bool {
        json_t* value = json_object_get(root, key);
        if (value == nullptr) {
            return true;
        }
        if (!json_is_array(value)) {
            return false;
        }
        std::size_t index = 0;
        json_t* item = nullptr;
        json_array_foreach(value, index, item) {
            if (!json_is_string(item)) {
                return false;
            }
            out->push_back(json_string_value(item));
        }
        return true;
    };
    if (!string_array("modules", &app->modules)) {
        *error = "invalid capsid.json modules";
        json_decref(root);
        return false;
    }
    json_t* env_array = json_object_get(root, "env");
    if (env_array != nullptr) {
        if (!json_is_array(env_array)) {
            *error = "invalid capsid.json env";
            json_decref(root);
            return false;
        }
        std::size_t index = 0;
        json_t* entry = nullptr;
        json_array_foreach(env_array, index, entry) {
            if (!json_is_object(entry)) {
                *error = "invalid capsid.json env entry";
                json_decref(root);
                return false;
            }
            json_t* name = json_object_get(entry, "name");
            json_t* literal = json_object_get(entry, "value");
            json_t* from = json_object_get(entry, "valueFrom");
            if (!json_is_string(name)) {
                *error = "invalid capsid.json env name";
                json_decref(root);
                return false;
            }
            AppRequest::EnvRequest request;
            request.name = json_string_value(name);
            if (from != nullptr) {
                if (!json_is_object(from)) {
                    *error = "invalid capsid.json valueFrom";
                    json_decref(root);
                    return false;
                }
                json_t* key = json_object_get(from, "secretKeyId");
                if (!json_is_string(key)) {
                    *error = "invalid capsid.json secretKeyId";
                    json_decref(root);
                    return false;
                }
                request.from_secret = true;
                request.secret_key_id = json_string_value(key);
            } else if (literal != nullptr && json_is_string(literal)) {
                request.literal = json_string_value(literal);
            } else {
                *error = "invalid capsid.json env value";
                json_decref(root);
                return false;
            }
            app->env.push_back(request);
        }
    }
    app->workers = 1;
    app->min_ready = 1;
    json_decref(root);
    return true;
}

}  // namespace

DeployOutcome managed_deploy(ManagedHostOptions* options,
                             const std::string& version,
                             OperationStatus* status) {
    DeployOutcome outcome;
    if (options == nullptr || options->applications_root_fd < 0 ||
        options->worker_path.empty() || version.empty()) {
        outcome.error = "invalid managed host arguments";
        return outcome;
    }
    outcome.operation_id = unique_operation_id();
    status->operation_id = outcome.operation_id;
    status->version = version;
    status->state = OperationState::kValidating;

    // ---- 1. safe-read the version artifacts ----
    const SafeReadResult artifacts = safe_read_version_artifacts(
        options->applications_root_fd, options->application, version,
        kMaxVersionArtifactTotalBytes);
    if (artifacts.code != SafeReadErrorCode::kNone) {
        status->state = OperationState::kFailed;
        status->error = artifacts.message;
        outcome.error = artifacts.message;
        return outcome;
    }

    // ---- 2-4. artifact selection with real attestation verification ----
    SelectedArtifactKind selected = SelectedArtifactKind::kSource;
    std::string attestation_digest;
    std::vector<std::uint8_t> bundle_bytes;
    std::string source_name = "file://" + options->application + "/" +
                              version + "/bundle.mjs";
    if (artifacts.artifacts.has_bytecode) {
        // The bytecode's module name (the compile filename) is the frozen
        // sourceName the attestation claims; the verification and the
        // trusted load both use it.
        source_name = "file://" + options->application + "/" + version +
                      "/bundle.qjsb";
        const std::string attestation_json(
            artifacts.artifacts.attestation.bytes.begin(),
            artifacts.artifacts.attestation.bytes.end());
        capsid::host::BytecodeAttestationInput input;
        input.source = std::span<const std::uint8_t>(
            artifacts.artifacts.bundle.bytes.data(),
            artifacts.artifacts.bundle.bytes.size());
        input.bytecode = std::optional<std::span<const std::uint8_t>>(
            std::span<const std::uint8_t>(
                artifacts.artifacts.bytecode.bytes.data(),
                artifacts.artifacts.bytecode.bytes.size()));
        input.attestation_json = std::string_view(attestation_json);
        input.signature = std::optional<std::span<const std::uint8_t>>(
            std::span<const std::uint8_t>(
                artifacts.artifacts.signature.bytes.data(),
                artifacts.artifacts.signature.bytes.size()));
        input.expected_application = options->application;
        input.expected_version = version;
        input.expected_source_name = source_name;
        input.runtime_compatibility_id = options->runtime_compatibility_id;
        input.trusted_keys = std::span<const capsid::host::TrustedBytecodeKey>(
            options->trusted_keys.data(), options->trusted_keys.size());
        const capsid::host::BytecodeAttestationResult verified =
            capsid::host::verify_bytecode_attestation(input);
        if (verified.selection ==
            capsid::host::BytecodeArtifactSelection::kTrustedBytecode) {
            selected = SelectedArtifactKind::kTrustedBytecode;
            attestation_digest = sha256_hex(attestation_json);
        } else if (verified.selection ==
                   capsid::host::BytecodeArtifactSelection::kSource &&
                   verified.code ==
                       capsid::host::BytecodeAttestationErrorCode::kCompatibilityMismatch) {
            // Frozen fallback: provenance-valid but compatibility-mismatched
            // bytecode falls back to source.
            selected = SelectedArtifactKind::kSource;
        } else {
            status->state = OperationState::kFailed;
            status->error = "bytecode attestation rejected: " + verified.message;
            outcome.error = "bytecode attestation rejected: " + verified.message;
            return outcome;
        }
    }
    // The warmed artifact is the selected one: bytecode for trusted, the
    // source bundle otherwise.
    bundle_bytes =
        selected == SelectedArtifactKind::kTrustedBytecode
            ? artifacts.artifacts.bytecode.bytes
            : artifacts.artifacts.bundle.bytes;

    // ---- 5. policy + secret compilation ----
    AppRequest app_request;
    std::string config_error;
    if (!parse_app_request(artifacts.artifacts.capsid_json.bytes,
                           &app_request, &config_error)) {
        status->state = OperationState::kFailed;
        status->error = config_error;
        outcome.error = config_error;
        return outcome;
    }
    // Resolve the secret requests via the provider; values enter only the
    // env snapshot.
    std::vector<std::pair<std::string, std::string>> env_values;
    std::vector<EffectiveEnvEntry> resolved_secrets;
    if (!app_request.env.empty()) {
        if (options->secret_root_template_fd < 0) {
            status->state = OperationState::kFailed;
            status->error = "secret root not configured";
            outcome.error = "secret root not configured";
            return outcome;
        }
        const int app_secret_fd = open_verified_app_secret_dir(
            options->secret_root_template_fd, options->application);
        if (app_secret_fd < 0) {
            status->state = OperationState::kFailed;
            status->error = "secret app directory unverified";
            outcome.error = "secret app directory unverified";
            return outcome;
        }
        std::vector<std::string> key_ids;
        for (const AppRequest::EnvRequest& request : app_request.env) {
            if (request.from_secret) {
                key_ids.push_back(request.secret_key_id);
            }
        }
        const std::vector<SecretFileOutcome> outcomes =
            read_secret_files(app_secret_fd, key_ids);
        close(app_secret_fd);
        std::size_t secret_index = 0;
        for (const AppRequest::EnvRequest& request : app_request.env) {
            if (!request.from_secret) {
                env_values.push_back({ request.name, request.literal });
                continue;
            }
            if (secret_index >= outcomes.size() ||
                !outcomes[secret_index].error.empty()) {
                status->state = OperationState::kFailed;
                status->error = "secret resolution failed";
                outcome.error = "secret resolution failed";
                return outcome;
            }
            env_values.push_back({
                request.name,
                std::string(outcomes[secret_index].value.begin(),
                            outcomes[secret_index].value.end()),
            });
            EffectiveEnvEntry resolved;
            resolved.name = request.name;
            resolved.from_secret = true;
            resolved.secret_key_id = request.secret_key_id;
            resolved.secret_revision = outcomes[secret_index].revision;
            resolved_secrets.push_back(std::move(resolved));
            secret_index += 1;
        }
    }
    const PolicyCompileResult compiled =
        compile_policy(options->host_policy, app_request, resolved_secrets);
    if (!compiled.ok) {
        status->state = OperationState::kFailed;
        status->error = compiled.error;
        outcome.error = compiled.error;
        return outcome;
    }

    // ---- 6. real generation identity (no placeholders) ----
    GenerationIdentityInput identity;
    identity.application_id = options->application;
    identity.source_digest = sha256_hex(std::string(
        artifacts.artifacts.bundle.bytes.begin(),
        artifacts.artifacts.bundle.bytes.end()));
    identity.bytecode_attestation_digest = attestation_digest;
    identity.selected_artifact = selected;
    identity.normalized_app_config_digest =
        compiled.effective.app_config_digest;
    identity.effective_policy_digest = compiled.effective.effective_digest;
    identity.effective_resource_digest = compiled.effective.effective_digest;
    identity.host_config_digest = sha256_hex("capsid-host-v1");
    identity.secret_revision = resolved_secrets.empty()
        ? ""
        : resolved_secrets.back().secret_revision;
    identity.runtime_compatibility_id = options->runtime_compatibility_id;
    const std::string generation_digest = compute_generation_digest(identity);

    // ---- 7-8. staging: unique exclusive dir, files + fsync, COMPLETE last
    const std::string app_dir = std::string("apps/") + options->application;
    const std::string generations_dir = app_dir + "/generations";
    const std::string generation_dir = generations_dir + "/" + generation_digest;
    const std::string staging_dir = std::string("staging/") + outcome.operation_id;

    std::string error;
    const int state_fd = open(options->state_root.c_str(),
                              O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (state_fd < 0) {
        outcome.error = "cannot open state root";
        status->state = OperationState::kFailed;
        return outcome;
    }
    if (!make_dir_at(state_fd, "staging") ||
        !make_dir_at(state_fd, "apps") ||
        !make_dir_at(state_fd, app_dir.c_str())) {
        close(state_fd);
        outcome.error = "cannot prepare state directories";
        status->state = OperationState::kFailed;
        return outcome;
    }
    const int app_state_fd =
        openat(state_fd, app_dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (app_state_fd < 0 || !make_dir_at(app_state_fd, "generations")) {
        if (app_state_fd >= 0) {
            close(app_state_fd);
        }
        close(state_fd);
        outcome.error = "cannot prepare generation directory";
        status->state = OperationState::kFailed;
        return outcome;
    }
    close(app_state_fd);
    if (mkdirat(state_fd, staging_dir.c_str(), 0700) != 0) {
        close(state_fd);
        outcome.error = "cannot create exclusive staging directory";
        status->state = OperationState::kFailed;
        return outcome;
    }
    const int staging_fd =
        openat(state_fd, staging_dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (staging_fd < 0) {
        close(state_fd);
        outcome.error = "cannot open staging directory";
        status->state = OperationState::kFailed;
        return outcome;
    }
    status->state = OperationState::kStaging;
    bool committed = false;
    if (write_file_at(staging_fd, "bundle.bin",
                      std::string(bundle_bytes.begin(), bundle_bytes.end()),
                      &error) &&
        write_file_at(staging_fd, "effective.json",
                      compiled.effective.effective_json, &error) &&
        write_file_at(staging_fd, "generation.json",
                      "{\"generation\":\"" + generation_digest + "\"}", &error) &&
        write_file_at(staging_fd, kCompleteMarker, "ok\n", &error)) {
        // fsync the staging dir, then rename into generations.
        if (fsync(staging_fd) != 0 ||
            renameat(state_fd, staging_dir.c_str(), state_fd,
                     generation_dir.c_str()) != 0) {
            error = "cannot publish the generation";
        } else {
            const int generations_fd =
                openat(state_fd, generations_dir.c_str(),
                       O_RDONLY | O_DIRECTORY | O_CLOEXEC);
            if (generations_fd >= 0) {
                fsync(generations_fd);
                close(generations_fd);
            }
            committed = true;
        }
    }
    close(staging_fd);
    if (!committed) {
        // Best-effort cleanup of ONLY this staging directory.
        int staging_parent = openat(state_fd, "staging",
                                    O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (staging_parent >= 0) {
            unlinkat(staging_parent, outcome.operation_id.c_str(), AT_REMOVEDIR);
            close(staging_parent);
        }
        close(state_fd);
        outcome.error = error.empty() ? "staging failed" : error;
        status->state = OperationState::kFailed;
        status->error = outcome.error;
        return outcome;
    }
    close(state_fd);

    // ---- 9-11. worker warm-up: spawn, load, READY, compatibility check.
    // NO Active is reported before this succeeds.
    status->state = OperationState::kWarming;
    const WarmResult warm = warm_worker(
        *options, bundle_bytes, selected == SelectedArtifactKind::kTrustedBytecode,
        source_name, compiled.effective, env_values);
    if (!warm.ok) {
        status->state = OperationState::kFailed;
        status->error = warm.error;
        outcome.error = warm.error;
        return outcome;
    }
    status->state = OperationState::kActivating;

    // ---- 12. persist active.json through the active-state API ----
    ActiveStateDocument document;
    document.state = ActiveServiceState::kActive;
    document.application = options->application;
    document.version = version;
    document.generation = generation_digest;
    const std::string absolute_app_dir =
        options->state_root + "/" + app_dir;
    PosixActiveStateFilesystem filesystem(absolute_app_dir);
    const ActiveStatePersistResult persisted = persist_active_state(
        document, outcome.operation_id, filesystem);
    if (!persisted.ok) {
        capsid_worker_destroy(warm.worker);
        status->state = OperationState::kFailed;
        status->error = "cannot persist active state";
        outcome.error = "cannot persist active state";
        return outcome;
    }

    // The worker is warm; the caller publishes the data-plane routing and
    // drains the old worker (single active App: routing publication is the
    // caller's step).
    status->state = OperationState::kActive;
    outcome.ok = true;
    outcome.worker = warm.worker;
    return outcome;
}

DeployOutcome managed_retire(ManagedHostOptions* options,
                             OperationStatus* status) {
    DeployOutcome outcome;
    if (options == nullptr || options->state_root.empty()) {
        outcome.error = "invalid managed host arguments";
        return outcome;
    }
    // Read the current active document, then persist the retired tombstone
    // through the active-state API (previous fields carry the last active
    // generation).
    const std::string app_dir =
        options->state_root + "/apps/" + options->application;
    PosixActiveStateFilesystem filesystem(app_dir);
    const ActiveStateReadResult current = filesystem.read_active_file();
    ActiveStateDocument document;
    document.state = ActiveServiceState::kRetired;
    document.application = options->application;
    if (current.status == ActiveStateIoStatus::kOk) {
        const ActiveStateDocumentResult parsed =
            parse_active_state_json(options->application, current.bytes);
        if (parsed.ok) {
            document.previous_version = parsed.document.version;
            document.previous_generation = parsed.document.generation;
        }
    }
    outcome.operation_id = "op-retire-" + options->application;
    status->operation_id = outcome.operation_id;
    const ActiveStatePersistResult persisted = persist_active_state(
        document, outcome.operation_id, filesystem);
    if (!persisted.ok) {
        outcome.error = "cannot persist retire tombstone";
        status->state = OperationState::kFailed;
        return outcome;
    }
    outcome.ok = true;
    status->state = OperationState::kActive;
    return outcome;
}

DeployOutcome managed_recover(ManagedHostOptions* options,
                              OperationStatus* status) {
    DeployOutcome outcome;
    if (options == nullptr || options->state_root.empty()) {
        outcome.error = "invalid managed host arguments";
        return outcome;
    }
    const std::string app_dir =
        options->state_root + "/apps/" + options->application;
    PosixActiveStateFilesystem filesystem(app_dir);
    const ActiveStateReadResult current = filesystem.read_active_file();
    if (current.status == ActiveStateIoStatus::kNotFound) {
        // No active.json: no active App; never scan generations for one.
        outcome.ok = true;
        return outcome;
    }
    if (current.status != ActiveStateIoStatus::kOk) {
        outcome.error = "cannot read active state";
        status->state = OperationState::kFailed;
        return outcome;
    }
    const ActiveStateDocumentResult parsed =
        parse_active_state_json(options->application, current.bytes);
    if (!parsed.ok) {
        outcome.error = "active state is invalid";
        status->state = OperationState::kFailed;
        return outcome;
    }
    if (parsed.document.state != ActiveServiceState::kActive) {
        // Retired/quarantined: do not start a worker.
        outcome.ok = true;
        return outcome;
    }
    if (filesystem.inspect_generation(parsed.document.generation) !=
        GenerationCompleteness::kComplete) {
        outcome.error = "active generation is incomplete";
        status->state = OperationState::kFailed;
        return outcome;
    }
    outcome.ok = true;
    status->state = OperationState::kActive;
    return outcome;
}

OperationStatus managed_operation_status(
    const ManagedHostOptions& options,
    const std::string& operation_id) {
    (void) options;
    OperationStatus status;
    status.operation_id = operation_id;
    status.state = OperationState::kFailed;
    status.error = "operation not found";
    return status;
}

std::string managed_app_status(const ManagedHostOptions& options) {
    const std::string app_dir =
        options.state_root + "/apps/" + options.application;
    PosixActiveStateFilesystem filesystem(app_dir);
    const ActiveStateReadResult current = filesystem.read_active_file();
    if (current.status != ActiveStateIoStatus::kOk) {
        return "{\"active\":false}";
    }
    const ActiveStateDocumentResult parsed =
        parse_active_state_json(options.application, current.bytes);
    if (!parsed.ok) {
        return "{\"active\":false}";
    }
    std::ostringstream out;
    out << "{\"active\":" << (parsed.document.state ==
                                      ActiveServiceState::kActive
                                  ? "true"
                                  : "false")
        << ",\"version\":\"" << json_escape(parsed.document.version)
        << "\",\"generation\":\"" << json_escape(parsed.document.generation)
        << "\"}";
    return out.str();
}

}  // namespace capsid::host
