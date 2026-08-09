// capsid-host executable entry point.
//
// The frozen M1A CLI is strictly validated before anything is spawned:
// unknown or missing arguments fail before any side effect. Startup order is
// fixed by the design: validate arguments, read and load the source bundle,
// spawn the worker, wait for READY and verify the compatibility ID, bind the
// listener, and only then write one canonical JSON line to --ready-fd.
// stdout never carries readiness or logs; diagnostics go to stderr.

#include "host/single_worker_server.h"
#include "host/static_pool_server.h"

#include "build_identity.h"
#include "capsid/runtime.h"
#include "host/admin_service.h"
#include "host/config.h"
#include "host/host_config_model.h"
#include "host/managed_admin_backend.h"
#include "host/trusted_key_store.h"

#include <boost/asio/ip/address.hpp>
#include <boost/system/error_code.hpp>

#include <dirent.h>
#include <signal.h>
#include <sys/stat.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "host/active_state.h"
#include "host/request_normalization.h"

namespace {

constexpr std::string_view kProbeGeneration =
    "sha256:0000000000000000000000000000000000000000000000000000000000000000";

void fail(const std::string& message) {
    std::fprintf(stderr, "capsid-host: %s\n", message.c_str());
    std::exit(2);
}

bool valid_application_id(const std::string& application) {
    capsid::host::ActiveStateDocument probe;
    probe.state = capsid::host::ActiveServiceState::kActive;
    probe.application = application;
    probe.version = "v0";
    probe.generation = std::string(kProbeGeneration);
    return capsid::host::encode_active_state_json(probe).ok;
}

std::uint64_t parse_positive_integer(const std::string& value,
                                     const char* name) {
    if (value.empty()) {
        fail(std::string("--") + name + " requires a positive integer");
    }
    for (const char c : value) {
        if (c < '0' || c > '9') {
            fail(std::string("--") + name +
                 " requires a positive integer: " + value);
        }
    }
    errno = 0;
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(value.c_str(), &end, 10);
    if (errno != 0 || end == nullptr || *end != '\0' || parsed == 0 ||
        parsed > static_cast<unsigned long long>(
                      std::numeric_limits<std::int64_t>::max())) {
        fail(std::string("--") + name +
             " requires a positive integer: " + value);
    }
    return static_cast<std::uint64_t>(parsed);
}

// Non-negative integer: allows the explicit 0 that disables a feature
// (e.g. --queue-requests 0).
std::uint64_t parse_nonnegative_integer(const std::string& value,
                                        const char* name) {
    if (value.empty()) {
        fail(std::string("--") + name + " requires a non-negative integer");
    }
    for (const char c : value) {
        if (c < '0' || c > '9') {
            fail(std::string("--") + name +
                 " requires a non-negative integer: " + value);
        }
    }
    errno = 0;
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(value.c_str(), &end, 10);
    if (errno != 0 || end == nullptr || *end != '\0' ||
        parsed > static_cast<unsigned long long>(
                      std::numeric_limits<std::int64_t>::max())) {
        fail(std::string("--") + name +
             " requires a non-negative integer: " + value);
    }
    return static_cast<std::uint64_t>(parsed);
}

// "250ms" / "5s" / "1m" style duration (the same grammar the effective
// config uses for queueTimeout). Unit suffix is required.
std::uint64_t parse_duration_ms(const std::string& value,
                                const char* name) {
    std::string::size_type number_end = 0;
    for (std::string::size_type index = 0; index < value.size(); ++index) {
        const char c = value[index];
        if (c < '0' || c > '9') {
            break;
        }
        number_end = index + 1;
    }
    if (number_end == 0 || number_end == value.size()) {
        fail(std::string("--") + name +
             " requires a duration like 250ms or 5s: " + value);
    }
    const std::string number_text = value.substr(0, number_end);
    const std::string unit = value.substr(number_end);
    std::uint64_t multiplier = 0;
    if (unit == "ms") {
        multiplier = 1;
    } else if (unit == "s") {
        multiplier = 1000;
    } else if (unit == "m") {
        multiplier = 60U * 1000U;
    } else {
        fail(std::string("--") + name +
             " requires ms, s or m units: " + value);
    }
    errno = 0;
    char* end = nullptr;
    const unsigned long long parsed =
        std::strtoull(number_text.c_str(), &end, 10);
    if (errno != 0 || end == nullptr || *end != '\0' || parsed == 0 ||
        parsed > static_cast<unsigned long long>(
                      std::numeric_limits<std::uint64_t>::max() /
                      multiplier)) {
        fail(std::string("--") + name + " is out of range: " + value);
    }
    return static_cast<std::uint64_t>(parsed) * multiplier;
}

// host:port; only decimal ports and non-empty hosts are accepted.
void parse_listen(const std::string& value,
                  std::string* out_address,
                  std::uint16_t* out_port) {
    const std::size_t colon = value.rfind(':');
    if (colon == std::string::npos || colon == 0 ||
        colon + 1 == value.size()) {
        fail("--listen requires host:port");
    }
    *out_address = value.substr(0, colon);
    const std::string port_text = value.substr(colon + 1);
    for (const char c : port_text) {
        if (c < '0' || c > '9') {
            fail("--listen port must be decimal: " + value);
        }
    }
    errno = 0;
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(port_text.c_str(), &end, 10);
    if (errno != 0 || end == nullptr || *end != '\0' || parsed > 65535) {
        fail("--listen port must be in [0, 65535]: " + value);
    }
    *out_port = static_cast<std::uint16_t>(parsed);
}

std::vector<std::uint8_t> read_bundle(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        fail("cannot read --source-bundle: " + path);
    }
    input.seekg(0, std::ios::end);
    const std::streamoff size = input.tellg();
    if (size < 0) {
        fail("cannot size --source-bundle: " + path);
    }
    input.seekg(0, std::ios::beg);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    if (size > 0) {
        input.read(reinterpret_cast<char*>(bytes.data()),
                   static_cast<std::streamsize>(size));
        if (!input) {
            fail("cannot read --source-bundle: " + path);
        }
    }
    return bytes;
}

// ---- managed mode: host.json authority, real coordinator, Admin service ----

// Process-level stop signal. SIGTERM is blocked process-wide and waited
// for with sigwait on the main thread, so no C++ object is ever touched
// inside a signal handler.
std::atomic<bool> g_stop{false};

// "256MiB" style size with explicit suffix (same grammar as the managed
// coordinator's worker.memoryMax).
bool parse_size_bytes_text(const std::string& text, std::uint64_t* out) {
    if (text.empty()) {
        return false;
    }
    char* end = nullptr;
    errno = 0;
    const unsigned long long base = std::strtoull(text.c_str(), &end, 10);
    if (errno != 0 || end == text.c_str()) {
        return false;
    }
    std::uint64_t multiplier = 0;
    const std::string suffix(end);
    if (suffix == "KiB") {
        multiplier = 1024ULL;
    } else if (suffix == "MiB") {
        multiplier = 1024ULL * 1024ULL;
    } else if (suffix == "GiB") {
        multiplier = 1024ULL * 1024ULL * 1024ULL;
    } else if (suffix == "KB") {
        multiplier = 1000ULL;
    } else if (suffix == "MB") {
        multiplier = 1000ULL * 1000ULL;
    } else if (suffix == "GB") {
        multiplier = 1000ULL * 1000ULL * 1000ULL;
    } else {
        return false;
    }
    if (base > std::numeric_limits<std::uint64_t>::max() / multiplier) {
        return false;
    }
    *out = static_cast<std::uint64_t>(base) * multiplier;
    return true;
}

// Safe open of a Host-owned directory: O_NOFOLLOW, directory, euid owner,
// no group/other bits.
int open_verified_root(const std::string& path, const char* what) {
    const int fd = open(path.c_str(),
                        O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        fail(std::string("cannot open ") + what);
    }
    struct stat st = {};
    if (fstat(fd, &st) != 0 || !S_ISDIR(st.st_mode) ||
        st.st_uid != geteuid() || (st.st_mode & 0077) != 0) {
        close(fd);
        fail(std::string("unverified ") + what);
    }
    return fd;
}

// Valid App ID (frozen lowercase grammar).
bool valid_managed_app_id(const std::string& value) {
    if (value.empty() || value.size() > 63) {
        return false;
    }
    if (!((value[0] >= 'a' && value[0] <= 'z') ||
          (value[0] >= '0' && value[0] <= '9'))) {
        return false;
    }
    for (const char c : value) {
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
              c == '.' || c == '_' || c == '-')) {
            return false;
        }
    }
    return true;
}

// True when the App has a durable active-state document (an active
// generation or a retired tombstone). Recovery only needs a permit for
// those; an App with no state cannot consume capacity.
bool has_durable_active_state(int state_root_fd,
                              const std::string& application) {
    const int apps_fd = openat(state_root_fd, "apps",
                               O_RDONLY | O_DIRECTORY | O_CLOEXEC |
                                   O_NOFOLLOW);
    if (apps_fd < 0) {
        return false;
    }
    struct stat directory = {};
    if (fstat(apps_fd, &directory) != 0 || !S_ISDIR(directory.st_mode) ||
        directory.st_uid != geteuid() || (directory.st_mode & 0077) != 0) {
        close(apps_fd);
        return false;
    }
    const int app_fd = openat(apps_fd, application.c_str(),
                              O_RDONLY | O_DIRECTORY | O_CLOEXEC |
                                  O_NOFOLLOW);
    close(apps_fd);
    if (app_fd < 0) {
        return false;
    }
    if (fstat(app_fd, &directory) != 0 || !S_ISDIR(directory.st_mode) ||
        directory.st_uid != geteuid() || (directory.st_mode & 0077) != 0) {
        close(app_fd);
        return false;
    }
    const int active_fd = openat(app_fd, "active.json",
                                 O_RDONLY | O_CLOEXEC | O_NOFOLLOW |
                                     O_NONBLOCK);
    close(app_fd);
    if (active_fd < 0) {
        return false;
    }
    struct stat active = {};
    const bool durable = fstat(active_fd, &active) == 0 &&
                         S_ISREG(active.st_mode) &&
                         active.st_uid == geteuid();
    close(active_fd);
    return durable;
}

// Discovers configured Apps beneath the verified applications root.
std::vector<std::string> discover_applications(int apps_fd) {
    std::vector<std::string> applications;
    DIR* dir = fdopendir(dup(apps_fd));
    if (dir == nullptr) {
        fail("cannot enumerate applications root");
    }
    for (;;) {
        errno = 0;
        struct dirent* entry = readdir(dir);
        if (entry == nullptr) {
            if (errno != 0 && errno != EINTR) {
                fail("cannot enumerate applications root");
            }
            break;
        }
        const std::string name(entry->d_name);
        if (!valid_managed_app_id(name)) {
            continue;
        }
        struct stat st = {};
        if (fstatat(apps_fd, entry->d_name, &st, AT_SYMLINK_NOFOLLOW) == 0 &&
            S_ISDIR(st.st_mode)) {
            applications.push_back(name);
        }
    }
    closedir(dir);
    return applications;
}

// Cross-platform stat timestamp accessors: Apple spells the fields
// st_mtimespec/st_ctimespec; other POSIX systems use st_mtim/st_ctim
// (same pattern as the coordinator's platform macros).
#if defined(__APPLE__)
#define CAPSID_MAIN_MTIME_SEC(st) ((st).st_mtimespec.tv_sec)
#define CAPSID_MAIN_MTIME_NSEC(st) ((st).st_mtimespec.tv_nsec)
#define CAPSID_MAIN_CTIME_SEC(st) ((st).st_ctimespec.tv_sec)
#define CAPSID_MAIN_CTIME_NSEC(st) ((st).st_ctimespec.tv_nsec)
#else
#define CAPSID_MAIN_MTIME_SEC(st) ((st).st_mtim.tv_sec)
#define CAPSID_MAIN_MTIME_NSEC(st) ((st).st_mtim.tv_nsec)
#define CAPSID_MAIN_CTIME_SEC(st) ((st).st_ctim.tv_sec)
#define CAPSID_MAIN_CTIME_NSEC(st) ((st).st_ctim.tv_nsec)
#endif

// Bounded read of the host.json document: O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC,
// only a Host-owned regular file, a size cap checked before reading and a
// device/inode/size re-check after reading (a FIFO or symlink never blocks
// or redirects startup, and a concurrent swap is rejected).
std::string read_host_config(const std::string& path) {
    const int fd = open(path.c_str(),
                        O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) {
        fail("cannot open --host-config: " + path);
    }
    struct stat before = {};
    if (fstat(fd, &before) != 0 || !S_ISREG(before.st_mode) ||
        before.st_uid != geteuid() || before.st_size < 0 ||
        static_cast<std::uint64_t>(before.st_size) > 1024U * 1024U) {
        close(fd);
        fail("--host-config is not a Host-owned regular file under 1 MiB");
    }
    std::string json;
    char buffer[4096];
    for (;;) {
        const ssize_t count = read(fd, buffer, sizeof(buffer));
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count < 0) {
            close(fd);
            fail("cannot read --host-config");
        }
        if (count == 0) {
            break;
        }
        if (json.size() + static_cast<std::size_t>(count) > 1024U * 1024U) {
            close(fd);
            fail("--host-config exceeds 1 MiB");
        }
        json.append(buffer, static_cast<std::size_t>(count));
    }
    struct stat after = {};
    if (fstat(fd, &after) != 0 || after.st_dev != before.st_dev ||
        after.st_ino != before.st_ino || after.st_size != before.st_size ||
        CAPSID_MAIN_MTIME_SEC(after) != CAPSID_MAIN_MTIME_SEC(before) ||
        CAPSID_MAIN_MTIME_NSEC(after) != CAPSID_MAIN_MTIME_NSEC(before) ||
        CAPSID_MAIN_CTIME_SEC(after) != CAPSID_MAIN_CTIME_SEC(before) ||
        CAPSID_MAIN_CTIME_NSEC(after) != CAPSID_MAIN_CTIME_NSEC(before)) {
        close(fd);
        fail("--host-config changed while it was read");
    }
    close(fd);
    return json;
}

int run_managed(const std::string& host_config_path,
                const std::string& worker_path) {
    // SIGTERM is blocked FIRST, before any thread exists: every thread
    // inherits the mask, and the main thread waits with sigwait below.
    sigset_t term_set;
    sigemptyset(&term_set);
    sigaddset(&term_set, SIGTERM);
    if (sigprocmask(SIG_BLOCK, &term_set, nullptr) != 0) {
        fail("cannot block SIGTERM");
    }
    const std::string host_json = read_host_config(host_config_path);
    // The authoritative schema boundary runs first; no second lax parser.
    const capsid::host::ConfigValidationResult validated =
        capsid::host::validate_config_json(
            capsid::host::ConfigDocument::kHost, host_json);
    if (!validated.ok) {
        fail("host.json rejected at " + validated.error.path + ": " +
             validated.error.message);
    }
    capsid::host::ParsedHostConfig config;
    std::string config_error;
    if (!parse_host_config(host_json, &config, &config_error)) {
        fail(config_error);
    }
    // Load the trusted bytecode keys BEFORE any worker spawns: attestation
    // verification is fail-closed, and an empty key set means every
    // trusted-bytecode deployment is rejected (never silently accepted).
    // The store owns the key bytes; the options below hold views into it,
    // so the store must outlive every ManagedHostOptions.
    capsid::host::TrustedKeyStore trusted_keys =
        capsid::host::TrustedKeyStore::load(config.trusted_keys,
                                            &config_error);
    if (trusted_keys.size() != config.trusted_keys.size()) {
        fail(config_error);
    }
    const std::vector<capsid::host::TrustedBytecodeKey> trusted_key_views(
        trusted_keys.keys().begin(), trusted_keys.keys().end());
    // Safe-open the roots; the secret template's parent is the dirfd the
    // coordinator opens App subdirectories from.
    const int apps_fd = open_verified_root(config.applications_root,
                                           "applications root");
    // The template must be EXACTLY <parent>/{application}: a trailing
    // suffix would be silently misinterpreted as part of the parent root.
    const std::string secret_template = config.secret_root_template;
    const std::string::size_type placeholder =
        secret_template.find("/{application}");
    const std::string secret_parent =
        secret_template.substr(0, placeholder);
    if (placeholder == std::string::npos || secret_parent.empty() ||
        secret_template.size() !=
            secret_parent.size() + std::string("/{application}").size()) {
        fail("secretRootTemplate must be exactly "
             "<root>/{application}");
    }
    const int secrets_fd = open_verified_root(secret_parent,
                                              "secret root template");
    const int state_fd = open_verified_root(config.state_root, "state root");
    const std::vector<std::string> applications =
        discover_applications(apps_fd);
    if (applications.empty()) {
        fail("applications root contains no configured Apps");
    }
    // Stable ownership for every App's options (the coordinator stores
    // pointers to them).
    std::vector<std::unique_ptr<capsid::host::ManagedHostOptions>> owned;
    std::vector<capsid::host::ManagedHostOptions*> app_options;
    for (const std::string& application : applications) {
        auto options = std::make_unique<capsid::host::ManagedHostOptions>();
        options->applications_root_fd = apps_fd;
        options->secret_root_template_fd = secrets_fd;
        options->state_root = config.state_root;
        options->application = application;
        options->worker_path = worker_path;
        options->host_policy = config.policy;
        options->trusted_keys = trusted_key_views;
        options->runtime_compatibility_id = CAPSID_BUILD_COMPATIBILITY_ID;
        options->stop_requested = &g_stop;
        app_options.push_back(options.get());
        owned.push_back(std::move(options));
    }
    // Active workers owned by this process: App -> complete worker pool.
    // A managed fixed pool is an atomic ownership unit; retaining only the
    // legacy single-worker pointer silently drops every shard beyond the
    // first one during recovery and replacement.
    std::map<std::string, std::vector<capsid_worker*>> active_worker_pools;
    std::mutex workers_mutex;
    const auto activate_pool =
        [&](const std::string& application,
            std::vector<capsid_worker*> workers) {
            std::lock_guard<std::mutex> lock(workers_mutex);
            const auto existing = active_worker_pools.find(application);
            if (existing != active_worker_pools.end()) {
                for (capsid_worker* worker : existing->second) {
                    if (worker != nullptr) {
                        capsid_worker_destroy(worker);
                    }
                }
            }
            active_worker_pools[application] = std::move(workers);
            return true;
        };
    const auto activate_worker =
        [&](const std::string& application, capsid_worker* worker) {
            std::vector<capsid_worker*> workers;
            if (worker != nullptr) {
                workers.push_back(worker);
            }
            return activate_pool(application, std::move(workers));
        };
    const auto retire_worker = [&](const std::string& application) {
        std::lock_guard<std::mutex> lock(workers_mutex);
        const auto existing = active_worker_pools.find(application);
        if (existing != active_worker_pools.end()) {
            for (capsid_worker* worker : existing->second) {
                if (worker != nullptr) {
                    capsid_worker_destroy(worker);
                }
            }
            active_worker_pools.erase(existing);
        }
    };
    const auto reclaim_workers = [&]() {
        std::lock_guard<std::mutex> lock(workers_mutex);
        for (const auto& entry : active_worker_pools) {
            for (capsid_worker* worker : entry.second) {
                if (worker != nullptr) {
                    capsid_worker_destroy(worker);
                }
            }
        }
        active_worker_pools.clear();
    };
    // The process-global worker permit (capacity.workersTotal). The slot
    // is bound to an active App: replacements do not re-acquire, a failed
    // replacement keeps the old slot, and only a newly acquired permit is
    // returned when the operation settles without a live worker.
    capsid::host::WorkerCapacityPermit capacity(
        static_cast<int>(config.capacity.workers_total));
    // Startup recovery: a durable active App is revalidated and its
    // replacement worker reaches READY before Admin readiness is
    // published. The global permit is acquired BEFORE any spawn; an
    // active-generation count beyond capacity fails closed at startup
    // instead of overspawning first.
    for (capsid::host::ManagedHostOptions* options : app_options) {
        // Only an App with durable state may consume a permit; a fresh
        // App never occupies capacity before its first deploy.
        bool newly_acquired = false;
        if (has_durable_active_state(state_fd, options->application)) {
            newly_acquired = capacity.acquire(options->application);
            if (!newly_acquired &&
                !capacity.holds(options->application)) {
                fail("active generation count exceeds "
                     "capacity.workersTotal");
            }
        }
        capsid::host::OperationStatus status;
        const capsid::host::DeployOutcome recovered =
            capsid::host::managed_recover(options, &status);
        if (!recovered.ok) {
            if (newly_acquired) {
                capacity.release(options->application);
            }
            fail("cannot recover active application " + options->application);
        }
        if (!recovered.workers.empty()) {
            // A state-file race can create an active document after the
            // preflight check. Reconcile the permit before retaining any
            // recovered workers; never leave a live pool outside capacity.
            if (!newly_acquired &&
                !capacity.holds(options->application)) {
                if (!capacity.acquire(options->application)) {
                    for (capsid_worker* worker : recovered.workers) {
                        capsid_worker_destroy(worker);
                    }
                    fail("active generation count exceeds "
                         "capacity.workersTotal");
                }
                newly_acquired = true;
            }
            // READY succeeded: the complete recovered pool holds the slot.
            capacity.record_success(options->application);
            activate_pool(options->application, std::move(recovered.workers));
        } else if (newly_acquired) {
            // No active/retired generation: no permanent occupancy.
            capacity.release(options->application);
        }
    }
    capsid::host::ManagedAdminBackend managed(app_options);
    managed.capacity = &capacity;
    capsid::host::AsyncAdminBackendOptions async_options;
    // Fixed bounded queue; startupsConcurrent is not the Admin ceiling.
    async_options.max_pending_operations = 8;
    async_options.external_stop = &g_stop;
    async_options.activate_worker = activate_worker;
    async_options.activate_pool = activate_pool;
    async_options.retire_worker = retire_worker;
    auto async = std::make_unique<capsid::host::AsyncAdminBackend>(
        &managed, async_options);
    capsid::host::AdminServiceOptions service_options;
    service_options.socket.path = config.admin_unix_path;
    service_options.socket.mode = config.admin_mode;
    service_options.http.api.authorization.allowed_uid =
        static_cast<std::uint64_t>(geteuid());
    service_options.http.api.max_header_bytes = 64U * 1024U;
    service_options.http.api.max_body_bytes = 64U * 1024U;
    service_options.http.header_timeout_ms = 5000;
    service_options.http.body_timeout_ms = 5000;
    service_options.http.write_timeout_ms = 5000;
    capsid::host::AdminService service(service_options, async.get());
    std::string service_error;
    if (!service.start(&service_error)) {
        fail("cannot start admin service: " + service_error);
    }
    // Shutdown order: stop the Admin control plane, cancel queued/running
    // deploys, reclaim active workers, then exit. The service's wait
    // removes the socket inode it created.
    int signal_number = 0;
    if (sigwait(&term_set, &signal_number) != 0) {
        fail("cannot wait for SIGTERM");
    }
    g_stop.store(true);
    service.request_stop();
    if (!service.wait(&service_error)) {
        fail("admin service failed: " + service_error);
    }
    // Explicitly wait for the async executor to settle (queued work
    // cancelled, running deploys interrupted) BEFORE reclaiming workers;
    // shutdown never depends on destructor ordering at function return.
    async.reset();
    reclaim_workers();
    close(state_fd);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::map<std::string, std::string> values;
    for (int index = 1; index < argc; index += 2) {
        const char* key = argv[index];
        const char* value = index + 1 < argc ? argv[index + 1] : nullptr;
        if (key == nullptr || key[0] != '-' || key[1] != '-') {
            fail("arguments must be --key value pairs");
        }
        if (value == nullptr || value[0] == '\0') {
            fail(std::string("missing value for ") + key);
        }
        const std::string name = key + 2;
        if (values.find(name) != values.end()) {
            fail("duplicate argument: --" + name);
        }
        values[name] = value;
    }

    const auto require = [&values](const std::string& name) -> std::string {
        auto it = values.find(name);
        if (it == values.end()) {
            fail("missing --" + name);
        }
        return it->second;
    };

    const std::string mode = require("mode");
    if (mode == "managed") {
        // Strict managed CLI: only --host-config and --worker are allowed.
        for (const std::pair<const std::string, std::string>& entry :
             values) {
            if (entry.first != "mode" && entry.first != "host-config" &&
                entry.first != "worker") {
                fail("--mode managed accepts only --host-config and "
                     "--worker");
            }
        }
        return run_managed(require("host-config"), require("worker"));
    }
    if (mode != "single-worker" && mode != "static-pool") {
        fail("--mode must be single-worker, static-pool or managed");
    }
    capsid::host::SingleWorkerServerOptions options;
    options.worker_path = require("worker");
    options.source_bundle_path = require("source-bundle");
    options.source_name = require("source-name");
    if (options.source_name.rfind("file://", 0) != 0) {
        fail("--source-name must be an absolute file URL");
    }
    options.application = require("application");
    if (!valid_application_id(options.application)) {
        fail("--application is not a valid App ID");
    }
    const std::string listen = require("listen");
    parse_listen(listen, &options.listen_address, &options.listen_port);
    // The address itself is validated before anything is spawned: an
    // unparseable address must fail the CLI phase, not the post-spawn bind
    // phase (a failure after spawn would otherwise have to tear down a live
    // worker).
    {
        boost::system::error_code address_error;
        const boost::asio::ip::address address =
            boost::asio::ip::make_address(options.listen_address,
                                          address_error);
        if (address_error) {
            fail("--listen requires an IP address: " +
                 options.listen_address);
        }
        (void)address;
    }
    const std::string routing = require("routing");
    if (routing != "path") {
        fail("--routing must be path in M1A");
    }
    options.public_scheme = require("public-scheme");
    if (options.public_scheme != "http" && options.public_scheme != "https") {
        fail("--public-scheme must be http or https");
    }
    options.public_authority = require("public-authority");
    if (!capsid::host::is_valid_public_authority(
            options.public_authority)) {
        fail("--public-authority must be host[:port]");
    }
    options.request_timeout_ms =
        parse_positive_integer(require("request-timeout-ms"),
                               "request-timeout-ms");
    const std::uint64_t window =
        parse_positive_integer(require("initial-stream-window"),
                               "initial-stream-window");
    if (window > std::numeric_limits<std::uint32_t>::max()) {
        fail("--initial-stream-window exceeds uint32");
    }
    options.initial_stream_window = static_cast<std::uint32_t>(window);
    const std::string sandbox = require("strict-sandbox");
    if (sandbox != "on" && sandbox != "off") {
        fail("--strict-sandbox must be on or off");
    }
    options.strict_sandbox = sandbox == "on";
    const std::uint64_t ready_fd =
        parse_positive_integer(require("ready-fd"), "ready-fd");
    if (ready_fd > static_cast<std::uint64_t>(
                       std::numeric_limits<short>::max())) {
        fail("--ready-fd must be a positive descriptor number");
    }
    options.ready_fd = static_cast<int>(ready_fd);
    // The READY record must be deliverable; verify the descriptor is open
    // before spawning the worker.
    if (fcntl(options.ready_fd, F_GETFD) == -1) {
        fail("--ready-fd is not an open descriptor");
    }

    // Benchmark-only static pool (NOT a managed production path): a fixed
    // 1/2/4-worker pool sharing one SO_REUSEPORT listener, driven by the
    // same worker/bundle/ready-fd parameters as the single-worker mode.
    // The pool keeps the pool-level READY contract and SIGTERM-bounded
    // shutdown; single-worker mode is unchanged.
    std::uint32_t workers = 1;
    if (mode == "static-pool") {
        const std::string workers_text = require("workers");
        const std::uint64_t parsed_workers =
            parse_positive_integer(workers_text, "workers");
        if (parsed_workers > std::numeric_limits<std::uint32_t>::max()) {
            fail("--workers exceeds uint32");
        }
        workers = static_cast<std::uint32_t>(parsed_workers);
        // M2 pool sizing scans {1,2,4,6,8}; the benchmark-only entry
        // accepts exactly this set (admission-sized pools come later).
        if (workers != 1 && workers != 2 && workers != 4 &&
            workers != 6 && workers != 8) {
            fail("--workers must be 1, 2, 4, 6 or 8 in static-pool mode");
        }
    }

    // M2 E-1 admission (§10.3): the benchmark CLI mirrors the effective
    // config's request/pool fields (the production path compiles the same
    // values through config → effective tier; see policy_compiler.cc).
    // Every field is optional; a missing value keeps the data plane
    // default. There is no main.cc hardcoding — the values flow into
    // StaticPoolServerOptions / SingleWorkerServerOptions below.
    const auto optional_value = [&values](const std::string& name)
        -> const std::string* {
        const auto it = values.find(name);
        return it == values.end() ? nullptr : &it->second;
    };
    const std::string* inflight_text = optional_value("max-inflight-per-worker");
    if (inflight_text != nullptr) {
        options.max_inflight_per_worker = parse_positive_integer(
            *inflight_text, "max-inflight-per-worker");
    }
    const std::string* queue_text = optional_value("queue-requests");
    if (queue_text != nullptr) {
        options.queue_requests =
            parse_nonnegative_integer(*queue_text, "queue-requests");
    }
    const std::string* queue_bytes_text = optional_value("queue-header-bytes");
    if (queue_bytes_text != nullptr) {
        if (!parse_size_bytes_text(*queue_bytes_text, &options.queue_header_bytes)) {
            fail("--queue-header-bytes must be a byte size (e.g. 2MiB)");
        }
    }
    const std::string* queue_timeout_text = optional_value("queue-timeout");
    if (queue_timeout_text != nullptr) {
        options.queue_timeout_ms = parse_duration_ms(
            *queue_timeout_text, "queue-timeout");
    }
    // M2 E-2 SSE permit (§9.3): the benchmark CLI mirrors the effective
    // config's request fields. Unlike the JSON route (where 0 = field not
    // set), a direct 0 here means unlimited — the same data-plane semantics
    // as --max-inflight-per-worker 0.
    const std::string* streaming_text = optional_value("max-streaming-inflight");
    if (streaming_text != nullptr) {
        options.max_streaming_inflight_per_worker = parse_nonnegative_integer(
            *streaming_text, "max-streaming-inflight");
    }
    const std::string* idle_text = optional_value("stream-idle-timeout");
    if (idle_text != nullptr) {
        options.stream_idle_timeout_ms = parse_nonnegative_integer(
            *idle_text, "stream-idle-timeout");
    }
    // M2 E-3 slow-client write deadline (§9.2): CLI 0 = unlimited, the same
    // data-plane semantics as the other CLI fields above.
    const std::string* write_timeout_text = optional_value("write-timeout");
    if (write_timeout_text != nullptr) {
        options.write_timeout_ms = parse_nonnegative_integer(
            *write_timeout_text, "write-timeout");
    }

    const std::vector<std::uint8_t> bundle =
        read_bundle(options.source_bundle_path);

    if (mode == "static-pool") {
        capsid::host::StaticPoolServerOptions pool_options;
        pool_options.workers = workers;
        // The admission values ride in the shard template (options above);
        // the pool-level StaticPoolServerOptions admission fields exist for
        // the production config route (effective tier → pool options, M2
        // managed-pool batch) and override the template when set.
        pool_options.worker_options = std::move(options);
        capsid::host::StaticPoolServer pool(std::move(pool_options));
        return pool.run(bundle);
    }
    capsid::host::SingleWorkerServer server(std::move(options));
    return server.run(bundle);
}
