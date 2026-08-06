// Frozen RED suite: the M1D managed host (modes registered as the frozen
// test names). Drives the real deploy pipeline: safe-read, attestation
// selection, policy/secret compilation, staging, worker warm-up with the
// real capsid-worker, active.json persistence, retire and recovery.

#include "host/managed_host.h"
#include "host/managed_admin_backend.h"
#include "host/bytecode_attestation.h"
#include "host/secret_snapshot.h"

#include "capsid/runtime.h"

#include <openssl/evp.h>

#include <algorithm>
#include <atomic>
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <barrier>
#include <cerrno>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <poll.h>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(__SANITIZE_THREAD__)
#define CAPSID_TEST_TSAN_BUILD 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define CAPSID_TEST_TSAN_BUILD 1
#endif
#endif
#if !defined(CAPSID_TEST_TSAN_BUILD)
#define CAPSID_TEST_TSAN_BUILD 0
#endif

#include "build_identity.h"

namespace {

void fail(const std::string& message) {
    std::cerr << "FAIL: " << message << std::endl;
    std::exit(1);
}

void require(bool condition, const std::string& message) {
    if (!condition) {
        fail(message);
    }
}

void write_file(const std::string& path, const std::string& content) {
    const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    require(fd >= 0, "cannot create fixture file: " + path);
    require(content.empty() ||
                write(fd, content.data(), content.size()) ==
                    static_cast<ssize_t>(content.size()),
            "cannot write fixture file");
    close(fd);
}

std::string read_file(const std::string& path) {
    const int fd = open(path.c_str(), O_RDONLY);
    require(fd >= 0, "cannot open file: " + path);
    std::string out;
    char buffer[4096];
    for (;;) {
        const ssize_t count = read(fd, buffer, sizeof(buffer));
        if (count <= 0) {
            break;
        }
        out.append(buffer, static_cast<std::size_t>(count));
    }
    close(fd);
    return out;
}

bool directory_has_entries(const std::string& path) {
    DIR* directory = opendir(path.c_str());
    require(directory != nullptr, "cannot open directory: " + path);
    bool found = false;
    for (;;) {
        errno = 0;
        dirent* entry = readdir(directory);
        if (entry == nullptr) {
            require(errno == 0, "cannot enumerate directory: " + path);
            break;
        }
        if (std::strcmp(entry->d_name, ".") != 0 &&
            std::strcmp(entry->d_name, "..") != 0) {
            found = true;
            break;
        }
    }
    closedir(directory);
    return found;
}

bool directory_files_contain(const std::string& path,
                             const std::string& needle) {
    DIR* directory = opendir(path.c_str());
    require(directory != nullptr, "cannot open directory: " + path);
    bool found = false;
    for (;;) {
        errno = 0;
        dirent* entry = readdir(directory);
        if (entry == nullptr) {
            require(errno == 0, "cannot enumerate directory: " + path);
            break;
        }
        if (std::strcmp(entry->d_name, ".") == 0 ||
            std::strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        const std::string file = path + "/" + entry->d_name;
        struct stat metadata = {};
        require(lstat(file.c_str(), &metadata) == 0,
                "cannot inspect generation file: " + file);
        if (S_ISREG(metadata.st_mode) &&
            read_file(file).find(needle) != std::string::npos) {
            found = true;
            break;
        }
    }
    closedir(directory);
    return found;
}

std::string run_command(const std::vector<std::string>& argv) {
    std::vector<char*> args;
    for (const std::string& argument : argv) {
        args.push_back(const_cast<char*>(argument.c_str()));
    }
    args.push_back(nullptr);
    int pipes[2];
    require(pipe(pipes) == 0, "cannot create pipe");
    const pid_t pid = fork();
    require(pid >= 0, "cannot fork");
    if (pid == 0) {
        dup2(pipes[1], STDOUT_FILENO);
        dup2(pipes[1], STDERR_FILENO);
        close(pipes[0]);
        close(pipes[1]);
        execv(args[0], args.data());
        _exit(127);
    }
    close(pipes[1]);
    std::string output;
    char buffer[4096];
    for (;;) {
        const ssize_t count = read(pipes[0], buffer, sizeof(buffer));
        if (count <= 0) {
            break;
        }
        output.append(buffer, static_cast<std::size_t>(count));
    }
    close(pipes[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    require(WIFEXITED(status) && WEXITSTATUS(status) == 0,
            "command failed: " + output);
    return output;
}

struct Fixtures {
    std::string apps_root;
    std::string state_root;
    std::string secret_root;
    int apps_fd = -1;
    int secrets_fd = -1;
    std::string worker_path;
    std::string compiler_tool;
    std::string fixture_source;
    std::string vdir;
};

Fixtures make_fixtures(const char* worker_path, const char* compiler_tool,
                       const char* fixture_source) {
    Fixtures f;
    f.apps_root = "/tmp/capsid-managed-apps-XXXXXX";
    require(mkdtemp(&f.apps_root[0]) != nullptr, "cannot create apps root");
    f.state_root = "/tmp/capsid-managed-state-XXXXXX";
    require(mkdtemp(&f.state_root[0]) != nullptr, "cannot create state root");
    f.secret_root = "/tmp/capsid-managed-secrets-XXXXXX";
    require(mkdtemp(&f.secret_root[0]) != nullptr, "cannot create secret root");
    f.apps_fd = open(f.apps_root.c_str(), O_RDONLY | O_DIRECTORY);
    f.secrets_fd = open(f.secret_root.c_str(), O_RDONLY | O_DIRECTORY);
    require(f.apps_fd >= 0 && f.secrets_fd >= 0, "cannot open fixture roots");
    f.worker_path = worker_path;
    f.compiler_tool = compiler_tool;
    f.fixture_source = fixture_source;
    require(mkdirat(f.apps_fd, "orders", 0700) == 0, "cannot create app dir");
    f.vdir = f.apps_root + "/orders/v1";
    require(mkdir(f.vdir.c_str(), 0700) == 0, "cannot create version dir");
    return f;
}

const char* kSourceBundle =
    "export default { fetch: () => new Response('managed-ok') };";

// Managed deployment consumes the same frozen capsid/app-v1 document as the
// M0 config boundary. Keeping a second legacy {modules,env} shape in this
// integration suite would let the coordinator bypass the authoritative
// schema while all happy-path tests still passed.
const char* kMinimalAppConfig =
    R"json({"apiVersion":"capsid/app-v1","entry":"bundle.mjs","permissions":{"modules":["capsid:env"]},"pool":{"minReady":1,"maxWorkers":1}})json";

const char* kSecretAppConfig =
    R"json({"apiVersion":"capsid/app-v1","entry":"bundle.mjs","permissions":{"modules":["capsid:env"],"env":{"APP_TOKEN":{"valueFrom":"api-token"}}},"pool":{"minReady":1,"maxWorkers":1}})json";

const char* kLiteralEnvAppConfig =
    R"json({"apiVersion":"capsid/app-v1","entry":"bundle.mjs","permissions":{"modules":["capsid:env"],"env":{"APP_MODE":{"value":"literal-managed"}}},"pool":{"minReady":1,"maxWorkers":1}})json";

const char* kThreeWorkerAppConfig =
    R"json({"apiVersion":"capsid/app-v1","entry":"bundle.mjs","permissions":{"modules":["capsid:env"]},"pool":{"minReady":3,"maxWorkers":3}})json";

void create_version(const Fixtures& fixtures, const std::string& version,
                    const std::string& config, const std::string& bundle) {
    const std::string directory =
        fixtures.apps_root + "/orders/" + version;
    require(mkdir(directory.c_str(), 0700) == 0,
            "cannot create version directory");
    write_file(directory + "/capsid.json", config);
    write_file(directory + "/bundle.mjs", bundle);
}

capsid::host::HostPolicy default_host_policy() {
    capsid::host::HostPolicy host;
    host.module_allowlist = { "capsid:env" };
    host.env_patterns = { "APP_*" };
    host.max_workers = 1;
    host.min_ready = 1;
    host.max_requests_per_worker = 10000;
    host.max_worker_memory_bytes = 256U * 1024U * 1024U;
    return host;
}

capsid::host::ManagedHostOptions make_options(Fixtures& f) {
    capsid::host::ManagedHostOptions options;
    options.applications_root_fd = f.apps_fd;
    options.secret_root_template_fd = f.secrets_fd;
    options.state_root = f.state_root;
    options.application = "orders";
    options.worker_path = f.worker_path;
    options.host_policy = default_host_policy();
    options.runtime_compatibility_id = CAPSID_BUILD_COMPATIBILITY_ID;
    return options;
}

template <typename Options, typename Activate, typename Retire>
bool set_async_worker_lifecycle(Options* options,
                                Activate activate,
                                Retire retire) {
    if constexpr (requires(Options& value, Activate activate_callback,
                           Retire retire_callback) {
                      value.activate_worker = activate_callback;
                      value.retire_worker = retire_callback;
                  }) {
        options->activate_worker = std::move(activate);
        options->retire_worker = std::move(retire);
        return true;
    }
    (void) options;
    (void) activate;
    (void) retire;
    return false;
}

template <typename Options, typename Activate>
bool set_async_pool_lifecycle(Options* options, Activate activate) {
    if constexpr (requires(Options& value, Activate activate_callback) {
                      value.activate_pool = activate_callback;
                  }) {
        options->activate_pool = std::move(activate);
        return true;
    }
    (void) options;
    (void) activate;
    return false;
}

capsid::host::OperationStatus wait_admin_operation(
    capsid::host::AsyncAdminBackend* backend,
    const std::string& operation_id) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(20);
    for (;;) {
        const capsid::host::OperationStatus status =
            backend->operation_status(operation_id);
        if (status.state == capsid::host::OperationState::kActive ||
            status.state == capsid::host::OperationState::kFailed) {
            return status;
        }
        require(std::chrono::steady_clock::now() < deadline,
                "Admin managed operation did not become terminal");
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

// Keep the RED suite buildable before HostPolicy grows the exact resource
// sets required by the frozen design. Negative tests require these helpers
// to return true; on the old bool-only policy they therefore fail at runtime
// with a precise diagnostic instead of making the entire test target fail to
// compile. Positive tests call the same helpers opportunistically so they
// continue to represent an explicitly allowed resource after the API grows.
template <typename Policy>
bool set_host_storage_namespaces(Policy* policy,
                                 std::vector<std::string> namespaces) {
    if constexpr (requires(Policy& value) { value.storage_namespaces; }) {
        policy->storage_namespaces = std::move(namespaces);
        return true;
    }
    (void)policy;
    (void)namespaces;
    return false;
}

template <typename Policy>
bool set_host_stdio_streams(Policy* policy,
                            std::vector<std::string> streams) {
    if constexpr (requires(Policy& value) { value.stdio_streams; }) {
        policy->stdio_streams = std::move(streams);
        return true;
    }
    (void)policy;
    (void)streams;
    return false;
}

// Recovery of hostile committed state is isolated in a child so the test
// can prove "fail promptly" without letting a blocking FIFO open hang the
// entire CTest process. A clean child exit means managed_recover returned a
// normal Failed result; timeout, signal, exception or successful recovery is
// a RED. The optional address-space ceiling makes the oversized sparse-file
// case deterministic without allowing the old unbounded reader to consume
// the machine's memory.
bool recovery_fails_promptly(capsid::host::ManagedHostOptions* options,
                             std::chrono::milliseconds timeout,
                             rlim_t address_space_ceiling = 0) {
    const pid_t child = fork();
    require(child >= 0, "cannot fork recovery boundary probe");
    if (child == 0) {
        if (address_space_ceiling != 0) {
            const struct rlimit limit = {
                address_space_ceiling, address_space_ceiling};
            if (setrlimit(RLIMIT_AS, &limit) != 0) {
                _exit(3);
            }
        }
        try {
            capsid::host::OperationStatus status;
            const capsid::host::DeployOutcome recovered =
                capsid::host::managed_recover(options, &status);
            if (recovered.worker != nullptr) {
                capsid_worker_destroy(recovered.worker);
            }
            _exit(!recovered.ok &&
                          status.state ==
                              capsid::host::OperationState::kFailed
                      ? 0
                      : 1);
        } catch (...) {
            _exit(2);
        }
    }

    const std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + timeout;
    int child_status = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        const pid_t waited = waitpid(child, &child_status, WNOHANG);
        require(waited >= 0, "cannot wait for recovery boundary probe");
        if (waited == child) {
            return WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    require(kill(child, SIGKILL) == 0,
            "cannot terminate blocked recovery boundary probe");
    require(waitpid(child, &child_status, 0) == child,
            "cannot reap blocked recovery boundary probe");
    return false;
}

// Worker request helper (same contract as the other worker tests).
std::string run_request(capsid_worker* worker) {
    require(capsid_worker_begin_bodyless_request(
                worker, 1, "GET", "https://example.test/managed", NULL, 0) ==
                CAPSID_OK,
            "begin bodyless request");
    bool head = false;
    std::string body;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(10);
    for (;;) {
        capsid_worker_flush(worker);
        capsid_event event = {};
        event.struct_size = sizeof(event);
        const capsid_result result = capsid_worker_next_event(worker, &event);
        if (result == CAPSID_OK) {
            if (event.type == CAPSID_EVENT_RESPONSE_HEAD) {
                head = true;
                continue;
            }
            if (event.type == CAPSID_EVENT_RESPONSE_BODY) {
                body.append(reinterpret_cast<const char*>(event.payload.data),
                            event.payload.size);
                capsid_worker_grant_response_credit(
                    worker, event.request_id,
                    static_cast<std::uint32_t>(event.payload.size));
                continue;
            }
            if (event.type == CAPSID_EVENT_RESPONSE_END) {
                require(head, "response end before head");
                return body;
            }
            if (event.type == CAPSID_EVENT_ERROR ||
                event.type == CAPSID_EVENT_EXIT) {
                fail("worker failed during request");
            }
            continue;
        }
        if (result != CAPSID_WOULD_BLOCK) {
            fail("worker event error");
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            fail("request timeout");
        }
        struct pollfd descriptor = {};
        descriptor.fd = capsid_worker_fd(worker);
        descriptor.events = POLLIN;
        poll(&descriptor, 1, 50);
    }
}

template <typename Outcome>
std::vector<capsid_worker*> outcome_workers(const Outcome& outcome) {
    if constexpr (requires(const Outcome& value) { value.workers; }) {
        if (!outcome.workers.empty()) {
            return outcome.workers;
        }
    }
    if (outcome.worker != nullptr) {
        return { outcome.worker };
    }
    return {};
}

void destroy_workers(const std::vector<capsid_worker*>& workers) {
    std::set<capsid_worker*> unique;
    for (capsid_worker* worker : workers) {
        require(worker != nullptr, "pool contains a null worker");
        require(unique.insert(worker).second,
                "pool contains a duplicate owning worker pointer");
    }
    for (capsid_worker* worker : workers) {
        capsid_worker_destroy(worker);
    }
}

std::uint64_t percentile_us(std::vector<std::uint64_t> values,
                            std::size_t percentile) {
    require(!values.empty(), "cannot compute a percentile without samples");
    std::sort(values.begin(), values.end());
    const std::size_t index =
        ((values.size() - 1) * percentile) / 100;
    return values[index];
}

// ---- attestation helpers ----

struct TestKey {
    std::vector<std::uint8_t> private_key;
    std::vector<std::uint8_t> public_key;
};

TestKey test_key() {
    TestKey key;
    key.private_key.resize(32);
    key.public_key.resize(32);
    EVP_PKEY* pkey = EVP_PKEY_new_raw_private_key(
        EVP_PKEY_ED25519, nullptr, key.private_key.data(), 32);
    require(pkey != nullptr, "cannot create key");
    std::size_t size = 32;
    require(EVP_PKEY_get_raw_public_key(pkey, key.public_key.data(), &size) == 1,
            "cannot extract public key");
    EVP_PKEY_free(pkey);
    return key;
}

std::vector<std::uint8_t> sign(const TestKey& key,
                               const std::vector<std::uint8_t>& message) {
    EVP_PKEY* pkey = EVP_PKEY_new_raw_private_key(
        EVP_PKEY_ED25519, nullptr, key.private_key.data(), 32);
    require(pkey != nullptr, "cannot recreate key");
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    require(ctx != nullptr &&
                EVP_DigestSignInit(ctx, nullptr, nullptr, nullptr, pkey) == 1,
            "cannot init signing");
    std::size_t size = 64;
    require(EVP_DigestSign(ctx, nullptr, &size, message.data(),
                           message.size()) == 1,
            "cannot size signature");
    std::vector<std::uint8_t> signature(size);
    require(EVP_DigestSign(ctx, signature.data(), &size, message.data(),
                           message.size()) == 1,
            "cannot sign");
    signature.resize(size);
    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(pkey);
    return signature;
}

// Compiles kSourceBundle (the exact source in bundle.mjs) and returns
// {bytecode, attestation, signature} with the test key; also writes the
// files into the version dir.
void compile_and_sign(const Fixtures& f, const TestKey& key,
                      std::vector<std::uint8_t>* bytecode,
                      std::string* attestation,
                      std::vector<std::uint8_t>* signature) {
    const std::string out_dir = f.vdir;
    const std::string source_file = out_dir + "/compile-source.mjs";
    write_file(source_file, kSourceBundle);
    const std::string bytecode_out = out_dir + "/bundle.qjsb";
    const std::string attestation_out = out_dir + "/bytecode.json";
    const std::string message_out = out_dir + "/message.bin";
    run_command({
        f.compiler_tool,
        "--source", source_file,
        "--source-name", ("file://orders/v1/bundle.qjsb"),
        "--application", "orders",
        "--version", "v1",
        "--key-id", "test-key-1",
        "--bytecode-out", bytecode_out,
        "--attestation-out", attestation_out,
        "--signing-message-out", message_out,
    });
    const std::string message = read_file(message_out);
    const std::vector<std::uint8_t> message_bytes(message.begin(), message.end());
    *signature = sign(key, message_bytes);
    write_file(out_dir + "/bytecode.sig",
               std::string(signature->begin(), signature->end()));
    const std::string bytecode_text = read_file(bytecode_out);
    *bytecode = std::vector<std::uint8_t>(bytecode_text.begin(),
                                          bytecode_text.end());
    *attestation = read_file(attestation_out);
}

std::string json_string_field(const std::string& json,
                              const std::string& field) {
    const std::string prefix = "\"" + field + "\":\"";
    const std::string::size_type begin = json.find(prefix);
    require(begin != std::string::npos, "cannot find JSON field: " + field);
    const std::string::size_type value_begin = begin + prefix.size();
    const std::string::size_type end = json.find('"', value_begin);
    require(end != std::string::npos, "unterminated JSON field: " + field);
    return json.substr(value_begin, end - value_begin);
}

void install_compatibility_mismatch(const Fixtures& fixtures,
                                    const TestKey& key,
                                    const std::string& attestation) {
    const std::string wrong_id =
        "sha256:0000000000000000000000000000000000000000000000000000000000000000";
    const std::string old_id = json_string_field(attestation,
                                                 "compatibilityId");
    const std::string needle = "\"compatibilityId\":\"" + old_id + "\"";
    const std::string::size_type position = attestation.find(needle);
    require(position != std::string::npos,
            "cannot locate compatibilityId claim");
    std::string tampered = attestation;
    tampered.replace(position, needle.size(),
                     "\"compatibilityId\":\"" + wrong_id + "\"");

    std::vector<std::uint8_t> message;
    const std::string fields[] = {
        "capsid-bytecode-v1",
        "orders",
        "v1",
        "file://orders/v1/bundle.qjsb",
        json_string_field(attestation, "sourceSha256"),
        json_string_field(attestation, "bytecodeSha256"),
        wrong_id,
        "test-key-1",
    };
    static constexpr char kDomain[] = "capsid-bytecode-attestation-v1\0";
    message.assign(reinterpret_cast<const std::uint8_t*>(kDomain),
                   reinterpret_cast<const std::uint8_t*>(kDomain) +
                       sizeof(kDomain) - 1);
    for (const std::string& field : fields) {
        const std::uint32_t size = static_cast<std::uint32_t>(field.size());
        message.push_back(static_cast<std::uint8_t>(size >> 24));
        message.push_back(static_cast<std::uint8_t>(size >> 16));
        message.push_back(static_cast<std::uint8_t>(size >> 8));
        message.push_back(static_cast<std::uint8_t>(size));
        message.insert(message.end(), field.begin(), field.end());
    }
    write_file(fixtures.vdir + "/bytecode.json", tampered);
    const std::vector<std::uint8_t> signature = sign(key, message);
    write_file(fixtures.vdir + "/bytecode.sig",
               std::string(signature.begin(), signature.end()));
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 5) {
        fail("expected mode, worker path, compiler tool and fixture");
    }
    const std::string mode = argv[1];
    const char* worker_path = argv[2];
    const char* compiler_tool = argv[3];
    const char* fixture_source = argv[4];
    Fixtures fixtures = make_fixtures(worker_path, compiler_tool, fixture_source);
    write_file(fixtures.vdir + "/capsid.json", kMinimalAppConfig);
    write_file(fixtures.vdir + "/bundle.mjs", kSourceBundle);

    if (mode == "host_managed_deploy_integration") {
        capsid::host::ManagedHostOptions options = make_options(fixtures);
        capsid::host::OperationStatus status;
        const capsid::host::DeployOutcome outcome =
            capsid::host::managed_deploy(&options, "v1", &status);
        require(outcome.ok, "source deploy failed: " + outcome.error);
        require(status.state == capsid::host::OperationState::kActive,
                "deploy not Active");
        require(outcome.worker != nullptr, "no warmed worker returned");
        // The warmed worker serves requests.
        require(run_request(outcome.worker) == "managed-ok",
                "warmed worker did not serve the bundle");
        capsid_worker_destroy(outcome.worker);
        // active.json persisted with the generation.
        const std::string active = read_file(
            fixtures.state_root + "/apps/orders/active.json");
        require(active.find("capsid-active-v1") != std::string::npos &&
                    active.find("\"generation\"") != std::string::npos,
                "active.json missing generation");
        // Retire: tombstone persisted; recovery then starts no worker.
        capsid::host::OperationStatus retire_status;
        const capsid::host::DeployOutcome retired =
            capsid::host::managed_retire(&options, &retire_status);
        require(retired.ok, "retire failed");
        capsid::host::OperationStatus recover_status;
        const capsid::host::DeployOutcome recovered =
            capsid::host::managed_recover(&options, &recover_status);
        require(recovered.ok, "recovery failed after retire");
        require(recovered.worker == nullptr,
                "retired recovery returned a worker");
        require(recover_status.state != capsid::host::OperationState::kActive,
                "retired app must not start a worker");
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode == "host_managed_queue_config_deploy") {
        // E-1 admission queue fields (pool.queue*, §10.3) ride the frozen
        // capsid/app-v1 document and must parse, compile and deploy like
        // any other pool field — a zero-consumption field would let a
        // queue silently disappear from the effective contract.
        write_file(fixtures.vdir + "/capsid.json",
                   R"json({"apiVersion":"capsid/app-v1","entry":"bundle.mjs","permissions":{"modules":["capsid:env"]},"pool":{"minReady":1,"maxWorkers":1,"queueRequests":8,"queueHeaderBytes":"2MiB","queueTimeout":"250ms"}})json");
        capsid::host::ManagedHostOptions options = make_options(fixtures);
        capsid::host::OperationStatus status;
        const capsid::host::DeployOutcome outcome =
            capsid::host::managed_deploy(&options, "v1", &status);
        require(outcome.ok, "queue-config deploy failed: " + outcome.error);
        require(status.state == capsid::host::OperationState::kActive,
                "queue-config deploy not Active");
        require(outcome.worker != nullptr, "no warmed worker returned");
        require(run_request(outcome.worker) == "managed-ok",
                "queue-config worker did not serve the bundle");
        capsid_worker_destroy(outcome.worker);
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode == "host_managed_sse_permit_config_deploy") {
        // E-2 SSE permit fields (request.*, §9.3) ride the frozen
        // capsid/app-v1 document like the E-1 queue fields — a
        // zero-consumption field would let the streaming budget silently
        // disappear from the effective contract.
        write_file(fixtures.vdir + "/capsid.json",
                   R"json({"apiVersion":"capsid/app-v1","entry":"bundle.mjs","permissions":{"modules":["capsid:env"]},"pool":{"minReady":1,"maxWorkers":1},"request":{"maxStreamingInflightPerWorker":3,"streamIdleTimeoutMs":90000}})json");
        capsid::host::ManagedHostOptions options = make_options(fixtures);
        capsid::host::OperationStatus status;
        const capsid::host::DeployOutcome outcome =
            capsid::host::managed_deploy(&options, "v1", &status);
        require(outcome.ok, "SSE-config deploy failed: " + outcome.error);
        require(status.state == capsid::host::OperationState::kActive,
                "SSE-config deploy not Active");
        require(outcome.worker != nullptr, "no warmed worker returned");
        require(run_request(outcome.worker) == "managed-ok",
                "SSE-config worker did not serve the bundle");
        capsid_worker_destroy(outcome.worker);
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode == "host_managed_write_timeout_config_deploy") {
        // E-3 §9.2: request.writeTimeoutMs rides the frozen capsid/app-v1
        // document like the E-2 SSE-permit fields — a zero-consumption
        // field would let the slow-client deadline silently disappear
        // from the effective contract.
        write_file(fixtures.vdir + "/capsid.json",
                   R"json({"apiVersion":"capsid/app-v1","entry":"bundle.mjs","permissions":{"modules":["capsid:env"]},"pool":{"minReady":1,"maxWorkers":1},"request":{"writeTimeoutMs":5000}})json");
        capsid::host::ManagedHostOptions options = make_options(fixtures);
        capsid::host::OperationStatus status;
        const capsid::host::DeployOutcome outcome =
            capsid::host::managed_deploy(&options, "v1", &status);
        require(outcome.ok, "write-timeout deploy failed: " + outcome.error);
        require(status.state == capsid::host::OperationState::kActive,
                "write-timeout deploy not Active");
        require(outcome.worker != nullptr, "no warmed worker returned");
        require(run_request(outcome.worker) == "managed-ok",
                "write-timeout worker did not serve the bundle");
        capsid_worker_destroy(outcome.worker);
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode == "host_managed_fixed_pool_deploy_and_recover") {
        write_file(fixtures.vdir + "/capsid.json", kThreeWorkerAppConfig);
        capsid::host::ManagedHostOptions options = make_options(fixtures);
        options.host_policy.max_workers = 3;

        capsid::host::OperationStatus status;
        const capsid::host::DeployOutcome deployed =
            capsid::host::managed_deploy(&options, "v1", &status);
        require(deployed.ok, "fixed 3/3 deploy failed: " + deployed.error);
        require(status.state == capsid::host::OperationState::kActive,
                "fixed pool deploy did not become Active");
        const std::vector<capsid_worker*> deployed_workers =
            outcome_workers(deployed);
        require(deployed_workers.size() == 3,
                "fixed 3/3 deploy did not return three READY workers");
        std::set<std::int64_t> deployed_pids;
        for (capsid_worker* worker : deployed_workers) {
            require(run_request(worker) == "managed-ok",
                    "a deployed pool worker could not serve the bundle");
            require(deployed_pids.insert(capsid_worker_pid(worker)).second,
                    "fixed pool reused a worker process");
        }
        destroy_workers(deployed_workers);

        capsid::host::OperationStatus recover_status;
        const capsid::host::DeployOutcome recovered =
            capsid::host::managed_recover(&options, &recover_status);
        require(recovered.ok, "fixed 3/3 recovery failed: " + recovered.error);
        require(recover_status.state == capsid::host::OperationState::kActive,
                "fixed pool recovery did not become Active");
        const std::vector<capsid_worker*> recovered_workers =
            outcome_workers(recovered);
        require(recovered_workers.size() == 3,
                "fixed 3/3 recovery did not return three READY workers");
        std::set<std::int64_t> recovered_pids;
        for (capsid_worker* worker : recovered_workers) {
            require(run_request(worker) == "managed-ok",
                    "a recovered pool worker could not serve the bundle");
            require(recovered_pids.insert(capsid_worker_pid(worker)).second,
                    "recovery reused a worker process inside the pool");
        }
        destroy_workers(recovered_workers);
        std::cout << "PASS" << std::endl;
        return 0;
    }

    // Manual Release benchmark probe, intentionally not registered in
    // CTest. This isolates managed-pool + Runtime IPC scaling before the
    // later HTTP/shard router exists. It is not the final end-to-end Host
    // benchmark and must never be reported as one.
    if (mode == "host_managed_pool_benchmark_probe") {
        const char* worker_count_env = std::getenv("CAPSID_POOL_BENCH_WORKERS");
        const char* duration_env = std::getenv("CAPSID_POOL_BENCH_SECONDS");
        require(worker_count_env != nullptr && duration_env != nullptr,
                "benchmark requires CAPSID_POOL_BENCH_WORKERS and "
                "CAPSID_POOL_BENCH_SECONDS");
        const unsigned long parsed_workers =
            std::strtoul(worker_count_env, nullptr, 10);
        const unsigned long parsed_seconds =
            std::strtoul(duration_env, nullptr, 10);
        require(parsed_workers > 0 && parsed_workers <= 64 &&
                    parsed_seconds > 0 && parsed_seconds <= 300,
                "benchmark worker count or duration is out of range");
        const std::uint32_t worker_count =
            static_cast<std::uint32_t>(parsed_workers);
        const std::string config =
            "{\"apiVersion\":\"capsid/app-v1\",\"entry\":\"bundle.mjs\","
            "\"permissions\":{\"modules\":[\"capsid:env\"]},\"pool\":"
            "{\"minReady\":" + std::to_string(worker_count) +
            ",\"maxWorkers\":" + std::to_string(worker_count) + "}}";
        write_file(fixtures.vdir + "/capsid.json", config);
        capsid::host::ManagedHostOptions options = make_options(fixtures);
        options.host_policy.max_workers = worker_count;
        capsid::host::OperationStatus status;
        const capsid::host::DeployOutcome deployed =
            capsid::host::managed_deploy(&options, "v1", &status);
        require(deployed.ok, "benchmark pool deploy failed: " + deployed.error);
        const std::vector<capsid_worker*> workers = outcome_workers(deployed);
        require(workers.size() == worker_count,
                "benchmark did not receive the requested pool size");

        // Warm every code path once before opening the measured barrier.
        for (capsid_worker* worker : workers) {
            require(run_request(worker) == "managed-ok",
                    "benchmark warm-up request failed");
        }
        std::barrier start(static_cast<std::ptrdiff_t>(workers.size() + 1));
        std::atomic<bool> stop{false};
        std::vector<std::vector<std::uint64_t>> latencies(workers.size());
        std::vector<std::thread> threads;
        threads.reserve(workers.size());
        for (std::size_t index = 0; index < workers.size(); ++index) {
            threads.emplace_back([&, index]() {
                start.arrive_and_wait();
                while (!stop.load(std::memory_order_relaxed)) {
                    const auto began = std::chrono::steady_clock::now();
                    require(run_request(workers[index]) == "managed-ok",
                            "benchmark measured request failed");
                    const auto elapsed = std::chrono::duration_cast<
                        std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - began);
                    latencies[index].push_back(
                        static_cast<std::uint64_t>(elapsed.count()));
                }
            });
        }
        start.arrive_and_wait();
        const auto measured_start = std::chrono::steady_clock::now();
        std::this_thread::sleep_for(std::chrono::seconds(parsed_seconds));
        stop.store(true, std::memory_order_relaxed);
        for (std::thread& thread : threads) {
            thread.join();
        }
        const double elapsed_seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                          measured_start).count();
        std::vector<std::uint64_t> all_latencies;
        std::vector<std::int64_t> pids;
        for (std::size_t index = 0; index < workers.size(); ++index) {
            all_latencies.insert(all_latencies.end(), latencies[index].begin(),
                                 latencies[index].end());
            pids.push_back(capsid_worker_pid(workers[index]));
        }
        require(!all_latencies.empty(), "benchmark completed no requests");
        std::cout << "{\"kind\":\"managed-pool-ipc-probe\",\"workers\":"
                  << worker_count << ",\"duration_s\":" << elapsed_seconds
                  << ",\"completed\":" << all_latencies.size()
                  << ",\"qps\":"
                  << static_cast<double>(all_latencies.size()) /
                         elapsed_seconds
                  << ",\"p50_us\":" << percentile_us(all_latencies, 50)
                  << ",\"p95_us\":" << percentile_us(all_latencies, 95)
                  << ",\"p99_us\":" << percentile_us(all_latencies, 99)
                  << ",\"pids\":[";
        for (std::size_t index = 0; index < pids.size(); ++index) {
            if (index != 0) {
                std::cout << ',';
            }
            std::cout << pids[index];
        }
        std::cout << "]}" << std::endl;
        destroy_workers(workers);
        return 0;
    }

    if (mode == "host_managed_fixed_pool_warm_failure_is_atomic") {
        capsid::host::ManagedHostOptions options = make_options(fixtures);
        options.host_policy.max_workers = 3;
        capsid::host::OperationStatus first_status;
        const capsid::host::DeployOutcome first =
            capsid::host::managed_deploy(&options, "v1", &first_status);
        require(first.ok && first.worker != nullptr,
                "cannot establish the old active worker");
        const std::string active_before = read_file(
            fixtures.state_root + "/apps/orders/active.json");

        create_version(fixtures, "v2", kThreeWorkerAppConfig, kSourceBundle);
        const std::string wrapper = fixtures.apps_root + "/fail-third-worker.sh";
        const std::string slots = fixtures.apps_root + "/worker-slots";
        require(mkdir(slots.c_str(), 0700) == 0,
                "cannot create worker wrapper state");
        const std::string pid_log = slots + "/pids";
        std::ostringstream script;
        script << "#!/bin/sh\n"
               << "if mkdir '" << slots << "/one' 2>/dev/null; then\n"
               << "  printf '%s\\n' \"$$\" >> '" << pid_log << "'\n"
               << "  exec '" << fixtures.worker_path << "' \"$@\"\n"
               << "elif mkdir '" << slots << "/two' 2>/dev/null; then\n"
               << "  printf '%s\\n' \"$$\" >> '" << pid_log << "'\n"
               << "  exec '" << fixtures.worker_path << "' \"$@\"\n"
               << "else\n"
               << "  printf '%s\\n' \"$$\" >> '" << pid_log << "'\n"
               << "  exit 42\n"
               << "fi\n";
        write_file(wrapper, script.str());
        require(chmod(wrapper.c_str(), 0700) == 0,
                "cannot make worker failure wrapper executable");
        options.worker_path = wrapper;

        capsid::host::OperationStatus failed_status;
        const capsid::host::DeployOutcome failed =
            capsid::host::managed_deploy(&options, "v2", &failed_status);
        require(!failed.ok &&
                    failed_status.state == capsid::host::OperationState::kFailed,
                "pool activated after one worker failed to warm");
        require(outcome_workers(failed).empty(),
                "failed pool returned partially warmed workers");
        require(read_file(fixtures.state_root + "/apps/orders/active.json") ==
                    active_before,
                "failed pool warm changed the old active generation");
        require(run_request(first.worker) == "managed-ok",
                "failed replacement damaged the old active worker");

        std::istringstream pids(read_file(pid_log));
        std::int64_t pid = 0;
        std::size_t pid_count = 0;
        while (pids >> pid) {
            ++pid_count;
            errno = 0;
            require(kill(static_cast<pid_t>(pid), 0) == -1 && errno == ESRCH,
                    "failed pool leaked a worker process");
        }
        require(pid_count == 3,
                "failure probe did not reach the third worker warm-up");
        capsid_worker_destroy(first.worker);
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode == "host_managed_admin_fixed_pool_handoff") {
        write_file(fixtures.vdir + "/capsid.json", kThreeWorkerAppConfig);
        capsid::host::ManagedHostOptions options = make_options(fixtures);
        options.host_policy.max_workers = 3;
        std::vector<capsid::host::ManagedHostOptions*> applications = {
            &options,
        };
        capsid::host::ManagedAdminBackend managed(applications);
        std::mutex lifecycle_mutex;
        std::condition_variable lifecycle_condition;
        std::vector<capsid_worker*> active_pool;
        int legacy_activate_calls = 0;

        capsid::host::AsyncAdminBackendOptions async_options;
        async_options.max_pending_operations = 2;
        async_options.activate_worker =
            [&](const std::string&, capsid_worker*) {
                ++legacy_activate_calls;
                return false;
            };
        const auto activate_pool =
            [&](const std::string& application,
                std::vector<capsid_worker*> workers) {
                if (application != "orders" || workers.size() != 3) {
                    return false;
                }
                std::lock_guard<std::mutex> lock(lifecycle_mutex);
                if (!active_pool.empty()) {
                    return false;
                }
                active_pool = std::move(workers);
                lifecycle_condition.notify_all();
                return true;
            };
        require(set_async_pool_lifecycle(&async_options, activate_pool),
                "Async Admin backend has no atomic pool ownership handoff");
        async_options.retire_worker = [&](const std::string& application) {
            std::vector<capsid_worker*> retired;
            {
                std::lock_guard<std::mutex> lock(lifecycle_mutex);
                if (application == "orders") {
                    retired.swap(active_pool);
                }
            }
            destroy_workers(retired);
            lifecycle_condition.notify_all();
        };
        capsid::host::AsyncAdminBackend backend(&managed, async_options);

        capsid::host::OperationStatus submitted;
        const capsid::host::DeployOutcome deployment =
            backend.deploy("orders", "v1", &submitted);
        require(deployment.ok, "fixed pool Admin deploy was not accepted");
        const capsid::host::OperationStatus active =
            wait_admin_operation(&backend, deployment.operation_id);
        require(active.state == capsid::host::OperationState::kActive,
                "fixed pool Admin deploy did not activate");
        {
            std::unique_lock<std::mutex> lock(lifecycle_mutex);
            require(lifecycle_condition.wait_for(
                        lock, std::chrono::seconds(2),
                        [&]() { return active_pool.size() == 3; }),
                    "Admin did not atomically hand off all three workers");
            require(legacy_activate_calls == 0,
                    "multi-worker pool used the legacy single-worker handoff");
            std::set<std::int64_t> pids;
            for (capsid_worker* worker : active_pool) {
                require(pids.insert(capsid_worker_pid(worker)).second,
                        "Admin pool handoff contained duplicate processes");
            }
        }

        capsid::host::OperationStatus retire_submitted;
        const capsid::host::DeployOutcome retired =
            backend.retire("orders", &retire_submitted);
        require(retired.ok, "fixed pool Admin retire was not accepted");
        const capsid::host::OperationStatus retired_status =
            wait_admin_operation(&backend, retired.operation_id);
        require(retired_status.state == capsid::host::OperationState::kActive,
                "fixed pool Admin retire did not settle");
        {
            std::unique_lock<std::mutex> lock(lifecycle_mutex);
            require(lifecycle_condition.wait_for(
                        lock, std::chrono::seconds(2),
                        [&]() { return active_pool.empty(); }),
                    "Admin retire did not reclaim the full active pool");
        }
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode == "host_managed_admin_worker_lifecycle") {
        capsid::host::ManagedHostOptions options = make_options(fixtures);
        std::vector<capsid::host::ManagedHostOptions*> applications = {
            &options,
        };
        capsid::host::ManagedAdminBackend managed(applications);
        std::mutex lifecycle_mutex;
        std::condition_variable lifecycle_condition;
        capsid_worker* active_worker = nullptr;

        const auto activate = [&](const std::string& application,
                                  capsid_worker* worker) {
            if (application != "orders" || worker == nullptr) {
                return false;
            }
            std::lock_guard<std::mutex> lock(lifecycle_mutex);
            if (active_worker != nullptr) {
                return false;
            }
            active_worker = worker;
            lifecycle_condition.notify_all();
            return true;  // ownership transferred to this lifecycle sink
        };
        const auto retire = [&](const std::string& application) {
            capsid_worker* worker = nullptr;
            {
                std::lock_guard<std::mutex> lock(lifecycle_mutex);
                if (application == "orders") {
                    worker = active_worker;
                    active_worker = nullptr;
                }
            }
            if (worker != nullptr) {
                capsid_worker_destroy(worker);
            }
            lifecycle_condition.notify_all();
        };

        capsid::host::AsyncAdminBackendOptions async_options;
        async_options.max_pending_operations = 2;
        require(set_async_worker_lifecycle(
                    &async_options, activate, retire),
                "Async Admin backend has no explicit worker ownership "
                "handoff");
        capsid::host::AsyncAdminBackend backend(&managed, async_options);

        capsid::host::OperationStatus submitted;
        const capsid::host::DeployOutcome deployed =
            backend.deploy("orders", "v1", &submitted);
        require(deployed.ok,
                "Admin-managed real worker deploy was not accepted");
        const capsid::host::OperationStatus active =
            wait_admin_operation(&backend, deployed.operation_id);
        require(active.state == capsid::host::OperationState::kActive,
                "Admin-managed real worker deploy did not activate");
        capsid_worker* borrowed = nullptr;
        {
            std::unique_lock<std::mutex> lock(lifecycle_mutex);
            require(lifecycle_condition.wait_for(
                        lock, std::chrono::seconds(2),
                        [&]() { return active_worker != nullptr; }),
                    "active Admin worker was discarded instead of handed "
                    "off");
            borrowed = active_worker;
        }
        require(run_request(borrowed) == "managed-ok",
                "Admin-owned active worker did not serve its bundle");

        capsid::host::OperationStatus retire_submitted;
        const capsid::host::DeployOutcome retired =
            backend.retire("orders", &retire_submitted);
        require(retired.ok, "Admin-managed retire was not accepted");
        const capsid::host::OperationStatus retired_status =
            wait_admin_operation(&backend, retired.operation_id);
        require(retired_status.state ==
                    capsid::host::OperationState::kActive,
                "Admin-managed retire did not complete");
        {
            std::unique_lock<std::mutex> lock(lifecycle_mutex);
            require(lifecycle_condition.wait_for(
                        lock, std::chrono::seconds(2),
                        [&]() { return active_worker == nullptr; }),
                    "retire did not release the active Admin worker");
        }
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode == "host_managed_trusted_bytecode") {
        const TestKey key = test_key();
        std::vector<std::uint8_t> bytecode;
        std::string attestation;
        std::vector<std::uint8_t> signature;
        compile_and_sign(fixtures, key, &bytecode, &attestation, &signature);
        capsid::host::ManagedHostOptions options = make_options(fixtures);
        capsid::host::TrustedBytecodeKey trusted;
        trusted.key_id = "test-key-1";
        trusted.public_key = std::span<const std::uint8_t>(
            key.public_key.data(), key.public_key.size());
        options.trusted_keys.push_back(trusted);
        capsid::host::OperationStatus status;
        const capsid::host::DeployOutcome outcome =
            capsid::host::managed_deploy(&options, "v1", &status);
        require(outcome.ok, "trusted bytecode deploy failed: " + outcome.error);
        require(outcome.worker != nullptr, "no worker for trusted bytecode");
        require(run_request(outcome.worker) == "managed-ok",
                "trusted bytecode worker did not serve");
        capsid_worker_destroy(outcome.worker);
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode == "host_managed_compatibility_fallback") {
        const TestKey key = test_key();
        std::vector<std::uint8_t> bytecode;
        std::string attestation;
        std::vector<std::uint8_t> signature;
        compile_and_sign(fixtures, key, &bytecode, &attestation, &signature);
        // Tamper the compatibilityId claim and re-sign: provenance-valid
        // but compatibility-mismatched -> source fallback.
        const std::string wrong_id =
            "sha256:0000000000000000000000000000000000000000000000000000000000000000";
        const std::string::size_type pos =
            attestation.find("\"compatibilityId\":\"");
        require(pos != std::string::npos, "cannot find compatibilityId");
        const std::string::size_type end =
            attestation.find('"', pos + std::strlen("\"compatibilityId\":\""));
        require(end != std::string::npos, "cannot find compatibilityId end");
        const std::string tampered =
            attestation.substr(0, pos + std::strlen("\"compatibilityId\":\"")) +
            wrong_id + attestation.substr(end);
        // The signing message must match the tampered attestation's
        // claims exactly: the real digests (unchanged by the tamper) and
        // the wrong compatibility id.
        std::string source_digest;
        std::string bytecode_digest;
        {
            const std::string::size_type source_pos =
                attestation.find("\"sourceSha256\":\"");
            const std::string::size_type source_end =
                attestation.find('"', source_pos +
                                      std::strlen("\"sourceSha256\":\""));
            source_digest = attestation.substr(
                source_pos + std::strlen("\"sourceSha256\":\""),
                source_end - source_pos - std::strlen("\"sourceSha256\":\""));
            const std::string::size_type bytecode_pos =
                attestation.find("\"bytecodeSha256\":\"");
            const std::string::size_type bytecode_end =
                attestation.find('"', bytecode_pos +
                                         std::strlen("\"bytecodeSha256\":\""));
            bytecode_digest = attestation.substr(
                bytecode_pos + std::strlen("\"bytecodeSha256\":\""),
                bytecode_end - bytecode_pos -
                    std::strlen("\"bytecodeSha256\":\""));
        }
        std::vector<std::uint8_t> message;
        const std::string fields[] = {
            "capsid-bytecode-v1", "orders", "v1", "file://orders/v1/bundle.qjsb",
            source_digest, bytecode_digest, wrong_id, "test-key-1",
        };
        static constexpr char kDomain[] = "capsid-bytecode-attestation-v1\0";
        message.assign(reinterpret_cast<const std::uint8_t*>(kDomain),
                       reinterpret_cast<const std::uint8_t*>(kDomain) +
                           sizeof(kDomain) - 1);
        for (const std::string& field : fields) {
            const std::uint32_t size =
                static_cast<std::uint32_t>(field.size());
            message.push_back(static_cast<std::uint8_t>(size >> 24));
            message.push_back(static_cast<std::uint8_t>(size >> 16));
            message.push_back(static_cast<std::uint8_t>(size >> 8));
            message.push_back(static_cast<std::uint8_t>(size));
            message.insert(message.end(), field.begin(), field.end());
        }
        write_file(fixtures.vdir + "/bytecode.json", tampered);
        const std::vector<std::uint8_t> re_signed = sign(key, message);
        write_file(fixtures.vdir + "/bytecode.sig",
                   std::string(re_signed.begin(), re_signed.end()));
        capsid::host::ManagedHostOptions options = make_options(fixtures);
        capsid::host::TrustedBytecodeKey trusted;
        trusted.key_id = "test-key-1";
        trusted.public_key = std::span<const std::uint8_t>(
            key.public_key.data(), key.public_key.size());
        options.trusted_keys.push_back(trusted);
        capsid::host::OperationStatus status;
        const capsid::host::DeployOutcome outcome =
            capsid::host::managed_deploy(&options, "v1", &status);
        require(outcome.ok,
                "compatibility fallback deploy failed: " + outcome.error);
        require(outcome.worker != nullptr, "no worker for fallback");
        require(run_request(outcome.worker) == "managed-ok",
                "fallback worker did not serve source");
        capsid_worker_destroy(outcome.worker);
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode == "host_managed_fallback_identity_retains_attestation") {
        // A provenance-valid compatibility fallback still carries verified
        // attestation metadata. It must not collapse to the same generation
        // identity as an otherwise identical source-only deployment.
        Fixtures source_only =
            make_fixtures(worker_path, compiler_tool, fixture_source);
        write_file(source_only.vdir + "/capsid.json", kMinimalAppConfig);
        write_file(source_only.vdir + "/bundle.mjs", kSourceBundle);
        capsid::host::ManagedHostOptions source_options =
            make_options(source_only);
        capsid::host::OperationStatus source_status;
        const capsid::host::DeployOutcome source =
            capsid::host::managed_deploy(&source_options, "v1",
                                         &source_status);
        require(source.ok && source.worker != nullptr,
                "source-only baseline deploy failed: " + source.error);
        capsid_worker_destroy(source.worker);
        const std::string source_generation = json_string_field(
            read_file(source_only.state_root + "/apps/orders/active.json"),
            "generation");

        const TestKey key = test_key();
        std::vector<std::uint8_t> bytecode;
        std::string attestation;
        std::vector<std::uint8_t> signature;
        compile_and_sign(fixtures, key, &bytecode, &attestation, &signature);
        install_compatibility_mismatch(fixtures, key, attestation);
        capsid::host::ManagedHostOptions fallback_options =
            make_options(fixtures);
        capsid::host::TrustedBytecodeKey trusted;
        trusted.key_id = "test-key-1";
        trusted.public_key = std::span<const std::uint8_t>(
            key.public_key.data(), key.public_key.size());
        fallback_options.trusted_keys.push_back(trusted);
        capsid::host::OperationStatus fallback_status;
        const capsid::host::DeployOutcome fallback =
            capsid::host::managed_deploy(&fallback_options, "v1",
                                         &fallback_status);
        require(fallback.ok && fallback.worker != nullptr,
                "compatibility fallback deploy failed: " + fallback.error);
        capsid_worker_destroy(fallback.worker);
        const std::string fallback_generation = json_string_field(
            read_file(fixtures.state_root + "/apps/orders/active.json"),
            "generation");
        require(fallback_generation != source_generation,
                "compatibility fallback discarded verified attestation from "
                "generation identity");
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode == "host_managed_deploy_fail_closed") {
        // Deploy source-only first (old active), then a version with an
        // invalid signature: the new deploy fails and the old active
        // remains.
        capsid::host::ManagedHostOptions options = make_options(fixtures);
        capsid::host::OperationStatus status;
        const capsid::host::DeployOutcome first =
            capsid::host::managed_deploy(&options, "v1", &status);
        require(first.ok, "initial deploy failed");
        capsid_worker_destroy(first.worker);
        const std::string active_before = read_file(
            fixtures.state_root + "/apps/orders/active.json");
        // Corrupt the bundle.qjsb (a version with broken bytecode).
        write_file(fixtures.vdir + "/bundle.qjsb", "broken-bytecode");
        write_file(fixtures.vdir + "/bytecode.json", "{}");
        write_file(fixtures.vdir + "/bytecode.sig", "short-sig");
        capsid::host::OperationStatus fail_status;
        const capsid::host::DeployOutcome failed =
            capsid::host::managed_deploy(&options, "v1", &fail_status);
        require(!failed.ok, "corrupt bytecode deploy accepted");
        require(failed.worker == nullptr, "failed deploy returned a worker");
        const std::string active_after = read_file(
            fixtures.state_root + "/apps/orders/active.json");
        require(active_after == active_before,
                "failed deploy changed the old active");
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode == "host_managed_secret_snapshot") {
        // A secret file + an env request; the warmed worker receives the
        // value via capsid:env and serves it.
        require(mkdirat(fixtures.secrets_fd, "orders", 0700) == 0,
                "cannot create secret app dir");
        write_file(fixtures.secret_root + "/orders/api-token",
                   "secret-value-managed-1");
        write_file(fixtures.vdir + "/capsid.json", kSecretAppConfig);
        write_file(fixtures.vdir + "/bundle.mjs",
                   "import { env } from 'capsid:env';\n"
                   "export default { fetch: () => new Response(env.get('APP_TOKEN')) };");
        capsid::host::ManagedHostOptions options = make_options(fixtures);
        capsid::host::OperationStatus status;
        const capsid::host::DeployOutcome outcome =
            capsid::host::managed_deploy(&options, "v1", &status);
        require(outcome.ok, "secret deploy failed: " + outcome.error);
        require(outcome.worker != nullptr, "no worker for secret deploy");
        require(run_request(outcome.worker) == "secret-value-managed-1",
                "secret value did not reach the worker");
        capsid_worker_destroy(outcome.worker);
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode == "host_managed_secret_snapshot_limit_precedes_staging") {
        const std::string value(13U * 1024U, 'x');
        const std::string config =
            "{\"apiVersion\":\"capsid/app-v1\",\"entry\":\"bundle.mjs\","
            "\"permissions\":{\"modules\":[\"capsid:env\"],\"env\":{"
            "\"APP_A\":{\"value\":\"" + value + "\"},"
            "\"APP_B\":{\"value\":\"" + value + "\"},"
            "\"APP_C\":{\"value\":\"" + value + "\"},"
            "\"APP_D\":{\"value\":\"" + value + "\"}}},"
            "\"pool\":{\"minReady\":1,\"maxWorkers\":1}}";
        write_file(fixtures.vdir + "/capsid.json", config);
        capsid::host::ManagedHostOptions options = make_options(fixtures);
        capsid::host::OperationStatus status;
        const capsid::host::DeployOutcome outcome =
            capsid::host::managed_deploy(&options, "v1", &status);
        require(!outcome.ok, "oversized environment snapshot was accepted");
        require(outcome.worker == nullptr,
                "oversized environment snapshot returned a worker");
        require(outcome.error.find("/permissions/env") != std::string::npos,
                "secret snapshot compiler diagnostic lost its JSON Pointer");
        struct stat mapping = {};
        require(lstat((fixtures.state_root + "/apps/orders/versions/v1").c_str(),
                      &mapping) != 0 && errno == ENOENT,
                "invalid environment froze a Version mapping before compile");
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode == "host_managed_secret_app_symlink_rejected") {
        std::string escape_root = "/tmp/capsid-managed-secret-escape-XXXXXX";
        require(mkdtemp(&escape_root[0]) != nullptr,
                "cannot create secret escape fixture");
        write_file(escape_root + "/api-token", "escaped-secret-value");
        require(symlink(escape_root.c_str(),
                        (fixtures.secret_root + "/orders").c_str()) == 0,
                "cannot create secret App symlink");
        write_file(fixtures.vdir + "/capsid.json", kSecretAppConfig);

        capsid::host::ManagedHostOptions options = make_options(fixtures);
        capsid::host::OperationStatus status;
        const capsid::host::DeployOutcome outcome =
            capsid::host::managed_deploy(&options, "v1", &status);
        require(!outcome.ok,
                "managed deploy followed the secret App directory symlink");
        require(outcome.worker == nullptr,
                "secret App symlink rejection returned a worker");
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode == "host_managed_secret_value_not_persisted") {
        const std::string secret_value =
            "secret-value-must-never-enter-generation-metadata";
        require(mkdirat(fixtures.secrets_fd, "orders", 0700) == 0,
                "cannot create secret App directory");
        write_file(fixtures.secret_root + "/orders/api-token", secret_value);
        write_file(fixtures.vdir + "/capsid.json", kSecretAppConfig);
        capsid::host::ManagedHostOptions options = make_options(fixtures);
        capsid::host::OperationStatus status;
        const capsid::host::DeployOutcome deployed =
            capsid::host::managed_deploy(&options, "v1", &status);
        require(deployed.ok && deployed.worker != nullptr,
                "secret deploy failed: " + deployed.error);
        capsid_worker_destroy(deployed.worker);

        const std::string generation = json_string_field(
            read_file(fixtures.state_root + "/apps/orders/active.json"),
            "generation");
        const std::string generation_dir = fixtures.state_root +
            "/apps/orders/generations/" + generation;
        require(!directory_files_contain(generation_dir, secret_value),
                "secret value was persisted in the committed generation");
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode ==
        "host_managed_recovery_accepts_large_valid_env_metadata") {
        require(mkdirat(fixtures.secrets_fd, "orders", 0700) == 0,
                "cannot create large-metadata secret directory");
        std::string config =
            "{\"apiVersion\":\"capsid/app-v1\",\"entry\":\"bundle.mjs\","
            "\"permissions\":{\"modules\":[\"capsid:env\"],\"env\":{";
        constexpr std::size_t kEntryCount = 256;
        for (std::size_t index = 0; index < kEntryCount; ++index) {
            std::string decimal = std::to_string(index);
            decimal.insert(decimal.begin(), 3 - decimal.size(), '0');
            const std::string env_name = "APP_SECRET_" + decimal;
            std::string key_id = "key-" + decimal + "-";
            key_id.append(128 - key_id.size(), 'x');
            if (index > 0) {
                config += ',';
            }
            config += "\"" + env_name + "\":{\"valueFrom\":\"" +
                      key_id + "\"}";
            write_file(fixtures.secret_root + "/orders/" + key_id, "x");
        }
        config +=
            "}},\"pool\":{\"minReady\":1,\"maxWorkers\":1}}";
        write_file(fixtures.vdir + "/capsid.json", config);

        capsid::host::ManagedHostOptions options = make_options(fixtures);
        capsid::host::OperationStatus deploy_status;
        const capsid::host::DeployOutcome deployed =
            capsid::host::managed_deploy(&options, "v1", &deploy_status);
        require(deployed.ok && deployed.worker != nullptr,
                "large valid metadata deploy failed: " + deployed.error);
        capsid_worker_destroy(deployed.worker);
        const std::string generation = json_string_field(
            read_file(fixtures.state_root + "/apps/orders/active.json"),
            "generation");
        const std::string env_json = read_file(
            fixtures.state_root + "/apps/orders/generations/" + generation +
            "/env.json");
        require(env_json.size() >
                    capsid::host::kMaxEnvironmentSnapshotBytes,
                "large-metadata fixture did not exceed the value budget");

        capsid::host::OperationStatus recover_status;
        const capsid::host::DeployOutcome recovered =
            capsid::host::managed_recover(&options, &recover_status);
        require(recovered.ok && recovered.worker != nullptr,
                "recovery rejected metadata produced by the compiler: " +
                    recovered.error);
        capsid_worker_destroy(recovered.worker);
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode == "host_managed_retire_and_recovery") {
        capsid::host::ManagedHostOptions options = make_options(fixtures);
        capsid::host::OperationStatus status;
        const capsid::host::DeployOutcome deployed =
            capsid::host::managed_deploy(&options, "v1", &status);
        require(deployed.ok, "deploy failed");
        capsid_worker_destroy(deployed.worker);
        capsid::host::OperationStatus retire_status;
        const capsid::host::DeployOutcome retired =
            capsid::host::managed_retire(&options, &retire_status);
        require(retired.ok, "retire failed");
        // Recovery after retire: no worker started, ok.
        capsid::host::OperationStatus recover_status;
        const capsid::host::DeployOutcome recovered =
            capsid::host::managed_recover(&options, &recover_status);
        require(recovered.ok, "recovery failed");
        // Re-deploy a new version reactivates.
        const std::string v2 = fixtures.apps_root + "/orders/v2";
        require(mkdir(v2.c_str(), 0700) == 0, "cannot create v2");
        write_file(v2 + "/capsid.json", kMinimalAppConfig);
        write_file(v2 + "/bundle.mjs", kSourceBundle);
        capsid::host::OperationStatus deploy2_status;
        const capsid::host::DeployOutcome redeployed =
            capsid::host::managed_deploy(&options, "v2", &deploy2_status);
        require(redeployed.ok, "re-deploy failed: " + redeployed.error);
        capsid_worker_destroy(redeployed.worker);
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode == "host_managed_retire_is_idempotent") {
        capsid::host::ManagedHostOptions options = make_options(fixtures);
        capsid::host::OperationStatus deploy_status;
        const capsid::host::DeployOutcome deployed =
            capsid::host::managed_deploy(&options, "v1", &deploy_status);
        require(deployed.ok && deployed.worker != nullptr,
                "initial deploy failed: " + deployed.error);
        capsid_worker_destroy(deployed.worker);

        capsid::host::OperationStatus first_status;
        const capsid::host::DeployOutcome first =
            capsid::host::managed_retire(&options, &first_status);
        require(first.ok && !first.operation_id.empty(),
                "first retire failed: " + first.error);
        const std::string tombstone = read_file(
            fixtures.state_root + "/apps/orders/active.json");

        capsid::host::OperationStatus second_status;
        const capsid::host::DeployOutcome second =
            capsid::host::managed_retire(&options, &second_status);
        require(second.ok && !second.operation_id.empty(),
                "repeated retire was not idempotent: " + second.error);
        require(second.operation_id != first.operation_id,
                "separate retire operations reused an operation id");
        require(read_file(fixtures.state_root + "/apps/orders/active.json") ==
                    tombstone,
                "idempotent retire changed the canonical tombstone");
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode == "host_managed_recovery_warms_worker") {
        capsid::host::ManagedHostOptions options = make_options(fixtures);
        capsid::host::OperationStatus deploy_status;
        const capsid::host::DeployOutcome deployed =
            capsid::host::managed_deploy(&options, "v1", &deploy_status);
        require(deployed.ok && deployed.worker != nullptr,
                "initial deploy failed: " + deployed.error);
        capsid_worker_destroy(deployed.worker);

        capsid::host::OperationStatus recover_status;
        const capsid::host::DeployOutcome recovered =
            capsid::host::managed_recover(&options, &recover_status);
        require(recovered.ok, "active recovery failed: " + recovered.error);
        require(recover_status.state == capsid::host::OperationState::kActive,
                "active recovery did not report Active");
        require(recovered.worker != nullptr,
                "active recovery reported Active without a READY worker");
        require(run_request(recovered.worker) == "managed-ok",
                "recovered worker did not serve the committed bundle");
        capsid_worker_destroy(recovered.worker);
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode == "host_managed_version_immutability") {
        capsid::host::ManagedHostOptions options = make_options(fixtures);
        capsid::host::OperationStatus first_status;
        const capsid::host::DeployOutcome first =
            capsid::host::managed_deploy(&options, "v1", &first_status);
        require(first.ok && first.worker != nullptr,
                "initial version deploy failed: " + first.error);
        capsid_worker_destroy(first.worker);
        const std::string active_before = read_file(
            fixtures.state_root + "/apps/orders/active.json");

        // Exact same App/Version content is idempotent and may either reuse
        // the active worker or return a newly warmed equivalent worker.
        capsid::host::OperationStatus repeat_status;
        const capsid::host::DeployOutcome repeated =
            capsid::host::managed_deploy(&options, "v1", &repeat_status);
        require(repeated.ok, "identical version redeploy was not idempotent");
        if (repeated.worker != nullptr) {
            capsid_worker_destroy(repeated.worker);
        }
        require(read_file(fixtures.state_root + "/apps/orders/active.json") ==
                    active_before,
                "identical version redeploy changed active identity");

        write_file(fixtures.vdir + "/bundle.mjs",
                   "export default { fetch: () => new Response('mutated') };");
        capsid::host::OperationStatus conflict_status;
        const capsid::host::DeployOutcome conflict =
            capsid::host::managed_deploy(&options, "v1", &conflict_status);
        require(!conflict.ok,
                "same Version ID accepted different immutable content");
        require(conflict.worker == nullptr,
                "version immutability conflict returned a worker");
        require(read_file(fixtures.state_root + "/apps/orders/active.json") ==
                    active_before,
                "version immutability conflict changed old active state");
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode == "host_managed_version_mapping_canonical_path") {
        capsid::host::ManagedHostOptions options = make_options(fixtures);
        capsid::host::OperationStatus status;
        const capsid::host::DeployOutcome deployed =
            capsid::host::managed_deploy(&options, "v1", &status);
        require(deployed.ok && deployed.worker != nullptr,
                "deploy failed: " + deployed.error);
        capsid_worker_destroy(deployed.worker);
        struct stat canonical = {};
        require(lstat((fixtures.state_root +
                       "/apps/orders/versions/v1.json").c_str(),
                      &canonical) == 0 && S_ISREG(canonical.st_mode),
                "immutable Version mapping is not versions/<version>.json");
        struct stat legacy = {};
        require(lstat((fixtures.state_root + "/apps/orders/versions/v1").c_str(),
                      &legacy) != 0 && errno == ENOENT,
                "legacy non-canonical Version mapping was published");
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode == "host_managed_resource_config_affects_identity") {
        capsid::host::ManagedHostOptions options = make_options(fixtures);
        capsid::host::OperationStatus first_status;
        const capsid::host::DeployOutcome first =
            capsid::host::managed_deploy(&options, "v1", &first_status);
        require(first.ok && first.worker != nullptr,
                "baseline deploy failed: " + first.error);
        capsid_worker_destroy(first.worker);
        const std::string first_generation = json_string_field(
            read_file(fixtures.state_root + "/apps/orders/active.json"),
            "generation");

        create_version(
            fixtures, "v2",
            R"json({"apiVersion":"capsid/app-v1","entry":"bundle.mjs","permissions":{"modules":["capsid:env"]},"worker":{"memoryMax":"64MiB"},"pool":{"minReady":1,"maxWorkers":1}})json",
            kSourceBundle);
        capsid::host::OperationStatus second_status;
        const capsid::host::DeployOutcome second =
            capsid::host::managed_deploy(&options, "v2", &second_status);
        require(second.ok && second.worker != nullptr,
                "resource-bearing deploy failed: " + second.error);
        capsid_worker_destroy(second.worker);
        const std::string second_generation = json_string_field(
            read_file(fixtures.state_root + "/apps/orders/active.json"),
            "generation");
        require(second_generation != first_generation,
                "worker resource config was omitted from generation identity");
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode == "host_managed_applies_worker_memory_limit") {
        write_file(
            fixtures.vdir + "/capsid.json",
            R"json({"apiVersion":"capsid/app-v1","entry":"bundle.mjs","worker":{"memoryMax":"1"},"pool":{"minReady":1,"maxWorkers":1}})json");
        capsid::host::ManagedHostOptions options = make_options(fixtures);
        capsid::host::OperationStatus status;
        const capsid::host::DeployOutcome deployed =
            capsid::host::managed_deploy(&options, "v1", &status);
        if (deployed.worker != nullptr) {
            capsid_worker_destroy(deployed.worker);
        }
        require(!deployed.ok,
                "worker.memoryMax changed identity but was not enforced");
        require(status.state == capsid::host::OperationState::kFailed,
                "invalid worker memory limit did not fail deployment");
        struct stat active = {};
        require(stat((fixtures.state_root + "/apps/orders/active.json").c_str(),
                     &active) != 0 && errno == ENOENT,
                "unenforced worker memory limit published active.json");
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode == "host_managed_rejects_request_limit_over_host") {
        write_file(
            fixtures.vdir + "/capsid.json",
            R"json({"apiVersion":"capsid/app-v1","entry":"bundle.mjs","request":{"maxInflightPerWorker":10001},"pool":{"minReady":1,"maxWorkers":1}})json");
        capsid::host::ManagedHostOptions options = make_options(fixtures);
        options.host_policy.max_requests_per_worker = 10000;
        capsid::host::OperationStatus status;
        const capsid::host::DeployOutcome deployed =
            capsid::host::managed_deploy(&options, "v1", &status);
        if (deployed.worker != nullptr) {
            capsid_worker_destroy(deployed.worker);
        }
        require(!deployed.ok,
                "App request limit above the Host maximum was ignored");
        require(status.state == capsid::host::OperationState::kFailed,
                "request limit overreach did not fail deployment");
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode == "host_managed_host_policy_affects_identity") {
        capsid::host::ManagedHostOptions first_options = make_options(fixtures);
        capsid::host::OperationStatus first_status;
        const capsid::host::DeployOutcome first =
            capsid::host::managed_deploy(&first_options, "v1", &first_status);
        require(first.ok && first.worker != nullptr,
                "baseline deploy failed: " + first.error);
        capsid_worker_destroy(first.worker);
        const std::string first_generation = json_string_field(
            read_file(fixtures.state_root + "/apps/orders/active.json"),
            "generation");

        Fixtures changed =
            make_fixtures(worker_path, compiler_tool, fixture_source);
        write_file(changed.vdir + "/capsid.json", kMinimalAppConfig);
        write_file(changed.vdir + "/bundle.mjs", kSourceBundle);
        capsid::host::ManagedHostOptions changed_options = make_options(changed);
        changed_options.host_policy.max_worker_memory_bytes =
            512U * 1024U * 1024U;
        capsid::host::OperationStatus changed_status;
        const capsid::host::DeployOutcome changed_outcome =
            capsid::host::managed_deploy(&changed_options, "v1",
                                         &changed_status);
        require(changed_outcome.ok && changed_outcome.worker != nullptr,
                "changed Host policy deploy failed: " + changed_outcome.error);
        capsid_worker_destroy(changed_outcome.worker);
        const std::string changed_generation = json_string_field(
            read_file(changed.state_root + "/apps/orders/active.json"),
            "generation");
        require(changed_generation != first_generation,
                "Host policy was replaced by a constant generation digest");
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode == "host_managed_fetch_policy_warms_worker") {
        write_file(
            fixtures.vdir + "/capsid.json",
            R"json({"apiVersion":"capsid/app-v1","entry":"bundle.mjs","permissions":{"fetch":{"allow":["api.example:443"]}},"pool":{"minReady":1,"maxWorkers":1}})json");
        capsid::host::ManagedHostOptions options = make_options(fixtures);
        options.host_policy.fetch_targets = { { "api.example", { 443 } } };
        capsid::host::OperationStatus status;
        const capsid::host::DeployOutcome deployed =
            capsid::host::managed_deploy(&options, "v1", &status);
        require(deployed.ok,
                "valid fetch policy could not warm a worker: " +
                    deployed.error);
        require(deployed.worker != nullptr,
                "valid fetch policy returned no READY worker");
        require(run_request(deployed.worker) == "managed-ok",
                "fetch policy worker did not serve the bundle");
        capsid_worker_destroy(deployed.worker);
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode == "host_managed_multiple_rules_stable") {
        write_file(
            fixtures.vdir + "/capsid.json",
            R"json({"apiVersion":"capsid/app-v1","entry":"bundle.mjs","permissions":{"fs":{"read":{"allow":["/tmp/capsid-managed-a","/tmp/capsid-managed-b"]}}},"pool":{"minReady":1,"maxWorkers":1}})json");
        capsid::host::ManagedHostOptions options = make_options(fixtures);
        options.host_policy.fs_read_roots = {
            "/tmp/capsid-managed-a", "/tmp/capsid-managed-b"
        };
        capsid::host::OperationStatus status;
        const capsid::host::DeployOutcome deployed =
            capsid::host::managed_deploy(&options, "v1", &status);
        require(deployed.ok,
                "multiple effective rules corrupted the Runtime descriptor: " +
                    deployed.error);
        require(deployed.worker != nullptr,
                "multiple effective rules returned no READY worker");
        capsid_worker_destroy(deployed.worker);
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode == "host_managed_multiple_env_entries_stable") {
        write_file(
            fixtures.vdir + "/capsid.json",
            R"json({"apiVersion":"capsid/app-v1","entry":"bundle.mjs","permissions":{"modules":["capsid:env"],"env":{"APP_A":{"value":"one"},"APP_B":{"value":"two"}}},"pool":{"minReady":1,"maxWorkers":1}})json");
        write_file(fixtures.vdir + "/bundle.mjs",
                   "import { env } from 'capsid:env';\n"
                   "export default { fetch: () => new Response("
                   "env.get('APP_A') + ':' + env.get('APP_B')) };\n");
        capsid::host::ManagedHostOptions options = make_options(fixtures);
        capsid::host::OperationStatus status;
        const capsid::host::DeployOutcome deployed =
            capsid::host::managed_deploy(&options, "v1", &status);
        require(deployed.ok,
                "multiple env entries corrupted the Runtime descriptor: " +
                    deployed.error);
        require(deployed.worker != nullptr,
                "multiple env entries returned no READY worker");
        require(run_request(deployed.worker) == "one:two",
                "multiple env entries reached the worker incorrectly");
        capsid_worker_destroy(deployed.worker);
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode == "host_managed_rejects_unknown_config") {
        capsid::host::ManagedHostOptions options = make_options(fixtures);
        capsid::host::OperationStatus first_status;
        const capsid::host::DeployOutcome first =
            capsid::host::managed_deploy(&options, "v1", &first_status);
        require(first.ok && first.worker != nullptr,
                "initial deploy failed: " + first.error);
        capsid_worker_destroy(first.worker);
        const std::string active_before = read_file(
            fixtures.state_root + "/apps/orders/active.json");

        create_version(
            fixtures, "v2",
            R"json({"apiVersion":"capsid/app-v1","entry":"bundle.mjs","permissions":{},"pool":{"minReady":1,"maxWorkers":1},"mystery":true})json",
            "export default { fetch: () => new Response('v2-ok') };");
        capsid::host::OperationStatus invalid_status;
        const capsid::host::DeployOutcome invalid =
            capsid::host::managed_deploy(&options, "v2", &invalid_status);
        require(!invalid.ok, "managed deploy accepted an unknown config field");
        require(invalid.worker == nullptr,
                "invalid config returned a warmed worker");
        require(invalid.error.find("/mystery") != std::string::npos,
                "unknown config diagnostic omitted its JSON Pointer");
        require(read_file(fixtures.state_root + "/apps/orders/active.json") ==
                    active_before,
                "invalid config changed old active state");
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode == "host_managed_literal_env_without_secret_root") {
        write_file(fixtures.vdir + "/capsid.json", kLiteralEnvAppConfig);
        write_file(fixtures.vdir + "/bundle.mjs",
                   "import { env } from 'capsid:env';\n"
                   "export default { fetch: () => new Response(env.get('APP_MODE')) };");
        capsid::host::ManagedHostOptions options = make_options(fixtures);
        options.secret_root_template_fd = -1;
        capsid::host::OperationStatus status;
        const capsid::host::DeployOutcome outcome =
            capsid::host::managed_deploy(&options, "v1", &status);
        require(outcome.ok, "literal env required a secret root: " + outcome.error);
        require(outcome.worker != nullptr, "literal env deploy returned no worker");
        require(run_request(outcome.worker) == "literal-managed",
                "literal env value did not reach the worker");
        capsid_worker_destroy(outcome.worker);
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode == "host_managed_runtime_identity_fail_closed") {
        capsid::host::ManagedHostOptions options = make_options(fixtures);
        options.runtime_compatibility_id =
            "sha256:0000000000000000000000000000000000000000000000000000000000000000";
        capsid::host::OperationStatus status;
        const capsid::host::DeployOutcome outcome =
            capsid::host::managed_deploy(&options, "v1", &status);
        require(!outcome.ok, "worker compatibility mismatch was accepted");
        require(outcome.worker == nullptr,
                "worker compatibility mismatch returned a worker");
        require(status.state == capsid::host::OperationState::kFailed,
                "worker compatibility mismatch did not fail the operation");
        struct stat active = {};
        require(stat((fixtures.state_root + "/apps/orders/active.json").c_str(),
                     &active) != 0 && errno == ENOENT,
                "worker compatibility mismatch published active.json");
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode == "host_managed_state_root_symlink_rejected") {
        std::string escape_root = "/tmp/capsid-managed-state-escape-XXXXXX";
        require(mkdtemp(&escape_root[0]) != nullptr,
                "cannot create state escape fixture");
        require(symlink(escape_root.c_str(),
                        (fixtures.state_root + "/apps").c_str()) == 0,
                "cannot create state apps symlink");

        capsid::host::ManagedHostOptions options = make_options(fixtures);
        capsid::host::OperationStatus status;
        const capsid::host::DeployOutcome outcome =
            capsid::host::managed_deploy(&options, "v1", &status);
        require(!outcome.ok, "managed deploy followed stateRoot/apps symlink");
        require(outcome.worker == nullptr,
                "stateRoot symlink rejection returned a worker");
        struct stat escaped = {};
        require(stat((escape_root + "/orders/active.json").c_str(), &escaped) !=
                    0 &&
                    errno == ENOENT,
                "managed deploy published active state outside stateRoot");
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode == "host_managed_staging_symlink_rejected") {
        std::string escape_root = "/tmp/capsid-managed-staging-escape-XXXXXX";
        require(mkdtemp(&escape_root[0]) != nullptr,
                "cannot create staging escape fixture");
        require(symlink(escape_root.c_str(),
                        (fixtures.state_root + "/staging").c_str()) == 0,
                "cannot create staging symlink");

        capsid::host::ManagedHostOptions options = make_options(fixtures);
        capsid::host::OperationStatus status;
        const capsid::host::DeployOutcome outcome =
            capsid::host::managed_deploy(&options, "v1", &status);
        require(!outcome.ok,
                "managed deploy followed the stateRoot/staging symlink");
        require(outcome.worker == nullptr,
                "staging symlink rejection returned a worker");
        struct stat active = {};
        require(stat((fixtures.state_root + "/apps/orders/active.json").c_str(),
                     &active) != 0 && errno == ENOENT,
                "staging symlink deploy published active.json");
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode == "host_managed_generations_symlink_rejected") {
        std::string escape_root =
            "/tmp/capsid-managed-generations-escape-XXXXXX";
        require(mkdtemp(&escape_root[0]) != nullptr,
                "cannot create generations escape fixture");
        require(mkdir((fixtures.state_root + "/apps").c_str(), 0700) == 0,
                "cannot create state apps directory");
        require(mkdir((fixtures.state_root + "/apps/orders").c_str(), 0700) ==
                    0,
                "cannot create App state directory");
        require(symlink(escape_root.c_str(),
                        (fixtures.state_root +
                         "/apps/orders/generations").c_str()) == 0,
                "cannot create generations symlink");

        capsid::host::ManagedHostOptions options = make_options(fixtures);
        capsid::host::OperationStatus status;
        const capsid::host::DeployOutcome outcome =
            capsid::host::managed_deploy(&options, "v1", &status);
        require(!outcome.ok,
                "managed deploy accepted a symlinked generations directory");
        require(outcome.worker == nullptr,
                "generations symlink rejection returned a worker");
        require(!directory_has_entries(escape_root),
                "managed deploy published a generation outside stateRoot");
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode == "host_managed_complete_symlink_rejected") {
        const std::string app_state = fixtures.state_root + "/apps/orders";
        const std::string generation =
            "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
        require(mkdir((fixtures.state_root + "/apps").c_str(), 0700) == 0,
                "cannot create state apps directory");
        require(mkdir(app_state.c_str(), 0700) == 0,
                "cannot create App state directory");
        require(mkdir((app_state + "/generations").c_str(), 0700) == 0,
                "cannot create generations directory");
        require(mkdir((app_state + "/generations/" + generation).c_str(),
                      0700) == 0,
                "cannot create generation directory");
        const std::string outside_marker = fixtures.state_root + "/outside";
        write_file(outside_marker, "not-a-generation-marker");
        require(symlink(outside_marker.c_str(),
                        (app_state + "/generations/" + generation +
                         "/COMPLETE")
                            .c_str()) == 0,
                "cannot create COMPLETE symlink");
        write_file(
            app_state + "/active.json",
            "{\"schema\":\"capsid-active-v1\",\"app\":\"orders\","
            "\"state\":\"active\",\"version\":\"v1\",\"generation\":\"" +
                generation + "\"}");

        capsid::host::ManagedHostOptions options = make_options(fixtures);
        capsid::host::OperationStatus status;
        const capsid::host::DeployOutcome outcome =
            capsid::host::managed_recover(&options, &status);
        require(!outcome.ok,
                "recovery accepted a symlink as the COMPLETE marker");
        require(outcome.worker == nullptr,
                "invalid COMPLETE marker returned a worker");
        require(status.state == capsid::host::OperationState::kFailed,
                "invalid COMPLETE marker did not fail recovery");
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode == "host_managed_concurrent_operation_ids") {
        constexpr std::size_t kThreadCount = 32;
        capsid::host::ManagedHostOptions options = make_options(fixtures);
        std::barrier start(static_cast<std::ptrdiff_t>(kThreadCount));
        std::vector<std::string> operation_ids(kThreadCount);
        std::vector<std::thread> threads;
        threads.reserve(kThreadCount);
        for (std::size_t index = 0; index < kThreadCount; ++index) {
            threads.emplace_back([&, index]() {
                start.arrive_and_wait();
                capsid::host::OperationStatus status;
                const capsid::host::DeployOutcome outcome =
                    capsid::host::managed_deploy(
                        &options, "missing-" + std::to_string(index), &status);
                require(!outcome.ok,
                        "missing concurrent version unexpectedly deployed");
                operation_ids[index] = outcome.operation_id;
            });
        }
        for (std::thread& thread : threads) {
            thread.join();
        }
        const std::set<std::string> unique_ids(operation_ids.begin(),
                                                operation_ids.end());
        require(unique_ids.size() == kThreadCount,
                "concurrent operations received duplicate IDs");
        require(unique_ids.find("") == unique_ids.end(),
                "concurrent operation received an empty ID");
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode == "host_managed_recovery_uses_committed_generation") {
        capsid::host::ManagedHostOptions options = make_options(fixtures);
        capsid::host::OperationStatus deploy_status;
        const capsid::host::DeployOutcome deployed =
            capsid::host::managed_deploy(&options, "v1", &deploy_status);
        require(deployed.ok && deployed.worker != nullptr,
                "initial deploy failed: " + deployed.error);
        capsid_worker_destroy(deployed.worker);

        // applicationsRoot is an upload boundary, not the recovery source.
        // Once active.json points at a complete internal generation, restart
        // must be able to rebuild exclusively from that committed snapshot.
        const std::string detached = fixtures.vdir + ".detached";
        require(rename(fixtures.vdir.c_str(), detached.c_str()) == 0,
                "cannot detach uploaded version after commit");

        capsid::host::OperationStatus recover_status;
        const capsid::host::DeployOutcome recovered =
            capsid::host::managed_recover(&options, &recover_status);
        require(recovered.ok,
                "recovery depended on mutable applicationsRoot: " +
                    recovered.error);
        require(recovered.worker != nullptr,
                "committed generation recovery returned no READY worker");
        require(run_request(recovered.worker) == "managed-ok",
                "committed generation recovery served the wrong bundle");
        capsid_worker_destroy(recovered.worker);
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode == "host_managed_recovery_cleans_stale_temp") {
        capsid::host::ManagedHostOptions options = make_options(fixtures);
        capsid::host::OperationStatus deploy_status;
        const capsid::host::DeployOutcome deployed =
            capsid::host::managed_deploy(&options, "v1", &deploy_status);
        require(deployed.ok && deployed.worker != nullptr,
                "initial deploy failed: " + deployed.error);
        capsid_worker_destroy(deployed.worker);

        const std::string stale = fixtures.state_root +
            "/apps/orders/active.json.tmp.interrupted-operation";
        write_file(stale, "partial-active-state");
        capsid::host::OperationStatus recover_status;
        const capsid::host::DeployOutcome recovered =
            capsid::host::managed_recover(&options, &recover_status);
        require(recovered.ok && recovered.worker != nullptr,
                "valid active state did not recover with a stale temp");
        capsid_worker_destroy(recovered.worker);
        struct stat stale_stat = {};
        require(stat(stale.c_str(), &stale_stat) != 0 && errno == ENOENT,
                "startup recovery did not clean active.json.tmp.*");
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode == "host_managed_recovery_rejects_generation_tamper") {
        capsid::host::ManagedHostOptions options = make_options(fixtures);
        capsid::host::OperationStatus deploy_status;
        const capsid::host::DeployOutcome deployed =
            capsid::host::managed_deploy(&options, "v1", &deploy_status);
        require(deployed.ok && deployed.worker != nullptr,
                "initial deploy failed: " + deployed.error);
        capsid_worker_destroy(deployed.worker);
        const std::string generation = json_string_field(
            read_file(fixtures.state_root + "/apps/orders/active.json"),
            "generation");
        write_file(
            fixtures.state_root + "/apps/orders/generations/" + generation +
                "/bundle.bin",
            "export default { fetch: () => new Response('tampered') };");

        capsid::host::OperationStatus recover_status;
        const capsid::host::DeployOutcome recovered =
            capsid::host::managed_recover(&options, &recover_status);
        if (recovered.worker != nullptr) {
            capsid_worker_destroy(recovered.worker);
        }
        require(!recovered.ok,
                "recovery activated a tampered committed bundle");
        require(recover_status.state == capsid::host::OperationState::kFailed,
                "generation tamper did not fail the recovery operation");
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode == "host_managed_recovery_rejects_snapshot_fifo_promptly") {
        capsid::host::ManagedHostOptions options = make_options(fixtures);
        capsid::host::OperationStatus deploy_status;
        const capsid::host::DeployOutcome deployed =
            capsid::host::managed_deploy(&options, "v1", &deploy_status);
        require(deployed.ok && deployed.worker != nullptr,
                "snapshot FIFO baseline deploy failed: " + deployed.error);
        capsid_worker_destroy(deployed.worker);
        const std::string generation = json_string_field(
            read_file(fixtures.state_root + "/apps/orders/active.json"),
            "generation");
        const std::string effective_path =
            fixtures.state_root + "/apps/orders/generations/" + generation +
            "/effective.json";
        require(unlink(effective_path.c_str()) == 0,
                "cannot remove committed effective snapshot");
        require(mkfifo(effective_path.c_str(), 0600) == 0,
                "cannot install committed snapshot FIFO");

        require(recovery_fails_promptly(&options,
                                        std::chrono::milliseconds(1500)),
                "recovery blocked on a committed snapshot FIFO");
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode ==
        "host_managed_recovery_rejects_optional_snapshot_fifo") {
        capsid::host::ManagedHostOptions options = make_options(fixtures);
        capsid::host::OperationStatus deploy_status;
        const capsid::host::DeployOutcome deployed =
            capsid::host::managed_deploy(&options, "v1", &deploy_status);
        require(deployed.ok && deployed.worker != nullptr,
                "optional FIFO baseline deploy failed: " + deployed.error);
        capsid_worker_destroy(deployed.worker);
        const std::string generation = json_string_field(
            read_file(fixtures.state_root + "/apps/orders/active.json"),
            "generation");
        const std::string bytecode_path =
            fixtures.state_root + "/apps/orders/generations/" + generation +
            "/bytecode.bin";
        require(mkfifo(bytecode_path.c_str(), 0600) == 0,
                "cannot install optional snapshot FIFO");

        require(recovery_fails_promptly(&options,
                                        std::chrono::milliseconds(1500)),
                "recovery ignored or blocked on an invalid optional snapshot");
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode == "host_managed_recovery_rejects_oversized_snapshot") {
#if defined(__SANITIZE_ADDRESS__)
        // The deterministic Release probe constrains its child with
        // RLIMIT_AS so an unbounded reader cannot consume host memory.
        // ASan reserves a huge shadow range and is fundamentally
        // incompatible with that limit; the same production read helper is
        // exercised by the FIFO/type test in this build.
        std::cout << "PASS" << std::endl;
        return 0;
#else
        capsid::host::ManagedHostOptions options = make_options(fixtures);
        capsid::host::OperationStatus deploy_status;
        const capsid::host::DeployOutcome deployed =
            capsid::host::managed_deploy(&options, "v1", &deploy_status);
        require(deployed.ok && deployed.worker != nullptr,
                "snapshot-size baseline deploy failed: " + deployed.error);
        capsid_worker_destroy(deployed.worker);
        const std::string generation = json_string_field(
            read_file(fixtures.state_root + "/apps/orders/active.json"),
            "generation");
        const std::string effective_path =
            fixtures.state_root + "/apps/orders/generations/" + generation +
            "/effective.json";
        const int fd = open(effective_path.c_str(), O_WRONLY | O_TRUNC);
        require(fd >= 0, "cannot open committed snapshot for size probe");
        constexpr off_t kOversizedSnapshotBytes =
            static_cast<off_t>(512) * 1024 * 1024;
        require(ftruncate(fd, kOversizedSnapshotBytes) == 0,
                "cannot create oversized sparse snapshot");
        close(fd);

        constexpr rlim_t kProbeAddressSpace =
            static_cast<rlim_t>(256) * 1024 * 1024;
        require(recovery_fails_promptly(&options,
                                        std::chrono::milliseconds(3000),
                                        kProbeAddressSpace),
                "recovery did not reject an oversized snapshot before reading it");
        std::cout << "PASS" << std::endl;
        return 0;
#endif
    }

    if (mode == "host_managed_recovery_revalidates_host_policy") {
        capsid::host::ManagedHostOptions options = make_options(fixtures);
        capsid::host::OperationStatus deploy_status;
        const capsid::host::DeployOutcome deployed =
            capsid::host::managed_deploy(&options, "v1", &deploy_status);
        require(deployed.ok && deployed.worker != nullptr,
                "initial deploy failed: " + deployed.error);
        capsid_worker_destroy(deployed.worker);

        // Restart uses the current Host authority. Revoking a module must
        // not resurrect the effective policy stored by an older generation.
        options.host_policy.module_allowlist.clear();
        capsid::host::OperationStatus recover_status;
        const capsid::host::DeployOutcome recovered =
            capsid::host::managed_recover(&options, &recover_status);
        if (recovered.worker != nullptr) {
            capsid_worker_destroy(recovered.worker);
        }
        require(!recovered.ok,
                "recovery bypassed the current Host module policy");
        require(recover_status.state == capsid::host::OperationState::kFailed,
                "Host policy revocation did not fail recovery");
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode == "host_managed_recovery_never_reuses_stale_secret") {
        require(mkdirat(fixtures.secrets_fd, "orders", 0700) == 0,
                "cannot create secret App directory");
        write_file(fixtures.secret_root + "/orders/api-token", "secret-v1");
        write_file(fixtures.vdir + "/capsid.json", kSecretAppConfig);
        write_file(fixtures.vdir + "/bundle.mjs",
                   "import { env } from 'capsid:env';\n"
                   "export default { fetch: () => new Response(env.get('APP_TOKEN')) };");
        capsid::host::ManagedHostOptions options = make_options(fixtures);
        capsid::host::OperationStatus deploy_status;
        const capsid::host::DeployOutcome deployed =
            capsid::host::managed_deploy(&options, "v1", &deploy_status);
        require(deployed.ok && deployed.worker != nullptr,
                "initial secret deploy failed: " + deployed.error);
        capsid_worker_destroy(deployed.worker);
        const std::string old_active = read_file(
            fixtures.state_root + "/apps/orders/active.json");
        write_file(fixtures.secret_root + "/orders/api-token", "secret-v2");

        capsid::host::OperationStatus recover_status;
        const capsid::host::DeployOutcome recovered =
            capsid::host::managed_recover(&options, &recover_status);
        if (!recovered.ok) {
            require(recovered.worker == nullptr,
                    "failed secret revalidation returned a worker");
        } else {
            require(recovered.worker != nullptr,
                    "successful secret rotation returned no worker");
            require(run_request(recovered.worker) == "secret-v2",
                    "recovery reused the stale committed secret value");
            capsid_worker_destroy(recovered.worker);
            require(read_file(fixtures.state_root +
                              "/apps/orders/active.json") != old_active,
                    "secret rotation did not publish a new generation");
        }
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode == "host_managed_recovery_rejects_trusted_bundle_swap") {
        const TestKey key = test_key();
        std::vector<std::uint8_t> bytecode;
        std::string attestation;
        std::vector<std::uint8_t> signature;
        compile_and_sign(fixtures, key, &bytecode, &attestation, &signature);
        capsid::host::ManagedHostOptions options = make_options(fixtures);
        capsid::host::TrustedBytecodeKey trusted;
        trusted.key_id = "test-key-1";
        trusted.public_key = std::span<const std::uint8_t>(
            key.public_key.data(), key.public_key.size());
        options.trusted_keys.push_back(trusted);

        capsid::host::OperationStatus deploy_status;
        const capsid::host::DeployOutcome deployed =
            capsid::host::managed_deploy(&options, "v1", &deploy_status);
        require(deployed.ok && deployed.worker != nullptr,
                "trusted baseline deploy failed: " + deployed.error);
        capsid_worker_destroy(deployed.worker);
        const std::string generation = json_string_field(
            read_file(fixtures.state_root + "/apps/orders/active.json"),
            "generation");

        // Produce a second, valid QuickJS bytecode bundle with the same
        // sourceName but different code. Recovery must bind the bytes it
        // actually loads (bundle.bin) to the bytecode bytes covered by the
        // committed attestation (bytecode.bin).
        const std::string altered_source = fixtures.vdir + "/altered.mjs";
        const std::string altered_bytecode = fixtures.vdir + "/altered.qjsb";
        write_file(altered_source,
                   "export default { fetch: () => new Response('swapped') };");
        run_command({
            fixtures.compiler_tool,
            "--source", altered_source,
            "--source-name", "file://orders/v1/bundle.qjsb",
            "--application", "orders",
            "--version", "v1",
            "--key-id", "unused-key",
            "--bytecode-out", altered_bytecode,
            "--attestation-out", fixtures.vdir + "/altered.json",
            "--signing-message-out", fixtures.vdir + "/altered-message.bin",
        });
        write_file(fixtures.state_root + "/apps/orders/generations/" +
                       generation + "/bundle.bin",
                   read_file(altered_bytecode));

        capsid::host::OperationStatus recover_status;
        const capsid::host::DeployOutcome recovered =
            capsid::host::managed_recover(&options, &recover_status);
        if (recovered.worker != nullptr) {
            capsid_worker_destroy(recovered.worker);
        }
        require(!recovered.ok,
                "recovery loaded bytecode not covered by the attestation");
        require(recover_status.state == capsid::host::OperationState::kFailed,
                "trusted bundle swap did not fail recovery");
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode == "host_managed_recovery_rejects_trusted_attestation_drift") {
        const TestKey key = test_key();
        std::vector<std::uint8_t> bytecode;
        std::string attestation;
        std::vector<std::uint8_t> signature;
        compile_and_sign(fixtures, key, &bytecode, &attestation, &signature);
        capsid::host::ManagedHostOptions options = make_options(fixtures);
        capsid::host::TrustedBytecodeKey trusted;
        trusted.key_id = "test-key-1";
        trusted.public_key = std::span<const std::uint8_t>(
            key.public_key.data(), key.public_key.size());
        options.trusted_keys.push_back(trusted);
        capsid::host::OperationStatus deploy_status;
        const capsid::host::DeployOutcome deployed =
            capsid::host::managed_deploy(&options, "v1", &deploy_status);
        require(deployed.ok && deployed.worker != nullptr,
                "trusted baseline deploy failed: " + deployed.error);
        capsid_worker_destroy(deployed.worker);
        const std::string generation = json_string_field(
            read_file(fixtures.state_root + "/apps/orders/active.json"),
            "generation");
        const std::string attestation_path =
            fixtures.state_root + "/apps/orders/generations/" + generation +
            "/attestation.json";
        std::string drifted = read_file(attestation_path);
        require(!drifted.empty() && drifted.front() == '{',
                "trusted snapshot has no JSON attestation");
        // Whitespace changes the file digest without changing any signed
        // claim. Verification still succeeds, so recovery must separately
        // compare the exact attestation digest bound into the generation.
        drifted.insert(1, " ");
        write_file(attestation_path, drifted);

        capsid::host::OperationStatus recover_status;
        const capsid::host::DeployOutcome recovered =
            capsid::host::managed_recover(&options, &recover_status);
        if (recovered.worker != nullptr) {
            capsid_worker_destroy(recovered.worker);
        }
        require(!recovered.ok,
                "trusted recovery ignored attestation document drift");
        require(recover_status.state == capsid::host::OperationState::kFailed,
                "trusted attestation drift did not fail recovery");
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode == "host_managed_recovery_rejects_source_name_tamper") {
        capsid::host::ManagedHostOptions options = make_options(fixtures);
        capsid::host::OperationStatus deploy_status;
        const capsid::host::DeployOutcome deployed =
            capsid::host::managed_deploy(&options, "v1", &deploy_status);
        require(deployed.ok && deployed.worker != nullptr,
                "source baseline deploy failed: " + deployed.error);
        capsid_worker_destroy(deployed.worker);
        const std::string generation = json_string_field(
            read_file(fixtures.state_root + "/apps/orders/active.json"),
            "generation");
        const std::string artifact_path =
            fixtures.state_root + "/apps/orders/generations/" + generation +
            "/artifact.json";
        std::string artifact = read_file(artifact_path);
        const std::string committed_source_name =
            json_string_field(artifact, "sourceName");
        const std::string original =
            "\"sourceName\":\"" + committed_source_name + "\"";
        const std::string::size_type position = artifact.find(original);
        require(position != std::string::npos,
                "source snapshot omitted its sourceName");
        artifact.replace(position, original.size(),
                         "\"sourceName\":\"tampered-source-name.mjs\"");
        write_file(artifact_path, artifact);

        capsid::host::OperationStatus recover_status;
        const capsid::host::DeployOutcome recovered =
            capsid::host::managed_recover(&options, &recover_status);
        if (recovered.worker != nullptr) {
            capsid_worker_destroy(recovered.worker);
        }
        require(!recovered.ok,
                "recovery accepted a sourceName outside generation identity");
        require(recover_status.state == capsid::host::OperationState::kFailed,
                "sourceName tamper did not fail recovery");
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode == "host_managed_recovery_rejects_source_name_separator_tamper") {
        capsid::host::ManagedHostOptions options = make_options(fixtures);
        capsid::host::OperationStatus deploy_status;
        const capsid::host::DeployOutcome deployed =
            capsid::host::managed_deploy(&options, "v1", &deploy_status);
        require(deployed.ok && deployed.worker != nullptr,
                "source baseline deploy failed: " + deployed.error);
        capsid_worker_destroy(deployed.worker);
        const std::string generation = json_string_field(
            read_file(fixtures.state_root + "/apps/orders/active.json"),
            "generation");
        const std::string artifact_path =
            fixtures.state_root + "/apps/orders/generations/" + generation +
            "/artifact.json";
        std::string artifact = read_file(artifact_path);
        const std::string committed_source_name =
            json_string_field(artifact, "sourceName");
        const std::string::size_type separator =
            committed_source_name.rfind('/');
        require(separator != std::string::npos,
                "sourceName has no version/name separator");
        std::string malformed_source_name = committed_source_name;
        malformed_source_name[separator] = 'X';
        const std::string original =
            "\"sourceName\":\"" + committed_source_name + "\"";
        const std::string::size_type position = artifact.find(original);
        require(position != std::string::npos,
                "artifact snapshot omitted sourceName");
        artifact.replace(position, original.size(),
                         "\"sourceName\":\"" + malformed_source_name +
                             "\"");
        write_file(artifact_path, artifact);

        capsid::host::OperationStatus recover_status;
        const capsid::host::DeployOutcome recovered =
            capsid::host::managed_recover(&options, &recover_status);
        if (recovered.worker != nullptr) {
            capsid_worker_destroy(recovered.worker);
        }
        require(!recovered.ok,
                "sourceName parser accepted a non-slash path separator");
        require(recover_status.state == capsid::host::OperationState::kFailed,
                "malformed sourceName separator did not fail recovery");
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode == "host_managed_recovery_rejects_source_name_segment_tamper") {
        capsid::host::ManagedHostOptions options = make_options(fixtures);
        capsid::host::OperationStatus deploy_status;
        const capsid::host::DeployOutcome deployed =
            capsid::host::managed_deploy(&options, "v1", &deploy_status);
        require(deployed.ok && deployed.worker != nullptr,
                "source baseline deploy failed: " + deployed.error);
        capsid_worker_destroy(deployed.worker);
        const std::string generation = json_string_field(
            read_file(fixtures.state_root + "/apps/orders/active.json"),
            "generation");
        const std::string artifact_path =
            fixtures.state_root + "/apps/orders/generations/" + generation +
            "/artifact.json";
        std::string artifact = read_file(artifact_path);
        const std::string committed_source_name =
            json_string_field(artifact, "sourceName");
        const std::string prefix = "file://orders/";
        const std::string::size_type final_separator =
            committed_source_name.rfind('/');
        require(committed_source_name.rfind(prefix, 0) == 0 &&
                    final_separator > prefix.size(),
                "sourceName has no bound middle segment");
        std::string tampered_source_name = committed_source_name;
        const std::string::size_type segment_offset = prefix.size();
        tampered_source_name[segment_offset] =
            tampered_source_name[segment_offset] == 'x' ? 'y' : 'x';
        const std::string original =
            "\"sourceName\":\"" + committed_source_name + "\"";
        const std::string::size_type position = artifact.find(original);
        require(position != std::string::npos,
                "artifact snapshot omitted sourceName");
        artifact.replace(position, original.size(),
                         "\"sourceName\":\"" + tampered_source_name +
                             "\"");
        write_file(artifact_path, artifact);

        capsid::host::OperationStatus recover_status;
        const capsid::host::DeployOutcome recovered =
            capsid::host::managed_recover(&options, &recover_status);
        if (recovered.worker != nullptr) {
            capsid_worker_destroy(recovered.worker);
        }
        require(!recovered.ok,
                "recovery accepted a different valid sourceName segment");
        require(recover_status.state == capsid::host::OperationState::kFailed,
                "sourceName segment tamper did not fail recovery");
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode == "host_managed_idempotent_rejects_corrupt_generation") {
        capsid::host::ManagedHostOptions options = make_options(fixtures);
        capsid::host::OperationStatus deploy_status;
        const capsid::host::DeployOutcome deployed =
            capsid::host::managed_deploy(&options, "v1", &deploy_status);
        require(deployed.ok && deployed.worker != nullptr,
                "initial deploy failed: " + deployed.error);
        capsid_worker_destroy(deployed.worker);
        const std::string active_path =
            fixtures.state_root + "/apps/orders/active.json";
        const std::string active_before = read_file(active_path);
        const std::string generation =
            json_string_field(active_before, "generation");
        // COMPLETE alone is not proof that a content-addressed generation
        // still contains the bytes/configuration named by its digest.
        // Keep the marker but corrupt the committed artifact; an idempotent
        // Version mapping must validate the object before reusing it.
        write_file(fixtures.state_root + "/apps/orders/generations/" +
                       generation + "/bundle.bin",
                   "export default { fetch: () => new Response('corrupt') };");

        capsid::host::OperationStatus repeat_status;
        const capsid::host::DeployOutcome repeated =
            capsid::host::managed_deploy(&options, "v1", &repeat_status);
        if (repeated.worker != nullptr) {
            capsid_worker_destroy(repeated.worker);
        }
        require(!repeated.ok,
                "idempotent deploy reused a corrupt generation");
        require(repeat_status.state == capsid::host::OperationState::kFailed,
                "corrupt mapped generation did not fail redeploy");
        require(read_file(active_path) == active_before,
                "failed idempotent redeploy changed active state");
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode == "host_managed_shared_generation_cleans_staging") {
        capsid::host::ManagedHostOptions options = make_options(fixtures);
        capsid::host::OperationStatus first_status;
        const capsid::host::DeployOutcome first =
            capsid::host::managed_deploy(&options, "v1", &first_status);
        require(first.ok && first.worker != nullptr,
                "first shared-generation deploy failed: " + first.error);
        capsid_worker_destroy(first.worker);
        create_version(fixtures, "v2", kMinimalAppConfig, kSourceBundle);
        capsid::host::OperationStatus second_status;
        const capsid::host::DeployOutcome second =
            capsid::host::managed_deploy(&options, "v2", &second_status);
        require(second.ok && second.worker != nullptr,
                "second shared-generation deploy failed: " + second.error);
        capsid_worker_destroy(second.worker);
        require(!directory_has_entries(fixtures.state_root + "/staging"),
                "shared generation left its complete staging tree behind");
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode == "host_managed_shared_generation_redeploys") {
        capsid::host::ManagedHostOptions options = make_options(fixtures);
        capsid::host::OperationStatus first_status;
        const capsid::host::DeployOutcome first =
            capsid::host::managed_deploy(&options, "v1", &first_status);
        require(first.ok && first.worker != nullptr,
                "first shared-generation deploy failed: " + first.error);
        capsid_worker_destroy(first.worker);
        create_version(fixtures, "v2", kMinimalAppConfig, kSourceBundle);
        capsid::host::OperationStatus second_status;
        const capsid::host::DeployOutcome second =
            capsid::host::managed_deploy(&options, "v2", &second_status);
        require(second.ok && second.worker != nullptr,
                "second shared-generation deploy failed: " + second.error);
        capsid_worker_destroy(second.worker);

        // versions/v2.json now legitimately points at a generation first
        // published by v1. Repeating v2 must validate and reuse that object;
        // the first-publisher sourceName is not evidence of corruption.
        capsid::host::OperationStatus repeat_status;
        const capsid::host::DeployOutcome repeated =
            capsid::host::managed_deploy(&options, "v2", &repeat_status);
        require(repeated.ok && repeated.worker != nullptr,
                "idempotent v2 rejected its shared generation: " +
                    repeated.error);
        require(run_request(repeated.worker) == "managed-ok",
                "reused shared generation served the wrong source");
        capsid_worker_destroy(repeated.worker);
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode == "host_managed_shared_generation_recovers") {
        capsid::host::ManagedHostOptions options = make_options(fixtures);
        capsid::host::OperationStatus first_status;
        const capsid::host::DeployOutcome first =
            capsid::host::managed_deploy(&options, "v1", &first_status);
        require(first.ok && first.worker != nullptr,
                "first shared-generation deploy failed: " + first.error);
        capsid_worker_destroy(first.worker);
        create_version(fixtures, "v2", kMinimalAppConfig, kSourceBundle);
        capsid::host::OperationStatus second_status;
        const capsid::host::DeployOutcome second =
            capsid::host::managed_deploy(&options, "v2", &second_status);
        require(second.ok && second.worker != nullptr,
                "second shared-generation deploy failed: " + second.error);
        capsid_worker_destroy(second.worker);

        capsid::host::OperationStatus recover_status;
        const capsid::host::DeployOutcome recovered =
            capsid::host::managed_recover(&options, &recover_status);
        require(recovered.ok && recovered.worker != nullptr,
                "active v2 could not recover its shared generation: " +
                    recovered.error);
        require(run_request(recovered.worker) == "managed-ok",
                "recovered shared generation served the wrong source");
        capsid_worker_destroy(recovered.worker);
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode == "host_managed_storage_namespace_reaches_worker") {
        write_file(
            fixtures.vdir + "/capsid.json",
            R"json({"apiVersion":"capsid/app-v1","entry":"bundle.mjs","permissions":{"modules":["capsid:storage"],"storage":{"namespaces":["tenant-a"]}},"pool":{"minReady":1,"maxWorkers":1}})json");
        write_file(fixtures.vdir + "/bundle.mjs",
                   "import { storage } from 'capsid:storage';\n"
                   "export default { fetch() { storage.set('tenant-a', 'k', "
                   "'storage-ok'); return new Response(storage.get('tenant-a', "
                   "'k')); } };\n");
        capsid::host::ManagedHostOptions options = make_options(fixtures);
        options.host_policy.module_allowlist = { "capsid:storage" };
        options.host_policy.storage_allowed = true;
        (void)set_host_storage_namespaces(&options.host_policy,
                                          {"tenant-a"});
        capsid::host::OperationStatus status;
        const capsid::host::DeployOutcome deployed =
            capsid::host::managed_deploy(&options, "v1", &status);
        require(deployed.ok && deployed.worker != nullptr,
                "storage deploy failed: " + deployed.error);
        require(run_request(deployed.worker) == "storage-ok",
                "compiled storage namespace did not reach the worker");
        capsid_worker_destroy(deployed.worker);
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode == "host_managed_stdio_stream_reaches_worker") {
        write_file(
            fixtures.vdir + "/capsid.json",
            R"json({"apiVersion":"capsid/app-v1","entry":"bundle.mjs","permissions":{"modules":["capsid:stdio"],"stdio":["stdout"]},"pool":{"minReady":1,"maxWorkers":1}})json");
        write_file(fixtures.vdir + "/bundle.mjs",
                   "import { stdio } from 'capsid:stdio';\n"
                   "export default { fetch() { stdio.write('stdout', "
                   "'managed-log'); return new Response('stdio-ok'); } };\n");
        capsid::host::ManagedHostOptions options = make_options(fixtures);
        options.host_policy.module_allowlist = { "capsid:stdio" };
        options.host_policy.stdio_allowed = true;
        (void)set_host_stdio_streams(&options.host_policy, {"stdout"});
        capsid::host::OperationStatus status;
        const capsid::host::DeployOutcome deployed =
            capsid::host::managed_deploy(&options, "v1", &status);
        require(deployed.ok && deployed.worker != nullptr,
                "stdio deploy failed: " + deployed.error);
        require(run_request(deployed.worker) == "stdio-ok",
                "compiled stdio stream did not reach the worker");
        capsid_worker_destroy(deployed.worker);
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode == "host_managed_rejects_storage_namespace_over_host") {
        write_file(
            fixtures.vdir + "/capsid.json",
            R"json({"apiVersion":"capsid/app-v1","entry":"bundle.mjs","permissions":{"modules":["capsid:storage"],"storage":{"namespaces":["tenant-a"]}},"pool":{"minReady":1,"maxWorkers":1}})json");
        capsid::host::ManagedHostOptions options = make_options(fixtures);
        options.host_policy.module_allowlist = {"capsid:storage"};
        options.host_policy.storage_allowed = true;
        require(set_host_storage_namespaces(&options.host_policy,
                                            {"tenant-b"}),
                "HostPolicy cannot express exact storage namespaces");
        capsid::host::OperationStatus status;
        const capsid::host::DeployOutcome deployed =
            capsid::host::managed_deploy(&options, "v1", &status);
        if (deployed.worker != nullptr) {
            capsid_worker_destroy(deployed.worker);
        }
        require(!deployed.ok,
                "Host bool-only storage permission widened tenant-a");
        require(status.state == capsid::host::OperationState::kFailed,
                "storage namespace overreach did not fail deployment");
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode == "host_managed_rejects_stdio_stream_over_host") {
        write_file(
            fixtures.vdir + "/capsid.json",
            R"json({"apiVersion":"capsid/app-v1","entry":"bundle.mjs","permissions":{"modules":["capsid:stdio"],"stdio":["stderr"]},"pool":{"minReady":1,"maxWorkers":1}})json");
        capsid::host::ManagedHostOptions options = make_options(fixtures);
        options.host_policy.module_allowlist = {"capsid:stdio"};
        options.host_policy.stdio_allowed = true;
        require(set_host_stdio_streams(&options.host_policy, {"stdout"}),
                "HostPolicy cannot express exact stdio streams");
        capsid::host::OperationStatus status;
        const capsid::host::DeployOutcome deployed =
            capsid::host::managed_deploy(&options, "v1", &status);
        if (deployed.worker != nullptr) {
            capsid_worker_destroy(deployed.worker);
        }
        require(!deployed.ok,
                "Host bool-only stdio permission widened stderr");
        require(status.state == capsid::host::OperationState::kFailed,
                "stdio stream overreach did not fail deployment");
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode == "host_managed_resource_fields_affect_identity") {
        const std::string config_v1 =
            R"json({"apiVersion":"capsid/app-v1","entry":"bundle.mjs","worker":{"jsHeap":"64MiB","processAddressSpace":"256MiB","memoryMax":"256MiB","fileDescriptors":64},"pool":{"minReady":1,"maxWorkers":1}})json";
        write_file(fixtures.vdir + "/capsid.json", config_v1);
        capsid::host::ManagedHostOptions options = make_options(fixtures);
        const auto deploy_generation = [&](const std::string& version) {
            capsid::host::OperationStatus status;
            const capsid::host::DeployOutcome deployed =
                capsid::host::managed_deploy(&options, version, &status);
            require(deployed.ok && deployed.worker != nullptr,
                    "resource identity deploy failed: " + deployed.error);
            capsid_worker_destroy(deployed.worker);
            return json_string_field(
                read_file(fixtures.state_root + "/apps/orders/active.json"),
                "generation");
        };
        const std::string first_generation = deploy_generation("v1");
        create_version(
            fixtures, "v2",
            R"json({"apiVersion":"capsid/app-v1","entry":"bundle.mjs","worker":{"jsHeap":"32MiB","processAddressSpace":"256MiB","memoryMax":"256MiB","fileDescriptors":64},"pool":{"minReady":1,"maxWorkers":1}})json",
            kSourceBundle);
        const std::string heap_generation = deploy_generation("v2");
        require(heap_generation != first_generation,
                "worker.jsHeap was omitted from generation identity");
        create_version(
            fixtures, "v3",
            R"json({"apiVersion":"capsid/app-v1","entry":"bundle.mjs","worker":{"jsHeap":"32MiB","processAddressSpace":"128MiB","memoryMax":"256MiB","fileDescriptors":64},"pool":{"minReady":1,"maxWorkers":1}})json",
            kSourceBundle);
        const std::string address_generation = deploy_generation("v3");
        require(address_generation != heap_generation,
                "worker.processAddressSpace was omitted from generation identity");
        create_version(
            fixtures, "v4",
            R"json({"apiVersion":"capsid/app-v1","entry":"bundle.mjs","worker":{"jsHeap":"32MiB","processAddressSpace":"128MiB","memoryMax":"256MiB","fileDescriptors":32},"pool":{"minReady":1,"maxWorkers":1}})json",
            kSourceBundle);
        const std::string descriptor_generation = deploy_generation("v4");
        require(descriptor_generation != address_generation,
                "worker.fileDescriptors was omitted from generation identity");
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode == "host_managed_resource_limits_reach_worker") {
        // The Runtime rejects processAddressSpace < jsHeap. If the managed
        // compiler silently ignores those fields (or maps memoryMax onto the
        // JS heap), this invalid effective worker configuration starts.
        write_file(
            fixtures.vdir + "/capsid.json",
            R"json({"apiVersion":"capsid/app-v1","entry":"bundle.mjs","worker":{"jsHeap":"128MiB","processAddressSpace":"64MiB","memoryMax":"256MiB"},"pool":{"minReady":1,"maxWorkers":1}})json");
        capsid::host::ManagedHostOptions options = make_options(fixtures);
        capsid::host::OperationStatus status;
        const capsid::host::DeployOutcome deployed =
            capsid::host::managed_deploy(&options, "v1", &status);
        if (deployed.worker != nullptr) {
            capsid_worker_destroy(deployed.worker);
        }
        require(!deployed.ok,
                "managed Host ignored distinct JS/address-space limits");
        require(status.state == capsid::host::OperationState::kFailed,
                "invalid worker resource relation did not fail deployment");
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode ==
        "host_managed_process_address_space_reaches_release_runtime") {
#if defined(__SANITIZE_ADDRESS__) || CAPSID_TEST_TSAN_BUILD
        // A finite RLIMIT_AS is incompatible with ASan's shadow-memory
        // reservation. The production Release path remains required to
        // forward this field; its sanitizer build is covered by the
        // compiler/identity tests instead of imposing RLIMIT_AS.
        std::cout << "PASS" << std::endl;
        return 0;
#else
        write_file(
            fixtures.vdir + "/capsid.json",
            R"json({"apiVersion":"capsid/app-v1","entry":"bundle.mjs","worker":{"processAddressSpace":"1MiB"},"pool":{"minReady":1,"maxWorkers":1}})json");
        capsid::host::ManagedHostOptions options = make_options(fixtures);
        capsid::host::OperationStatus status;
        const capsid::host::DeployOutcome deployed =
            capsid::host::managed_deploy(&options, "v1", &status);
        if (deployed.worker != nullptr) {
            capsid_worker_destroy(deployed.worker);
        }
        require(!deployed.ok,
                "Release managed Host ignored processAddressSpace");
        require(status.state == capsid::host::OperationState::kFailed,
                "unenforceable processAddressSpace did not fail deployment");
        std::cout << "PASS" << std::endl;
        return 0;
#endif
    }

    if (mode ==
        "host_managed_process_address_space_skipped_under_tsan") {
#if CAPSID_TEST_TSAN_BUILD
        // TSan, like ASan, reserves a vast shadow-memory address range. A
        // finite RLIMIT_AS prevents an instrumented worker from reaching
        // READY, so the managed Host must keep compile-time relation checks
        // but skip forwarding processAddressSpace to the TSan worker.
        write_file(
            fixtures.vdir + "/capsid.json",
            R"json({"apiVersion":"capsid/app-v1","entry":"bundle.mjs","worker":{"processAddressSpace":"1MiB"},"pool":{"minReady":1,"maxWorkers":1}})json");
        capsid::host::ManagedHostOptions options = make_options(fixtures);
        capsid::host::OperationStatus status;
        const capsid::host::DeployOutcome deployed =
            capsid::host::managed_deploy(&options, "v1", &status);
        require(deployed.ok && deployed.worker != nullptr,
                "TSan managed Host forwarded an incompatible RLIMIT_AS: " +
                    deployed.error);
        capsid_worker_destroy(deployed.worker);
#endif
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode == "host_managed_failed_deploy_cleans_staging") {
        std::string escape = "/tmp/capsid-managed-cleanup-escape-XXXXXX";
        require(mkdtemp(&escape[0]) != nullptr,
                "cannot create failed-cleanup escape fixture");
        const std::string apps = fixtures.state_root + "/apps";
        const std::string app = apps + "/orders";
        require(mkdir(apps.c_str(), 0700) == 0,
                "cannot create state apps fixture");
        require(mkdir(app.c_str(), 0700) == 0,
                "cannot create state App fixture");
        require(symlink(escape.c_str(), (app + "/generations").c_str()) == 0,
                "cannot create generations failure fixture");
        capsid::host::ManagedHostOptions options = make_options(fixtures);
        capsid::host::OperationStatus status;
        const capsid::host::DeployOutcome deployed =
            capsid::host::managed_deploy(&options, "v1", &status);
        if (deployed.worker != nullptr) {
            capsid_worker_destroy(deployed.worker);
        }
        require(!deployed.ok,
                "generations symlink did not trigger staging failure");
        require(!directory_has_entries(fixtures.state_root + "/staging"),
                "failed deploy leaked its non-empty staging tree");
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode == "host_managed_staging_mode_rejected") {
        const std::string staging = fixtures.state_root + "/staging";
        require(mkdir(staging.c_str(), 0700) == 0,
                "cannot create staging mode fixture");
        require(chmod(staging.c_str(), 0777) == 0,
                "cannot make staging mode unsafe");
        capsid::host::ManagedHostOptions options = make_options(fixtures);
        capsid::host::OperationStatus status;
        const capsid::host::DeployOutcome deployed =
            capsid::host::managed_deploy(&options, "v1", &status);
        if (deployed.worker != nullptr) {
            capsid_worker_destroy(deployed.worker);
        }
        require(!deployed.ok,
                "managed deploy accepted group/other-writable staging");
        require(status.state == capsid::host::OperationState::kFailed,
                "unsafe staging mode did not fail deployment");
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode == "host_managed_generations_mode_rejected") {
        const std::string apps = fixtures.state_root + "/apps";
        const std::string app = apps + "/orders";
        const std::string generations = app + "/generations";
        require(mkdir(apps.c_str(), 0700) == 0,
                "cannot create state apps fixture");
        require(mkdir(app.c_str(), 0700) == 0,
                "cannot create state App fixture");
        require(mkdir(generations.c_str(), 0700) == 0,
                "cannot create generations mode fixture");
        require(chmod(generations.c_str(), 0777) == 0,
                "cannot make generations mode unsafe");
        capsid::host::ManagedHostOptions options = make_options(fixtures);
        capsid::host::OperationStatus status;
        const capsid::host::DeployOutcome deployed =
            capsid::host::managed_deploy(&options, "v1", &status);
        if (deployed.worker != nullptr) {
            capsid_worker_destroy(deployed.worker);
        }
        require(!deployed.ok,
                "managed deploy accepted group/other-writable generations");
        require(status.state == capsid::host::OperationState::kFailed,
                "unsafe generations mode did not fail deployment");
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode == "host_managed_rejects_corrupt_version_mapping") {
        capsid::host::ManagedHostOptions options = make_options(fixtures);
        capsid::host::OperationStatus deploy_status;
        const capsid::host::DeployOutcome deployed =
            capsid::host::managed_deploy(&options, "v1", &deploy_status);
        require(deployed.ok && deployed.worker != nullptr,
                "initial deploy failed: " + deployed.error);
        capsid_worker_destroy(deployed.worker);
        const std::string mapping = fixtures.state_root +
            "/apps/orders/versions/v1.json";
        const std::string generation = json_string_field(read_file(mapping),
                                                         "generation");
        write_file(mapping,
                   "{\"generation\":\"" + generation +
                       "\",\"mystery\":true}");

        capsid::host::OperationStatus repeat_status;
        const capsid::host::DeployOutcome repeated =
            capsid::host::managed_deploy(&options, "v1", &repeat_status);
        if (repeated.worker != nullptr) {
            capsid_worker_destroy(repeated.worker);
        }
        require(!repeated.ok,
                "managed deploy accepted a corrupt Version mapping");
        require(repeat_status.state == capsid::host::OperationState::kFailed,
                "corrupt Version mapping did not fail the operation");
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode == "host_managed_failed_operation_status") {
        capsid::host::ManagedHostOptions options = make_options(fixtures);
        capsid::host::OperationStatus deploy_status;
        const capsid::host::DeployOutcome failed =
            capsid::host::managed_deploy(&options, "missing", &deploy_status);
        require(!failed.ok && !failed.operation_id.empty(),
                "failed deploy did not return an operation id");
        require(deploy_status.state == capsid::host::OperationState::kFailed,
                "failed deploy did not enter Failed state");

        const capsid::host::OperationStatus queried =
            capsid::host::managed_operation_status(options,
                                                   failed.operation_id);
        require(queried.state == capsid::host::OperationState::kFailed,
                "failed operation query returned the wrong state");
        require(queried.error == deploy_status.error,
                "failed operation was not retained for status queries");
        std::cout << "PASS" << std::endl;
        return 0;
    }

    fail("unknown mode: " + mode);
}
