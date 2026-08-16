// capsid-host executable entry point.
//
// The frozen M1A CLI is strictly validated before anything is spawned:
// unknown or missing arguments fail before any side effect. Startup order is
// fixed by the design: validate arguments, read and load the source bundle,
// spawn the worker, wait for READY and verify the compatibility ID, bind the
// listener, and only then write one canonical JSON line to --ready-fd.
// stdout never carries readiness or logs; diagnostics go to stderr.

#include <iostream>

#include "host/single_worker_server.h"
#include "host/static_pool_server.h"

#include "build_identity.h"
#include "capsid/runtime.h"
#include "host/admin_service.h"
#include "host/config.h"
#include "host/generation_pool.h"
#include "host/host_config_model.h"
#include "host/managed_admin_backend.h"
#include "host/managed_listener.h"
#include "host/metrics.h"
#include "host/process_snapshot.h"
#include "host/routing_snapshot.h"
#include "host/structured_log.h"
#include "host/trusted_key_store.h"
#include "host/worker_supervisor.h"

#include <jansson.h>

#include <boost/asio/ip/address.hpp>
#include <boost/system/error_code.hpp>

#include <dirent.h>
#include <signal.h>
#include <sys/stat.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "host/active_state.h"
#include "host/request_normalization.h"

namespace {

constexpr std::string_view kProbeGeneration =
    "sha256:0000000000000000000000000000000000000000000000000000000000000000";

void fail(const std::string& message) {
    // M2 item 7 (§12.2): a startup failure is a structured line on stderr
    // — one JSON object with the fixed field set, message JSON-escaped.
    // There is no StructuredLog instance yet at this point (a failure may
    // precede its construction), so the line is written directly.
    std::string escaped;
    escaped.reserve(message.size());
    for (const char c : message) {
        switch (c) {
            case '"': escaped += "\\\""; break;
            case '\\': escaped += "\\\\"; break;
            case '\n': escaped += "\\n"; break;
            case '\r': escaped += "\\r"; break;
            case '\t': escaped += "\\t"; break;
            default: escaped.push_back(c); break;
        }
    }
    const std::uint64_t timestamp_ms =
        static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count());
    std::fprintf(stderr,
                 "{\"timestamp\":\"%llu\",\"level\":\"error\","
                 "\"event\":\"startup\",\"result\":\"fail\","
                 "\"message\":\"%s\"}\n",
                 static_cast<unsigned long long>(timestamp_ms),
                 escaped.c_str());
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
    // Fail-early validation only: the coordinator re-opens the state root
    // per operation (verified the same way); this open proves the root is
    // present and well-formed before any worker spawns.
    const int state_fd = open_verified_root(config.state_root, "state root");
    const std::vector<std::string> applications =
        discover_applications(apps_fd);
    if (applications.empty()) {
        fail("applications root contains no configured Apps");
    }
    // M2 item 7 (design §12): the process-wide structured log and metrics
    // registry. Created after the config is validated so their lifetime
    // covers every option object below; injected into the coordinator,
    // supervisors and the Admin API. The worker-pid atomic tracks the
    // currently active worker for the render-time process snapshot.
    auto structured_log = std::make_unique<capsid::host::StructuredLog>(
        [](const std::string& line) {
            // One JSON object per line on stderr (design §12.2); a torn
            // partial write is impossible per call (single write below the
            // PIPE_BUF ceiling on regular stderr).
            const ssize_t written = ::write(STDERR_FILENO, line.data(),
                                            line.size());
            (void)written;
        });
    auto metrics = std::make_unique<capsid::host::MetricsRegistry>();
    std::atomic<pid_t> worker_pid{0};
    metrics->set_process_snapshot_provider(
        capsid::host::default_process_snapshot_provider(&worker_pid));
    const capsid::host::ResolvedRecoveryPolicy recovery_resolved =
        capsid::host::resolve_recovery_policy(config.recovery);
    if (!recovery_resolved.ok) {
        fail("invalid host.json recovery policy: " +
             recovery_resolved.error);
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
        options->bindings_root = config.bindings_root;
        if (!config.bindings_root.empty()) {
            std::string scan_error;
            if (!capsid::host::scan_bindings_root(
                    config.bindings_root, {0, geteuid()},
                    &options->binding_registry, &scan_error)) {
                std::cerr << "capsid-host: bindingsRoot rejected: "
                          << scan_error << std::endl;
                return EXIT_FAILURE;
            }
            options->binding_registry_loaded = true;
        }
        options->application = application;
        options->worker_path = worker_path;
        options->host_policy = config.policy;
        options->recovery_policy = recovery_resolved.policy;
        options->trusted_keys = trusted_key_views;
        options->runtime_compatibility_id = CAPSID_BUILD_COMPATIBILITY_ID;
        options->stop_requested = &g_stop;
        options->log = structured_log.get();
        options->metrics = metrics.get();
        app_options.push_back(options.get());
        owned.push_back(std::move(options));
    }
    // ---- PR-09c §9.3: the Managed data plane ----
    // The routing snapshot maps each App to its ACTIVE GenerationPool; the
    // listener routes every request through one atomic snapshot load and
    // pins the pool it found. Pools are ADOPTED from the warmed fleet — the
    // §8.3 replacement factory and the generation identity travel with the
    // deploy/recover outcome — so replacements respawn the same artifact
    // and effective config, and the route identity survives replacement.
    auto routing = std::make_shared<capsid::host::RoutingTable>();
    // App -> ACTIVE pool (the only pool a new request may route to).
    std::map<std::string,
             std::shared_ptr<capsid::host::GenerationPool>> active_pools;
    // Drained (retired or replaced) pools stay alive until shutdown: the
    // pool destructor drains and joins, and holding the shared_ptr keeps
    // that off the Admin worker thread.
    std::vector<std::shared_ptr<capsid::host::GenerationPool>> draining_pools;
    // §9.6-6 retired Apps: the name keeps a route-level tombstone (no
    // pool) so the router answers 404 after retire — the durable retire
    // tombstone of the coordinator is mirrored here, and a redeploy
    // revives the route. Never-deployed Apps stay unrouted (503).
    std::set<std::string> retired_apps;
    std::mutex pools_mutex;
    // The listener is created AFTER startup recovery (see below); pools
    // activated during recovery are wired by its start(), which wires every
    // pool in the current snapshot. Pools activated later are wired by the
    // §9.3 prepare callback (adopt_generation) before any commit publishes
    // a snapshot.
    std::unique_ptr<capsid::host::ManagedListener> data_plane;
    // Builds a fresh snapshot over the current view (live pools +
    // tombstones). Caller holds pools_mutex. Allocation allowed —
    // prepare-time only, never inside a commit.
    const auto build_snapshot = [&]() {
        std::vector<std::pair<
            std::string, std::shared_ptr<capsid::host::GenerationPool>>> routes;
        routes.reserve(active_pools.size() + retired_apps.size());
        for (const auto& entry : active_pools) {
            routes.emplace_back(entry.first, entry.second);
        }
        for (const std::string& application : retired_apps) {
            // Tombstone: routed name, no pool — the router's 404.
            routes.emplace_back(application, nullptr);
        }
        return capsid::host::RoutingSnapshot::build(std::move(routes));
    };
    // Publishes a fresh snapshot over the current view (the routing table
    // itself is lock-free; the lock makes the active_pools view atomic for
    // its writers). Allocation-free — also used by the commit callbacks.
    const auto publish_snapshot = [&]() {
        routing->publish(build_snapshot());
    };
    // The process-global weighted capacity ledger (§9.4): workersTotal is
    // the steady-state budget, activationSurgeWorkers the overlapping
    // replacement budget. Every reserve happens BEFORE any spawn, inside
    // the coordinator; the ledger is wired into every App's options below
    // and into every adopted pool's drain-complete hook.
    capsid::host::WorkerCapacityLedger ledger(
        config.capacity.workers_total,
        config.capacity.activation_surge_workers);
    // Ownership handoff for a warmed generation: adopt the fleet into a
    // GenerationPool (create_adopted destroys everything on failure — no
    // worker escapes) and install the event sink. A generation without its
    // replacement factory is rejected (factory null) — §8.3 replacement is
    // not optional. The pool is not yet routed, so it has no in-flight
    // requests when the sink is installed.
    // Active workers owned by this process: App -> worker. Direction A:
    // this map is a PURE OBSERVER — entries are overwritten and erased by
    // the pool's lifecycle callbacks, but never destroyed here. The
    // executor's worker thread is the sole reaper (destroying a worker
    // here while its old pool executor still holds it is the dual-engine
    // UAF); the map only mirrors the pool's fleet for the metrics
    // snapshot (worker_pid).
    std::map<std::string, capsid_worker*> active_workers;
    std::mutex workers_mutex;
    // M2 item 5b: the process-global fair startup-permit queue (design
    // §10.5.6). Both startup paths (deploy via the Admin backend,
    // replacement via the generation pools) share this single instance;
    // the queue decides order and fairness, capacity still decides
    // concurrency. The bound matches the Admin pending-queue bound.
    capsid::host::StartupPermitCoordinator startup_permits(&g_stop, 8);
    const auto adopt_generation =
        [&](const std::string& application,
            const std::vector<capsid_worker*>& workers,
            const capsid::host::WorkerExecutor::WorkerFactory& factory,
            const std::string& version,
            const std::string& generation_digest)
        -> std::shared_ptr<capsid::host::GenerationPool> {
            if (!factory) {
                for (capsid_worker* worker : workers) {
                    capsid_worker_destroy(worker);
                }
                return nullptr;
            }
            capsid::host::GenerationPoolOptions pool_options;
            pool_options.application_id = application;
            pool_options.version = version;
            pool_options.generation_digest = generation_digest;
            pool_options.workers =
                static_cast<std::uint32_t>(workers.size());
            pool_options.factory = factory;
            pool_options.recovery = recovery_resolved.policy;
            // Direction A: the pool is the only recovery engine, so it
            // owns the shared fair startup-permit queue for its
            // replacement spawns, and it reports fleet lifecycle changes
            // (started/exited/quarantine) through callbacks — the worker
            // map is a pure observer and never destroys a worker.
            pool_options.startup_permits = &startup_permits;
            pool_options.log = structured_log.get();
            pool_options.metrics = metrics.get();
            pool_options.on_worker_started =
                [&, application](const capsid::host::WorkerExecutor* executor) {
                    std::lock_guard<std::mutex> lock(workers_mutex);
                    capsid_worker* worker = executor->worker();
                    active_workers[application] = worker;
                    // M2 item 7 (§12.1): the render-time process snapshot
                    // reads the currently active worker's RSS.
                    worker_pid.store(
                        worker != nullptr
                            ? static_cast<pid_t>(capsid_worker_pid(worker))
                            : 0);
                };
            pool_options.on_worker_exited =
                [&, application](const capsid::host::WorkerExecutor* executor) {
                    std::lock_guard<std::mutex> lock(workers_mutex);
                    const auto existing = active_workers.find(application);
                    if (existing != active_workers.end() &&
                        existing->second == executor->worker()) {
                        active_workers.erase(existing);
                        worker_pid.store(0);
                    }
                };
            pool_options.on_quarantine = [&, application]() {
                // The durable tombstone: write BEFORE the pool's drain
                // signal (the pool issues it right after this callback),
                // so a crash in the window leaves a quarantined document,
                // which boot recovery honors (kKeepQuarantined never
                // resurrects). A failed write is logged by the coordinator;
                // the recovery is stopped either way.
                capsid::host::ManagedHostOptions* target = nullptr;
                for (capsid::host::ManagedHostOptions* options : app_options) {
                    if (options->application == application) {
                        target = options;
                        break;
                    }
                }
                if (target != nullptr) {
                    capsid::host::OperationStatus status;
                    (void)managed_quarantine(target, &status);
                }
                // The observer map is cleared without destroying anything:
                // the pool's drain reaps the workers through their
                // executors (the sole reaper).
                std::lock_guard<std::mutex> lock(workers_mutex);
                active_workers.erase(application);
                worker_pid.store(0);
            };
            const std::uint32_t pool_size = pool_options.workers;
            // §9.4: the reaper-finished instant releases the pool's
            // capacity count. The hook captures the size by value and
            // touches only the ledger (never the pools map or the plan),
            // so it is safe on the pool's pump thread.
            pool_options.on_drain_complete =
                [&ledger, application, pool_size] {
                    ledger.release_drained(application, pool_size);
                };
            std::string pool_error;
            std::shared_ptr<capsid::host::GenerationPool> pool =
                capsid::host::GenerationPool::create_adopted(
                    std::move(pool_options), workers, &pool_error);
            if (pool == nullptr) {
                return nullptr;
            }
            // The sink must be installed before the pool starts serving:
            // a request pinned before wiring would lose its response
            // events (wire_pool precondition: no in-flight requests).
            if (data_plane != nullptr) {
                data_plane->wire_pool(pool);
            }
            return pool;
        };
    // ---- §9.3 activation transaction callbacks ----
    // The coordinator runs these around the durable active.json write
    // (persist). prepare builds the ENTIRE new world (pool + view +
    // snapshot); commit is a single atomic publication (no allocation, no
    // I/O, no lock waiting); abort rolls the view back. The Async Admin
    // worker is single-threaded, so a whole transaction is never observed
    // mid-flight by another transaction.
    const auto prepare_activation =
        [&](const std::string& application,
            const capsid::host::DeployOutcome& prepared,
            std::string* error) -> std::unique_ptr<capsid::host::ActivationPlan> {
            std::unique_ptr<capsid::host::ActivationPlan> plan(
                new capsid::host::ActivationPlan());
            plan->application = application;
            plan->version = prepared.version;
            plan->generation_digest = prepared.generation_digest;
            plan->new_workers = prepared.workers.size();
            std::shared_ptr<capsid::host::GenerationPool> pool =
                adopt_generation(application, prepared.workers,
                                 prepared.generation_factory,
                                 prepared.version,
                                 prepared.generation_digest);
            if (pool == nullptr) {
                *error = "cannot activate the new generation";
                return nullptr;
            }
            plan->new_pool = pool;
            {
                std::lock_guard<std::mutex> lock(pools_mutex);
                const auto existing = active_pools.find(application);
                if (existing != active_pools.end()) {
                    plan->old_pool = existing->second;
                    plan->old_workers =
                        existing->second->configured_workers();
                    // The replaced generation stops serving only at
                    // COMMIT; the reference is parked here so commit
                    // needs no allocation.
                    draining_pools.push_back(existing->second);
                }
                // View pre-insert: commit's find+assign never allocates,
                // and the snapshot below already reflects the new
                // generation. A redeploy revives a retired App: the live
                // route wins over the tombstone, so the publish must not
                // emit both.
                active_pools[application] = pool;
                retired_apps.erase(application);
                // The COMPLETE new route map is built at prepare time so
                // commit is a single atomic snapshot swap.
                plan->new_snapshot = build_snapshot();
            }
            return plan;
        };
    const auto commit_activation = [&](capsid::host::ActivationPlan* plan) {
        std::lock_guard<std::mutex> lock(pools_mutex);
        // The new pool is already in the view (prepared): commit only
        // publishes and signals the drain. No allocation, no file I/O.
        if (plan->old_pool != nullptr) {
            // Drain signal only: new requests route to the new pool,
            // in-flight requests finish on the old one, and a drained
            // generation never starts a replacement.
            plan->old_pool->request_drain();
        }
        const auto entry = active_pools.find(plan->application);
        if (entry != active_pools.end()) {
            entry->second = plan->new_pool;
        }
        retired_apps.erase(plan->application);
        routing->publish(plan->new_snapshot);
    };
    const auto abort_activation = [&](capsid::host::ActivationPlan* plan) {
        std::lock_guard<std::mutex> lock(pools_mutex);
        // The persist failed: the disk still points at the old generation,
        // so the memory must too. The node check is defensive — the
        // single-threaded Async worker guarantees no other transaction
        // touched the view since prepare.
        const auto entry = active_pools.find(plan->application);
        if (entry != active_pools.end() &&
            entry->second == plan->new_pool) {
            if (plan->old_pool != nullptr) {
                entry->second = plan->old_pool;
            } else {
                active_pools.erase(entry);
            }
        }
        // The plan (and with it the never-published pool) dies on return;
        // the still-serving old pool stays parked in draining_pools until
        // shutdown (harmless — stop_and_join is idempotent).
    };
    // ---- §9.3 retire transaction callbacks ----
    const auto prepare_retire =
        [&](const std::string& application,
            std::string* error) -> std::unique_ptr<capsid::host::RetirePlan> {
            (void) error;
            std::unique_ptr<capsid::host::RetirePlan> plan(
                new capsid::host::RetirePlan());
            plan->application = application;
            {
                std::lock_guard<std::mutex> lock(pools_mutex);
                const auto existing = active_pools.find(application);
                if (existing != active_pools.end()) {
                    plan->pool = existing->second;
                    plan->workers =
                        existing->second->configured_workers();
                    // The route becomes a TOMBSTONE at prepare; commit
                    // only publishes. New requests get 404 (§9.6-6) while
                    // old in-flight requests finish on their pinned pool.
                    active_pools.erase(existing);
                    retired_apps.insert(application);
                    draining_pools.push_back(plan->pool);
                    plan->view_mutated = true;
                } else if (retired_apps.find(application) ==
                           retired_apps.end()) {
                    // Idempotent retire of a never-deployed App: the
                    // durable tombstone is (re)persisted; the route keeps
                    // its current shape (tombstone pre-inserted so commit
                    // has nothing to allocate).
                    retired_apps.insert(application);
                    plan->view_mutated = true;
                }
                plan->new_snapshot = build_snapshot();
            }
            return plan;
        };
    const auto commit_retire = [&](capsid::host::RetirePlan* plan) {
        std::lock_guard<std::mutex> lock(pools_mutex);
        if (plan->pool != nullptr) {
            // Drain signal only; the reference is parked in
            // draining_pools.
            plan->pool->request_drain();
        }
        routing->publish(plan->new_snapshot);
    };
    const auto abort_retire = [&](capsid::host::RetirePlan* plan) {
        std::lock_guard<std::mutex> lock(pools_mutex);
        if (!plan->view_mutated) {
            return;
        }
        const auto entry = active_pools.find(plan->application);
        if (plan->pool != nullptr) {
            // Restore the still-serving generation (may allocate — abort
            // runs only in the already-failed path).
            if (entry != active_pools.end()) {
                entry->second = plan->pool;
            } else {
                active_pools[plan->application] = plan->pool;
            }
            retired_apps.erase(plan->application);
        } else {
            retired_apps.erase(plan->application);
        }
    };
    const auto reclaim_workers = [&]() {
        std::vector<std::shared_ptr<capsid::host::GenerationPool>> all;
        {
            std::lock_guard<std::mutex> lock(pools_mutex);
            for (const auto& entry : active_pools) {
                all.push_back(entry.second);
            }
            active_pools.clear();
            all.insert(all.end(), draining_pools.begin(),
                       draining_pools.end());
            draining_pools.clear();
        }
        for (const auto& pool : all) {
            pool->stop_and_join();
        }
    };
    // Wire the §9.3 transaction callbacks and the §9.4 ledger into every
    // App's coordinator options.
    for (capsid::host::ManagedHostOptions* options : app_options) {
        options->ledger = &ledger;
        options->prepare_activation = prepare_activation;
        options->commit_activation = commit_activation;
        options->abort_activation = abort_activation;
        options->prepare_retire = prepare_retire;
        options->commit_retire = commit_retire;
        options->abort_retire = abort_retire;
    }
    // Startup recovery: a durable active App is revalidated and its
    // replacement worker reaches READY before Admin readiness is
    // published. The §9.4 reserve for the recovered fleet happens INSIDE
    // run_recover_operation — the coordinator learns the pool size only
    // after re-compiling the effective policy — and before any spawn: an
    // active-generation count beyond capacity fails closed at startup
    // instead of overspawning first.
    for (capsid::host::ManagedHostOptions* options : app_options) {
        capsid::host::OperationStatus status;
        const capsid::host::DeployOutcome recovered =
            capsid::host::managed_recover(options, &status);
        if (!recovered.ok) {
            fail("cannot recover active application " + options->application);
        }
        if (!recovered.workers.empty()) {
            // The reserve was committed inside run_recover_operation
            // (reserve_fresh before any spawn); a fresh adoption has
            // nothing further to settle.
            const std::shared_ptr<capsid::host::GenerationPool> pool =
                adopt_generation(options->application, recovered.workers,
                                 recovered.generation_factory,
                                 recovered.version,
                                 recovered.generation_digest);
            if (pool == nullptr) {
                fail("cannot activate the recovered pool for application " +
                     options->application);
            }
            {
                std::lock_guard<std::mutex> lock(pools_mutex);
                active_pools[options->application] = pool;
                publish_snapshot();
            }
        } else {
            // Durable state but no recovered workers: the App is retired
            // (or quarantined). Mirror the coordinator's durable tombstone
            // in the route — the name stays routed with no pool, so the
            // router keeps answering 404 after restart, exactly as it did
            // before the restart (§9.6-6). No capacity occupancy.
            {
                std::lock_guard<std::mutex> lock(pools_mutex);
                retired_apps.insert(options->application);
                publish_snapshot();
            }
        }
    }
    // §9.2 data plane: 0..1 public listeners, bound all-or-fail BEFORE
    // Admin readiness is published (a configured listener that cannot bind
    // must fail the Host, never serve a degraded control plane). The v1
    // event sink is single-consumer — exactly one listener may own the
    // pools' fan-out — so more than one configured listener fails closed at
    // startup instead of serving with a broken response fan-out.
    if (config.listeners.size() > 1) {
        fail("host.json configures multiple listeners; the v1 data plane "
             "supports exactly one");
    }
    if (config.listeners.size() == 1) {
        capsid::host::ManagedListenerOptions listener_options;
        listener_options.config = config.listeners[0];
        listener_options.routing = routing;
        listener_options.log = structured_log.get();
        data_plane = std::make_unique<capsid::host::ManagedListener>(
            std::move(listener_options));
        std::string listener_error;
        if (!data_plane->start(&listener_error)) {
            fail("cannot bind the data plane listener: " + listener_error);
        }
    }
    // M2 item 5a (direction A): one supervisor thread per App schedules
    // the active-health probes against the App's generation pool. The
    // pool is the only recovery engine; the supervisor only decides when
    // to probe and when consecutive failures recycle the worker. Created
    // after startup recovery so recovered pools exist; joined on shutdown
    // after the worker reclaim.
    std::vector<std::unique_ptr<capsid::host::WorkerSupervisor>> supervisors;
    for (capsid::host::ManagedHostOptions* options : app_options) {
        capsid::host::WorkerSupervisorOptions supervisor_options;
        supervisor_options.managed_options = options;
        supervisor_options.policy = recovery_resolved.policy;
        supervisor_options.current_pool =
            [&, application = options->application]()
            -> capsid::host::GenerationPool* {
                std::lock_guard<std::mutex> lock(pools_mutex);
                const auto existing = active_pools.find(application);
                return existing != active_pools.end() ? existing->second.get()
                                                      : nullptr;
            };
        supervisor_options.stop_requested = &g_stop;
        // M2 item 7: the process-wide log/metrics (probe events are
        // app-lane; the recycle verdicts feed §12.1).
        supervisor_options.log = structured_log.get();
        supervisor_options.metrics = metrics.get();
        // M2 item 6 (design §7.4): the active health probe schedule.
        // Individual Apps opt in with capsid.json healthCheck; the host
        // interval/failures apply to every configured App.
        supervisor_options.active_health_interval_ms =
            config.recovery.active_health_interval_ms;
        supervisor_options.active_health_failures =
            config.recovery.active_health_failures;
        supervisors.push_back(
            std::make_unique<capsid::host::WorkerSupervisor>(
                std::move(supervisor_options)));
    }
    capsid::host::ManagedAdminBackend managed(app_options);
    // M2 item 5b: the shared fair startup-permit queue. Capacity itself is
    // gated by the §9.4 ledger inside the coordinator (see run_deploy_operation);
    // the queue only decides ORDER and fairness across Apps.
    managed.startup_permits = &startup_permits;
    managed.metrics = metrics.get();
    capsid::host::AsyncAdminBackendOptions async_options;
    // Fixed bounded queue; startupsConcurrent is not the Admin ceiling.
    async_options.max_pending_operations = 8;
    async_options.external_stop = &g_stop;
    auto async = std::make_unique<capsid::host::AsyncAdminBackend>(
        &managed, async_options);
    capsid::host::AdminServiceOptions service_options;
    service_options.socket.path = config.admin_unix_path;
    service_options.socket.mode = config.admin_mode;
    service_options.http.api.authorization.allowed_uid =
        static_cast<std::uint64_t>(geteuid());
    service_options.http.api.max_header_bytes = 64U * 1024U;
    service_options.http.api.max_body_bytes = 64U * 1024U;
    // M2 item 7: the Admin API renders /metrics from the process-wide
    // registry and logs authorization failures as control-plane events.
    service_options.http.api.log = structured_log.get();
    service_options.http.api.metrics = metrics.get();
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
    // The data plane stops accepting and closes its sessions BEFORE the
    // pools drain: no new request can pin a dying pool, and the mailbox
    // (owned by the listener) outlives every pool's sink.
    if (data_plane != nullptr) {
        data_plane->request_stop();
        if (!data_plane->wait(&service_error)) {
            fail("data plane failed: " + service_error);
        }
    }
    reclaim_workers();
    // The reclaim destroyed every observed worker, which closes its IPC fd;
    // the supervisor threads see the channel die and exit. Joined after the
    // reclaim so their stop is a no-op observation, never a counted event.
    for (const auto& supervisor : supervisors) {
        supervisor->join();
    }
    close(state_fd);
    // M2 item 7 (§12.2): the shutdown event lands, then both lanes drain
    // (the supervisor join above already closed every observed channel).
    structured_log->log(capsid::host::LogLane::kControl,
                        {.event = capsid::host::log_events::kShutdown});
    structured_log->flush();
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
    // Default 64 KiB: the four-stack matrix (2026-08-13, 64K vs 16K) showed
    // the response window must cover a full 64 KiB response without a
    // mid-stream credit round trip; larger windows show no further gain
    // (E14 scan) and only widen the per-connection buffering bound.
    const auto window_it = values.find("initial-stream-window");
    const std::uint64_t window =
        parse_positive_integer(
            window_it == values.end() ? "65536" : window_it->second,
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

    // M2 item 7 (design §12): the process-wide structured log and metrics
    // registry for the data-plane modes. The render-time snapshot follows
    // the single active worker, which these modes publish at spawn.
    auto structured_log = std::make_unique<capsid::host::StructuredLog>(
        [](const std::string& line) {
            const ssize_t written = ::write(STDERR_FILENO, line.data(),
                                            line.size());
            (void)written;
        });
    auto metrics = std::make_unique<capsid::host::MetricsRegistry>();
    std::atomic<pid_t> worker_pid{0};
    metrics->set_process_snapshot_provider(
        capsid::host::default_process_snapshot_provider(&worker_pid));
    options.log = structured_log.get();
    options.metrics = metrics.get();

    if (mode == "static-pool") {
        capsid::host::StaticPoolServerOptions pool_options;
        pool_options.workers = workers;
        // The admission values ride in the shard template (options above);
        // the pool-level StaticPoolServerOptions admission fields exist for
        // the production config route (effective tier → pool options, M2
        // managed-pool batch) and override the template when set.
        pool_options.worker_options = std::move(options);
        capsid::host::StaticPoolServer pool(std::move(pool_options));
        const int exit_code = pool.run(bundle);
        structured_log->log(capsid::host::LogLane::kControl,
                            {.event = capsid::host::log_events::kShutdown});
        structured_log->flush();
        return exit_code;
    }
    capsid::host::SingleWorkerServer server(std::move(options));
    const int exit_code = server.run(bundle);
    structured_log->log(capsid::host::LogLane::kControl,
                        {.event = capsid::host::log_events::kShutdown});
    structured_log->flush();
    return exit_code;
}
