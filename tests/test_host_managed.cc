// Frozen RED suite: the M1D managed host (modes registered as the frozen
// test names). Drives the real deploy pipeline: safe-read, attestation
// selection, policy/secret compilation, staging, worker warm-up with the
// real capsid-worker, active.json persistence, retire and recovery.

#include "host/managed_host.h"
#include "host/managed_admin_backend.h"
#include "host/bytecode_attestation.h"
#include "host/generation_pool.h"
#include "host/secret_snapshot.h"
#include "host/worker_executor.h"

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
// TSan slows worker startup and host boot by an order of magnitude. The
// hosted tsan matrix keeps the fixed deadlines scaled so the tests verify
// the mechanism, not the clock (the scaling is compile-time: release,
// asan and ubsan builds keep the original deadlines).
#if defined(__SANITIZE_THREAD__) || \
    (defined(__has_feature) && __has_feature(thread_sanitizer))
constexpr int kTestWaitScale = 8;
#else
constexpr int kTestWaitScale = 1;
#endif


#include <chrono>
#include <barrier>
#include <cerrno>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
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

// PR-10 §9.3: the activation/retire transactions live on the coordinator
// options (ManagedHostOptions prepare/commit/abort), never on the Async
// layer. This harness mirrors main.cc's wiring for tests without a data
// plane: the "route view" is a single observed pool, commit adopts it
// (drain signal on the old generation), retire drains it.

// Fast, deterministic recovery: 20ms initial backoff, no jitter, budget of
// two unexpected exits before quarantine, no stability reset during a test.
capsid::host::WorkerRecoveryPolicy generation_recovery_policy() {
    capsid::host::WorkerRecoveryPolicy policy;
    policy.max_events = 2;
    policy.window_ms = 60000;
    policy.backoff_initial_ms = 20;
    policy.backoff_maximum_ms = 1000;
    policy.jitter_basis_points = 0;
    policy.stable_reset_ms = 60000;
    policy.replacements_concurrent_per_app = 1;
    return policy;
}

struct TransactionHarness {
    std::mutex mutex;
    std::condition_variable condition;
    std::shared_ptr<capsid::host::GenerationPool> active_pool;
    bool activated = false;
    bool retired = false;
};

void wire_admin_transactions(capsid::host::ManagedHostOptions* options,
                             TransactionHarness* harness) {
    options->prepare_activation =
        [](const std::string& application,
           const capsid::host::DeployOutcome& prepared,
           std::string* error)
        -> std::unique_ptr<capsid::host::ActivationPlan> {
            auto plan = std::make_unique<capsid::host::ActivationPlan>();
            plan->application = application;
            plan->version = prepared.version;
            plan->generation_digest = prepared.generation_digest;
            plan->new_workers = prepared.workers.size();
            capsid::host::GenerationPoolOptions pool_options;
            pool_options.application_id = application;
            pool_options.version = prepared.version;
            pool_options.generation_digest = prepared.generation_digest;
            pool_options.workers =
                static_cast<std::uint32_t>(prepared.workers.size());
            pool_options.factory = prepared.generation_factory;
            // Fast deterministic recovery policy: create_adopted rejects a
            // zeroed policy, and this harness mirrors main.cc's wiring.
            pool_options.recovery = generation_recovery_policy();
            std::string pool_error;
            std::shared_ptr<capsid::host::GenerationPool> pool =
                capsid::host::GenerationPool::create_adopted(
                    std::move(pool_options), prepared.workers, &pool_error);
            if (pool == nullptr) {
                *error = "cannot adopt the warmed pool";
                return nullptr;
            }
            plan->new_pool = pool;
            return plan;
        };
    options->commit_activation =
        [harness](capsid::host::ActivationPlan* plan) {
            std::lock_guard<std::mutex> lock(harness->mutex);
            if (plan->old_pool != nullptr) {
                plan->old_pool->request_drain();
            }
            harness->active_pool = plan->new_pool;
            harness->activated = true;
            harness->condition.notify_all();
        };
    options->abort_activation = [](capsid::host::ActivationPlan* plan) {
        // the never-published pool dies with the plan
        (void) plan;
    };
    options->prepare_retire =
        [harness](const std::string& application, std::string* error)
        -> std::unique_ptr<capsid::host::RetirePlan> {
            (void) error;
            auto plan = std::make_unique<capsid::host::RetirePlan>();
            plan->application = application;
            {
                std::lock_guard<std::mutex> lock(harness->mutex);
                plan->pool = harness->active_pool;
                plan->workers = plan->pool != nullptr
                                    ? plan->pool->configured_workers()
                                    : 0;
            }
            return plan;
        };
    options->commit_retire =
        [harness](capsid::host::RetirePlan* plan) {
            std::lock_guard<std::mutex> lock(harness->mutex);
            if (plan->pool != nullptr) {
                plan->pool->request_drain();
            }
            harness->active_pool.reset();
            harness->retired = true;
            harness->condition.notify_all();
        };
    options->abort_retire = [](capsid::host::RetirePlan* plan) {
        (void) plan;
    };
}

capsid::host::OperationStatus wait_admin_operation(
    capsid::host::AsyncAdminBackend* backend,
    const std::string& operation_id) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(20 * kTestWaitScale);
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
        std::chrono::steady_clock::now() + std::chrono::seconds(10 * kTestWaitScale);
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

// Polls `predicate` until it holds or the deadline expires; returns whether
// it held. Never sleeps longer than 10ms per poll (same idiom as the
// generation pool suite).
bool wait_for(const std::function<bool()>& predicate,
              std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return predicate();
}

// Submits a bodyless begin plus an upfront credit grant and waits until the
// pool's inflight returns to 0 — the response completed end-to-end. The
// pump owns the event drain, so the inflight counter is the only observable
// completion signal at this layer.
void generation_round_trip(capsid::host::GenerationPool& pool,
                           std::uint64_t request_id) {
    capsid::host::WorkerExecutor* worker = pool.pick_worker();
    require(worker != nullptr,
            "generation round trip: no READY worker to serve the request");
    capsid::host::Command begin;
    begin.type = capsid::host::CommandType::kBeginRequest;
    begin.request_id = request_id;
    begin.method = "GET";
    begin.url = "https://pool.invalid/generation";
    begin.end_request = true;  // bodyless fusion: begin + end in one frame
    worker->submit(std::move(begin));
    capsid::host::Command grant;
    grant.type = capsid::host::CommandType::kGrantResponseCredit;
    grant.request_id = request_id;
    grant.credit = 4096;  // covers the whole response body upfront
    worker->submit(std::move(grant));
    require(wait_for([&] { return pool.inflight() == 0; },
                     std::chrono::seconds(15 * kTestWaitScale)),
            "generation round trip did not complete within 15s");
}

#if defined(__linux__)
// The pool spawns the workers as OUR direct children; scan /proc for the
// children of this test process (same idiom as the generation pool suite).
void generation_kill_worker_child() {
    const pid_t self = getpid();
    DIR* directory = opendir("/proc");
    require(directory != nullptr, "cannot open /proc to find the worker");
    pid_t found = -1;
    struct dirent* entry = nullptr;
    while ((entry = readdir(directory)) != nullptr) {
        if (entry->d_name[0] < '0' || entry->d_name[0] > '9') {
            continue;
        }
        char path[320];  // NAME_MAX (255) + "/proc/" + "/stat" + NUL
        std::snprintf(path, sizeof(path), "/proc/%s/stat", entry->d_name);
        FILE* file = std::fopen(path, "r");
        if (file == nullptr) {
            continue;
        }
        char comm[256];
        long ppid = -1;
        // Format: pid (comm) state ppid ... — see the generation pool suite
        // for the scan-format notes.
        const int scanned =
            std::fscanf(file, "%*d %255[^)]%*c %*c %ld", comm, &ppid);
        std::fclose(file);
        if (scanned == 2 && ppid == static_cast<long>(self)) {
            found = static_cast<pid_t>(std::strtol(entry->d_name, nullptr, 10));
            break;
        }
    }
    closedir(directory);
    require(found > 0, "cannot find the pool's worker child process");
    require(kill(found, SIGKILL) == 0, "cannot SIGKILL the pool worker");
}
#endif  // __linux__

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

// Generalized compile+sign: the caller names the version directory and the
// key id, so a rotated or re-keyed deployment can be built in-place with a
// second key (compile_and_sign is the v1/test-key-1 specialization).
void compile_and_sign_version(const Fixtures& f, const std::string& version,
                              const std::string& key_id, const TestKey& key,
                              std::string* attestation,
                              std::vector<std::uint8_t>* signature) {
    const std::string directory = f.apps_root + "/orders/" + version;
    const std::string source_file = directory + "/compile-source.mjs";
    write_file(source_file, kSourceBundle);
    const std::string bytecode_out = directory + "/bundle.qjsb";
    const std::string attestation_out = directory + "/bytecode.json";
    const std::string message_out = directory + "/message.bin";
    run_command({
        f.compiler_tool,
        "--source", source_file,
        "--source-name", ("file://orders/" + version + "/bundle.qjsb"),
        "--application", "orders",
        "--version", version,
        "--key-id", key_id,
        "--bytecode-out", bytecode_out,
        "--attestation-out", attestation_out,
        "--signing-message-out", message_out,
    });
    const std::string message = read_file(message_out);
    const std::vector<std::uint8_t> message_bytes(message.begin(), message.end());
    *signature = sign(key, message_bytes);
    write_file(directory + "/bytecode.sig",
               std::string(signature->begin(), signature->end()));
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

// M2 item 7: an optional JSON string field must be distinguished from a
// missing one (fallback reason is only written when a fallback happened).
bool json_has_string_field(const std::string& json,
                           const std::string& field) {
    const std::string needle = "\"" + field + "\":\"";
    return json.find(needle) != std::string::npos;
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

    // PR-09c (§9.3): the deploy outcome carries the generation's §8.3
    // replacement factory + identity; a data-plane pool adopted over the
    // warmed fleet respawns a SIGKILLed worker through that factory (same
    // artifact, same effective config) and serves again. RED gate: the
    // outcome fields do not exist on the PR-09b tree.
    if (mode == "host_managed_generation_factory_replacement") {
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
        // PR-09c additive contract: the generation identity travels with
        // the outcome so a data-plane pool can be adopted in place.
        require(static_cast<bool>(deployed.generation_factory),
                "deploy outcome lost the generation replacement factory");
        require(deployed.version == "v1",
                "deploy outcome lost the generation version");
        require(deployed.generation_digest.rfind("sha256:", 0) == 0 &&
                    deployed.generation_digest.size() == 71,
                "deploy outcome lost the generation digest");

        capsid::host::GenerationPoolOptions pool_options;
        pool_options.application_id = "orders";
        pool_options.version = deployed.version;
        pool_options.generation_digest = deployed.generation_digest;
        pool_options.workers =
            static_cast<std::uint32_t>(deployed_workers.size());
        pool_options.factory = deployed.generation_factory;
        pool_options.recovery = generation_recovery_policy();
        std::string pool_error;
        std::shared_ptr<capsid::host::GenerationPool> pool =
            capsid::host::GenerationPool::create_adopted(
                std::move(pool_options), deployed_workers, &pool_error);
        require(pool != nullptr, "cannot adopt the warmed pool: " + pool_error);
        require(pool->ready_workers() == 3,
                "adopted pool is not fully READY");

        generation_round_trip(*pool, 1001);

#if defined(__linux__)
        // Kill one worker child: the pool must respawn it through the
        // GENERATION factory (the deploy's own artifact/config) and return
        // to full READY capacity.
        generation_kill_worker_child();
        require(wait_for([&] { return pool->ready_workers() == 3; },
                         std::chrono::seconds(20 * kTestWaitScale)),
                "replacement through the generation factory did not restore "
                "full READY capacity");
        generation_round_trip(*pool, 1002);  // the replacement serves
#endif  // __linux__

        pool->stop_and_join();
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
        // §9.3: the ownership handoff runs INSIDE the coordinator's
        // transaction (prepare/commit/abort on the options), not on the
        // Async layer. The harness observes the committed pool.
        TransactionHarness harness;
        wire_admin_transactions(&options, &harness);

        capsid::host::AsyncAdminBackendOptions async_options;
        async_options.max_pending_operations = 2;
        capsid::host::AsyncAdminBackend backend(&managed, async_options);

        capsid::host::OperationStatus submitted;
        const capsid::host::DeployOutcome deployment =
            backend.deploy("orders", "v1", &submitted);
        require(deployment.ok, "fixed pool Admin deploy was not accepted");
        const capsid::host::OperationStatus active =
            wait_admin_operation(&backend, deployment.operation_id);
        require(active.state == capsid::host::OperationState::kActive,
                "fixed pool Admin deploy did not activate");
        std::shared_ptr<capsid::host::GenerationPool> committed;
        {
            std::unique_lock<std::mutex> lock(harness.mutex);
            require(harness.condition.wait_for(
                        lock, std::chrono::seconds(2 * kTestWaitScale),
                        [&]() { return harness.activated; }),
                    "Admin deploy did not commit the whole pool");
            committed = harness.active_pool;
        }
        require(committed != nullptr, "Admin deploy committed no pool");
        require(committed->configured_workers() == 3 &&
                    committed->ready_workers() == 3 &&
                    committed->state() ==
                        capsid::host::GenerationPool::State::kActive,
                "committed pool is not a live 3-worker fleet");
        require(committed->application_id() == "orders" &&
                    committed->version() == "v1",
                "committed pool lost the generation identity");

        capsid::host::OperationStatus retire_submitted;
        const capsid::host::DeployOutcome retired =
            backend.retire("orders", &retire_submitted);
        require(retired.ok, "fixed pool Admin retire was not accepted");
        const capsid::host::OperationStatus retired_status =
            wait_admin_operation(&backend, retired.operation_id);
        require(retired_status.state == capsid::host::OperationState::kActive,
                "fixed pool Admin retire did not settle");
        {
            std::unique_lock<std::mutex> lock(harness.mutex);
            require(harness.condition.wait_for(
                        lock, std::chrono::seconds(2 * kTestWaitScale),
                        [&]() { return harness.retired; }),
                    "Admin retire did not run the retire transaction");
        }
        std::string drain_error;
        require(committed->wait(&drain_error),
                "Admin retire did not drain the full pool");
        require(committed->state() ==
                    capsid::host::GenerationPool::State::kDead,
                "retired pool did not reach kDead");
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode == "host_managed_admin_worker_lifecycle") {
        capsid::host::ManagedHostOptions options = make_options(fixtures);
        std::vector<capsid::host::ManagedHostOptions*> applications = {
            &options,
        };
        capsid::host::ManagedAdminBackend managed(applications);
        // §9.3: the single-worker lifecycle is the same transaction — the
        // pool is a one-worker fleet, commit adopts it, retire drains it.
        TransactionHarness harness;
        wire_admin_transactions(&options, &harness);

        capsid::host::AsyncAdminBackendOptions async_options;
        async_options.max_pending_operations = 2;
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
        std::shared_ptr<capsid::host::GenerationPool> committed;
        {
            std::unique_lock<std::mutex> lock(harness.mutex);
            require(harness.condition.wait_for(
                        lock, std::chrono::seconds(2 * kTestWaitScale),
                        [&]() { return harness.activated; }),
                    "active Admin worker was discarded instead of adopted");
            committed = harness.active_pool;
        }
        require(committed != nullptr &&
                    committed->configured_workers() == 1 &&
                    committed->ready_workers() == 1 &&
                    committed->state() ==
                        capsid::host::GenerationPool::State::kActive,
                "Admin-owned active worker is not a live one-worker fleet");

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
            std::unique_lock<std::mutex> lock(harness.mutex);
            require(harness.condition.wait_for(
                        lock, std::chrono::seconds(2 * kTestWaitScale),
                        [&]() { return harness.retired; }),
                    "retire did not run the retire transaction");
        }
        std::string drain_error;
        require(committed->wait(&drain_error),
                "retire did not drain the active Admin worker");
        require(committed->state() ==
                    capsid::host::GenerationPool::State::kDead,
                "retired Admin worker did not reach kDead");
        std::cout << "PASS" << std::endl;
        return 0;
    }

    // PR-10 §9.3: a coordinator WITHOUT the transaction callbacks is the
    // legacy path — it hands the owning pool back through the outcome. The
    // Async layer has no publisher, so it fails the public operation closed
    // and recycles every worker: an unclaimed pool must never leak.
    if (mode == "host_managed_admin_legacy_unclaimed_pool") {
        write_file(fixtures.vdir + "/capsid.json", kThreeWorkerAppConfig);
        const std::string pid_log = fixtures.vdir + "/legacy-pids.log";
        std::ostringstream wrapper;
        wrapper << "#!/bin/sh\n"
                << "printf '%s\\n' \"$$\" >> '" << pid_log << "'\n"
                << "exec '" << fixtures.worker_path << "' \"$@\"\n";
        const std::string wrapper_path = fixtures.vdir + "/legacy-worker";
        write_file(wrapper_path, wrapper.str());
        require(chmod(wrapper_path.c_str(), 0700) == 0,
                "cannot make legacy worker wrapper executable");
        capsid::host::ManagedHostOptions options = make_options(fixtures);
        options.host_policy.max_workers = 3;
        options.worker_path = wrapper_path;
        // No transaction callbacks: the legacy direct-call path hands the
        // owning pool back through the deploy outcome.
        std::vector<capsid::host::ManagedHostOptions*> applications = {
            &options,
        };
        capsid::host::ManagedAdminBackend managed(applications);
        capsid::host::AsyncAdminBackendOptions async_options;
        capsid::host::AsyncAdminBackend backend(&managed, async_options);
        capsid::host::OperationStatus submitted;
        const capsid::host::DeployOutcome deployment =
            backend.deploy("orders", "v1", &submitted);
        require(deployment.ok, "legacy unclaimed deploy was not accepted");
        const capsid::host::OperationStatus terminal =
            wait_admin_operation(&backend, deployment.operation_id);
        require(terminal.state == capsid::host::OperationState::kFailed,
                "unclaimed pool did not fail closed at the Async layer");
        // Every spawned worker must have been recycled.
        std::istringstream pids(read_file(pid_log));
        std::int64_t pid = 0;
        std::size_t pid_count = 0;
        while (pids >> pid) {
            ++pid_count;
            errno = 0;
            require(kill(static_cast<pid_t>(pid), 0) == -1 && errno == ESRCH,
                    "unclaimed pool leaked a worker process");
        }
        require(pid_count == 3, "legacy pool did not warm all three workers");
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

    if (mode == "host_managed_fallback_records_reason") {
        // M2 item 7 (design §13:682): a compatibility fallback records the
        // verified attestation, the fallback reason AND the actual selected
        // artifact in artifact.json. The reason is written only when a
        // fallback actually happened: a trusted-bytecode deployment has no
        // reason field at all, so the committed document stays byte-stable
        // for the non-fallback path (restart_identity_stable).
        const TestKey key = test_key();
        std::vector<std::uint8_t> bytecode;
        std::string attestation;
        std::vector<std::uint8_t> signature;
        compile_and_sign(fixtures, key, &bytecode, &attestation, &signature);
        install_compatibility_mismatch(fixtures, key, attestation);
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
        capsid_worker_destroy(outcome.worker);
        const std::string generation = json_string_field(
            read_file(fixtures.state_root + "/apps/orders/active.json"),
            "generation");
        const std::string artifact = read_file(
            fixtures.state_root + "/apps/orders/generations/" + generation +
            "/artifact.json");
        require(json_string_field(artifact, "selected") == "source",
                "fallback artifact did not select source");
        require(json_string_field(artifact, "reason") ==
                    "compatibility-mismatch",
                "fallback artifact omits the fallback reason");
        require(json_string_field(artifact, "attestationDigest") != "",
                "fallback artifact omits the verified attestation");
        // Positive control: the non-fallback path never records a reason.
        Fixtures trusted_only =
            make_fixtures(worker_path, compiler_tool, fixture_source);
        write_file(trusted_only.vdir + "/capsid.json", kMinimalAppConfig);
        write_file(trusted_only.vdir + "/bundle.mjs", kSourceBundle);
        std::vector<std::uint8_t> trusted_bytecode;
        std::string trusted_attestation;
        std::vector<std::uint8_t> trusted_signature;
        compile_and_sign(trusted_only, key, &trusted_bytecode,
                         &trusted_attestation, &trusted_signature);
        capsid::host::ManagedHostOptions trusted_options =
            make_options(trusted_only);
        capsid::host::TrustedBytecodeKey trusted_key;
        trusted_key.key_id = "test-key-1";
        trusted_key.public_key = std::span<const std::uint8_t>(
            key.public_key.data(), key.public_key.size());
        trusted_options.trusted_keys.push_back(trusted_key);
        capsid::host::OperationStatus trusted_status;
        const capsid::host::DeployOutcome trusted_outcome =
            capsid::host::managed_deploy(&trusted_options, "v1",
                                         &trusted_status);
        require(trusted_outcome.ok && trusted_outcome.worker != nullptr,
                "trusted control deploy failed: " + trusted_outcome.error);
        capsid_worker_destroy(trusted_outcome.worker);
        const std::string trusted_generation = json_string_field(
            read_file(trusted_only.state_root + "/apps/orders/active.json"),
            "generation");
        const std::string trusted_artifact = read_file(
            trusted_only.state_root + "/apps/orders/generations/" +
            trusted_generation + "/artifact.json");
        require(json_string_field(trusted_artifact, "selected") ==
                    "trusted-bytecode",
                "trusted control deploy did not select trusted bytecode");
        require(!json_has_string_field(trusted_artifact, "reason"),
                "trusted deployment unexpectedly records a fallback reason");
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode == "host_managed_bytecode_key_rotation_deploys") {
        // M2 item 4 row 1. A key rotation replaces the trusted key: the old
        // key leaves trusted_keys, the new key enters it, and the next
        // deployment — signed with the new key — verifies and deploys as
        // trusted bytecode. Rotation rides a new Version ID (the frozen
        // version contract rejects same-Version-ID republishes).
        const TestKey first_key = test_key();
        std::vector<std::uint8_t> bytecode;
        std::string attestation;
        std::vector<std::uint8_t> signature;
        compile_and_sign(fixtures, first_key, &bytecode, &attestation,
                         &signature);
        capsid::host::ManagedHostOptions options = make_options(fixtures);
        capsid::host::TrustedBytecodeKey trusted;
        trusted.key_id = "test-key-1";
        trusted.public_key = std::span<const std::uint8_t>(
            first_key.public_key.data(), first_key.public_key.size());
        options.trusted_keys.push_back(trusted);
        capsid::host::OperationStatus status;
        const capsid::host::DeployOutcome first =
            capsid::host::managed_deploy(&options, "v1", &status);
        require(first.ok && first.worker != nullptr,
                "initial trusted deploy failed: " + first.error);
        capsid_worker_destroy(first.worker);
        const std::string generation_before = json_string_field(
            read_file(fixtures.state_root + "/apps/orders/active.json"),
            "generation");

        // Rotate: a NEW key id and key replace the trusted set.
        const TestKey rotated_key = test_key();
        create_version(fixtures, "v2", kMinimalAppConfig, kSourceBundle);
        compile_and_sign_version(fixtures, "v2", "test-key-2", rotated_key,
                                 &attestation, &signature);
        options.trusted_keys.clear();
        capsid::host::TrustedBytecodeKey rotated_trusted;
        rotated_trusted.key_id = "test-key-2";
        rotated_trusted.public_key = std::span<const std::uint8_t>(
            rotated_key.public_key.data(), rotated_key.public_key.size());
        options.trusted_keys.push_back(rotated_trusted);
        const capsid::host::DeployOutcome rotated =
            capsid::host::managed_deploy(&options, "v2", &status);
        require(rotated.ok && rotated.worker != nullptr,
                "rotated-key deploy failed: " + rotated.error);
        require(run_request(rotated.worker) == "managed-ok",
                "rotated-key worker did not serve trusted bytecode");
        capsid_worker_destroy(rotated.worker);
        const std::string generation_after = json_string_field(
            read_file(fixtures.state_root + "/apps/orders/active.json"),
            "generation");
        require(generation_after != generation_before,
                "key rotation did not change the generation digest");
        // The rotated deployment is trusted bytecode, not a silent source
        // fallback: the generation doc records the selection.
        const std::string rotated_artifact = read_file(
            fixtures.state_root + "/apps/orders/generations/" +
            generation_after + "/artifact.json");
        require(json_string_field(rotated_artifact, "selected") ==
                    "trusted-bytecode",
                "rotated-key deploy fell back to source");
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode == "host_managed_revoked_key_deploy_rejected") {
        // M2 item 4 row 2. A deployment signed by a key that is no longer
        // in trusted_keys must be REJECTED (kUnknownKey -> kReject), never
        // silently downgraded: the deploy fails and the previously active
        // generation stays active.
        const TestKey revoked_key = test_key();
        std::vector<std::uint8_t> bytecode;
        std::string attestation;
        std::vector<std::uint8_t> signature;
        compile_and_sign(fixtures, revoked_key, &bytecode, &attestation,
                         &signature);
        capsid::host::ManagedHostOptions options = make_options(fixtures);
        capsid::host::TrustedBytecodeKey trusted;
        trusted.key_id = "test-key-1";
        trusted.public_key = std::span<const std::uint8_t>(
            revoked_key.public_key.data(), revoked_key.public_key.size());
        options.trusted_keys.push_back(trusted);
        capsid::host::OperationStatus status;
        const capsid::host::DeployOutcome first =
            capsid::host::managed_deploy(&options, "v1", &status);
        require(first.ok && first.worker != nullptr,
                "initial trusted deploy failed: " + first.error);
        capsid_worker_destroy(first.worker);
        const std::string active_before = read_file(
            fixtures.state_root + "/apps/orders/active.json");

        // Revoke: the deployment key leaves trusted_keys. A later deploy
        // still signed with it must be rejected at the attestation gate.
        const TestKey replacement_key = test_key();
        create_version(fixtures, "v2", kMinimalAppConfig, kSourceBundle);
        compile_and_sign_version(fixtures, "v2", "test-key-1", revoked_key,
                                 &attestation, &signature);
        options.trusted_keys.clear();
        capsid::host::TrustedBytecodeKey replacement;
        replacement.key_id = "test-key-2";
        replacement.public_key = std::span<const std::uint8_t>(
            replacement_key.public_key.data(),
            replacement_key.public_key.size());
        options.trusted_keys.push_back(replacement);
        capsid::host::OperationStatus revoked_status;
        const capsid::host::DeployOutcome revoked =
            capsid::host::managed_deploy(&options, "v2", &revoked_status);
        require(!revoked.ok, "deploy signed by a revoked key was accepted");
        require(revoked.worker == nullptr,
                "revoked-key deploy returned a worker");
        require(revoked.error.find("bytecode attestation rejected") !=
                    std::string::npos,
                "revoked-key deploy failed outside the attestation gate: " +
                    revoked.error);
        require(revoked_status.state ==
                    capsid::host::OperationState::kFailed,
                "revoked-key deploy did not fail the operation");
        const std::string active_after = read_file(
            fixtures.state_root + "/apps/orders/active.json");
        require(active_after == active_before,
                "revoked-key rejection changed the old active");
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode == "host_managed_revoked_key_recovery_fail_closed") {
        // M2 item 4 row 3 (§13:210). Key revocation affects restart
        // recovery: a committed trusted generation is re-verified against
        // the CURRENT trusted_keys, so once the signing key is revoked the
        // generation must be REFUSED at recovery — fail-closed, never a
        // silent fallback to the source bundle.
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

        // Revoke the key, then restart. Recovery re-verifies the committed
        // attestation against the current trusted set: kUnknownKey must
        // REJECT the generation, not fall it back to source.
        const TestKey new_key = test_key();
        capsid::host::ManagedHostOptions restarted = make_options(fixtures);
        capsid::host::TrustedBytecodeKey new_trusted;
        new_trusted.key_id = "test-key-2";
        new_trusted.public_key = std::span<const std::uint8_t>(
            new_key.public_key.data(), new_key.public_key.size());
        restarted.trusted_keys.push_back(new_trusted);
        capsid::host::OperationStatus recover_status;
        const capsid::host::DeployOutcome recovered =
            capsid::host::managed_recover(&restarted, &recover_status);
        require(!recovered.ok,
                "recovery re-verified a revoked-key generation as trusted");
        require(recovered.worker == nullptr,
                "revoked-key recovery returned a worker");
        require(recover_status.state ==
                    capsid::host::OperationState::kFailed,
                "revoked-key recovery did not fail closed");
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode == "host_managed_restart_identity_stable") {
        // M2 item 4 row 4. Provenance is frozen into the committed
        // generation: the generation document binds attestationDigest and
        // the selected artifact kind. A restart that re-verifies with the
        // SAME trusted keys must recover the SAME generation identity —
        // same generation id, same attestation document, same trusted
        // selection — with no drift.
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
        const std::string generation_before = json_string_field(
            read_file(fixtures.state_root + "/apps/orders/active.json"),
            "generation");
        const std::string artifact_before = read_file(
            fixtures.state_root + "/apps/orders/generations/" +
            generation_before + "/artifact.json");
        require(json_string_field(artifact_before, "selected") ==
                    "trusted-bytecode",
                "baseline generation was not trusted bytecode");
        require(json_string_field(artifact_before, "attestationDigest") !=
                    "",
                "trusted generation omitted its attestation digest");

        capsid::host::OperationStatus recover_status;
        const capsid::host::DeployOutcome recovered =
            capsid::host::managed_recover(&options, &recover_status);
        require(recovered.ok && recovered.worker != nullptr,
                "trusted restart failed: " + recovered.error);
        require(run_request(recovered.worker) == "managed-ok",
                "trusted restart served the wrong bundle");
        capsid_worker_destroy(recovered.worker);
        const std::string generation_after = json_string_field(
            read_file(fixtures.state_root + "/apps/orders/active.json"),
            "generation");
        require(generation_after == generation_before,
                "restart changed the committed generation identity");
        const std::string artifact_after = read_file(
            fixtures.state_root + "/apps/orders/generations/" +
            generation_after + "/artifact.json");
        require(artifact_after == artifact_before,
                "restart drifted the generation's provenance document");
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

    if (mode == "host_managed_deploy_persist_failure_aborts") {
        // §9.6-2 through the §9.3 transaction: the new generation reaches
        // READY, the persist fails — abort recycles the new pool and the
        // old generation keeps serving with its active.json untouched.
        create_version(fixtures, "v2", kMinimalAppConfig, kSourceBundle);
        const std::string pid_log = fixtures.vdir + "/abort-pids.log";
        std::ostringstream wrapper;
        wrapper << "#!/bin/sh\n"
                << "printf '%s\\n' \"$$\" >> '" << pid_log << "'\n"
                << "exec '" << fixtures.worker_path << "' \"$@\"\n";
        const std::string wrapper_path = fixtures.vdir + "/abort-worker";
        write_file(wrapper_path, wrapper.str());
        require(chmod(wrapper_path.c_str(), 0700) == 0,
                "cannot make abort worker wrapper executable");
        capsid::host::ManagedHostOptions options = make_options(fixtures);
        options.worker_path = wrapper_path;
        TransactionHarness harness;
        wire_admin_transactions(&options, &harness);
        capsid::host::OperationStatus first_status;
        const capsid::host::DeployOutcome first =
            capsid::host::managed_deploy(&options, "v1", &first_status);
        require(first.ok, "initial v1 deploy failed");
        std::shared_ptr<capsid::host::GenerationPool> v1_pool;
        {
            std::unique_lock<std::mutex> lock(harness.mutex);
            require(harness.condition.wait_for(
                        lock, std::chrono::seconds(2 * kTestWaitScale),
                        [&]() { return harness.activated; }),
                    "v1 deploy did not commit its pool");
            v1_pool = harness.active_pool;
        }
        generation_round_trip(*v1_pool, 7001);
        // The persist's renameat(tmp, active.json) cannot overwrite a
        // directory: the write path stays exactly as far as a real storage
        // failure would take it (temp written, rename denied).
        require(remove((fixtures.state_root +
                        "/apps/orders/active.json").c_str()) == 0,
                "cannot remove active.json for the failure injection");
        require(mkdir((fixtures.state_root +
                       "/apps/orders/active.json").c_str(), 0700) == 0,
                "cannot replace active.json with a directory");
        capsid::host::OperationStatus fail_status;
        const capsid::host::DeployOutcome failed =
            capsid::host::managed_deploy(&options, "v2", &fail_status);
        require(!failed.ok, "deploy with a failing persist was accepted");
        require(failed.worker == nullptr && failed.workers.empty(),
                "aborted deploy returned a worker");
        require(fail_status.error.find("persist") != std::string::npos,
                "aborted deploy lost its persist diagnostic");
        // The v1 fleet survives the failed v2, exactly as §9.6-2 requires.
        generation_round_trip(*v1_pool, 7002);
        // The v2 worker warmed up (second spawn) but the abort must have
        // recycled it: every worker of the aborted pool is destroyed.
        std::vector<std::int64_t> pids;
        std::istringstream first_pids(read_file(pid_log));
        std::int64_t pid = 0;
        while (first_pids >> pid) {
            pids.push_back(pid);
        }
        require(pids.size() == 2, "aborted deploy did not spawn its own pool");
        errno = 0;
        require(kill(static_cast<pid_t>(pids[0]), 0) == 0,
                "v1 worker died with the aborted v2 deploy");
        // The pool's drain is a signal; give the worker a bounded moment
        // to exit, then require it is GONE (a live process after the drain
        // is a leak). A zombie (kill 0 succeeds on a reaped-pending pid)
        // counts as not-gone: it is still a process table entry.
        bool v2_gone = false;
        for (int attempt = 0; attempt < 100 && !v2_gone; ++attempt) {
            errno = 0;
            if (kill(static_cast<pid_t>(pids[1]), 0) == -1 && errno == ESRCH) {
                v2_gone = true;
            }
            if (!v2_gone) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        }
        require(v2_gone, "aborted v2 pool leaked a worker process");
        // Restore the state directory and prove the old active survives:
        // an idempotent v1 redeploy revalidates and re-persists it.
        require(rmdir((fixtures.state_root +
                       "/apps/orders/active.json").c_str()) == 0,
                "cannot restore the active state directory");
        capsid::host::OperationStatus replay_status;
        const capsid::host::DeployOutcome replay =
            capsid::host::managed_deploy(&options, "v1", &replay_status);
        require(replay.ok,
                "idempotent v1 redeploy failed after the aborted v2: " +
                    replay.error);
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

    if (mode == "host_managed_secret_rotation_generates_new_pool") {
        // M2 item 3 rows 1-2. A rotated secret file (new content → new
        // file-v1: dev/ino/size/ctime revision → new aggregate revision →
        // new generation digest) must drive a NEW worker pool on redeploy;
        // the pre-rotation worker keeps its env snapshot frozen at spawn
        // and stays healthy through the redeploy; a redeploy with an
        // unchanged secret is idempotent and reuses the committed
        // generation (same digest, no new pool).
        require(mkdirat(fixtures.secrets_fd, "orders", 0700) == 0,
                "cannot create secret app dir");
        const std::string secret_file =
            fixtures.secret_root + "/orders/api-token";
        write_file(secret_file, "secret-rotation-v1");
        const std::string bundle =
            "import { env } from 'capsid:env';\n"
            "export default { fetch: () => "
            "new Response(env.get('APP_TOKEN')) };";
        write_file(fixtures.vdir + "/capsid.json", kSecretAppConfig);
        write_file(fixtures.vdir + "/bundle.mjs", bundle);
        capsid::host::ManagedHostOptions options = make_options(fixtures);
        capsid::host::OperationStatus status;

        const capsid::host::DeployOutcome first =
            capsid::host::managed_deploy(&options, "v1", &status);
        require(first.ok && first.worker != nullptr,
                "initial secret deploy failed: " + first.error);
        require(run_request(first.worker) == "secret-rotation-v1",
                "initial snapshot missing from the first worker");
        const std::string generation_before = json_string_field(
            read_file(fixtures.state_root +
                      "/apps/orders/active.json"),
            "generation");

        // No rotation: the same version and secret resolve to the same
        // generation — idempotent redeploy reuses the committed snapshot.
        const capsid::host::DeployOutcome reused =
            capsid::host::managed_deploy(&options, "v1", &status);
        require(reused.ok && reused.worker != nullptr,
                "idempotent redeploy failed: " + reused.error);
        require(run_request(reused.worker) == "secret-rotation-v1",
                "idempotent redeploy lost the snapshot");
        const std::string generation_reused = json_string_field(
            read_file(fixtures.state_root +
                      "/apps/orders/active.json"),
            "generation");
        require(generation_reused == generation_before,
                "an unchanged secret produced a new generation");

        // Rotation: rewriting the file in place changes its size and ctime,
        // so the provider's file-v1: revision (and therefore the aggregate
        // revision and the generation digest) changes. Same-Version-ID
        // republish is rejected by the frozen version contract (asserted
        // by host_managed_version_immutability), so rotation flows through
        // a new Version ID carrying the same bundle.
        write_file(secret_file, "secret-rotation-v2-different-length");
        create_version(fixtures, "v2", kSecretAppConfig, bundle);
        const capsid::host::DeployOutcome rotated =
            capsid::host::managed_deploy(&options, "v2", &status);
        require(rotated.ok && rotated.worker != nullptr,
                "rotated redeploy failed: " + rotated.error);
        require(run_request(rotated.worker) ==
                    "secret-rotation-v2-different-length",
                "rotated value did not reach the new worker");
        const std::string generation_after = json_string_field(
            read_file(fixtures.state_root +
                      "/apps/orders/active.json"),
            "generation");
        require(generation_after != generation_before,
                "secret rotation did not change the generation digest");

        // The old worker keeps its frozen snapshot and stays healthy.
        require(run_request(first.worker) == "secret-rotation-v1",
                "old worker lost its pre-rotation env snapshot");

        // Row 5: the provider's file-v1: revision also changes when the
        // file is replaced outright — a NEW device/inode, not just a
        // rewritten ctime/size. That must drive another rotation.
        require(unlink(secret_file.c_str()) == 0,
                "cannot remove the secret file for replacement");
        write_file(secret_file, "secret-rotation-v3-replaced-inode");
        create_version(fixtures, "v3", kSecretAppConfig, bundle);
        const capsid::host::DeployOutcome replaced =
            capsid::host::managed_deploy(&options, "v3", &status);
        require(replaced.ok && replaced.worker != nullptr,
                "inode-replaced redeploy failed: " + replaced.error);
        require(run_request(replaced.worker) ==
                    "secret-rotation-v3-replaced-inode",
                "replaced value did not reach the new worker");
        const std::string generation_third = json_string_field(
            read_file(fixtures.state_root +
                      "/apps/orders/active.json"),
            "generation");
        require(generation_third != generation_after,
                "replaced secret file (new device/inode) did not change "
                "the generation digest");

        // Every prior worker is still healthy with its frozen snapshot.
        require(run_request(rotated.worker) ==
                    "secret-rotation-v2-different-length",
                "v2 worker lost its mid-rotation env snapshot");

        // All three generations stay committed: content-addressed
        // generations are immutable history, never cleaned up by redeploy.
        const std::string generations_root =
            fixtures.state_root + "/apps/orders/generations/";
        const std::string generation_ids[] = {
            generation_before, generation_after, generation_third,
        };
        for (const std::string& generation_id : generation_ids) {
            require(access((generations_root + generation_id + "/COMPLETE")
                               .c_str(),
                           F_OK) == 0,
                    "a committed generation was cleaned up by redeploy");
        }

        capsid_worker_destroy(first.worker);
        capsid_worker_destroy(reused.worker);
        capsid_worker_destroy(rotated.worker);
        capsid_worker_destroy(replaced.worker);
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
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
        // The deterministic Release probe constrains its child with
        // RLIMIT_AS so an unbounded reader cannot consume host memory.
        // ASan reserves a huge shadow range and is fundamentally
        // incompatible with that limit; TSan likewise fails its internal
        // allocator under the ceiling. The same production read helper is
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
