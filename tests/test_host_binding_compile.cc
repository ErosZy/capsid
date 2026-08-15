// Binding v1 §7.8 host pipeline: the typed App binding parse and the
// Manifest ∩ App effective compile with static subset proofs.

#include "host/binding_compile.h"
#include "host/binding_registry.h"
#include "host/generation_identity.h"
#include "ipc_validation.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

using capsid::host::AppBindingRequest;
using capsid::host::BindingPackageSnapshot;
using capsid::host::BindingRegistrySnapshot;
using capsid::host::compile_effective_bindings;
using capsid::host::parse_app_bindings;

void fail(const std::string &message) {
    std::cerr << "test-host-binding-compile: " << message << std::endl;
    std::exit(1);
}

void require(bool condition, const std::string &message) {
    if (!condition) {
        fail(message);
    }
}

const std::string kManifest =
    R"json({"apiVersion":"capsid/binding-v1","sandbox":{"requires":["network-client","filesystem-read"]},"permissions":{"modules":["tjs:internal/core","tjs:posix-socket"],"net":{"allow":["*.internal.example.com:443","10.0.0.0/8:3306","127.0.0.1:27017"]},"fs":{"read":["/etc/capsid/mongo"],"write":[]},"env":["APP_MODE"],"stdio":[]}})json";

BindingPackageSnapshot package(const std::string &id,
                               const std::string &manifest) {
    BindingPackageSnapshot snapshot;
    snapshot.id = id;
    snapshot.manifest_json = manifest;
    snapshot.source = "export default () => ({});";
    snapshot.manifest_digest = "sha256:" + std::string(64, 'a');
    snapshot.source_digest = "sha256:" + std::string(64, 'b');
    return snapshot;
}

void test_parse_app_bindings() {
    const std::string app_json =
        R"json({"apiVersion":"capsid/app-v2","entry":"bundle.mjs","pool":{"minReady":1,"maxWorkers":1},"bindings":{"mongo":{"permissions":{"net":{"allow":["127.0.0.1:27017"]},"fs":{"read":["/etc/capsid/mongo/ca.pem"],"write":[]},"env":["APP_MODE"],"stdio":[]},"config":{"database":"orders","tls":true},"secrets":{"password":{"valueFrom":"mongo-password"}}}}})json";
    std::vector<AppBindingRequest> requests;
    std::string error;
    require(parse_app_bindings(
                std::vector<std::uint8_t>(app_json.begin(), app_json.end()),
                &requests, &error),
            "app bindings parse failed: " + error);
    require(requests.size() == 1 && requests[0].id == "mongo",
            "binding request parsed wrong");
    require(requests[0].net_rules.size() == 1 &&
                requests[0].net_rules[0] == "127.0.0.1:27017" &&
                requests[0].fs_read.size() == 1 &&
                requests[0].fs_read[0] == "/etc/capsid/mongo/ca.pem" &&
                requests[0].env.size() == 1 &&
                requests[0].env[0] == "APP_MODE",
            "binding permission request parsed wrong");
    require(requests[0].secrets.size() == 1 &&
                requests[0].secrets[0].name == "password" &&
                requests[0].secrets[0].key_id == "mongo-password",
            "binding secret refs parsed wrong");
    require(requests[0].config_json.find("\"database\"") !=
                std::string::npos,
            "binding config parsed wrong");

    // Zero bindings.
    const std::string no_bindings =
        R"json({"apiVersion":"capsid/app-v2","entry":"bundle.mjs","pool":{"minReady":1,"maxWorkers":1}})json";
    std::vector<AppBindingRequest> empty;
    require(parse_app_bindings(
                std::vector<std::uint8_t>(no_bindings.begin(),
                                          no_bindings.end()),
                &empty, &error),
            "zero-binding parse failed");
    require(empty.empty(), "zero-binding parse produced entries");
}

void test_compile_effective_bindings() {
    BindingRegistrySnapshot registry;
    registry.packages.push_back(package("mongo", kManifest));

    AppBindingRequest request;
    request.id = "mongo";
    request.net_rules = {"127.0.0.1:27017", "orders.internal.example.com:443",
                         "10.1.2.0/24:3306"};
    request.fs_read = {"/etc/capsid/mongo/ca.pem"};
    request.env = {"APP_MODE"};
    request.config_json = R"json({"database":"orders"})json";
    request.secrets = {{"password", "mongo-password"}};

    const capsid::host::BindingCompileResult result =
        compile_effective_bindings(registry, {request}, "rev-1");
    require(result.ok, "effective compile failed: " + result.error);
    require(result.bindings.size() == 1 &&
                result.bindings[0].id == "mongo",
            "effective bindings are wrong");
    require(!result.set_digest.empty() &&
                result.set_digest.rfind("sha256:", 0) == 0,
            "set digest is not a sha256 digest");
    require(result.bindings[0].profiles.size() == 2 &&
                result.bindings[0].modules.size() == 2,
            "manifest fields were not carried into the effective binding");
    require(result.bindings[0].digest_entry.secret_revision == "rev-1" &&
                result.bindings[0].digest_entry.secret_key_ids.size() == 1,
            "digest entry secret fields are wrong");
}

void test_subset_proofs_fail_closed() {
    BindingRegistrySnapshot registry;
    registry.packages.push_back(package("mongo", kManifest));

    const auto try_compile = [&registry](AppBindingRequest request) {
        return compile_effective_bindings(registry, {request}, "rev-1");
    };

    // Net target outside the manifest.
    {
        AppBindingRequest request;
        request.id = "mongo";
        request.net_rules = {"evil.example.com:27017"};
        const auto result = try_compile(request);
        require(!result.ok && !result.error.empty(),
                "uncovered net target was accepted");
    }
    // Wrong port.
    {
        AppBindingRequest request;
        request.id = "mongo";
        request.net_rules = {"127.0.0.1:9999"};
        const auto result = try_compile(request);
        require(!result.ok, "wrong-port net target was accepted");
    }
    // Wildcard host cannot be covered by a concrete manifest rule.
    {
        AppBindingRequest request;
        request.id = "mongo";
        request.net_rules = {"*:27017"};
        const auto result = try_compile(request);
        require(!result.ok, "any-host app rule was accepted");
    }
    // Subnet wider than the manifest block.
    {
        AppBindingRequest request;
        request.id = "mongo";
        request.net_rules = {"10.0.0.0/7:3306"};
        const auto result = try_compile(request);
        require(!result.ok, "wider CIDR app rule was accepted");
    }
    // fs path outside the manifest root.
    {
        AppBindingRequest request;
        request.id = "mongo";
        request.fs_read = {"/var/lib/capsid/mongo"};
        const auto result = try_compile(request);
        require(!result.ok, "uncovered fs read path was accepted");
    }
    // env name outside the manifest list.
    {
        AppBindingRequest request;
        request.id = "mongo";
        request.env = {"APP_SECRET"};
        const auto result = try_compile(request);
        require(!result.ok, "uncovered env name was accepted");
    }
    // Unknown binding id.
    {
        AppBindingRequest request;
        request.id = "redis";
        const auto result = try_compile(request);
        require(!result.ok, "uninstalled binding was accepted");
    }
}

void test_digest_sensitivity_and_immutability() {
    BindingRegistrySnapshot registry;
    registry.packages.push_back(package("mongo", kManifest));
    AppBindingRequest request;
    request.id = "mongo";
    request.net_rules = {"127.0.0.1:27017"};
    request.config_json = R"json({"database":"orders"})json";
    request.secrets = {{"password", "mongo-password"}};

    const std::string baseline =
        compile_effective_bindings(registry, {request}, "rev-1").set_digest;

    // Config change alters the digest.
    AppBindingRequest changed = request;
    changed.config_json = R"json({"database":"archive"})json";
    require(compile_effective_bindings(registry, {changed}, "rev-1")
                .set_digest != baseline,
            "config change did not alter the set digest");

    // Secret revision change alters the digest (values are not inputs at
    // all — the compile API takes no secret value).
    require(compile_effective_bindings(registry, {request}, "rev-2")
                .set_digest != baseline,
            "secret revision change did not alter the set digest");

    // Manifest change alters the digest.
    BindingRegistrySnapshot changed_registry;
    changed_registry.packages.push_back(package("mongo",
        R"json({"apiVersion":"capsid/binding-v1","permissions":{"modules":["tjs:utils"]}})json"));
    require(compile_effective_bindings(changed_registry, {request}, "rev-1")
                .set_digest != baseline,
            "manifest change did not alter the set digest");
}

void test_snapshot_round_trip() {
    BindingRegistrySnapshot registry;
    registry.packages.push_back(package("mongo", kManifest));
    AppBindingRequest request;
    request.id = "mongo";
    request.net_rules = {"127.0.0.1:27017"};
    request.fs_read = {"/etc/capsid/mongo/ca.pem"};
    request.config_json = R"json({"database":"orders"})json";
    request.secrets = {{"password", "mongo-password"}};

    const auto compiled =
        compile_effective_bindings(registry, {request}, "rev-1");
    require(compiled.ok, "compile failed");
    const std::string serialized =
        capsid::host::serialize_bindings_snapshot(compiled.bindings);
    require(!serialized.empty(), "snapshot serialization failed");
    require(serialized.find("password-value") == std::string::npos &&
                serialized.find("secret") != std::string::npos ==
                    (serialized.find("mongo-password") !=
                     std::string::npos),
            "snapshot content is unexpected");

    const auto parsed =
        capsid::host::parse_bindings_snapshot(serialized);
    require(parsed.ok, "snapshot parse failed: " + parsed.error);
    require(parsed.bindings.size() == 1 &&
                parsed.bindings[0].id == "mongo" &&
                parsed.bindings[0].package.manifest_json == kManifest &&
                parsed.bindings[0].request.config_json ==
                    R"json({"database":"orders"})json" &&
                parsed.bindings[0].request.net_rules.size() == 1 &&
                parsed.bindings[0].request.secrets.size() == 1 &&
                parsed.bindings[0].request.secrets[0].name == "password" &&
                parsed.bindings[0].request.secrets[0].key_id ==
                    "mongo-password" &&
                parsed.bindings[0].secret_values.empty(),
            "snapshot round-trip is wrong");
    // The parse recomputes digests from the committed bytes — it never
    // trusts serialized digest fields (there are none).
    require(parsed.bindings[0].package.manifest_digest ==
                capsid::host::compute_binding_manifest_digest(kManifest) &&
                !parsed.bindings[0].package.manifest_digest.empty() &&
                parsed.bindings[0].package.source_digest.rfind("sha256:", 0) ==
                    0,
            "snapshot digests were not recomputed");

    const auto malformed =
        capsid::host::parse_bindings_snapshot("not json");
    require(!malformed.ok, "malformed snapshot was accepted");
}

void test_worker_ready_verification() {
    const std::string compat = "sha256:" + std::string(64, 'a');
    std::vector<std::uint8_t> baseline(compat.begin(), compat.end());
    std::string error;

    // Zero-binding baseline passes.
    require(capsid::host::verify_worker_ready(
                baseline, compat, {}, 0, 0, "", &error),
            "zero-binding baseline was rejected: " + error);
    // A zero-binding worker reporting an extended proof is rejected.
    std::vector<std::uint8_t> extended = baseline;
    capsid::append_ready_proof(
        &extended, 0, 2, 3, "",
        "sha256:" + std::string(64, 'b'));
    require(!capsid::host::verify_worker_ready(
                extended, compat, {}, 0, 0, "", &error) &&
                !error.empty(),
            "zero-binding extended READY was accepted");

    // A binding worker's proof must carry the Host's expected profile
    // digest.
    BindingRegistrySnapshot registry;
    registry.packages.push_back(package("mongo", kManifest));
    AppBindingRequest request;
    request.id = "mongo";
    request.config_json = R"json({"database":"orders"})json";
    const auto compiled =
        compile_effective_bindings(registry, {request}, "rev-1");
    require(compiled.ok, "compile failed");
    const std::string expected_digest =
        capsid::host::compute_effective_profile_digest(
            compiled.bindings);
    require(expected_digest.rfind("sha256:", 0) == 0 &&
                expected_digest.size() == 71,
            "expected profile digest is not a sha256 digest");

    std::vector<std::uint8_t> good = baseline;
    capsid::append_ready_proof(&good, 0, 2, 3, "", expected_digest);
    require(capsid::host::verify_worker_ready(
                good, compat, compiled.bindings, 0, 0, "", &error),
            "matching binding READY was rejected: " + error);

    std::vector<std::uint8_t> bad = baseline;
    capsid::append_ready_proof(
        &bad, 0, 2, 3, "",
        "sha256:" + std::string(64, 'f'));
    require(!capsid::host::verify_worker_ready(
                bad, compat, compiled.bindings, 0, 0, "", &error) &&
                !error.empty(),
            "digest-mismatched binding READY was accepted");

    require(!capsid::host::verify_worker_ready(
                baseline, compat, compiled.bindings, 0, 0, "", &error) &&
                !error.empty(),
            "binding worker with a baseline READY was accepted");

    std::vector<std::uint8_t> malformed = baseline;
    malformed.pop_back();
    require(!capsid::host::verify_worker_ready(
                malformed, compat, {}, 0, 0, "", &error),
            "malformed READY was accepted");
}

}  // namespace

int main() {
    test_worker_ready_verification();
    test_snapshot_round_trip();
    test_parse_app_bindings();
    test_compile_effective_bindings();
    test_subset_proofs_fail_closed();
    test_digest_sensitivity_and_immutability();
    return 0;
}
