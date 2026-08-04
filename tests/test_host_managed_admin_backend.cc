// Frozen M1D managed Admin adapter RED suite.
//
// The HTTP layer may only expose the already-verified managed coordinator.
// This suite freezes two distinct production roles:
//   1. ManagedAdminBackend maps an exact App ID to its ManagedHostOptions and
//      calls the real managed_* coordinator functions.
//   2. AsyncAdminBackend places deploy/retire work on a bounded background
//      queue, returns an operation ID immediately, and owns observable
//      validating -> terminal operation status without leaking inner errors.

#if __has_include("host/managed_admin_backend.h")
#include "host/managed_admin_backend.h"
#define CAPSID_HAS_MANAGED_ADMIN_BACKEND 1
#else
#define CAPSID_HAS_MANAGED_ADMIN_BACKEND 0
#endif

#include "host/admin_api.h"
#include "host/managed_host.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
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

#if CAPSID_HAS_MANAGED_ADMIN_BACKEND

bool valid_operation_id(const std::string& value) {
    if (value.empty() || value.size() > 64) {
        return false;
    }
    const auto alnum = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9');
    };
    if (!alnum(value.front())) {
        return false;
    }
    for (const char c : value) {
        if (!alnum(c) && c != '_' && c != '-') {
            return false;
        }
    }
    return true;
}

class BlockingAdminBackend final : public capsid::host::AdminBackend {
public:
    capsid::host::DeployOutcome deploy(
        const std::string& application,
        const std::string& version,
        capsid::host::OperationStatus* status) override {
        (void) application;
        std::unique_lock<std::mutex> lock(mutex_);
        ++calls_;
        started_ = true;
        condition_.notify_all();
        condition_.wait(lock, [&]() { return released_; });
        capsid::host::DeployOutcome outcome;
        outcome.operation_id = "inner-operation-must-not-escape";
        status->operation_id = outcome.operation_id;
        status->version = version;
        if (fail_) {
            outcome.error = sensitive_error_;
            status->state = capsid::host::OperationState::kFailed;
            status->error = sensitive_error_;
        } else {
            outcome.ok = true;
            status->state = capsid::host::OperationState::kActive;
        }
        return outcome;
    }

    capsid::host::DeployOutcome retire(
        const std::string& application,
        capsid::host::OperationStatus* status) override {
        return deploy(application, {}, status);
    }

    capsid::host::OperationStatus operation_status(
        const std::string& operation_id) override {
        capsid::host::OperationStatus status;
        status.operation_id = operation_id;
        status.state = capsid::host::OperationState::kFailed;
        status.error = "inner operation not found";
        return status;
    }

    std::string app_status(const std::string& application) override {
        (void) application;
        return "{\"active\":false}";
    }

    void wait_started() {
        std::unique_lock<std::mutex> lock(mutex_);
        require(condition_.wait_for(lock, std::chrono::seconds(2),
                                    [&]() { return started_; }),
                "async Admin operation never reached its backend");
    }

    void release(bool fail_operation = false) {
        std::lock_guard<std::mutex> lock(mutex_);
        fail_ = fail_operation;
        released_ = true;
        condition_.notify_all();
    }

    int calls() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return calls_;
    }

    const std::string& sensitive_error() const { return sensitive_error_; }

private:
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    bool started_ = false;
    bool released_ = false;
    bool fail_ = false;
    int calls_ = 0;
    std::string sensitive_error_ =
        "secret-canary-from-inner-managed-operation";
};

capsid::host::OperationStatus wait_terminal(
    capsid::host::AsyncAdminBackend* backend,
    const std::string& operation_id) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(2);
    for (;;) {
        const capsid::host::OperationStatus status =
            backend->operation_status(operation_id);
        if (status.state == capsid::host::OperationState::kActive ||
            status.state == capsid::host::OperationState::kFailed) {
            return status;
        }
        require(std::chrono::steady_clock::now() < deadline,
                "async Admin operation did not reach a terminal state");
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

struct StateFixture {
    std::string root;

    StateFixture() {
        root = "/tmp/capsid-managed-admin-XXXXXX";
        require(mkdtemp(root.data()) != nullptr,
                "cannot create managed Admin state fixture");
        require(mkdir((root + "/apps").c_str(), 0700) == 0,
                "cannot create managed Admin apps directory");
        require(mkdir((root + "/apps/orders").c_str(), 0700) == 0 &&
                    mkdir((root + "/apps/payments").c_str(), 0700) == 0,
                "cannot create managed Admin App directories");
        write_active("orders", "orders-v1", 'a');
        write_active("payments", "payments-v2", 'b');
    }

    ~StateFixture() {
        (void) unlink((root + "/apps/orders/active.json").c_str());
        (void) unlink((root + "/apps/payments/active.json").c_str());
        (void) rmdir((root + "/apps/orders").c_str());
        (void) rmdir((root + "/apps/payments").c_str());
        (void) rmdir((root + "/apps").c_str());
        (void) rmdir(root.c_str());
    }

    void write_active(const std::string& application,
                      const std::string& version,
                      char digest_byte) {
        const std::string path = root + "/apps/" + application +
                                 "/active.json";
        const std::string bytes =
            "{\"schema\":\"capsid-active-v1\",\"app\":\"" +
            application + "\",\"state\":\"active\",\"version\":\"" +
            version + "\",\"generation\":\"sha256:" +
            std::string(64, digest_byte) + "\"}";
        const int fd = open(path.c_str(),
                            O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        require(fd >= 0, "cannot create managed Admin active state");
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            const ssize_t count = write(fd, bytes.data() + offset,
                                        bytes.size() - offset);
            if (count < 0 && errno == EINTR) {
                continue;
            }
            require(count > 0, "cannot write managed Admin active state");
            offset += static_cast<std::size_t>(count);
        }
        require(close(fd) == 0, "cannot close managed Admin active state");
    }

    std::string read_active(const std::string& application) const {
        std::ifstream input(root + "/apps/" + application +
                            "/active.json", std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(input),
                           std::istreambuf_iterator<char>());
    }
};

#endif

}  // namespace

int main(int argc, char** argv) {
    require(argc == 2, "expected one managed Admin backend test mode");
    const std::string mode = argv[1];

#if !CAPSID_HAS_MANAGED_ADMIN_BACKEND
    fail("managed Admin backend is not implemented: " + mode);
#else
    if (mode == "host_admin_async_deploy_progress") {
        BlockingAdminBackend inner;
        capsid::host::AsyncAdminBackendOptions options;
        options.max_pending_operations = 4;
        capsid::host::AsyncAdminBackend backend(&inner, options);
        capsid::host::OperationStatus submitted;
        const auto started = std::chrono::steady_clock::now();
        const capsid::host::DeployOutcome outcome =
            backend.deploy("orders", "v1", &submitted);
        const auto elapsed = std::chrono::steady_clock::now() - started;
        require(outcome.ok && valid_operation_id(outcome.operation_id),
                "async Admin deploy was not accepted with a valid ID");
        require(elapsed < std::chrono::milliseconds(250),
                "Admin deploy blocked on the managed operation");
        require(submitted.operation_id == outcome.operation_id &&
                    submitted.version == "v1" &&
                    submitted.state ==
                        capsid::host::OperationState::kValidating,
                "Admin deploy did not publish its initial progress");
        inner.wait_started();
        const capsid::host::OperationStatus running =
            backend.operation_status(outcome.operation_id);
        require(running.operation_id == outcome.operation_id &&
                    running.state != capsid::host::OperationState::kActive &&
                    running.state != capsid::host::OperationState::kFailed,
                "async Admin operation became terminal before backend exit");
        inner.release();
        const capsid::host::OperationStatus terminal =
            wait_terminal(&backend, outcome.operation_id);
        require(terminal.operation_id == outcome.operation_id &&
                    terminal.version == "v1" &&
                    terminal.state == capsid::host::OperationState::kActive,
                "async Admin success lost its public operation identity");
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode == "host_admin_async_failure_and_capacity") {
        BlockingAdminBackend inner;
        capsid::host::AsyncAdminBackendOptions options;
        options.max_pending_operations = 1;
        capsid::host::AsyncAdminBackend backend(&inner, options);
        capsid::host::OperationStatus first_status;
        const capsid::host::DeployOutcome first =
            backend.deploy("orders", "v1", &first_status);
        require(first.ok, "first bounded Admin operation was not accepted");
        inner.wait_started();
        capsid::host::OperationStatus second_status;
        const capsid::host::DeployOutcome second =
            backend.retire("payments", &second_status);
        require(!second.ok && inner.calls() == 1,
                "Admin operation capacity was not fail-closed");
        inner.release(true);
        const capsid::host::OperationStatus terminal =
            wait_terminal(&backend, first.operation_id);
        require(terminal.state == capsid::host::OperationState::kFailed,
                "failed managed operation was not recorded as failed");
        require(terminal.error.find(inner.sensitive_error()) ==
                    std::string::npos,
                "async Admin registry retained a sensitive inner error");
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode == "host_managed_admin_routes_real_coordinator") {
        StateFixture state;
        capsid::host::ManagedHostOptions orders;
        orders.application = "orders";
        orders.state_root = state.root;
        capsid::host::ManagedHostOptions payments;
        payments.application = "payments";
        payments.state_root = state.root;
        std::vector<capsid::host::ManagedHostOptions*> applications = {
            &orders, &payments,
        };
        capsid::host::ManagedAdminBackend backend(applications);

        const std::string orders_status = backend.app_status("orders");
        const std::string payments_status = backend.app_status("payments");
        require(orders_status.find("orders-v1") != std::string::npos &&
                    orders_status.find("payments-v2") == std::string::npos &&
                    payments_status.find("payments-v2") !=
                        std::string::npos,
                "managed Admin backend routed App status to wrong options");

        capsid::host::OperationStatus retire_status;
        const capsid::host::DeployOutcome retired =
            backend.retire("orders", &retire_status);
        require(retired.ok && retire_status.operation_id ==
                                  retired.operation_id,
                "managed Admin backend did not invoke real retire");
        const std::string retired_state = state.read_active("orders");
        require(retired_state.find("\"state\":\"retired\"") !=
                    std::string::npos &&
                    retired_state.find("orders-v1") != std::string::npos,
                "real coordinator did not persist the retire tombstone");

        capsid::host::OperationStatus unknown_status;
        const capsid::host::DeployOutcome unknown =
            backend.deploy("unknown", "v1", &unknown_status);
        require(!unknown.ok,
                "managed Admin backend accepted an unconfigured App");
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode == "host_admin_managed_status_dispatch_round_trip") {
        StateFixture state;
        capsid::host::ManagedHostOptions orders;
        orders.application = "orders";
        orders.state_root = state.root;
        std::vector<capsid::host::ManagedHostOptions*> applications = {
            &orders,
        };
        capsid::host::ManagedAdminBackend managed(applications);
        capsid::host::AsyncAdminBackendOptions async_options;
        async_options.max_pending_operations = 2;
        capsid::host::AsyncAdminBackend backend(&managed, async_options);

        capsid::host::AdminApiOptions api_options;
        api_options.authorization.allowed_uid =
            static_cast<std::uint64_t>(geteuid());
        capsid::host::AdminPeerCredentials peer;
        peer.uid = static_cast<std::uint64_t>(geteuid());
        peer.gid = static_cast<std::uint64_t>(getegid());
        capsid::host::AdminRequest request;
        request.method = "GET";
        request.target = "/v1/apps/orders";
        request.header_bytes = 64;
        const capsid::host::AdminResponse response =
            capsid::host::handle_admin_request(
                api_options, peer, request, &backend);
        require(response.status == 200 &&
                    response.content_type == "application/json",
                "real managed App status did not cross Admin dispatch");
        require(response.body.find("\"app\":\"orders\"") !=
                    std::string::npos &&
                    response.body.find("orders-v1") != std::string::npos,
                "Admin dispatch lost or changed real managed App status");
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode == "host_admin_async_rejects_submission_after_stop") {
        BlockingAdminBackend inner;
        std::atomic<bool> stop{true};
        capsid::host::AsyncAdminBackendOptions options;
        options.max_pending_operations = 2;
        options.external_stop = &stop;
        capsid::host::AsyncAdminBackend backend(&inner, options);
        capsid::host::OperationStatus submitted;
        const capsid::host::DeployOutcome outcome =
            backend.deploy("orders", "v1", &submitted);
        require(!outcome.ok && inner.calls() == 0 &&
                    submitted.state == capsid::host::OperationState::kFailed,
                "Admin accepted a new operation after shutdown began");
        std::cout << "PASS" << std::endl;
        return 0;
    }

    fail("unknown managed Admin backend test mode: " + mode);
#endif
}
