// Binding v1 host compile pipeline. The subset proofs are static: a net
// rule is contained only by exact match, wildcard-label suffix match or
// CIDR block containment (never DNS results); fs paths by prefix; env and
// stdio by exact membership.

#include "host/binding_compile.h"

#include "host/config.h"
#include "ipc_validation.h"
#include "sandbox.h"

#include <jansson.h>

#include <algorithm>
#include <arpa/inet.h>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#if defined(__linux__)
#include <linux/landlock.h>
#include <sys/syscall.h>
#include <unistd.h>
#ifndef SYS_landlock_create_ruleset
#ifdef __NR_landlock_create_ruleset
#define SYS_landlock_create_ruleset __NR_landlock_create_ruleset
#endif
#endif
#endif

namespace capsid::host {
namespace {

uint32_t host_landlock_abi() {
#if defined(__linux__) && defined(SYS_landlock_create_ruleset)
    const int abi = static_cast<int>(syscall(
        SYS_landlock_create_ruleset,
        NULL,
        0,
        LANDLOCK_CREATE_RULESET_VERSION));
    return abi > 0 ? static_cast<uint32_t>(abi) : 0;
#else
    return 0;
#endif
}

bool fail(std::string* error, const char* message) {
    if (error != nullptr) {
        *error = message;
    }
    return false;
}

bool read_string_array(json_t* parent, const char* key,
                       std::vector<std::string>* out, std::string* error) {
    json_t* value = json_object_get(parent, key);
    if (value == nullptr) {
        return true;
    }
    if (!json_is_array(value)) {
        return fail(error, "binding permission list is not an array");
    }
    std::size_t index = 0;
    json_t* item = nullptr;
    json_array_foreach(value, index, item) {
        if (!json_is_string(item)) {
            return fail(error, "binding permission entry is not a string");
        }
        out->push_back(json_string_value(item));
    }
    return true;
}

// Static net-target containment. Returns true when `app` is provably
// covered by `manifest`; both were grammar-validated at schema time.
bool net_target_contained(const std::string& app,
                          const std::string& manifest) {
    if (app == manifest) {
        return true;
    }
    const auto split = [](const std::string& target,
                          std::string* host, std::string* port) {
        if (target.empty()) {
            return false;
        }
        if (target[0] == '[') {
            const std::size_t close = target.find(']');
            if (close == std::string::npos ||
                close + 1 >= target.size() ||
                target[close + 1] != ':') {
                return false;
            }
            *host = target.substr(1, close - 1);
            *port = target.substr(close + 2);
            return true;
        }
        const std::size_t colon = target.find(':');
        if (colon == std::string::npos ||
            target.find(':', colon + 1) != std::string::npos) {
            return false;
        }
        *host = target.substr(0, colon);
        *port = target.substr(colon + 1);
        return true;
    };
    std::string app_host;
    std::string app_port;
    std::string manifest_host;
    std::string manifest_port;
    if (!split(app, &app_host, &app_port) ||
        !split(manifest, &manifest_host, &manifest_port) ||
        app_port != manifest_port) {
        return false;
    }
    if (manifest_host == "*") {
        return app_host != "*";
    }
    if (app_host == "*") {
        return false;
    }
    // Wildcard-label suffix: app a.b.c is contained by *.b.c.
    if (manifest_host.size() >= 2 && manifest_host[0] == '*' &&
        manifest_host[1] == '.') {
        const std::string suffix = manifest_host.substr(1);  // ".b.c"
        return app_host.size() > suffix.size() &&
               app_host.compare(
                   app_host.size() - suffix.size(), suffix.size(),
                   suffix) == 0;
    }
    if (app_host.find('*') != std::string::npos ||
        manifest_host.find('*') != std::string::npos) {
        return false;
    }
    // CIDR containment: same address family, manifest prefix no wider,
    // networks equal after masking.
    const std::size_t app_slash = app_host.find('/');
    const std::size_t manifest_slash = manifest_host.find('/');
    if (app_slash == std::string::npos ||
        manifest_slash == std::string::npos) {
        return false;
    }
    const std::string app_address = app_host.substr(0, app_slash);
    const std::string manifest_address =
        manifest_host.substr(0, manifest_slash);
    const unsigned int app_prefix =
        static_cast<unsigned int>(std::stoul(app_host.substr(app_slash + 1)));
    const unsigned int manifest_prefix = static_cast<unsigned int>(
        std::stoul(manifest_host.substr(manifest_slash + 1)));
    if (manifest_prefix > app_prefix) {
        return false;
    }
    unsigned char app_bytes[16] = {};
    unsigned char manifest_bytes[16] = {};
    const bool app_v6 = app_address.find(':') != std::string::npos;
    const bool manifest_v6 =
        manifest_address.find(':') != std::string::npos;
    if (app_v6 != manifest_v6) {
        return false;
    }
    if (app_v6) {
        if (inet_pton(AF_INET6, app_address.c_str(), app_bytes) != 1 ||
            inet_pton(AF_INET6, manifest_address.c_str(),
                      manifest_bytes) != 1 ||
            manifest_prefix > 128 || app_prefix > 128) {
            return false;
        }
    } else {
        if (inet_pton(AF_INET, app_address.c_str(), app_bytes) != 1 ||
            inet_pton(AF_INET, manifest_address.c_str(),
                      manifest_bytes) != 1 ||
            manifest_prefix > 32 || app_prefix > 32) {
            return false;
        }
    }
    for (unsigned int bit = 0; bit < manifest_prefix; ++bit) {
        const unsigned int byte = bit / 8;
        const unsigned int shift = 7 - (bit % 8);
        if (((app_bytes[byte] >> shift) & 1) !=
            ((manifest_bytes[byte] >> shift) & 1)) {
            return false;
        }
    }
    return true;
}

bool fs_path_contained(const std::string& app, const std::string& manifest) {
    if (app == manifest) {
        return true;
    }
    return app.size() > manifest.size() &&
           app.compare(0, manifest.size(), manifest) == 0 &&
           app[manifest.size()] == '/';
}

bool contains_exact(const std::vector<std::string>& list,
                    const std::string& value) {
    return std::find(list.begin(), list.end(), value) != list.end();
}

// Manifest JSON -> typed fields. The manifest already passed
// validate_binding_manifest, so every lookup is of a known shape.
bool parse_manifest_fields(const std::string& manifest_json,
                           std::vector<std::string>* profiles,
                           std::vector<std::string>* modules,
                           std::vector<std::string>* net_rules,
                           std::vector<std::string>* fs_read,
                           std::vector<std::string>* fs_write,
                           std::vector<std::string>* env,
                           std::vector<std::string>* stdio,
                           std::string* error) {
    json_error_t parse_error;
    json_t* root = json_loads(manifest_json.c_str(), JSON_REJECT_DUPLICATES,
                              &parse_error);
    if (root == nullptr) {
        return fail(error, "binding manifest is unparseable");
    }
    json_t* sandbox = json_object_get(root, "sandbox");
    json_t* permissions = json_object_get(root, "permissions");
    if (sandbox != nullptr &&
        !read_string_array(sandbox, "requires", profiles, error)) {
        json_decref(root);
        return false;
    }
    if (permissions == nullptr ||
        !read_string_array(permissions, "modules", modules, error) ||
        !read_string_array(permissions, "env", env, error) ||
        !read_string_array(permissions, "stdio", stdio, error)) {
        json_decref(root);
        return fail(error, "binding manifest is malformed");
    }
    json_t* net = json_object_get(permissions, "net");
    if (net != nullptr &&
        !read_string_array(net, "allow", net_rules, error)) {
        json_decref(root);
        return false;
    }
    json_t* fs = json_object_get(permissions, "fs");
    if (fs != nullptr &&
        (!read_string_array(fs, "read", fs_read, error) ||
         !read_string_array(fs, "write", fs_write, error))) {
        json_decref(root);
        return false;
    }
    json_decref(root);
    return true;
}

}  // namespace

void fill_digest_entry(EffectiveBinding& binding);

bool parse_app_bindings(const std::vector<std::uint8_t>& bytes,
                        std::vector<AppBindingRequest>* out,
                        std::string* error) {
    if (out == nullptr) {
        return fail(error, "binding parse output is null");
    }
    json_error_t parse_error;
    json_t* root = json_loadb(
        reinterpret_cast<const char*>(bytes.data()), bytes.size(),
        JSON_REJECT_DUPLICATES, &parse_error);
    if (root == nullptr || !json_is_object(root)) {
        if (root != nullptr) {
            json_decref(root);
        }
        return fail(error, "invalid capsid.json");
    }
    json_t* bindings = json_object_get(root, "bindings");
    if (bindings == nullptr) {
        json_decref(root);
        return true;  // zero bindings
    }
    if (!json_is_object(bindings)) {
        json_decref(root);
        return fail(error, "app bindings is not an object");
    }
    std::vector<AppBindingRequest> parsed;
    void* iter = json_object_iter(bindings);
    for (; iter != nullptr; iter = json_object_iter_next(bindings, iter)) {
        AppBindingRequest request;
        request.id = json_object_iter_key(iter);
        json_t* entry = json_object_iter_value(iter);
        json_t* permissions = json_object_get(entry, "permissions");
        if (permissions != nullptr) {
            json_t* net = json_object_get(permissions, "net");
            if (net != nullptr &&
                !read_string_array(net, "allow", &request.net_rules,
                                   error)) {
                json_decref(root);
                return false;
            }
            json_t* fs = json_object_get(permissions, "fs");
            if (fs != nullptr &&
                (!read_string_array(fs, "read", &request.fs_read, error) ||
                 !read_string_array(fs, "write", &request.fs_write,
                                    error))) {
                json_decref(root);
                return false;
            }
            if (!read_string_array(permissions, "env", &request.env,
                                   error) ||
                !read_string_array(permissions, "stdio", &request.stdio,
                                   error)) {
                json_decref(root);
                return false;
            }
        }
        json_t* config = json_object_get(entry, "config");
        if (config != nullptr) {
            char* canonical = json_dumps(
                config, JSON_COMPACT | JSON_SORT_KEYS | JSON_ENSURE_ASCII);
            if (canonical == nullptr) {
                json_decref(root);
                return fail(error, "binding config is unreadable");
            }
            request.config_json = canonical;
            std::free(canonical);
        }
        json_t* secrets = json_object_get(entry, "secrets");
        if (secrets != nullptr) {
            void* secret_iter = json_object_iter(secrets);
            for (; secret_iter != nullptr;
                 secret_iter = json_object_iter_next(secrets,
                                                     secret_iter)) {
                // The map key is the public name the factory receives;
                // the valueFrom string is the provider key id.
                json_t* reference =
                    json_object_get(json_object_iter_value(secret_iter),
                                    "valueFrom");
                if (reference != nullptr && json_is_string(reference)) {
                    BindingSecretRef secret_ref;
                    secret_ref.name = json_object_iter_key(secret_iter);
                    secret_ref.key_id = json_string_value(reference);
                    request.secrets.push_back(std::move(secret_ref));
                }
            }
        }
        std::sort(request.secrets.begin(), request.secrets.end(),
                  [](const BindingSecretRef& a, const BindingSecretRef& b) {
                      return a.key_id < b.key_id;
                  });
        parsed.push_back(std::move(request));
    }
    std::sort(parsed.begin(), parsed.end(),
              [](const AppBindingRequest& a, const AppBindingRequest& b) {
                  return a.id < b.id;
              });
    json_decref(root);
    *out = std::move(parsed);
    return true;
}


std::string compute_effective_profile_digest(
    const std::vector<EffectiveBinding>& bindings) {
    if (bindings.empty()) {
        return {};
    }
    std::vector<std::string> profiles;
    for (const EffectiveBinding& binding : bindings) {
        for (const std::string& profile : binding.profiles) {
            profiles.push_back(profile);
        }
    }
    std::sort(profiles.begin(), profiles.end());
    profiles.erase(std::unique(profiles.begin(), profiles.end()),
                   profiles.end());
    std::string canonical;
    for (const std::string& profile : profiles) {
        canonical += profile;
        canonical += '\n';
    }
    return sha256_hex(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(canonical.data()),
        canonical.size()));
}

capsid::WorkerReadyProof expected_worker_ready_proof(
    const std::vector<EffectiveBinding>& bindings,
    bool strict_sandbox) {
    capsid::WorkerReadyProof expected;
    if (bindings.empty()) {
        return expected;
    }
    expected.extended = true;
    expected.applied_feature_bits = strict_sandbox
        ? static_cast<uint32_t>(CAPSID_SANDBOX_FEATURE_STRICT_BASE)
        : static_cast<uint32_t>(CAPSID_SANDBOX_FEATURE_RLIMITS);
    expected.seccomp_mode = strict_sandbox ? capsid::kSeccompModeFilter : 0;
    expected.landlock_abi = strict_sandbox ? host_landlock_abi() : 0;
    expected.network_namespace_identity.clear();
    expected.sandbox_profile_digest =
        compute_effective_profile_digest(bindings);
    return expected;
}

bool verify_worker_ready(
    const std::vector<std::uint8_t>& payload,
    const std::string& compatibility_id,
    const capsid::WorkerReadyProof& expected,
    std::string* error) {
    if (error != nullptr) {
        error->clear();
    }
    capsid::WorkerReadyProof proof;
    std::string reported_compat;
    std::string proof_error;
    if (!capsid::parse_ready_proof(
            payload, &reported_compat, &proof, &proof_error)) {
        if (error != nullptr) {
            *error = "worker READY payload is malformed: " + proof_error;
        }
        return false;
    }
    if (reported_compat != compatibility_id) {
        if (error != nullptr) {
            *error = "worker compatibility mismatch";
        }
        return false;
    }
    if (proof.extended != expected.extended) {
        if (error != nullptr) {
            *error = expected.extended
                ? "binding worker READY lacks the sandbox proof"
                : "zero-binding worker reported an extended READY";
        }
        return false;
    }
    if (!expected.extended) {
        return true;
    }
    if (proof.applied_feature_bits != expected.applied_feature_bits) {
        if (error != nullptr) {
            *error = "worker READY applied sandbox features mismatch";
        }
        return false;
    }
    if (proof.sandbox_profile_digest != expected.sandbox_profile_digest) {
        if (error != nullptr) {
            *error = "worker READY profile digest mismatch";
        }
        return false;
    }
    // §4.3: every proof field is compared exactly. Zero and the empty
    // namespace identity are real expected states (non-strict/no namespace),
    // never sentinel values that disable verification.
    if (proof.seccomp_mode != expected.seccomp_mode) {
        if (error != nullptr) {
            *error = "worker READY seccomp mode mismatch";
        }
        return false;
    }
    if (proof.landlock_abi != expected.landlock_abi) {
        if (error != nullptr) {
            *error = "worker READY Landlock ABI mismatch";
        }
        return false;
    }
    if (proof.network_namespace_identity !=
        expected.network_namespace_identity) {
        if (error != nullptr) {
            *error = "worker READY network namespace identity mismatch";
        }
        return false;
    }
    return true;
}

std::string serialize_bindings_snapshot(
    const std::vector<EffectiveBinding>& bindings) {
    json_t* root = json_array();
    if (root == nullptr) {
        return {};
    }
    std::vector<const EffectiveBinding*> ordered;
    ordered.reserve(bindings.size());
    for (const EffectiveBinding& binding : bindings) {
        ordered.push_back(&binding);
    }
    std::sort(ordered.begin(), ordered.end(),
              [](const EffectiveBinding* left,
                 const EffectiveBinding* right) {
                  return left->id < right->id;
              });
    std::uint64_t total_source_bytes = 0;
    std::string previous_id;
    for (const EffectiveBinding* binding_pointer : ordered) {
        const EffectiveBinding& binding = *binding_pointer;
        if (!valid_binding_id(binding.id) ||
            (!previous_id.empty() && binding.id <= previous_id) ||
            binding.package.manifest_json.size() >
                kMaxBindingManifestBytes ||
            binding.package.source.size() > kMaxBindingSourceBytes ||
            binding.request.config_json.size() >
                kMaxBindingConfigBytes ||
            binding.package.source.size() >
                kMaxBindingGenerationSourceBytes - total_source_bytes ||
            !validate_binding_manifest(
                 binding.package.manifest_json).ok) {
            json_decref(root);
            return {};
        }
        previous_id = binding.id;
        total_source_bytes += binding.package.source.size();
        json_t* entry = json_object();
        const auto set_string = [entry](const char* key,
                                        const std::string& value) {
            json_t* encoded = json_stringn(value.data(), value.size());
            return encoded != nullptr &&
                   json_object_set_new(entry, key, encoded) == 0;
        };
        if (entry == nullptr || !set_string("id", binding.id) ||
            !set_string("manifest", binding.package.manifest_json) ||
            !set_string("source", binding.package.source) ||
            !set_string("config", binding.request.config_json)) {
            if (entry != nullptr) {
                json_decref(entry);
            }
            json_decref(root);
            return {};
        }
        const auto list = [](const std::vector<std::string>& values) {
            json_t* array = json_array();
            if (array == nullptr) {
                return static_cast<json_t*>(nullptr);
            }
            for (const std::string& value : values) {
                json_t* encoded = json_stringn(value.data(), value.size());
                if (encoded == nullptr ||
                    json_array_append_new(array, encoded) != 0) {
                    json_decref(array);
                    return static_cast<json_t*>(nullptr);
                }
            }
            return array;
        };
        const auto set_list = [&list, entry](
                                  const char* key,
                                  const std::vector<std::string>& values) {
            json_t* array = list(values);
            // Jansson's _new APIs consume a non-null value on both success
            // and failure, so no extra decref is permitted here.
            return array != nullptr &&
                   json_object_set_new(entry, key, array) == 0;
        };
        if (!set_list("net", binding.request.net_rules) ||
            !set_list("fs_read", binding.request.fs_read) ||
            !set_list("fs_write", binding.request.fs_write) ||
            !set_list("env", binding.request.env) ||
            !set_list("stdio", binding.request.stdio) ||
            !set_list("profiles", binding.profiles) ||
            !set_list("modules", binding.modules)) {
            json_decref(entry);
            json_decref(root);
            return {};
        }
        json_t* secret_array = json_array();
        if (secret_array == nullptr) {
            json_decref(entry);
            json_decref(root);
            return {};
        }
        std::vector<BindingSecretRef> sorted_secrets =
            binding.request.secrets;
        std::sort(sorted_secrets.begin(), sorted_secrets.end(),
                  [](const BindingSecretRef& left,
                     const BindingSecretRef& right) {
                      if (left.name != right.name) {
                          return left.name < right.name;
                      }
                      if (left.key_id != right.key_id) {
                          return left.key_id < right.key_id;
                      }
                      return left.opaque_revision < right.opaque_revision;
                  });
        std::string previous_secret_name;
        for (const BindingSecretRef& secret_ref : sorted_secrets) {
            if (secret_ref.opaque_revision.empty()) {
                json_decref(secret_array);
                json_decref(entry);
                json_decref(root);
                return {};
            }
            if (!previous_secret_name.empty() &&
                secret_ref.name == previous_secret_name) {
                json_decref(secret_array);
                json_decref(entry);
                json_decref(root);
                return {};
            }
            previous_secret_name = secret_ref.name;
            json_t* pair = json_array();
            const auto append_string = [pair](const std::string& value) {
                json_t* encoded = json_stringn(value.data(), value.size());
                return encoded != nullptr &&
                       json_array_append_new(pair, encoded) == 0;
            };
            if (pair == nullptr || !append_string(secret_ref.name) ||
                !append_string(secret_ref.key_id) ||
                !append_string(secret_ref.opaque_revision)) {
                json_decref(pair);
                json_decref(secret_array);
                json_decref(entry);
                json_decref(root);
                return {};
            }
            if (json_array_append_new(secret_array, pair) != 0) {
                json_decref(secret_array);
                json_decref(entry);
                json_decref(root);
                return {};
            }
        }
        if (json_object_set_new(entry, "secrets", secret_array) != 0) {
            json_decref(entry);
            json_decref(root);
            return {};
        }
        if (json_array_append_new(root, entry) != 0) {
            json_decref(root);
            return {};
        }
    }
    char* text = json_dumps(root, JSON_COMPACT | JSON_ENSURE_ASCII);
    json_decref(root);
    if (text == nullptr) {
        return {};
    }
    std::string result(text);
    std::free(text);
    if (result.size() > kMaxBindingsSnapshotBytes) {
        return {};
    }
    return result;
}

namespace {

bool snapshot_string(json_t* parent, const char* key, std::string* out) {
    json_t* value = json_object_get(parent, key);
    if (!json_is_string(value)) {
        return false;
    }
    out->assign(json_string_value(value), json_string_length(value));
    return true;
}

bool snapshot_list(json_t* parent,
                   const char* key,
                   std::vector<std::string>* out) {
    json_t* value = json_object_get(parent, key);
    if (!json_is_array(value)) {
        return false;
    }
    std::unordered_set<std::string> seen;
    std::size_t index = 0;
    json_t* item = nullptr;
    json_array_foreach(value, index, item) {
        if (!json_is_string(item)) {
            return false;
        }
        std::string text(json_string_value(item), json_string_length(item));
        if (!seen.insert(text).second) {
            return false;
        }
        out->push_back(std::move(text));
    }
    return true;
}

bool canonical_config(const std::string& config, json_t** parsed) {
    if (config.size() > kMaxBindingConfigBytes) {
        return false;
    }
    json_error_t parse_error;
    json_t* value = json_loadb(config.data(), config.size(),
                               JSON_REJECT_DUPLICATES, &parse_error);
    if (!json_is_object(value)) {
        if (value != nullptr) {
            json_decref(value);
        }
        return false;
    }
    char* dumped = json_dumps(value,
                              JSON_COMPACT | JSON_SORT_KEYS |
                                  JSON_ENSURE_ASCII);
    if (dumped == nullptr) {
        json_decref(value);
        return false;
    }
    const bool matches = config == dumped;
    std::free(dumped);
    if (!matches) {
        json_decref(value);
        return false;
    }
    *parsed = value;
    return true;
}

json_t* snapshot_json_list(const std::vector<std::string>& values) {
    json_t* array = json_array();
    if (array == nullptr) {
        return nullptr;
    }
    for (const std::string& value : values) {
        json_t* item = json_stringn(value.data(), value.size());
        if (item == nullptr || json_array_append_new(array, item) != 0) {
            // json_array_append_new consumes non-null item even on failure.
            json_decref(array);
            return nullptr;
        }
    }
    return array;
}

// Rebuild one minimal capsid/app-v2 document and pass it through the same
// schema boundary as deployment. This re-establishes Binding ID, net/env,
// secret-name/key, config depth/size, and typed permission invariants during
// recovery instead of trusting the committed JSON wrapper.
bool validate_snapshot_request(const AppBindingRequest& request,
                               json_t* config) {
    json_t* app = json_object();
    json_t* pool = json_object();
    json_t* bindings = json_object();
    json_t* binding = json_object();
    json_t* permissions = json_object();
    json_t* net = json_object();
    json_t* fs = json_object();
    json_t* secrets = json_object();
    const auto cleanup = [&]() {
        json_decref(app);
        json_decref(pool);
        json_decref(bindings);
        json_decref(binding);
        json_decref(permissions);
        json_decref(net);
        json_decref(fs);
        json_decref(secrets);
    };
    if (app == nullptr || pool == nullptr || bindings == nullptr ||
        binding == nullptr || permissions == nullptr || net == nullptr ||
        fs == nullptr || secrets == nullptr) {
        cleanup();
        return false;
    }
    const auto put_new = [](json_t* object, const char* key, json_t* value) {
        return value != nullptr &&
               json_object_set_new(object, key, value) == 0;
    };
    const auto transfer = [](json_t* object, const char* key,
                             json_t** value) {
        if (*value == nullptr) {
            return false;
        }
        json_t* owned = *value;
        *value = nullptr;
        return json_object_set_new(object, key, owned) == 0;
    };
    if (!put_new(app, "apiVersion", json_string("capsid/app-v2")) ||
        !put_new(pool, "minReady", json_integer(1)) ||
        !put_new(pool, "maxWorkers", json_integer(1)) ||
        !transfer(app, "pool", &pool) ||
        !put_new(net, "allow", snapshot_json_list(request.net_rules)) ||
        !put_new(fs, "read", snapshot_json_list(request.fs_read)) ||
        !put_new(fs, "write", snapshot_json_list(request.fs_write)) ||
        !transfer(permissions, "net", &net) ||
        !transfer(permissions, "fs", &fs) ||
        !put_new(permissions, "env",
                 snapshot_json_list(request.env)) ||
        !put_new(permissions, "stdio",
                 snapshot_json_list(request.stdio)) ||
        !transfer(binding, "permissions", &permissions) ||
        json_object_set(binding, "config", config) != 0) {
        cleanup();
        return false;
    }
    for (const BindingSecretRef& secret : request.secrets) {
        json_t* reference = json_object();
        if (reference == nullptr) {
            cleanup();
            return false;
        }
        if (!put_new(reference, "valueFrom",
                     json_stringn(secret.key_id.data(),
                                  secret.key_id.size()))) {
            json_decref(reference);
            cleanup();
            return false;
        }
        // json_object_set_new_nocheck consumes reference on both success and
        // failure, so it must not be decref'd after this call.
        if (json_object_set_new_nocheck(
                secrets, secret.name.c_str(), reference) != 0) {
            cleanup();
            return false;
        }
    }
    if (!transfer(binding, "secrets", &secrets)) {
        cleanup();
        return false;
    }
    json_t* owned_binding = binding;
    binding = nullptr;
    if (json_object_set_new_nocheck(bindings, request.id.c_str(),
                                    owned_binding) != 0 ||
        !transfer(app, "bindings", &bindings)) {
        cleanup();
        return false;
    }
    char* text = json_dumps(app, JSON_COMPACT | JSON_ENSURE_ASCII);
    json_decref(app);
    if (text == nullptr) {
        return false;
    }
    const ConfigValidationResult validation =
        validate_config_json(ConfigDocument::kApplication, text);
    std::free(text);
    return validation.ok;
}

}  // namespace

BindingSnapshotParseResult parse_bindings_snapshot(const std::string& json) {
    BindingSnapshotParseResult result;
    result.ok = false;
    if (json.size() > kMaxBindingsSnapshotBytes) {
        result.error = "bindings snapshot exceeds the size limit";
        return result;
    }
    json_error_t parse_error;
    json_t* root = json_loadb(json.data(), json.size(),
                              JSON_REJECT_DUPLICATES, &parse_error);
    if (!json_is_array(root)) {
        if (root != nullptr) {
            json_decref(root);
        }
        result.error = "bindings snapshot is malformed";
        return result;
    }

    static const char* const kFields[] = {
        "id",       "manifest", "source",  "config",
        "net",      "fs_read",  "fs_write", "env",
        "stdio",    "profiles", "modules", "secrets",
    };
    BindingRegistrySnapshot registry;
    std::vector<AppBindingRequest> requests;
    std::vector<std::vector<std::string>> committed_profiles;
    std::vector<std::vector<std::string>> committed_modules;
    std::uint64_t total_source_bytes = 0;
    std::string previous_id;
    std::size_t index = 0;
    json_t* entry = nullptr;
    json_array_foreach(root, index, entry) {
        if (!json_is_object(entry) ||
            json_object_size(entry) !=
                sizeof(kFields) / sizeof(kFields[0])) {
            json_decref(root);
            result.error = "bindings snapshot entry has an invalid field set";
            return result;
        }
        for (const char* field : kFields) {
            if (json_object_get(entry, field) == nullptr) {
                json_decref(root);
                result.error =
                    "bindings snapshot entry has an invalid field set";
                return result;
            }
        }

        BindingPackageSnapshot package;
        AppBindingRequest request;
        std::vector<std::string> profiles;
        std::vector<std::string> modules;
        if (!snapshot_string(entry, "id", &package.id) ||
            !snapshot_string(entry, "manifest", &package.manifest_json) ||
            !snapshot_string(entry, "source", &package.source) ||
            !snapshot_string(entry, "config", &request.config_json) ||
            !valid_binding_id(package.id) ||
            (!previous_id.empty() && package.id <= previous_id)) {
            json_decref(root);
            result.error = "bindings snapshot binding IDs are invalid";
            return result;
        }
        previous_id = package.id;
        request.id = package.id;
        if (package.manifest_json.size() > kMaxBindingManifestBytes ||
            package.source.size() > kMaxBindingSourceBytes ||
            package.source.size() >
                kMaxBindingGenerationSourceBytes - total_source_bytes) {
            json_decref(root);
            result.error = "bindings snapshot artifact exceeds a size limit";
            return result;
        }
        total_source_bytes += package.source.size();
        const ConfigValidationResult manifest_validation =
            validate_binding_manifest(package.manifest_json);
        if (!manifest_validation.ok) {
            json_decref(root);
            result.error = "bindings snapshot manifest is invalid";
            return result;
        }
        if (!snapshot_list(entry, "net", &request.net_rules) ||
            !snapshot_list(entry, "fs_read", &request.fs_read) ||
            !snapshot_list(entry, "fs_write", &request.fs_write) ||
            !snapshot_list(entry, "env", &request.env) ||
            !snapshot_list(entry, "stdio", &request.stdio) ||
            !snapshot_list(entry, "profiles", &profiles) ||
            !snapshot_list(entry, "modules", &modules)) {
            json_decref(root);
            result.error = "bindings snapshot list is invalid";
            return result;
        }

        json_t* secret_array = json_object_get(entry, "secrets");
        if (!json_is_array(secret_array)) {
            json_decref(root);
            result.error = "bindings snapshot secrets are invalid";
            return result;
        }
        std::unordered_set<std::string> secret_names;
        std::size_t pair_index = 0;
        json_t* pair = nullptr;
        json_array_foreach(secret_array, pair_index, pair) {
            if (!json_is_array(pair) || json_array_size(pair) != 3) {
                json_decref(root);
                result.error = "bindings snapshot secrets are invalid";
                return result;
            }
            BindingSecretRef secret;
            json_t* public_name = json_array_get(pair, 0);
            json_t* key_id = json_array_get(pair, 1);
            json_t* revision = json_array_get(pair, 2);
            if (!json_is_string(public_name) || !json_is_string(key_id) ||
                !json_is_string(revision)) {
                json_decref(root);
                result.error = "bindings snapshot secrets are invalid";
                return result;
            }
            secret.name.assign(json_string_value(public_name),
                               json_string_length(public_name));
            secret.key_id.assign(json_string_value(key_id),
                                 json_string_length(key_id));
            secret.opaque_revision.assign(json_string_value(revision),
                                          json_string_length(revision));
            if (secret.opaque_revision.empty() ||
                !secret_names.insert(secret.name).second) {
                json_decref(root);
                result.error = "bindings snapshot secrets are invalid";
                return result;
            }
            request.secrets.push_back(std::move(secret));
        }

        json_t* config = nullptr;
        if (!canonical_config(request.config_json, &config) ||
            !validate_snapshot_request(request, config)) {
            if (config != nullptr) {
                json_decref(config);
            }
            json_decref(root);
            result.error = "bindings snapshot request is invalid";
            return result;
        }
        json_decref(config);

        package.manifest_digest =
            compute_binding_manifest_digest(package.manifest_json);
        package.source_digest = sha256_hex(std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(package.source.data()),
            package.source.size()));
        registry.packages.push_back(std::move(package));
        requests.push_back(std::move(request));
        committed_profiles.push_back(std::move(profiles));
        committed_modules.push_back(std::move(modules));
    }
    json_decref(root);

    BindingCompileResult compiled =
        compile_effective_bindings(registry, requests);
    if (!compiled.ok || compiled.bindings.size() != committed_profiles.size()) {
        result.error = "bindings snapshot effective policy is invalid";
        return result;
    }
    for (std::size_t binding_index = 0;
         binding_index < compiled.bindings.size(); ++binding_index) {
        if (compiled.bindings[binding_index].profiles !=
                committed_profiles[binding_index] ||
            compiled.bindings[binding_index].modules !=
                committed_modules[binding_index]) {
            result.error =
                "bindings snapshot manifest-derived fields do not match";
            return result;
        }
    }
    result.bindings = std::move(compiled.bindings);
    result.ok = true;
    return result;
}
bool build_binding_descriptor(const EffectiveBinding& binding,
                              capsid_binding_descriptor* out) {
    if (out == nullptr) {
        return false;
    }
    std::memset(out, 0, sizeof(*out));
    out->struct_size = sizeof(*out);
    out->version = CAPSID_BINDING_DESCRIPTOR_VERSION;
    out->binding_name = binding.id.c_str();
    out->source.data =
        reinterpret_cast<const std::uint8_t*>(binding.package.source.data());
    out->source.size = binding.package.source.size();
    out->config_json.data =
        reinterpret_cast<const std::uint8_t*>(binding.request.config_json.data());
    out->config_json.size = binding.request.config_json.size();
    // The descriptor's policy/sandbox views below must stay alive for the
    // duration of the call; capsid_worker_load_binding copies them.
    static thread_local std::vector<std::string> net_rules;
    static thread_local std::vector<const char*> net_views;
    static thread_local std::vector<std::string> fs_read;
    static thread_local std::vector<const char*> fs_read_views;
    static thread_local std::vector<std::string> fs_write;
    static thread_local std::vector<const char*> fs_write_views;
    static thread_local std::vector<std::string> env;
    static thread_local std::vector<const char*> env_views;
    static thread_local std::vector<std::string> stdio;
    static thread_local std::vector<const char*> stdio_views;
    static thread_local std::vector<std::string> modules;
    static thread_local std::vector<const char*> module_views;
    static thread_local std::vector<std::string> profiles;
    static thread_local std::vector<const char*> profile_views;
    static thread_local std::vector<capsid_binding_secret> secrets;
    static thread_local capsid_binding_policy policy;
    static thread_local capsid_sandbox_requirements sandbox;
    net_rules = binding.request.net_rules;
    fs_read = binding.request.fs_read;
    fs_write = binding.request.fs_write;
    env = binding.request.env;
    stdio = binding.request.stdio;
    modules = binding.modules;
    profiles = binding.profiles;
    const auto views = [](const std::vector<std::string>& values,
                          std::vector<const char*>* out_views) {
        out_views->clear();
        out_views->reserve(values.size());
        for (const std::string& value : values) {
            out_views->push_back(value.c_str());
        }
    };
    views(net_rules, &net_views);
    views(fs_read, &fs_read_views);
    views(fs_write, &fs_write_views);
    views(env, &env_views);
    views(stdio, &stdio_views);
    views(modules, &module_views);
    views(profiles, &profile_views);
    std::memset(&policy, 0, sizeof(policy));
    policy.modules = module_views.empty() ? nullptr : &module_views[0];
    policy.module_count = static_cast<uint32_t>(module_views.size());
    policy.net_rules = net_views.empty() ? nullptr : &net_views[0];
    policy.net_rule_count = static_cast<uint32_t>(net_views.size());
    policy.fs_read = fs_read_views.empty() ? nullptr : &fs_read_views[0];
    policy.fs_read_count = static_cast<uint32_t>(fs_read_views.size());
    policy.fs_write = fs_write_views.empty() ? nullptr : &fs_write_views[0];
    policy.fs_write_count = static_cast<uint32_t>(fs_write_views.size());
    policy.env = env_views.empty() ? nullptr : &env_views[0];
    policy.env_count = static_cast<uint32_t>(env_views.size());
    policy.stdio = stdio_views.empty() ? nullptr : &stdio_views[0];
    policy.stdio_count = static_cast<uint32_t>(stdio_views.size());
    out->policy = &policy;
    std::memset(&sandbox, 0, sizeof(sandbox));
    sandbox.profiles = profile_views.empty() ? nullptr : &profile_views[0];
    sandbox.profile_count = static_cast<uint32_t>(profile_views.size());
    out->sandbox = &sandbox;
    secrets.clear();
    secrets.reserve(binding.secret_values.size());
    for (const auto& value : binding.secret_values) {
        capsid_binding_secret secret;
        std::memset(&secret, 0, sizeof(secret));
        secret.key = value.name.c_str();
        secret.value.data = reinterpret_cast<const std::uint8_t*>(
            value.value.data());
        secret.value.size = value.value.size();
        secrets.push_back(secret);
    }
    if (!secrets.empty()) {
        out->secrets = &secrets[0];
        out->secret_count = static_cast<uint32_t>(secrets.size());
    }
    return true;
}


// Rebuilds every identity-relevant digest field from the binding's own
// committed bytes — used by both the deploy compile and the recovery
// parse so both re-derive identical digest material.
void fill_digest_entry(EffectiveBinding& binding) {
    const auto append_u32 = [](std::vector<std::uint8_t>* output,
                               std::size_t count) {
        const std::uint32_t value = static_cast<std::uint32_t>(count);
        output->push_back(static_cast<std::uint8_t>(value >> 24));
        output->push_back(static_cast<std::uint8_t>(value >> 16));
        output->push_back(static_cast<std::uint8_t>(value >> 8));
        output->push_back(static_cast<std::uint8_t>(value));
    };
    const auto append_field = [&append_u32](
                                  std::vector<std::uint8_t>* output,
                                  std::string_view value) {
        append_u32(output, value.size());
        output->insert(output->end(), value.begin(), value.end());
    };
    binding.digest_entry.id = binding.id;
    binding.digest_entry.manifest_digest =
        binding.package.manifest_digest;
    binding.digest_entry.source_digest =
        binding.package.source_digest;
    binding.digest_entry.config_digest = sha256_hex(
        std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(
                binding.request.config_json.data()),
            binding.request.config_json.size()));
    std::vector<std::uint8_t> canonical;
    static constexpr char kPermissionDomain[] =
        "capsid-binding-permissions-v1\0";
    canonical.insert(canonical.end(), kPermissionDomain,
                     kPermissionDomain + sizeof(kPermissionDomain) - 1);
    const std::vector<std::string>* permission_lists[] = {
        &binding.request.net_rules,
        &binding.request.fs_read,
        &binding.request.fs_write,
        &binding.request.env,
        &binding.request.stdio,
    };
    for (const std::vector<std::string>* list : permission_lists) {
        append_u32(&canonical, list->size());
        for (const std::string& value : *list) {
            append_field(&canonical, value);
        }
    }
    binding.digest_entry.permission_digest = sha256_hex(
        std::span<const std::uint8_t>(canonical.data(), canonical.size()));
    std::string profile_canonical;
    std::vector<std::string> sorted_profiles = binding.profiles;
    std::sort(sorted_profiles.begin(), sorted_profiles.end());
    for (const std::string& profile : sorted_profiles) {
        profile_canonical += profile + "\n";
    }
    binding.digest_entry.profile_digest = sha256_hex(
        std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(
                profile_canonical.data()),
            profile_canonical.size()));
    binding.digest_entry.binding_runtime_compatibility =
        std::string(kBindingRuntimeCompatibilityVersion);
    binding.digest_entry.secret_key_ids.clear();
    std::vector<BindingSecretRef> sorted_secrets = binding.request.secrets;
    std::sort(sorted_secrets.begin(), sorted_secrets.end(),
              [](const BindingSecretRef& left,
                 const BindingSecretRef& right) {
                  if (left.name != right.name) {
                      return left.name < right.name;
                  }
                  if (left.key_id != right.key_id) {
                      return left.key_id < right.key_id;
                  }
                  return left.opaque_revision < right.opaque_revision;
              });
    std::vector<std::uint8_t> revision_record;
    static constexpr char kSecretDomain[] =
        "capsid-binding-secret-revisions-v1\0";
    revision_record.insert(revision_record.end(), kSecretDomain,
                           kSecretDomain + sizeof(kSecretDomain) - 1);
    append_u32(&revision_record, sorted_secrets.size());
    for (const BindingSecretRef& secret_ref : sorted_secrets) {
        binding.digest_entry.secret_key_ids.push_back(secret_ref.key_id);
        append_field(&revision_record, secret_ref.name);
        append_field(&revision_record, secret_ref.key_id);
        append_field(&revision_record, secret_ref.opaque_revision);
    }
    std::sort(binding.digest_entry.secret_key_ids.begin(),
              binding.digest_entry.secret_key_ids.end());
    binding.digest_entry.secret_revision = sha256_hex(
        std::span<const std::uint8_t>(revision_record.data(),
                                      revision_record.size()));
}

BindingCompileResult compile_effective_bindings(
    const BindingRegistrySnapshot& registry,
    const std::vector<AppBindingRequest>& requests) {
    BindingCompileResult result;
    result.ok = false;

    // Every declared id must exist exactly once in the registry snapshot.
    std::vector<std::string> declared;
    for (const AppBindingRequest& request : requests) {
        if (!std::binary_search(
                declared.begin(), declared.end(), request.id)) {
            declared.push_back(request.id);
        } else {
            result.error = "duplicate binding declaration: " + request.id;
            return result;
        }
    }
    std::sort(declared.begin(), declared.end());

    std::vector<EffectiveBinding> effective;
    effective.reserve(requests.size());
    for (const AppBindingRequest& request : requests) {
        const BindingPackageSnapshot* package = nullptr;
        for (const BindingPackageSnapshot& candidate : registry.packages) {
            if (candidate.id == request.id) {
                package = &candidate;
                break;
            }
        }
        if (package == nullptr) {
            result.error =
                "binding is not installed: " + request.id;
            return result;
        }

        EffectiveBinding binding;
        binding.id = request.id;
        binding.package = *package;
        binding.request = request;

        // Manifest maximums.
        std::string error;
        std::vector<std::string> manifest_net;
        std::vector<std::string> manifest_fs_read;
        std::vector<std::string> manifest_fs_write;
        std::vector<std::string> manifest_env;
        std::vector<std::string> manifest_stdio;
        if (!parse_manifest_fields(
                package->manifest_json, &binding.profiles,
                &binding.modules, &manifest_net, &manifest_fs_read,
                &manifest_fs_write, &manifest_env, &manifest_stdio,
                &error)) {
            result.error =
                "binding manifest is malformed: " + request.id;
            return result;
        }

        // Static subset proofs (§2.3): every App rule must be provably
        // covered by a manifest rule.
        for (const std::string& app_rule : request.net_rules) {
            bool covered = false;
            for (const std::string& manifest_rule : manifest_net) {
                if (net_target_contained(app_rule, manifest_rule)) {
                    covered = true;
                    break;
                }
            }
            if (!covered) {
                result.error = "binding net rule is not covered by the "
                               "manifest: " +
                               request.id + " " + app_rule;
                return result;
            }
        }
        for (const std::string& app_path : request.fs_read) {
            bool covered = false;
            for (const std::string& manifest_path : manifest_fs_read) {
                if (fs_path_contained(app_path, manifest_path)) {
                    covered = true;
                    break;
                }
            }
            if (!covered) {
                result.error = "binding fs read path is not covered by the "
                               "manifest: " +
                               request.id + " " + app_path;
                return result;
            }
        }
        for (const std::string& app_path : request.fs_write) {
            bool covered = false;
            for (const std::string& manifest_path : manifest_fs_write) {
                if (fs_path_contained(app_path, manifest_path)) {
                    covered = true;
                    break;
                }
            }
            if (!covered) {
                result.error = "binding fs write path is not covered by "
                               "the manifest: " +
                               request.id + " " + app_path;
                return result;
            }
        }
        for (const std::string& app_env : request.env) {
            if (!contains_exact(manifest_env, app_env)) {
                result.error = "binding env name is not covered by the "
                               "manifest: " +
                               request.id + " " + app_env;
                return result;
            }
        }
        for (const std::string& app_stream : request.stdio) {
            if (!contains_exact(manifest_stdio, app_stream)) {
                result.error = "binding stdio stream is not covered by "
                               "the manifest: " +
                               request.id + " " + app_stream;
                return result;
            }
        }

        fill_digest_entry(binding);
        effective.push_back(std::move(binding));
    }
    result.set_digest = refresh_binding_set_digest(&effective);
    result.bindings = std::move(effective);
    result.ok = true;
    return result;
}

std::string refresh_binding_set_digest(
    std::vector<EffectiveBinding>* bindings) {
    if (bindings == nullptr) {
        return {};
    }
    std::vector<BindingSetDigestEntry> digest_entries;
    digest_entries.reserve(bindings->size());
    for (EffectiveBinding& binding : *bindings) {
        fill_digest_entry(binding);
        digest_entries.push_back(binding.digest_entry);
    }
    return compute_binding_set_digest(digest_entries);
}

}  // namespace capsid::host
