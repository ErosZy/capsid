#include "worker_runtime.h"

#include "build_identity.h"
#include "capability_policy.h"
#include <array>
#include <cstdint>

#include "egress_policy.h"
#include "ipc_validation.h"
#include "outbound_buffer.h"
#include "protocol.h"
#include "sandbox.h"
#include "capsid/runtime.h"

extern "C" {
#include "tjs.h"
#include "utils.h"
#include "uv.h"

int capsid_tjs_set_ca_bundle_path(TJSRuntime *runtime, const char *path);
int capsid_tjs_set_cookie_jar_path(TJSRuntime *runtime, const char *path);
int capsid_tjs_set_egress_policy(
    TJSRuntime *runtime,
    int (*check)(void *opaque,
                 const char *host,
                 uint16_t port,
                 const struct sockaddr *address,
                 socklen_t address_len,
                 char *reason,
                 size_t reason_size),
    void *opaque);
JSModuleDef *tjs_module_loader(
    JSContext *ctx,
    const char *module_name,
    void *opaque,
    JSValueConst attributes);
JSModuleDef *tjs__load_builtin(
    JSContext *ctx,
    const char *module_name);
}

#include <errno.h>
#if defined(_WIN32)
#include "win32_compat.h"
#else
#include <dirent.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>
#endif
#if defined(__linux__)
#include <linux/openat2.h>
#include <sys/syscall.h>
#endif

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

extern "C" {
extern const uint8_t capsid__bootstrap[];
extern const uint32_t capsid__bootstrap_size;
}

namespace {

// Requests whose response has ended keep at most this many terminal
// tombstones (see remember_terminal). The Host tracks at most
// config_.max_inflight requests at once and never reuses an id, so the
// oldest tombstone is always for a request whose late frames have long
// since drained.
static const size_t kMaxTerminalTombstones = 2048;

// §7.4: a poisoned worker gets a strictly bounded cleanup window. At the
// poison deadline the worker exits unconditionally (flush + stop), so a
// detached resource or leaked continuation cannot hold capacity forever.
static const uint64_t kPoisonGraceNs = 100 * 1000000ull;  // 100ms

// §7.4: when a response is already gone yet a non-terminal token still
// holds refs, the JS chain may still be live — parked on a uv-loop timer
// that will fire and settle it inertly (the timeout/error paths skip
// capsidRequestSettled, so no settle signal arrives natively). Reclaim
// defers poison while pending JS work exists, but only for this bounded
// window: a timer that never settles (or a perpetual interval) must not
// pin the worker in deferral forever — after the window the token is
// treated as a detached continuation and the worker poisons.
static const uint64_t kReclaimSettleWindowNs = 2 * 1000000000ull;  // 2s

ssize_t write_socket(int fd, const uint8_t *data, size_t size) {
#if defined(_WIN32)
    // Winsock send() takes the raw SOCKET handle, not the CRT fd.
    ssize_t sent = capsid::win32::send_fd(fd, data, size, 0);
    {
        FILE *dbg = std::fopen("E:/capsid/build-win/worker-debug.log", "ab");
        if (dbg != NULL) {
            uint16_t frame_type = 0;
            if (size >= 8) {
                frame_type = static_cast<uint16_t>(data[6]) |
                             static_cast<uint16_t>(data[7] << 8);
            }
            std::fprintf(dbg, "write_socket: fd=%d size=%lld sent=%lld errno=%d wsa=%d type=%u\n",
                         fd,
                         static_cast<long long>(size),
                         static_cast<long long>(sent),
                         errno,
                         WSAGetLastError(),
                         static_cast<unsigned>(frame_type));
            std::fclose(dbg);
        }
    }
    if (sent < 0 && WSAGetLastError() == 183) {
        // debug probe: is ERROR_ALREADY_EXISTS transient on retry?
        Sleep(250);
        const ssize_t retried = capsid::win32::send_fd(fd, data, size, 0);
        const uint8_t probe_bytes[4] = {0x50, 0x49, 0x4e, 0x47};
        const ssize_t probe_sent = capsid::win32::send_fd(fd, probe_bytes, 4, 0);
        const SOCKET probe_handle = static_cast<SOCKET>(_get_osfhandle(fd));
        struct sockaddr_in peer_address = {};
        int peer_size = sizeof(peer_address);
        const int peer_result = getpeername(
            probe_handle,
            reinterpret_cast<struct sockaddr *>(&peer_address),
            &peer_size);
        int so_error = 0;
        int so_error_size = sizeof(so_error);
        const int so_result = getsockopt(
            probe_handle, SOL_SOCKET, SO_ERROR,
            reinterpret_cast<char *>(&so_error), &so_error_size);
        FILE *dbg = std::fopen("E:/capsid/build-win/worker-debug.log", "ab");
        if (dbg != NULL) {
            std::fprintf(dbg,
                         "write_socket retry: sent=%lld errno=%d wsa=%d "
                         "getpeername=%d(peer_err=%d) SO_ERROR=%d(so_err=%d) "
                         "probe4=%lld(probe_errno=%d probe_wsa=%d)\n",
                         static_cast<long long>(retried),
                         errno,
                         WSAGetLastError(),
                         peer_result,
                         peer_result == 0 ? 0 : WSAGetLastError(),
                         so_error,
                         so_result == 0 ? 0 : WSAGetLastError(),
                         static_cast<long long>(probe_sent),
                         errno,
                         WSAGetLastError());
            std::fclose(dbg);
        }
        sent = retried;
    }
    return sent;
#elif defined(MSG_NOSIGNAL)
    return send(fd, data, size, MSG_NOSIGNAL);
#else
    return send(fd, data, size, 0);
#endif
}

typedef capsid::WorkerStartupConfig WorkerConfig;

/*
 * Preserve short responses and the first 64 KiB of long streams exactly.
 * Profiling showed that earlier coalescing regressed 16/64 KiB L2 latency,
 * while bounded 16 KiB batches after this prefix reduce host-side frame/CGo
 * amplification for 128 KiB streams. These are worker-private copies: IPC
 * credit is charged when bytes are accepted and the public wire protocol is
 * unchanged.
 */
const size_t kStorageNamespaceQuota = 64u * 1024u;
const size_t kStorageEntryLimit = 256u;
const size_t kStorageKeyLimit = 256u;
const size_t kStorageValueLimit = 16u * 1024u;
const size_t kStdioMessageLimit = 16u * 1024u;
const size_t kFsFileLimit = 1024u * 1024u;
const size_t kFsDirectoryEntryLimit = 1024u;

bool is_utility_module(const char *name) {
    static const char *const modules[] = {
        "capsid:assert",
        "capsid:getopts",
        "capsid:hashing",
        "capsid:ipaddr",
        "capsid:utils",
        "capsid:uuid"
    };
    if (!name) {
        return false;
    }
    for (size_t index = 0;
         index < sizeof(modules) / sizeof(modules[0]);
         ++index) {
        if (std::strcmp(name, modules[index]) == 0) {
            return true;
        }
    }
    return false;
}

const char *utility_implementation_module(
    const char *name) {
    struct Mapping {
        const char *public_name;
        const char *implementation_name;
    };
    static const Mapping mappings[] = {
        { "capsid:assert", "tjs:assert" },
        { "capsid:getopts", "tjs:getopts" },
        { "capsid:hashing", "tjs:hashing" },
        { "capsid:ipaddr", "tjs:ipaddr" },
        { "capsid:utils", "tjs:utils" },
        { "capsid:uuid", "tjs:uuid" }
    };
    for (size_t index = 0;
         index < sizeof(mappings) / sizeof(mappings[0]);
         ++index) {
        if (name &&
            std::strcmp(
                name,
                mappings[index].public_name) == 0) {
            return mappings[index].implementation_name;
        }
    }
    return NULL;
}

// Result of a response-body enqueue (design §3.1).
enum class EnqueueResult {
    kQueued,     // all bytes entered the wire queue
    kWouldBlock, // no credit / no wire space: promise stays pending
    kFatal,      // protocol/state inconsistency: fail closed
};

struct PendingWrite {
    // Call-time snapshot of the unsent remainder (design §3.2): the
    // bytes are copied into native memory at the write call, so the
    // application mutating the source array afterwards cannot change
    // the response, and the pump reads the copy directly without
    // repeated JS API calls. The copy is bounded by one frame.
    std::vector<uint8_t> data;
    size_t offset;  // bytes already written to the wire queue
    size_t size;
    JSValue resolve;
    JSValue reject;

    PendingWrite()
        : offset(0),
          size(0),
          resolve(JS_UNDEFINED),
          reject(JS_UNDEFINED) {}
};

// A terminal (ResponseEnd / Error) that could not enter the wire queue
// yet. Bounded metadata only; body bytes are discarded for errors.
struct TerminalPending {
    enum class Kind { kResponseEnd, kResponseError };
    Kind kind;
    std::string message;
    uint32_t error_flags;
};

// Response lifecycle (design §3.5): explicit phases so the timeout and
// terminal paths execute exactly once even while the wire queue stays
// saturated.
enum class ResponsePhase {
    kOpen,            // normal processing, timeout armed
    kEndPending,      // ResponseEnd deferred (queue full / body draining);
                      // the deadline stays armed — the host may stall and
                      // the request must still time out
    kFailurePending,  // error/timeout terminal deferred; deadline disarmed
};

// WP-02 §6.2: request identity token, captured by the QuickJS job-context
// hooks so every Promise reaction of a request carries that request's
// identity. refs: 1 from the registry owner, 1 while a ResponseState owns
// it, +1 per captured job. Freed only when the bootstrap chain settles and
// the post-drain reclaim drops the last (registry) ref.
struct RequestToken {
    uint64_t generation;
    uint64_t request_id;
    uint64_t deadline_ns;
    bool terminal;
    int refs;
    // §7.4: refs observed by the previous reclaim round. A chain that is
    // still unwinding after a cancel drops refs round over round; the
    // reclaim judges a candidate token against this baseline so it never
    // poisons in the gap between the chain's jobs (no pending job, refs
    // momentarily stable). A refs count that stops falling is a detached
    // continuation.
    int last_reclaim_refs_;
    // The cancel path sets this for one round: the cancel's own drain
    // runs the chain to completion, but the completed chain's captured
    // refs release only when a later GC round collects it (promise
    // finalizers are two-phase). The first reclaim after a cancel defers
    // unconditionally; poison decisions resume on the next round.
    bool reclaim_grace;

    RequestToken(uint64_t gen, uint64_t id, uint64_t deadline)
        : generation(gen),
          request_id(id),
          deadline_ns(deadline),
          terminal(false),
          refs(1),
          last_reclaim_refs_(0),
          reclaim_grace(false) {}
};

struct ResponseState {
    uint64_t credit;
    uint64_t request_credit;
    uint64_t response_body_bytes_accepted;
    uint64_t deadline_ns;
    bool request_ended;
    std::deque<PendingWrite> pending;
    // C++11 target: no std::optional; the flag marks a deferred terminal.
    TerminalPending terminal;
    bool terminal_pending;
    ResponsePhase phase;
    // WP-02 §6.2: owner ref on the request token. The token outlives the
    // response (the JS chain settles after the response ends), so the
    // ref is released when this entry is erased, never earlier.
    RequestToken *token;
    // Diagnostic timestamps (performance loop; zero cost when idle).
    uint64_t t_begin_ns;
    uint64_t t_head_ns;
    uint64_t t_write_done_ns;

    ResponseState()
        : credit(0),
          request_credit(0),
          response_body_bytes_accepted(0),
          deadline_ns(0),
          request_ended(false),
          terminal_pending(false),
          phase(ResponsePhase::kOpen),
          token(NULL),
          t_begin_ns(0),
          t_head_ns(0),
          t_write_done_ns(0) {}
};

struct StorageNamespace {
    std::map<std::string, std::string> entries;
    size_t bytes;

    StorageNamespace() : bytes(0) {}
};

// Diagnostic phase sampler: the deadline tick records which phase the
// worker is in, giving a coarse but cheap attribution of worker CPU.
enum class WorkerPhase {
    kIdle,
    kRead,
    kProcess,
    kJS,
    kFlush,
};

class WorkerRuntime {
public:
    WorkerRuntime(int fd, int network_namespace_fd)
        : fd_(fd),
          network_namespace_fd_(network_namespace_fd),
          runtime_(NULL),
          ctx_(NULL),
          poll_started_(false),
          deadline_timer_started_(false),
          poll_events_(UV_READABLE),
          pump_in_progress_(false),
          diag_samples_(0),
          flush_syscall_samples_(0),
          flush_syscall_total_(0),
          bridge_calls_(0),
          bridge_us_(0),
          diag_enabled_(false),
          current_phase_(WorkerPhase::kIdle),
          phase_counts_(),
          phase_samples_(0),
          shutting_down_(false),
          bundle_is_trusted_bytecode_(false),
          bundle_name_("capsid:app/main"),
          application_handler_(JS_UNDEFINED),
          application_handler_this_(JS_UNDEFINED),
          begin_request_(JS_UNDEFINED),
          request_chunk_(JS_UNDEFINED),
          request_end_(JS_UNDEFINED),
          cancel_request_(JS_UNDEFINED),
          next_token_generation_(0),
          current_token_(NULL),
          poisoned_(false),
          poison_reason_(NULL),
          poison_started_ns_(0),
          poison_deadline_ns_(0),
          poison_triggers_(0),
          poison_exit_started_(false),
          reclaim_pending_(false),
          reclaim_retry_(false),
          reclaim_retry_start_ns_(0),
          retained_refs_(0),
          interrupted_request_id_(0),
          audit_window_started_ns_(0),
          audit_window_count_(0),
          audit_repeat_count_(0),
          denied_module_() {
        std::memset(&poll_, 0, sizeof(poll_));
        std::memset(&deadline_timer_, 0, sizeof(deadline_timer_));
    }

    ~WorkerRuntime() {
        if (ctx_) {
            for (std::map<uint64_t, ResponseState>::iterator it = responses_.begin();
                 it != responses_.end();
                 ++it) {
                reject_pending(it->second, "worker shutting down");
            }
            free_bridge_functions();
        }
        if (runtime_) {
            // §7.5: close capsid-owned loop handles before the txiki
            // runtime frees the loop. TJS_Run can return through paths
            // that never called shutdown() — a job exception stops the
            // runtime from tjs__drain_microtasks (upstream fatal
            // behavior), and the poisoned worker's rejection jobs throw
            // "worker poisoned" from native entry points. TJS_FreeRuntime
            // then finds poll_/deadline_timer_ still open, uv_loop_close
            // fails on the non-internal handles, and debug builds abort
            // on the assertion. shutdown() is idempotent, so the normal
            // exit paths (EOF/poison drain) are unaffected.
            shutdown();
            TJS_FreeRuntime(runtime_);
            // §7.5: a poison exit can leave registry tokens behind — refs
            // held by parked JS continuations are never released, because
            // the job machinery does not fire release hooks for values
            // freed by the runtime teardown. The JS side is gone by now;
            // free the survivors. A token released during teardown has
            // already removed itself from the registry.
            for (std::map<uint64_t, RequestToken *>::iterator it =
                     token_registry_.begin();
                 it != token_registry_.end();) {
                RequestToken *token = it->second;
                token_registry_.erase(it++);
                delete token;
            }
        }
        if (fd_ >= 0) {
            close(fd_);
        }
        if (g_worker == this) {
            g_worker = NULL;
        }
    }

    int run() {
        {
            FILE *dbg = std::fopen("E:/capsid/build-win/worker-debug.log", "ab");
            if (dbg != NULL) {
                std::fprintf(dbg, "run: entered\n");
                std::fclose(dbg);
            }
        }
        if (!read_startup(false)) {
            return 1;
        }

        capsid::SandboxConfig sandbox_config;
        sandbox_config.address_space_limit = config_.process_memory_limit;
        sandbox_config.file_descriptor_limit =
            config_.file_descriptor_limit;
        sandbox_config.strict = config_.strict_sandbox;
        sandbox_config.required_features =
            config_.sandbox_required_features;
        sandbox_config.preinstalled_features =
            config_.preinstalled_sandbox_features;
        sandbox_config.network_namespace_fd =
            network_namespace_fd_;
        if (!config_.tls_ca_bundle_path.empty()) {
            sandbox_config.read_only_paths.push_back(
                config_.tls_ca_bundle_path);
        }
        if (config_.capability_policy.module_decision(
                "capsid:fs") == capsid::kModuleGranted) {
            const std::vector<capsid::CapabilityPolicy::Rule> &rules =
                config_.capability_policy.rules();
            for (std::vector<
                     capsid::CapabilityPolicy::Rule>::const_iterator
                     rule = rules.begin();
                 rule != rules.end();
                 ++rule) {
                if (rule->action ==
                        CAPSID_PERMISSION_ALLOW &&
                    rule->permission ==
                        CAPSID_PERMISSION_READ) {
                    sandbox_config.read_only_paths.push_back(
                        rule->resource);
                }
            }
        }
        uint32_t sandbox_features = 0;
        std::string sandbox_error;
        if (!capsid::apply_sandbox(
                sandbox_config, &sandbox_features, &sandbox_error)) {
            send_error(0, std::string("sandbox setup failed: ") + sandbox_error);
            flush_blocking();
            return 1;
        }
        {
            FILE *dbg = std::fopen("E:/capsid/build-win/worker-debug.log", "ab");
            if (dbg != NULL) {
                std::fprintf(dbg, "run: sandbox applied\n");
                std::fclose(dbg);
            }
        }
        if (!read_startup(true)) {
            return 1;
        }
        {
            FILE *dbg = std::fopen("E:/capsid/build-win/worker-debug.log", "ab");
            if (dbg != NULL) {
                std::fprintf(dbg, "run: startup read\n");
                std::fclose(dbg);
            }
        }

        TJSRunOptions options;
        TJS_DefaultOptions(&options);
        options.mem_limit = static_cast<int>(
            std::min(config_.js_heap_limit, static_cast<uint64_t>(std::numeric_limits<int>::max())));
        options.stack_size = config_.js_stack_size;
        options.skip_run_main = true;
        options.bootstrap = bootstrap;
        options.bootstrap_opaque = this;

        g_worker = this;
        const char *diag_env = std::getenv("CAPSID_PERF_DIAG");
        diag_enabled_ =
            diag_env != NULL && std::strcmp(diag_env, "1") == 0;
        runtime_ = TJS_NewRuntimeOptions(&options);
        if (!runtime_ || !ctx_) {
            return 1;
        }
        {
            FILE *dbg = std::fopen("E:/capsid/build-win/worker-debug.log", "ab");
            if (dbg != NULL) {
                std::fprintf(dbg, "run: runtime created\n");
                std::fclose(dbg);
            }
        }
        JS_SetInterruptHandler(
            JS_GetRuntime(ctx_), interrupt_handler, this);
        JSJobContextHooks job_hooks;
        job_hooks.capture = job_capture_hook;
        job_hooks.enter = job_enter_hook;
        job_hooks.leave = job_leave_hook;
        job_hooks.release = job_release_hook;
        JS_SetJobContextHooks(
            JS_GetRuntime(ctx_), &job_hooks, this);
        // §7.2: the same four hooks drive the txiki-layer async context so
        // native resources (timers, httpclient, webcrypto ops) capture the
        // owning token across libuv callbacks and re-enter it when their
        // callbacks fire.
        TJSAsyncContextHooks async_ctx_hooks;
        async_ctx_hooks.capture = job_capture_hook;
        async_ctx_hooks.enter = job_enter_hook;
        async_ctx_hooks.leave = job_leave_hook;
        async_ctx_hooks.release = job_release_hook;
        tjs_set_async_context_hooks(ctx_, &async_ctx_hooks, this);
        std::string bridge_error;
        if (!load_bridge_functions(&bridge_error)) {
            send_error(0, bridge_error);
            flush_blocking();
            return 1;
        }
        seal_module_loader();

        std::string load_error;
        {
            FILE *dbg = std::fopen("E:/capsid/build-win/worker-debug.log", "ab");
            if (dbg != NULL) {
                std::fprintf(dbg, "run: before load_application\n");
                std::fclose(dbg);
            }
        }
        if (!load_application(&load_error)) {
            send_error(0, load_error);
            flush_blocking();
            return 1;
        }
        {
            FILE *dbg = std::fopen("E:/capsid/build-win/worker-debug.log", "ab");
            if (dbg != NULL) {
                std::fprintf(dbg, "run: after load_application\n");
                std::fclose(dbg);
            }
        }

        set_nonblocking();
#if defined(_WIN32)
        // Windows: flush READY before arming the loop poll. libuv's
        // fast-poll submits an AFD poll registration whose completion
        // races a second send on the same socket (ERROR_ALREADY_EXISTS);
        // sending everything up front keeps the startup flush inside the
        // registration-free window and makes the ordering deterministic.
        static_assert(sizeof(CAPSID_BUILD_COMPATIBILITY_ID) - 1 == 71,
                      "compatibility ID must be sha256: plus 64 hex digits");
        send_payload(capsid::protocol::kReady, 0, sandbox_features,
                     reinterpret_cast<const std::uint8_t *>(
                         CAPSID_BUILD_COMPATIBILITY_ID),
                     sizeof(CAPSID_BUILD_COMPATIBILITY_ID) - 1);
        flush_output();
#endif
        {
            FILE *dbg = std::fopen("E:/capsid/build-win/worker-debug.log", "ab");
            if (dbg != NULL) {
                std::fprintf(dbg, "run: before uv_poll_init\n");
                std::fclose(dbg);
            }
        }
        if (uv_poll_init(TJS_GetLoop(runtime_), &poll_, fd_) != 0) {
            return 1;
        }
        {
            FILE *dbg = std::fopen("E:/capsid/build-win/worker-debug.log", "ab");
            if (dbg != NULL) {
                std::fprintf(dbg, "run: after uv_poll_init\n");
                std::fclose(dbg);
            }
        }
        poll_.data = this;
        poll_started_ = true;
        if (uv_timer_init(TJS_GetLoop(runtime_), &deadline_timer_) != 0) {
            return 1;
        }
        deadline_timer_.data = this;
        if (uv_timer_start(
                &deadline_timer_, deadline_timer_callback, 10, 10) != 0) {
            return 1;
        }
        deadline_timer_started_ = true;
        {
            FILE *dbg = std::fopen("E:/capsid/build-win/worker-debug.log", "ab");
            if (dbg != NULL) {
                std::fprintf(dbg, "run: before update_poll\n");
                std::fclose(dbg);
            }
        }
        update_poll();
        {
            FILE *dbg = std::fopen("E:/capsid/build-win/worker-debug.log", "ab");
            if (dbg != NULL) {
                std::fprintf(dbg, "run: after update_poll\n");
                std::fclose(dbg);
            }
        }
#if !defined(_WIN32)
        // The READY payload is the 71-byte compatibility ID from the single
        // generated identity source, so a host can compare the running
        // worker against the linked library and the bytecode compiler
        // without trusting either side. sandbox features stay in flags.
        // (On Windows this flush happens before uv_poll_init instead.)
        static_assert(sizeof(CAPSID_BUILD_COMPATIBILITY_ID) - 1 == 71,
                      "compatibility ID must be sha256: plus 64 hex digits");
        send_payload(capsid::protocol::kReady, 0, sandbox_features,
                     reinterpret_cast<const std::uint8_t *>(
                         CAPSID_BUILD_COMPATIBILITY_ID),
                     sizeof(CAPSID_BUILD_COMPATIBILITY_ID) - 1);
        flush_output();
        {
            FILE *dbg = std::fopen("E:/capsid/build-win/worker-debug.log", "ab");
            if (dbg != NULL) {
                std::fprintf(dbg, "run: after ready flush\n");
                std::fclose(dbg);
            }
        }
#endif

        const int run_result = TJS_Run(runtime_);
        // §7.5: a clean exit — not poisoned and with no response state
        // left (a host EOF with inflight requests legitimately still
        // holds token refs) — must not retain a live continuation on any
        // token. The same discriminator as reclaim_settled_tokens: a
        // token awaiting its tick reclaim (terminal or response already
        // gone, refs == 1, nothing left to resume it) is fine; anything
        // with refs > 1 is a leak. The poison exit path reports its own
        // retained count on POISON EXIT.
        bool token_leak = false;
        if (!poisoned_ && responses_.empty()) {
            for (std::map<uint64_t, RequestToken *>::const_iterator it =
                     token_registry_.begin();
                 it != token_registry_.end();
                 ++it) {
                const RequestToken *token = it->second;
                const bool response_gone =
                    responses_.find(token->request_id) == responses_.end();
                const bool reclaimable = (token->terminal || response_gone) &&
                                         token->refs == 1;
                if (!reclaimable) {
                    token_leak = true;
                }
            }
        }
        if (token_leak) {
            std::fprintf(stderr,
                         "TOKEN LEAK on clean exit: retained=%llu\n",
                         static_cast<unsigned long long>(retained_refs_));
            return 1;
        }
        return run_result;
    }

private:
    static WorkerRuntime *g_worker;

    static int interrupt_handler(JSRuntime *, void *opaque) {
        // §6.2: identity comes from the token of the job actually running,
        // never from a bare global id + responses_ lookup.
        WorkerRuntime *self = static_cast<WorkerRuntime *>(opaque);
        if (!self || self->interrupted_request_id_ != 0) {
            return 0;
        }
        RequestToken *token = self->current_token_;
        if (!token || token->terminal || token->deadline_ns == 0) {
            return 0;
        }
        const uint64_t now = uv_hrtime();
        // §7.4: a poisoned worker may run a bounded drain, but terminal
        // jobs are capped by the independent poison deadline as well —
        // no post-poison continuation runs unbounded.
        if (self->poisoned_) {
            return now >= self->poison_deadline_ns_ ? 1 : 0;
        }
        if (now >= token->deadline_ns) {
            self->interrupted_request_id_ = token->request_id;
            return 1;
        }
        return 0;
    }

    // WP-02 §6.2 job-context hooks. capture retains the active token (or
    // yields a null context when no request is active — module-loading and
    // worker-scope jobs are legal); enter/leave restore nesting; release
    // drops the retained ref. Hooks never throw across the C boundary.
    static int job_capture_hook(JSContext *,
                                void *opaque,
                                void **out_job_context) {
        WorkerRuntime *self = static_cast<WorkerRuntime *>(opaque);
        *out_job_context = NULL;
        if (self && self->current_token_ != NULL) {
            self->retain_token(self->current_token_);
            *out_job_context = self->current_token_;
        }
        return 0;
    }

    static void *job_enter_hook(JSContext *,
                                void *job_context,
                                void *opaque) {
        WorkerRuntime *self = static_cast<WorkerRuntime *>(opaque);
        void *previous = self ? self->current_token_ : NULL;
        if (self) {
            self->current_token_ =
                static_cast<RequestToken *>(job_context);
        }
        return previous;
    }

    static void job_leave_hook(JSContext *,
                               void *previous_context,
                               void *opaque) {
        WorkerRuntime *self = static_cast<WorkerRuntime *>(opaque);
        if (self) {
            self->current_token_ =
                static_cast<RequestToken *>(previous_context);
        }
    }

    static void job_release_hook(void *job_context, void *opaque) {
        WorkerRuntime *self = static_cast<WorkerRuntime *>(opaque);
        if (self) {
            self->release_token(
                static_cast<RequestToken *>(job_context));
        }
    }

    void retain_token(RequestToken *token) {
        if (token) {
            ++token->refs;
            retained_refs_ += 1;
        }
    }

    void release_token(RequestToken *token) {
        if (!token || token->refs <= 0) {
            return;
        }
        retained_refs_ -= 1;
        if (--token->refs == 0) {
            token_registry_.erase(token->generation);
            delete token;
        }
    }

    uint64_t active_request_id() const {
        return current_token_ ? current_token_->request_id : 0;
    }

    // §6.3: unified gate for request-level native APIs. Returns the active
    // token, or NULL when the caller is allowed worker scope (audit paths
    // with explicit id 0). Any violation throws and identity tampering
    // poisons the worker (full poison mechanics land in WP-03).
    RequestToken *require_active_request(JSContext *ctx,
                                         uint64_t explicit_id,
                                         bool has_explicit,
                                         bool allow_worker_scope,
                                         const char *site = "") {
        if (poisoned_) {
            JS_ThrowInternalError(ctx, "worker poisoned");
            return NULL;
        }
        RequestToken *token = current_token_;
        if (!token) {
            // Worker scope is legal only where explicitly authorized; an
            // active request job always carries a token (capture is
            // fail-closed), so a missing token here means module-loading
            // or worker-scope execution.
            if (allow_worker_scope) {
                return NULL;
            }
            JS_ThrowInternalError(ctx, "no active request");
            return NULL;
        }
        if (has_explicit && explicit_id != token->request_id) {
            if (diag_enabled_) {
                std::fprintf(
                    stderr,
                    "POISON SITE identity-mismatch explicit=%llu active=%llu native=%s\n",
                    static_cast<unsigned long long>(explicit_id),
                    static_cast<unsigned long long>(token->request_id),
                    site);
            }
            poisoned_ = true;
            JS_ThrowInternalError(ctx, "request identity mismatch");
            return NULL;
        }
        if (token->terminal) {
            JS_ThrowInternalError(ctx, "request already settled");
            return NULL;
        }
        std::map<uint64_t, RequestToken *>::iterator found =
            token_registry_.find(token->generation);
        if (found == token_registry_.end() || found->second != token) {
            if (diag_enabled_) {
                std::fprintf(
                    stderr,
                    "POISON SITE stale-token active=%llu\n",
                    static_cast<unsigned long long>(token->request_id));
            }
            poisoned_ = true;
            JS_ThrowInternalError(ctx, "stale request token");
            return NULL;
        }
        return token;
    }

    bool settle_request(uint64_t id) {
        for (std::map<uint64_t, RequestToken *>::iterator it =
                 token_registry_.begin();
             it != token_registry_.end();
             ++it) {
            RequestToken *token = it->second;
            if (token->request_id == id) {
                if (token->terminal) {
                    return false;  // double settle
                }
                // §6.4: settle ends the ResponseState owner ref. The
                // post-drain reclaim then sees only the registry owner
                // (refs==1 -> free) plus any surviving job/resource refs
                // (poison). The response entry itself survives until the
                // transport drains the terminal, so it must tolerate a
                // NULL token (erase_response and the bridges do).
                std::map<uint64_t, ResponseState>::iterator found =
                    responses_.find(id);
                if (found != responses_.end()) {
                    release_token(found->second.token);
                    found->second.token = NULL;
                }
                token->terminal = true;
                // §7.4: the normal completion path runs no drain; the
                // deadline tick performs the post-settle reclaim.
                request_reclaim();
                return true;
            }
        }
        return false;
    }

    // §6.4.3-5 + §7.4 poison state machine. Idempotent: the first reason
    // wins, later triggers only increment the diagnostic counter. On entry
    // every inflight token is terminalized once and its response rejected,
    // so no capability can produce a side effect afterwards; the worker
    // then drains on a strict poison deadline and exits (§7.4/§7.5).
    void enter_poison(const char *reason) {
        if (poisoned_) {
            if (reason != NULL) {
                poison_triggers_ += 1;
            }
            return;
        }
        poisoned_ = true;
        poison_reason_ = reason;
        poison_started_ns_ = uv_hrtime();
        poison_deadline_ns_ = poison_started_ns_ + kPoisonGraceNs;
        std::fprintf(stderr, "WORKER POISONED: %s\n",
                     reason != NULL ? reason : "unspecified");
        for (std::map<uint64_t, ResponseState>::iterator it =
                 responses_.begin();
             it != responses_.end();
             ++it) {
            if (it->second.token != NULL && !it->second.token->terminal) {
                it->second.token->terminal = true;
            }
            reject_pending(it->second, "worker poisoned");
        }
    }

    // §7.4: the bounded drain window ends at the poison deadline; the
    // worker then flushes and exits unconditionally (EOF -> host EXIT).
    void check_poison() {
        if (!poisoned_ || poison_exit_started_) {
            return;
        }
        if (uv_hrtime() >= poison_deadline_ns_) {
            initiate_poison_exit();
        }
    }

    void initiate_poison_exit() {
        if (poison_exit_started_) {
            return;
        }
        poison_exit_started_ = true;
        if (diag_enabled_) {
            std::fprintf(stderr,
                         "POISON EXIT reason=%s retained=%llu triggers=%llu\n",
                         poison_reason_ != NULL ? poison_reason_ : "unspecified",
                         static_cast<unsigned long long>(retained_refs_),
                         static_cast<unsigned long long>(poison_triggers_));
        }
        flush_blocking();
        shutdown();
    }

    // §6.4.3-5 + §7.4: after a drain (or the settle tick), a token must be
    // held by nothing but the registry. A terminal token with surviving
    // refs — or a token whose response already ended yet is still held by
    // a job or native resource — is a detached-continuation leak and
    // poisons the worker. refs==1 with the response gone means nothing can
    // ever resume the chain (any live continuation holds a captured ref),
    // so the token is reclaimed.
    void reclaim_settled_tokens() {
        std::vector<uint64_t> reclaimable;
        bool deferred_this_round = false;
        for (std::map<uint64_t, RequestToken *>::iterator it =
                 token_registry_.begin();
             it != token_registry_.end();
             ++it) {
            RequestToken *token = it->second;
            // §7.4: remember the refs of the previous round before
            // updating; a candidate whose refs are still falling is a
            // chain actively unwinding (the cancel continuation's promise
            // reactions release refs across several rounds), not a
            // detached continuation. The baseline is refreshed on every
            // observation so the first candidate round after a cancel
            // compares against the post-continuation count.
            const int prev_reclaim_refs = token->last_reclaim_refs_;
            token->last_reclaim_refs_ = token->refs;
            const bool response_gone =
                responses_.find(token->request_id) == responses_.end();
            if (!token->terminal && !response_gone) {
                continue;  // in flight and healthy
            }
            if (token->refs == 1) {
                reclaimable.push_back(it->first);
            } else if (!token->terminal &&
                       (defer_reclaim_while_live() ||
                        token->refs < prev_reclaim_refs ||
                        token->reclaim_grace)) {
                if (token->reclaim_grace) {
                    // §7.4: the first reclaim after a cancel defers
                    // unconditionally — the cancel's drain already ran the
                    // chain to completion, and the completed chain's
                    // captured refs fall only once a later GC collects the
                    // dead promise subgraph. Poisoning on this round
                    // false-positives a healthy cancellation.
                    token->reclaim_grace = false;
                    if (diag_enabled_) {
                        std::fprintf(
                            stderr,
                            "RECLAIM GRACE id=%llu refs=%llu\n",
                            static_cast<unsigned long long>(token->request_id),
                            static_cast<unsigned long long>(token->refs));
                    }
                }
                // The response is gone and the chain has not settled (no
                // capsidRequestSettled: cancel/timeout deleted the state
                // before .finally). Refs are held by a live JS chain that
                // is actively unwinding — pending jobs, or refs falling
                // against the previous round's baseline. Defer the poison
                // decision to the next tick — the tick's drain+GC will
                // reclaim once the chain settles. A chain parked on an
                // unfired timer defers nothing: the poison lands before
                // the timer can run its continuation (the timeout-path
                // regression: the 80ms timer's continuation emitted a
                // native LOG inside the poison grace). Bounded by
                // kReclaimSettleWindowNs (see defer_reclaim_while_live).
                if (diag_enabled_) {
                    std::fprintf(
                        stderr,
                        "RECLAIM DEFER request_id=%llu refs=%llu\n",
                        static_cast<unsigned long long>(token->request_id),
                        static_cast<unsigned long long>(token->refs));
                }
                reclaim_pending_ = true;
                if (!reclaim_retry_) {
                    reclaim_retry_ = true;
                    reclaim_retry_start_ns_ = uv_hrtime();
                }
                deferred_this_round = true;
            } else {
                if (diag_enabled_) {
                    std::fprintf(
                        stderr,
                        "POISON TRIGGER request_id=%llu refs=%llu terminal=%d\n",
                        static_cast<unsigned long long>(token->request_id),
                        static_cast<unsigned long long>(token->refs),
                        token->terminal ? 1 : 0);
                }
                enter_poison(token->terminal
                                 ? "terminal continuation leak"
                                 : "detached resource after response end");
            }
        }
        if (!deferred_this_round) {
            // The retry sequence ends when a decision round neither
            // defers nor re-arms; a fresh reclaim request then starts a
            // full cycle (see the deadline tick).
            reclaim_retry_ = false;
            reclaim_retry_start_ns_ = 0;
        }
        for (size_t i = 0; i < reclaimable.size(); ++i) {
            release_token(token_registry_[reclaimable[i]]);
        }
    }

    // §7.4: a fresh reclaim request (settle / cancel / timeout) starts a
    // full cycle: any in-flight retry sequence (deferred poison while a
    // chain was still live) ends, so the next deadline tick runs the
    // unconditional drain+GC before the reclaim decision.
    //
    // Cancel, timeout and settle share one semantics: a live non-terminal
    // token whose response is gone defers only while jobs are pending
    // (the chain is actively winding down); a chain parked on an unfired
    // timer is a detached continuation and poisons on the next tick.
    void request_reclaim() {
        reclaim_pending_ = true;
        reclaim_retry_ = false;
        reclaim_retry_start_ns_ = 0;
    }

    // §7.4: true while the JS side still has pending jobs that could
    // settle a non-terminal chain (the cancel continuation's promise
    // reactions, the reject timers of an abort path). Bounded: past
    // kReclaimSettleWindowNs from the first deferral the chain is treated
    // as a detached continuation even if jobs remain, and the worker
    // poisons instead of deferring forever.
    //
    // A chain parked on a uv timer that has not fired is NOT a deferral
    // reason: letting it fire later would run the continuation — exactly
    // the timeout-path hazard where the 80ms timer's continuation emitted
    // a native LOG after the request timed out. Timer deferral was removed
    // for both hard and soft reclaims; the poison then lands before the
    // timer fires and the terminalized token makes the continuation's
    // capability call throw (require_active_request).
    bool defer_reclaim_while_live() {
        if (reclaim_retry_ &&
            uv_hrtime() - reclaim_retry_start_ns_ >= kReclaimSettleWindowNs) {
            return false;
        }
        return JS_IsJobPending(JS_GetRuntime(ctx_)) != 0;
    }

    void erase_response(
        std::map<uint64_t, ResponseState>::iterator it) {
        if (it == responses_.end()) {
            return;
        }
        release_token(it->second.token);
        it->second.token = NULL;
        responses_.erase(it);
    }

    static void deadline_timer_callback(uv_timer_t *timer) {
        WorkerRuntime *self =
            static_cast<WorkerRuntime *>(timer->data);
        if (!self) {
            return;
        }
        // §7.4: settle leaves no drain on the normal path, so the tick
        // performs the post-settle reclaim (may poison), then checks the
        // poison deadline before expiring requests.
        if (self->reclaim_pending_) {
            // §7.4: the tick performs the post-settle reclaim. Drain any
            // pending jobs (a fired timer enqueues the settle chain), then
            // run a full GC — a settled chain is invisible garbage that
            // only drops its captured token refs once collected. Retry
            // ticks GC unconditionally: a chain whose jobs drained to
            // completion inside the cancel path holds refs only through
            // two-phase promise finalizers, and the first retry round is
            // exactly when those refs fall. A chain parked on a long
            // timer stays reachable through the timer, so the GC cannot
            // extend its life — the poison deadline still lands before
            // the timer fires.
            self->reclaim_pending_ = false;
            self->drain_jobs();
            JS_RunGC(JS_GetRuntime(self->ctx_));
            self->reclaim_settled_tokens();
        }
        self->check_poison();
        if (!self->diag_enabled_) {
            self->expire_requests();
            return;
        }
        const size_t index = static_cast<size_t>(self->current_phase_);
        if (index < 5) {
            self->phase_counts_[index] += 1;
        }
        self->phase_samples_ += 1;
        if (self->phase_samples_ % 300 == 0) {
            std::fprintf(stderr,
                         "PHASE idle=%llu read=%llu process=%llu js=%llu flush=%llu\n",
                         static_cast<unsigned long long>(self->phase_counts_[0]),
                         static_cast<unsigned long long>(self->phase_counts_[1]),
                         static_cast<unsigned long long>(self->phase_counts_[2]),
                         static_cast<unsigned long long>(self->phase_counts_[3]),
                         static_cast<unsigned long long>(self->phase_counts_[4]));
            std::fprintf(stderr,
                         "TOKENS retained=%llu\n",
                         static_cast<unsigned long long>(self->retained_refs_));
        }
        self->expire_requests();
    }

    static int bootstrap(TJSRuntime *runtime, JSContext *ctx, void *opaque) {
        WorkerRuntime *self = static_cast<WorkerRuntime *>(opaque);
        self->runtime_ = runtime;
        self->ctx_ = ctx;

        JSValue core = TJS_GetInternalCore(runtime);
        if (capsid_tjs_set_cookie_jar_path(runtime, "") != 0 ||
            capsid_tjs_set_egress_policy(
                runtime, egress_check, self) != 0 ||
            (!self->config_.tls_ca_bundle_path.empty() &&
             capsid_tjs_set_ca_bundle_path(
                 runtime,
                 self->config_.tls_ca_bundle_path.c_str()) != 0) ||
            !self->define_native(core, "capsidLog", js_log, 2) ||
            !self->define_native(core, "capsidRequestCredit", js_request_credit, 2) ||
            !self->define_native(core, "capsidResponseHead", js_response_head, 4) ||
            !self->define_native(core, "capsidResponseWrite", js_response_write, 2) ||
            !self->define_native(core, "capsidResponseEnd", js_response_end, 1) ||
            !self->define_native(core, "capsidResponseError", js_response_error, 2) ||
            !self->define_native(core, "capsidResponseFinal", js_response_final, 5) ||
            !self->define_native(core, "capsidResponseFixed", js_response_fixed, 5) ||
            !self->define_native(core, "capsidRequestSettled", js_request_settled, 1) ||
            !self->define_native(
                core,
                "capsidFetchRequestBodyLimit",
                js_fetch_request_body_limit,
                0) ||
            !self->define_native(
                core,
                "capsidFetchResponseBodyLimit",
                js_fetch_response_body_limit,
                0) ||
            !self->define_native(
                core,
                "capsidFixedResponseBodyLimit",
                js_fixed_response_body_limit,
                0) ||
            !self->define_native(core, "capsidInstallBridge", js_install_bridge, 4)) {
            if (!JS_HasException(ctx)) {
                JS_ThrowInternalError(ctx, "failed to install Capsid native bridge");
            }
            return -1;
        }
        return TJS_EvalBytecode(
            ctx, capsid__bootstrap, capsid__bootstrap_size, true);
    }

    // Fills the diagnostic message a denied fetch reports to the app.
    // The hook contract is: reason is written only on deny; callers zero
    // it before the check.
    static void set_egress_deny_reason(
        char *reason,
        size_t reason_size,
        const std::string &resource,
        capsid::EgressDenyReason deny_reason) {
        if (!reason || reason_size == 0) {
            return;
        }
        switch (deny_reason) {
        case capsid::EgressDenyReason::kProtected:
            snprintf(reason,
                     reason_size,
                     "Network request denied by egress policy: address "
                     "'%s' is in a protected range and not explicitly "
                     "authorized",
                     resource.c_str());
            break;
        case capsid::EgressDenyReason::kExplicitDeny:
            snprintf(reason,
                     reason_size,
                     "Network request denied by egress policy: denied by "
                     "an explicit egress rule");
            break;
        case capsid::EgressDenyReason::kNoMatch:
        case capsid::EgressDenyReason::kNone:
        default:
            snprintf(reason,
                     reason_size,
                     "Network request denied by egress policy: '%s' is "
                     "not authorized",
                     resource.c_str());
            break;
        }
    }

    static int egress_check(void *opaque,
                            const char *host,
                            uint16_t port,
                            const struct sockaddr *address,
                            socklen_t address_len,
                            char *reason,
                            size_t reason_size) {
        WorkerRuntime *self =
            static_cast<WorkerRuntime *>(opaque);
        if (reason && reason_size != 0) {
            reason[0] = '\0';
        }
        if (!self || !host || port == 0) {
            return 0;
        }
        const std::string host_text(host);
        const std::string resource =
            host_text + ":" + std::to_string(port);
        capsid::EgressDecision decision;
        if (self->config_.capability_policy.enabled()) {
            decision =
                self->config_.capability_policy.net_policy()
                    .decide_host(host_text, port);
        } else {
            decision =
                self->config_.egress_policy.decide_host(
                    host_text, port);
        }
        if (!decision.allowed) {
            self->emit_audit(
                CAPSID_AUDIT_STAGE_OPERATION,
                CAPSID_AUDIT_DENY,
                self->active_request_id(),
                decision.rule_id,
                std::string(),
                "net",
                "host",
                resource);
            set_egress_deny_reason(
                reason, reason_size, resource, decision.deny_reason);
            return 0;
        }
        if (self->config_.capability_policy.enabled() &&
            self->config_.legacy_egress_configured) {
            const capsid::EgressDecision legacy =
                self->config_.egress_policy.decide_host(
                    host_text, port);
            if (!legacy.allowed) {
                self->emit_audit(
                    CAPSID_AUDIT_STAGE_OPERATION,
                    CAPSID_AUDIT_DENY,
                    self->active_request_id(),
                    legacy.rule_id,
                    std::string(),
                    "net",
                    "host",
                    resource);
                set_egress_deny_reason(
                    reason, reason_size, resource, legacy.deny_reason);
                return 0;
            }
        }
        if (address) {
            // The hostname passed the host stage, so the address being
            // connected came from the fetch engine's own resolution of
            // that authorized hostname: only explicit address rules may
            // still deny it. This applies equally to public and internal
            // DNS names, including when they resolve into a protected range.
            if (self->config_.capability_policy.enabled()) {
                decision =
                    self->config_.capability_policy.net_policy()
                        .decide_resolved_address_authoritative(
                            address, address_len, port);
            } else {
                decision =
                    self->config_.egress_policy
                        .decide_resolved_address_authoritative(
                            address, address_len, port);
            }
            if (!decision.allowed) {
                self->emit_audit(
                    CAPSID_AUDIT_STAGE_OPERATION,
                    CAPSID_AUDIT_DENY,
                    self->active_request_id(),
                    decision.rule_id,
                    std::string(),
                    "net",
                    "address",
                    resource);
                set_egress_deny_reason(
                    reason, reason_size, resource, decision.deny_reason);
                return 0;
            }
            if (self->config_.capability_policy.enabled() &&
                self->config_.legacy_egress_configured) {
                const capsid::EgressDecision legacy =
                    self->config_.egress_policy
                        .decide_resolved_address_authoritative(
                            address, address_len, port);
                if (!legacy.allowed) {
                    self->emit_audit(
                        CAPSID_AUDIT_STAGE_OPERATION,
                        CAPSID_AUDIT_DENY,
                        self->active_request_id(),
                        legacy.rule_id,
                        std::string(),
                        "net",
                        "address",
                        resource);
                    set_egress_deny_reason(
                        reason, reason_size, resource,
                        legacy.deny_reason);
                    return 0;
                }
            }
        } else {
            self->emit_audit(
                CAPSID_AUDIT_STAGE_OPERATION,
                CAPSID_AUDIT_ALLOW,
                self->active_request_id(),
                decision.rule_id,
                std::string(),
                "net",
                "host",
                resource);
        }
        return 1;
    }

    bool define_native(JSValue core,
                       const char *name,
                       JSCFunction *function,
                       int length) {
        const int result =
            JS_DefinePropertyValueStr(ctx_,
                                      core,
                                      name,
                                      JS_NewCFunction(ctx_, function, name, length),
                                      JS_PROP_C_W_E);
        return result > 0;
    }

    static JSValue js_log(JSContext *ctx,
                          JSValueConst,
                          int argc,
                          JSValueConst *argv) {
        if (!g_worker || argc < 2) {
            return JS_UNDEFINED;
        }
        // §6.3: LOG is worker-scope legal (module load phase, id 0) but
        // must carry the active token's id during request execution.
        RequestToken *token =
            g_worker->require_active_request(ctx, 0, false, true);
        if (!token && JS_HasException(ctx)) {
            return JS_EXCEPTION;  // poisoned / settled
        }
        const std::string level = to_string(ctx, argv[0]);
        const std::string message = to_string(ctx, argv[1]);
        capsid::protocol::Frame frame;
        frame.type = capsid::protocol::kLog;
        frame.flags = 0;
        frame.request_id = token ? token->request_id : 0;
        append_string16(&frame.payload,
                        reinterpret_cast<const uint8_t *>(level.data()),
                        level.size());
        frame.payload.insert(frame.payload.end(), message.begin(), message.end());
        g_worker->queue_output(frame);
        return JS_UNDEFINED;
    }

    static JSValue js_fetch_request_body_limit(JSContext *ctx,
                                               JSValueConst,
                                               int,
                                               JSValueConst *) {
        return JS_NewInt64(
            ctx,
            static_cast<int64_t>(
                g_worker
                    ? g_worker->config_.max_fetch_request_body_bytes
                    : 0));
    }

    static JSValue js_fetch_response_body_limit(JSContext *ctx,
                                                JSValueConst,
                                                int,
                                                JSValueConst *) {
        return JS_NewInt64(
            ctx,
            static_cast<int64_t>(
                g_worker
                    ? g_worker->config_.max_fetch_response_body_bytes
                    : 0));
    }

    static JSValue js_fixed_response_body_limit(JSContext *ctx,
                                                JSValueConst,
                                                int,
                                                JSValueConst *) {
        return JS_NewInt64(
            ctx,
            static_cast<int64_t>(capsid::protocol::kMaxFixedBodySize));
    }

    // §6.4: the only lifecycle entry a terminal token may still call. The
    // bootstrap calls it after requests.delete(id) and the final cleanup;
    // native validates the token and marks it terminal. The post-drain
    // reclaim then drops the token (or poisons the worker) by refcount.
    static JSValue js_request_settled(JSContext *ctx,
                                      JSValueConst,
                                      int argc,
                                      JSValueConst *argv) {
        uint64_t id = 0;
        if (!g_worker || argc < 1 ||
            JS_ToBigUint64(ctx, &id, argv[0]) || id == 0) {
            return JS_ThrowInternalError(
                ctx, "invalid settled request id");
        }
        if (g_worker->settle_request(id)) {
            return JS_UNDEFINED;
        }
        return JS_ThrowInternalError(
            ctx, "cannot settle unknown request");
    }

    static JSValue js_install_bridge(JSContext *ctx,
                                     JSValueConst,
                                     int argc,
                                     JSValueConst *argv) {
        if (!g_worker || argc < 4) {
            return JS_ThrowInternalError(ctx, "invalid Capsid bridge installation");
        }
        for (int i = 0; i < 4; ++i) {
            if (!JS_IsFunction(ctx, argv[i])) {
                return JS_ThrowTypeError(ctx, "Capsid request bridge entries must be functions");
            }
        }

        JS_FreeValue(ctx, g_worker->begin_request_);
        JS_FreeValue(ctx, g_worker->request_chunk_);
        JS_FreeValue(ctx, g_worker->request_end_);
        JS_FreeValue(ctx, g_worker->cancel_request_);
        g_worker->begin_request_ = JS_DupValue(ctx, argv[0]);
        g_worker->request_chunk_ = JS_DupValue(ctx, argv[1]);
        g_worker->request_end_ = JS_DupValue(ctx, argv[2]);
        g_worker->cancel_request_ = JS_DupValue(ctx, argv[3]);
        return JS_UNDEFINED;
    }

    static JSValue js_request_credit(JSContext *ctx,
                                     JSValueConst,
                                     int argc,
                                     JSValueConst *argv) {
        uint64_t id = 0;
        uint32_t credit = 0;
        if (!g_worker || argc < 2 || JS_ToBigUint64(ctx, &id, argv[0]) ||
            JS_ToUint32(ctx, &credit, argv[1]) || id == 0 || credit == 0) {
            return JS_EXCEPTION;
        }
        if (!g_worker->require_active_request(ctx, id, true, false, "js_request_credit")) {
            return JS_EXCEPTION;
        }
        std::map<uint64_t, ResponseState>::iterator state =
            g_worker->responses_.find(id);
        if (state == g_worker->responses_.end() ||
            state->second.request_credit >
                g_worker->config_.initial_window ||
            credit >
                g_worker->config_.initial_window -
                    state->second.request_credit) {
            return JS_ThrowRangeError(ctx, "invalid request credit update");
        }
        state->second.request_credit += credit;
        g_worker->send_window_update(id, credit);
        return JS_UNDEFINED;
    }

    // The fixed-body path must hold its head until the exact UTF-8 body size
    // is known. Ordinary responses use build_response_head below and retain
    // the original head-first IPC pipeline.
    static bool build_fixed_response_head(
                                    JSContext *ctx,
                                    uint64_t id,
                                    uint32_t status,
                                    JSValueConst status_text_value,
                                    JSValueConst headers_value,
                                    capsid::protocol::Frame *out_frame) {
        if (!out_frame) {
            return false;
        }
        capsid::protocol::Frame frame;
        frame.type = capsid::protocol::kResponseHead;
        frame.flags = 0;
        frame.request_id = id;
        capsid::protocol::append_u16(
            &frame.payload, static_cast<uint16_t>(status));
        const std::string status_text = to_string(ctx, status_text_value);
        if (status_text.size() > std::numeric_limits<uint16_t>::max() ||
            status_text.size() + sizeof(uint16_t) >
                g_worker->config_.max_header_bytes - frame.payload.size()) {
            return false;
        }
        append_string16(
            &frame.payload,
            reinterpret_cast<const uint8_t *>(status_text.data()),
            status_text.size());

        uint32_t count = 0;
        JSValue length_value =
            JS_GetPropertyStr(ctx, headers_value, "length");
        if (JS_ToUint32(ctx, &count, length_value)) {
            JS_FreeValue(ctx, length_value);
            return false;
        }
        JS_FreeValue(ctx, length_value);
        if (count > std::numeric_limits<uint16_t>::max()) {
            return false;
        }
        capsid::protocol::append_u16(
            &frame.payload, static_cast<uint16_t>(count));
        for (uint32_t i = 0; i < count; ++i) {
            JSValue pair = JS_GetPropertyUint32(ctx, headers_value, i);
            JSValue name_value = JS_GetPropertyUint32(ctx, pair, 0);
            JSValue value_value = JS_GetPropertyUint32(ctx, pair, 1);
            const std::string name = to_string(ctx, name_value);
            const std::string value = to_string(ctx, value_value);
            JS_FreeValue(ctx, value_value);
            JS_FreeValue(ctx, name_value);
            JS_FreeValue(ctx, pair);
            if (name.size() > std::numeric_limits<uint16_t>::max() ||
                value.size() > std::numeric_limits<uint32_t>::max()) {
                return false;
            }
            const size_t overhead = sizeof(uint16_t) + sizeof(uint32_t);
            if (name.size() >
                    g_worker->config_.max_header_bytes -
                        frame.payload.size() ||
                overhead >
                    g_worker->config_.max_header_bytes -
                        frame.payload.size() - name.size() ||
                value.size() >
                    g_worker->config_.max_header_bytes -
                        frame.payload.size() - name.size() - overhead) {
                return false;
            }
            append_string16(
                &frame.payload,
                reinterpret_cast<const uint8_t *>(name.data()),
                name.size());
            append_string32(
                &frame.payload,
                reinterpret_cast<const uint8_t *>(value.data()),
                value.size());
        }
        if (frame.payload.size() > g_worker->config_.max_header_bytes) {
            return false;
        }
        *out_frame = std::move(frame);
        return true;
    }

    // Builds and queues the ordinary ResponseHead frame. Keep this hot path
    // independent of the bounded fixed-response representation so large and
    // streamed responses retain their original IPC overlap and code shape.
    static bool build_response_head(JSContext *ctx,
                                    uint64_t id,
                                    uint32_t status,
                                    JSValueConst status_text_value,
                                    JSValueConst headers_value) {
        capsid::protocol::Frame frame;
        frame.type = capsid::protocol::kResponseHead;
        frame.flags = 0;
        frame.request_id = id;
        capsid::protocol::append_u16(
            &frame.payload, static_cast<uint16_t>(status));
        const std::string status_text = to_string(ctx, status_text_value);
        if (status_text.size() > std::numeric_limits<uint16_t>::max() ||
            status_text.size() + sizeof(uint16_t) >
                g_worker->config_.max_header_bytes - frame.payload.size()) {
            return false;
        }
        append_string16(
            &frame.payload,
            reinterpret_cast<const uint8_t *>(status_text.data()),
            status_text.size());

        uint32_t count = 0;
        JSValue length_value =
            JS_GetPropertyStr(ctx, headers_value, "length");
        if (JS_ToUint32(ctx, &count, length_value)) {
            JS_FreeValue(ctx, length_value);
            return false;
        }
        JS_FreeValue(ctx, length_value);
        if (count > std::numeric_limits<uint16_t>::max()) {
            return false;
        }
        capsid::protocol::append_u16(
            &frame.payload, static_cast<uint16_t>(count));
        for (uint32_t i = 0; i < count; ++i) {
            JSValue pair = JS_GetPropertyUint32(ctx, headers_value, i);
            JSValue name_value = JS_GetPropertyUint32(ctx, pair, 0);
            JSValue value_value = JS_GetPropertyUint32(ctx, pair, 1);
            const std::string name = to_string(ctx, name_value);
            const std::string value = to_string(ctx, value_value);
            JS_FreeValue(ctx, value_value);
            JS_FreeValue(ctx, name_value);
            JS_FreeValue(ctx, pair);
            if (name.size() > std::numeric_limits<uint16_t>::max() ||
                value.size() > std::numeric_limits<uint32_t>::max()) {
                return false;
            }
            const size_t overhead = sizeof(uint16_t) + sizeof(uint32_t);
            if (name.size() >
                    g_worker->config_.max_header_bytes -
                        frame.payload.size() ||
                overhead >
                    g_worker->config_.max_header_bytes -
                        frame.payload.size() - name.size() ||
                value.size() >
                    g_worker->config_.max_header_bytes -
                        frame.payload.size() - name.size() - overhead) {
                return false;
            }
            append_string16(
                &frame.payload,
                reinterpret_cast<const uint8_t *>(name.data()),
                name.size());
            append_string32(
                &frame.payload,
                reinterpret_cast<const uint8_t *>(value.data()),
                value.size());
        }
        if (frame.payload.size() > g_worker->config_.max_header_bytes) {
            return false;
        }
        return g_worker->queue_output(frame);
    }

    static JSValue js_response_head(JSContext *ctx,
                                    JSValueConst,
                                    int argc,
                                    JSValueConst *argv) {
        uint64_t id = 0;
        uint32_t status = 0;
        if (!g_worker || argc < 4 || JS_ToBigUint64(ctx, &id, argv[0]) ||
            JS_ToUint32(ctx, &status, argv[1]) || id == 0 || status > 999) {
            return JS_EXCEPTION;
        }
        if (!g_worker->require_active_request(ctx, id, true, false, "js_response_head")) {
            return JS_EXCEPTION;
        }
        std::map<uint64_t, ResponseState>::iterator head_state =
            g_worker->responses_.find(id);
        if (head_state == g_worker->responses_.end()) {
            return JS_UNDEFINED;
        }
        head_state->second.t_head_ns = uv_hrtime();

        if (!build_response_head(ctx, id, status, argv[2], argv[3])) {
            return JS_ThrowInternalError(
                ctx, "response head encoding failed");
        }
        return JS_UNDEFINED;
    }

    // Ordinary single-shot response for non-streamed bodies. This is kept
    // separate from js_response_fixed so the high-volume large-body path
    // retains its original head-first pipeline and has no fixed-body branch.
    static JSValue js_response_final(JSContext *ctx,
                                     JSValueConst,
                                     int argc,
                                     JSValueConst *argv) {
        uint64_t id = 0;
        uint32_t status = 0;
        if (!g_worker || argc < 5 || JS_ToBigUint64(ctx, &id, argv[0]) ||
            JS_ToUint32(ctx, &status, argv[1]) || id == 0 || status > 999) {
            return JS_EXCEPTION;
        }
        if (!g_worker->require_active_request(ctx, id, true, false, "js_response_final")) {
            return JS_EXCEPTION;
        }
        std::map<uint64_t, ResponseState>::iterator state =
            g_worker->responses_.find(id);
        if (state == g_worker->responses_.end()) {
            return JS_UNDEFINED;
        }
        state->second.t_head_ns = uv_hrtime();
        if (!build_response_head(ctx, id, status, argv[2], argv[3])) {
            return JS_ThrowInternalError(
                ctx, "response head encoding failed");
        }
        // Body: Uint8Array or string; both are encoded/read here, then
        // pushed through the fast path with the remainder snapshotted
        // for credit-driven advancement. Untouched string Responses arrive
        // here as strings so the common ASCII case can use QuickJS's stable
        // string storage directly, without a JS TextEncoder allocation and
        // a second Uint8Array copy.
        size_t body_size = 0;
        const uint8_t *body_bytes = NULL;
        const char *body_text = NULL;
        std::vector<uint8_t> normalized_body;
        if (!JS_IsNull(argv[4]) && !JS_IsUndefined(argv[4])) {
            if (JS_IsString(argv[4])) {
                body_text = JS_ToCStringLen(ctx, &body_size, argv[4]);
                if (!body_text) {
                    return JS_EXCEPTION;
                }
                body_bytes = reinterpret_cast<const uint8_t *>(body_text);

                // JS_ToCStringLen preserves unmatched UTF-16 surrogate code
                // points as their three-byte UTF-8 encodings. Fetch's
                // TextEncoder semantics require each unmatched surrogate to
                // become U+FFFD instead. Valid pairs have already become one
                // four-byte scalar, so only the UTF-8 surrogate range needs
                // rewriting; replacement is also three bytes and never
                // changes the body length. Allocate only on this rare path.
                const size_t surrogate_search_size =
                    body_size > 2 ? body_size - 2 : 0;
                size_t search_offset = 0;
                while (search_offset < surrogate_search_size) {
                    const void *found = std::memchr(
                        body_bytes + search_offset,
                        0xed,
                        surrogate_search_size - search_offset);
                    if (!found) {
                        break;
                    }
                    const size_t i =
                        static_cast<const uint8_t *>(found) - body_bytes;
                    if (body_bytes[i] == 0xed &&
                        body_bytes[i + 1] >= 0xa0 &&
                        body_bytes[i + 1] <= 0xbf &&
                        body_bytes[i + 2] >= 0x80 &&
                        body_bytes[i + 2] <= 0xbf) {
                        if (normalized_body.empty()) {
                            normalized_body.assign(
                                body_bytes, body_bytes + body_size);
                        }
                        normalized_body[i] = 0xef;
                        normalized_body[i + 1] = 0xbf;
                        normalized_body[i + 2] = 0xbd;
                    }
                    search_offset = i + 1;
                }
                if (!normalized_body.empty()) {
                    body_bytes = &normalized_body[0];
                }
            } else {
                body_bytes = JS_GetUint8Array(ctx, &body_size, argv[4]);
            }
        }
        if (body_bytes != NULL && body_size > 0) {
            size_t fast_sent = 0;
            const EnqueueResult result = g_worker->queue_response_bytes_fast(
                id, body_bytes, body_size, &state->second, &fast_sent);
            if (result == EnqueueResult::kFatal) {
                if (body_text) {
                    JS_FreeCString(ctx, body_text);
                }
                return JS_ThrowInternalError(ctx, "response output is wedged");
            }
            if (result == EnqueueResult::kWouldBlock) {
                PendingWrite pending;
                pending.data.assign(body_bytes + fast_sent,
                                    body_bytes + body_size);
                pending.offset = 0;
                pending.size = pending.data.size();
                pending.resolve = JS_UNDEFINED;
                pending.reject = JS_UNDEFINED;
                state->second.pending.push_back(std::move(pending));
                g_worker->enqueue_pump(id);
            }
        }
        if (body_text) {
            JS_FreeCString(ctx, body_text);
        }
        TerminalPending terminal;
        terminal.kind = TerminalPending::Kind::kResponseEnd;
        terminal.error_flags = 0;
        g_worker->queue_terminal_or_defer(id, terminal);
        return JS_UNDEFINED;
    }

    // Bounded single-shot response. The JS bootstrap calls this entry only
    // when its representation proves the body may fit the fixed-response
    // bound; native still checks the exact encoded byte count.
    static JSValue js_response_fixed(JSContext *ctx,
                                     JSValueConst,
                                     int argc,
                                     JSValueConst *argv) {
        uint64_t id = 0;
        uint32_t status = 0;
        if (!g_worker || argc < 5 || JS_ToBigUint64(ctx, &id, argv[0]) ||
            JS_ToUint32(ctx, &status, argv[1]) || id == 0 || status > 999) {
            return JS_EXCEPTION;
        }
        if (!g_worker->require_active_request(ctx, id, true, false, "js_response_final")) {
            return JS_EXCEPTION;
        }
        std::map<uint64_t, ResponseState>::iterator state =
            g_worker->responses_.find(id);
        if (state == g_worker->responses_.end()) {
            return JS_UNDEFINED;
        }
        state->second.t_head_ns = uv_hrtime();
        capsid::protocol::Frame head;
        if (!build_fixed_response_head(
                ctx, id, status, argv[2], argv[3], &head)) {
            return JS_ThrowInternalError(
                ctx, "response head encoding failed");
        }
        // Body: Uint8Array or string; both are encoded/read here, then
        // pushed through the fast path with the remainder snapshotted
        // for credit-driven advancement. Untouched string Responses arrive
        // here as strings so the common ASCII case can use QuickJS's stable
        // string storage directly, without a JS TextEncoder allocation and
        // a second Uint8Array copy.
        size_t body_size = 0;
        const uint8_t *body_bytes = NULL;
        const char *body_text = NULL;
        std::vector<uint8_t> normalized_body;
        if (!JS_IsNull(argv[4]) && !JS_IsUndefined(argv[4])) {
            if (JS_IsString(argv[4])) {
                body_text = JS_ToCStringLen(ctx, &body_size, argv[4]);
                if (!body_text) {
                    return JS_EXCEPTION;
                }
                body_bytes = reinterpret_cast<const uint8_t *>(body_text);

                // JS_ToCStringLen preserves unmatched UTF-16 surrogate code
                // points as their three-byte UTF-8 encodings. Fetch's
                // TextEncoder semantics require each unmatched surrogate to
                // become U+FFFD instead. Valid pairs have already become one
                // four-byte scalar, so only the UTF-8 surrogate range needs
                // rewriting; replacement is also three bytes and never
                // changes the body length. Allocate only on this rare path.
                const size_t surrogate_search_size =
                    body_size > 2 ? body_size - 2 : 0;
                size_t search_offset = 0;
                while (search_offset < surrogate_search_size) {
                    const void *found = std::memchr(
                        body_bytes + search_offset,
                        0xed,
                        surrogate_search_size - search_offset);
                    if (!found) {
                        break;
                    }
                    const size_t i =
                        static_cast<const uint8_t *>(found) - body_bytes;
                    if (body_bytes[i] == 0xed &&
                        body_bytes[i + 1] >= 0xa0 &&
                        body_bytes[i + 1] <= 0xbf &&
                        body_bytes[i + 2] >= 0x80 &&
                        body_bytes[i + 2] <= 0xbf) {
                        if (normalized_body.empty()) {
                            normalized_body.assign(
                                body_bytes, body_bytes + body_size);
                        }
                        normalized_body[i] = 0xef;
                        normalized_body[i + 1] = 0xbf;
                        normalized_body[i + 2] = 0xbd;
                    }
                    search_offset = i + 1;
                }
                if (!normalized_body.empty()) {
                    body_bytes = &normalized_body[0];
                }
            } else {
                body_bytes = JS_GetUint8Array(ctx, &body_size, argv[4]);
            }
        }
        const bool fixed_body =
            body_size <= capsid::protocol::kMaxFixedBodySize &&
            body_size <= state->second.credit;
        if (fixed_body) {
            if (g_worker->config_.max_header_bytes < sizeof(uint32_t) ||
                head.payload.size() >
                    g_worker->config_.max_header_bytes - sizeof(uint32_t)) {
                if (body_text) {
                    JS_FreeCString(ctx, body_text);
                }
                return JS_ThrowInternalError(
                    ctx, "response head encoding failed");
            }
            head.flags = capsid::protocol::kFlagResponseFixedBody;
            const uint32_t size = static_cast<uint32_t>(body_size);
            head.payload.insert(head.payload.begin() + 2, 4, 0);
            head.payload[2] = static_cast<uint8_t>(size);
            head.payload[3] = static_cast<uint8_t>(size >> 8);
            head.payload[4] = static_cast<uint8_t>(size >> 16);
            head.payload[5] = static_cast<uint8_t>(size >> 24);
        }
        if (!g_worker->queue_output(head)) {
            if (body_text) {
                JS_FreeCString(ctx, body_text);
            }
            return JS_ThrowInternalError(
                ctx, "response head encoding failed");
        }
        if (body_bytes != NULL && body_size > 0) {
            size_t fast_sent = 0;
            const EnqueueResult result = g_worker->queue_response_bytes_fast(
                id, body_bytes, body_size, &state->second, &fast_sent);
            if (result == EnqueueResult::kFatal) {
                if (body_text) {
                    JS_FreeCString(ctx, body_text);
                }
                return JS_ThrowInternalError(ctx, "response output is wedged");
            }
            if (result == EnqueueResult::kWouldBlock) {
                PendingWrite pending;
                pending.data.assign(body_bytes + fast_sent,
                                    body_bytes + body_size);
                pending.offset = 0;
                pending.size = pending.data.size();
                pending.resolve = JS_UNDEFINED;
                pending.reject = JS_UNDEFINED;
                state->second.pending.push_back(std::move(pending));
                g_worker->enqueue_pump(id);
            }
        }
        if (body_text) {
            JS_FreeCString(ctx, body_text);
        }
        // End terminal: waits for the body to drain, then sends the
        // ResponseEnd frame (existing machinery).
        TerminalPending terminal;
        terminal.kind = TerminalPending::Kind::kResponseEnd;
        terminal.error_flags = 0;
        g_worker->queue_terminal_or_defer(id, terminal);
        return JS_UNDEFINED;
    }

    static JSValue js_response_write(JSContext *ctx,
                                     JSValueConst,
                                     int argc,
                                     JSValueConst *argv) {
        uint64_t id = 0;
        size_t size = 0;
        if (!g_worker || argc < 2 || JS_ToBigUint64(ctx, &id, argv[0]) || id == 0) {
            return JS_EXCEPTION;
        }
        if (!g_worker->require_active_request(ctx, id, true, false, "js_response_write")) {
            return JS_EXCEPTION;
        }
        std::map<uint64_t, ResponseState>::iterator state =
            g_worker->responses_.find(id);
        if (state == g_worker->responses_.end()) {
            return JS_UNDEFINED;
        }
        uint8_t *bytes = JS_GetUint8Array(ctx, &size, argv[1]);
        if (!bytes) {
            return JS_EXCEPTION;
        }
        if (size == 0) {
            return JS_UNDEFINED;
        }

        // Contract #3: pressure must never raise RangeError. The blocked
        // path returns a promise that stays pending while the bytes await
        // credit / wire space and resolves once the whole chunk has been
        // accepted. The unblocked path returns undefined: the chunk was
        // accepted synchronously, so the bootstrap skips the promise
        // capability, the resolve call and the await hop entirely
        // (E13a — the common case pays no promise machinery per chunk).
        size_t fast_sent = 0;
        const EnqueueResult result = g_worker->queue_response_bytes_fast(
            id, bytes, size, &state->second, &fast_sent);
        if (result == EnqueueResult::kQueued) {
            return JS_UNDEFINED;
        }
        if (result == EnqueueResult::kFatal) {
            return JS_ThrowInternalError(ctx, "response output is wedged");
        }
        // kWouldBlock: the promise capability is created only here, then
        // snapshot the bytes in the JS heap (design §3.2) so the
        // application mutating the source array while the promise is
        // pending cannot change the response bytes; the snapshot also
        // keeps the data alive without GC pressure on the caller's
        // buffer. The pump advances segments from the snapshot.
        // Snapshot only the unsent remainder (call-time copy): the
        // fast path already accepted the first `fast_sent` bytes into
        // the wire queue, and copying the rest once is cheaper than a
        // JS API call per pump advance.
        JSValue resolving[2];
        JSValue promise = JS_NewPromiseCapability(ctx, resolving);
        if (JS_IsException(promise)) {
            return promise;
        }
        PendingWrite pending;
        pending.data.assign(bytes + fast_sent, bytes + size);
        pending.offset = 0;
        pending.size = pending.data.size();
        pending.resolve = resolving[0];
        pending.reject = resolving[1];
        state->second.pending.push_back(std::move(pending));
        g_worker->enqueue_pump(id);
        g_worker->pump_one(id);
        return promise;
    }

    static JSValue js_response_end(JSContext *ctx,
                                   JSValueConst,
                                   int argc,
                                   JSValueConst *argv) {
        uint64_t id = 0;
        if (!g_worker || argc < 1 || JS_ToBigUint64(ctx, &id, argv[0]) || id == 0) {
            return JS_EXCEPTION;
        }
        if (!g_worker->require_active_request(ctx, id, true, false, "js_response_end")) {
            return JS_EXCEPTION;
        }
        std::map<uint64_t, ResponseState>::iterator state =
            g_worker->responses_.find(id);
        if (state == g_worker->responses_.end()) {
            return JS_UNDEFINED;
        }
        // Contract #5/#6: a terminal must never be dropped because the
        // queue is full. While body bytes are still pending, the end is
        // deferred until they drain; the pump then sends it. Never throw
        // "not ready to end" either.
        TerminalPending terminal;
        terminal.kind = TerminalPending::Kind::kResponseEnd;
        terminal.error_flags = 0;
        g_worker->queue_terminal_or_defer(id, terminal);
        return JS_UNDEFINED;
    }

    static JSValue js_response_error(JSContext *ctx,
                                     JSValueConst,
                                     int argc,
                                     JSValueConst *argv) {
        uint64_t id = 0;
        if (!g_worker || argc < 2 || JS_ToBigUint64(ctx, &id, argv[0]) || id == 0) {
            return JS_EXCEPTION;
        }
        if (!g_worker->require_active_request(ctx, id, true, false, "js_response_error")) {
            return JS_EXCEPTION;
        }
        std::map<uint64_t, ResponseState>::iterator state =
            g_worker->responses_.find(id);
        if (state == g_worker->responses_.end()) {
            return JS_UNDEFINED;
        }
        const bool timed_out =
            g_worker->interrupted_request_id_ == id;
        const std::string message = to_string(ctx, argv[1]);
        const uint32_t flags =
            timed_out ? capsid::protocol::kErrorFlagTimeout : 0;
        // Discard the unsent body, then guarantee the error terminal.
        g_worker->reject_pending(
            state->second,
            timed_out ? "request timed out" : "response failed");
        TerminalPending terminal;
        terminal.kind = TerminalPending::Kind::kResponseError;
        terminal.message = message;
        terminal.error_flags = flags;
        g_worker->queue_terminal_or_defer(id, terminal);
        if (timed_out) {
            g_worker->interrupted_request_id_ = 0;
        }
        return JS_UNDEFINED;
    }

    static std::string to_string(JSContext *ctx, JSValueConst value) {
        const char *text = JS_ToCString(ctx, value);
        if (!text) {
            return std::string();
        }
        const std::string result(text);
        JS_FreeCString(ctx, text);
        return result;
    }

    static bool to_bytes(
        JSContext *ctx,
        JSValueConst value,
        std::string *result) {
        size_t size = 0;
        const char *text =
            JS_ToCStringLen(ctx, &size, value);
        if (!text) {
            return false;
        }
        result->assign(text, size);
        JS_FreeCString(ctx, text);
        return true;
    }

    static void append_string16(std::vector<uint8_t> *output,
                                const uint8_t *data,
                                size_t size) {
        capsid::protocol::append_u16(output, static_cast<uint16_t>(size));
        if (size != 0) {
            output->insert(output->end(), data, data + size);
        }
    }

    static void append_string32(std::vector<uint8_t> *output,
                                const uint8_t *data,
                                size_t size) {
        capsid::protocol::append_u32(output, static_cast<uint32_t>(size));
        if (size != 0) {
            output->insert(output->end(), data, data + size);
        }
    }

    static capsid_audit_decision audit_decision(
        capsid_permission_state state) {
        switch (state) {
            case CAPSID_PERMISSION_STATE_GRANTED:
                return CAPSID_AUDIT_ALLOW;
            case CAPSID_PERMISSION_STATE_PARTIAL:
                return CAPSID_AUDIT_PARTIAL;
            case CAPSID_PERMISSION_STATE_UNAVAILABLE:
                return CAPSID_AUDIT_UNAVAILABLE;
            case CAPSID_PERMISSION_STATE_DENIED:
                return CAPSID_AUDIT_DENY;
        }
        return CAPSID_AUDIT_DENY;
    }

    void emit_audit(capsid_audit_stage stage,
                    capsid_audit_decision decision,
                    uint64_t request_id,
                    uint32_t rule_id,
                    const std::string &module,
                    const std::string &capability,
                    const std::string &resource_kind,
                    const std::string &resource) {
#if defined(_WIN32)
        // debug: drop startup audits to isolate ERROR_ALREADY_EXISTS
        if (!poll_started_) {
            return;
        }
#endif
        const uint64_t now = uv_hrtime();
        if (audit_window_started_ns_ == 0 ||
            now - audit_window_started_ns_ >=
                UINT64_C(1000000000)) {
            audit_window_started_ns_ = now;
            audit_window_count_ = 0;
            audit_repeat_key_.clear();
            audit_repeat_count_ = 0;
        }
        if (decision != CAPSID_AUDIT_ALLOW) {
            // §6.3: the dedup key must carry request identity, otherwise
            // one repeated deny across two requests collapses into one
            // audit. Worker-scope audits (id 0) still dedup per rule.
            std::string repeat_key =
                std::to_string(request_id) + ":" +
                std::to_string(static_cast<uint32_t>(stage)) +
                ":" +
                std::to_string(static_cast<uint32_t>(decision)) +
                ":" + std::to_string(rule_id);
            const std::string *repeat_fields[] = {
                &module,
                &capability,
                &resource_kind,
                &resource
            };
            for (size_t index = 0;
                 index <
                     sizeof(repeat_fields) /
                         sizeof(repeat_fields[0]);
                 ++index) {
                repeat_key.push_back('\0');
                repeat_key.append(*repeat_fields[index]);
            }
            if (repeat_key == audit_repeat_key_) {
                ++audit_repeat_count_;
            } else {
                audit_repeat_key_.swap(repeat_key);
                audit_repeat_count_ = 1;
            }
            if (audit_repeat_count_ > 8) {
                return;
            }
        }
        /* Audit output itself must not become an application-controlled DoS. */
        if (audit_window_count_ >= 64) {
            return;
        }
        ++audit_window_count_;

        capsid::protocol::Frame frame;
        frame.type = capsid::protocol::kAudit;
        frame.flags = 0;
        frame.request_id = request_id;
        capsid::protocol::append_u32(&frame.payload, 1);
        capsid::protocol::append_u32(
            &frame.payload, static_cast<uint32_t>(stage));
        capsid::protocol::append_u32(
            &frame.payload, static_cast<uint32_t>(decision));
        capsid::protocol::append_u64(
            &frame.payload,
#if defined(_WIN32)
            static_cast<uint64_t>(capsid::win32::getpid()));
#else
            static_cast<uint64_t>(getpid()));
#endif
        capsid::protocol::append_u32(&frame.payload, rule_id);
        capsid::protocol::append_u32(
            &frame.payload,
            config_.capability_policy.enabled()
                ? config_.capability_policy.version()
                : 0);
        const std::string &application =
            config_.capability_policy.application_identity();
        const std::string manifest(
            capsid::capability_manifest_hash());
        const std::string *fields[] = {
            &application,
            &module,
            &capability,
            &resource_kind,
            &resource,
            &manifest
        };
        for (size_t index = 0;
             index < sizeof(fields) / sizeof(fields[0]);
             ++index) {
            append_string16(
                &frame.payload,
                reinterpret_cast<const uint8_t *>(
                    fields[index]->data()),
                fields[index]->size());
        }
        queue_output(frame);
    }

    bool read_startup(bool require_bundle) {
        while (!startup_state_.hello_received() ||
               (require_bundle &&
                !startup_state_.bundle_complete())) {
            capsid::protocol::Frame frame;
            const capsid::protocol::ParseResult result = parser_.next(&frame);
            if (result == capsid::protocol::kParseError) {
                return false;
            }
            if (result == capsid::protocol::kParseFrame) {
                if (!handle_startup_frame(frame)) {
                    return false;
                }
                continue;
            }
            uint8_t buffer[64 * 1024];
            const ssize_t count =
#if defined(_WIN32)
                capsid::win32::read_fd(fd_, buffer, sizeof(buffer));
#else
                read(fd_, buffer, sizeof(buffer));
#endif
            if (count > 0) {
                if (!parser_.append(buffer, static_cast<size_t>(count))) {
                    return false;
                }
                continue;
            }
            if (count < 0 && errno == EINTR) {
                continue;
            }
            if (count < 0 && errno == EAGAIN) {
                // The channel may arrive nonblocking on Windows before
                // set_nonblocking() has been reached (line 532); wait for
                // readability instead of treating the transient state as
                // a dead worker.
                capsid_pollfd descriptor = {};
                descriptor.fd = fd_;
                descriptor.events = POLLIN;
                const int polled =
                    capsid::win32::capsid_poll(&descriptor, 1, 2000);
                if (polled > 0 && (descriptor.revents & POLLIN) != 0) {
                    continue;
                }
            }
            return false;
        }
        return true;
    }

    bool handle_startup_frame(const capsid::protocol::Frame &frame) {
        std::string error;
        if (!startup_state_.consume(frame, &error)) {
            return false;
        }
        config_ = startup_state_.config();
        if (startup_state_.bundle_complete()) {
            bundle_.swap(startup_state_.bundle());
            bundle_name_ = startup_state_.bundle_name();
            bundle_is_trusted_bytecode_ =
                startup_state_.bundle_is_trusted_bytecode();
        }
        return true;
    }

    void set_nonblocking() {
#if defined(_WIN32)
        capsid::win32::set_socket_nonblocking(fd_);
#else
        const int flags = fcntl(fd_, F_GETFL, 0);
        if (flags >= 0) {
            fcntl(fd_, F_SETFL, flags | O_NONBLOCK);
        }
#endif
    }

    bool load_bridge_functions(std::string *error) {
        if (!JS_IsFunction(ctx_, begin_request_) ||
            !JS_IsFunction(ctx_, request_chunk_) ||
            !JS_IsFunction(ctx_, request_end_) ||
            !JS_IsFunction(ctx_, cancel_request_)) {
            *error = "Capsid bootstrap did not install its request bridge";
            return false;
        }
        return true;
    }

    void free_bridge_functions() {
        JS_FreeValue(ctx_, application_handler_);
        JS_FreeValue(ctx_, application_handler_this_);
        JS_FreeValue(ctx_, begin_request_);
        JS_FreeValue(ctx_, request_chunk_);
        JS_FreeValue(ctx_, request_end_);
        JS_FreeValue(ctx_, cancel_request_);
        application_handler_ = JS_UNDEFINED;
        application_handler_this_ = JS_UNDEFINED;
        begin_request_ = JS_UNDEFINED;
        request_chunk_ = JS_UNDEFINED;
        request_end_ = JS_UNDEFINED;
        cancel_request_ = JS_UNDEFINED;
    }

    static bool permission_from_name(
        const std::string &name,
        capsid_permission_name *permission) {
        if (!permission) {
            return false;
        }
        struct Entry {
            const char *name;
            capsid_permission_name permission;
        };
        static const Entry entries[] = {
            { "read", CAPSID_PERMISSION_READ },
            { "write", CAPSID_PERMISSION_WRITE },
            { "net", CAPSID_PERMISSION_NET },
            { "env", CAPSID_PERMISSION_ENV },
            { "sys", CAPSID_PERMISSION_SYS },
            { "ffi", CAPSID_PERMISSION_FFI },
            { "rawSocket", CAPSID_PERMISSION_RAW_SOCKET },
            { "stdio", CAPSID_PERMISSION_STDIO },
            { "storage", CAPSID_PERMISSION_STORAGE },
            { "engine", CAPSID_PERMISSION_ENGINE }
        };
        for (size_t index = 0;
             index < sizeof(entries) / sizeof(entries[0]);
             ++index) {
            if (name == entries[index].name) {
                *permission = entries[index].permission;
                return true;
            }
        }
        return false;
    }

    capsid::PermissionDecision effective_query(
        capsid_permission_name permission,
        const std::string &resource,
        uint16_t port) const {
        capsid::PermissionDecision decision =
            config_.capability_policy.query(
                permission, resource, port);
        if (permission != CAPSID_PERMISSION_NET ||
            !config_.legacy_egress_configured) {
            return decision;
        }
        if (resource.empty() || port == 0) {
            const capsid_permission_state legacy =
                config_.egress_policy.query_state();
            if (decision.state == CAPSID_PERMISSION_STATE_DENIED ||
                legacy == CAPSID_PERMISSION_STATE_DENIED) {
                decision.state = CAPSID_PERMISSION_STATE_DENIED;
            } else if (
                decision.state == CAPSID_PERMISSION_STATE_GRANTED &&
                legacy == CAPSID_PERMISSION_STATE_GRANTED) {
                decision.state = CAPSID_PERMISSION_STATE_GRANTED;
            } else {
                decision.state = CAPSID_PERMISSION_STATE_PARTIAL;
            }
            decision.rule_id = 0;
            return decision;
        }
        const capsid::EgressDecision legacy =
            config_.egress_policy.decide_host(resource, port);
        if (decision.state == CAPSID_PERMISSION_STATE_GRANTED &&
            !legacy.allowed) {
            decision.state = CAPSID_PERMISSION_STATE_DENIED;
            decision.rule_id = legacy.rule_id;
        }
        return decision;
    }

    static JSValue js_permission_query(
        JSContext *ctx,
        JSValueConst,
        int argc,
        JSValueConst *argv) {
        if (!g_worker || argc < 1 ||
            !JS_IsObject(argv[0])) {
            return JS_ThrowTypeError(
                ctx, "permission query requires a descriptor");
        }
        JSValue name_value =
            JS_GetPropertyStr(ctx, argv[0], "name");
        if (JS_IsException(name_value)) {
            return JS_EXCEPTION;
        }
        if (!JS_IsString(name_value)) {
            JS_FreeValue(ctx, name_value);
            return JS_ThrowTypeError(
                ctx, "permission name must be a string");
        }
        const std::string name = to_string(ctx, name_value);
        JS_FreeValue(ctx, name_value);

        // §6.3: permission queries are worker-scope legal (module load)
        // but must be rejected from terminal requests.
        if (!g_worker->require_active_request(ctx, 0, false, true) &&
            JS_HasException(ctx)) {
            return JS_EXCEPTION;
        }

        capsid_permission_name permission = CAPSID_PERMISSION_NONE;
        if (!permission_from_name(name, &permission)) {
            g_worker->emit_audit(
                CAPSID_AUDIT_STAGE_QUERY,
                CAPSID_AUDIT_UNAVAILABLE,
                g_worker->active_request_id(),
                0,
                "capsid:permissions",
                name,
                "unknown",
                std::string());
            return JS_ThrowRangeError(
                ctx, "unknown permission: %s", name.c_str());
        }

        std::string resource;
        uint16_t port = 0;
        const char *property = NULL;
        if (permission == CAPSID_PERMISSION_NET) {
            JSValue host =
                JS_GetPropertyStr(ctx, argv[0], "host");
            JSValue port_value =
                JS_GetPropertyStr(ctx, argv[0], "port");
            if (JS_IsException(host) ||
                JS_IsException(port_value)) {
                JS_FreeValue(ctx, host);
                JS_FreeValue(ctx, port_value);
                return JS_EXCEPTION;
            }
            const bool has_host = !JS_IsUndefined(host);
            const bool has_port = !JS_IsUndefined(port_value);
            if (has_host != has_port ||
                (has_host && !JS_IsString(host))) {
                JS_FreeValue(ctx, host);
                JS_FreeValue(ctx, port_value);
                return JS_ThrowTypeError(
                    ctx,
                    "net query requires both host and port");
            }
            if (has_host) {
                resource = to_string(ctx, host);
                uint32_t port_number = 0;
                if (JS_ToUint32(
                        ctx, &port_number, port_value) ||
                    port_number == 0 ||
                    port_number > 65535) {
                    JS_FreeValue(ctx, host);
                    JS_FreeValue(ctx, port_value);
                    return JS_ThrowRangeError(
                        ctx, "invalid net query port");
                }
                port = static_cast<uint16_t>(port_number);
            }
            JS_FreeValue(ctx, host);
            JS_FreeValue(ctx, port_value);
        } else {
            switch (permission) {
                case CAPSID_PERMISSION_READ:
                case CAPSID_PERMISSION_WRITE:
                case CAPSID_PERMISSION_FFI:
                    property = "path";
                    break;
                case CAPSID_PERMISSION_ENV:
                    property = "variable";
                    break;
                case CAPSID_PERMISSION_SYS:
                    property = "kind";
                    break;
                case CAPSID_PERMISSION_STDIO:
                    property = "stream";
                    break;
                case CAPSID_PERMISSION_STORAGE:
                    property = "namespace";
                    break;
                case CAPSID_PERMISSION_ENGINE:
                    property = "operation";
                    break;
                case CAPSID_PERMISSION_RAW_SOCKET:
                case CAPSID_PERMISSION_NONE:
                case CAPSID_PERMISSION_NET:
                    break;
            }
            if (property) {
                JSValue value =
                    JS_GetPropertyStr(ctx, argv[0], property);
                if (JS_IsException(value)) {
                    return JS_EXCEPTION;
                }
                if (!JS_IsUndefined(value)) {
                    if (!JS_IsString(value)) {
                        JS_FreeValue(ctx, value);
                        return JS_ThrowTypeError(
                            ctx,
                            "permission resource must be a string");
                    }
                    resource = to_string(ctx, value);
                }
                JS_FreeValue(ctx, value);
            }
        }

        const capsid::PermissionDecision decision =
            g_worker->effective_query(
                permission, resource, port);
        const std::string audit_resource =
            permission == CAPSID_PERMISSION_NET &&
                    !resource.empty()
                ? resource + ":" + std::to_string(port)
                : resource;
        g_worker->emit_audit(
            CAPSID_AUDIT_STAGE_QUERY,
            audit_decision(decision.state),
            g_worker->active_request_id(),
            decision.rule_id,
            "capsid:permissions",
            capsid::permission_name(permission),
            capsid::permission_resource_kind(permission),
            audit_resource);
        return JS_NewString(
            ctx, capsid::permission_state_name(decision.state));
    }

    static int permissions_module_init(
        JSContext *ctx,
        JSModuleDef *module) {
        JSValue permissions = JS_NewObject(ctx);
        if (JS_IsException(permissions)) {
            return -1;
        }
        const int defined = JS_DefinePropertyValueStr(
            ctx,
            permissions,
            "query",
            JS_NewCFunction(
                ctx,
                js_permission_query,
                "query",
                1),
            JS_PROP_ENUMERABLE);
        if (defined <= 0 ||
            JS_FreezeObject(ctx, permissions) < 0) {
            JS_FreeValue(ctx, permissions);
            return -1;
        }
        return JS_SetModuleExport(
            ctx,
            module,
            "permissions",
            permissions);
    }

    static JSValue js_env_get(
        JSContext *ctx,
        JSValueConst,
        int argc,
        JSValueConst *argv) {
        if (!g_worker || argc < 1 ||
            !JS_IsString(argv[0])) {
            return JS_ThrowTypeError(
                ctx,
                "environment variable name must be a string");
        }
        const std::string name =
            to_string(ctx, argv[0]);
        const capsid::PermissionDecision decision =
            g_worker->config_.capability_policy.evaluate(
                CAPSID_PERMISSION_ENV,
                name);
        if (decision.resource != name ||
            name.find('*') != std::string::npos) {
            return JS_ThrowRangeError(
                ctx,
                "invalid environment variable name");
        }
        if (decision.state !=
            CAPSID_PERMISSION_STATE_GRANTED) {
            g_worker->emit_audit(
                CAPSID_AUDIT_STAGE_OPERATION,
                CAPSID_AUDIT_DENY,
                g_worker->active_request_id(),
                decision.rule_id,
                "capsid:env",
                "env",
                "variable",
                decision.resource);
            return JS_ThrowReferenceError(
                ctx,
                "environment access denied: %s",
                name.c_str());
        }
        std::string value;
        const bool present =
            g_worker->config_.capability_policy.env_value(
                name,
                &value);
        g_worker->emit_audit(
            CAPSID_AUDIT_STAGE_OPERATION,
            CAPSID_AUDIT_ALLOW,
            g_worker->active_request_id(),
            decision.rule_id,
            "capsid:env",
            "env",
            "variable",
            decision.resource);
        if (!present) {
            return JS_UNDEFINED;
        }
        return JS_NewStringLen(
            ctx,
            value.data(),
            value.size());
    }

    static int env_module_init(
        JSContext *ctx,
        JSModuleDef *module) {
        JSValue environment = JS_NewObject(ctx);
        if (JS_IsException(environment)) {
            return -1;
        }
        const int defined = JS_DefinePropertyValueStr(
            ctx,
            environment,
            "get",
            JS_NewCFunction(
                ctx,
                js_env_get,
                "get",
                1),
            JS_PROP_ENUMERABLE);
        if (defined <= 0 ||
            JS_FreezeObject(ctx, environment) < 0) {
            JS_FreeValue(ctx, environment);
            return -1;
        }
        return JS_SetModuleExport(
            ctx,
            module,
            "env",
            environment);
    }

    static JSValue js_system_get(
        JSContext *ctx,
        JSValueConst,
        int argc,
        JSValueConst *argv) {
        if (!g_worker || argc < 1 ||
            !JS_IsString(argv[0])) {
            return JS_ThrowTypeError(
                ctx,
                "system information kind must be a string");
        }
        const std::string kind =
            to_string(ctx, argv[0]);
        const bool supported =
            kind == "runtimeVersion" ||
            kind == "featureFlags";
        if (!supported) {
            g_worker->emit_audit(
                CAPSID_AUDIT_STAGE_OPERATION,
                CAPSID_AUDIT_UNAVAILABLE,
                g_worker->active_request_id(),
                0,
                "capsid:system",
                "sys",
                "kind",
                kind);
            return JS_ThrowRangeError(
                ctx,
                "system information is unavailable: %s",
                kind.c_str());
        }
        const capsid::PermissionDecision decision =
            g_worker->config_.capability_policy.query(
                CAPSID_PERMISSION_SYS,
                kind,
                0);
        if (decision.state !=
            CAPSID_PERMISSION_STATE_GRANTED) {
            g_worker->emit_audit(
                CAPSID_AUDIT_STAGE_OPERATION,
                decision.state ==
                        CAPSID_PERMISSION_STATE_UNAVAILABLE
                    ? CAPSID_AUDIT_UNAVAILABLE
                    : CAPSID_AUDIT_DENY,
                g_worker->active_request_id(),
                decision.rule_id,
                "capsid:system",
                "sys",
                "kind",
                kind);
            return JS_ThrowReferenceError(
                ctx,
                "system information access denied: %s",
                kind.c_str());
        }
        g_worker->emit_audit(
            CAPSID_AUDIT_STAGE_OPERATION,
            CAPSID_AUDIT_ALLOW,
            g_worker->active_request_id(),
            decision.rule_id,
            "capsid:system",
            "sys",
            "kind",
            kind);
        if (kind == "runtimeVersion") {
            return JS_NewString(
                ctx, CAPSID_RUNTIME_VERSION);
        }

        JSValue flags = JS_NewObject(ctx);
        if (JS_IsException(flags)) {
            return JS_EXCEPTION;
        }
        const int profile = JS_DefinePropertyValueStr(
            ctx,
            flags,
            "profile",
            JS_NewString(
                ctx,
                "CAPSID-MIN-2025-subset-v0"),
            JS_PROP_ENUMERABLE);
        const int bytecode = JS_DefinePropertyValueStr(
            ctx,
            flags,
            "trustedBytecode",
            JS_NewBool(ctx, true),
            JS_PROP_ENUMERABLE);
        const int wasm = JS_DefinePropertyValueStr(
            ctx,
            flags,
            "wasm",
            JS_NewBool(ctx, true),
            JS_PROP_ENUMERABLE);
        const int policy = JS_DefinePropertyValueStr(
            ctx,
            flags,
            "capabilityPolicyVersion",
            JS_NewUint32(
                ctx,
                CAPSID_CAPABILITY_POLICY_VERSION),
            JS_PROP_ENUMERABLE);
        if (profile <= 0 || bytecode <= 0 ||
            wasm <= 0 || policy <= 0 ||
            JS_FreezeObject(ctx, flags) < 0) {
            JS_FreeValue(ctx, flags);
            return JS_EXCEPTION;
        }
        return flags;
    }

    static int system_module_init(
        JSContext *ctx,
        JSModuleDef *module) {
        JSValue system = JS_NewObject(ctx);
        if (JS_IsException(system)) {
            return -1;
        }
        const int defined = JS_DefinePropertyValueStr(
            ctx,
            system,
            "get",
            JS_NewCFunction(
                ctx,
                js_system_get,
                "get",
                1),
            JS_PROP_ENUMERABLE);
        if (defined <= 0 ||
            JS_FreezeObject(ctx, system) < 0) {
            JS_FreeValue(ctx, system);
            return -1;
        }
        return JS_SetModuleExport(
            ctx,
            module,
            "system",
            system);
    }

    static bool storage_arguments(
        JSContext *ctx,
        int argc,
        JSValueConst *argv,
        int required,
        std::string *storage_namespace,
        std::string *key,
        std::string *value) {
        if (!g_worker || argc < required ||
            !JS_IsString(argv[0]) ||
            (required >= 2 && !JS_IsString(argv[1])) ||
            (required >= 3 && !JS_IsString(argv[2]))) {
            JS_ThrowTypeError(
                ctx,
                "storage namespace, key, and value must be strings");
            return false;
        }
        if (!to_bytes(ctx, argv[0], storage_namespace)) {
            return false;
        }
        const capsid::PermissionDecision decision =
            g_worker->config_.capability_policy.query(
                CAPSID_PERMISSION_STORAGE,
                *storage_namespace,
                0);
        if (decision.resource != *storage_namespace) {
            g_worker->emit_audit(
                CAPSID_AUDIT_STAGE_OPERATION,
                CAPSID_AUDIT_DENY,
                g_worker->active_request_id(),
                0,
                "capsid:storage",
                "storage",
                "namespace",
                "<invalid>");
            JS_ThrowRangeError(
                ctx, "invalid storage namespace");
            return false;
        }
        if (decision.state !=
            CAPSID_PERMISSION_STATE_GRANTED) {
            g_worker->emit_audit(
                CAPSID_AUDIT_STAGE_OPERATION,
                CAPSID_AUDIT_DENY,
                g_worker->active_request_id(),
                decision.rule_id,
                "capsid:storage",
                "storage",
                "namespace",
                decision.resource);
            JS_ThrowReferenceError(
                ctx,
                "storage access denied: %s",
                storage_namespace->c_str());
            return false;
        }
        if (required >= 2) {
            if (!to_bytes(ctx, argv[1], key)) {
                return false;
            }
            if (key->empty() ||
                key->size() > kStorageKeyLimit ||
                key->find('\0') != std::string::npos) {
                g_worker->emit_audit(
                    CAPSID_AUDIT_STAGE_OPERATION,
                    CAPSID_AUDIT_DENY,
                    g_worker->active_request_id(),
                    decision.rule_id,
                    "capsid:storage",
                    "storage",
                    "namespace",
                    decision.resource);
                JS_ThrowRangeError(
                    ctx, "invalid storage key");
                return false;
            }
        }
        if (required >= 3) {
            if (!to_bytes(ctx, argv[2], value)) {
                return false;
            }
            if (value->size() > kStorageValueLimit) {
                g_worker->emit_audit(
                    CAPSID_AUDIT_STAGE_OPERATION,
                    CAPSID_AUDIT_DENY,
                    g_worker->active_request_id(),
                    decision.rule_id,
                    "capsid:storage",
                    "storage",
                    "namespace",
                    decision.resource);
                JS_ThrowRangeError(
                    ctx, "storage value exceeds 16384 bytes");
                return false;
            }
        }
        if (g_worker->storage_allow_audited_
                .insert(*storage_namespace).second) {
            g_worker->emit_audit(
                CAPSID_AUDIT_STAGE_OPERATION,
                CAPSID_AUDIT_ALLOW,
                g_worker->active_request_id(),
                decision.rule_id,
                "capsid:storage",
                "storage",
                "namespace",
                decision.resource);
        }
        return true;
    }

    static JSValue js_storage_get(
        JSContext *ctx,
        JSValueConst,
        int argc,
        JSValueConst *argv) {
        std::string storage_namespace;
        std::string key;
        std::string unused;
        if (!storage_arguments(
                ctx, argc, argv, 2,
                &storage_namespace, &key, &unused)) {
            return JS_EXCEPTION;
        }
        const std::map<std::string, StorageNamespace>::const_iterator
            space = g_worker->storage_.find(storage_namespace);
        if (space == g_worker->storage_.end()) {
            return JS_UNDEFINED;
        }
        const std::map<std::string, std::string>::const_iterator entry =
            space->second.entries.find(key);
        if (entry == space->second.entries.end()) {
            return JS_UNDEFINED;
        }
        return JS_NewStringLen(
            ctx, entry->second.data(), entry->second.size());
    }

    static JSValue js_storage_set(
        JSContext *ctx,
        JSValueConst,
        int argc,
        JSValueConst *argv) {
        std::string storage_namespace;
        std::string key;
        std::string value;
        if (!storage_arguments(
                ctx, argc, argv, 3,
                &storage_namespace, &key, &value)) {
            return JS_EXCEPTION;
        }
        StorageNamespace &space =
            g_worker->storage_[storage_namespace];
        const std::map<std::string, std::string>::iterator existing =
            space.entries.find(key);
        const size_t old_size =
            existing == space.entries.end()
                ? 0
                : existing->first.size() +
                      existing->second.size();
        const size_t new_size = key.size() + value.size();
        if ((existing == space.entries.end() &&
             space.entries.size() >= kStorageEntryLimit) ||
            new_size > kStorageNamespaceQuota ||
            space.bytes - old_size >
                kStorageNamespaceQuota - new_size) {
            const capsid::PermissionDecision decision =
                g_worker->config_.capability_policy.query(
                    CAPSID_PERMISSION_STORAGE,
                    storage_namespace,
                    0);
            g_worker->emit_audit(
                CAPSID_AUDIT_STAGE_OPERATION,
                CAPSID_AUDIT_DENY,
                g_worker->active_request_id(),
                decision.rule_id,
                "capsid:storage",
                "storage",
                "namespace",
                storage_namespace);
            return JS_ThrowRangeError(
                ctx, "storage quota exceeded");
        }
        space.bytes = space.bytes - old_size + new_size;
        space.entries[key] = value;
        return JS_UNDEFINED;
    }

    static JSValue js_storage_delete(
        JSContext *ctx,
        JSValueConst,
        int argc,
        JSValueConst *argv) {
        std::string storage_namespace;
        std::string key;
        std::string unused;
        if (!storage_arguments(
                ctx, argc, argv, 2,
                &storage_namespace, &key, &unused)) {
            return JS_EXCEPTION;
        }
        std::map<std::string, StorageNamespace>::iterator space =
            g_worker->storage_.find(storage_namespace);
        if (space == g_worker->storage_.end()) {
            return JS_NewBool(ctx, false);
        }
        std::map<std::string, std::string>::iterator entry =
            space->second.entries.find(key);
        if (entry == space->second.entries.end()) {
            return JS_NewBool(ctx, false);
        }
        space->second.bytes -=
            entry->first.size() + entry->second.size();
        space->second.entries.erase(entry);
        if (space->second.entries.empty()) {
            g_worker->storage_.erase(space);
        }
        return JS_NewBool(ctx, true);
    }

    static JSValue js_storage_clear(
        JSContext *ctx,
        JSValueConst,
        int argc,
        JSValueConst *argv) {
        std::string storage_namespace;
        std::string unused_key;
        std::string unused_value;
        if (!storage_arguments(
                ctx, argc, argv, 1,
                &storage_namespace,
                &unused_key,
                &unused_value)) {
            return JS_EXCEPTION;
        }
        g_worker->storage_.erase(storage_namespace);
        return JS_UNDEFINED;
    }

    static JSValue js_storage_keys(
        JSContext *ctx,
        JSValueConst,
        int argc,
        JSValueConst *argv) {
        std::string storage_namespace;
        std::string unused_key;
        std::string unused_value;
        if (!storage_arguments(
                ctx, argc, argv, 1,
                &storage_namespace,
                &unused_key,
                &unused_value)) {
            return JS_EXCEPTION;
        }
        JSValue keys = JS_NewArray(ctx);
        if (JS_IsException(keys)) {
            return keys;
        }
        uint32_t index = 0;
        const std::map<std::string, StorageNamespace>::const_iterator
            space = g_worker->storage_.find(storage_namespace);
        if (space != g_worker->storage_.end()) {
            for (std::map<std::string, std::string>::const_iterator
                     entry = space->second.entries.begin();
                 entry != space->second.entries.end();
                 ++entry) {
                if (JS_SetPropertyUint32(
                        ctx,
                        keys,
                        index++,
                        JS_NewStringLen(
                            ctx,
                            entry->first.data(),
                            entry->first.size())) < 0) {
                    JS_FreeValue(ctx, keys);
                    return JS_EXCEPTION;
                }
            }
        }
        if (JS_FreezeObject(ctx, keys) < 0) {
            JS_FreeValue(ctx, keys);
            return JS_EXCEPTION;
        }
        return keys;
    }

    static int storage_module_init(
        JSContext *ctx,
        JSModuleDef *module) {
        struct Method {
            const char *name;
            JSCFunction *function;
            int argc;
        };
        static const Method methods[] = {
            { "get", js_storage_get, 2 },
            { "set", js_storage_set, 3 },
            { "delete", js_storage_delete, 2 },
            { "clear", js_storage_clear, 1 },
            { "keys", js_storage_keys, 1 }
        };
        JSValue storage = JS_NewObject(ctx);
        if (JS_IsException(storage)) {
            return -1;
        }
        for (size_t index = 0;
             index < sizeof(methods) / sizeof(methods[0]);
             ++index) {
            if (JS_DefinePropertyValueStr(
                    ctx,
                    storage,
                    methods[index].name,
                    JS_NewCFunction(
                        ctx,
                        methods[index].function,
                        methods[index].name,
                        methods[index].argc),
                    JS_PROP_ENUMERABLE) <= 0) {
                JS_FreeValue(ctx, storage);
                return -1;
            }
        }
        if (JS_FreezeObject(ctx, storage) < 0) {
            JS_FreeValue(ctx, storage);
            return -1;
        }
        return JS_SetModuleExport(
            ctx, module, "storage", storage);
    }

    static JSValue js_stdio_write(
        JSContext *ctx,
        JSValueConst,
        int argc,
        JSValueConst *argv) {
        if (!g_worker || argc < 2 ||
            !JS_IsString(argv[0]) ||
            !JS_IsString(argv[1])) {
            return JS_ThrowTypeError(
                ctx,
                "stdio stream and message must be strings");
        }
        std::string stream;
        std::string message;
        if (!to_bytes(ctx, argv[0], &stream) ||
            !to_bytes(ctx, argv[1], &message)) {
            return JS_EXCEPTION;
        }
        const capsid::PermissionDecision decision =
            g_worker->config_.capability_policy.query(
                CAPSID_PERMISSION_STDIO, stream, 0);
        if (decision.resource != stream ||
            (stream != "stdin" &&
             stream != "stdout" &&
             stream != "stderr")) {
            g_worker->emit_audit(
                CAPSID_AUDIT_STAGE_OPERATION,
                CAPSID_AUDIT_DENY,
                g_worker->active_request_id(),
                0,
                "capsid:stdio",
                "stdio",
                "stream",
                "<invalid>");
            return JS_ThrowRangeError(
                ctx, "invalid stdio stream");
        }
        if (decision.state ==
            CAPSID_PERMISSION_STATE_UNAVAILABLE) {
            g_worker->emit_audit(
                CAPSID_AUDIT_STAGE_OPERATION,
                CAPSID_AUDIT_UNAVAILABLE,
                g_worker->active_request_id(),
                0,
                "capsid:stdio",
                "stdio",
                "stream",
                stream);
            return JS_ThrowReferenceError(
                ctx,
                "stdio stream is unavailable: %s",
                stream.c_str());
        }
        if (decision.state !=
            CAPSID_PERMISSION_STATE_GRANTED) {
            g_worker->emit_audit(
                CAPSID_AUDIT_STAGE_OPERATION,
                CAPSID_AUDIT_DENY,
                g_worker->active_request_id(),
                decision.rule_id,
                "capsid:stdio",
                "stdio",
                "stream",
                stream);
            return JS_ThrowReferenceError(
                ctx,
                "stdio access denied: %s",
                stream.c_str());
        }
        if (message.size() > kStdioMessageLimit) {
            g_worker->emit_audit(
                CAPSID_AUDIT_STAGE_OPERATION,
                CAPSID_AUDIT_DENY,
                g_worker->active_request_id(),
                decision.rule_id,
                "capsid:stdio",
                "stdio",
                "stream",
                stream);
            return JS_ThrowRangeError(
                ctx, "stdio message exceeds 16384 bytes");
        }
        if (g_worker->stdio_allow_audited_
                .insert(stream).second) {
            g_worker->emit_audit(
                CAPSID_AUDIT_STAGE_OPERATION,
                CAPSID_AUDIT_ALLOW,
                g_worker->active_request_id(),
                decision.rule_id,
                "capsid:stdio",
                "stdio",
                "stream",
                stream);
        }
        capsid::protocol::Frame frame;
        frame.type = capsid::protocol::kLog;
        frame.flags = 0;
        frame.request_id =
            g_worker->active_request_id();
        append_string16(
            &frame.payload,
            reinterpret_cast<const uint8_t *>(
                stream.data()),
            stream.size());
        frame.payload.insert(
            frame.payload.end(),
            message.begin(),
            message.end());
        if (!g_worker->queue_output(frame)) {
            return JS_ThrowInternalError(
                ctx, "stdio output queue is full");
        }
        return JS_UNDEFINED;
    }

    static int stdio_module_init(
        JSContext *ctx,
        JSModuleDef *module) {
        JSValue stdio = JS_NewObject(ctx);
        if (JS_IsException(stdio)) {
            return -1;
        }
        if (JS_DefinePropertyValueStr(
                ctx,
                stdio,
                "write",
                JS_NewCFunction(
                    ctx,
                    js_stdio_write,
                    "write",
                    2),
                JS_PROP_ENUMERABLE) <= 0 ||
            JS_FreezeObject(ctx, stdio) < 0) {
            JS_FreeValue(ctx, stdio);
            return -1;
        }
        return JS_SetModuleExport(
            ctx, module, "stdio", stdio);
    }

    static int open_read_path(
        const std::string &path,
        int flags) {
#if defined(__linux__) && defined(SYS_openat2)
        struct open_how how = {};
        how.flags =
            static_cast<uint64_t>(
                flags | O_CLOEXEC | O_NONBLOCK);
        how.resolve =
            RESOLVE_NO_SYMLINKS |
            RESOLVE_NO_MAGICLINKS;
        return static_cast<int>(syscall(
            SYS_openat2,
            AT_FDCWD,
            path.c_str(),
            &how,
            sizeof(how)));
#else
        (void)path;
        (void)flags;
        errno = ENOSYS;
        return -1;
#endif
    }

    static bool fs_path(
        JSContext *ctx,
        int argc,
        JSValueConst *argv,
        std::string *path,
        capsid::PermissionDecision *decision) {
        if (!g_worker || argc < 1 ||
            !JS_IsString(argv[0])) {
            JS_ThrowTypeError(
                ctx, "filesystem path must be a string");
            return false;
        }
        if (!to_bytes(ctx, argv[0], path)) {
            return false;
        }
        *decision =
            g_worker->config_.capability_policy.query(
                CAPSID_PERMISSION_READ, *path, 0);
        if (decision->resource != *path) {
            g_worker->emit_audit(
                CAPSID_AUDIT_STAGE_OPERATION,
                CAPSID_AUDIT_DENY,
                g_worker->active_request_id(),
                0,
                "capsid:fs",
                "read",
                "path",
                "<invalid>");
            JS_ThrowRangeError(
                ctx, "invalid filesystem path");
            return false;
        }
        if (decision->state !=
            CAPSID_PERMISSION_STATE_GRANTED) {
            g_worker->emit_audit(
                CAPSID_AUDIT_STAGE_OPERATION,
                CAPSID_AUDIT_DENY,
                g_worker->active_request_id(),
                decision->rule_id,
                "capsid:fs",
                "read",
                "path",
                decision->resource);
            JS_ThrowReferenceError(
                ctx,
                "filesystem access denied: %s",
                path->c_str());
            return false;
        }
        if (g_worker->fs_allow_audited_
                .insert(*path).second) {
            g_worker->emit_audit(
                CAPSID_AUDIT_STAGE_OPERATION,
                CAPSID_AUDIT_ALLOW,
                g_worker->active_request_id(),
                decision->rule_id,
                "capsid:fs",
                "read",
                "path",
                decision->resource);
        }
        return true;
    }

    static JSValue fs_open_error(
        JSContext *ctx,
        const std::string &path,
        const capsid::PermissionDecision &decision,
        int error_number) {
        g_worker->emit_audit(
            CAPSID_AUDIT_STAGE_OPERATION,
            CAPSID_AUDIT_DENY,
            g_worker->active_request_id(),
            decision.rule_id,
            "capsid:fs",
            "read",
            "path",
            path);
        if (error_number == ELOOP) {
            return JS_ThrowReferenceError(
                ctx, "filesystem symlinks are disabled");
        }
        if (error_number == ENOSYS) {
            return JS_ThrowReferenceError(
                ctx, "secure filesystem access is unavailable");
        }
        return JS_ThrowReferenceError(
            ctx, "filesystem operation failed");
    }

    static JSValue js_fs_read_text(
        JSContext *ctx,
        JSValueConst,
        int argc,
        JSValueConst *argv) {
#if defined(_WIN32)
        // The filesystem permission module requires openat2 path semantics
        // (RESOLVE_NO_SYMLINKS, Linux-only); see docs/windows.md.
        (void)ctx;
        (void)argc;
        (void)argv;
        return JS_ThrowInternalError(
            ctx, "filesystem module is unavailable on this platform");
#else
        std::string path;
        capsid::PermissionDecision decision;
        if (!fs_path(
                ctx, argc, argv, &path, &decision)) {
            return JS_EXCEPTION;
        }
        const int descriptor =
            open_read_path(path, O_RDONLY);
        if (descriptor < 0) {
            return fs_open_error(
                ctx, path, decision, errno);
        }
        struct stat info = {};
        if (fstat(descriptor, &info) != 0 ||
            !S_ISREG(info.st_mode)) {
            const int saved =
                errno == 0 ? EINVAL : errno;
            close(descriptor);
            return fs_open_error(
                ctx, path, decision, saved);
        }
        if (info.st_size < 0 ||
            static_cast<uint64_t>(info.st_size) >
                kFsFileLimit) {
            close(descriptor);
            g_worker->emit_audit(
                CAPSID_AUDIT_STAGE_OPERATION,
                CAPSID_AUDIT_DENY,
                g_worker->active_request_id(),
                decision.rule_id,
                "capsid:fs",
                "read",
                "path",
                path);
            return JS_ThrowRangeError(
                ctx,
                "filesystem file exceeds 1048576 bytes");
        }
        std::string contents;
        contents.resize(
            static_cast<size_t>(info.st_size));
        size_t offset = 0;
        while (offset < contents.size()) {
            const ssize_t count = read(
                descriptor,
                &contents[offset],
                contents.size() - offset);
            if (count > 0) {
                offset += static_cast<size_t>(count);
                continue;
            }
            if (count < 0 && errno == EINTR) {
                continue;
            }
            if (count < 0 && errno == EAGAIN) {
                continue;
            }
            if (count == 0) {
                contents.resize(offset);
                break;
            }
            const int saved = errno;
            close(descriptor);
            return fs_open_error(
                ctx, path, decision, saved);
        }
        close(descriptor);
        return JS_NewStringLen(
            ctx, contents.data(), contents.size());
#endif  // !defined(_WIN32)
    }

    static JSValue js_fs_stat(
        JSContext *ctx,
        JSValueConst,
        int argc,
        JSValueConst *argv) {
#if defined(_WIN32)
        // The filesystem permission module requires openat2 path semantics
        // (RESOLVE_NO_SYMLINKS, Linux-only); see docs/windows.md.
        (void)ctx;
        (void)argc;
        (void)argv;
        return JS_ThrowInternalError(
            ctx, "filesystem module is unavailable on this platform");
#else
        std::string path;
        capsid::PermissionDecision decision;
        if (!fs_path(
                ctx, argc, argv, &path, &decision)) {
            return JS_EXCEPTION;
        }
        const int descriptor =
            open_read_path(path, O_RDONLY);
        if (descriptor < 0) {
            return fs_open_error(
                ctx, path, decision, errno);
        }
        struct stat info = {};
        if (fstat(descriptor, &info) != 0 ||
            (!S_ISREG(info.st_mode) &&
             !S_ISDIR(info.st_mode))) {
            const int saved =
                errno == 0 ? EINVAL : errno;
            close(descriptor);
            return fs_open_error(
                ctx, path, decision, saved);
        }
        close(descriptor);
        JSValue result = JS_NewObject(ctx);
        if (JS_IsException(result)) {
            return result;
        }
        const char *type =
            S_ISDIR(info.st_mode) ? "directory" : "file";
        if (JS_DefinePropertyValueStr(
                ctx,
                result,
                "type",
                JS_NewString(ctx, type),
                JS_PROP_ENUMERABLE) <= 0 ||
            JS_DefinePropertyValueStr(
                ctx,
                result,
                "size",
                JS_NewInt64(
                    ctx,
                    static_cast<int64_t>(info.st_size)),
                JS_PROP_ENUMERABLE) <= 0 ||
            JS_FreezeObject(ctx, result) < 0) {
            JS_FreeValue(ctx, result);
            return JS_EXCEPTION;
        }
        return result;
#endif  // !defined(_WIN32)
    }

    static JSValue js_fs_list(
        JSContext *ctx,
        JSValueConst,
        int argc,
        JSValueConst *argv) {
#if defined(_WIN32)
        // The filesystem permission module requires openat2 path semantics
        // (RESOLVE_NO_SYMLINKS, Linux-only); see docs/windows.md.
        (void)ctx;
        (void)argc;
        (void)argv;
        return JS_ThrowInternalError(
            ctx, "filesystem module is unavailable on this platform");
#else
        std::string path;
        capsid::PermissionDecision decision;
        if (!fs_path(
                ctx, argc, argv, &path, &decision)) {
            return JS_EXCEPTION;
        }
        const int descriptor =
            open_read_path(path, O_RDONLY | O_DIRECTORY);
        if (descriptor < 0) {
            return fs_open_error(
                ctx, path, decision, errno);
        }
        DIR *directory = fdopendir(descriptor);
        if (!directory) {
            const int saved = errno;
            close(descriptor);
            return fs_open_error(
                ctx, path, decision, saved);
        }
        std::vector<std::string> entries;
        errno = 0;
        for (;;) {
            struct dirent *entry = readdir(directory);
            if (!entry) {
                break;
            }
            if (std::strcmp(entry->d_name, ".") == 0 ||
                std::strcmp(entry->d_name, "..") == 0) {
                continue;
            }
            if (entries.size() >=
                kFsDirectoryEntryLimit) {
                closedir(directory);
                g_worker->emit_audit(
                    CAPSID_AUDIT_STAGE_OPERATION,
                    CAPSID_AUDIT_DENY,
                    g_worker->active_request_id(),
                    decision.rule_id,
                    "capsid:fs",
                    "read",
                    "path",
                    path);
                return JS_ThrowRangeError(
                    ctx,
                    "filesystem directory exceeds 1024 entries");
            }
            entries.push_back(entry->d_name);
        }
        const int read_error = errno;
        closedir(directory);
        if (read_error != 0) {
            return fs_open_error(
                ctx, path, decision, read_error);
        }
        std::sort(entries.begin(), entries.end());
        JSValue result = JS_NewArray(ctx);
        if (JS_IsException(result)) {
            return result;
        }
        for (uint32_t index = 0;
             index < entries.size();
             ++index) {
            if (JS_SetPropertyUint32(
                    ctx,
                    result,
                    index,
                    JS_NewStringLen(
                        ctx,
                        entries[index].data(),
                        entries[index].size())) < 0) {
                JS_FreeValue(ctx, result);
                return JS_EXCEPTION;
            }
        }
        if (JS_FreezeObject(ctx, result) < 0) {
            JS_FreeValue(ctx, result);
            return JS_EXCEPTION;
        }
        return result;
#endif  // !defined(_WIN32)
    }

    static int fs_module_init(
        JSContext *ctx,
        JSModuleDef *module) {
        struct Method {
            const char *name;
            JSCFunction *function;
        };
        static const Method methods[] = {
            { "readText", js_fs_read_text },
            { "stat", js_fs_stat },
            { "list", js_fs_list }
        };
        JSValue fs = JS_NewObject(ctx);
        if (JS_IsException(fs)) {
            return -1;
        }
        for (size_t index = 0;
             index < sizeof(methods) / sizeof(methods[0]);
             ++index) {
            if (JS_DefinePropertyValueStr(
                    ctx,
                    fs,
                    methods[index].name,
                    JS_NewCFunction(
                        ctx,
                        methods[index].function,
                        methods[index].name,
                        1),
                    JS_PROP_ENUMERABLE) <= 0) {
                JS_FreeValue(ctx, fs);
                return -1;
            }
        }
        if (JS_FreezeObject(ctx, fs) < 0) {
            JS_FreeValue(ctx, fs);
            return -1;
        }
        return JS_SetModuleExport(
            ctx, module, "fs", fs);
    }

    static JSModuleDef *module_load(
        JSContext *ctx,
        const char *name,
        void *,
        JSValueConst attributes) {
        (void) attributes;
#ifdef CAPSID_BENCHMARK_SQLITE_ONLY
        if (name &&
            std::strcmp(name, "capsid:sqlite") == 0) {
            return tjs_module_loader(
                ctx, "tjs:sqlite", NULL, attributes);
        }
        if (name &&
            std::strcmp(
                name, "tjs:internal/core") == 0) {
            return tjs_module_loader(
                ctx, name, NULL, attributes);
        }
#endif
        if (is_utility_module(name)) {
            JSModuleDef *module =
                tjs__load_builtin(
                    ctx,
                    utility_implementation_module(name));
            if (!module) {
                JS_ThrowReferenceError(
                    ctx,
                    "module is unavailable: %s",
                    name ? name : "<unknown>");
            }
            return module;
        }
        if (name &&
            std::strcmp(name, "tjs:internal/core") == 0) {
            return tjs_module_loader(
                ctx, name, NULL, attributes);
        }
        if (name &&
            std::strcmp(name, "capsid:env") == 0) {
            JSModuleDef *module = JS_NewCModule(
                ctx, name, env_module_init);
            if (!module ||
                JS_AddModuleExport(
                    ctx, module, "env") < 0) {
                return NULL;
            }
            return module;
        }
        if (name &&
            std::strcmp(name, "capsid:system") == 0) {
            JSModuleDef *module = JS_NewCModule(
                ctx, name, system_module_init);
            if (!module ||
                JS_AddModuleExport(
                    ctx, module, "system") < 0) {
                return NULL;
            }
            return module;
        }
        if (name &&
            std::strcmp(name, "capsid:storage") == 0) {
            JSModuleDef *module = JS_NewCModule(
                ctx, name, storage_module_init);
            if (!module ||
                JS_AddModuleExport(
                    ctx, module, "storage") < 0) {
                return NULL;
            }
            return module;
        }
        if (name &&
            std::strcmp(name, "capsid:stdio") == 0) {
            JSModuleDef *module = JS_NewCModule(
                ctx, name, stdio_module_init);
            if (!module ||
                JS_AddModuleExport(
                    ctx, module, "stdio") < 0) {
                return NULL;
            }
            return module;
        }
        if (name &&
            std::strcmp(name, "capsid:fs") == 0) {
            JSModuleDef *module = JS_NewCModule(
                ctx, name, fs_module_init);
            if (!module ||
                JS_AddModuleExport(
                    ctx, module, "fs") < 0) {
                return NULL;
            }
            return module;
        }
        if (!name ||
            std::strcmp(name, "capsid:permissions") != 0) {
            JS_ThrowReferenceError(
                ctx,
                "module is unavailable: %s",
                name ? name : "<unknown>");
            return NULL;
        }
        JSModuleDef *module = JS_NewCModule(
            ctx, name, permissions_module_init);
        if (!module ||
            JS_AddModuleExport(
                ctx, module, "permissions") < 0) {
            return NULL;
        }
        return module;
    }

    static char *normalize_module(
        JSContext *ctx,
        const char *base_name,
        const char *name,
        void *opaque) {
        WorkerRuntime *self =
            static_cast<WorkerRuntime *>(opaque);
        const std::string module =
            name ? name : "<unknown>";
#ifdef CAPSID_BENCHMARK_SQLITE_ONLY
        if (module == "capsid:sqlite" ||
            module == "tjs:internal/core") {
            self->emit_audit(
                CAPSID_AUDIT_STAGE_MODULE,
                CAPSID_AUDIT_ALLOW,
                0,
                0,
                module,
                "benchmark-only module",
                "specifier",
                module);
            return js_strdup(ctx, module.c_str());
        }
#endif
        const bool trusted_utility_import =
            base_name &&
            std::strcmp(base_name, "tjs:hashing") == 0 &&
            module == "tjs:internal/core";
        if (trusted_utility_import) {
            self->emit_audit(
                CAPSID_AUDIT_STAGE_MODULE,
                CAPSID_AUDIT_ALLOW,
                0,
                0,
                module,
                "trusted utility dependency",
                "specifier",
                module);
            return js_strdup(ctx, module.c_str());
        }
        const capsid::ModuleDecision decision =
            self->config_.capability_policy
                .module_decision(module);
        if (decision == capsid::kModuleGranted) {
            self->emit_audit(
                CAPSID_AUDIT_STAGE_MODULE,
                CAPSID_AUDIT_ALLOW,
                0,
                0,
                module,
                "module",
                "specifier",
                module);
            return js_strdup(ctx, module.c_str());
        }

        self->denied_module_ = module;
        capsid_audit_stage stage = CAPSID_AUDIT_STAGE_MODULE;
        capsid_audit_decision audit = CAPSID_AUDIT_DENY;
        if (decision == capsid::kModuleUnavailable) {
            self->module_error_ =
                "module is unavailable: " + module;
            stage = CAPSID_AUDIT_STAGE_BUILD;
            audit = CAPSID_AUDIT_UNAVAILABLE;
        } else if (decision == capsid::kModuleForbidden) {
            self->module_error_ =
                "module is forbidden: " + module;
        } else {
            self->module_error_ =
                "module is not authorized: " + module;
        }
        self->emit_audit(
            stage,
            audit,
            0,
            0,
            module,
            "module",
            "specifier",
            module);
        JS_ThrowReferenceError(
            ctx, "%s", self->module_error_.c_str());
        return NULL;
    }

    static int deny_attributes(JSContext *ctx, void *, JSValueConst attributes) {
        JSPropertyEnum *properties = NULL;
        uint32_t count = 0;
        if (JS_GetOwnPropertyNames(ctx,
                                   &properties,
                                   &count,
                                   attributes,
                                   JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY)) {
            return -1;
        }
        JS_FreePropertyEnum(ctx, properties, count);
        if (count != 0) {
            JS_ThrowTypeError(ctx, "import attributes are disabled");
            return -1;
        }
        return 0;
    }

    void seal_module_loader() {
        JS_SetModuleLoaderFunc2(JS_GetRuntime(ctx_),
                                normalize_module,
                                module_load,
                                deny_attributes,
                                this);
    }

    bool load_application(std::string *error) {
        if (bundle_.empty()) {
            *error = "application bundle is empty";
            return false;
        }
        denied_module_.clear();
        module_error_.clear();
        JSValue module = JS_UNDEFINED;
        if (bundle_is_trusted_bytecode_) {
            module = JS_ReadObject(
                ctx_,
                &bundle_[0],
                bundle_.size(),
                JS_READ_OBJ_BYTECODE);
            if (JS_IsException(module)) {
                *error = std::string(
                    "trusted application bytecode load failed: ") +
                    exception_string();
                return false;
            }
            if (JS_VALUE_GET_TAG(module) != JS_TAG_MODULE) {
                JS_FreeValue(ctx_, module);
                *error =
                    "trusted application bytecode is not a module";
                return false;
            }
            JSModuleDef *definition =
                static_cast<JSModuleDef *>(JS_VALUE_GET_PTR(module));
            JSAtom module_name_atom =
                JS_GetModuleName(ctx_, definition);
            const char *module_name =
                JS_AtomToCString(ctx_, module_name_atom);
            const bool name_matches =
                module_name &&
                bundle_name_ == module_name;
            if (module_name) {
                JS_FreeCString(ctx_, module_name);
            }
            JS_FreeAtom(ctx_, module_name_atom);
            if (!name_matches) {
                JS_FreeValue(ctx_, module);
                *error =
                    "trusted application bytecode module name mismatch";
                return false;
            }
        } else {
            /*
             * QuickJS' lexer normally respects input_len, but some
             * end-of-input lookahead paths still expect a readable NUL
             * sentinel. The IPC payload is an exact-sized vector, so provide
             * that sentinel without including it in the source length.
             */
            const size_t source_size = bundle_.size();
            bundle_.push_back(0);
            module = JS_Eval(
                ctx_,
                reinterpret_cast<const char *>(&bundle_[0]),
                source_size,
                bundle_name_.c_str(),
                JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
        }
        if (JS_IsException(module)) {
            const std::string exception = exception_string();
            if (!denied_module_.empty()) {
                *error = std::string("application module resolution failed: "
                                     "ReferenceError: ") +
                         module_error_;
            } else {
                *error = std::string("application compile failed: ") + exception;
            }
            return false;
        }
        if (JS_ResolveModule(ctx_, module) < 0) {
            JS_FreeValue(ctx_, module);
            *error = std::string("application module resolution failed: ") + exception_string();
            return false;
        }

        JSModuleDef *definition = static_cast<JSModuleDef *>(JS_VALUE_GET_PTR(module));
        JSValue evaluation = JS_EvalFunction(ctx_, JS_DupValue(ctx_, module));
        if (JS_IsException(evaluation)) {
            JS_FreeValue(ctx_, module);
            *error = std::string("application evaluation failed: ") + exception_string();
            return false;
        }
        drain_jobs();
        if (JS_PromiseState(ctx_, evaluation) == JS_PROMISE_REJECTED) {
            JSValue reason = JS_PromiseResult(ctx_, evaluation);
            *error = std::string("application evaluation rejected: ") + to_string(ctx_, reason);
            JS_FreeValue(ctx_, reason);
            JS_FreeValue(ctx_, evaluation);
            JS_FreeValue(ctx_, module);
            return false;
        }
        if (JS_PromiseState(ctx_, evaluation) == JS_PROMISE_PENDING) {
            *error = "application top-level await must settle without external I/O";
            JS_FreeValue(ctx_, evaluation);
            JS_FreeValue(ctx_, module);
            return false;
        }
        JS_FreeValue(ctx_, evaluation);

        JSValue module_namespace = JS_GetModuleNamespace(ctx_, definition);
        JSValue default_export = JS_GetPropertyStr(ctx_, module_namespace, "default");
        JSValue handler = JS_UNDEFINED;
        JSValue this_value = JS_UNDEFINED;
        if (JS_IsObject(default_export)) {
            JSValue candidate = JS_GetPropertyStr(ctx_, default_export, "fetch");
            if (JS_IsFunction(ctx_, candidate)) {
                handler = candidate;
                this_value = JS_DupValue(ctx_, default_export);
            } else {
                JS_FreeValue(ctx_, candidate);
            }
        }
        if (!JS_IsFunction(ctx_, handler)) {
            JS_FreeValue(ctx_, handler);
            handler = JS_GetPropertyStr(ctx_, module_namespace, "fetch");
            this_value = JS_UNDEFINED;
        }

        bool installed = false;
        if (JS_IsFunction(ctx_, handler)) {
            application_handler_ = JS_DupValue(ctx_, handler);
            application_handler_this_ = JS_DupValue(ctx_, this_value);
            installed = true;
        } else {
            *error = "application must export default.fetch or a named fetch function";
        }

        JS_FreeValue(ctx_, this_value);
        JS_FreeValue(ctx_, handler);
        JS_FreeValue(ctx_, default_export);
        JS_FreeValue(ctx_, module_namespace);
        JS_FreeValue(ctx_, module);
        return installed;
    }

    void drain_jobs() {
        PhaseGuard guard(this, WorkerPhase::kJS);
        JSContext *job_ctx = NULL;
        while (JS_IsJobPending(JS_GetRuntime(ctx_))) {
            const int result = JS_ExecutePendingJob(JS_GetRuntime(ctx_), &job_ctx);
            if (result <= 0) {
                if (diag_enabled_ && result < 0) {
                    std::fprintf(stderr,
                                 "DRAIN EXCEPTION: %s\n",
                                 exception_string().c_str());
                }
                if (diag_enabled_) {
                    std::fprintf(stderr,
                                 "DRAIN STOP result=%d jobs-pending=%d retained=%llu\n",
                                 result,
                                 JS_IsJobPending(JS_GetRuntime(ctx_)) ? 1 : 0,
                                 static_cast<unsigned long long>(retained_refs_));
                }
                break;
            }
        }
        // §7.4: the post-drain reclaim is deferred to the deadline tick
        // (callers set reclaim_pending_). Draining synchronously here
        // would reclaim while uv-loop timers that captured ctx_data at
        // creation (e.g. the abort path's setTimeout(reject, 0)) have
        // not fired yet, false-poisoning a clean cancellation.
    }

    std::string exception_string() {
        JSValue exception = JS_GetException(ctx_);
        const std::string description = to_string(ctx_, exception);
        JSValue stack = JS_GetPropertyStr(ctx_, exception, "stack");
        const std::string stack_text =
            JS_IsUndefined(stack) ? std::string() : to_string(ctx_, stack);
        JS_FreeValue(ctx_, stack);
        JS_FreeValue(ctx_, exception);
        if (stack_text.empty()) {
            return description;
        }
        if (description.empty() ||
            stack_text.find(description) != std::string::npos) {
            return stack_text;
        }
        return description + "\n" + stack_text;
    }

    static void poll_callback(uv_poll_t *handle, int status, int events) {
        WorkerRuntime *self = static_cast<WorkerRuntime *>(handle->data);
        if (status < 0) {
            self->shutdown();
            return;
        }
        if ((events & UV_READABLE) != 0) {
            self->read_input();
        }
        if ((events & UV_WRITABLE) != 0) {
            self->flush_output();
        }
    }

    struct PhaseGuard {
        WorkerRuntime *self;
        WorkerPhase phase;
        explicit PhaseGuard(WorkerRuntime *s, WorkerPhase p)
            : self(s), phase(p) {
            self->current_phase_ = phase;
        }
        ~PhaseGuard() { self->current_phase_ = WorkerPhase::kIdle; }
    };

    void read_input() {
        PhaseGuard guard(this, WorkerPhase::kRead);
        uint8_t buffer[64 * 1024];
        for (;;) {
            const ssize_t count =
#if defined(_WIN32)
                capsid::win32::read_fd(fd_, buffer, sizeof(buffer));
#else
                read(fd_, buffer, sizeof(buffer));
#endif
            if (count > 0) {
                if (!parser_.append(buffer, static_cast<size_t>(count))) {
                    shutdown();
                    return;
                }
                process_frames();
                continue;
            }
            if (count < 0 && errno == EINTR) {
                continue;
            }
            if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                return;
            }
            shutdown();
            return;
        }
    }

    void process_frames() {
        PhaseGuard guard(this, WorkerPhase::kProcess);
        for (;;) {
            capsid::protocol::Frame frame;
            const capsid::protocol::ParseResult result = parser_.next(&frame);
            if (result == capsid::protocol::kParseNeedMore) {
                return;
            }
            if (result == capsid::protocol::kParseError || !handle_frame(frame)) {
                send_error(frame.request_id, "invalid IPC frame");
                flush_output();
                shutdown();
                return;
            }
        }
    }

    bool handle_frame(const capsid::protocol::Frame &frame) {
        switch (frame.type) {
            case capsid::protocol::kRequestHead:
                return begin_request(frame);
            case capsid::protocol::kRequestBody:
                return request_body(frame);
            case capsid::protocol::kRequestEnd:
                return end_request(frame.request_id);
            case capsid::protocol::kWindowUpdate:
                return add_response_credit(frame);
            case capsid::protocol::kCancel:
                return cancel_request(frame.request_id);
            case capsid::protocol::kMemoryMetricsRequest:
                return send_memory_metrics(frame);
            case capsid::protocol::kShutdown:
                shutdown();
                return true;
            default:
                return false;
        }
    }

    static uint64_t nonnegative_metric(int64_t value) {
        return value < 0 ? 0 : static_cast<uint64_t>(value);
    }

    bool send_memory_metrics(const capsid::protocol::Frame &request) {
        if (request.request_id != 0 || !request.payload.empty()) {
            return false;
        }
        JSMemoryUsage usage;
        std::memset(&usage, 0, sizeof(usage));
        JS_ComputeMemoryUsage(JS_GetRuntime(ctx_), &usage);

        capsid::protocol::Frame response;
        response.type = capsid::protocol::kMemoryMetricsResponse;
        response.flags = 0;
        response.request_id = 0;
        capsid::protocol::append_u32(
            &response.payload, CAPSID_MEMORY_METRICS_VERSION);
        const int64_t fields[] = {
            usage.malloc_size,
            usage.malloc_limit,
            usage.memory_used_size,
            usage.atom_count,
            usage.atom_size,
            usage.str_count,
            usage.str_size,
            usage.obj_count,
            usage.obj_size,
            usage.prop_count,
            usage.prop_size,
            usage.shape_count,
            usage.shape_size,
            usage.js_func_count,
            usage.js_func_size,
            usage.js_func_code_size,
            usage.binary_object_count,
            usage.binary_object_size
        };
        for (size_t index = 0;
             index < sizeof(fields) / sizeof(fields[0]);
             ++index) {
            capsid::protocol::append_u64(
                &response.payload, nonnegative_metric(fields[index]));
        }
        return queue_output(response);
    }

    bool begin_request(const capsid::protocol::Frame &frame) {
        if (poisoned_) {
            // §7.4: reject new RequestHead frames. The error terminal plus
            // the pending EXIT makes the host treat the worker as a
            // capacity drop and replace it.
            send_error(frame.request_id, "worker poisoned");
            return true;
        }
        if (responses_.find(frame.request_id) != responses_.end() ||
            responses_.size() >= config_.max_inflight) {
            return false;
        }
        capsid::WorkerRequestHead decoded;
        std::string decode_error;
        if (!capsid::decode_worker_request_head(
                frame,
                config_.max_header_bytes,
                &decoded,
                &decode_error)) {
            return false;
        }

        JSValue headers = JS_NewArray(ctx_);
        for (size_t i = 0; i < decoded.headers.size(); ++i) {
            JSValue pair = JS_NewArray(ctx_);
            JS_SetPropertyUint32(
                ctx_,
                pair,
                0,
                JS_NewStringLen(
                    ctx_,
                    decoded.headers[i].name.data(),
                    decoded.headers[i].name.size()));
            JS_SetPropertyUint32(
                ctx_,
                pair,
                1,
                JS_NewStringLen(
                    ctx_,
                    decoded.headers[i].value.data(),
                    decoded.headers[i].value.size()));
            JS_SetPropertyUint32(
                ctx_, headers, static_cast<uint32_t>(i), pair);
        }

        ResponseState response;
        response.t_begin_ns = uv_hrtime();
        response.credit = config_.initial_window;
        // Bodyless requests (kFlagRequestEnd): no request-direction credit
        // and the request direction ends immediately, exactly as if the
        // request-end frame had been processed.
        response.request_credit =
            decoded.bodyless ? 0 : config_.initial_window;
        response.request_ended = decoded.bodyless;
        const uint64_t timeout_ns =
            config_.timeout_ms >
                    std::numeric_limits<uint64_t>::max() / 1000000u
                ? std::numeric_limits<uint64_t>::max()
                : config_.timeout_ms * 1000000u;
        const uint64_t now = uv_hrtime();
        response.deadline_ns =
            timeout_ns > std::numeric_limits<uint64_t>::max() - now
                ? std::numeric_limits<uint64_t>::max()
                : now + timeout_ns;
        // WP-02 §6.2: the token is created here, before the bridge runs,
        // and the bridge executes inside the token scope — the handler's
        // first Promise reaction is enqueued by the bridge call itself and
        // must capture the token. Never defer this to a JS-side enter.
        RequestToken *token = new RequestToken(
            next_token_generation_++, frame.request_id,
            response.deadline_ns);
        token_registry_[token->generation] = token;
        // Registry owner ref, balanced by the final release_token; keeps
        // retained_refs_ == sum(refs) over live tokens (reaches zero on
        // a clean worker exit).
        retained_refs_ += 1;
        response.token = token;
        // §6.2: the ResponseState holds an owner reference. Without this
        // retain, the post-drain reclaim sees refs==1 (registry only) once
        // the chain jobs release, frees the token while the response entry
        // still points at it, and the next transport touch (e.g. the
        // deadline tick) is a use-after-free.
        retain_token(token);
        responses_[frame.request_id] = response;
        pump_order_.push_back(frame.request_id);
        JSValue arguments[6] = {
            JS_DupValue(ctx_, application_handler_),
            JS_DupValue(ctx_, application_handler_this_),
            JS_NewBigUint64(ctx_, frame.request_id),
            JS_NewStringLen(
                ctx_, decoded.method.data(), decoded.method.size()),
            JS_NewStringLen(
                ctx_, decoded.url.data(), decoded.url.size()),
            headers,
        };
        RequestToken *saved_token = current_token_;
        current_token_ = token;
        const bool called = call_bridge(begin_request_, 6, arguments);
        current_token_ = saved_token;
        for (size_t i = 0; i < 6; ++i) {
            JS_FreeValue(ctx_, arguments[i]);
        }
        if (called && !decoded.bodyless) {
            // Bodyless requests get no request-direction window update.
            send_window_update(frame.request_id, config_.initial_window);
        }
        if (called && decoded.bodyless) {
            // Notify the JS side that the request direction ended
            // immediately, matching the request-end frame semantics. A
            // bridge failure propagates exactly like the standalone
            // request-end frame path: fail closed.
            //
            // CAPSID_TEST_FAIL_REQUEST_END_BRIDGE (test-only injection via
            // the host-provided environment snapshot): the app layer cannot
            // make the request-end bridge fail — tjs:internal/* is
            // capability-forbidden for apps and the bootstrap requestEnd
            // early-returns for bodyless requests — so the frozen RED
            // (worker_bodyless_end_failure) injects the failure through the
            // snapshot and asserts the fused begin propagates it.
            std::string injected;
            // Presence alone must not trigger: only the exact value "1"
            // arms the injection, so a stray snapshot entry cannot flip
            // production behavior.
            const bool fail_end =
                config_.capability_policy.env_value(
                    "CAPSID_TEST_FAIL_REQUEST_END_BRIDGE",
                    &injected) && injected == "1";
            const bool end_ok =
                fail_end
                    ? false
                    : call_id_bridge(request_end_, frame.request_id);
            if (!end_ok) {
                return false;
            }
        }
        return called;
    }

    bool request_body(const capsid::protocol::Frame &frame) {
        if (frame.request_id == 0) {
            return false;
        }
        std::map<uint64_t, ResponseState>::iterator state =
            responses_.find(frame.request_id);
        if (state == responses_.end()) {
            // A late request-body frame for a request whose response already
            // ended is an idempotent no-op (see remember_terminal); ids that
            // never existed still fail closed.
            return is_terminal(frame.request_id);
        }
        if (state->second.request_ended ||
            frame.payload.size() > state->second.request_credit) {
            return false;
        }
        state->second.request_credit -= frame.payload.size();
        JSValue arguments[2] = {
            JS_NewBigUint64(ctx_, frame.request_id),
            JS_NewUint8ArrayCopy(ctx_,
                                 frame.payload.empty() ? NULL : &frame.payload[0],
                                 frame.payload.size()),
        };
        // Request-direction bridges run in the token scope too: their
        // promise reactions are request identity.
        RequestToken *saved_token = current_token_;
        current_token_ = state->second.token;
        const bool called = call_bridge(request_chunk_, 2, arguments);
        current_token_ = saved_token;
        JS_FreeValue(ctx_, arguments[1]);
        JS_FreeValue(ctx_, arguments[0]);
        return called;
    }

    bool end_request(uint64_t id) {
        if (id == 0) {
            return false;
        }
        std::map<uint64_t, ResponseState>::iterator state =
            responses_.find(id);
        if (state == responses_.end()) {
            // A late request-end for a request whose response already ended
            // is an idempotent no-op (see remember_terminal); ids that never
            // existed still fail closed.
            return is_terminal(id);
        }
        if (state->second.request_ended) {
            return false;
        }
        state->second.request_ended = true;
        RequestToken *saved_token = current_token_;
        current_token_ = state->second.token;
        const bool called = call_id_bridge(request_end_, id);
        current_token_ = saved_token;
        return called;
    }

    // A request whose response has ended keeps a bounded terminal tombstone:
    // the Host may still deliver request-direction frames (body, end) that
    // were queued before RESPONSE_END was processed — the IPC is a
    // SOCK_STREAM, so no ordering is guaranteed between the Host's writes
    // and the Runtime's reads. Frames for a tombstoned id are idempotent
    // no-ops; frames for an id that never existed still fail closed.
    void remember_terminal(uint64_t id) {
        terminal_requests_.insert(id);
        if (terminal_requests_.size() > kMaxTerminalTombstones) {
            terminal_requests_.erase(terminal_requests_.begin());
        }
    }

    bool is_terminal(uint64_t id) const {
        return terminal_requests_.count(id) != 0;
    }

    bool cancel_request(uint64_t id) {
        if (id == 0) {
            return false;
        }
        std::map<uint64_t, ResponseState>::iterator state =
            responses_.find(id);
        if (state == responses_.end()) {
            return true;
        }
        reject_pending(state->second, "request canceled");
        RequestToken *saved_token = current_token_;
        RequestToken *cancelled_token = state->second.token;
        current_token_ = state->second.token;
        const bool called = call_id_bridge(cancel_request_, id);
        current_token_ = saved_token;
        erase_response(state);
        remember_terminal(id);
        pump_response_output();
        if (interrupted_request_id_ == id) {
            interrupted_request_id_ = 0;
        }
        /*
         * cancelRequest aborts the JavaScript Request synchronously, but its
         * Promise reactions (framework error conversion, finally blocks and
         * disposal hooks) are QuickJS jobs. This bridge is driven by our own
         * libuv handle, outside txiki's normal JS-callback job drain.
         */
        drain_jobs();
        if (cancelled_token != NULL) {
            // §7.4: seed the reclaim baseline with the post-continuation
            // refs so the first reclaim round after a cancel compares
            // against the count left by the continuation's reactions,
            // never against zero. The chain keeps unwinding from here
            // across rounds; only a refs count that stops falling is a
            // detached continuation.
            cancelled_token->last_reclaim_refs_ =
                cancelled_token->refs;
            // §7.4: the first reclaim round after a cancel defers
            // unconditionally (see reclaim_settled_tokens): the cancel's
            // drain completed the chain, but the dead chain's captured
            // refs release only once the next GC round collects it.
            // Poisoning on the cancel's own first round false-positives
            // a healthy cancellation whose chain drained cleanly.
            cancelled_token->reclaim_grace = true;
        }
        // §7.4: reclaim deferred to the next deadline tick so winding-down
        // 0ms timers (setTimeout(reject, 0) in the abort path) release
        // their captured ctx_data first. A driver cancel is a kill order:
        // a live non-terminal continuation must poison on the next tick
        // instead of being deferred until it runs.
        request_reclaim();
        return called;
    }

    bool add_response_credit(const capsid::protocol::Frame &frame) {
        std::map<uint64_t, ResponseState>::iterator state_it =
            responses_.find(frame.request_id);
        if (frame.payload.size() != sizeof(uint32_t)) {
            return false;
        }
        if (state_it == responses_.end()) {
            /*
             * A final body frame and RESPONSE_END can already be queued before
             * the host's credit replenishment arrives. Treat that late update
             * as an idempotent no-op.
             */
            return frame.request_id != 0;
        }
        const uint8_t *cursor = &frame.payload[0];
        const uint8_t *end = cursor + frame.payload.size();
        uint32_t credit = 0;
        if (!capsid::protocol::read_u32(&cursor, end, &credit) || cursor != end || credit == 0) {
            return false;
        }
        ResponseState &state = state_it->second;
        if (state.credit > std::numeric_limits<uint64_t>::max() - credit) {
            return false;
        }
        state.credit += credit;
        pump_one(frame.request_id);
        return true;
    }

    bool call_id_bridge(JSValue function, uint64_t id) {
        if (id == 0) {
            return false;
        }
        JSValue argument = JS_NewBigUint64(ctx_, id);
        const bool result = call_bridge(function, 1, &argument);
        JS_FreeValue(ctx_, argument);
        return result;
    }

    bool call_bridge(JSValue function, int argc, JSValue *argv) {
        const uint64_t t0 = diag_enabled_ ? uv_hrtime() : 0;
        JSValue result = JS_Call(ctx_, function, JS_UNDEFINED, argc, argv);
        if (diag_enabled_) {
            bridge_calls_ += 1;
            bridge_us_ += uv_hrtime() - t0;
            if (bridge_calls_ % 500 == 0) {
                std::fprintf(stderr,
                             "BRIDGE calls=%llu avg_us=%.1f\n",
                             static_cast<unsigned long long>(bridge_calls_),
                             static_cast<double>(bridge_us_) / bridge_calls_ / 1000.0);
            }
        }
        if (JS_IsException(result)) {
            // §7.5: a poisoned worker must not emit identity-0 events; the
            // poison exit path owns all diagnostics.
            if (!poisoned_) {
                send_error(0, exception_string());
            }
            return false;
        }
        JS_FreeValue(ctx_, result);
        return true;
    }

    void expire_requests() {
        const uint64_t now = uv_hrtime();
        std::vector<uint64_t> expired;
        for (std::map<uint64_t, ResponseState>::const_iterator it =
                 responses_.begin();
             it != responses_.end();
             ++it) {
            // kOpen and kEndPending both keep a live deadline: deferring
            // a ResponseEnd must not silently cancel the timeout. Once a
            // timeout or error moved the request to kFailurePending the
            // deadline is disarmed and later ticks must not re-run the
            // cancel path (design §3.5).
            if ((it->second.phase == ResponsePhase::kOpen ||
                 it->second.phase == ResponsePhase::kEndPending) &&
                it->second.deadline_ns != 0 &&
                now >= it->second.deadline_ns) {
                expired.push_back(it->first);
            }
        }
        for (size_t index = 0; index < expired.size(); ++index) {
            const uint64_t id = expired[index];
            std::map<uint64_t, ResponseState>::iterator state =
                responses_.find(id);
            if (state == responses_.end()) {
                continue;
            }
            // Timeout fires exactly once: disarm the deadline and move
            // to TerminalPending before the cancel bridge runs, so a
            // saturated queue cannot re-trigger it on the next tick.
            state->second.deadline_ns = 0;
            state->second.phase = ResponsePhase::kFailurePending;
            reject_pending(state->second, "request timed out");
            interrupted_request_id_ = id;
            call_id_bridge(cancel_request_, id);
            send_error(
                id,
                "request deadline exceeded",
                capsid::protocol::kErrorFlagTimeout);
            interrupted_request_id_ = 0;
            /*
             * The deadline timer is a native embedder handle. Drain the jobs
             * queued by AbortSignal dispatch before another request observes
             * request-local framework state.
             */
            drain_jobs();
            // §7.4: same deferral as cancel_request — the timeout path's
            // reject-timer releases its captured ctx_data on the next loop
            // tick, so reclaim must not run before then. No timer
            // deferral: a chain parked on an unfired timer is a detached
            // continuation and poisons on the next tick (the 80ms-timer
            // continuation regression this unifies with the cancel path).
            request_reclaim();
        }
    }

    // Resolves and frees the front pending write (fully accepted by the
    // wire queue). Single release point for the JS values.
    //
    // WP-02 §6.2: the credit-driven pump runs in the native read/poll
    // callback, outside any QuickJS job, so a bare resolve here would
    // enqueue the awaiting continuation with no captured token and the
    // strict response gates would throw "no active request" inside it —
    // the chain dies and the terminal is never queued. Resume inside the
    // request's token scope (the same wrapping the bridges use) so the
    // continuation captures the token at enqueue time. A NULL token
    // (post-settle) resumes worker-scope, which is the fail-closed case.
    void resolve_pending(ResponseState &state) {
        state.t_write_done_ns = uv_hrtime();
        PendingWrite &pending = state.pending.front();
        RequestToken *saved_token = current_token_;
        current_token_ = state.token;
        JSValue result = JS_Call(ctx_, pending.resolve, JS_UNDEFINED, 0, NULL);
        current_token_ = saved_token;
        if (JS_IsException(result)) {
            JS_FreeValue(ctx_, JS_GetException(ctx_));
        }
        JS_FreeValue(ctx_, result);
        JS_FreeValue(ctx_, pending.resolve);
        JS_FreeValue(ctx_, pending.reject);
        state.pending.pop_front();
    }

    void reject_pending(ResponseState &state, const char *message) {
        while (!state.pending.empty()) {
            PendingWrite &pending = state.pending.front();
            JSValue argument = JS_NewString(ctx_, message);
            // WP-02 §6.2: same scope wrapping as resolve_pending — the
            // rejection is delivered from a native callback and its
            // continuation must capture the request token.
            RequestToken *saved_token = current_token_;
            current_token_ = state.token;
            JSValue result =
                JS_Call(ctx_, pending.reject, JS_UNDEFINED, 1, &argument);
            current_token_ = saved_token;
            JS_FreeValue(ctx_, result);
            JS_FreeValue(ctx_, argument);
            JS_FreeValue(ctx_, pending.resolve);
            JS_FreeValue(ctx_, pending.reject);
            state.pending.pop_front();
        }
    }

    // Advances the front pending chunk by up to `quantum` bytes: reads
    // from the held JS view, writes frames into the wire queue, resolves
    // the promise once the chunk is fully accepted. Single-request byte
    // order is preserved. Returns true when any progress was made.
    bool advance_pending(ResponseState &state,
                         uint64_t id,
                         size_t quantum) {
        bool progressed = false;
        while (!state.pending.empty() && state.credit > 0 && quantum > 0) {
            PendingWrite &pending = state.pending.front();
            if (pending.offset == pending.size) {
                resolve_pending(state);
                progressed = true;
                continue;
            }
            const size_t remaining = pending.size - pending.offset;
            size_t chunk_size = static_cast<size_t>(
                std::min<uint64_t>(
                    std::min<uint64_t>(
                        remaining, capsid::protocol::kMaxPayloadSize),
                    std::min<uint64_t>(state.credit, quantum)));
            const size_t cap = wire_payload_capacity();
            if (chunk_size > cap) {
                chunk_size = cap;
            }
            if (chunk_size == 0) {
                break;
            }
            const size_t wire = chunk_size + capsid::protocol::kHeaderSize;
            if (!has_output_capacity(wire)) {
                break;
            }
            if (!outbound_.append(
                    capsid::protocol::kResponseBody,
                    0,
                    id,
                    &pending.data[pending.offset],
                    chunk_size)) {
                return false;
            }
            pending.offset += chunk_size;
            state.credit -= chunk_size;
            quantum -= chunk_size;
            account_response_body_bytes(&state, chunk_size);
            progressed = true;
            if (pending.offset == pending.size) {
                resolve_pending(state);
            }
        }
        return progressed;
    }

    // Returns true when the terminal frame entered the wire queue (the
    // caller erases the response); false defers the terminal (bounded
    // metadata) for a later pump. Never drops it.
    bool try_send_terminal(uint64_t id,
                           ResponseState &state,
                           const TerminalPending &terminal) {
        if (terminal.kind == TerminalPending::Kind::kResponseEnd) {
            // The end waits until every body byte is on the wire.
            if (!state.pending.empty()) {
                state.terminal = terminal;
                state.terminal_pending = true;
                state.phase = ResponsePhase::kEndPending;
                return false;
            }
            capsid::protocol::Frame frame;
            frame.type = capsid::protocol::kResponseEnd;
            frame.flags = 0;
            frame.request_id = id;
            if (!queue_output(frame)) {
                state.terminal = terminal;
                state.terminal_pending = true;
                state.phase = ResponsePhase::kEndPending;
                return false;
            }
            return true;
        }
        capsid::protocol::Frame frame;
        frame.type = capsid::protocol::kError;
        frame.flags = terminal.error_flags;
        frame.request_id = id;
        const size_t size = std::min<size_t>(
            terminal.message.size(), error_payload_capacity());
        frame.payload.assign(
            terminal.message.begin(), terminal.message.begin() + size);
        if (!queue_output(frame)) {
            state.terminal = terminal;
            state.terminal_pending = true;
            state.phase = ResponsePhase::kFailurePending;
            return false;
        }
        return true;
    }

    // Diagnostic sampling: every 256 completed responses, print the
    // per-request breakdown (handler / write / tail) to stderr.
    void diag_sample(uint64_t id, const ResponseState &state) {
        (void)id;
        if (!diag_enabled_) {
            return;
        }
        diag_samples_ += 1;
        if (diag_samples_ % 256 != 0) {
            return;
        }
        const uint64_t now = uv_hrtime();
        const uint64_t total =
            state.t_begin_ns != 0 ? now - state.t_begin_ns : 0;
        const uint64_t handler =
            state.t_head_ns != 0 ? state.t_head_ns - state.t_begin_ns : 0;
        const uint64_t write =
            state.t_write_done_ns != 0
                ? state.t_write_done_ns - state.t_head_ns
                : 0;
        const uint64_t tail = now - state.t_write_done_ns;
        std::fprintf(stderr,
                     "DIAG total=%.3fms handler=%.3fms write=%.3fms tail=%.3fms\n",
                     total / 1000000.0,
                     handler / 1000000.0,
                     write / 1000000.0,
                     tail / 1000000.0);
    }

    // Terminal entry point from JS (responseEnd / responseError). Sends
    // immediately when possible; otherwise the terminal is deferred and
    // the pump completes it (contract #5/#6).
    void queue_terminal_or_defer(uint64_t id,
                                 const TerminalPending &terminal) {
        std::map<uint64_t, ResponseState>::iterator state_it =
            responses_.find(id);
        if (state_it == responses_.end()) {
            return;
        }
        ResponseState &state = state_it->second;
        if (try_send_terminal(id, state, terminal)) {
            if (terminal.kind == TerminalPending::Kind::kResponseEnd) {
                g_worker->diag_sample(id, state);
            }
            erase_response(state_it);
            remember_terminal(id);
        } else {
            enqueue_pump(id);
            pump_one(id);
        }
    }

    // Adds a request to the global rotation if not already present
    // (requests with pending bytes or a deferred terminal). The deque
    // is small; a linear probe is fine.
    void enqueue_pump(uint64_t id) {
        if (std::find(pump_order_.begin(), pump_order_.end(), id) ==
            pump_order_.end()) {
            pump_order_.push_back(id);
        }
    }

    // Advances one request's pending/terminal state; erases the
    // response when the terminal entered the wire queue. Used for
    // credit-driven progress, which is per-request O(1) — a global pass
    // per credit frame would multiply the cost by the concurrency.
    void pump_one(uint64_t id) {
        std::map<uint64_t, ResponseState>::iterator it =
            responses_.find(id);
        if (it == responses_.end()) {
            return;
        }
        ResponseState &state = it->second;
        const bool progressed = advance_pending(
            state, id, capsid::protocol::kMaxPayloadSize);
        if (state.pending.empty() && state.terminal_pending) {
            TerminalPending terminal = state.terminal;
            state.terminal_pending = false;
            if (try_send_terminal(id, state, terminal)) {
                erase_response(it);
                remember_terminal(id);
                return;
            }
        }
        if (progressed) {
            update_poll();
        }
    }

    // One unified advance pass (contract #7): each response moves at
    // most one quantum (kMaxPayloadSize) per pass, so a large response
    // cannot starve a small one. Triggered on socket space release and
    // after new writes / terminal deferrals; credit arrival uses the
    // cheaper pump_one instead.
    void pump_response_output() {
        if (pump_in_progress_) {
            return;
        }
        pump_in_progress_ = true;
        std::vector<uint64_t> done;
        // True round-robin (design §3.4): the rotation cursor survives
        // across pump passes. Each pass walks the requests currently in
        // pump_order_ front-to-back, advances one quantum each, and
        // re-queues them at the back; the next pass starts where this
        // one ended, so a low-id large response can never claim every
        // pass's first slot.
        const size_t rounds = pump_order_.size();
        for (size_t i = 0; i < rounds; ++i) {
            if (pump_order_.empty()) {
                break;
            }
            const uint64_t id = pump_order_.front();
            pump_order_.pop_front();
            std::map<uint64_t, ResponseState>::iterator it =
                responses_.find(id);
            if (it == responses_.end()) {
                // Completed/erased since enqueue: drop from the rotation.
                continue;
            }
            ResponseState &state = it->second;
            if (state.credit == 0 && !state.terminal_pending) {
                // No credit and no deferred terminal: this request can
                // make no progress on wire space alone; its credit
                // arrival drives pump_one directly. Drop it from the
                // rotation so the global pass does not re-examine it
                // every frame.
                continue;
            }
            const bool progressed = advance_pending(
                state, id, capsid::protocol::kMaxPayloadSize);
            if (state.pending.empty() && state.terminal_pending) {
                TerminalPending terminal = state.terminal;
                state.terminal_pending = false;
                if (try_send_terminal(id, state, terminal)) {
                    done.push_back(id);
                    continue;  // erased below; not requeued
                }
            }
            if (progressed) {
                update_poll();
            }
            pump_order_.push_back(id);
        }
        for (std::vector<uint64_t>::const_iterator id = done.begin();
             id != done.end();
             ++id) {
            std::map<uint64_t, ResponseState>::iterator it =
                responses_.find(*id);
            if (it != responses_.end()) {
                erase_response(it);
                remember_terminal(*id);
            }
        }
        pump_in_progress_ = false;
    }

    void send_window_update(uint64_t id, uint32_t credit) {
        capsid::protocol::Frame frame;
        frame.type = capsid::protocol::kWindowUpdate;
        frame.flags = 0;
        frame.request_id = id;
        capsid::protocol::append_u32(&frame.payload, credit);
        queue_output(frame);
    }

    void send_error(uint64_t id,
                    const std::string &message,
                    uint32_t flags = 0) {
        if (id != 0) {
            std::map<uint64_t, ResponseState>::iterator state_it =
                responses_.find(id);
            if (state_it != responses_.end()) {
                // Discard the unsent body, then guarantee the error
                // terminal (defer when the queue is full).
                reject_pending(state_it->second, "request failed");
                state_it->second.phase = ResponsePhase::kFailurePending;
                TerminalPending terminal;
                terminal.kind = TerminalPending::Kind::kResponseError;
                terminal.message = message;
                terminal.error_flags = flags;
                queue_terminal_or_defer(id, terminal);
                return;
            }
        }
        // Startup / broadcast errors (id == 0) or already-erased
        // requests: no response state is waiting. The queue cannot be
        // saturated at startup; send directly.
        capsid::protocol::Frame frame;
        frame.type = capsid::protocol::kError;
        frame.flags = flags;
        frame.request_id = id;
        const size_t size = std::min<size_t>(
            message.size(), error_payload_capacity());
        frame.payload.assign(message.begin(), message.begin() + size);
        queue_output(frame);
    }

    void send_simple(uint16_t type,
                     uint64_t id,
                     uint32_t flags = 0) {
        capsid::protocol::Frame frame;
        frame.type = type;
        frame.flags = flags;
        frame.request_id = id;
        queue_output(frame);
    }

    void send_payload(uint16_t type,
                      uint64_t id,
                      uint32_t flags,
                      const std::uint8_t *payload,
                      std::size_t payload_size) {
        capsid::protocol::Frame frame;
        frame.type = type;
        frame.flags = flags;
        frame.request_id = id;
        frame.payload.assign(payload, payload + payload_size);
        queue_output(frame);
    }

    bool queue_output(const capsid::protocol::Frame &frame) {
        return queue_output_bytes(
            frame.type,
            frame.flags,
            frame.request_id,
            frame.payload.empty() ? NULL : &frame.payload[0],
            frame.payload.size());
    }

    // Fast path: writes as many bytes as credit and wire capacity allow,
    // segmented to kMaxPayloadSize. The caller turns kWouldBlock into a
    // pending entry (JS view held, no native copy) and the pump advances
    // the remainder. Never raises RangeError on pressure (contract #3).
    EnqueueResult queue_response_bytes_fast(uint64_t request_id,
                                            const uint8_t *payload,
                                            size_t payload_size,
                                            ResponseState *state,
                                            size_t *sent_out = NULL) {
        if (!payload || payload_size == 0 || !state) {
            return EnqueueResult::kFatal;
        }
        size_t sent = 0;
        while (sent < payload_size && state->credit > 0) {
            const size_t remaining = payload_size - sent;
            size_t chunk_size = static_cast<size_t>(
                std::min<uint64_t>(
                    std::min<uint64_t>(
                        remaining, capsid::protocol::kMaxPayloadSize),
                    state->credit));
            const size_t cap = wire_payload_capacity();
            if (chunk_size > cap) {
                chunk_size = cap;
            }
            if (chunk_size == 0) {
                break;
            }
            const size_t wire = chunk_size + capsid::protocol::kHeaderSize;
            if (!has_output_capacity(wire)) {
                break;
            }
            if (!outbound_.append(
                    capsid::protocol::kResponseBody,
                    0,
                    request_id,
                    payload + sent,
                    chunk_size)) {
                return EnqueueResult::kFatal;
            }
            sent += chunk_size;
            state->credit -= chunk_size;
            account_response_body_bytes(state, chunk_size);
        }
        if (sent > 0) {
            update_poll();
        }
        if (sent_out != NULL) {
            *sent_out = sent;
        }
        return sent == payload_size
                   ? EnqueueResult::kQueued
                   : EnqueueResult::kWouldBlock;
    }

    static void account_response_body_bytes(
        ResponseState *state,
        size_t size) {
        if (!state || size == 0) {
            return;
        }
        const uint64_t amount = static_cast<uint64_t>(size);
        if (state->response_body_bytes_accepted >
            std::numeric_limits<uint64_t>::max() - amount) {
            state->response_body_bytes_accepted =
                std::numeric_limits<uint64_t>::max();
            return;
        }
        state->response_body_bytes_accepted += amount;
    }

    bool queue_output_bytes(uint16_t type,
                            uint32_t flags,
                            uint64_t request_id,
                            const uint8_t *payload,
                            size_t payload_size) {
        if (payload_size > capsid::protocol::kMaxPayloadSize) {
            return false;
        }
        const size_t wire_size =
            capsid::protocol::kHeaderSize + payload_size;
        if (!has_output_capacity(wire_size)) {
            return false;
        }
        if (!outbound_.append(type, flags, request_id, payload, payload_size)) {
            return false;
        }
        update_poll();
        return true;
    }

    bool has_output_capacity(size_t additional) const {
        // Contract #8: the native wire queue never exceeds
        // max_queued_bytes. Pending entries hold JS views only, so they
        // do not consume this budget.
        const size_t limit = config_.max_queued_bytes;
        const size_t queued = outbound_.logical_size();
        return queued <= limit && additional <= limit - queued;
    }

    // Largest payload that fits the wire queue right now, after the
    // frame header. A queue smaller than one frame still accepts
    // payloads up to limit - header (segmented), never zero-sized
    // frames.
    size_t wire_payload_capacity() const {
        const size_t limit = config_.max_queued_bytes;
        const size_t queued = outbound_.logical_size();
        if (queued > limit ||
            limit - queued < capsid::protocol::kHeaderSize) {
            return 0;
        }
        return limit - queued - capsid::protocol::kHeaderSize;
    }

    // Largest error payload that fits a single frame in this queue:
    // never exceed the wire budget, so a long error message cannot
    // wedge the terminal forever (design §3.5).
    size_t error_payload_capacity() const {
        const size_t limit = config_.max_queued_bytes;
        const size_t max_by_queue =
            limit > capsid::protocol::kHeaderSize
                ? limit - capsid::protocol::kHeaderSize
                : 0;
        return std::min<size_t>(
            capsid::protocol::kMaxPayloadSize, max_by_queue);
    }

    // Production writer for OutboundBuffer: the IPC socket, with
    // EINTR retried and EAGAIN mapped to a stall.
    struct WriterOpaque {
        int fd;
        uint64_t calls;
        // Diag-only: write syscalls per frame type. Indexed by the
        // frame type from the frame header at the write cursor; a
        // resume after EAGAIN points mid-frame and is skipped.
        bool diag;
        std::array<uint64_t, 32> frame_types{};
    };

    static ssize_t socket_writer(const uint8_t *data, size_t size,
                                 void *opaque) {
        WriterOpaque *state = static_cast<WriterOpaque *>(opaque);
        state->calls += 1;
        if (state->diag && size >= capsid::protocol::kHeaderSize) {
            const uint16_t type = static_cast<uint16_t>(data[6]) |
                                  static_cast<uint16_t>(data[7] << 8);
            if (type < state->frame_types.size()) {
                state->frame_types[type] += 1;
            }
        }
        for (;;) {
            const ssize_t count = write_socket(state->fd, data, size);
            if (count >= 0) {
                return count;
            }
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return 0;  // stall
            }
            return -1;
        }
    }

    void flush_output() {
        PhaseGuard guard(this, WorkerPhase::kFlush);
        writer_opaque_.fd = fd_;
        writer_opaque_.calls = 0;
        writer_opaque_.diag = diag_enabled_;
        writer_opaque_.frame_types.fill(0);
        if (!outbound_.flush(socket_writer, &writer_opaque_)) {
            {
                FILE *dbg = std::fopen("E:/capsid/build-win/worker-debug.log", "ab");
                if (dbg != NULL) {
                    std::fprintf(dbg, "flush_output: FAILED errno=%d wsa=%d\n",
                                 errno,
#if defined(_WIN32)
                                 WSAGetLastError());
#else
                                 0);
#endif
                    std::fclose(dbg);
                }
            }
            shutdown();
            return;
        }
        {
            FILE *dbg = std::fopen("E:/capsid/build-win/worker-debug.log", "ab");
            if (dbg != NULL) {
                std::fprintf(dbg, "flush_output: ok calls=%u\n",
                             static_cast<unsigned>(writer_opaque_.calls));
                std::fclose(dbg);
            }
        }
        if (diag_enabled_ && writer_opaque_.calls > 0) {
            flush_syscall_samples_ += 1;
            flush_syscall_total_ += writer_opaque_.calls;
            for (size_t i = 0; i < writer_opaque_.frame_types.size(); ++i) {
                flush_type_total_[i] += writer_opaque_.frame_types[i];
            }
            if (flush_syscall_samples_ % 200 == 0) {
                const uint64_t named = flush_type_total_[capsid::protocol::kResponseHead] +
                                       flush_type_total_[capsid::protocol::kResponseBody] +
                                       flush_type_total_[capsid::protocol::kResponseEnd] +
                                       flush_type_total_[capsid::protocol::kWindowUpdate];
                std::fprintf(stderr,
                             "FLUSH syscalls=%llu samples=%llu avg=%.2f"
                             " types(head=%llu body=%llu end=%llu"
                             " win=%llu other=%llu)\n",
                             static_cast<unsigned long long>(flush_syscall_total_),
                             static_cast<unsigned long long>(flush_syscall_samples_),
                             static_cast<double>(flush_syscall_total_) /
                                 flush_syscall_samples_,
                             static_cast<unsigned long long>(flush_type_total_[capsid::protocol::kResponseHead]),
                             static_cast<unsigned long long>(flush_type_total_[capsid::protocol::kResponseBody]),
                             static_cast<unsigned long long>(flush_type_total_[capsid::protocol::kResponseEnd]),
                             static_cast<unsigned long long>(flush_type_total_[capsid::protocol::kWindowUpdate]),
                             static_cast<unsigned long long>(flush_syscall_total_ - named));
            }
        }
        if (outbound_.drained()) {
            pump_response_output();
        }
        update_poll();
    }

    void flush_blocking() {
        // Startup path: the descriptor is still in blocking mode, so
        // the writer's single call sends everything buffered. The writer
        // reads the full WriterOpaque — handing it the bare fd pointer
        // aliases past an int and trips UBSan's invalid-bool load on the
        // diag flag.
        WriterOpaque opaque;
        opaque.fd = fd_;
        opaque.calls = 0;
        opaque.diag = false;
        outbound_.flush(socket_writer, &opaque);
    }

    void update_poll() {
        if (!poll_started_) {
            return;
        }
        int events = UV_READABLE;
        if (!outbound_.drained()) {
            events |= UV_WRITABLE;
        }
        if (events != poll_events_) {
            poll_events_ = events;
        }
        uv_poll_start(&poll_, poll_events_, poll_callback);
    }

    void shutdown() {
        if (shutting_down_) {
            return;
        }
        shutting_down_ = true;
        if (poll_started_) {
            uv_poll_stop(&poll_);
            if (!uv_is_closing(reinterpret_cast<uv_handle_t *>(&poll_))) {
                uv_close(reinterpret_cast<uv_handle_t *>(&poll_), NULL);
            }
        }
        if (deadline_timer_started_) {
            uv_timer_stop(&deadline_timer_);
            if (!uv_is_closing(
                    reinterpret_cast<uv_handle_t *>(&deadline_timer_))) {
                uv_close(
                    reinterpret_cast<uv_handle_t *>(&deadline_timer_),
                    NULL);
            }
            deadline_timer_started_ = false;
        }
        if (runtime_) {
            TJS_Stop(runtime_);
        }
    }

    int fd_;
    int network_namespace_fd_;
    TJSRuntime *runtime_;
    JSContext *ctx_;
    uv_poll_t poll_;
    bool poll_started_;
    uv_timer_t deadline_timer_;
    bool deadline_timer_started_;
    int poll_events_;
    capsid::OutboundBuffer outbound_;
    WriterOpaque writer_opaque_;
    bool pump_in_progress_;
    uint64_t diag_samples_;
    uint64_t flush_syscall_samples_;
    uint64_t flush_syscall_total_;
    std::array<uint64_t, 32> flush_type_total_{};
    uint64_t bridge_calls_;
    uint64_t bridge_us_;
    bool diag_enabled_;
    volatile WorkerPhase current_phase_;
    uint64_t phase_counts_[5];
    uint64_t phase_samples_;
    bool shutting_down_;
    capsid::protocol::Parser parser_;
    capsid::WorkerStartupState startup_state_;
    WorkerConfig config_;
    std::vector<uint8_t> bundle_;
    bool bundle_is_trusted_bytecode_;
    std::string bundle_name_;
    std::map<uint64_t, ResponseState> responses_;
    // Round-robin rotation order for pump_response_output (design §3.4):
    // a request is enqueued on begin and rotated to the back after each
    // pass, so the pump cursor persists across passes.
    std::deque<uint64_t> pump_order_;
    // Bounded tombstone of ids whose response has ended (see
    // remember_terminal).
    std::set<uint64_t> terminal_requests_;
    std::map<std::string, StorageNamespace> storage_;
    std::set<std::string> storage_allow_audited_;
    std::set<std::string> stdio_allow_audited_;
    std::set<std::string> fs_allow_audited_;
    JSValue application_handler_;
    JSValue application_handler_this_;
    JSValue begin_request_;
    JSValue request_chunk_;
    JSValue request_end_;
    JSValue cancel_request_;
    uint64_t next_token_generation_;
    std::map<uint64_t, RequestToken *> token_registry_;
    RequestToken *current_token_;
    // §7.4 poison state machine. Idempotent: the first reason wins
    // (poison_reason_), later triggers only increment poison_triggers_;
    // poison_deadline_ns_ is an independent monotonic deadline checked by
    // the interrupt handler and the deadline tick.
    bool poisoned_;
    const char *poison_reason_;
    uint64_t poison_started_ns_;
    uint64_t poison_deadline_ns_;
    uint64_t poison_triggers_;
    bool poison_exit_started_;
    // Set by settle_request (the normal completion path runs no drain);
    // the deadline tick performs the post-settle reclaim. Reclaim also
    // re-arms itself (reclaim_retry_) while a non-terminal token defers
    // poison because its JS chain is still live; the retry budget is
    // kReclaimSettleWindowNs from the first deferral.
    bool reclaim_pending_;
    bool reclaim_retry_;
    uint64_t reclaim_retry_start_ns_;
    // Sum of refs over live tokens (registry owner + captured jobs and
    // native resources). Must reach zero before exit; printed in diag.
    uint64_t retained_refs_;
    uint64_t interrupted_request_id_;
    uint64_t audit_window_started_ns_;
    uint32_t audit_window_count_;
    std::string audit_repeat_key_;
    uint32_t audit_repeat_count_;
    std::string denied_module_;
    std::string module_error_;
};

WorkerRuntime *WorkerRuntime::g_worker = NULL;

}  // namespace

int capsid_run_worker(int ipc_fd, int network_namespace_fd) {
    WorkerRuntime runtime(ipc_fd, network_namespace_fd);
    return runtime.run();
}
