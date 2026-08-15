// Binding v1 host compile pipeline (docs/binding-technical-design.md §2/§6):
// the typed App binding request, the Manifest ∩ App effective compile with
// static subset proofs, and the immutable per-generation snapshot the
// worker spawn path and the Generation Identity both consume.

#ifndef CAPSID_HOST_BINDING_COMPILE_H
#define CAPSID_HOST_BINDING_COMPILE_H

#include "capsid/runtime.h"
#include "host/binding_registry.h"
#include "host/generation_identity.h"

#include <cstdint>
#include <string>
#include <vector>

namespace capsid::host {

// One App binding declaration (bindings.<id>), extracted from the
// schema-validated capsid.json. Secret key ids only — never values.
struct AppBindingRequest {
    std::string id;
    std::vector<std::string> net_rules;  // allow targets
    std::vector<std::string> fs_read;
    std::vector<std::string> fs_write;
    std::vector<std::string> env;
    std::vector<std::string> stdio;
    std::string config_json;  // canonical compact serialization
    std::vector<std::string> secret_key_ids;
};

// Parses the app document's bindings map. The document is assumed to have
// passed validate_config_json (app-v2); this layer extracts typed data and
// fails closed on anything unexpected.
bool parse_app_bindings(const std::vector<std::uint8_t>& bytes,
                        std::vector<AppBindingRequest>* out,
                        std::string* error);

// One committed effective Binding: the manifest maximum intersected with
// the App's proven subset, plus the digest material.
struct EffectiveBinding {
    std::string id;
    BindingPackageSnapshot package;  // manifest + source bytes (immutable)
    AppBindingRequest request;
    std::vector<std::string> profiles;  // manifest sandbox.requires
    std::vector<std::string> modules;   // manifest permission modules
    BindingSetDigestEntry digest_entry;
    // key id -> resolved value, filled by the caller from the committed
    // secret snapshot. Values never enter digests or disk.
    std::vector<std::pair<std::string, std::string>> secret_values;
};

struct BindingCompileResult {
    bool ok = false;
    std::string error;  // static text
    std::vector<EffectiveBinding> bindings;  // sorted by id
    std::string set_digest;
};

// Serializes the committed binding snapshot (manifest, source, config,
// effective permissions, profiles, modules, secret key ids — never secret
// values). Recovery rebuilds exclusively from this document.
std::string serialize_bindings_snapshot(
    const std::vector<EffectiveBinding>& bindings);

struct BindingSnapshotParseResult {
    bool ok = false;
    std::string error;  // static text
    std::vector<EffectiveBinding> bindings;  // secret_values empty
};

BindingSnapshotParseResult parse_bindings_snapshot(const std::string& json);

// Materializes a worker wire descriptor over the effective binding's
// storage (copied by capsid_worker_load_binding before it returns).
bool build_binding_descriptor(const EffectiveBinding& binding,
                              capsid_binding_descriptor* out);

// Compiles the effective binding set: every declared id must exist in the
// registry snapshot, and every App permission must be statically provable
// as a subset of its manifest's (no DNS-derived containment). The digest
// follows §6; secret values never enter it (they are not even inputs).
BindingCompileResult compile_effective_bindings(
    const BindingRegistrySnapshot& registry,
    const std::vector<AppBindingRequest>& requests,
    const std::string& secret_revision);

}  // namespace capsid::host

#endif
