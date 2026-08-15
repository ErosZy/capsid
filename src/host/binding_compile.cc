// Binding v1 host compile pipeline. The subset proofs are static: a net
// rule is contained only by exact match, wildcard-label suffix match or
// CIDR block containment (never DNS results); fs paths by prefix; env and
// stdio by exact membership.

#include "host/binding_compile.h"

#include "host/config.h"
#include "ipc_validation.h"

#include <jansson.h>

#include <algorithm>
#include <arpa/inet.h>
#include <cstdlib>
#include <cstring>
#include <span>
#include <utility>
#include <string>
#include <vector>

namespace capsid::host {
namespace {

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

bool verify_worker_ready(const std::vector<std::uint8_t>& payload,
                         const std::string& compatibility_id,
                         const std::vector<EffectiveBinding>& bindings,
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
    if (!proof.extended) {
        // §7.2: a zero-binding worker reports the exact baseline; a
        // Binding generation must carry the proof.
        if (!bindings.empty()) {
            if (error != nullptr) {
                *error = "binding worker READY lacks the sandbox proof";
            }
            return false;
        }
        return true;
    }
    if (bindings.empty()) {
        if (error != nullptr) {
            *error = "zero-binding worker reported an extended READY";
        }
        return false;
    }
    const std::string expected =
        compute_effective_profile_digest(bindings);
    if (proof.sandbox_profile_digest != expected) {
        if (error != nullptr) {
            *error = "worker READY profile digest mismatch";
        }
        return false;
    }
    return true;
}

std::string serialize_bindings_snapshot(
    const std::vector<EffectiveBinding>& bindings) {
    json_t* root = json_array();
    for (const EffectiveBinding& binding : bindings) {
        json_t* entry = json_object();
        json_object_set_new(entry, "id",
                            json_string(binding.id.c_str()));
        json_object_set_new(entry, "manifest",
                            json_string(binding.package.manifest_json.c_str()));
        json_object_set_new(entry, "source",
                            json_string(binding.package.source.c_str()));
        json_object_set_new(entry, "config",
                            json_string(binding.request.config_json.c_str()));
        const auto list = [](const std::vector<std::string>& values) {
            json_t* array = json_array();
            for (const std::string& value : values) {
                json_array_append_new(array, json_string(value.c_str()));
            }
            return array;
        };
        json_object_set_new(entry, "net", list(binding.request.net_rules));
        json_object_set_new(entry, "fs_read", list(binding.request.fs_read));
        json_object_set_new(entry, "fs_write", list(binding.request.fs_write));
        json_object_set_new(entry, "env", list(binding.request.env));
        json_object_set_new(entry, "stdio", list(binding.request.stdio));
        json_object_set_new(entry, "profiles", list(binding.profiles));
        json_object_set_new(entry, "modules", list(binding.modules));
        json_t* secret_array = json_array();
        for (const BindingSecretRef& secret_ref :
             binding.request.secrets) {
            json_t* pair = json_array();
            json_array_append_new(
                pair, json_string(secret_ref.name.c_str()));
            json_array_append_new(
                pair, json_string(secret_ref.key_id.c_str()));
            json_array_append_new(secret_array, pair);
        }
        json_object_set_new(entry, "secrets", secret_array);
        json_array_append_new(root, entry);
    }
    char* text = json_dumps(root, JSON_COMPACT | JSON_ENSURE_ASCII);
    json_decref(root);
    if (text == nullptr) {
        return {};
    }
    std::string result(text);
    std::free(text);
    return result;
}

BindingSnapshotParseResult parse_bindings_snapshot(const std::string& json) {
    BindingSnapshotParseResult result;
    result.ok = false;
    json_error_t parse_error;
    json_t* root = json_loads(json.c_str(), JSON_REJECT_DUPLICATES,
                              &parse_error);
    if (root == nullptr || !json_is_array(root)) {
        if (root != nullptr) {
            json_decref(root);
        }
        result.error = "bindings snapshot is malformed";
        return result;
    }
    const auto read_list = [](json_t* entry, const char* key,
                              std::vector<std::string>* out) -> bool {
        json_t* value = json_object_get(entry, key);
        if (value == nullptr || !json_is_array(value)) {
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
    std::size_t index = 0;
    json_t* entry = nullptr;
    json_array_foreach(root, index, entry) {
        EffectiveBinding binding;
        const char* id = json_string_value(json_object_get(entry, "id"));
        const char* manifest =
            json_string_value(json_object_get(entry, "manifest"));
        const char* source =
            json_string_value(json_object_get(entry, "source"));
        const char* config =
            json_string_value(json_object_get(entry, "config"));
        if (id == nullptr || manifest == nullptr || source == nullptr ||
            config == nullptr) {
            json_decref(root);
            result.error = "bindings snapshot entry is malformed";
            return result;
        }
        binding.id = id;
        binding.package.id = id;
        binding.package.manifest_json = manifest;
        binding.package.source = source;
        binding.package.manifest_digest =
            compute_binding_manifest_digest(binding.package.manifest_json);
        binding.package.source_digest = sha256_hex(std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(
                binding.package.source.data()),
            binding.package.source.size()));
        binding.request.id = id;
        binding.request.config_json = config;
        // §6: recovery re-derives every identity digest from the
        // committed bytes; it never trusts serialized digest fields.
        fill_digest_entry(binding);
        if (!read_list(entry, "net", &binding.request.net_rules) ||
            !read_list(entry, "fs_read", &binding.request.fs_read) ||
            !read_list(entry, "fs_write", &binding.request.fs_write) ||
            !read_list(entry, "env", &binding.request.env) ||
            !read_list(entry, "stdio", &binding.request.stdio) ||
            !read_list(entry, "profiles", &binding.profiles) ||
            !read_list(entry, "modules", &binding.modules)) {
            json_decref(root);
            result.error = "bindings snapshot entry is malformed";
            return result;
        }
        json_t* secret_array = json_object_get(entry, "secrets");
        if (secret_array != nullptr) {
            if (!json_is_array(secret_array)) {
                json_decref(root);
                result.error = "bindings snapshot entry is malformed";
                return result;
            }
            std::size_t pair_index = 0;
            json_t* pair = nullptr;
            json_array_foreach(secret_array, pair_index, pair) {
                if (!json_is_array(pair) || json_array_size(pair) != 2 ||
                    !json_is_string(json_array_get(pair, 0)) ||
                    !json_is_string(json_array_get(pair, 1))) {
                    json_decref(root);
                    result.error =
                        "bindings snapshot entry is malformed";
                    return result;
                }
                BindingSecretRef secret_ref;
                secret_ref.name =
                    json_string_value(json_array_get(pair, 0));
                secret_ref.key_id =
                    json_string_value(json_array_get(pair, 1));
                binding.request.secrets.push_back(
                    std::move(secret_ref));
            }
        }
        result.bindings.push_back(std::move(binding));
    }
    json_decref(root);
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
    std::string canonical = "net:";
    for (const std::string& rule : binding.request.net_rules) {
        canonical += rule + ",";
    }
    canonical += ";fs-read:";
    for (const std::string& path : binding.request.fs_read) {
        canonical += path + ",";
    }
    canonical += ";fs-write:";
    for (const std::string& path : binding.request.fs_write) {
        canonical += path + ",";
    }
    canonical += ";env:";
    for (const std::string& name : binding.request.env) {
        canonical += name + ",";
    }
    canonical += ";stdio:";
    for (const std::string& stream : binding.request.stdio) {
        canonical += stream + ",";
    }
    binding.digest_entry.permission_digest = sha256_hex(
        std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(canonical.data()),
            canonical.size()));
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
    binding.digest_entry.secret_key_ids.clear();
    for (const BindingSecretRef& secret_ref : binding.request.secrets) {
        binding.digest_entry.secret_key_ids.push_back(secret_ref.key_id);
    }
    std::sort(binding.digest_entry.secret_key_ids.begin(),
              binding.digest_entry.secret_key_ids.end());
}

BindingCompileResult compile_effective_bindings(
    const BindingRegistrySnapshot& registry,
    const std::vector<AppBindingRequest>& requests,
    const std::string& secret_revision) {
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
        binding.digest_entry.secret_revision = secret_revision;
        effective.push_back(std::move(binding));
    }

    std::vector<BindingSetDigestEntry> digest_entries;
    digest_entries.reserve(effective.size());
    for (const EffectiveBinding& binding : effective) {
        digest_entries.push_back(binding.digest_entry);
    }
    result.set_digest = compute_binding_set_digest(digest_entries);
    result.bindings = std::move(effective);
    result.ok = true;
    return result;
}

}  // namespace capsid::host
