// Frozen M1D Unix Admin API RED suite.
//
// The first-party Host's old --mode single-worker executable is a benchmark
// fixture, not a deployment API. This suite freezes the internal Admin
// boundary that a managed executable will own: Unix listener creation,
// kernel peer credentials, one global authorization decision, four exact
// routes, bounded requests and redacted errors. The production header is
// intentionally feature-detected so the pre-implementation tree still
// builds and produces explicit runtime REDs instead of one opaque compiler
// failure.

#if __has_include("host/admin_api.h")
#include "host/admin_api.h"
#define CAPSID_HAS_ADMIN_API 1
#else
#define CAPSID_HAS_ADMIN_API 0
#endif

#include "host/managed_host.h"

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

[[noreturn]] void fail(const std::string& message) {
    std::cerr << "FAIL: " << message << std::endl;
    std::exit(1);
}

void require(bool condition, const std::string& message) {
    if (!condition) {
        fail(message);
    }
}

#if CAPSID_HAS_ADMIN_API

class FakeAdminBackend final : public capsid::host::AdminBackend {
public:
    capsid::host::DeployOutcome deploy(
        const std::string& application,
        const std::string& version,
        capsid::host::OperationStatus* status) override {
        ++deploy_calls;
        last_application = application;
        last_version = version;
        capsid::host::DeployOutcome outcome;
        outcome.operation_id = deploy_operation_id;
        status->operation_id = outcome.operation_id;
        status->version = version;
        if (throw_deploy) {
            throw std::runtime_error(sensitive_error);
        }
        if (fail_deploy) {
            outcome.error = sensitive_error;
            status->state = capsid::host::OperationState::kFailed;
            status->error = sensitive_error;
            return outcome;
        }
        outcome.ok = true;
        status->state = capsid::host::OperationState::kActive;
        return outcome;
    }

    capsid::host::DeployOutcome retire(
        const std::string& application,
        capsid::host::OperationStatus* status) override {
        ++retire_calls;
        last_application = application;
        capsid::host::DeployOutcome outcome;
        outcome.ok = true;
        outcome.operation_id = "op-retire-1";
        status->operation_id = outcome.operation_id;
        status->state = capsid::host::OperationState::kActive;
        return outcome;
    }

    capsid::host::OperationStatus operation_status(
        const std::string& operation_id) override {
        ++operation_calls;
        last_operation = operation_id;
        capsid::host::OperationStatus status;
        status.operation_id = operation_status_id.empty()
            ? operation_id
            : operation_status_id;
        if (operation_id == "missing") {
            status.state = capsid::host::OperationState::kFailed;
            status.error = "operation not found";
            return status;
        }
        status.version = operation_status_version;
        status.state = capsid::host::OperationState::kActive;
        return status;
    }

    std::string app_status(const std::string& application) override {
        ++app_calls;
        last_application = application;
        if (!app_status_document.empty()) {
            return app_status_document;
        }
        return "{\"active\":true,\"app\":\"" + application +
               "\",\"version\":\"v1\",\"generation\":\"sha256:" +
               std::string(64, 'a') + "\"}";
    }

    int total_calls() const {
        return deploy_calls + retire_calls + operation_calls + app_calls;
    }

    int deploy_calls = 0;
    int retire_calls = 0;
    int operation_calls = 0;
    int app_calls = 0;
    bool fail_deploy = false;
    bool throw_deploy = false;
    std::string sensitive_error;
    std::string app_status_document;
    std::string deploy_operation_id = "op-deploy-1";
    std::string operation_status_id;
    std::string operation_status_version = "v1";
    std::string last_application;
    std::string last_version;
    std::string last_operation;
};

capsid::host::AdminApiOptions authorized_options() {
    capsid::host::AdminApiOptions options;
    options.authorization.allowed_uid = static_cast<std::uint64_t>(geteuid());
    options.max_header_bytes = 1024;
    options.max_body_bytes = 1024;
    return options;
}

capsid::host::AdminPeerCredentials current_peer() {
    capsid::host::AdminPeerCredentials peer;
    peer.uid = static_cast<std::uint64_t>(geteuid());
    peer.gid = static_cast<std::uint64_t>(getegid());
    return peer;
}

capsid::host::AdminRequest request(std::string method,
                                   std::string target,
                                   std::string content_type = {},
                                   std::string body = {}) {
    capsid::host::AdminRequest value;
    value.method = std::move(method);
    value.target = std::move(target);
    value.content_type = std::move(content_type);
    value.body = std::move(body);
    value.header_bytes = 64;
    return value;
}

capsid::host::AdminResponse dispatch(
    const capsid::host::AdminApiOptions& options,
    const capsid::host::AdminPeerCredentials& peer,
    const capsid::host::AdminRequest& value,
    FakeAdminBackend* backend) {
    return capsid::host::handle_admin_request(options, peer, value, backend);
}

void require_json_response(const capsid::host::AdminResponse& response,
                           unsigned status,
                           const std::string& label) {
    require(response.status == status,
            label + " returned HTTP " + std::to_string(response.status));
    require(response.content_type == "application/json",
            label + " did not return application/json");
    require(!response.body.empty() && response.body.front() == '{' &&
                response.body.back() == '}',
            label + " did not return one JSON object");
}

template <typename Options>
bool configure_socket_group(Options* options, std::uint64_t gid) {
    if constexpr (requires(Options& value, std::uint64_t group) {
                      value.group_gid = group;
                  }) {
        options->group_gid = gid;
        return true;
    }
    (void) options;
    (void) gid;
    return false;
}

#endif

}  // namespace

int main(int argc, char** argv) {
    require(argc == 2, "expected one Admin API test mode");
    const std::string mode = argv[1];

#if !CAPSID_HAS_ADMIN_API
    fail("Unix Admin API is not implemented: " + mode);
#else
    if (mode == "host_admin_peer_credentials") {
        int sockets[2] = {-1, -1};
        require(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0,
                "cannot create Admin credential socketpair");
        capsid::host::AdminPeerCredentials peer;
        std::string error;
        require(capsid::host::query_admin_peer_credentials(
                    sockets[0], &peer, &error),
                "cannot query kernel Admin peer credentials: " + error);
        close(sockets[0]);
        close(sockets[1]);
        require(peer.uid == static_cast<std::uint64_t>(geteuid()) &&
                    peer.gid == static_cast<std::uint64_t>(getegid()),
                "Admin peer credentials were not sourced from the kernel");
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode == "host_admin_socket_mode") {
        std::string directory = "/tmp/capsid-admin-api-XXXXXX";
        require(mkdtemp(directory.data()) != nullptr,
                "cannot create Admin socket fixture directory");
        const std::string path = directory + "/admin.sock";
        capsid::host::AdminSocketOptions options;
        options.path = path;
        options.mode = 0600;
        options.backlog = 4;
        int listener = -1;
        std::string error;
        require(capsid::host::open_admin_listener(options, &listener, &error),
                "cannot open Admin listener: " + error);
        struct stat metadata = {};
        require(listener >= 0 && lstat(path.c_str(), &metadata) == 0,
                "Admin listener did not publish its Unix socket");
        require(S_ISSOCK(metadata.st_mode),
                "Admin listener path is not a socket");
        require((metadata.st_mode & 0777) == 0600,
                "Admin listener ignored the configured 0600 mode");
        require(metadata.st_uid == geteuid(),
                "Admin listener is not owned by the Host euid");
        const int descriptor_flags = fcntl(listener, F_GETFD);
        require(descriptor_flags >= 0 &&
                    (descriptor_flags & FD_CLOEXEC) != 0,
                "Admin listener can leak across worker exec");
        close(listener);
        require(unlink(path.c_str()) == 0,
                "cannot remove Admin listener fixture");
        require(rmdir(directory.c_str()) == 0,
                "cannot remove Admin socket fixture directory");
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode == "host_admin_global_authorization") {
        capsid::host::AdminApiOptions options = authorized_options();
        options.authorization.allowed_uid =
            static_cast<std::uint64_t>(geteuid()) + 1U;
        options.authorization.allowed_gid = std::nullopt;
        const capsid::host::AdminPeerCredentials peer = current_peer();
        FakeAdminBackend backend;
        const capsid::host::AdminRequest requests[] = {
            request("POST", "/v1/deploy", "application/json",
                    "{\"app\":\"orders\",\"version\":\"v1\"}"),
            request("POST", "/v1/apps/orders/retire"),
            request("GET", "/v1/operations/op-deploy-1"),
            request("GET", "/v1/apps/orders"),
        };
        for (const capsid::host::AdminRequest& value : requests) {
            require_json_response(dispatch(options, peer, value, &backend),
                                  403, "unauthorized Admin endpoint");
        }
        require(backend.total_calls() == 0,
                "unauthorized Admin peer reached a coordinator callback");

        // A configured management group is an OR authority, not a per-App
        // ACL: the same peer becomes authorized for every endpoint.
        options.authorization.allowed_gid = peer.gid;
        require(capsid::host::admin_peer_authorized(options.authorization,
                                                    peer),
                "configured Admin group did not authorize its peer");
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode == "host_admin_four_endpoints") {
        const capsid::host::AdminApiOptions options = authorized_options();
        const capsid::host::AdminPeerCredentials peer = current_peer();
        FakeAdminBackend backend;

        const capsid::host::AdminResponse deployed = dispatch(
            options, peer,
            request("POST", "/v1/deploy", "application/json",
                    "{\"app\":\"orders\",\"version\":\"v1\"}"),
            &backend);
        require_json_response(deployed, 202, "deploy endpoint");
        require(backend.deploy_calls == 1 &&
                    backend.last_application == "orders" &&
                    backend.last_version == "v1",
                "deploy endpoint changed App/Version dispatch");
        require(deployed.body.find("op-deploy-1") != std::string::npos &&
                    deployed.body.find("orders") != std::string::npos &&
                    deployed.body.find("v1") != std::string::npos,
                "deploy response omitted its operation identity");

        const capsid::host::AdminResponse retired = dispatch(
            options, peer, request("POST", "/v1/apps/orders/retire"),
            &backend);
        require_json_response(retired, 202, "retire endpoint");
        require(backend.retire_calls == 1 &&
                    backend.last_application == "orders",
                "retire endpoint dispatched the wrong App");

        const capsid::host::AdminResponse operation = dispatch(
            options, peer,
            request("GET", "/v1/operations/op-deploy-1"), &backend);
        require_json_response(operation, 200, "operation endpoint");
        require(backend.operation_calls == 1 &&
                    backend.last_operation == "op-deploy-1" &&
                    operation.body.find("op-deploy-1") != std::string::npos,
                "operation endpoint returned the wrong operation");

        const capsid::host::AdminResponse app = dispatch(
            options, peer, request("GET", "/v1/apps/orders"), &backend);
        require_json_response(app, 200, "App endpoint");
        require(backend.app_calls == 1 &&
                    backend.last_application == "orders" &&
                    app.body.find("\"app\":\"orders\"") !=
                        std::string::npos,
                "App endpoint returned another App's state");
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode == "host_admin_request_limits") {
        const capsid::host::AdminApiOptions options = authorized_options();
        const capsid::host::AdminPeerCredentials peer = current_peer();
        FakeAdminBackend backend;

        capsid::host::AdminRequest large_header =
            request("GET", "/v1/apps/orders");
        large_header.header_bytes = options.max_header_bytes + 1;
        require_json_response(dispatch(options, peer, large_header, &backend),
                              431, "oversized Admin header");
        require_json_response(
            dispatch(options, peer,
                     request("POST", "/v1/deploy", "application/json",
                             std::string(options.max_body_bytes + 1, 'x')),
                     &backend),
            413, "oversized Admin body");
        require_json_response(
            dispatch(options, peer,
                     request("POST", "/v1/deploy", "text/plain", "{}"),
                     &backend),
            415, "wrong Admin content type");
        require(backend.total_calls() == 0,
                "bounded Admin rejection reached a coordinator callback");
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode == "host_admin_strict_requests_and_redaction") {
        const capsid::host::AdminApiOptions options = authorized_options();
        const capsid::host::AdminPeerCredentials peer = current_peer();
        FakeAdminBackend backend;
        const std::string bad_bodies[] = {
            "{",
            "{\"app\":\"orders\",\"app\":\"other\",\"version\":\"v1\"}",
            "{\"app\":\"orders\",\"version\":\"v1\",\"mystery\":true}",
            "{\"app\":\"../orders\",\"version\":\"v1\"}",
            "{\"app\":\"orders\",\"version\":\"../v1\"}",
        };
        for (const std::string& body : bad_bodies) {
            require_json_response(
                dispatch(options, peer,
                         request("POST", "/v1/deploy", "application/json",
                                 body),
                         &backend),
                400, "strict deploy request");
        }
        require_json_response(
            dispatch(options, peer,
                     request("GET", "/v1/apps/orders", {}, "not-empty"),
                     &backend),
            400, "GET request body");
        require(backend.total_calls() == 0,
                "malformed Admin request reached a coordinator callback");

        const std::string canary =
            "secret-canary-and-bundle-bytes-must-not-reach-admin";
        backend.fail_deploy = true;
        backend.sensitive_error = canary;
        const capsid::host::AdminResponse failed = dispatch(
            options, peer,
            request("POST", "/v1/deploy", "application/json",
                    "{\"app\":\"orders\",\"version\":\"v1\"}"),
            &backend);
        require(failed.status >= 400 && failed.status <= 599,
                "failed coordinator operation returned success");
        require(failed.body.find(canary) == std::string::npos,
                "Admin response leaked an internal/secret error");
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode == "host_admin_identifier_grammar") {
        const capsid::host::AdminApiOptions options = authorized_options();
        const capsid::host::AdminPeerCredentials peer = current_peer();
        FakeAdminBackend backend;
        const std::string bad_deploy_bodies[] = {
            "{\"app\":\"Orders\",\"version\":\"v1\"}",
            "{\"app\":\"oRders\",\"version\":\"v1\"}",
            "{\"app\":\"orders?debug\",\"version\":\"v1\"}",
            "{\"app\":\"orders\\\\other\",\"version\":\"v1\"}",
            "{\"app\":\"orders\\u0000other\",\"version\":\"v1\"}",
            "{\"app\":\"" + std::string(64, 'a') +
                "\",\"version\":\"v1\"}",
            "{\"app\":\"orders\",\"version\":\"v1?debug\"}",
            "{\"app\":\"orders\",\"version\":\"" +
                std::string(129, 'v') + "\"}",
        };
        for (const std::string& body : bad_deploy_bodies) {
            require_json_response(
                dispatch(options, peer,
                         request("POST", "/v1/deploy", "application/json",
                                 body),
                         &backend),
                400, "invalid Admin App/Version ID");
        }
        const std::string bad_targets[] = {
            "/v1/apps/Orders",
            "/v1/apps/orders%2fother",
            "/v1/apps/" + std::string(64, 'a'),
            "/v1/operations/op.bad",
            "/v1/operations/" + std::string(65, 'a'),
        };
        for (const std::string& target : bad_targets) {
            require_json_response(
                dispatch(options, peer, request("GET", target), &backend),
                400, "invalid Admin path identifier");
        }
        require(backend.total_calls() == 0,
                "invalid Admin identifier reached a coordinator callback");
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode == "host_admin_backend_output_validation") {
        const capsid::host::AdminApiOptions options = authorized_options();
        const capsid::host::AdminPeerCredentials peer = current_peer();
        const std::string canary =
            "secret-canary-from-invalid-backend-output";

        FakeAdminBackend deploy_backend;
        deploy_backend.deploy_operation_id = canary + ".invalid";
        const capsid::host::AdminResponse deploy = dispatch(
            options, peer,
            request("POST", "/v1/deploy", "application/json",
                    "{\"app\":\"orders\",\"version\":\"v1\"}"),
            &deploy_backend);
        require_json_response(deploy, 500,
                              "invalid backend operation ID");
        require(deploy.body.find(canary) == std::string::npos,
                "Admin API reflected an invalid backend operation ID");

        FakeAdminBackend operation_backend;
        operation_backend.operation_status_version = canary + "?invalid";
        const capsid::host::AdminResponse operation = dispatch(
            options, peer,
            request("GET", "/v1/operations/op-deploy-1"),
            &operation_backend);
        require_json_response(operation, 500,
                              "invalid backend operation status");
        require(operation.body.find(canary) == std::string::npos,
                "Admin API reflected invalid operation status data");

        FakeAdminBackend app_backend;
        app_backend.app_status_document =
            "{\"active\":true,\"app\":\"payments\","
            "\"version\":\"v1\",\"generation\":\"sha256:" +
            std::string(64, 'a') + "\"}";
        require_json_response(
            dispatch(options, peer, request("GET", "/v1/apps/orders"),
                     &app_backend),
            500, "mismatched backend App status");

        FakeAdminBackend app_version_backend;
        app_version_backend.app_status_document =
            "{\"active\":true,\"app\":\"orders\",\"version\":\"" +
            canary + "?invalid\",\"generation\":\"sha256:" +
            std::string(64, 'a') + "\"}";
        const capsid::host::AdminResponse app_version = dispatch(
            options, peer, request("GET", "/v1/apps/orders"),
            &app_version_backend);
        require_json_response(app_version, 500,
                              "invalid backend App version");
        require(app_version.body.find(canary) == std::string::npos,
                "Admin API reflected an invalid App status version");
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode == "host_admin_submission_status") {
        const capsid::host::AdminApiOptions options = authorized_options();
        const capsid::host::AdminPeerCredentials peer = current_peer();
        FakeAdminBackend backend;
        const capsid::host::AdminResponse response = dispatch(
            options, peer,
            request("POST", "/v1/deploy", "application/json",
                    "{\"app\":\"orders\",\"version\":\"v1\"}"),
            &backend);
        require_json_response(response, 202, "Admin deployment submission");
        require(response.body.find("\"status\":\"active\"") !=
                    std::string::npos,
                "Admin deployment response omitted its submitted status");
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode == "host_admin_bodyless_routes_and_safe_status") {
        const capsid::host::AdminApiOptions options = authorized_options();
        const capsid::host::AdminPeerCredentials peer = current_peer();
        FakeAdminBackend backend;
        require_json_response(
            dispatch(options, peer,
                     request("POST", "/v1/apps/orders/retire",
                             "application/json", "{}"),
                     &backend),
            400, "retire request body");
        require(backend.total_calls() == 0,
                "body-carrying retire reached the coordinator");

        const std::string canary =
            "secret-canary-must-not-cross-app-status-boundary";
        backend.app_status_document =
            "{\"active\":false,\"environment\":\"" + canary + "\"}";
        const capsid::host::AdminResponse status = dispatch(
            options, peer, request("GET", "/v1/apps/orders"), &backend);
        require_json_response(status, 500, "unsafe App status document");
        require(status.body.find(canary) == std::string::npos,
                "Admin API reflected unsafe backend App status data");
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode == "host_admin_backend_exception_redacted") {
        const capsid::host::AdminApiOptions options = authorized_options();
        const capsid::host::AdminPeerCredentials peer = current_peer();
        FakeAdminBackend backend;
        const std::string canary =
            "secret-canary-from-throwing-admin-backend";
        backend.throw_deploy = true;
        backend.sensitive_error = canary;
        capsid::host::AdminResponse response;
        try {
            response = dispatch(
                options, peer,
                request("POST", "/v1/deploy", "application/json",
                        "{\"app\":\"orders\",\"version\":\"v1\"}"),
                &backend);
        } catch (...) {
            fail("Admin backend exception crossed the request boundary");
        }
        require_json_response(response, 500, "throwing Admin backend");
        require(response.body.find(canary) == std::string::npos,
                "Admin response reflected a backend exception");
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode == "host_admin_socket_group") {
        std::string directory = "/tmp/capsid-admin-group-XXXXXX";
        require(mkdtemp(directory.data()) != nullptr,
                "cannot create grouped Admin socket fixture");
        const std::string path = directory + "/admin.sock";
        capsid::host::AdminSocketOptions options;
        options.path = path;
        options.mode = 0660;
        options.backlog = 4;
        const int group_count = getgroups(0, nullptr);
        require(group_count >= 0, "cannot enumerate Admin test groups");
        std::vector<gid_t> groups(static_cast<std::size_t>(group_count));
        if (group_count > 0) {
            require(getgroups(group_count, groups.data()) == group_count,
                    "cannot read Admin test groups");
        }
        gid_t expected_group = getegid();
        for (const gid_t group : groups) {
            if (group != getegid()) {
                expected_group = group;
                break;
            }
        }
        if (expected_group == getegid()) {
            // This machine has no distinct supplementary group with which
            // pathname ownership can be observed. Keep the test portable;
            // Linux CI and the M1 target machines provide one.
            require(rmdir(directory.c_str()) == 0,
                    "cannot clean untestable Admin group fixture");
            std::cout << "PASS" << std::endl;
            return 0;
        }
        const bool has_group = configure_socket_group(
            &options, static_cast<std::uint64_t>(expected_group));
        if (!has_group) {
            (void) rmdir(directory.c_str());
        }
        require(has_group,
                "Admin socket options do not expose the fixed management "
                "group");
        int listener = -1;
        std::string error;
        require(capsid::host::open_admin_listener(options, &listener, &error),
                "cannot open grouped Admin listener: " + error);
        struct stat metadata = {};
        require(lstat(path.c_str(), &metadata) == 0 &&
                    S_ISSOCK(metadata.st_mode),
                "grouped Admin listener path is not a socket");
        const bool exact_identity =
            metadata.st_uid == geteuid() &&
            metadata.st_gid == expected_group &&
            (metadata.st_mode & 0777) == 0660;
        close(listener);
        require(unlink(path.c_str()) == 0 && rmdir(directory.c_str()) == 0,
                "cannot clean grouped Admin socket fixture");
        require(exact_identity,
                "Admin listener did not apply its exact owner/group/mode");
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode == "host_admin_stale_socket_recovery") {
        std::string directory = "/tmp/capsid-admin-stale-XXXXXX";
        require(mkdtemp(directory.data()) != nullptr,
                "cannot create stale Admin socket fixture");
        const std::string path = directory + "/admin.sock";
        capsid::host::AdminSocketOptions options;
        options.path = path;
        options.mode = 0600;
        options.backlog = 4;
        int first = -1;
        std::string error;
        require(capsid::host::open_admin_listener(options, &first, &error),
                "cannot create initial Admin listener");
        close(first);  // crash residue: pathname remains, no live listener

        int replacement = -1;
        const bool recovered = capsid::host::open_admin_listener(
            options, &replacement, &error);
        if (!recovered) {
            (void) unlink(path.c_str());
            (void) rmdir(directory.c_str());
        }
        require(recovered,
                "Host-owned stale Admin socket was not recovered: " + error);

        // A second opener must recognize that replacement as live and must
        // neither steal nor unlink it.
        int competing = -1;
        require(!capsid::host::open_admin_listener(
                    options, &competing, &error),
                "active Admin listener was replaced as stale");
        require(competing == -1,
                "failed Admin listener unexpectedly returned an fd");
        struct stat metadata = {};
        require(lstat(path.c_str(), &metadata) == 0 &&
                    S_ISSOCK(metadata.st_mode),
                "active Admin listener pathname was removed");
        const int client = socket(AF_UNIX, SOCK_STREAM, 0);
        require(client >= 0, "cannot create active-listener probe");
        struct sockaddr_un address = {};
        address.sun_family = AF_UNIX;
        std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
        require(connect(client,
                        reinterpret_cast<const struct sockaddr*>(&address),
                        sizeof(address)) == 0,
                "preserved Admin listener no longer accepts connections");
        close(client);
        close(replacement);
        require(unlink(path.c_str()) == 0 && rmdir(directory.c_str()) == 0,
                "cannot clean stale Admin socket fixture");
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode == "host_admin_socket_path_fail_closed") {
        std::string directory = "/tmp/capsid-admin-path-XXXXXX";
        require(mkdtemp(directory.data()) != nullptr,
                "cannot create Admin path fixture");
        const std::string path = directory + "/admin.sock";
        require(chmod(directory.c_str(), 0777) == 0,
                "cannot make Admin parent intentionally insecure");
        capsid::host::AdminSocketOptions options;
        options.path = path;
        options.mode = 0600;
        int listener = -1;
        std::string error;
        const bool insecure_opened = capsid::host::open_admin_listener(
            options, &listener, &error);
        if (listener >= 0) {
            close(listener);
        }
        (void) unlink(path.c_str());
        require(chmod(directory.c_str(), 0700) == 0,
                "cannot restore Admin parent fixture mode");
        if (insecure_opened) {
            (void) rmdir(directory.c_str());
        }
        require(!insecure_opened,
                "Admin listener accepted a group/world-writable parent");

        const int regular = open(path.c_str(),
                                 O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
                                 0600);
        require(regular >= 0, "cannot create Admin collision fixture");
        require(close(regular) == 0, "cannot close collision fixture");
        listener = -1;
        require(!capsid::host::open_admin_listener(
                    options, &listener, &error),
                "Admin listener replaced a non-socket pathname");
        struct stat metadata = {};
        require(lstat(path.c_str(), &metadata) == 0 &&
                    S_ISREG(metadata.st_mode),
                "Admin listener removed or changed a collision file");
        require(unlink(path.c_str()) == 0 && rmdir(directory.c_str()) == 0,
                "cannot clean Admin collision fixture");
        std::cout << "PASS" << std::endl;
        return 0;
    }

    fail("unknown Admin API test mode: " + mode);
#endif
}
