// M1D managed host coordinator. See managed_host.h.
//
// The deploy pipeline follows the frozen commit sequence and never reports
// Active before the real worker is READY: safe-read, attestation
// verification with the frozen selection rules (trusted bytecode / source
// fallback / fail closed), policy + secret compilation, real generation
// identity, unique exclusive staging, per-file fsync, COMPLETE last,
// rename into generations, version mapping, worker spawn + load + READY +
// compatibility check, and finally the active-state persist API.

#include "host/managed_host.h"

#include "capsid/runtime.h"
#include "host/active_state.h"
#include "host/artifact_safe_read.h"
#include "host/config.h"
#include "host/generation_identity.h"
#include "host/managed_registry.h"
#include "host/metrics.h"
#include "host/secret_file_provider.h"
#include "host/secret_snapshot.h"
#include "host/structured_log.h"
#include "host/worker_event_source.h"

#include <jansson.h>
#include <openssl/evp.h>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <poll.h>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace capsid::host {
namespace {

constexpr const char* kCompleteMarker = "COMPLETE";

// M2 item 7 (design §12.2): single write path for coordinator events.
// Null log (unit fixtures without the process-wide instance) is a no-op.
void emit_log(StructuredLog* log, LogLane lane, LogFields fields) {
    if (log != nullptr) {
        log->log(lane, std::move(fields));
    }
}

// M2 item 7 (design §12.1): the deploy-stage counters are emitted on the
// success edge of every pipeline phase; an operation-level failure is
// counted by the operation series at the terminal state.
void emit_deploy_stage(MetricsRegistry* metrics,
                       const std::string& stage,
                       const std::string& result,
                       const std::string& app) {
    if (metrics != nullptr) {
        metrics->count_deploy_stage(stage, result, app);
    }
}

// M2 item 7 (design §12.1): worker-family event counters (starting, ready,
// crash, unhealthy, replacement).
void count_event(MetricsRegistry* metrics, const std::string& event,
                 const std::string& app, const std::string& generation) {
    if (metrics != nullptr) {
        metrics->count_worker_event(event, app, generation);
    }
}

// M2 item 7 (design §12.1): refresh the log-drop gauge at every terminal
// operation state so the log family is always present for a served App
// (drops are counted globally; in the single-App managed Host the value is
// exact).
void refresh_log_dropped(MetricsRegistry* metrics, StructuredLog* log,
                         const std::string& app) {
    if (metrics != nullptr) {
        metrics->set_log_dropped(app,
                                 log != nullptr ? log->dropped_app_events()
                                                : 0);
    }
}

std::string sha256_hex(const std::string& data) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_size = 0;
    if (EVP_Digest(data.data(), data.size(), digest, &digest_size,
                   EVP_sha256(), nullptr) != 1 ||
        digest_size != 32) {
        return "";
    }
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (unsigned int i = 0; i < digest_size; ++i) {
        out.push_back(kHex[digest[i] >> 4]);
        out.push_back(kHex[digest[i] & 0x0f]);
    }
    return out;
}

std::string json_escape(const std::string& value) {
    std::ostringstream out;
    for (const char c : value) {
        switch (c) {
        case '"': out << "\\\""; break;
        case '\\': out << "\\\\"; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        default: out << c;
        }
    }
    return out.str();
}

bool write_file_at(int dir_fd, const char* name, const std::string& content,
                   std::string* error) {
    // O_EXCL + O_NOFOLLOW: the caller creates a fresh file; an existing
    // file or symlink at the name is an error, never an overwrite or a
    // redirect.
    const int fd = openat(dir_fd, name,
                          O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
                          0600);
    if (fd < 0) {
        *error = "cannot create state file";
        return false;
    }
    // EINTR/short-write loop: every byte is written or the operation fails.
    std::size_t written = 0;
    bool ok = true;
    while (written < content.size()) {
        const ssize_t count =
            write(fd, content.data() + written, content.size() - written);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            ok = false;
            break;
        }
        written += static_cast<std::size_t>(count);
    }
    if (ok && fsync(fd) != 0) {
        *error = "cannot sync state file";
        close(fd);
        return false;
    }
    close(fd);
    if (!ok) {
        *error = "cannot write state file";
    }
    return ok;
}

bool make_dir_at(int dir_fd, const char* name) {
    if (mkdirat(dir_fd, name, 0700) == 0) {
        return true;
    }
    return errno == EEXIST;
}

// Creates the subdirectory when missing, then reopens it with O_NOFOLLOW
// and verifies it is a directory owned by the Host euid with no
// group/other permission bits (mode 0700). Every level of the state chain
// (staging, generations, versions, generation objects) gets the same
// owner/mode check; a pre-existing symlink or a group/other-writable
// directory fails the walk instead of redirecting subsequent I/O. The
// caller owns the returned fd.
int prepare_subdir_at(int parent_fd, const char* name) {
    if (!make_dir_at(parent_fd, name)) {
        return -1;
    }
    const int fd = openat(parent_fd, name,
                          O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        return -1;
    }
    struct stat st = {};
    if (fstat(fd, &st) != 0 || !S_ISDIR(st.st_mode) ||
        st.st_uid != geteuid() || (st.st_mode & 0077) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

// Same verification for a subdirectory that must already exist (never
// created here): O_NOFOLLOW open, directory, Host-owned, mode 0700.
int open_verified_subdir(int parent_fd, const char* name) {
    const int fd = openat(parent_fd, name,
                          O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        return -1;
    }
    struct stat st = {};
    if (fstat(fd, &st) != 0 || !S_ISDIR(st.st_mode) ||
        st.st_uid != geteuid() || (st.st_mode & 0077) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

// Unique per-operation id: pid + a process-wide atomic counter. The
// counter is atomic because concurrent deploy operations may allocate ids
// from different threads; a non-atomic counter produced duplicate ids
// under the 32-way concurrency test.
std::string unique_operation_id() {
    static std::atomic<std::uint64_t> counter{0};
    const std::uint64_t id = counter.fetch_add(1, std::memory_order_relaxed);
    std::ostringstream out;
    out << "op-" << static_cast<long long>(getpid()) << "-" << id;
    return out.str();
}

// In-process operation registry and per-App operation locks moved to
// host/managed_registry.{h,cc} (spec §13.4): bounded by TTL + hard cap
// instead of growing without bound under an unbounded set of operation ids
// or application names.

// ---- worker warm-up: spawn, load, READY, compatibility check ----

struct WarmResult {
    bool ok = false;
    capsid_worker* worker = nullptr;
    std::string error;  // static text
};

// Spawn + load + ONE flush. The worker is up with its bundle loaded, but
// READY has NOT been consumed: the deploy/recover warm-up consumes it with
// the compatibility check (warm_worker), while the generation replacement
// factory hands the worker to a WorkerExecutor whose start() consumes READY
// and checks the compatibility id itself (PR-09c §8.3/§9.3). On failure the
// worker is destroyed and nullptr is returned — a half-started worker never
// escapes.
capsid_worker* spawn_loaded_worker(
    const ManagedHostOptions& options,
    const std::vector<std::uint8_t>& bundle,
    bool trusted_bytecode,
    const std::string& source_name,
    const EffectiveConfig& effective,
    const std::vector<std::pair<std::string, std::string>>& env_values,
    std::string* error) {
    capsid_worker* worker = nullptr;
    // ---- two-phase descriptor build (shared with the local-capsid.json
    // data planes; see build_runtime_policy) ----
    RuntimePolicy runtime_policy;
    if (!build_runtime_policy(effective, env_values, &runtime_policy,
                              error)) {
        return nullptr;
    }

    capsid_worker_config config;
    capsid_worker_config_init(&config);
    config.worker_path = options.worker_path.c_str();
    runtime_policy.apply(&config);
    // Effective resource/request/strict-sandbox controls reach the worker,
    // each in its own Runtime field: jsHeap bounds the QuickJS heap,
    // processAddressSpace bounds the process address space
    // (process_memory_limit -> RLIMIT_AS in the worker), fileDescriptors
    // bounds open descriptors, and maxInflightPerWorker bounds the
    // concurrent request window. memoryMax is the memory permit for Host
    // budget accounting and identity only; it never impersonates a worker
    // field. Sanitizer builds are the one deliberate exception: RLIMIT_AS
    // is fundamentally incompatible with ASan/TSan-instrumented workers (the
    // shadow-memory reservation exhausts any finite address-space limit
    // before READY), so the sanitizer build skips the forwarding while the
    // Release build always enforces it; the relation the Runtime's HELLO
    // validation would reject (process_memory_limit < js_heap_limit) is
    // additionally enforced by the policy compiler before staging, so an
    // invalid worker configuration can never start. The Host's
    // strict-sandbox decision is applied verbatim (worker spawn fails
    // closed when the platform cannot honor it).
    if (effective.js_heap_bytes > 0) {
        config.js_heap_limit = effective.js_heap_bytes;
    }
#if defined(CAPSID_ASAN_BUILD) || defined(CAPSID_TSAN_BUILD) || \
    defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
    const bool forward_address_space_limit = false;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
    const bool forward_address_space_limit = false;
#else
    const bool forward_address_space_limit = true;
#endif
#else
    const bool forward_address_space_limit = true;
#endif
    if (forward_address_space_limit &&
        effective.process_address_bytes > 0) {
        config.process_memory_limit = effective.process_address_bytes;
    }
    capsid_resource_limits limits;
    capsid_resource_limits_init(&limits);
    if (effective.file_descriptors > 0) {
        if (effective.file_descriptors >
            std::numeric_limits<std::uint32_t>::max()) {
            *error = "file descriptor limit exceeds the worker window";
            return nullptr;
        }
        limits.enabled_fields |= CAPSID_RESOURCE_LIMIT_FILE_DESCRIPTORS;
        limits.file_descriptors =
            static_cast<std::uint32_t>(effective.file_descriptors);
        config.resource_limits = &limits;
    }
    if (effective.requests_per_worker > 0) {
        if (effective.requests_per_worker >
            static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max())) {
            *error = "request limit exceeds the worker window";
            return nullptr;
        }
        config.max_inflight_requests =
            static_cast<std::uint32_t>(effective.requests_per_worker);
    }
    config.strict_sandbox = effective.strict_sandbox ? 1 : 0;
    const capsid_result spawn_result = capsid_worker_spawn(&config, &worker);
    if (spawn_result != CAPSID_OK) {
        *error = "worker spawn failed";
        return nullptr;
    }
    // M2 item 7: §12.1 deploy family — spawn and load phases on their
    // success edge.
    emit_deploy_stage(options.metrics, "spawn", "ok", options.application);
    // M2 item 7: §12.1 isolation family — the strict sandbox is the single
    // required/applied feature in managed mode; recorded per worker spawn
    // (idempotent gauge).
    if (options.metrics != nullptr) {
        options.metrics->set_isolation_required_features(options.application,
                                                         1);
        options.metrics->set_isolation_applied_features(
            options.application, effective.strict_sandbox ? 1 : 0);
    }
    capsid::host::WorkerEventSource event_source;
    event_source.set_worker(worker);
    const capsid_result load_result =
        trusted_bytecode
            ? capsid_worker_load_trusted_bytecode_named(
                  worker, bundle.data(), bundle.size(), source_name.c_str())
            : capsid_worker_load_bundle_named(
                  worker, bundle.data(), bundle.size(), source_name.c_str());
    if (load_result != CAPSID_OK) {
        capsid_worker_destroy(worker);
        *error = "worker bundle load failed";
        return nullptr;
    }
    // One flush: the loader may already have posted READY; the consumer's
    // drain loop observes it on its own next flush.
    const capsid_result flush = capsid_worker_flush(worker);
    if (flush != CAPSID_OK && flush != CAPSID_WOULD_BLOCK) {
        capsid_worker_destroy(worker);
        *error = "worker flush failed";
        return nullptr;
    }
    return worker;
}

WarmResult warm_worker(const ManagedHostOptions& options,
                       const std::vector<std::uint8_t>& bundle,
                       bool trusted_bytecode,
                       const std::string& source_name,
                       const std::string& generation,
                       const EffectiveConfig& effective,
                       const std::vector<std::pair<std::string, std::string>>& env_values) {
    WarmResult out;
    // M2 item 7: §12.1 worker family on the spawn edge — the operation
    // identifier is the generation being deployed.
    emit_log(options.log, LogLane::kControl,
             {.event = log_events::kWorkerStarting,
              .app = options.application,
              .generation = generation});
    count_event(options.metrics, "starting", options.application, generation);
    std::string spawn_error;
    capsid_worker* spawned = spawn_loaded_worker(
        options, bundle, trusted_bytecode, source_name, effective, env_values,
        &spawn_error);
    if (spawned == nullptr) {
        out.error = spawn_error;
        return out;
    }
    out.worker = spawned;
    // ---- READY handshake ----
    capsid::host::WorkerEventSource event_source;
    event_source.set_worker(out.worker);
    emit_deploy_stage(options.metrics, "load", "ok", options.application);
    // Wait for READY; the payload is the worker's compatibility ID.
    const std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(15);
    for (;;) {
        const capsid_result flush = capsid_worker_flush(out.worker);
        if (flush != CAPSID_OK && flush != CAPSID_WOULD_BLOCK) {
            capsid_worker_destroy(out.worker);
            out.worker = nullptr;
            out.error = "worker flush failed";
            return out;
        }
        capsid_event event = {};
        event.struct_size = sizeof(event);
        const capsid_result result =
            capsid_worker_next_event(out.worker, &event);
        if (result == CAPSID_OK) {
            if (event.type == CAPSID_EVENT_READY) {
                const std::string reported(
                    reinterpret_cast<const char*>(event.payload.data),
                    event.payload.size);
                if (reported != options.runtime_compatibility_id) {
                    capsid_worker_destroy(out.worker);
                    out.worker = nullptr;
                    out.error = "worker compatibility mismatch";
                    emit_deploy_stage(options.metrics, "health", "fail",
                                      options.application);
                    return out;
                }
                out.ok = true;
                // M2 item 7: §12.1 worker family on the READY edge; the
                // health probe phase is what the READY handshake is.
                emit_log(options.log, LogLane::kControl,
                         {.event = log_events::kWorkerReady,
                          .app = options.application,
                          .generation = generation});
                count_event(options.metrics, "ready", options.application,
                            generation);
                emit_deploy_stage(options.metrics, "health", "ok",
                                  options.application);
                return out;
            }
            if (event.type == CAPSID_EVENT_ERROR) {
                const std::string detail(
                    reinterpret_cast<const char*>(event.payload.data),
                    event.payload.size);
                capsid_worker_destroy(out.worker);
                out.worker = nullptr;
                out.error = detail.empty() ? "worker failed before READY"
                                           : detail;
                return out;
            }
            if (event.type == CAPSID_EVENT_EXIT) {
                capsid_worker_destroy(out.worker);
                out.worker = nullptr;
                out.error = "worker failed before READY";
                return out;
            }
            continue;
        }
        if (result != CAPSID_WOULD_BLOCK) {
            capsid_worker_destroy(out.worker);
            out.worker = nullptr;
            out.error = "worker event error";
            return out;
        }
        // The process-level stop signal aborts the handshake promptly: a
        // SIGTERM shutdown must not wait out the 15-second READY deadline.
        if (options.stop_requested != nullptr &&
            options.stop_requested->load()) {
            capsid_worker_destroy(out.worker);
            out.worker = nullptr;
            out.error = "worker READY interrupted";
            return out;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            capsid_worker_destroy(out.worker);
            out.worker = nullptr;
            out.error = "worker READY timeout";
            return out;
        }
        // Wait through the WorkerEventSource adapter (the single Host
        // adapter for the worker IPC descriptor), in bounded slices so the
        // stop signal is observed promptly.
        const std::chrono::steady_clock::time_point wait_until =
            std::min(deadline, std::chrono::steady_clock::now() +
                                   std::chrono::milliseconds(100));
        event_source.wait(wait_until);
    }
}

// PR-09c §8.3/§9.3: the generation's replacement factory — spawn/load/flush
// from the deploy's own options, artifact, source name, effective config
// and env values, all captured BY VALUE so replacements reproduce the exact
// generation after the deploy operation and its options object are gone.
// The factory entry-checks the process stop signal (the §9.2 replacement
// path aborts promptly on shutdown) and never consumes READY — the
// WorkerExecutor that spawns the replacement consumes READY and verifies
// the compatibility id against capsid_runtime_build_info().
WorkerExecutor::WorkerFactory make_generation_factory(
    const ManagedHostOptions& options,
    const std::vector<std::uint8_t>& bundle,
    bool trusted_bytecode,
    const std::string& source_name,
    const EffectiveConfig& effective,
    const std::vector<std::pair<std::string, std::string>>& env_values) {
    return [options, bundle, trusted_bytecode, source_name, effective,
            env_values](capsid_worker** out,
                        std::string* factory_error) -> bool {
        if (options.stop_requested != nullptr &&
            options.stop_requested->load()) {
            *factory_error = "host stop requested";
            return false;
        }
        std::string spawn_error;
        capsid_worker* worker = spawn_loaded_worker(
            options, bundle, trusted_bytecode, source_name, effective,
            env_values, &spawn_error);
        if (worker == nullptr) {
            *factory_error = spawn_error;
            return false;
        }
        *out = worker;
        return true;
    };
}

struct WarmPoolResult {
    bool ok = false;
    std::vector<capsid_worker*> workers;
    std::string error;  // static text
};

// Warm the ENTIRE fixed pool: exactly effective.workers distinct worker
// processes, each READY and compatibility-verified. The pool is atomic:
// when any single worker fails to warm, every already-warmed worker of the
// new pool is destroyed and recycled and nothing escapes, so a partially
// warmed pool can never be observed or handed off.
WarmPoolResult warm_worker_pool(
    const ManagedHostOptions& options,
    const std::vector<std::uint8_t>& bundle,
    bool trusted_bytecode,
    const std::string& source_name,
    const std::string& generation,
    const EffectiveConfig& effective,
    const std::vector<std::pair<std::string, std::string>>& env_values) {
    WarmPoolResult out;
    out.workers.reserve(effective.workers);
    for (std::uint32_t index = 0; index < effective.workers; ++index) {
        const WarmResult warm = warm_worker(options, bundle, trusted_bytecode,
                                            source_name, generation, effective,
                                            env_values);
        if (!warm.ok) {
            for (capsid_worker* worker : out.workers) {
                capsid_worker_destroy(worker);
            }
            out.workers.clear();
            out.error = warm.error;
            return out;
        }
        out.workers.push_back(warm.worker);
    }
    out.ok = true;
    return out;
}

// Publish the warmed pool on the outcome. The legacy single-worker field
// aliases workers[0] only for an exactly-one-worker pool; any other pool
// size leaves it null so an old consumer cannot claim just one worker.
void publish_pool(DeployOutcome* outcome, std::vector<capsid_worker*> workers) {
    outcome->workers = std::move(workers);
    if (outcome->workers.size() == 1) {
        outcome->worker = outcome->workers[0];
    }
}

// Destroy and recycle every worker of a pool (a failed deploy's own new
// pool; the old active pool is never touched).
void destroy_pool(const std::vector<capsid_worker*>& workers) {
    for (capsid_worker* worker : workers) {
        capsid_worker_destroy(worker);
    }
}

// POSIX adapter for the active-state persist contract. All I/O is anchored
// to the already-verified App state directory fd: openat/renameat only, with
// O_NOFOLLOW on every component. No path concatenation reaches the
// filesystem, so a symlinked apps/<app> or generations/<id>/COMPLETE cannot
// redirect state I/O outside stateRoot.
class PosixActiveStateFilesystem final : public ActiveStateFilesystem {
public:
    explicit PosixActiveStateFilesystem(int app_dir_fd)
        : app_dir_fd_(app_dir_fd) {}
    ~PosixActiveStateFilesystem() override { close(app_dir_fd_); }
    PosixActiveStateFilesystem(const PosixActiveStateFilesystem&) = delete;
    PosixActiveStateFilesystem& operator=(
        const PosixActiveStateFilesystem&) = delete;

    ActiveStateIoStatus cleanup_stale_active_temps() override {
        // Remove every active.json.tmp.* entry beneath the App state
        // directory: a crash between temp creation and rename leaves one,
        // and the next startup must not accumulate them. Unlink failures
        // on individual entries are ignored (best effort); a broken
        // directory scan is the only hard error.
        const int scan_fd = dup(app_dir_fd_);
        if (scan_fd < 0) {
            return ActiveStateIoStatus::kError;
        }
        DIR* dir = fdopendir(scan_fd);
        if (dir == nullptr) {
            close(scan_fd);
            return ActiveStateIoStatus::kError;
        }
        for (;;) {
            errno = 0;
            struct dirent* entry = readdir(dir);
            if (entry == nullptr) {
                if (errno != 0 && errno != EINTR) {
                    closedir(dir);
                    return ActiveStateIoStatus::kError;
                }
                break;
            }
            if (std::strncmp(entry->d_name, "active.json.tmp.", 16) == 0) {
                (void) unlinkat(app_dir_fd_, entry->d_name, 0);
            }
        }
        closedir(dir);
        return ActiveStateIoStatus::kOk;
    }
    ActiveStateReadResult read_active_file() override {
        ActiveStateReadResult result;
        const int fd = openat(app_dir_fd_, "active.json",
                              O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
        if (fd < 0) {
            result.status = errno == ENOENT ? ActiveStateIoStatus::kNotFound
                                            : ActiveStateIoStatus::kError;
            return result;
        }
        struct stat before = {};
        if (fstat(fd, &before) != 0 || !S_ISREG(before.st_mode) ||
            before.st_uid != geteuid() || before.st_size < 0 ||
            static_cast<std::uint64_t>(before.st_size) >
                kMaxActiveStateBytes) {
            close(fd);
            result.status = ActiveStateIoStatus::kError;
            return result;
        }
        char buffer[4096];
        std::string bytes;
        for (;;) {
            const ssize_t count = read(fd, buffer, sizeof(buffer));
            if (count < 0 && errno == EINTR) {
                continue;
            }
            if (count < 0) {
                // A persistent read error is not EOF: the active document
                // may be mid-rotation; fail the read instead of parsing a
                // truncated document as if it were complete.
                close(fd);
                result.status = ActiveStateIoStatus::kError;
                return result;
            }
            if (count == 0) {
                break;
            }
            if (bytes.size() + static_cast<std::size_t>(count) >
                kMaxActiveStateBytes) {
                close(fd);
                result.status = ActiveStateIoStatus::kError;
                return result;
            }
            bytes.append(buffer, static_cast<std::size_t>(count));
        }
        close(fd);
        result.status = ActiveStateIoStatus::kOk;
        result.bytes = std::move(bytes);
        return result;
    }
    GenerationCompleteness inspect_generation(
        std::string_view generation) override {
        // generations/<generation>/COMPLETE, every step O_NOFOLLOW; a
        // symlink anywhere in the chain is not a complete generation.
        const int generations_fd =
            openat(app_dir_fd_, "generations",
                   O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (generations_fd < 0) {
            return errno == ENOENT ? GenerationCompleteness::kMissing
                                   : GenerationCompleteness::kError;
        }
        const std::string generation_path(generation);
        const int generation_fd =
            openat(generations_fd, generation_path.c_str(),
                   O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        close(generations_fd);
        if (generation_fd < 0) {
            return errno == ENOENT ? GenerationCompleteness::kMissing
                                   : GenerationCompleteness::kError;
        }
        struct stat st = {};
        const int marker_fd =
            openat(generation_fd, kCompleteMarker,
                   O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
        close(generation_fd);
        if (marker_fd < 0) {
            return errno == ENOENT ? GenerationCompleteness::kMissing
                                   : GenerationCompleteness::kError;
        }
        const bool complete = fstat(marker_fd, &st) == 0 && S_ISREG(st.st_mode);
        close(marker_fd);
        return complete ? GenerationCompleteness::kComplete
                        : GenerationCompleteness::kError;
    }
    ActiveStateIoStatus create_active_temp_exclusive(
        std::string_view temp_name) override {
        const int fd = openat(app_dir_fd_, std::string(temp_name).c_str(),
                              O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC |
                                  O_NOFOLLOW,
                              0600);
        if (fd < 0) {
            return errno == EEXIST ? ActiveStateIoStatus::kAlreadyExists
                                   : ActiveStateIoStatus::kError;
        }
        close(fd);
        return ActiveStateIoStatus::kOk;
    }
    ActiveStateIoStatus write_active_temp(std::string_view temp_name,
                                          std::string_view bytes) override {
        const int fd = openat(app_dir_fd_, std::string(temp_name).c_str(),
                              O_WRONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
        if (fd < 0) {
            return ActiveStateIoStatus::kError;
        }
        struct stat st = {};
        if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) ||
            st.st_uid != geteuid()) {
            close(fd);
            return ActiveStateIoStatus::kError;
        }
        // EINTR/short-write loop: every byte reaches the temp file or the
        // persist fails. A single write() can return short on signals or
        // a full buffer without persisting the document.
        bool ok = true;
        std::size_t written = 0;
        while (written < bytes.size()) {
            const ssize_t count = write(fd, bytes.data() + written,
                                        bytes.size() - written);
            if (count < 0 && errno == EINTR) {
                continue;
            }
            if (count <= 0) {
                ok = false;
                break;
            }
            written += static_cast<std::size_t>(count);
        }
        close(fd);
        return ok ? ActiveStateIoStatus::kOk : ActiveStateIoStatus::kError;
    }
    ActiveStateIoStatus sync_active_temp(std::string_view temp_name) override {
        const int fd = openat(app_dir_fd_, std::string(temp_name).c_str(),
                              O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
        if (fd < 0) {
            return ActiveStateIoStatus::kError;
        }
        struct stat st = {};
        const bool ok = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) &&
                        st.st_uid == geteuid() && fsync(fd) == 0;
        close(fd);
        return ok ? ActiveStateIoStatus::kOk : ActiveStateIoStatus::kError;
    }
    ActiveStateIoStatus rename_temp_over_active(
        std::string_view temp_name) override {
        if (renameat(app_dir_fd_, std::string(temp_name).c_str(),
                     app_dir_fd_, "active.json") != 0) {
            return ActiveStateIoStatus::kError;
        }
        return ActiveStateIoStatus::kOk;
    }
    ActiveStateIoStatus sync_app_directory() override {
        if (fsync(app_dir_fd_) != 0) {
            return ActiveStateIoStatus::kError;
        }
        return ActiveStateIoStatus::kOk;
    }

private:
    int app_dir_fd_;
};

// §9.3 activation transaction tail, shared by every successful warm path
// (deploy and idempotent redeploy): prepare (before the persist, may
// fail) → persist (may fail) → commit (after the persist, noexcept).
//
// Caller contract:
//   - a failed prepare has already destroyed the warmed workers it was
//     handed (create_adopted's failure contract); the coordinator never
//     destroys them again,
//   - on the transactional path the plan owns the pool after prepare, so
//     the outcome's worker fields are cleared on every exit here,
//   - the §9.4 ledger reserve is settled here: commit on success, abort
//     on a failed persist (a failed warm never reaches this point and
//     must roll its own reserve back).
// Returns true when the activation was committed.
bool finish_activation(ManagedHostOptions* options, DeployOutcome* outcome,
                       const std::vector<capsid_worker*>& warm,
                       const std::string& version,
                       const std::string& generation_digest,
                       int app_state_fd, bool replacement,
                       OperationStatus* status) {
    status->state = OperationState::kActivating;
    std::unique_ptr<ActivationPlan> plan;
    if (options->prepare_activation != nullptr) {
        // The warmed workers travel on the outcome (const view) so the
        // prepare callback can adopt the fleet into its own pool; the plan
        // owns the workers from prepare onward, so the outcome's fields are
        // cleared on every exit below.
        outcome->workers = warm;
        // The transaction callbacks are an all-or-nothing group; a broken
        // configuration fails the deploy instead of half-publishing.
        if (options->commit_activation == nullptr ||
            options->abort_activation == nullptr) {
            destroy_pool(warm);
            status->state = OperationState::kFailed;
            status->error = "activation transaction misconfigured";
            outcome->error = status->error;
            outcome->workers.clear();
            outcome->worker = nullptr;
            return false;
        }
        std::string plan_error;
        plan = options->prepare_activation(options->application, *outcome,
                                           &plan_error);
        if (plan == nullptr) {
            // The failed prepare destroyed the warmed workers.
            status->state = OperationState::kFailed;
            status->error =
                plan_error.empty() ? "cannot activate the new generation"
                                   : plan_error;
            outcome->error = status->error;
            outcome->workers.clear();
            outcome->worker = nullptr;
            return false;
        }
    }
    // ---- persist (may fail) ----
    ActiveStateDocument document;
    document.state = ActiveServiceState::kActive;
    document.application = options->application;
    document.version = version;
    document.generation = generation_digest;
    PosixActiveStateFilesystem filesystem(app_state_fd);
    const ActiveStatePersistResult persisted = persist_active_state(
        document, outcome->operation_id, filesystem);
    if (!persisted.ok) {
        // Atomic: the failed deploy recycles its own new pool (the plan's
        // abort, or destroy_pool on the legacy path); the old active.json
        // and the old active pool stay untouched.
        if (plan != nullptr) {
            options->abort_activation(plan.get());
        } else {
            destroy_pool(warm);
        }
        if (options->ledger != nullptr) {
            options->ledger->abort_reserve(
                options->application, static_cast<std::uint64_t>(warm.size()),
                replacement);
        }
        status->state = OperationState::kFailed;
        status->error = "cannot persist active state";
        outcome->error = status->error;
        outcome->workers.clear();
        outcome->worker = nullptr;
        return false;
    }
    // ---- commit (noexcept; see activation_transaction.h) ----
    if (plan != nullptr) {
        options->commit_activation(plan.get());
        // Ownership moved to the plan: the warmed pointers are stale.
        outcome->workers.clear();
        outcome->worker = nullptr;
    } else {
        // Legacy caller path: the coordinator hands the owning pool back.
        publish_pool(outcome, warm);
    }
    // §9.4 settle the reserve: the new pool holds its count. (Commit
    // callback and ledger settle are both after the persist; the ledger
    // switch is part of the transactional settle.)
    if (options->ledger != nullptr) {
        if (replacement) {
            options->ledger->commit_replace(
                options->application,
                static_cast<std::uint64_t>(warm.size()),
                options->ledger->steady_of(options->application));
        } else {
            options->ledger->commit_fresh(options->application);
        }
    }
    status->state = OperationState::kActive;
    outcome->ok = true;
    // M2 item 7: §12.1 deploy family — the generation is active; the
    // caller's drain (old pool) belongs to the caller's data-plane.
    emit_deploy_stage(options->metrics, "activate", "ok",
                      options->application);
    emit_log(options->log, LogLane::kControl,
             {.event = log_events::kDeployStage,
              .app = options->application,
              .generation = generation_digest,
              .stage = "activate",
              .result = "ok"});
    return true;
}

// Opens stateRoot exactly once per operation with O_NOFOLLOW and verifies
// it (directory, Host-owned, mode 0700). Every subdirectory descent anchors
// to the returned fd, so stateRoot cannot be swapped between a path-based
// open and the state I/O that follows (TOCTOU). The caller owns the fd.
int open_verified_state_root(const std::string& state_root) {
    const int state_fd =
        open(state_root.c_str(),
             O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (state_fd < 0) {
        return -1;
    }
    struct stat st = {};
    if (fstat(state_fd, &st) != 0 || !S_ISDIR(st.st_mode) ||
        st.st_uid != geteuid() || (st.st_mode & 0077) != 0) {
        close(state_fd);
        return -1;
    }
    return state_fd;
}

// Descends the verified stateRoot fd chain: apps -> <app>, every step
// O_NOFOLLOW and verified (directory, Host-owned, mode 0700). A symlinked
// apps or App directory fails the chain instead of redirecting state I/O.
// With create=true the missing components are created (deploy); otherwise
// the chain must already exist and -1 with errno ENOENT means there is no
// App state at all.
int open_verified_app_state_dir(int state_fd, const std::string& application,
                                bool create) {
    int apps_fd = openat(state_fd, "apps",
                         O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (apps_fd < 0 && create && errno == ENOENT &&
        mkdirat(state_fd, "apps", 0700) == 0) {
        apps_fd = openat(state_fd, "apps",
                         O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    }
    struct stat st = {};
    if (apps_fd < 0 || fstat(apps_fd, &st) != 0 || !S_ISDIR(st.st_mode) ||
        st.st_uid != geteuid() || (st.st_mode & 0077) != 0) {
        if (apps_fd >= 0) {
            close(apps_fd);
        }
        return -1;
    }
    int app_fd = openat(apps_fd, application.c_str(),
                        O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (app_fd < 0 && create && errno == ENOENT &&
        mkdirat(apps_fd, application.c_str(), 0700) == 0) {
        app_fd = openat(apps_fd, application.c_str(),
                        O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    }
    close(apps_fd);
    if (app_fd < 0) {
        return -1;
    }
    if (fstat(app_fd, &st) != 0 || !S_ISDIR(st.st_mode) ||
        st.st_uid != geteuid() || (st.st_mode & 0077) != 0) {
        close(app_fd);
        return -1;
    }
    return app_fd;
}

// Strict parse of a canonical digest document, {"generation":"sha256:.."}:
// exactly one field, no duplicates, no unknown or extra fields, and a
// well-formed generation digest value. Version mappings and the committed
// generation.json share this shape; a damaged or foreign file never passes
// as a mapping (a substring scan would accept trailing garbage and unknown
// fields).
bool parse_generation_digest_document(const std::string& bytes,
                                      std::string* out) {
    if (bytes.empty() || bytes.size() > 4096) {
        return false;
    }
    json_error_t parse_error;
    json_t* root = json_loadb(bytes.data(), bytes.size(),
                              JSON_REJECT_DUPLICATES, &parse_error);
    if (root == nullptr || !json_is_object(root) ||
        json_object_size(root) != 1) {
        if (root) {
            json_decref(root);
        }
        return false;
    }
    json_t* generation = json_object_get(root, "generation");
    if (!json_is_string(generation)) {
        json_decref(root);
        return false;
    }
    const std::string digest = json_string_value(generation);
    json_decref(root);
    if (digest.size() != 71 || digest.compare(0, 7, "sha256:") != 0) {
        return false;
    }
    for (std::size_t index = 7; index < digest.size(); ++index) {
        const char c = digest[index];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return false;
        }
    }
    *out = digest;
    return true;
}

// Immutable version mapping: versions/<version>.json holds the generation
// Timestamp accessors for the read-after-check: macOS spells the fields
// st_mtimespec/st_ctimespec; other POSIX systems use st_mtim/st_ctim
// (same pattern as the secret provider's platform macros).
#if defined(__APPLE__)
#define CAPSID_HOST_MTIME_SEC(st) ((st).st_mtimespec.tv_sec)
#define CAPSID_HOST_MTIME_NSEC(st) ((st).st_mtimespec.tv_nsec)
#define CAPSID_HOST_CTIME_SEC(st) ((st).st_ctimespec.tv_sec)
#define CAPSID_HOST_CTIME_NSEC(st) ((st).st_ctimespec.tv_nsec)
#else
#define CAPSID_HOST_MTIME_SEC(st) ((st).st_mtim.tv_sec)
#define CAPSID_HOST_MTIME_NSEC(st) ((st).st_mtim.tv_nsec)
#define CAPSID_HOST_CTIME_SEC(st) ((st).st_ctim.tv_sec)
#define CAPSID_HOST_CTIME_NSEC(st) ((st).st_ctim.tv_nsec)
#endif

// Tri-state result of a bounded committed-snapshot read.
enum class ReadFileStatus {
    kOk,
    kNotFound,  // ENOENT: the file does not exist
    kInvalid,   // symlink, non-regular, foreign owner, oversized, I/O error
                // or concurrent mutation
};

// Bounded read of one file beneath dir_fd:
//   - opened O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK — a FIFO or a
//     symlink must not block the host or redirect the read;
//   - fstat'ed to accept only a Host-owned regular file;
//   - st_size checked before reading and the read loop itself enforces the
//     same hard ceiling, so a sparse or racing file can never grow past
//     the bound or consume host memory;
//   - after the read, inode/size/mtime/ctime are re-checked so a
//     concurrent swap is rejected instead of returning mixed content.
ReadFileStatus read_file_at(int dir_fd, const char* name,
                            std::size_t max_bytes, std::string* content) {
    const int fd = openat(dir_fd, name,
                          O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) {
        return errno == ENOENT ? ReadFileStatus::kNotFound
                               : ReadFileStatus::kInvalid;
    }
    struct stat st = {};
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) ||
        st.st_uid != geteuid() || st.st_size < 0 ||
        static_cast<std::uint64_t>(st.st_size) > max_bytes) {
        close(fd);
        return ReadFileStatus::kInvalid;
    }
    std::string bytes;
    char buffer[4096];
    for (;;) {
        const ssize_t count = read(fd, buffer, sizeof(buffer));
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count < 0) {
            // A persistent read error is not EOF; surface it as a failed
            // read rather than returning a truncated file as complete.
            close(fd);
            return ReadFileStatus::kInvalid;
        }
        if (count == 0) {
            break;
        }
        if (bytes.size() + static_cast<std::size_t>(count) > max_bytes) {
            close(fd);
            return ReadFileStatus::kInvalid;
        }
        bytes.append(buffer, static_cast<std::size_t>(count));
    }
    // Re-check the identity the read started from: a concurrent swap
    // (different device/inode or mutated size/timestamps) invalidates the
    // bytes. Timestamp fields are read through the same platform macro
    // used by the secret provider (macOS spells them st_mtimespec /
    // st_ctimespec).
    struct stat after = {};
    if (fstat(fd, &after) != 0 || after.st_dev != st.st_dev ||
        after.st_ino != st.st_ino || after.st_size != st.st_size ||
        CAPSID_HOST_MTIME_SEC(after) != CAPSID_HOST_MTIME_SEC(st) ||
        CAPSID_HOST_MTIME_NSEC(after) != CAPSID_HOST_MTIME_NSEC(st) ||
        CAPSID_HOST_CTIME_SEC(after) != CAPSID_HOST_CTIME_SEC(st) ||
        CAPSID_HOST_CTIME_NSEC(after) != CAPSID_HOST_CTIME_NSEC(st)) {
        close(fd);
        return ReadFileStatus::kInvalid;
    }
    close(fd);
    *content = std::move(bytes);
    return ReadFileStatus::kOk;
}

// digest of the published content. Returns -1 when the version was never
// published, 0 when the recorded generation matches (idempotent redeploy),
// 1 when the same Version ID holds different content (immutability

// conflict), and -2 on any I/O error or malformed document.
int check_version_mapping(int app_fd, const std::string& version,
                          const std::string& generation) {
    const std::string mapping_name = version + ".json";
    const int versions_fd = open_verified_subdir(app_fd, "versions");
    if (versions_fd < 0) {
        return errno == ENOENT ? -1 : -2;
    }
    // The mapping document is bounded to 4 KiB like every digest
    // document, read with the same regular-file/owner/nonblock checks.
    std::string bytes;
    const ReadFileStatus read_status = read_file_at(
        versions_fd, mapping_name.c_str(), 4096, &bytes);
    close(versions_fd);
    if (read_status == ReadFileStatus::kNotFound) {
        return -1;
    }
    if (read_status != ReadFileStatus::kOk) {
        return -2;
    }
    std::string recorded;
    if (!parse_generation_digest_document(bytes, &recorded)) {
        return -2;
    }
    return recorded == generation ? 0 : 1;
}

// True when generations/<generation>/COMPLETE exists as a regular file,
// every step O_NOFOLLOW (mirror of PosixActiveStateFilesystem::
// inspect_generation, but taking the App dir fd directly).
bool generation_is_complete(int app_fd, const std::string& generation) {
    const int generations_fd =
        openat(app_fd, "generations",
               O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (generations_fd < 0) {
        return false;
    }
    const int gen_fd = openat(generations_fd, generation.c_str(),
                              O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    close(generations_fd);
    if (gen_fd < 0) {
        return false;
    }
    struct stat st = {};
    const int marker_fd =
        openat(gen_fd, kCompleteMarker,
               O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    close(gen_fd);
    if (marker_fd < 0) {
        return false;
    }
    const bool complete = fstat(marker_fd, &st) == 0 && S_ISREG(st.st_mode);
    close(marker_fd);
    return complete;
}

// Records the immutable version->generation mapping after the generation
// commit. The mapping file is written exclusively: a concurrent deploy of
// the same version with the same content fails here (kAlreadyExists) rather
// than racing; the version gate already rejects different content.
bool write_version_mapping(int app_fd, const std::string& version,
                           const std::string& generation,
                           std::string* error) {
    if (!make_dir_at(app_fd, "versions")) {
        *error = "cannot prepare version mapping directory";
        return false;
    }
    // The versions directory gets the same owner/mode verification as
    // staging and generations: Host-owned, mode 0700, no symlink.
    const int versions_fd = open_verified_subdir(app_fd, "versions");
    if (versions_fd < 0) {
        *error = "cannot open version mapping directory";
        return false;
    }
    const bool ok = write_file_at(versions_fd, (version + ".json").c_str(),
                                  "{\"generation\":\"" + generation + "\"}\n",
                                  error);
    // The mapping is the durable record of the frozen version; sync the
    // directory too, or a crash can lose the new entry after the file
    // itself was synced.
    if (ok && fsync(versions_fd) != 0) {
        *error = "cannot sync version mapping directory";
        close(versions_fd);
        return false;
    }
    close(versions_fd);
    return ok;
}

// Owner/mode verification for the pre-opened secret root and the App
// subdirectory (M1D audit item 3): the App dir must be owned by the Host
// euid and mode 0700 (no group/other access). O_NOFOLLOW rejects a
// symlinked App directory so secret reads cannot escape the secret root.
int open_verified_app_secret_dir(int root_fd, const std::string& app) {
    const int fd = openat(root_fd, app.c_str(),
                          O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        return -1;
    }
    struct stat st = {};
    if (fstat(fd, &st) != 0 || st.st_uid != geteuid() ||
        (st.st_mode & 0077) != 0 || !S_ISDIR(st.st_mode)) {
        close(fd);
        return -1;
    }
    return fd;
}

// Records the immutable version->generation mapping after the generation
// commit. The mapping file is written exclusively: a concurrent deploy of
// the same version with the same content fails here (kAlreadyExists) rather
// than racing; the version gate already rejects different content.
// Recursively removes the subdirectory `name` beneath dir_fd and everything
// inside it. Every step is anchored to dirfds opened with O_NOFOLLOW and
// every entry is unlinked by name from its own parent fd — a symlink inside
// the tree is unlinked, never followed, so the removal cannot escape the
// subtree. Used for the exclusive per-operation staging tree: on failure
// (or when a shared generation already holds the content) the tree must
// not accumulate, and it must never be removed by an unverified path walk.
void remove_tree_at(int dir_fd, const char* name) {
    const int fd = openat(dir_fd, name,
                          O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        return;
    }
    DIR* directory = fdopendir(fd);
    if (directory == nullptr) {
        close(fd);
        return;
    }
    for (;;) {
        errno = 0;
        struct dirent* entry = readdir(directory);
        if (entry == nullptr) {
            if (errno != 0 && errno != EINTR) {
                break;
            }
            break;
        }
        if (std::strcmp(entry->d_name, ".") == 0 ||
            std::strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        struct stat st = {};
        if (fstatat(fd, entry->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0) {
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            remove_tree_at(fd, entry->d_name);
        }
        (void) unlinkat(fd, entry->d_name, 0);
    }
    closedir(directory);
    (void) unlinkat(dir_fd, name, AT_REMOVEDIR);
}

// Rejects any key of `object` not listed in `allowed`. Used by the
// snapshot parsers below: committed metadata is Host-produced canonical
// JSON, so an unknown field is corruption, not a forward-compatible
// extension.
bool reject_unknown_keys(json_t* object,
                         const std::set<std::string>& allowed) {
    const char* key = nullptr;
    json_t* value = nullptr;
    json_object_foreach(object, key, value) {
        if (allowed.find(key) == allowed.end()) {
            return false;
        }
    }
    return true;
}

bool parse_env_metadata(const std::string& json,
                        std::vector<EnvironmentSnapshotMetadata>* out,
                        std::string* error) {
    json_error_t parse_error;
    json_t* root = json_loadb(json.data(), json.size(),
                              JSON_REJECT_DUPLICATES, &parse_error);
    if (root == nullptr || !json_is_object(root) ||
        json_object_size(root) != 1) {
        *error = "invalid environment metadata snapshot";
        if (root) {
            json_decref(root);
        }
        return false;
    }
    json_t* environment = json_object_get(root, "environment");
    if (!json_is_array(environment)) {
        *error = "invalid environment metadata snapshot";
        json_decref(root);
        return false;
    }
    std::size_t index = 0;
    json_t* item = nullptr;
    json_array_foreach(environment, index, item) {
        if (!json_is_object(item)) {
            *error = "invalid environment metadata entry";
            json_decref(root);
            return false;
        }
        static const std::set<std::string> kEntryFields = {
            "name", "source", "keyId", "revision",
        };
        if (!reject_unknown_keys(item, kEntryFields)) {
            *error = "unknown environment metadata field";
            json_decref(root);
            return false;
        }
        json_t* name = json_object_get(item, "name");
        json_t* source = json_object_get(item, "source");
        json_t* key_id = json_object_get(item, "keyId");
        json_t* revision = json_object_get(item, "revision");
        if (!json_is_string(name) || !json_is_string(source)) {
            *error = "invalid environment metadata entry";
            json_decref(root);
            return false;
        }
        EnvironmentSnapshotMetadata metadata;
        metadata.name = json_string_value(name);
        const std::string source_text = json_string_value(source);
        if (source_text == "secret") {
            if (!json_is_string(key_id) || !json_is_string(revision)) {
                *error = "invalid secret environment metadata entry";
                json_decref(root);
                return false;
            }
            metadata.source = EnvironmentValueSource::kSecret;
            metadata.secret_key_id = json_string_value(key_id);
            metadata.opaque_revision = json_string_value(revision);
        } else if (source_text == "literal") {
            metadata.source = EnvironmentValueSource::kLiteral;
        } else {
            *error = "invalid environment metadata source";
            json_decref(root);
            return false;
        }
        out->push_back(std::move(metadata));
    }
    json_decref(root);
    return true;
}

// Deserializes the committed artifact.json record.
bool parse_artifact_json(const std::string& json, SelectedArtifactKind* selected,
                         std::string* source_name,
                         std::string* attestation_digest,
                         std::string* error) {
    json_error_t parse_error;
    json_t* root = json_loadb(json.data(), json.size(),
                              JSON_REJECT_DUPLICATES, &parse_error);
    if (root == nullptr || !json_is_object(root)) {
        *error = "invalid artifact snapshot";
        if (root) {
            json_decref(root);
        }
        return false;
    }
    // M2 item 7: "reason" is OPTIONAL — an old committed artifact record
    // (or a non-fallback deployment) simply has none. The three original
    // fields stay mandatory, so an old record parses exactly as before
    // (fail-closed on the fields that define the selection).
    static const std::set<std::string> kArtifactFields = {
        "selected", "sourceName", "attestationDigest", "reason",
    };
    if (!reject_unknown_keys(root, kArtifactFields)) {
        *error = "unknown artifact snapshot field";
        json_decref(root);
        return false;
    }
    json_t* kind = json_object_get(root, "selected");
    json_t* name = json_object_get(root, "sourceName");
    json_t* digest = json_object_get(root, "attestationDigest");
    json_t* reason = json_object_get(root, "reason");
    if (!json_is_string(kind) || !json_is_string(name) ||
        !json_is_string(digest) ||
        (reason != nullptr && !json_is_string(reason))) {
        *error = "invalid artifact snapshot record";
        json_decref(root);
        return false;
    }
    const std::string kind_text = json_string_value(kind);
    if (kind_text == "trusted-bytecode") {
        *selected = SelectedArtifactKind::kTrustedBytecode;
    } else if (kind_text == "source") {
        *selected = SelectedArtifactKind::kSource;
    } else {
        *error = "invalid artifact kind";
        json_decref(root);
        return false;
    }
    *source_name = json_string_value(name);
    *attestation_digest =
        json_is_string(digest) ? json_string_value(digest) : "";
    json_decref(root);
    return true;
}

// Artifact record for the committed generation: the selected kind, the
// frozen sourceName and the attestation digest when the deployment carried
// a provenance-valid attestation (trusted or compatibility-fallback).
// The optional reason (§13:682) names why the selection happened — it is
// written ONLY for a fallback, so a trusted or source deployment keeps an
// identical byte record across restarts (restart_identity_stable).
std::string artifact_json(SelectedArtifactKind selected,
                          const std::string& source_name,
                          const std::string& attestation_digest,
                          const std::string& reason = "") {
    std::ostringstream out;
    out << "{\"selected\":\""
        << (selected == SelectedArtifactKind::kTrustedBytecode
                ? "trusted-bytecode"
                : "source")
        << "\",\"sourceName\":\"" << json_escape(source_name)
        << "\",\"attestationDigest\":\"" << json_escape(attestation_digest)
        << "\"";
    if (!reason.empty()) {
        out << ",\"reason\":\"" << json_escape(reason) << "\"";
    }
    out << "}";
    return out.str();
}

// Canonical Host-policy serialization for the generation identity: the
// effective configuration depends on the Host policy, so a Host policy
// change must produce a different generation. Collections are sorted, so
// semantically identical policies in different orders hash identically.
std::string host_policy_identity(const HostPolicy& host) {
    std::ostringstream out;
    std::set<std::string> modules(host.module_allowlist.begin(),
                                  host.module_allowlist.end());
    for (const std::string& module : modules) {
        out << "m:" << module << ';';
    }
    std::set<std::string> patterns(host.env_patterns.begin(),
                                   host.env_patterns.end());
    for (const std::string& pattern : patterns) {
        out << "e:" << pattern << ';';
    }
    std::set<std::string> denies(host.env_deny_patterns.begin(),
                                 host.env_deny_patterns.end());
    for (const std::string& pattern : denies) {
        out << "d:" << pattern << ';';
    }
    std::set<std::string> roots(host.fs_read_roots.begin(),
                                host.fs_read_roots.end());
    for (const std::string& root : roots) {
        out << "f:" << root << ';';
    }
    std::set<std::string> targets;
    for (const FetchTarget& target : host.fetch_targets) {
        std::ostringstream entry;
        entry << target.host << ':';
        std::set<std::uint16_t> ports(target.ports.begin(),
                                      target.ports.end());
        for (const std::uint16_t port : ports) {
            entry << port << ',';
        }
        targets.insert(entry.str());
    }
    for (const std::string& target : targets) {
        out << "t:" << target << ';';
    }
    // The precise storage/stdio allow sets are part of the Host identity:
    // a namespace/stream grant change must re-derive the generation
    // identity (and therefore re-validate at recovery) exactly like any
    // other Host authorization change. Sets are sorted, so semantically
    // identical policies in different orders hash identically.
    std::set<std::string> storage_sets(host.storage_namespaces.begin(),
                                       host.storage_namespaces.end());
    for (const std::string& namespace_name : storage_sets) {
        out << "sn:" << namespace_name << ';';
    }
    std::set<std::string> stdio_sets(host.stdio_streams.begin(),
                                     host.stdio_streams.end());
    for (const std::string& stream : stdio_sets) {
        out << "ss:" << stream << ';';
    }
    out << "storage:" << (host.storage_allowed ? 1 : 0)
        << ";stdio:" << (host.stdio_allowed ? 1 : 0)
        << ";workers:" << host.max_workers
        << ";ready:" << host.min_ready
        << ";rps:" << host.max_requests_per_worker
        << ";mem:" << host.max_worker_memory_bytes
        << ";qreq:" << host.max_queue_requests
        << ";qbytes:" << host.max_queue_header_bytes
        << ";qtimeout:" << host.max_queue_timeout_ms
        << ";streaming:" << host.max_streaming_inflight_per_worker
        << ";idle:" << host.max_stream_idle_timeout_ms
        << ";wtimeout:" << host.max_write_timeout_ms
        << ";sandbox:" << (host.strict_sandbox ? 1 : 0);
    return sha256_hex(out.str());
}

// The source bundle file name from the committed capsid.json ("entry").
// The source artifact's frozen sourceName is derived from this trusted
// document (never from the mutable artifact record), so recovery can
// re-derive the name and compare it against the committed record instead
// of trusting artifact.json verbatim.
bool config_entry_name(const std::string& capsid_json, std::string* out) {
    json_error_t parse_error;
    json_t* root = json_loadb(capsid_json.data(), capsid_json.size(),
                              JSON_REJECT_DUPLICATES, &parse_error);
    if (root == nullptr || !json_is_object(root)) {
        if (root) {
            json_decref(root);
        }
        return false;
    }
    json_t* entry = json_object_get(root, "entry");
    const bool ok = json_is_string(entry) && !json_is_null(entry) &&
                    json_string_value(entry)[0] != '\0';
    if (ok) {
        *out = json_string_value(entry);
    }
    json_decref(root);
    return ok;
}

// Frozen sourceName derivation: the name a committed generation must
// carry. A bytecode-bearing version always compiles to bundle.qjsb (the
// compile convention the attestation claims); a pure source version names
// its bundle after the committed config's entry. Both are derived from
// trusted state only — the artifact record is compared against this, not
// the other way around.
std::string derive_source_name(const std::string& application,
                               const std::string& version,
                               const std::string& capsid_json,
                               bool bytecode_attestation_present) {
    if (bytecode_attestation_present) {
        return "file://" + application + "/" + version + "/bundle.qjsb";
    }
    std::string entry;
    if (config_entry_name(capsid_json, &entry)) {
        return "file://" + application + "/" + version + "/" + entry;
    }
    return "";
}

// Structural validation of a committed sourceName against the frozen
// derivation rule: "file://{application}/{version}/{name}" where {name} is
// the committed config's entry (source artifact) or "bundle.qjsb"
// (bytecode-bearing artifact) and {version} is a non-empty segment
// separated by literal '/' characters. The separator is verified as an
// actual slash: a malformed name like "file://app/v1Xbundle.mjs" (where
// the version and the artifact name are fused) must not be accepted by
// length arithmetic. The embedded version segment is returned; its
// legitimacy is checked against the version mappings by the caller (a
// generation's name was bound to whichever version published it first, so
// the segment must be one of the versions mapped to this generation).
std::string expected_source_suffix(const std::string& capsid_json,
                                   bool bytecode_attestation_present) {
    if (bytecode_attestation_present) {
        return "bundle.qjsb";
    }
    std::string entry;
    if (config_entry_name(capsid_json, &entry)) {
        return entry;
    }
    return "";
}

// Collects every Version ID whose immutable mapping points at
// `generation` (versions/<version>.json -> {"generation":"..."}). The
// embedded version segment of a shared generation's sourceName must be one
// of these: the generation was named by whichever mapped version published
// it first, and the immutable version mappings are the only trusted record
// of that. Every mapping is read with a size cap and must be a regular
// file (no symlinks); a corrupt entry fails the scan (fail closed).
bool collect_mapping_versions(int app_state_fd, const std::string& generation,
                              std::set<std::string>* versions_out) {
    const int versions_fd = open_verified_subdir(app_state_fd, "versions");
    if (versions_fd < 0) {
        return errno == ENOENT;
    }
    const int scan_fd = dup(versions_fd);
    if (scan_fd < 0) {
        close(versions_fd);
        return false;
    }
    DIR* dir = fdopendir(scan_fd);
    if (dir == nullptr) {
        close(versions_fd);
        close(scan_fd);
        return false;
    }
    bool ok = true;
    for (;;) {
        errno = 0;
        struct dirent* entry = readdir(dir);
        if (entry == nullptr) {
            if (errno != 0 && errno != EINTR) {
                ok = false;
            }
            break;
        }
        const std::string name(entry->d_name);
        if (name.size() <= 5 ||
            name.compare(name.size() - 5, 5, ".json") != 0) {
            continue;
        }
        const std::string version_name = name.substr(0, name.size() - 5);
        if (version_name.empty()) {
            continue;
        }
        struct stat st = {};
        if (fstatat(versions_fd, entry->d_name, &st, AT_SYMLINK_NOFOLLOW) !=
                0 ||
            !S_ISREG(st.st_mode) || st.st_size > 4096) {
            ok = false;
            break;
        }
        std::string bytes;
        if (read_file_at(versions_fd, entry->d_name, 4096, &bytes) !=
            ReadFileStatus::kOk) {
            ok = false;
            break;
        }
        std::string mapped;
        if (!parse_generation_digest_document(bytes, &mapped)) {
            ok = false;
            break;
        }
        if (mapped == generation) {
            versions_out->insert(version_name);
        }
    }
    closedir(dir);
    close(versions_fd);
    return ok;
}
bool parse_source_name_structure(const std::string& application,
                                 const std::string& capsid_json,
                                 bool bytecode_attestation_present,
                                 const std::string& source_name,
                                 std::string* version_out) {
    const std::string expected_suffix =
        expected_source_suffix(capsid_json, bytecode_attestation_present);
    if (expected_suffix.empty()) {
        return false;
    }
    const std::string prefix = "file://" + application + "/";
    if (source_name.size() <= prefix.size() + 1 + expected_suffix.size() ||
        source_name.compare(0, prefix.size(), prefix) != 0) {
        return false;
    }
    // The version segment runs from the prefix to the FIRST slash after it
    // (the version grammar forbids path separators, so the first slash IS
    // the separator), and the artifact name must start exactly there.
    const std::string::size_type separator = source_name.find('/', prefix.size());
    if (separator == std::string::npos ||
        separator + 1 + expected_suffix.size() != source_name.size()) {
        return false;
    }
    const std::string version_text =
        source_name.substr(prefix.size(), separator - prefix.size());
    if (version_text.empty()) {
        return false;
    }
    if (source_name.compare(separator + 1, expected_suffix.size(),
                            expected_suffix) != 0) {
        return false;
    }
    if (version_out != nullptr) {
        *version_out = version_text;
    }
    return true;
}

}  // namespace

// Result of the shared prepare pipeline (deploy step 1-6: safe-read,
// artifact selection with attestation verification, authoritative schema +
// policy + secret compilation, generation identity). Deploy stages every
// field into the committed generation snapshot; recovery rebuilds from that
// snapshot alone and never re-reads the upload boundary.
struct PreparedDeployment {
    bool ok = false;
    std::string error;
    // The selected artifact bytes (bundle.bin in the snapshot).
    std::vector<std::uint8_t> bundle_bytes;
    SelectedArtifactKind selected = SelectedArtifactKind::kSource;
    std::string source_name;
    EffectiveConfig effective;
    std::vector<std::pair<std::string, std::string>> env_values;
    std::string generation_digest;
    // Full snapshot material for re-verification at recovery time: the
    // source bundle, the raw bytecode, the attestation claim and its
    // signature. Empty when the deployment carried none.
    std::vector<std::uint8_t> source_bytes;
    std::vector<std::uint8_t> bytecode_bytes;
    std::string attestation_json_text;
    std::vector<std::uint8_t> signature_bytes;
    std::string attestation_digest;
    // The authoritative capsid/app-v1 document, committed verbatim so
    // recovery can re-run schema validation and policy/secret compilation
    // against the current Host authority without the upload boundary.
    std::string capsid_json;
    // The secret-snapshot compiler's metadata document (names, sources,
    // key IDs, opaque revisions — never values). The committed generation
    // stores no literal or secret values.
    std::string env_metadata_json;
    // M2 item 7 (design §13:682): why the actual selected artifact was
    // chosen. Empty for a direct source or trusted-bytecode selection;
    // "compatibility-mismatch" when a provenance-valid attestation fell
    // back to source. Recorded in artifact.json; never part of the
    // generation identity (the artifact record is not a digest input).
    std::string fallback_reason;
};

PreparedDeployment prepare_deployment(ManagedHostOptions* options,
                                      const std::string& version,
                                      OperationStatus* status) {
    PreparedDeployment prepared;
    const auto fail = [&](const std::string& message) {
        status->state = OperationState::kFailed;
        status->error = message;
        prepared.error = message;
    };

    // ---- 1. safe-read the version artifacts ----
    const SafeReadResult artifacts = safe_read_version_artifacts(
        options->applications_root_fd, options->application, version,
        kMaxVersionArtifactTotalBytes);
    if (artifacts.code != SafeReadErrorCode::kNone) {
        fail(artifacts.message);
        return prepared;
    }

    // ---- 2-4. artifact selection with real attestation verification ----
    SelectedArtifactKind selected = SelectedArtifactKind::kSource;
    std::string attestation_digest;
    // The frozen sourceName: derived from the committed capsid.json (entry)
    // for a source artifact, or the compile convention bundle.qjsb when the
    // version carries bytecode. The derived name is what the attestation
    // must claim and what the generation record must carry.
    std::string source_name;
    // Snapshot material for recovery-time re-verification; empty when the
    // version carried no attestation.
    std::string attestation_json_text;
    if (artifacts.artifacts.has_bytecode) {
        // The bytecode's module name (the compile filename) is the frozen
        // sourceName the attestation claims; the verification and the
        // trusted load both use it.
        source_name = "file://" + options->application + "/" + version +
                      "/bundle.qjsb";
        attestation_json_text.assign(
            artifacts.artifacts.attestation.bytes.begin(),
            artifacts.artifacts.attestation.bytes.end());
        capsid::host::BytecodeAttestationInput input;
        input.source = std::span<const std::uint8_t>(
            artifacts.artifacts.bundle.bytes.data(),
            artifacts.artifacts.bundle.bytes.size());
        input.bytecode = std::optional<std::span<const std::uint8_t>>(
            std::span<const std::uint8_t>(
                artifacts.artifacts.bytecode.bytes.data(),
                artifacts.artifacts.bytecode.bytes.size()));
        input.attestation_json = std::string_view(attestation_json_text);
        input.signature = std::optional<std::span<const std::uint8_t>>(
            std::span<const std::uint8_t>(
                artifacts.artifacts.signature.bytes.data(),
                artifacts.artifacts.signature.bytes.size()));
        input.expected_application = options->application;
        input.expected_version = version;
        input.expected_source_name = source_name;
        input.runtime_compatibility_id = options->runtime_compatibility_id;
        input.trusted_keys = std::span<const capsid::host::TrustedBytecodeKey>(
            options->trusted_keys.data(), options->trusted_keys.size());
        const capsid::host::BytecodeAttestationResult verified =
            capsid::host::verify_bytecode_attestation(input);
        if (verified.selection ==
            capsid::host::BytecodeArtifactSelection::kTrustedBytecode) {
            selected = SelectedArtifactKind::kTrustedBytecode;
            attestation_digest = sha256_hex(attestation_json_text);
        } else if (verified.selection ==
                   capsid::host::BytecodeArtifactSelection::kSource &&
                   verified.code ==
                       capsid::host::BytecodeAttestationErrorCode::kCompatibilityMismatch) {
            // Frozen fallback: provenance-valid but compatibility-mismatched
            // bytecode falls back to source. The verified attestation stays
            // in the generation identity: a fallback must not collapse to
            // the identity of an otherwise identical source-only deploy.
            selected = SelectedArtifactKind::kSource;
            attestation_digest = sha256_hex(attestation_json_text);
            // §13:682: the committed artifact record names the fallback
            // reason next to the verified attestation and the actual
            // selected artifact.
            prepared.fallback_reason = "compatibility-mismatch";
        } else {
            fail("bytecode attestation rejected: " + verified.message);
            return prepared;
        }
    }
    // The warmed artifact is the selected one: bytecode for trusted, the
    // source bundle otherwise.
    prepared.bundle_bytes =
        selected == SelectedArtifactKind::kTrustedBytecode
            ? artifacts.artifacts.bytecode.bytes
            : artifacts.artifacts.bundle.bytes;

    // ---- 5. authoritative schema, then policy + secret compilation ----
    // The capsid/app-v1 document is validated against the frozen schema
    // first (unknown fields, duplicates, shapes and limits), so the managed
    // boundary cannot bypass the authority with a legacy document shape.
    const std::string capsid_json(
        artifacts.artifacts.capsid_json.bytes.begin(),
        artifacts.artifacts.capsid_json.bytes.end());
    const ConfigValidationResult validated = validate_config_json(
        ConfigDocument::kApplication, capsid_json);
    if (!validated.ok) {
        fail("capsid.json rejected at " + validated.error.path +
             ": " + validated.error.message);
        return prepared;
    }
    if (selected == SelectedArtifactKind::kSource &&
        attestation_digest.empty()) {
        // Source-only: derive the frozen sourceName from the trusted
        // committed config's entry (recovery re-derives the same name).
        source_name = derive_source_name(options->application, version,
                                         capsid_json, false);
        if (source_name.empty()) {
            fail("capsid.json has no usable entry");
            return prepared;
        }
    }
    AppRequest app_request;
    std::string config_error;
    if (!parse_app_request(artifacts.artifacts.capsid_json.bytes,
                           &app_request, &config_error)) {
        fail(config_error);
        return prepared;
    }
    // Resolve the secret requests via the provider, then compile the whole
    // env request through the authoritative secret-snapshot compiler
    // (compile_secret_snapshot): names, patterns, values, secret keys and
    // the total snapshot budget are all rejected here, before any staging.
    // A secret root is only required when at least one env entry references
    // valueFrom; pure literal entries need no provider.
    bool has_secret_requests = false;
    for (const AppRequest::EnvRequest& request : app_request.env) {
        if (request.from_secret) {
            has_secret_requests = true;
            break;
        }
    }
    std::vector<SecretFileOutcome> outcomes;
    int app_secret_fd = -1;
    if (has_secret_requests) {
        if (options->secret_root_template_fd < 0) {
            fail("secret root not configured");
            return prepared;
        }
        app_secret_fd = open_verified_app_secret_dir(
            options->secret_root_template_fd, options->application);
        if (app_secret_fd < 0) {
            fail("secret app directory unverified");
            return prepared;
        }
        std::vector<std::string> key_ids;
        for (const AppRequest::EnvRequest& request : app_request.env) {
            if (request.from_secret) {
                key_ids.push_back(request.secret_key_id);
            }
        }
        outcomes = read_secret_files(app_secret_fd, key_ids);
        close(app_secret_fd);
    }
    std::vector<std::string_view> host_names;
    for (const std::string& pattern : options->host_policy.env_patterns) {
        host_names.push_back(pattern);
    }
    std::vector<EnvironmentRequest> requests;
    for (const AppRequest::EnvRequest& request : app_request.env) {
        EnvironmentRequest parsed;
        parsed.name = request.name;
        if (request.from_secret) {
            parsed.value_from = request.secret_key_id;
        } else {
            parsed.value = request.literal;
        }
        requests.push_back(parsed);
    }
    std::vector<ResolvedSecret> resolved;
    std::size_t secret_index = 0;
    for (const AppRequest::EnvRequest& request : app_request.env) {
        if (!request.from_secret) {
            continue;
        }
        if (secret_index >= outcomes.size() ||
            !outcomes[secret_index].error.empty()) {
            fail("secret resolution failed");
            return prepared;
        }
        const SecretFileOutcome& outcome = outcomes[secret_index];
        resolved.push_back(ResolvedSecret{
            request.secret_key_id,
            std::string_view(
                reinterpret_cast<const char*>(outcome.value.data()),
                outcome.value.size()),
            outcome.revision,
        });
        secret_index += 1;
    }
    bool app_requests_env_module = false;
    for (const std::string& module : app_request.modules) {
        if (module == "capsid:env") {
            app_requests_env_module = true;
            break;
        }
    }
    bool host_allows_env_module = false;
    for (const std::string& module : options->host_policy.module_allowlist) {
        if (module == "capsid:env") {
            host_allows_env_module = true;
            break;
        }
    }
    SecretSnapshotCompileInput snapshot_input;
    snapshot_input.application_id = options->application;
    snapshot_input.host_allows_env_module = host_allows_env_module;
    snapshot_input.app_requests_env_module = app_requests_env_module;
    snapshot_input.host_environment_names = host_names;
    snapshot_input.requests = requests;
    snapshot_input.resolved_secrets = resolved;
    const SecretSnapshotCompileResult snapshot =
        compile_secret_snapshot(snapshot_input);
    if (!snapshot.ok) {
        fail("secret snapshot rejected at " + snapshot.error.path +
             ": " + snapshot.error.message);
        return prepared;
    }
    const std::string snapshot_revision = snapshot.snapshot.secret_revision();
    for (const EnvironmentSnapshotEntry& entry : snapshot.snapshot.entries()) {
        prepared.env_values.push_back({ entry.name, entry.value });
    }
    // The policy compiler requires one resolved entry per env request,
    // literal or secret; build them from the compiled snapshot metadata.
    // Literal values are recovered from the request itself.
    std::vector<EffectiveEnvEntry> resolved_secrets;
    for (const EnvironmentSnapshotMetadata& meta : snapshot.snapshot.metadata()) {
        EffectiveEnvEntry resolved;
        resolved.name = meta.name;
        if (meta.source == EnvironmentValueSource::kSecret) {
            resolved.from_secret = true;
            resolved.secret_key_id = meta.secret_key_id;
            resolved.secret_revision = meta.opaque_revision;
        } else {
            for (const AppRequest::EnvRequest& request : app_request.env) {
                if (request.name == meta.name && !request.from_secret) {
                    resolved.literal = request.literal;
                    break;
                }
            }
        }
        resolved_secrets.push_back(std::move(resolved));
    }
    const PolicyCompileResult compiled =
        compile_policy(options->host_policy, app_request, resolved_secrets);
    if (!compiled.ok) {
        fail(compiled.error);
        return prepared;
    }
    prepared.effective = compiled.effective;

    // ---- 6. real generation identity (no placeholders) ----
    GenerationIdentityInput identity;
    identity.application_id = options->application;
    identity.source_digest = sha256_hex(std::string(
        artifacts.artifacts.bundle.bytes.begin(),
        artifacts.artifacts.bundle.bytes.end()));
    identity.bytecode_attestation_digest = attestation_digest;
    identity.selected_artifact = selected;
    identity.normalized_app_config_digest =
        compiled.effective.app_config_digest;
    identity.effective_policy_digest = compiled.effective.effective_digest;
    identity.effective_resource_digest = compiled.effective.effective_digest;
    identity.host_config_digest = host_policy_identity(options->host_policy);
    identity.secret_revision = snapshot_revision;
    identity.runtime_compatibility_id = options->runtime_compatibility_id;
    prepared.generation_digest = compute_generation_digest(identity);
    prepared.selected = selected;
    prepared.source_name = std::move(source_name);
    // Snapshot material: the raw source bundle (never the selected
    // artifact), the raw bytecode and the attestation claim + signature
    // when the version carried them. The committed generation keeps all of
    // this so recovery can re-verify without the upload boundary.
    prepared.source_bytes = artifacts.artifacts.bundle.bytes;
    prepared.bytecode_bytes = artifacts.artifacts.bytecode.bytes;
    prepared.attestation_json_text = std::move(attestation_json_text);
    prepared.signature_bytes = artifacts.artifacts.signature.bytes;
    prepared.attestation_digest = std::move(attestation_digest);
    prepared.capsid_json = capsid_json;
    // Metadata only: names, source kind, secret key IDs and opaque
    // revisions. Values never enter the committed generation.
    prepared.env_metadata_json =
        snapshot.snapshot.effective_environment_json();
    prepared.ok = true;
    return prepared;
}

bool env_metadata_equal(
    const std::vector<EnvironmentSnapshotMetadata>& left,
    const std::vector<EnvironmentSnapshotMetadata>& right);

// Result of the shared committed-generation validation. Recovery, the
// idempotent Version reuse path and the rename-EEXIST shared-generation
// path all run the SAME validation before reusing a generation, so the
// check strength cannot drift between the paths (a corrupt COMPLETE-marked
// object must never be reactivated or remapped).
struct ValidatedGeneration {
    bool ok = false;
    std::string error;  // static text
    SelectedArtifactKind selected = SelectedArtifactKind::kSource;
    std::string source_name;
    std::string bundle_bin;
    EffectiveConfig effective;
    std::vector<std::pair<std::string, std::string>> env_values;
};

// Validates a committed generation snapshot in full and rebuilds the warm
// material from it exclusively. The validation chain is fail-closed and
// identical for every caller:
//   1. generations/<generation>/COMPLETE exists (durable publish marker);
//   2. every snapshot file parses under its strict schema;
//   3. generation.json equals the expected content-addressed digest;
//   4. the artifact record's sourceName equals the name derived from the
//      trusted committed capsid.json + app/version (never trusted verbatim);
//   5. the committed capsid.json re-validates against the frozen schema and
//      re-parses;
//   6. the attestation re-verifies (trusted) or re-verifies as the same
//      compatibility fallback, the attestation document digest matches the
//      recorded digest, and for trusted bytecode the bytes actually loaded
//      (bundle.bin) equal the bytecode covered by the attestation;
//   7. the CURRENT secret provider re-resolves and the recomputed env
//      metadata equals the committed env.json (values never come from the
//      snapshot);
//   8. the effective policy recompiles under the CURRENT Host policy and
//      matches the committed effective.json byte for byte;
//   9. the full generation identity recomputes to the expected digest;
//  10. a source generation's bundle.bin is exactly its source.bin.
//
// The sourceName's embedded version segment is NOT compared to the calling
// operation's Version ID: content-addressed generations are shared across
// versions, so a generation first published by v1 and mapped by v2 is
// legitimately named ".../v1/...". The segment must instead be one of the
// versions whose immutable mapping points at this generation (the only
// trusted record of who named it); a drift to any other version is
// rejected, while the sharing scenarios stay valid.
ValidatedGeneration validate_committed_generation(
    int app_state_fd, const ManagedHostOptions& options,
    const std::string& generation) {
    ValidatedGeneration validated;
    const auto fail = [&](const std::string& message) {
        validated.ok = false;
        validated.error = message;
    };
    // COMPLETE alone is not proof (the marker can survive content damage);
    // it is the durable-publish precondition, then the full chain below
    // re-derives the content.
    if (!generation_is_complete(app_state_fd, generation)) {
        fail("committed generation is not complete");
        return validated;
    }
    std::string error;
    std::string bundle_bin;
    std::string source_bin;
    std::string bytecode_bin;
    std::string attestation_json;
    std::string signature_bytes;
    std::string effective_json;
    std::string env_json;
    std::string artifact_text;
    std::string capsid_json;
    std::string generation_text;
    {
        const int generations_fd =
            prepare_subdir_at(app_state_fd, "generations");
        if (generations_fd < 0) {
            fail("cannot open generations directory");
            return validated;
        }
        const int generation_fd =
            open_verified_subdir(generations_fd, generation.c_str());
        close(generations_fd);
        if (generation_fd < 0) {
            fail("cannot open committed generation");
            return validated;
        }
        // Required snapshot documents: every read must succeed under the
        // bounded regular-file contract (missing, FIFO/symlink, foreign
        // owner, oversized or concurrent-mutated files all fail the
        // generation). Per-file ceilings reuse the safe-read boundary
        // constants; generation.json stays bounded at 4 KiB.
        const ReadFileStatus bundle_status = read_file_at(
            generation_fd, "bundle.bin", kMaxArtifactFileBytes, &bundle_bin);
        const ReadFileStatus source_status = read_file_at(
            generation_fd, "source.bin", kMaxArtifactFileBytes, &source_bin);
        const ReadFileStatus effective_status = read_file_at(
            generation_fd, "effective.json", kMaxConfigFileBytes,
            &effective_json);
        // env.json is the committed metadata document (names, key IDs,
        // opaque revisions — never values); it is read under the separate
        // metadata ceiling, not the name/value Runtime budget.
        const ReadFileStatus env_status = read_file_at(
            generation_fd, "env.json", kMaxEnvironmentMetadataJsonBytes,
            &env_json);
        const ReadFileStatus artifact_status = read_file_at(
            generation_fd, "artifact.json", kMaxConfigFileBytes,
            &artifact_text);
        const ReadFileStatus capsid_status = read_file_at(
            generation_fd, "capsid.json", kMaxConfigFileBytes, &capsid_json);
        const ReadFileStatus generation_status = read_file_at(
            generation_fd, "generation.json", 4096, &generation_text);
        // Bytecode material is optional: present exactly when the version
        // carried bytecode + attestation. Only ENOENT is ignorable — a
        // FIFO, symlink, oversized or otherwise invalid optional file
        // fails the whole generation validation.
        const ReadFileStatus bytecode_status = read_file_at(
            generation_fd, "bytecode.bin", kMaxArtifactFileBytes,
            &bytecode_bin);
        const ReadFileStatus attestation_status = read_file_at(
            generation_fd, "attestation.json", kMaxAttestationFileBytes,
            &attestation_json);
        const ReadFileStatus signature_status = read_file_at(
            generation_fd, "bytecode.sig", kBytecodeSignatureBytes,
            &signature_bytes);
        close(generation_fd);
        const bool complete_snapshot =
            bundle_status == ReadFileStatus::kOk &&
            source_status == ReadFileStatus::kOk &&
            effective_status == ReadFileStatus::kOk &&
            env_status == ReadFileStatus::kOk &&
            artifact_status == ReadFileStatus::kOk &&
            capsid_status == ReadFileStatus::kOk &&
            generation_status == ReadFileStatus::kOk;
        const bool optional_snapshot_clean =
            bytecode_status != ReadFileStatus::kInvalid &&
            attestation_status != ReadFileStatus::kInvalid &&
            signature_status != ReadFileStatus::kInvalid;
        if (!complete_snapshot) {
            fail("committed generation snapshot is incomplete");
            return validated;
        }
        if (!optional_snapshot_clean) {
            fail("committed generation snapshot is invalid");
            return validated;
        }
    }
    // 3. The committed generation record must match the expected digest.
    std::string recorded_generation;
    if (!parse_generation_digest_document(generation_text,
                                          &recorded_generation) ||
        recorded_generation != generation) {
        fail("committed generation record mismatch");
        return validated;
    }
    // 4. The artifact record: selected kind, frozen source name and the
    // attestation digest carried by the deployment.
    SelectedArtifactKind selected = SelectedArtifactKind::kSource;
    std::string snapshot_source_name;
    std::string snapshot_attestation_digest;
    if (!parse_artifact_json(artifact_text, &selected, &snapshot_source_name,
                             &snapshot_attestation_digest, &error)) {
        fail(error);
        return validated;
    }
    // 5. The sourceName must follow the frozen derivation rule pinned to
    // the trusted committed config (entry / compile convention), with a
    // literal '/' separator before the artifact name — a mutable
    // artifact.json must not be able to rename the loaded artifact. The
    // embedded version segment must then be one of the versions whose
    // immutable mapping points at this generation: it is never compared to
    // the calling operation's Version ID, because a generation first
    // published by v1 and mapped by v2 is legitimately named by v1. Any
    // drift to a version that does not map here is rejected.
    std::string embedded_version;
    if (!parse_source_name_structure(options.application, capsid_json,
                                     !attestation_json.empty(),
                                     snapshot_source_name,
                                     &embedded_version)) {
        fail("committed source name mismatch");
        return validated;
    }
    std::set<std::string> mapped_versions;
    if (!collect_mapping_versions(app_state_fd, generation, &mapped_versions)) {
        fail("cannot read version mappings");
        return validated;
    }
    if (mapped_versions.find(embedded_version) == mapped_versions.end()) {
        fail("committed source name mismatch");
        return validated;
    }
    const std::string derived_source_name = snapshot_source_name;
    // 6. The committed capsid.json is re-validated against the frozen
    // schema and re-parsed, so the CURRENT Host policy and CURRENT secret
    // provider can be applied to it.
    const ConfigValidationResult validated_config = validate_config_json(
        ConfigDocument::kApplication, capsid_json);
    if (!validated_config.ok) {
        fail("committed config rejected at " +
             validated_config.error.path + ": " +
             validated_config.error.message);
        return validated;
    }
    AppRequest app_request;
    if (!parse_app_request(
            std::vector<std::uint8_t>(capsid_json.begin(), capsid_json.end()),
            &app_request, &error)) {
        fail(error);
        return validated;
    }
    // 7. Re-verify the attestation from snapshot material. A committed
    // trusted generation must re-verify as trusted AND the attestation
    // document itself must be exactly the one bound into the generation
    // (whitespace drift changes the file digest without changing any
    // signed claim), AND the bytes actually loaded (bundle.bin) must be
    // the bytecode the attestation covers. A committed fallback
    // (provenance-valid, compatibility-mismatched) must re-verify as the
    // same fallback with the same attestation digest; a source-only
    // generation must have carried no attestation at all.
    if (!attestation_json.empty()) {
        capsid::host::BytecodeAttestationInput input;
        input.source = std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(source_bin.data()),
            source_bin.size());
        input.bytecode = std::optional<std::span<const std::uint8_t>>(
            std::span<const std::uint8_t>(
                reinterpret_cast<const std::uint8_t*>(bytecode_bin.data()),
                bytecode_bin.size()));
        input.attestation_json = std::string_view(attestation_json);
        input.signature = std::optional<std::span<const std::uint8_t>>(
            std::span<const std::uint8_t>(
                reinterpret_cast<const std::uint8_t*>(signature_bytes.data()),
                signature_bytes.size()));
        input.expected_application = options.application;
        // The embedded version segment is the version the claims were
        // bound to (already proven to be a version mapped to this
        // generation above).
        input.expected_version = embedded_version;
        input.expected_source_name = derived_source_name;
        input.runtime_compatibility_id = options.runtime_compatibility_id;
        input.trusted_keys = std::span<const capsid::host::TrustedBytecodeKey>(
            options.trusted_keys.data(), options.trusted_keys.size());
        const capsid::host::BytecodeAttestationResult verified =
            capsid::host::verify_bytecode_attestation(input);
        if (selected == SelectedArtifactKind::kTrustedBytecode) {
            if (verified.selection !=
                    capsid::host::BytecodeArtifactSelection::kTrustedBytecode ||
                sha256_hex(bundle_bin) != sha256_hex(bytecode_bin)) {
                fail("committed trusted generation failed re-verification");
                return validated;
            }
            if (sha256_hex(attestation_json) != snapshot_attestation_digest) {
                fail("committed attestation digest mismatch");
                return validated;
            }
        } else if (verified.selection !=
                       capsid::host::BytecodeArtifactSelection::kSource ||
                   verified.code !=
                       capsid::host::BytecodeAttestationErrorCode::
                           kCompatibilityMismatch ||
                   sha256_hex(attestation_json) !=
                       snapshot_attestation_digest) {
            fail("committed fallback generation failed re-verification");
            return validated;
        }
    } else if (selected == SelectedArtifactKind::kTrustedBytecode ||
               !snapshot_attestation_digest.empty()) {
        fail("committed generation lost its attestation");
        return validated;
    }
    // 8. Re-resolve the CURRENT secrets and re-compile the environment
    // snapshot. Values never come from the committed generation: rotation
    // after commit must either re-validate against the current provider or
    // fail closed.
    bool has_secret_requests = false;
    for (const AppRequest::EnvRequest& request : app_request.env) {
        if (request.from_secret) {
            has_secret_requests = true;
            break;
        }
    }
    std::vector<SecretFileOutcome> outcomes;
    if (has_secret_requests) {
        if (options.secret_root_template_fd < 0) {
            fail("secret root not configured");
            return validated;
        }
        const int app_secret_fd = open_verified_app_secret_dir(
            options.secret_root_template_fd, options.application);
        if (app_secret_fd < 0) {
            fail("secret app directory unverified");
            return validated;
        }
        std::vector<std::string> key_ids;
        for (const AppRequest::EnvRequest& request : app_request.env) {
            if (request.from_secret) {
                key_ids.push_back(request.secret_key_id);
            }
        }
        outcomes = read_secret_files(app_secret_fd, key_ids);
        close(app_secret_fd);
    }
    std::vector<std::string_view> host_names;
    for (const std::string& pattern : options.host_policy.env_patterns) {
        host_names.push_back(pattern);
    }
    std::vector<EnvironmentRequest> requests;
    for (const AppRequest::EnvRequest& request : app_request.env) {
        EnvironmentRequest parsed;
        parsed.name = request.name;
        if (request.from_secret) {
            parsed.value_from = request.secret_key_id;
        } else {
            parsed.value = request.literal;
        }
        requests.push_back(parsed);
    }
    std::vector<ResolvedSecret> resolved;
    std::size_t secret_index = 0;
    for (const AppRequest::EnvRequest& request : app_request.env) {
        if (!request.from_secret) {
            continue;
        }
        if (secret_index >= outcomes.size() ||
            !outcomes[secret_index].error.empty()) {
            fail("secret resolution failed");
            return validated;
        }
        const SecretFileOutcome& resolved_outcome = outcomes[secret_index];
        resolved.push_back(ResolvedSecret{
            request.secret_key_id,
            std::string_view(
                reinterpret_cast<const char*>(resolved_outcome.value.data()),
                resolved_outcome.value.size()),
            resolved_outcome.revision,
        });
        secret_index += 1;
    }
    bool app_requests_env_module = false;
    for (const std::string& module : app_request.modules) {
        if (module == "capsid:env") {
            app_requests_env_module = true;
            break;
        }
    }
    bool host_allows_env_module = false;
    for (const std::string& module : options.host_policy.module_allowlist) {
        if (module == "capsid:env") {
            host_allows_env_module = true;
            break;
        }
    }
    SecretSnapshotCompileInput snapshot_input;
    snapshot_input.application_id = options.application;
    snapshot_input.host_allows_env_module = host_allows_env_module;
    snapshot_input.app_requests_env_module = app_requests_env_module;
    snapshot_input.host_environment_names = host_names;
    snapshot_input.requests = requests;
    snapshot_input.resolved_secrets = resolved;
    const SecretSnapshotCompileResult snapshot =
        compile_secret_snapshot(snapshot_input);
    if (!snapshot.ok) {
        fail("secret snapshot rejected at " + snapshot.error.path +
             ": " + snapshot.error.message);
        return validated;
    }
    // 9. The committed env metadata must match the recomputed snapshot
    // metadata (names, sources, key ids, revisions). The committed
    // generation carries no values, so this document is the record of what
    // the provider resolved at commit time.
    std::vector<EnvironmentSnapshotMetadata> committed_metadata;
    if (!parse_env_metadata(env_json, &committed_metadata, &error)) {
        fail(error);
        return validated;
    }
    if (!env_metadata_equal(snapshot.snapshot.metadata(),
                            committed_metadata)) {
        fail("committed environment metadata mismatch");
        return validated;
    }
    // 10. Recompile the effective policy under the CURRENT Host policy and
    // require it to match the committed effective.json byte for byte.
    std::vector<EffectiveEnvEntry> resolved_secrets;
    for (const EnvironmentSnapshotMetadata& meta : snapshot.snapshot.metadata()) {
        EffectiveEnvEntry resolved;
        resolved.name = meta.name;
        if (meta.source == EnvironmentValueSource::kSecret) {
            resolved.from_secret = true;
            resolved.secret_key_id = meta.secret_key_id;
            resolved.secret_revision = meta.opaque_revision;
        } else {
            for (const AppRequest::EnvRequest& request : app_request.env) {
                if (request.name == meta.name && !request.from_secret) {
                    resolved.literal = request.literal;
                    break;
                }
            }
        }
        resolved_secrets.push_back(std::move(resolved));
    }
    const PolicyCompileResult compiled =
        compile_policy(options.host_policy, app_request, resolved_secrets);
    if (!compiled.ok) {
        fail(compiled.error);
        return validated;
    }
    if (compiled.effective.effective_json != effective_json) {
        fail("committed effective config mismatch");
        return validated;
    }
    // 11. Recompute the complete generation identity from snapshot
    // material and the CURRENT authority; it must equal the committed
    // digest.
    GenerationIdentityInput identity;
    identity.application_id = options.application;
    identity.source_digest = sha256_hex(source_bin);
    identity.bytecode_attestation_digest = snapshot_attestation_digest;
    identity.selected_artifact = selected;
    identity.normalized_app_config_digest =
        compiled.effective.app_config_digest;
    identity.effective_policy_digest = compiled.effective.effective_digest;
    identity.effective_resource_digest = compiled.effective.effective_digest;
    identity.host_config_digest = host_policy_identity(options.host_policy);
    identity.secret_revision = snapshot.snapshot.secret_revision();
    identity.runtime_compatibility_id = options.runtime_compatibility_id;
    const std::string recomputed_digest = compute_generation_digest(identity);
    if (recomputed_digest != generation) {
        fail("committed generation identity mismatch");
        return validated;
    }
    // 12. The warmed artifact bytes must match the identity. For a source
    // generation bundle.bin is exactly the source bundle (a tampered
    // bundle.bin is caught here); for a trusted generation the attestation
    // re-verification above bound bundle.bin to the attested bytecode.
    if (selected == SelectedArtifactKind::kSource &&
        sha256_hex(bundle_bin) != sha256_hex(source_bin)) {
        fail("committed bundle tampered");
        return validated;
    }
    validated.ok = true;
    validated.selected = selected;
    validated.source_name = derived_source_name;
    validated.bundle_bin = std::move(bundle_bin);
    validated.effective = std::move(compiled.effective);
    // The env values come from the recomputed snapshot (the current
    // provider), never from committed metadata.
    for (const EnvironmentSnapshotEntry& entry : snapshot.snapshot.entries()) {
        validated.env_values.push_back({ entry.name, entry.value });
    }
    return validated;
}


// Internal deploy body. Every exit path leaves a terminal state in
// *status; the public wrapper records that terminal state in the
// operation registry (success and failure alike).
DeployOutcome run_deploy_operation(ManagedHostOptions* options,
                                   const std::string& version,
                                   OperationStatus* status) {
    DeployOutcome outcome;
    outcome.operation_id = unique_operation_id();
    status->operation_id = outcome.operation_id;
    status->version = version;
    status->state = OperationState::kValidating;

    // ---- 1-6. shared prepare pipeline: safe-read, artifact selection with
    // real attestation verification, authoritative schema + policy + secret
    // compilation, real generation identity ----
    const PreparedDeployment prepared = prepare_deployment(options, version, status);
    if (!prepared.ok) {
        outcome.error = prepared.error;
        return outcome;
    }
    // M2 item 7: §12.1 deploy family — validation completed; the remaining
    // stage events land on their success edges below (stage/activate).
    emit_deploy_stage(options->metrics, "validate", "ok",
                      options->application);
    emit_log(options->log, LogLane::kControl,
             {.event = log_events::kDeployStage,
              .app = options->application,
              .generation = prepared.generation_digest,
              .stage = "validate",
              .result = "ok"});
    const SelectedArtifactKind selected = prepared.selected;
    const std::string source_name = prepared.source_name;
    const EffectiveConfig& effective = prepared.effective;
    const std::vector<std::pair<std::string, std::string>>& env_values =
        prepared.env_values;
    const std::string& generation_digest = prepared.generation_digest;
    const std::vector<std::uint8_t>& bundle_bytes = prepared.bundle_bytes;

    // ---- 7. immutable version mapping gate ----
    // Version IDs are immutable once published: the same Version ID must
    // map to exactly one generation forever. A redeploy of the same
    // version resolves to the same generation (idempotent); different
    // content under the same Version ID is rejected before any staging.
    if (version.find('/') != std::string::npos) {
        outcome.error = "invalid version id";
        status->state = OperationState::kFailed;
        return outcome;
    }
    // stateRoot is opened exactly once per operation with O_NOFOLLOW and
    // verified; apps/<app> and staging descend from that single fd, so the
    // root cannot be swapped between the open and the state I/O (TOCTOU).
    // A symlink anywhere (apps, <app>, staging, generations) fails the
    // deploy instead of redirecting state I/O.
    std::string error;
    const int state_fd = open_verified_state_root(options->state_root);
    if (state_fd < 0) {
        outcome.error = "cannot open state root";
        status->state = OperationState::kFailed;
        return outcome;
    }
    const int app_state_fd = open_verified_app_state_dir(
        state_fd, options->application, /*create=*/true);
    if (app_state_fd < 0) {
        close(state_fd);
        outcome.error = "cannot prepare state directories";
        status->state = OperationState::kFailed;
        return outcome;
    }
    // Per-App mutex: the version gate, staging, commit, mapping write and
    // active-state persist form one read-modify-write sequence per App and
    // must not interleave with a concurrent deploy/retire/recover on the
    // same App. AppOperationLock pins a bounded per-App slot (§13.4).
    capsid::host::AppOperationLock app_lock(options->application);
    const int mapping =
        check_version_mapping(app_state_fd, version, generation_digest);
    if (mapping == -2) {
        close(app_state_fd);
        close(state_fd);
        outcome.error = "cannot read version mapping";
        status->state = OperationState::kFailed;
        return outcome;
    }
    if (mapping == 1) {
        // Same Version ID, different immutable content: reject and leave
        // the old active state untouched.
        close(app_state_fd);
        close(state_fd);
        outcome.error =
            "version id already published with different content";
        status->state = OperationState::kFailed;
        return outcome;
    }
    if (mapping == 0) {
        // Idempotent redeploy: the Version ID already maps to this
        // generation. The mapping and the COMPLETE marker are NOT proof
        // the object still holds its content — the committed generation is
        // fully re-validated through the same fail-closed chain as
        // recovery, and only then reused. active.json is rewritten with
        // identical bytes, so the active identity never changes.
        status->state = OperationState::kValidating;
        const ValidatedGeneration validated = validate_committed_generation(
            app_state_fd, *options, generation_digest);
        if (!validated.ok) {
            close(app_state_fd);
            close(state_fd);
            status->state = OperationState::kFailed;
            status->error = validated.error;
            outcome.error = validated.error;
            return outcome;
        }
        status->state = OperationState::kWarming;
        const std::vector<std::uint8_t> bundle(
            validated.bundle_bin.begin(), validated.bundle_bin.end());
        // §9.4: reserve the ENTIRE target pool count before any spawn. A
        // denied reserve (steady budget exhausted, or a zero-downtime
        // replace without surge/headroom) fails the deploy before a
        // single process starts. `replacement` is captured BEFORE the
        // reserve: a fresh reserve turns the App into a holder, which
        // must not flip the category of a later rollback.
        bool replacement = false;
        if (options->ledger != nullptr) {
            replacement = options->ledger->holds(options->application);
            const bool reserved =
                replacement
                    ? options->ledger->reserve_replace(
                          options->application, validated.effective.workers)
                    : options->ledger->reserve_fresh(
                          options->application, validated.effective.workers);
            if (!reserved) {
                close(app_state_fd);
                close(state_fd);
                status->state = OperationState::kFailed;
                status->error = "worker capacity exceeded";
                outcome.error = status->error;
                return outcome;
            }
        }
        const WarmPoolResult warm = warm_worker_pool(
            *options, bundle,
            validated.selected == SelectedArtifactKind::kTrustedBytecode,
            validated.source_name, generation_digest, validated.effective,
            validated.env_values);
        if (!warm.ok) {
            // The failed warm rolls its own reserve back.
            if (options->ledger != nullptr) {
                options->ledger->abort_reserve(
                    options->application, validated.effective.workers,
                    replacement);
            }
            close(app_state_fd);
            close(state_fd);
            status->state = OperationState::kFailed;
            status->error = warm.error;
            outcome.error = warm.error;
            return outcome;
        }
        close(state_fd);
        // PR-09c: the generation identity + replacement factory travel with
        // the outcome so the data plane can adopt a pool in place.
        outcome.version = version;
        outcome.generation_digest = generation_digest;
        outcome.generation_factory = make_generation_factory(
            *options, bundle,
            validated.selected == SelectedArtifactKind::kTrustedBytecode,
            validated.source_name, validated.effective, validated.env_values);
        finish_activation(options, &outcome, warm.workers, version,
                          generation_digest, app_state_fd, replacement,
                          status);
        close(app_state_fd);
        return outcome;
    }
    // mapping == -1: first publish of this Version ID; stage it.
    if (!make_dir_at(state_fd, "staging")) {
        close(app_state_fd);
        close(state_fd);
        outcome.error = "cannot prepare state directories";
        status->state = OperationState::kFailed;
        return outcome;
    }
    // The staging directory itself is reopened with O_NOFOLLOW and verified
    // (a pre-existing symlink at stateRoot/staging is rejected, not
    // followed), then the per-operation directory is created inside it.
    const int staging_root_fd = prepare_subdir_at(state_fd, "staging");
    if (staging_root_fd < 0) {
        close(app_state_fd);
        close(state_fd);
        outcome.error = "staging directory is not a verified directory";
        status->state = OperationState::kFailed;
        return outcome;
    }
    if (mkdirat(staging_root_fd, outcome.operation_id.c_str(), 0700) != 0) {
        close(staging_root_fd);
        close(app_state_fd);
        close(state_fd);
        outcome.error = "cannot create exclusive staging directory";
        status->state = OperationState::kFailed;
        return outcome;
    }
    // The exclusive operation directory is reopened with the same
    // owner/mode verification as every other level of the state chain.
    const int staging_fd =
        open_verified_subdir(staging_root_fd, outcome.operation_id.c_str());
    if (staging_fd < 0) {
        close(staging_root_fd);
        close(app_state_fd);
        close(state_fd);
        outcome.error = "cannot open staging directory";
        status->state = OperationState::kFailed;
        return outcome;
    }
    status->state = OperationState::kStaging;
    bool committed = false;
    // The committed generation is a complete internal snapshot: the
    // selected artifact, the raw source bundle, the raw bytecode, the
    // attestation claim + signature (when present), the effective config,
    // the worker env values and the artifact record. Recovery rebuilds
    // exclusively from these files; the upload version directory is never
    // consulted again.
    if (write_file_at(staging_fd, "bundle.bin",
                      std::string(bundle_bytes.begin(), bundle_bytes.end()),
                      &error) &&
        write_file_at(staging_fd, "source.bin",
                      std::string(prepared.source_bytes.begin(),
                                  prepared.source_bytes.end()),
                      &error) &&
        (prepared.bytecode_bytes.empty() ||
         write_file_at(staging_fd, "bytecode.bin",
                       std::string(prepared.bytecode_bytes.begin(),
                                   prepared.bytecode_bytes.end()),
                       &error)) &&
        (prepared.attestation_json_text.empty() ||
         write_file_at(staging_fd, "attestation.json",
                       prepared.attestation_json_text, &error)) &&
        (prepared.signature_bytes.empty() ||
         write_file_at(staging_fd, "bytecode.sig",
                       std::string(prepared.signature_bytes.begin(),
                                   prepared.signature_bytes.end()),
                       &error)) &&
        write_file_at(staging_fd, "effective.json",
                      effective.effective_json, &error) &&
        // The committed env document carries names, source kind, secret
        // key IDs and opaque revisions only. Literal and secret values
        // never enter the generation; recovery re-reads the current secret
        // provider and the literal values from the committed capsid.json.
        write_file_at(staging_fd, "env.json", prepared.env_metadata_json,
                      &error) &&
        write_file_at(staging_fd, "capsid.json", prepared.capsid_json,
                      &error) &&
        write_file_at(staging_fd, "artifact.json",
                      artifact_json(selected, source_name,
                                    prepared.attestation_digest,
                                    prepared.fallback_reason),
                      &error) &&
        write_file_at(staging_fd, "generation.json",
                      "{\"generation\":\"" + generation_digest + "\"}", &error) &&
        write_file_at(staging_fd, kCompleteMarker, "ok\n", &error)) {
        // fsync the staging dir, then rename into generations. Both
        // parents are the verified dirfd chain; a symlinked generations
        // directory fails prepare_subdir_at instead of redirecting the
        // rename. Identical content under a different Version ID produces
        // the same generation digest; a rename onto an already-published
        // generation is accepted only when that generation fully
        // re-validates through the same fail-closed chain as recovery
        // (content-addressed sharing must never reuse a corrupt object).
        const int generations_fd =
            prepare_subdir_at(app_state_fd, "generations");
        if (generations_fd < 0) {
            error = "cannot prepare generations directory";
        } else if (fsync(staging_fd) != 0) {
            error = "cannot sync the staged generation";
        } else {
            errno = 0;
            const int rename_result = renameat(
                staging_root_fd, outcome.operation_id.c_str(),
                generations_fd, generation_digest.c_str());
            bool shared_generation = false;
            bool shared_ok = false;
            if (rename_result != 0 &&
                (errno == EEXIST || errno == ENOTEMPTY)) {
                // "Target already exists" (implementations differ): the
                // generation is already published by a concurrent
                // operation. COMPLETE alone is not proof of intact
                // content; the object must fully re-validate before this
                // operation is allowed to share it (the shared validator
                // binds the sourceName segment to the mapped versions).
                shared_generation = true;
                const ValidatedGeneration shared =
                    validate_committed_generation(
                        app_state_fd, *options, generation_digest);
                shared_ok = shared.ok;
                if (!shared.ok) {
                    error = "cannot share an invalid generation";
                }
            }
            const bool accepted = rename_result == 0 ||
                                  (shared_generation && shared_ok);
            if (accepted && fsync(generations_fd) == 0) {
                committed = true;
            } else if (!accepted && error.empty()) {
                error = "cannot publish the generation";
            }
            if (committed && shared_generation) {
                // This operation's staging tree is unreferenced: the
                // generation it staged is already committed under its
                // content address. Remove the tree through the verified
                // dirfd chain so the shared-generation path never leaks
                // staging.
                remove_tree_at(staging_root_fd,
                               outcome.operation_id.c_str());
            }
        }
        if (generations_fd >= 0) {
            close(generations_fd);
        }
    }
    close(staging_fd);
    if (!committed) {
        // Cleanup of ONLY this operation's staging tree: every step is a
        // verified dirfd walk (O_NOFOLLOW) and only the exclusive
        // operation directory is touched, so a failed deploy can never
        // leak a non-empty staging tree or follow a planted symlink out
        // of stateRoot.
        remove_tree_at(staging_root_fd, outcome.operation_id.c_str());
        close(staging_root_fd);
        close(app_state_fd);
        close(state_fd);
        outcome.error = error.empty() ? "staging failed" : error;
        status->state = OperationState::kFailed;
        status->error = outcome.error;
        return outcome;
    }
    close(staging_root_fd);
    // M2 item 7: §12.1 deploy family — staging committed (generation
    // snapshot + COMPLETE marker + object rename all durable).
    emit_deploy_stage(options->metrics, "stage", "ok", options->application);
    emit_log(options->log, LogLane::kControl,
             {.event = log_events::kDeployStage,
              .app = options->application,
              .generation = generation_digest,
              .stage = "stage",
              .result = "ok"});
    // ---- 9. immutable version mapping, after the generation commit ----
    // Only a successfully published generation may freeze a Version ID.
    if (!write_version_mapping(app_state_fd, version, generation_digest,
                               &error)) {
        close(app_state_fd);
        close(state_fd);
        outcome.error = error.empty() ? "cannot record version mapping"
                                      : error;
        status->state = OperationState::kFailed;
        return outcome;
    }
    close(state_fd);

    // ---- 10-12. worker warm-up: the whole fixed pool must be READY.
    // NO Active is reported before the ENTIRE pool succeeds.
    // §9.4: reserve the ENTIRE target pool count before any spawn. A
    // denied reserve (steady budget exhausted, or a zero-downtime replace
    // without surge/headroom) fails the deploy before a single process
    // starts. `replacement` is captured BEFORE the reserve (a fresh
    // reserve turns the App into a holder — the rollback must not flip
    // its category).
    bool replacement = false;
    if (options->ledger != nullptr) {
        replacement = options->ledger->holds(options->application);
        const bool reserved =
            replacement
                ? options->ledger->reserve_replace(
                      options->application, effective.workers)
                : options->ledger->reserve_fresh(options->application,
                                                 effective.workers);
        if (!reserved) {
            close(app_state_fd);
            status->state = OperationState::kFailed;
            status->error = "worker capacity exceeded";
            outcome.error = status->error;
            return outcome;
        }
    }
    status->state = OperationState::kWarming;
    const WarmPoolResult warm = warm_worker_pool(
        *options, bundle_bytes, selected == SelectedArtifactKind::kTrustedBytecode,
        source_name, generation_digest, effective, env_values);
    if (!warm.ok) {
        // The failed warm rolls its own reserve back.
        if (options->ledger != nullptr) {
            options->ledger->abort_reserve(options->application,
                                           effective.workers, replacement);
        }
        close(app_state_fd);
        status->state = OperationState::kFailed;
        status->error = warm.error;
        outcome.error = warm.error;
        return outcome;
    }
    // PR-09c: the generation identity + replacement factory travel with
    // the outcome so the data plane can adopt a pool in place.
    outcome.version = version;
    outcome.generation_digest = generation_digest;
    outcome.generation_factory = make_generation_factory(
        *options, bundle_bytes,
        selected == SelectedArtifactKind::kTrustedBytecode, source_name,
        effective, env_values);
    finish_activation(options, &outcome, warm.workers, version,
                      generation_digest, app_state_fd, replacement,
                      status);
    close(app_state_fd);
    return outcome;
}

DeployOutcome managed_deploy(ManagedHostOptions* options,
                             const std::string& version,
                             OperationStatus* status) {
    DeployOutcome outcome;
    if (options == nullptr || options->applications_root_fd < 0 ||
        options->worker_path.empty() || version.empty()) {
        outcome.error = "invalid managed host arguments";
        return outcome;
    }
    outcome = run_deploy_operation(options, version, status);
    // Every operation records its terminal state (success or failure) in
    // the registry.
    record_operation(outcome.operation_id, *status);
    // M2 item 7: §12.1 deploy family — one terminal operation count per
    // deploy, whether it activated or failed anywhere in the chain.
    if (options->metrics != nullptr) {
        const bool ok = status->state == OperationState::kActive;
        options->metrics->count_deploy_operation(ok ? "ok" : "fail",
                                                 options->application);
        refresh_log_dropped(options->metrics, options->log,
                            options->application);
    }
    return outcome;
}

// Internal retire body; the public wrapper records the terminal state.
DeployOutcome run_retire_operation(ManagedHostOptions* options,
                                   OperationStatus* status) {
    DeployOutcome outcome;
    outcome.operation_id = unique_operation_id();
    status->operation_id = outcome.operation_id;
    // Read the current active document, then persist the retired tombstone
    // through the active-state API (previous fields carry the last active
    // generation). The state chain is entered through the verified
    // O_NOFOLLOW dirfd walk; a missing App state fails the retire.
    const int state_fd = open_verified_state_root(options->state_root);
    if (state_fd < 0) {
        outcome.error = "cannot open state root";
        status->state = OperationState::kFailed;
        return outcome;
    }
    const int app_state_fd = open_verified_app_state_dir(
        state_fd, options->application, /*create=*/false);
    close(state_fd);
    if (app_state_fd < 0) {
        outcome.error = "cannot open app state directory";
        status->state = OperationState::kFailed;
        return outcome;
    }
    // Per-App mutex: the retire read-modify-write must not interleave with
    // a concurrent deploy/recover on the same App.
    capsid::host::AppOperationLock app_lock(options->application);
    PosixActiveStateFilesystem filesystem(app_state_fd);
    const ActiveStateReadResult current = filesystem.read_active_file();
    ActiveStateDocument document;
    document.state = ActiveServiceState::kRetired;
    document.application = options->application;
    if (current.status == ActiveStateIoStatus::kOk) {
        const ActiveStateDocumentResult parsed =
            parse_active_state_json(options->application, current.bytes);
        if (parsed.ok) {
            if (parsed.document.state == ActiveServiceState::kRetired) {
                // Idempotent retire: the tombstone already carries the
                // previous generation; re-persist exactly that so repeated
                // retires never mutate the canonical tombstone.
                document.previous_version = parsed.document.previous_version;
                document.previous_generation =
                    parsed.document.previous_generation;
            } else {
                document.previous_version = parsed.document.version;
                document.previous_generation = parsed.document.generation;
            }
        }
    }
    // §9.3 retire transaction: prepare_retire before the tombstone
    // persist (may fail), commit_retire after it (noexcept — drain
    // signal + ledger category switch), abort_retire when it failed.
    std::unique_ptr<RetirePlan> plan;
    if (options->prepare_retire != nullptr) {
        if (options->commit_retire == nullptr ||
            options->abort_retire == nullptr) {
            outcome.error = "retire transaction misconfigured";
            status->state = OperationState::kFailed;
            return outcome;
        }
        std::string plan_error;
        plan = options->prepare_retire(options->application, &plan_error);
        if (plan == nullptr) {
            outcome.error =
                plan_error.empty() ? "cannot prepare the retire" : plan_error;
            status->state = OperationState::kFailed;
            return outcome;
        }
    }
    const ActiveStatePersistResult persisted = persist_active_state(
        document, outcome.operation_id, filesystem);
    if (!persisted.ok) {
        if (plan != nullptr) {
            options->abort_retire(plan.get());
        }
        outcome.error = "cannot persist retire tombstone";
        status->state = OperationState::kFailed;
        return outcome;
    }
    if (plan != nullptr) {
        options->commit_retire(plan.get());
    }
    // §9.4: the serving pool leaves steady for surge (draining); its
    // count is released when the reaper finished (the pool's
    // on_drain_complete hook).
    if (options->ledger != nullptr) {
        options->ledger->begin_retire(
            options->application, options->ledger->steady_of(options->application));
    }
    status->state = OperationState::kActive;
    outcome.ok = true;
    // M2 item 7: §12.1 recovery family — a retire is a terminal recovery
    // decision, recorded only when the tombstone is durable.
    if (options->metrics != nullptr) {
        options->metrics->count_recovery_retire(options->application);
        refresh_log_dropped(options->metrics, options->log,
                            options->application);
    }
    emit_log(options->log, LogLane::kControl,
             {.event = log_events::kRetire, .app = options->application,
              .result = "ok"});
    return outcome;
}

DeployOutcome managed_retire(ManagedHostOptions* options,
                             OperationStatus* status) {
    DeployOutcome outcome;
    if (options == nullptr || options->state_root.empty()) {
        outcome.error = "invalid managed host arguments";
        return outcome;
    }
    outcome = run_retire_operation(options, status);
    record_operation(outcome.operation_id, *status);
    return outcome;
}

// Internal quarantine body; the public wrapper records the terminal state.
// The tombstone keeps the CURRENT active document's version and generation
// (the quarantined generation) and marks the reason CRASH_BUDGET_EXCEEDED,
// mirroring the retire tombstone path: same verified dirfd walk, same
// per-App mutex, same persist chain. Idempotent: an already-quarantined
// or retired App is a no-op success.
DeployOutcome run_quarantine_operation(ManagedHostOptions* options,
                                       OperationStatus* status) {
    DeployOutcome outcome;
    outcome.operation_id = unique_operation_id();
    status->operation_id = outcome.operation_id;
    const int state_fd = open_verified_state_root(options->state_root);
    if (state_fd < 0) {
        outcome.error = "cannot open state root";
        status->state = OperationState::kFailed;
        return outcome;
    }
    const int app_state_fd = open_verified_app_state_dir(
        state_fd, options->application, /*create=*/false);
    close(state_fd);
    if (app_state_fd < 0) {
        outcome.error = "cannot open app state directory";
        status->state = OperationState::kFailed;
        return outcome;
    }
    // Per-App mutex: the quarantine read-modify-write must not interleave
    // with a concurrent deploy/retire/recover on the same App.
    capsid::host::AppOperationLock app_lock(options->application);
    PosixActiveStateFilesystem filesystem(app_state_fd);
    const ActiveStateReadResult current = filesystem.read_active_file();
    if (current.status == ActiveStateIoStatus::kNotFound) {
        // No durable document at all: nothing to quarantine.
        outcome.ok = true;
        status->state = OperationState::kActive;
        return outcome;
    }
    if (current.status != ActiveStateIoStatus::kOk) {
        outcome.error = "cannot read active state";
        status->state = OperationState::kFailed;
        return outcome;
    }
    const ActiveStateDocumentResult parsed =
        parse_active_state_json(options->application, current.bytes);
    if (!parsed.ok) {
        outcome.error = "cannot parse active state";
        status->state = OperationState::kFailed;
        return outcome;
    }
    if (parsed.document.state != ActiveServiceState::kActive) {
        // Already quarantined or retired: the App is already not serving.
        outcome.ok = true;
        status->state = OperationState::kActive;
        return outcome;
    }
    ActiveStateDocument document;
    document.state = ActiveServiceState::kQuarantined;
    document.application = options->application;
    document.version = parsed.document.version;
    document.generation = parsed.document.generation;
    document.reason = std::string(kCrashBudgetExceededReason);
    const ActiveStatePersistResult persisted = persist_active_state(
        document, outcome.operation_id, filesystem);
    if (!persisted.ok) {
        outcome.error = "cannot persist quarantine tombstone";
        status->state = OperationState::kFailed;
        return outcome;
    }
    status->state = OperationState::kActive;
    outcome.ok = true;
    // M2 item 7: §12.1 recovery family — the coordinator-level quarantine
    // (crash-budget tombstone) mirrors the supervisor-level one.
    if (options->metrics != nullptr) {
        options->metrics->count_recovery_quarantine(options->application);
        refresh_log_dropped(options->metrics, options->log,
                            options->application);
    }
    emit_log(options->log, LogLane::kControl,
             {.event = log_events::kQuarantine, .app = options->application,
              .result = "ok"});
    return outcome;
}

DeployOutcome managed_quarantine(ManagedHostOptions* options,
                                 OperationStatus* status) {
    DeployOutcome outcome;
    if (options == nullptr || options->state_root.empty()) {
        outcome.error = "invalid managed host arguments";
        return outcome;
    }
    outcome = run_quarantine_operation(options, status);
    record_operation(outcome.operation_id, *status);
    return outcome;
}

ManagedLifecycleSnapshot managed_read_lifecycle(ManagedHostOptions* options) {
    ManagedLifecycleSnapshot snapshot;
    if (options == nullptr) {
        return snapshot;
    }
    const int state_fd = open_verified_state_root(options->state_root);
    if (state_fd < 0) {
        return snapshot;
    }
    const int app_state_fd = open_verified_app_state_dir(
        state_fd, options->application, /*create=*/false);
    close(state_fd);
    if (app_state_fd < 0) {
        return snapshot;
    }
    // The strict recovery rules own the read: missing active.json is a
    // successful kNone, retired/quarantined never inspect a generation,
    // and an active document activates only when its generation is
    // COMPLETE (the same fail-closed authority boot recovery uses).
    PosixActiveStateFilesystem filesystem(app_state_fd);
    const ActiveStateRecoveryResult recovered =
        recover_active_state(options->application, filesystem);
    if (!recovered.ok) {
        return snapshot;
    }
    snapshot.ok = true;
    switch (recovered.action) {
    case ActiveStateRecoveryAction::kActivate:
        snapshot.state.phase = ServiceLifecyclePhase::kActive;
        snapshot.state.document = recovered.document;
        break;
    case ActiveStateRecoveryAction::kKeepRetired:
        snapshot.state.phase = ServiceLifecyclePhase::kRetired;
        snapshot.state.document = recovered.document;
        break;
    case ActiveStateRecoveryAction::kKeepQuarantined:
        snapshot.state.phase = ServiceLifecyclePhase::kQuarantined;
        snapshot.state.document = recovered.document;
        break;
    case ActiveStateRecoveryAction::kNone:
        snapshot.state.phase = ServiceLifecyclePhase::kAbsent;
        break;
    }
    return snapshot;
}

// M2 item 6: read the active health probe config (design §7.4) from the
// COMMITTED generation's capsid.json — the same verified dirfd walk and
// bounded read boot recovery uses, so the supervisor re-reads it on every
// re-anchor and a redeployed healthCheck takes effect with the new
// generation. Fails closed: any read or parse failure yields
// configured=false (the App keeps passive signals only) with a diagnostic
// line — a probe is never fabricated from an unreadable document.
HealthCheckConfig managed_read_health_check(ManagedHostOptions* options,
                                            const std::string& generation) {
    HealthCheckConfig result;
    if (options == nullptr) {
        return result;
    }
    const int state_fd = open_verified_state_root(options->state_root);
    if (state_fd < 0) {
        return result;
    }
    const int app_state_fd = open_verified_app_state_dir(
        state_fd, options->application, /*create=*/false);
    close(state_fd);
    if (app_state_fd < 0) {
        return result;
    }
    const int generations_fd =
        prepare_subdir_at(app_state_fd, "generations");
    close(app_state_fd);
    if (generations_fd < 0) {
        return result;
    }
    const int generation_fd =
        open_verified_subdir(generations_fd, generation.c_str());
    close(generations_fd);
    if (generation_fd < 0) {
        return result;
    }
    std::string capsid_json;
    const ReadFileStatus status = read_file_at(
        generation_fd, "capsid.json", kMaxConfigFileBytes, &capsid_json);
    close(generation_fd);
    if (status != ReadFileStatus::kOk) {
        // M2 item 7: §12.2 control-plane event — a committed health check
        // that cannot be read means the App is silently passive; never
        // fabricate a probe from an unreadable document.
        emit_log(options->log, LogLane::kControl,
                 {.event = log_events::kHealthProbe,
                  .app = options->application,
                  .generation = generation,
                  .result = "error",
                  .message = "cannot read committed health check; "
                             "passive signals only"});
        return result;
    }
    AppRequest app;
    std::string error;
    if (!parse_app_request(
            std::vector<std::uint8_t>(capsid_json.begin(),
                                      capsid_json.end()),
            &app, &error)) {
        emit_log(options->log, LogLane::kControl,
                 {.event = log_events::kHealthProbe,
                  .app = options->application,
                  .generation = generation,
                  .result = "error",
                  .message = "cannot parse committed health check; "
                             "passive signals only"});
        return result;
    }
    return app.health_check;
}

// Compares two environment metadata records (name, source kind, secret key
// id, opaque revision) in lockstep. Both sides are compiler-sorted by name,
// so order is a stable part of the comparison.
bool env_metadata_equal(
    const std::vector<EnvironmentSnapshotMetadata>& left,
    const std::vector<EnvironmentSnapshotMetadata>& right) {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index) {
        const EnvironmentSnapshotMetadata& a = left[index];
        const EnvironmentSnapshotMetadata& b = right[index];
        if (a.name != b.name || a.source != b.source ||
            a.secret_key_id != b.secret_key_id ||
            a.opaque_revision != b.opaque_revision) {
            return false;
        }
    }
    return true;
}

// Internal recovery body; the public wrapper records the terminal state.
DeployOutcome run_recover_operation(ManagedHostOptions* options,
                                    OperationStatus* status) {
    DeployOutcome outcome;
    outcome.operation_id = unique_operation_id();
    status->operation_id = outcome.operation_id;
    const int state_fd = open_verified_state_root(options->state_root);
    if (state_fd < 0) {
        if (errno == ENOENT) {
            // No stateRoot at all: no active App; never scan for one.
            outcome.ok = true;
            return outcome;
        }
        outcome.error = "cannot open state root";
        status->state = OperationState::kFailed;
        return outcome;
    }
    const int app_state_fd = open_verified_app_state_dir(
        state_fd, options->application, /*create=*/false);
    close(state_fd);
    if (app_state_fd < 0) {
        if (errno == ENOENT) {
            // No App state directory at all: no active App; never scan
            // generations for one.
            outcome.ok = true;
            return outcome;
        }
        outcome.error = "cannot open app state directory";
        status->state = OperationState::kFailed;
        return outcome;
    }
    // Per-App mutex: recovery's read/rebuild/activate sequence must not
    // interleave with a concurrent deploy/retire on the same App.
    capsid::host::AppOperationLock app_lock(options->application);
    // The active-state recovery flow owns the read/parse/decision: it
    // cleans stale active.json.tmp.* first (best effort), then maps the
    // document to an action (activate only when the generation is
    // COMPLETE; missing active.json is a no-op; retired/quarantined never
    // start a worker).
    PosixActiveStateFilesystem filesystem(app_state_fd);
    const ActiveStateRecoveryResult recovered =
        recover_active_state(options->application, filesystem);
    if (!recovered.ok) {
        outcome.error = "active state recovery failed";
        status->state = OperationState::kFailed;
        status->error = outcome.error;
        return outcome;
    }
    if (recovered.action != ActiveStateRecoveryAction::kActivate) {
        // No active App, or retired/quarantined: do not start a worker.
        outcome.ok = true;
        return outcome;
    }
    status->version = recovered.document.version;
    const std::string& active_generation = recovered.document.generation;
    // ---- rebuild exclusively from the committed generation snapshot ----
    // The upload version directory is an upload boundary, not a recovery
    // source. The snapshot is re-validated in full: strict parsing of every
    // committed document, re-verification of the attestation, re-reading
    // the CURRENT secret provider, and re-compilation of the effective
    // policy under the CURRENT Host policy. The complete generation
    // identity is recomputed and must match the committed digest; any drift
    // (tampered bundle, revoked Host policy, rotated secrets) fails closed
    // and never activates a worker.
    // The committed generation is re-validated in full through the same
    // fail-closed chain used by idempotent redeploy and shared-generation
    // reuse: every snapshot document, the attestation (with the document
    // digest and bundle binding), the CURRENT secret provider, the CURRENT
    // Host policy recompilation, and the recomputed generation identity.
    // Any drift fails closed and never activates a worker.
    status->state = OperationState::kValidating;
    const ValidatedGeneration validated = validate_committed_generation(
        app_state_fd, *options, active_generation);
    if (!validated.ok) {
        outcome.error = validated.error;
        status->state = OperationState::kFailed;
        status->error = outcome.error;
        return outcome;
    }
    status->state = OperationState::kWarming;
    const std::vector<std::uint8_t> bundle(
        validated.bundle_bin.begin(), validated.bundle_bin.end());
    // §9.4: reserve the recovered fleet's count BEFORE any spawn. At
    // startup nothing holds, so this is always a fresh reserve; a denied
    // reserve fails the Host closed before a single process starts
    // (an active-generation count beyond capacity never overspawns).
    if (options->ledger != nullptr &&
        !options->ledger->reserve_fresh(options->application,
                                        validated.effective.workers)) {
        status->state = OperationState::kFailed;
        status->error = "worker capacity exceeded";
        outcome.error = status->error;
        return outcome;
    }
    const WarmPoolResult warm = warm_worker_pool(
        *options, bundle,
        validated.selected == SelectedArtifactKind::kTrustedBytecode,
        validated.source_name, active_generation, validated.effective,
        validated.env_values);
    if (!warm.ok) {
        if (options->ledger != nullptr) {
            options->ledger->abort_reserve(
                options->application, validated.effective.workers,
                /*replacement=*/false);
        }
        status->state = OperationState::kFailed;
        status->error = warm.error;
        outcome.error = warm.error;
        return outcome;
    }
    status->state = OperationState::kActive;
    outcome.ok = true;
    // PR-09c: the generation identity + replacement factory travel with
    // the outcome so the data plane can adopt a pool in place.
    outcome.version = recovered.document.version;
    outcome.generation_digest = active_generation;
    outcome.generation_factory = make_generation_factory(
        *options, bundle,
        validated.selected == SelectedArtifactKind::kTrustedBytecode,
        validated.source_name, validated.effective, validated.env_values);
    publish_pool(&outcome, warm.workers);
    return outcome;
}

DeployOutcome managed_recover(ManagedHostOptions* options,
                              OperationStatus* status) {
    DeployOutcome outcome;
    if (options == nullptr || options->state_root.empty()) {
        outcome.error = "invalid managed host arguments";
        return outcome;
    }
    outcome = run_recover_operation(options, status);
    record_operation(outcome.operation_id, *status);
    return outcome;
}

OperationStatus managed_operation_status(
    const ManagedHostOptions& options,
    const std::string& operation_id) {
    (void) options;
    OperationStatus status;
    status.operation_id = operation_id;
    status.state = OperationState::kFailed;
    status.error = "operation not found";
    lookup_operation(operation_id, &status);
    return status;
}

std::string managed_app_status(const ManagedHostOptions& options) {
    // Every result for a configured App carries the canonical "app" field,
    // including the inactive shapes, so the Admin boundary can verify the
    // document belongs to the routed App.
    const std::string inactive =
        "{\"active\":false,\"app\":\"" +
        json_escape(options.application) + "\"}";
    const int state_fd = open_verified_state_root(options.state_root);
    if (state_fd < 0) {
        return inactive;
    }
    const int app_state_fd = open_verified_app_state_dir(
        state_fd, options.application, /*create=*/false);
    close(state_fd);
    if (app_state_fd < 0) {
        return inactive;
    }
    PosixActiveStateFilesystem filesystem(app_state_fd);
    const ActiveStateReadResult current = filesystem.read_active_file();
    if (current.status != ActiveStateIoStatus::kOk) {
        return inactive;
    }
    const ActiveStateDocumentResult parsed =
        parse_active_state_json(options.application, current.bytes);
    if (!parsed.ok) {
        return inactive;
    }
    std::ostringstream out;
    out << "{\"active\":" << (parsed.document.state ==
                                      ActiveServiceState::kActive
                                  ? "true"
                                  : "false")
        << ",\"app\":\"" << json_escape(options.application)
        << "\",\"version\":\"" << json_escape(parsed.document.version)
        << "\",\"generation\":\"" << json_escape(parsed.document.generation)
        << "\"}";
    return out.str();
}

}  // namespace capsid::host
