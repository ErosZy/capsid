// Binding v1 host compile pipeline (docs/binding-technical-design.md §2/§6):
// the typed App binding request, the Manifest ∩ App effective compile with
// static subset proofs, and the immutable per-generation snapshot the
// worker spawn path and the Generation Identity both consume.

#ifndef CAPSID_HOST_BINDING_COMPILE_H
#define CAPSID_HOST_BINDING_COMPILE_H

#include "capsid/runtime.h"
#include "host/binding_registry.h"
#include "host/generation_identity.h"
#include "ipc_validation.h"

#include <cstdint>
#include <string>
#include <vector>

namespace capsid::host {

// One App binding declaration (bindings.<id>), extracted from the
// schema-validated capsid.json. Secret references keep BOTH the public
// name the factory receives (password) and the key id (mongo-password);
// values never appear here.
struct BindingSecretRef {
    std::string name;     // the factory-visible key
    std::string key_id;   // the provider lookup id
    // Provider-owned opaque version. Filled after resolution or parsed from
    // the immutable generation snapshot; never derived from secret bytes.
    std::string opaque_revision;
};

struct AppBindingRequest {
    std::string id;
    std::vector<std::string> net_rules;  // allow targets
    std::vector<std::string> fs_read;
    std::vector<std::string> fs_write;
    std::vector<std::string> env;
    std::vector<std::string> stdio;
    std::string config_json;  // canonical compact serialization
    std::vector<BindingSecretRef> secrets;  // sorted by key_id
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
    // Resolved secret values (name, key id, value), filled by the caller
    // from the secret provider. Values never enter digests or disk.
    struct SecretValue {
        std::string name;
        std::string key_id;
        std::string value;
    };
    std::vector<SecretValue> secret_values;
};

struct BindingCompileResult {
    bool ok = false;
    std::string error;  // static text
    std::vector<EffectiveBinding> bindings;  // sorted by id
    std::string set_digest;
};

// The §7.4 READY profile digest the Host expects: SHA-256 over the
// canonical (sorted, de-duplicated, newline-joined) union of every
// effective binding's profiles — the exact worker-side algorithm.
std::string compute_effective_profile_digest(
    const std::vector<EffectiveBinding>& bindings);

// Constructs the exact proof a managed worker is required to report. Managed
// workers currently do not receive a network namespace descriptor; an empty
// namespace identity is therefore an exact value, not a wildcard.
capsid::WorkerReadyProof expected_worker_ready_proof(
    const std::vector<EffectiveBinding>& bindings,
    bool strict_sandbox);

// §4.3: the Host-side READY verdict. The payload must parse as the
// 71-byte compatibility id plus an optional proof; a Binding worker's
// proof must carry exactly the Host's expected profile digest. Returns
// true only on a full match, with a static diagnostic otherwise.
bool verify_worker_ready(
    const std::vector<std::uint8_t>& payload,
    const std::string& compatibility_id,
    const capsid::WorkerReadyProof& expected,
    std::string* error);

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
    const std::vector<AppBindingRequest>& requests);

// Rebuilds every entry after provider revisions have been attached and
// returns the complete Binding-set digest.
std::string refresh_binding_set_digest(
    std::vector<EffectiveBinding>* bindings);

}  // namespace capsid::host

#endif
