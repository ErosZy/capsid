#include "worker_runtime.h"

#include "binding_rpc.h"
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
int capsid_tjs_set_fs_policy(
    TJSRuntime *runtime,
    int (*check)(void *opaque, const char *path, int access_kind,
                 char *reason, size_t reason_size),
    void *opaque);
int capsid_tjs_set_stdio_policy(
    TJSRuntime *runtime,
    int (*check)(void *opaque, const char *stream,
                 char *reason, size_t reason_size),
    void *opaque);
// Binding v1 §4.1: registers the raw core.fs module on a runtime (the
// Binding Runtime only); every entry point is per-origin gated.
int capsid_tjs_install_binding_fs(TJSRuntime *runtime);
int capsid_tjs_harden_binding_core(TJSRuntime *runtime);
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
// Binding v1 §5.1: drains a runtime's microtasks and deferred rejections,
// completing async module evaluation (quickjs-ng) before exports are read.
void tjs__execute_jobs(JSContext *ctx);
}

#include <errno.h>
// Included on every platform: capsid_pollfd/capsid_poll are used
// unconditionally in read_startup, and the header's POSIX branch is
// self-contained passthroughs.
#include "win32_compat.h"
#if !defined(_WIN32)
#include <dirent.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>
#endif
#if defined(__linux__)
#include <linux/openat2.h>
#include <sys/syscall.h>
#endif

#if defined(_WIN32)
#define CAPSID_FS_ISREG(mode) (((mode) & _S_IFREG) != 0)
#define CAPSID_FS_ISDIR(mode) (((mode) & _S_IFDIR) != 0)
#else
#define CAPSID_FS_ISREG(mode) (S_ISREG(mode))
#define CAPSID_FS_ISDIR(mode) (S_ISDIR(mode))
#endif

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <deque>
#include <limits>
#include <map>
#include <new>
#include <optional>
#include <set>
#include <string>
#include <thread>
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
    return capsid::win32::send_fd(fd, data, size, 0);
#elif defined(MSG_NOSIGNAL)
    return send(fd, data, size, MSG_NOSIGNAL);
#else
    return send(fd, data, size, 0);
#endif
}

// §7.7: restores the binding dispatch window on scope exit so the egress
// gate always observes a well-defined binding context.
struct BindingWindowGuard {
    std::string &id_slot;
    uint64_t &request_slot;
    const std::string previous_id;
    const uint64_t previous_request;
    BindingWindowGuard(std::string &id_slot,
                       uint64_t &request_slot,
                       const std::string &id,
                       uint64_t request_id)
        : id_slot(id_slot),
          request_slot(request_slot),
          previous_id(id_slot),
          previous_request(request_slot) {
        id_slot = id;
        request_slot = request_id;
    }
    ~BindingWindowGuard() {
        id_slot = previous_id;
        request_slot = previous_request;
    }
};

struct BindingAsyncContext {
    std::string binding_id;
    uint64_t request_id;
};

// Pre-sandbox resolver pool warm-up. Strict sandboxes deny clone, while
// uv_getaddrinfo creates its process-global libuv worker threads on first
// submission; the first fetch would otherwise abort the process after the
// sandbox is installed. This submits one "localhost" lookup on a dedicated
// loop/thread and waits at most one second: the thread keeps running if the
// system resolver is slow, but startup never blocks on it.
struct ResolverWarmState {
    uv_loop_t loop;
    uv_getaddrinfo_t request;
    uv_timer_t timer;
    bool timer_initialized = false;
    bool timer_closed = false;
    std::atomic<bool> done{false};

    ResolverWarmState() {
        memset(&loop, 0, sizeof(loop));
        memset(&request, 0, sizeof(request));
        memset(&timer, 0, sizeof(timer));
    }
};

void resolver_warm_timer_closed(uv_handle_t *handle) {
    ResolverWarmState *state =
        static_cast<ResolverWarmState *>(handle->data);
    state->timer_closed = true;
    uv_stop(&state->loop);
}

void resolver_warm_timeout(uv_timer_t *timer) {
    ResolverWarmState *state =
        static_cast<ResolverWarmState *>(timer->data);
    // Signal the starter that it may proceed; the resolve thread stays
    // alive until uv_getaddrinfo completes because its request cannot be
    // cancelled.
    state->done = true;
}

void resolver_warm_resolved(uv_getaddrinfo_t *request,
                            int status,
                            struct addrinfo *result) {
    (void)status;
    ResolverWarmState *state =
        static_cast<ResolverWarmState *>(request->data);
    if (result != NULL) {
        uv_freeaddrinfo(result);
    }
    if (state->timer_initialized) {
        state->timer_initialized = false;
        uv_timer_stop(&state->timer);
        uv_close(reinterpret_cast<uv_handle_t *>(&state->timer),
                 resolver_warm_timer_closed);
    } else {
        state->done = true;
        uv_stop(&state->loop);
    }
}

void resolver_warm_thread(ResolverWarmState *state) {
    uv_run(&state->loop, UV_RUN_DEFAULT);
    // The timer close callback stops the loop; when no timer was ever
    // initialized the loop was already stopped by the resolve callback.
    uv_loop_close(&state->loop);
    delete state;
}

bool warm_resolver_pool_once() {
    ResolverWarmState *state = new (std::nothrow) ResolverWarmState();
    if (state == NULL) {
        return false;
    }
    if (uv_loop_init(&state->loop) != 0) {
        delete state;
        return false;
    }
    state->request.data = state;
    state->timer.data = state;
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_ADDRCONFIG | AI_V4MAPPED;
    if (uv_getaddrinfo(&state->loop, &state->request,
                       resolver_warm_resolved, "localhost", NULL,
                       &hints) != 0) {
        uv_loop_close(&state->loop);
        delete state;
        return false;
    }
    if (uv_timer_init(&state->loop, &state->timer) == 0) {
        state->timer_initialized = true;
        uv_timer_start(&state->timer, resolver_warm_timeout, 1000, 0);
    }
    std::thread thread(resolver_warm_thread, state);
    thread.detach();
    const std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!state->done.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    return true;
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
// Consumed by the capsid:fs permission module on Linux, macOS and Windows.
const size_t kFsFileLimit = 1024u * 1024u;
const size_t kFsDirectoryEntryLimit = 1024u;

#if defined(_WIN32)
// Windows fs paths use the canonical drive-letter absolute form
// (C:/dir/file). Backslashes are accepted on input but normalized away, so
// the policy compiler, capability policy and worker compare one canonical
// string. Only drive-letter absolute paths are supported; UNC paths and
// drive-relative paths fail closed.
bool normalize_windows_fs_path(const std::string &input,
                               std::string *normalized) {
    if (input.size() < 3 ||
        !std::isalpha(static_cast<unsigned char>(input[0])) ||
        input[1] != ':' || (input[2] != '/' && input[2] != '\\')) {
        return false;
    }
    std::vector<std::string> components;
    std::size_t start = 3;
    while (start <= input.size()) {
        const std::size_t end = input.find_first_of("/\\", start);
        const std::size_t component_end =
            end == std::string::npos ? input.size() : end;
        const std::string component = input.substr(
            start, component_end - start);
        if (component.empty() || component == ".") {
            // Repeated separators normalize away.
        } else if (component == "..") {
            if (components.empty()) {
                return false;
            }
            components.pop_back();
        } else {
            components.push_back(component);
        }
        if (component_end == input.size()) {
            break;
        }
        start = component_end + 1;
    }
    std::string out;
    out.push_back(static_cast<char>(
        std::toupper(static_cast<unsigned char>(input[0]))));
    out += ":/";
    for (std::size_t index = 0; index < components.size(); ++index) {
        if (index != 0) {
            out.push_back('/');
        }
        out += components[index];
    }
    *normalized = std::move(out);
    return true;
}

std::wstring windows_fs_wide(const std::string &utf8) {
    const int size = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, utf8.c_str(), -1, nullptr, 0);
    if (size <= 0) {
        return {};
    }
    std::wstring wide(static_cast<std::size_t>(size - 1), L'\0');
    MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, utf8.c_str(), -1,
        &wide[0], size);
    return wide;
}

std::string windows_fs_utf8(const std::wstring &wide) {
    const int size = WideCharToMultiByte(
        CP_UTF8, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return {};
    }
    std::string utf8(static_cast<std::size_t>(size - 1), '\0');
    WideCharToMultiByte(
        CP_UTF8, 0, wide.c_str(), -1, &utf8[0], size, nullptr, nullptr);
    return utf8;
}

// Opens a canonical Windows fs path without following any reparse point.
// directory=true opens a directory handle, otherwise a read-only file
// handle. On success the returned CRT fd owns the handle.
int open_windows_read_path(const std::string &canonical_path,
                           bool directory) {
    std::string normalized;
    if (!normalize_windows_fs_path(canonical_path, &normalized)) {
        errno = EINVAL;
        return -1;
    }
    std::wstring wide = windows_fs_wide(normalized);
    if (wide.empty()) {
        errno = EINVAL;
        return -1;
    }
    for (wchar_t &character : wide) {
        if (character == L'/') {
            character = L'\\';
        }
    }
    std::wstring current = wide.substr(0, 3);
    const DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE |
                        FILE_SHARE_DELETE;
    const auto reject_reparse = [](HANDLE handle) -> bool {
        FILE_ATTRIBUTE_TAG_INFO attributes = {};
        if (!GetFileInformationByHandleEx(
                handle, FileAttributeTagInfo, &attributes,
                sizeof(attributes))) {
            return true;
        }
        return (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    };
    HANDLE dir_handle = CreateFileW(
        current.c_str(), FILE_LIST_DIRECTORY, share, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (dir_handle == INVALID_HANDLE_VALUE) {
        errno = GetLastError() == ERROR_PATH_NOT_FOUND ||
                        GetLastError() == ERROR_FILE_NOT_FOUND
                    ? ENOENT
                    : EACCES;
        return -1;
    }
    if (reject_reparse(dir_handle)) {
        CloseHandle(dir_handle);
        errno = ELOOP;
        return -1;
    }
    std::size_t begin = 3;
    while (begin < wide.size()) {
        const std::size_t end = wide.find(L'\\', begin);
        const bool last = end == std::wstring::npos;
        const std::wstring component = wide.substr(
            begin, last ? std::wstring::npos : end - begin);
        if (component.empty()) {
            break;
        }
        if (!current.empty() && current.back() != L'\\') {
            current.push_back(L'\\');
        }
        current += component;
        const HANDLE next = CreateFileW(
            current.c_str(),
            last && !directory ? GENERIC_READ : FILE_LIST_DIRECTORY,
            share, nullptr, OPEN_EXISTING,
            (last && !directory ? FILE_ATTRIBUTE_NORMAL
                                : FILE_FLAG_BACKUP_SEMANTICS) |
                FILE_FLAG_OPEN_REPARSE_POINT,
            nullptr);
        if (next == INVALID_HANDLE_VALUE) {
            const DWORD saved = GetLastError();
            CloseHandle(dir_handle);
            errno = saved == ERROR_PATH_NOT_FOUND ||
                            saved == ERROR_FILE_NOT_FOUND
                        ? ENOENT
                        : EACCES;
            return -1;
        }
        if (reject_reparse(next)) {
            CloseHandle(next);
            CloseHandle(dir_handle);
            errno = ELOOP;
            return -1;
        }
        if (!last) {
            CloseHandle(dir_handle);
            dir_handle = next;
            begin = end + 1;
            continue;
        }
        CloseHandle(dir_handle);
        const int fd = _open_osfhandle(
            reinterpret_cast<intptr_t>(next), _O_RDONLY | _O_BINARY);
        if (fd < 0) {
            CloseHandle(next);
            errno = EMFILE;
            return -1;
        }
        return fd;
    }
    // The canonical path was a drive root.
    const int fd = _open_osfhandle(
        reinterpret_cast<intptr_t>(dir_handle), _O_RDONLY | _O_BINARY);
    if (fd < 0) {
        CloseHandle(dir_handle);
        errno = EMFILE;
        return -1;
    }
    return fd;
}

bool list_windows_directory(const std::string &canonical_path,
                            std::vector<std::string> *entries) {
    const int fd = open_windows_read_path(canonical_path, true);
    if (fd < 0) {
        return false;
    }
    const HANDLE handle = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
    const DWORD needed = GetFinalPathNameByHandleW(
        handle, nullptr, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (needed == 0) {
        _close(fd);
        errno = EIO;
        return false;
    }
    std::wstring directory(needed, L'\0');
    const DWORD written = GetFinalPathNameByHandleW(
        handle, &directory[0], needed,
        FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (written == 0 || written > needed) {
        _close(fd);
        errno = EIO;
        return false;
    }
    directory.resize(written);
    _close(fd);
    if (!directory.empty() && directory.back() != L'\\') {
        directory.push_back(L'\\');
    }
    directory.push_back(L'*');
    WIN32_FIND_DATAW find_data = {};
    const HANDLE find = FindFirstFileW(directory.c_str(), &find_data);
    if (find == INVALID_HANDLE_VALUE) {
        errno = GetLastError() == ERROR_FILE_NOT_FOUND ? 0 : EIO;
        return false;
    }
    do {
        if (std::wcscmp(find_data.cFileName, L".") == 0 ||
            std::wcscmp(find_data.cFileName, L"..") == 0) {
            continue;
        }
        std::string name = windows_fs_utf8(find_data.cFileName);
        if (name.empty()) {
            FindClose(find);
            errno = EINVAL;
            return false;
        }
        entries->push_back(std::move(name));
    } while (FindNextFileW(find, &find_data) != 0);
    const DWORD saved = GetLastError();
    FindClose(find);
    if (saved != ERROR_NO_MORE_FILES) {
        errno = EIO;
        return false;
    }
    return true;
}
#endif

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
    // §5.1: one QuickJS runtime/context per Binding, created only when
    // bindings exist. Each attaches to the User runtime's loop
    // (shared_loop) and is pumped by it; heaps, globals, module loaders and
    // job queues stay separate between Bindings and from the User runtime.
    struct BindingRuntimeMethodTable {
        BindingRuntimeMethodTable()
            : policy(NULL),
              runtime(NULL),
              ctx(NULL),
              factory_object(JS_UNDEFINED),
              promise_ctor(JS_UNDEFINED),
              promise_resolve(JS_UNDEFINED),
              promise_then(JS_UNDEFINED),
              abort_controller_ctor(JS_UNDEFINED),
              abort(JS_UNDEFINED) {}
        std::string id;
        const capsid::BindingPolicy *policy;
        TJSRuntime *runtime;   // owned; freed before the User runtime
        JSContext *ctx;        // owned by runtime
        JSValue factory_object;              // referenced in ctx
        // Captured before any Binding package evaluates so RPC control flow
        // stays independent of mutations to this Binding's global object.
        JSValue promise_ctor;
        JSValue promise_resolve;
        JSValue promise_then;
        JSValue abort_controller_ctor;
        JSValue abort;
        std::vector<std::string> method_names;  // Runtime-neutral bytes
    };

    // §7.6: the user_to_binding / binding_to_user call queues. Calls carry
    // an id; the pending table owns the JSValues so the queues stay
    // relocatable.
    enum class BindingCallState {
        kQueued,
        kDispatched,
        kSettled,
    };

    struct PendingBindingCall {
        PendingBindingCall()
            : request_id(0),
              deadline_ns(0),
              user_resolve(JS_UNDEFINED),
              user_reject(JS_UNDEFINED),
              abort_controller(JS_UNDEFINED),
              binding_result(JS_UNDEFINED),
              binding_error(JS_UNDEFINED),
              state(BindingCallState::kQueued) {}
        std::string binding_id;
        size_t table_index = 0;
        std::string method;
        capsid::NeutralValue input;
        uint64_t request_id;
        uint64_t deadline_ns;
        JSValue user_resolve;      // user ctx
        JSValue user_reject;       // user ctx
        JSValue abort_controller;  // binding ctx
        JSValue binding_result;    // binding ctx (fulfillment value)
        JSValue binding_error;     // binding ctx (rejection reason)
        BindingCallState state;
    };
    // Clears the Binding dispatch identity on every exit path from
    // pump_binding_calls, including interrupt-driven JS exceptions.
    struct BindingDispatchScope {
        WorkerRuntime *self;
        explicit BindingDispatchScope(WorkerRuntime *owner) : self(owner) {}
        ~BindingDispatchScope() {
            self->current_binding_call_id_ = 0;
            self->binding_interrupted_call_id_ = 0;
        }
    };
    std::deque<uint64_t> user_to_binding_;
    std::deque<uint64_t> binding_to_user_;
    std::map<uint64_t, PendingBindingCall> pending_binding_calls_;
    uint64_t next_binding_call_id_ = 1;
    size_t inflight_binding_calls_ = 0;
    // Binding Runtime interrupt authority: the call id currently executing
    // inside the Binding Runtime, and the id that caused a CPU interrupt.
    uint64_t current_binding_call_id_ = 0;
    uint64_t binding_interrupted_call_id_ = 0;
    // binding id -> index into binding_tables_.
    std::map<std::string, size_t> binding_table_index_;
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
          bootstrapping_binding_runtime_(false),
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
        // §7.6: free every pending binding call before the runtimes go.
        for (std::map<uint64_t, PendingBindingCall>::iterator it =
                 pending_binding_calls_.begin();
             it != pending_binding_calls_.end();
             ++it) {
            self_finish_binding_call(it->second);
        }
        pending_binding_calls_.clear();
        // §7.5: free every Binding Runtime before the User runtime, so its
        // handles close on the shared loop while the User runtime still
        // owns the loop. Runtimes are freed in reverse creation order.
        for (size_t index = binding_tables_.size(); index > 0; --index) {
            BindingRuntimeMethodTable &table = binding_tables_[index - 1];
            if (table.ctx != NULL) {
                JS_FreeValue(table.ctx, table.factory_object);
                table.factory_object = JS_UNDEFINED;
                JS_FreeValue(table.ctx, table.promise_ctor);
                table.promise_ctor = JS_UNDEFINED;
                JS_FreeValue(table.ctx, table.promise_resolve);
                table.promise_resolve = JS_UNDEFINED;
                JS_FreeValue(table.ctx, table.promise_then);
                table.promise_then = JS_UNDEFINED;
                JS_FreeValue(table.ctx, table.abort_controller_ctor);
                table.abort_controller_ctor = JS_UNDEFINED;
                JS_FreeValue(table.ctx, table.abort);
                table.abort = JS_UNDEFINED;
            }
            if (table.runtime != NULL) {
                TJS_FreeRuntime(table.runtime);
                table.runtime = NULL;
                table.ctx = NULL;
            }
        }
        binding_tables_.clear();
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
        if (!read_startup(false)) {
            return 1;
        }
        // §4.3 startup order: every LOAD_BINDING and the App bundle are
        // received (as bounded bytes, no JavaScript) before the sandbox
        // installs, so Binding sandbox profiles can widen the worker's
        // kernel-level union.
        if (!read_startup(true)) {
            return 1;
        }
        // §7.2: zero LOAD_BINDING keeps the exact single-runtime path — an
        // empty list allocates nothing beyond the vector itself.
        bindings_.assign(startup_state_.bindings().begin(),
                         startup_state_.bindings().end());
        // §7.3: compile the per-Binding policies before any JavaScript
        // runs. The User policy stays exactly as the HELLO compiled it;
        // Binding grants can never widen it.
        std::string binding_policy_error;
        if (!binding_policies_.configure(
                bindings_, &binding_policy_error)) {
            send_error(0, std::string("binding policy setup failed: ") +
                              binding_policy_error);
            flush_blocking();
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
        // §4.2: the process-level sandbox is the union of the User
        // requirements and every Binding's profile. Profiles never widen
        // the per-origin Native gates compiled above.
        for (std::vector<capsid::WorkerBindingDescriptor>::const_iterator
                 binding = bindings_.begin();
             binding != bindings_.end();
             ++binding) {
            sandbox_config.binding_profiles.insert(
                sandbox_config.binding_profiles.end(),
                binding->profiles.begin(),
                binding->profiles.end());
        }
        // Binding fs paths enter the process-level Landlock union; the
        // per-origin Native gates stay authoritative.
        for (std::vector<capsid::WorkerBindingDescriptor>::const_iterator
                 binding = bindings_.begin();
             binding != bindings_.end();
             ++binding) {
            sandbox_config.binding_read_paths.insert(
                sandbox_config.binding_read_paths.end(),
                binding->fs_read.begin(),
                binding->fs_read.end());
            sandbox_config.binding_write_paths.insert(
                sandbox_config.binding_write_paths.end(),
                binding->fs_write.begin(),
                binding->fs_write.end());
        }
        uint32_t sandbox_features = 0;
        uint32_t sandbox_landlock_abi = 0;
        uint32_t sandbox_seccomp_mode = 0;
        const std::string sandbox_namespace_identity =
            capsid::network_namespace_identity(network_namespace_fd_);
        if (network_namespace_fd_ >= 0 &&
            sandbox_namespace_identity.empty()) {
            send_error(0, "sandbox setup failed: network namespace identity unavailable");
            flush_blocking();
            return 1;
        }
        // Fetch pre-resolution runs uv_getaddrinfo on libuv's
        // process-global work pool. The pool's threads are created on
        // first submission, and libuv aborts the process when creation
        // fails; the strict sandbox denies clone, so warm the pool —
        // and prime the system resolver — before the kernel sandbox
        // installs. The warm-up is non-blocking beyond one second.
        if (config_.strict_sandbox) {
            (void)warm_resolver_pool_once();
        }
        std::string sandbox_error;
        if (!capsid::apply_sandbox(
                sandbox_config, &sandbox_features, &sandbox_landlock_abi,
                &sandbox_seccomp_mode, &sandbox_error)) {
            send_error(0, std::string("sandbox setup failed: ") + sandbox_error);
            flush_blocking();
            return 1;
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
        // §7.5: create the Binding Runtime (only when bindings exist) and
        // warm every binding factory before READY. A factory/method-export
        // violation fails the worker startup.
        std::string binding_runtime_error;
        if (!create_binding_runtime(&binding_runtime_error)) {
            send_error(0, std::string("binding runtime setup failed: ") +
                              binding_runtime_error);
            flush_blocking();
            return 1;
        }

        std::string load_error;
        if (!load_application(&load_error)) {

            send_error(0, load_error);
            flush_blocking();
            return 1;
        }

        set_nonblocking();
        if (uv_poll_init(TJS_GetLoop(runtime_), &poll_, fd_) != 0) {
            return 1;
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
        update_poll();
        // The READY payload is the 71-byte compatibility ID from the single
        // generated identity source, so a host can compare the running
        // worker against the linked library and the bytecode compiler
        // without trusting either side. sandbox features stay in flags.
        // §4.3: workers with bindings append the sandbox proof; zero
        // binding workers keep the exact baseline payload.
        static_assert(sizeof(CAPSID_BUILD_COMPATIBILITY_ID) - 1 == 71,
                      "compatibility ID must be sha256: plus 64 hex digits");
        std::vector<uint8_t> ready_payload(
            reinterpret_cast<const std::uint8_t *>(
                CAPSID_BUILD_COMPATIBILITY_ID),
            reinterpret_cast<const std::uint8_t *>(
                CAPSID_BUILD_COMPATIBILITY_ID) +
                sizeof(CAPSID_BUILD_COMPATIBILITY_ID) - 1);
        // seccomp_mode/landlock_abi and the namespace identity are filled
        // by the Linux launcher (Binding §7.9); the digest is computed
        // from the canonical profile union either way.
        capsid::append_ready_proof(
            &ready_payload,
            sandbox_features,
            sandbox_seccomp_mode,
            sandbox_landlock_abi,
            sandbox_namespace_identity,
            capsid::compute_binding_profile_digest(bindings_));
        send_payload(capsid::protocol::kReady, 0, sandbox_features,
                     ready_payload.data(), ready_payload.size());
        flush_output();

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

    // Binding v1 §5.3: the Binding Runtime gets its own interrupt handler.
    // It only fires while a Binding method is synchronously executing on the
    // worker thread; the active call carries the absolute deadline and a
    // poisoned worker keeps only the bounded poison-drain window.
    static int binding_interrupt_handler(JSRuntime *, void *opaque) {
        WorkerRuntime *self = static_cast<WorkerRuntime *>(opaque);
        if (!self || self->current_binding_call_id_ == 0 ||
            self->binding_interrupted_call_id_ != 0) {
            return 0;
        }
        const uint64_t now = uv_hrtime();
        if (self->poisoned_) {
            return now >= self->poison_deadline_ns_ ? 1 : 0;
        }
        const std::map<uint64_t, PendingBindingCall>::iterator found =
            self->pending_binding_calls_.find(
                self->current_binding_call_id_);
        if (found == self->pending_binding_calls_.end() ||
            found->second.state != BindingCallState::kDispatched ||
            found->second.deadline_ns == 0 ||
            now < found->second.deadline_ns) {
            return 0;
        }
        self->binding_interrupted_call_id_ =
            self->current_binding_call_id_;
        return 1;
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

    // Binding v1 §5.1: the Binding Runtime's job/async hooks propagate
    // the binding id across async continuations — a timer, connection-pool
    // reconnection or DNS callback re-enters with the same BindingToken
    // the operation started under. Each captured job owns a heap copy.
    static int binding_job_capture_hook(JSContext *,
                                        void *opaque,
                                        void **out_job_context) {
        WorkerRuntime *self = static_cast<WorkerRuntime *>(opaque);
        *out_job_context = NULL;
        if (self && !self->current_binding_id_.empty()) {
            BindingAsyncContext *captured =
                new (std::nothrow) BindingAsyncContext();
            if (captured == NULL) {
                return -1;
            }
            captured->binding_id = self->current_binding_id_;
            captured->request_id = self->current_binding_request_id_;
            *out_job_context = captured;
        }
        return 0;
    }

    static void *binding_job_enter_hook(JSContext *,
                                        void *job_context,
                                        void *opaque) {
        WorkerRuntime *self = static_cast<WorkerRuntime *>(opaque);
        BindingAsyncContext *previous = NULL;
        if (self && !self->current_binding_id_.empty()) {
            previous = new (std::nothrow) BindingAsyncContext();
            if (previous != NULL) {
                previous->binding_id = self->current_binding_id_;
                previous->request_id = self->current_binding_request_id_;
            }
        }
        if (self) {
            const BindingAsyncContext *captured =
                static_cast<const BindingAsyncContext *>(job_context);
            self->current_binding_id_ =
                captured != NULL ? captured->binding_id : "";
            self->current_binding_request_id_ =
                captured != NULL ? captured->request_id : 0;
        }
        return previous;
    }

    static void binding_job_leave_hook(JSContext *,
                                       void *previous_context,
                                       void *opaque) {
        WorkerRuntime *self = static_cast<WorkerRuntime *>(opaque);
        const BindingAsyncContext *previous =
            static_cast<const BindingAsyncContext *>(previous_context);
        if (self) {
            self->current_binding_id_ =
                previous != NULL ? previous->binding_id : "";
            self->current_binding_request_id_ =
                previous != NULL ? previous->request_id : 0;
        }
        delete previous;
    }

    static void binding_job_release_hook(void *job_context,
                                         void *opaque) {
        (void)opaque;
        delete static_cast<BindingAsyncContext *>(job_context);
    }

    static int binding_resource_authorize_hook(JSContext *,
                                               void *owner,
                                               void *opaque) {
        WorkerRuntime *self = static_cast<WorkerRuntime *>(opaque);
        const BindingAsyncContext *captured =
            static_cast<const BindingAsyncContext *>(owner);
        return self != NULL && owner != NULL &&
                       !self->current_binding_id_.empty() &&
                       self->current_binding_id_ ==
                           captured->binding_id
                   ? 1
                   : 0;
    }

    // Installs the binding identity hooks on one Binding Runtime context;
    // every native gate (fs, egress) then observes the originating binding
    // across async continuations instead of losing it after dispatch.
    void install_binding_async_hooks(JSContext *ctx) {
        JSJobContextHooks job_hooks;
        job_hooks.capture = binding_job_capture_hook;
        job_hooks.enter = binding_job_enter_hook;
        job_hooks.leave = binding_job_leave_hook;
        job_hooks.release = binding_job_release_hook;
        JS_SetJobContextHooks(
            JS_GetRuntime(ctx), &job_hooks, this);
        TJSAsyncContextHooks async_hooks;
        async_hooks.capture = binding_job_capture_hook;
        async_hooks.enter = binding_job_enter_hook;
        async_hooks.leave = binding_job_leave_hook;
        async_hooks.release = binding_job_release_hook;
        tjs_set_async_context_hooks(ctx, &async_hooks, this);
        TJSResourceOwnerHooks owner_hooks;
        owner_hooks.capture = binding_job_capture_hook;
        owner_hooks.authorize = binding_resource_authorize_hook;
        owner_hooks.release = binding_job_release_hook;
        tjs_set_resource_owner_hooks(ctx, &owner_hooks, this);
    }

    // Frees one per-Binding runtime and every JSValue it owns. The User
    // runtime stays alive: Binding handles close on the shared loop while
    // the loop owner still exists.
    void release_binding_table(BindingRuntimeMethodTable *table) {
        if (table == NULL) {
            return;
        }
        if (table->ctx != NULL) {
            JS_FreeValue(table->ctx, table->factory_object);
            table->factory_object = JS_UNDEFINED;
            JS_FreeValue(table->ctx, table->promise_ctor);
            table->promise_ctor = JS_UNDEFINED;
            JS_FreeValue(table->ctx, table->promise_resolve);
            table->promise_resolve = JS_UNDEFINED;
            JS_FreeValue(table->ctx, table->promise_then);
            table->promise_then = JS_UNDEFINED;
            JS_FreeValue(table->ctx, table->abort_controller_ctor);
            table->abort_controller_ctor = JS_UNDEFINED;
            JS_FreeValue(table->ctx, table->abort);
            table->abort = JS_UNDEFINED;
        }
        if (table->runtime != NULL) {
            TJS_FreeRuntime(table->runtime);
            table->runtime = NULL;
            table->ctx = NULL;
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
            enter_poison("request identity mismatch");
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
            enter_poison("stale request token");
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
        // Poison is terminal for both heaps. Abort and erase every Binding
        // call now instead of retaining cross-runtime JSValues through the
        // grace period or waiting for an uncooperative Promise to settle.
        while (!pending_binding_calls_.empty()) {
            cancel_binding_calls(
                pending_binding_calls_.begin()->second.request_id);
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
        // A response terminal is also the lifetime boundary for every
        // fire-and-forget Binding call created by that request. Reclaim them
        // here even if the Binding Promise never cooperates or settles.
        cancel_binding_calls(it->first);
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
        // Binding v1 §5.2: dispatch queued binding calls and settle their
        // user promises on the same single-threaded tick.
        self->pump_binding_calls();
        self->pump_binding_results();
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
        // Binding v1 §5.1: the Binding Runtime reuses the same bootstrap
        // bytecode for the profile global surface, but must never claim
        // the User runtime pointers or install the User request bridge.
        const bool binding_runtime =
            self->bootstrapping_binding_runtime_;
        if (!binding_runtime) {
            self->runtime_ = runtime;
            self->ctx_ = ctx;
        }

        JSValue core = TJS_GetInternalCore(runtime);
        if (binding_runtime) {
            // §4.1: bindings read/write through the raw core.fs module,
            // registered here for the Binding Runtime only and gated per
            // entry point by the binding's FS policy.
            if (capsid_tjs_install_binding_fs(runtime) != 0) {
                JS_ThrowInternalError(
                    ctx, "failed to install Binding fs module");
                return -1;
            }
            // §3.3: the Binding Runtime has no User request bridge; the
            // bridge natives throw so bootstrap wiring cannot cross the
            // runtime boundary. Fetch limits stay readable (harmless) and
            // capsidInstallBridge becomes a no-op.
            if (!self->define_native(ctx, core, "capsidRequestCredit", js_bridge_forbidden, 2) ||
                !self->define_native(ctx, core, "capsidResponseHead", js_bridge_forbidden, 4) ||
                !self->define_native(ctx, core, "capsidResponseWrite", js_bridge_forbidden, 2) ||
                !self->define_native(ctx, core, "capsidResponseEnd", js_bridge_forbidden, 1) ||
                !self->define_native(ctx, core, "capsidResponseError", js_bridge_forbidden, 2) ||
                !self->define_native(ctx, core, "capsidResponseFinal", js_bridge_forbidden, 5) ||
                !self->define_native(ctx, core, "capsidResponseFixed", js_bridge_forbidden, 5) ||
                !self->define_native(ctx, core, "capsidRequestSettled", js_bridge_forbidden, 1) ||
                !self->define_native(ctx, core, "capsidLog", js_log, 2) ||
                !self->define_native(ctx, core, "capsidFetchRequestBodyLimit",
                    js_fetch_request_body_limit, 0) ||
                !self->define_native(ctx, core, "capsidFetchResponseBodyLimit",
                    js_fetch_response_body_limit, 0) ||
                !self->define_native(ctx, core, "capsidFixedResponseBodyLimit",
                    js_fixed_response_body_limit, 0) ||
                !self->define_native(ctx, core, "capsidInstallBridge", js_install_bridge, 4)) {
                if (!JS_HasException(ctx)) {
                    JS_ThrowInternalError(
                        ctx,
                        "failed to install Capsid binding bootstrap");
                }
                return -1;
            }
            // §7.7: the Binding Runtime's egress gate consults the
            // current binding's policy — never the User policy.
            if (capsid_tjs_set_cookie_jar_path(runtime, "") != 0 ||
                capsid_tjs_set_egress_policy(
                    runtime, binding_egress_check, self) != 0 ||
                capsid_tjs_set_fs_policy(
                    runtime, binding_fs_check, self) != 0 ||
                capsid_tjs_set_stdio_policy(
                    runtime, binding_stdio_check, self) != 0) {
                if (!JS_HasException(ctx)) {
                    JS_ThrowInternalError(
                        ctx,
                        "failed to install Capsid binding native gates");
                }
                return -1;
            }
            const int bootstrap_result = TJS_EvalBytecode(
                ctx, capsid__bootstrap, capsid__bootstrap_size, true);
            if (bootstrap_result != 0) {
                return bootstrap_result;
            }
            if (capsid_tjs_harden_binding_core(runtime) != 0) {
                if (!JS_HasException(ctx)) {
                    JS_ThrowInternalError(
                        ctx, "failed to harden Binding core");
                }
                return -1;
            }
            return 0;
        }
        if (capsid_tjs_set_cookie_jar_path(runtime, "") != 0 ||
            capsid_tjs_set_egress_policy(
                runtime, egress_check, self) != 0 ||
            (!self->config_.tls_ca_bundle_path.empty() &&
             capsid_tjs_set_ca_bundle_path(
                 runtime,
                 self->config_.tls_ca_bundle_path.c_str()) != 0) ||
            !self->define_native(ctx, core, "capsidLog", js_log, 2) ||
            !self->define_native(ctx, core, "capsidRequestCredit", js_request_credit, 2) ||
            !self->define_native(ctx, core, "capsidResponseHead", js_response_head, 4) ||
            !self->define_native(ctx, core, "capsidResponseWrite", js_response_write, 2) ||
            !self->define_native(ctx, core, "capsidResponseEnd", js_response_end, 1) ||
            !self->define_native(ctx, core, "capsidResponseError", js_response_error, 2) ||
            !self->define_native(ctx, core, "capsidResponseFinal", js_response_final, 5) ||
            !self->define_native(ctx, core, "capsidResponseFixed", js_response_fixed, 5) ||
            !self->define_native(ctx, core, "capsidRequestSettled", js_request_settled, 1) ||
            !self->define_native(ctx, core,
                "capsidFetchRequestBodyLimit",
                js_fetch_request_body_limit,
                0) ||
            !self->define_native(ctx, core,
                "capsidFetchResponseBodyLimit",
                js_fetch_response_body_limit,
                0) ||
            !self->define_native(ctx, core,
                "capsidFixedResponseBodyLimit",
                js_fixed_response_body_limit,
                0) ||
            !self->define_native(ctx, core, "capsidInstallBridge", js_install_bridge, 4)) {
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

    // §7.7: the Binding Runtime's egress gate. The binding id comes from
    // the dispatch window (current_binding_id_); no valid binding context
    // fails closed before any syscall. The check consults only the named
    // Binding policy — never the User policy.
    static int binding_egress_check(void *opaque,
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
        if (!self || (!host && !address)) {
            return 0;
        }
        const capsid::BindingPolicy *policy =
            self->binding_policies_.policy(self->current_binding_id_);
        const bool dns_request = host != NULL && port == 0 && address == NULL;
        if (policy == NULL || !policy->has_net_policy) {
            if (reason && reason_size != 0) {
                const std::string text = dns_request
                    ? "CAPSID_DNS_EGRESS_DENIED: " + std::string(host)
                    : "egress denied: no binding net policy";
                std::snprintf(reason, reason_size, "%s", text.c_str());
            }
            return 0;
        }
        if (dns_request) {
            const std::string host_text(host);
            const capsid::EgressDecision dns =
                policy->egress.decide_host_any_port(host_text);
            if (!dns.allowed) {
                if (reason && reason_size != 0) {
                    const std::string text =
                        "CAPSID_DNS_EGRESS_DENIED: " + host_text;
                    std::snprintf(reason, reason_size, "%s", text.c_str());
                }
                return 0;
            }
            return 1;
        }

        uint16_t effective_port = port;
        if (effective_port == 0 && address != NULL) {
            if (address->sa_family == AF_INET &&
                address_len >=
                    static_cast<socklen_t>(sizeof(sockaddr_in))) {
                effective_port = ntohs(
                    reinterpret_cast<const sockaddr_in *>(address)->sin_port);
            } else if (address->sa_family == AF_INET6 &&
                       address_len >=
                           static_cast<socklen_t>(sizeof(sockaddr_in6))) {
                effective_port = ntohs(
                    reinterpret_cast<const sockaddr_in6 *>(address)->sin6_port);
            }
        }
        if (effective_port == 0) {
            return 0;
        }

        if (host != NULL) {
            const std::string host_text(host);
            const capsid::EgressDecision host_decision =
                policy->egress.decide_host(host_text, effective_port);
            if (!host_decision.allowed) {
                if (reason && reason_size != 0) {
                    const std::string text =
                        "egress denied by binding policy: " + host_text +
                        ":" + std::to_string(effective_port);
                    std::snprintf(reason, reason_size, "%s", text.c_str());
                }
                return 0;
            }
        }
        // A resolved/raw address is checked after its host/literal preflight.
        // Raw TCP/UDP call the same gate a second time with only sockaddr;
        // derive the embedded port so the second stage cannot accidentally
        // fail open or reject every explicitly allowed client connection.
        if (address != NULL) {
            const capsid::EgressDecision address_decision =
                policy->egress.decide_resolved_address_authoritative(
                    address, address_len, effective_port);
            if (!address_decision.allowed) {
                if (reason && reason_size != 0) {
                    const std::string text =
                        "egress denied by binding policy: resolved address";
                    std::snprintf(reason, reason_size, "%s",
                                  text.c_str());
                }
                return 0;
            }
        }
        return 1;
    }

    // §3.1: the Binding Runtime's FS gate consults the current
    // binding's policy; no valid binding context fails closed before the
    // syscall.
    static int binding_fs_check(void *opaque,
                                const char *path,
                                int access_kind,
                                char *reason,
                                size_t reason_size) {
        WorkerRuntime *self =
            static_cast<WorkerRuntime *>(opaque);
        if (reason && reason_size != 0) {
            reason[0] = '\0';
        }
        if (!self || !path) {
            return 0;
        }
        const capsid::BindingPolicy *policy =
            self->binding_policies_.policy(self->current_binding_id_);
        const bool is_watch = access_kind == 2;
        const bool watch_profile =
            policy != NULL &&
            std::find(policy->profiles.begin(), policy->profiles.end(),
                      "filesystem-watch") != policy->profiles.end();
        if (policy == NULL || (is_watch && !watch_profile)) {
            if (reason && reason_size != 0) {
                const std::string text =
                    is_watch && policy != NULL
                        ? "CAPSID_FSWATCH_PROFILE_DENIED"
                        : "fs denied: no binding policy";
                std::snprintf(reason, reason_size, "%s", text.c_str());
            }
            return 0;
        }
        const capsid::PermissionDecision decision =
            policy->capability.evaluate(
                access_kind == 1 ? CAPSID_PERMISSION_WRITE
                                 : CAPSID_PERMISSION_READ,
                path);
        if (decision.state != CAPSID_PERMISSION_STATE_GRANTED) {
            if (reason && reason_size != 0) {
                const std::string text =
                    std::string("fs denied by binding policy: ") + path;
                std::snprintf(reason, reason_size, "%s", text.c_str());
            }
            return 0;
        }
        return 1;
    }

    // WASI receives only the conventional stream whose exact public name is
    // present in the current Binding policy. The stream and Binding origin
    // both come from native code, never from a caller-supplied origin value.
    static int binding_stdio_check(void *opaque,
                                   const char *stream,
                                   char *reason,
                                   size_t reason_size) {
        WorkerRuntime *self = static_cast<WorkerRuntime *>(opaque);
        if (reason && reason_size != 0) {
            reason[0] = '\0';
        }
        if (!self || !stream) {
            return 0;
        }
        const capsid::BindingPolicy *policy =
            self->binding_policies_.policy(self->current_binding_id_);
        if (policy != NULL &&
            std::find(policy->stdio.begin(), policy->stdio.end(), stream) !=
                policy->stdio.end()) {
            return 1;
        }
        if (reason && reason_size != 0) {
            const std::string text =
                std::string("stdio denied by binding policy: ") + stream;
            std::snprintf(reason, reason_size, "%s", text.c_str());
        }
        return 0;
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

    // §5.1: natives must be defined with the context that owns `core` —
    // the User ctx for the User bootstrap, the Binding ctx for the Binding
    // bootstrap. Cross-runtime JSValue mutation corrupts the shape hash.
    bool define_native(JSContext *ctx,
                       JSValue core,
                       const char *name,
                       JSCFunction *function,
                       int length) {
        const int result =
            JS_DefinePropertyValueStr(ctx,
                                      core,
                                      name,
                                      JS_NewCFunction(ctx, function, name, length),
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
        if (g_worker->bootstrapping_binding_runtime_) {
            // §3.3/§5.1: the bootstrap bytecode wires the request bridge
            // unconditionally; inside the Binding Runtime the bridge must
            // not exist — accept and drop the wiring (the native stubs
            // installed for this bootstrap throw on any later call).
            return JS_UNDEFINED;
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

    // Binding v1 §3.3: the User request bridge never exists inside the
    // Binding Runtime; the bootstrap-native stubs throw on any call.
    static JSValue js_bridge_forbidden(JSContext *ctx,
                                       JSValueConst,
                                       int argc,
                                       JSValueConst *argv) {
        (void)argc;
        (void)argv;
        return JS_ThrowInternalError(
            ctx,
            "the Capsid request bridge is unavailable in the Binding Runtime");
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
#elif defined(__APPLE__)
        // macOS has no openat2: reproduce the same no-symlink component
        // semantics with a dirfd walk. Every intermediate component is
        // opened O_NOFOLLOW|O_DIRECTORY and the final component is opened
        // O_NOFOLLOW with the caller's flags, so a symlink anywhere in the
        // path fails with ELOOP.
        if (path.empty() || path[0] != '/') {
            errno = EINVAL;
            return -1;
        }
        if (path == "/") {
            return open(
                "/", flags | O_CLOEXEC | O_NONBLOCK);
        }
        int dir_fd = open("/", O_RDONLY | O_CLOEXEC | O_DIRECTORY);
        if (dir_fd < 0) {
            return -1;
        }
        std::size_t begin = 1;
        while (begin < path.size()) {
            const std::size_t end = path.find('/', begin);
            const std::string component = path.substr(
                begin, end == std::string::npos ? std::string::npos
                                                : end - begin);
            const bool last = end == std::string::npos;
            if (component.empty() || component == "." ||
                component == "..") {
                close(dir_fd);
                errno = EINVAL;
                return -1;
            }
            const int next = openat(
                dir_fd,
                component.c_str(),
                (last ? flags : O_RDONLY | O_DIRECTORY) |
                    O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
            const int saved = errno;
            close(dir_fd);
            if (next < 0) {
                errno = saved;
                return -1;
            }
            if (last) {
                return next;
            }
            dir_fd = next;
            begin = end + 1;
        }
        close(dir_fd);
        errno = EINVAL;
        return -1;
#elif defined(_WIN32)
        // Drive-letter canonical path opened without following any
        // reparse point. O_DIRECTORY is handled by list_windows_directory.
        (void)flags;
        return open_windows_read_path(path, false);
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
#if defined(_WIN32)
        {
            std::string normalized;
            if (!normalize_windows_fs_path(*path, &normalized)) {
                JS_ThrowRangeError(
                    ctx, "invalid filesystem path");
                return false;
            }
            *path = std::move(normalized);
        }
#endif
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
            !CAPSID_FS_ISREG(info.st_mode)) {
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
#if defined(_WIN32)
            // MSVC read() takes an unsigned int count; the 1 MiB cap keeps
            // the cast lossless.
            const ssize_t count = read(
                descriptor,
                &contents[offset],
                static_cast<unsigned int>(contents.size() - offset));
#else
            const ssize_t count = read(
                descriptor,
                &contents[offset],
                contents.size() - offset);
#endif
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
    }

    static JSValue js_fs_stat(
        JSContext *ctx,
        JSValueConst,
        int argc,
        JSValueConst *argv) {
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
            (!CAPSID_FS_ISREG(info.st_mode) &&
             !CAPSID_FS_ISDIR(info.st_mode))) {
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
            CAPSID_FS_ISDIR(info.st_mode) ? "directory" : "file";
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
    }

    static JSValue js_fs_list(
        JSContext *ctx,
        JSValueConst,
        int argc,
        JSValueConst *argv) {
        std::string path;
        capsid::PermissionDecision decision;
        if (!fs_path(
                ctx, argc, argv, &path, &decision)) {
            return JS_EXCEPTION;
        }
        std::vector<std::string> entries;
#if defined(_WIN32)
        if (!list_windows_directory(path, &entries)) {
            if (errno == 0) {
                entries.clear();
            } else {
                return fs_open_error(
                    ctx, path, decision, errno);
            }
        }
#else
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
            entries.push_back(entry->d_name);
        }
        const int read_error = errno;
        closedir(directory);
        if (read_error != 0) {
            return fs_open_error(
                ctx, path, decision, read_error);
        }
#endif
        if (entries.size() > kFsDirectoryEntryLimit) {
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
        if (name &&
            std::strncmp(name, "capsid:binding/", 15) == 0) {
            return binding_facade_load(ctx, name, NULL, attributes);
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

    static JSModuleDef *binding_facade_load(
        JSContext *ctx,
        const char *name,
        void *opaque,
        JSValueConst attributes) {
        (void)opaque;
        (void)attributes;
        // normalize_module already verified the binding is declared.
        JSModuleDef *module =
            JS_NewCModule(ctx, name, binding_facade_init);
        if (module == NULL ||
            JS_AddModuleExport(ctx, module, "default") < 0) {
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
        if (module.compare(0, 15, "capsid:binding/") == 0) {
            // §2.4/§7.6: Bindings are declared by LOAD_BINDING frames. A
            // declared import resolves to the synthetic facade; an
            // undeclared one fails here — it never lazily creates a
            // Binding Runtime.
            if (self->binding_table_index_.count(module.substr(15)) ==
                0) {
                self->denied_module_ = module;
                self->module_error_ =
                    "binding is not declared: " + module;
                self->emit_audit(
                    CAPSID_AUDIT_STAGE_MODULE,
                    CAPSID_AUDIT_DENY,
                    0,
                    0,
                    module,
                    "binding",
                    "specifier",
                    module);
                JS_ThrowReferenceError(
                    ctx, "%s", self->module_error_.c_str());
                return NULL;
            }
            self->emit_audit(
                CAPSID_AUDIT_STAGE_MODULE,
                CAPSID_AUDIT_ALLOW,
                0,
                0,
                module,
                "binding",
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

    // --- Binding v1 dual runtime (§5) -----------------------------------

    static bool binding_implementation_module(
        const std::string &public_name,
        std::string *implementation_name) {
        if (!capsid::binding_module_known(public_name) ||
            public_name.compare(0, 7, "capsid:") != 0) {
            return false;
        }
        *implementation_name = "tjs:" + public_name.substr(7);
        return true;
    }

    // TJS bytecode retains its upstream internal import names. Only these
    // audited dependency edges may cross the private alias boundary; package
    // source itself can name only the public capsid:* side.
    static bool binding_internal_dependency(
        const char *base_name,
        const std::string &requested_name,
        std::string *public_name) {
        struct Dependency {
            const char *base;
            const char *requested;
            const char *public_alias;
        };
        static const Dependency dependencies[] = {
            {"tjs:hashing", "tjs:internal/core", "capsid:internal/core"},
            {"tjs:path", "tjs:internal/path", "capsid:internal/path"},
            {"tjs:sqlite", "tjs:internal/core", "capsid:internal/core"},
            {"tjs:wasi", "tjs:internal/core", "capsid:internal/core"},
        };
        if (base_name == nullptr) {
            return false;
        }
        for (const Dependency &dependency : dependencies) {
            if (std::strcmp(base_name, dependency.base) == 0 &&
                requested_name == dependency.requested) {
                *public_name = dependency.public_alias;
                return true;
            }
        }
        return false;
    }

    // The Binding Runtime module gate authorizes the public Capsid name, then
    // normalizes it to the private TJS implementation module. The package
    // contract and policy never contain tjs:* names.
    static char *binding_normalize_module(
        JSContext *ctx,
        const char *base_name,
        const char *name,
        void *opaque) {
        WorkerRuntime *self = static_cast<WorkerRuntime *>(opaque);
        const std::string module = name ? name : "";
        // Linking needs the package identity, but that identity must never
        // become a native-operation token. Dynamic imports during a method
        // dispatch use the active immutable Binding identity instead.
        const std::string &binding_id =
            !self->loading_binding_id_.empty()
                ? self->loading_binding_id_
                : self->current_binding_id_;
        const capsid::BindingPolicy *policy =
            self->binding_policies_.policy(binding_id);
        std::string public_module = module;
        std::string implementation_module;
        const bool public_import = binding_implementation_module(
            module, &implementation_module);
        const bool internal_dependency = !public_import &&
            binding_internal_dependency(
                base_name, module, &public_module);
        if (internal_dependency) {
            implementation_module = module;
        }
        if ((!public_import && !internal_dependency) || policy == NULL ||
            policy->module_decision(public_module) !=
                capsid::kModuleGranted) {
            JS_ThrowReferenceError(
                ctx,
                "module is not authorized for this binding: %s",
                module.c_str());
            return NULL;
        }
        return js_strdup(ctx, implementation_module.c_str());
    }

    static JSModuleDef *binding_module_load(
        JSContext *ctx,
        const char *name,
        void *opaque,
        JSValueConst attributes) {
        (void)opaque;
        // Normalization already authorized the public alias or an audited
        // internal dependency edge. The upstream loader sees only its private
        // implementation name.
        return tjs_module_loader(ctx, name, NULL, attributes);
    }

    static WorkerRuntime *binding_worker_from_context(JSContext *ctx) {
        TJSRuntime *runtime = TJS_GetRuntime(ctx);
        return runtime != NULL
                   ? static_cast<WorkerRuntime *>(
                         TJS_GetBootstrapOpaque(runtime))
                   : NULL;
    }

    static std::string json_escape_fragment(const std::string &value) {
        static const char kHex[] = "0123456789abcdef";
        std::string escaped;
        escaped.reserve(value.size() + 8);
        for (const unsigned char ch : value) {
            switch (ch) {
                case '"': escaped += "\\\""; break;
                case '\\': escaped += "\\\\"; break;
                case '\b': escaped += "\\b"; break;
                case '\f': escaped += "\\f"; break;
                case '\n': escaped += "\\n"; break;
                case '\r': escaped += "\\r"; break;
                case '\t': escaped += "\\t"; break;
                default:
                    if (ch < 0x20) {
                        escaped += "\\u00";
                        escaped.push_back(kHex[(ch >> 4) & 0x0f]);
                        escaped.push_back(kHex[ch & 0x0f]);
                    } else {
                        escaped.push_back(static_cast<char>(ch));
                    }
                    break;
            }
        }
        return escaped;
    }

    static void replace_all(std::string *text,
                            const std::string &needle,
                            const char *replacement) {
        if (needle.empty()) {
            return;
        }
        size_t offset = 0;
        const size_t replacement_size = std::strlen(replacement);
        while ((offset = text->find(needle, offset)) != std::string::npos) {
            text->replace(offset, needle.size(), replacement);
            offset += replacement_size;
        }
    }

    void redact_binding_log(const std::string &binding_id,
                            std::string *text) const {
        const std::map<std::string, std::vector<std::string>>::const_iterator
            found = binding_log_secret_values_.find(binding_id);
        if (found == binding_log_secret_values_.end()) {
            return;
        }
        for (const std::string &secret : found->second) {
            replace_all(text, secret, "[REDACTED]");
            const std::string escaped = json_escape_fragment(secret);
            if (escaped != secret) {
                replace_all(text, escaped, "[REDACTED]");
            }
        }
    }

    void redact_binding_log_fields(const std::string &binding_id,
                                   std::string *text) const {
        const std::map<std::string, std::vector<std::string>>::const_iterator
            found = binding_log_secret_values_.find(binding_id);
        if (found == binding_log_secret_values_.end()) {
            return;
        }
        // `text` is JSON. Replacing a raw secret such as `"` or `{` could
        // rewrite JSON syntax rather than a string value. Match the JSON
        // representation instead, then parse the redacted result below.
        // If an unescaped punctuation-only secret is ambiguous with JSON
        // syntax, the parse fails and the log call emits nothing: fail
        // closed rather than forwarding either a secret or malformed JSON.
        for (const std::string &secret : found->second) {
            replace_all(text, json_escape_fragment(secret), "[REDACTED]");
        }
    }

    static JSValue binding_log_fn(JSContext *ctx,
                                  JSValue this_val,
                                  int argc,
                                  JSValue *argv,
                                  int magic,
                                  JSValue *func_data) {
        (void)this_val;
        static const char *const levels[] = {
            "debug", "info", "warn", "error"};
        const char *level = levels[magic & 3];
        std::string message;
        if (argc > 0 && !JS_IsUndefined(argv[0])) {
            size_t length = 0;
            const char *text = JS_ToCStringLen(ctx, &length, argv[0]);
            if (text == NULL) {
                return JS_EXCEPTION;
            }
            message.assign(text, length);
            JS_FreeCString(ctx, text);
        }
        std::string fields = "{}";
        if (argc > 1 && !JS_IsUndefined(argv[1])) {
            const bool is_array = JS_IsArray(argv[1]);
            if (!JS_IsObject(argv[1]) || JS_IsNull(argv[1]) ||
                is_array || JS_IsProxy(argv[1])) {
                return JS_ThrowTypeError(
                    ctx, "binding log fields must be a non-Proxy object");
            }
            JSValue encoded = JS_JSONStringify(
                ctx, argv[1], JS_UNDEFINED, JS_UNDEFINED);
            if (JS_IsException(encoded)) {
                return encoded;
            }
            if (JS_IsUndefined(encoded)) {
                JS_FreeValue(ctx, encoded);
                return JS_ThrowTypeError(
                    ctx, "binding log fields must serialize to an object");
            }
            size_t length = 0;
            const char *text = JS_ToCStringLen(ctx, &length, encoded);
            if (text == NULL) {
                JS_FreeValue(ctx, encoded);
                return JS_EXCEPTION;
            }
            fields.assign(text, length);
            JS_FreeCString(ctx, text);
            JS_FreeValue(ctx, encoded);
        }
        if (message.size() > kStdioMessageLimit ||
            fields.size() > kStdioMessageLimit) {
            return JS_ThrowRangeError(
                ctx, "binding log message or fields exceeds 16384 bytes");
        }
        WorkerRuntime *self = binding_worker_from_context(ctx);
        if (!self) {
            return JS_ThrowInternalError(
                ctx, "binding log runtime owner is unavailable");
        }
        size_t binding_id_size = 0;
        const char *binding_id_text = JS_ToCStringLen(
            ctx, &binding_id_size, func_data[0]);
        if (binding_id_text == NULL) {
            return JS_EXCEPTION;
        }
        const std::string binding_id(binding_id_text, binding_id_size);
        JS_FreeCString(ctx, binding_id_text);
        // The log object belongs to exactly one Binding. A call from another
        // Binding is rejected. During factory warm-up there is no dispatch
        // window; the loading identity is the authoritative owner there.
        const std::string &authoritative_binding =
            !self->current_binding_id_.empty()
                ? self->current_binding_id_
                : self->loading_binding_id_;
        if (authoritative_binding != binding_id) {
            return JS_ThrowTypeError(
                ctx, "binding log object cannot be called from another binding");
        }
        self->redact_binding_log(binding_id, &message);
        self->redact_binding_log_fields(binding_id, &fields);
        if (message.size() > kStdioMessageLimit ||
            fields.size() > kStdioMessageLimit) {
            return JS_ThrowRangeError(
                ctx, "redacted binding log exceeds 16384 bytes");
        }
        JSValue validated_fields = JS_ParseJSON(
            ctx, fields.data(), fields.size(), "<binding-log-fields>");
        if (JS_IsException(validated_fields)) {
            return validated_fields;
        }
        const bool fields_is_array = JS_IsArray(validated_fields);
        const bool fields_is_object = JS_IsObject(validated_fields) &&
                                      !JS_IsNull(validated_fields) &&
                                      !fields_is_array;
        JS_FreeValue(ctx, validated_fields);
        if (!fields_is_object) {
            return JS_ThrowTypeError(
                ctx, "binding log fields must serialize to an object");
        }
        capsid::protocol::Frame frame;
        frame.type = capsid::protocol::kLog;
        frame.flags = capsid::protocol::kFlagBindingLog;
        frame.request_id = self->current_binding_request_id_;
        append_string16(
            &frame.payload,
            reinterpret_cast<const uint8_t *>(binding_id.data()),
            binding_id.size());
        append_string16(
            &frame.payload,
            reinterpret_cast<const uint8_t *>(level),
            std::strlen(level));
        append_string32(
            &frame.payload,
            reinterpret_cast<const uint8_t *>(message.data()),
            message.size());
        append_string32(
            &frame.payload,
            reinterpret_cast<const uint8_t *>(fields.data()),
            fields.size());
        if (!self->queue_output(frame)) {
            return JS_ThrowInternalError(
                ctx, "binding log output queue is full");
        }
        return JS_UNDEFINED;
    }

    JSValue build_binding_log_object(JSContext *ctx,
                                     const std::string &binding_id) {
        JSValue log = JS_NewObjectProto(ctx, JS_NULL);
        if (JS_IsException(log)) {
            return log;
        }
        static const char *const names[] = {
            "debug", "info", "warn", "error"};
        for (int index = 0; index < 4; ++index) {
            JSValue data[1] = {
                JS_NewStringLen(ctx, binding_id.data(), binding_id.size())};
            if (JS_IsException(data[0])) {
                JS_FreeValue(ctx, log);
                return JS_EXCEPTION;
            }
            JSValue function = JS_NewCFunctionData2(
                ctx,
                binding_log_fn,
                names[index],
                1,
                index,
                1,
                data);
            JS_FreeValue(ctx, data[0]);
            if (JS_IsException(function)) {
                JS_FreeValue(ctx, log);
                return JS_EXCEPTION;
            }
            if (JS_SetPropertyStr(
                ctx,
                log,
                names[index],
                function) < 0) {
                JS_FreeValue(ctx, log);
                return JS_EXCEPTION;
            }
        }
        if (JS_FreezeObject(ctx, log) < 0) {
            JS_FreeValue(ctx, log);
            return JS_EXCEPTION;
        }
        return log;
    }

    // Prepares JSON-derived factory inputs without executing user-visible
    // property access. Plain objects receive null prototypes recursively;
    // arrays keep Array.prototype but are recursively frozen. JSON and the
    // Host-created secrets object cannot contain cycles, proxies, symbols or
    // accessors, nevertheless every one is rejected explicitly so this
    // helper remains safe if its callers evolve.
    static bool prepare_binding_init_value(JSContext *ctx,
                                           JSValueConst value,
                                           size_t depth,
                                           std::string *reason) {
        if (!JS_IsObject(value)) {
            return true;
        }
        if (depth > 64) {
            *reason = "factory input exceeds the nesting limit";
            return false;
        }
        if (JS_IsProxy(value)) {
            *reason = "factory input contains a Proxy";
            return false;
        }
        JSPropertyEnum *tab = NULL;
        uint32_t count = 0;
        if (JS_GetOwnPropertyNames(
                ctx, &tab, &count, value,
                JS_GPN_STRING_MASK | JS_GPN_SYMBOL_MASK |
                    JS_GPN_SET_ENUM) < 0) {
            *reason = "factory input property enumeration failed";
            return false;
        }
        for (uint32_t index = 0; index < count; ++index) {
            JSValue atom_value = JS_AtomToValue(ctx, tab[index].atom);
            const bool is_symbol = JS_IsSymbol(atom_value);
            JS_FreeValue(ctx, atom_value);
            if (is_symbol) {
                JS_FreePropertyEnum(ctx, tab, count);
                *reason = "factory input contains a symbol property";
                return false;
            }
            JSPropertyDescriptor descriptor;
            const int present = JS_GetOwnProperty(
                ctx, &descriptor, value, tab[index].atom);
            if (present <= 0) {
                JS_FreePropertyEnum(ctx, tab, count);
                *reason = "factory input property is unreadable";
                return false;
            }
            const bool is_accessor =
                !JS_IsUndefined(descriptor.getter) ||
                !JS_IsUndefined(descriptor.setter);
            if (is_accessor) {
                JS_FreeValue(ctx, descriptor.getter);
                JS_FreeValue(ctx, descriptor.setter);
                JS_FreeValue(ctx, descriptor.value);
                JS_FreePropertyEnum(ctx, tab, count);
                *reason = "factory input contains an accessor";
                return false;
            }
            const bool child_ok = prepare_binding_init_value(
                ctx, descriptor.value, depth + 1, reason);
            JS_FreeValue(ctx, descriptor.getter);
            JS_FreeValue(ctx, descriptor.setter);
            JS_FreeValue(ctx, descriptor.value);
            if (!child_ok) {
                JS_FreePropertyEnum(ctx, tab, count);
                return false;
            }
        }
        JS_FreePropertyEnum(ctx, tab, count);
        if (!JS_IsArray(value) && JS_SetPrototype(ctx, value, JS_NULL) < 0) {
            *reason = "factory input prototype could not be removed";
            return false;
        }
        if (JS_FreezeObject(ctx, value) < 0) {
            *reason = "factory input could not be frozen";
            return false;
        }
        return true;
    }

    static bool valid_binding_method_name(const std::string &name) {
        static const char *const reserved[] = {
            "constructor", "prototype", "__proto__",
            "then",        "catch",     "finally"};
        if (name.empty() || name.size() > 64) {
            return false;
        }
        for (size_t index = 0;
             index < sizeof(reserved) / sizeof(reserved[0]);
             ++index) {
            if (name == reserved[index]) {
                return false;
            }
        }
        const auto ascii_alpha = [](unsigned char ch) {
            return (ch >= 'a' && ch <= 'z') ||
                   (ch >= 'A' && ch <= 'Z');
        };
        const unsigned char first = static_cast<unsigned char>(name[0]);
        if (!(ascii_alpha(first) || first == '_' || first == '$')) {
            return false;
        }
        for (size_t index = 1; index < name.size(); ++index) {
            const unsigned char ch =
                static_cast<unsigned char>(name[index]);
            if (!(ascii_alpha(ch) || (ch >= '0' && ch <= '9') ||
                  ch == '_' || ch == '$')) {
                return false;
            }
        }
        return true;
    }

    static std::string js_error_text(JSContext *ctx) {
        JSValue exception = JS_GetException(ctx);
        const char *text = JS_ToCString(ctx, exception);
        std::string result = text ? text : "<unprintable>";
        if (text != NULL) {
            JS_FreeCString(ctx, text);
        }
        JS_FreeValue(ctx, exception);
        return result;
    }

    // Loads one binding package into the Binding Runtime: evaluate the
    // module, call the synchronous factory with (config, secrets, log),
    // and freeze the returned method table. Any violation fails the
    // worker startup (generation warmup failure, §2.4).
    bool load_binding_package(
        const capsid::WorkerBindingDescriptor &descriptor,
        BindingRuntimeMethodTable *table,
        std::string *error) {
        JSContext *ctx = table->ctx;
        const std::string module_name =
            "capsid:binding/" + descriptor.name;
        // Mirror the production bundle sequence: compile only, resolve the
        // module graph, evaluate, drain the job queue, then read the
        // exports from the namespace. The lexer contract is the same as
        // load_application: end-of-input lookahead expects a readable NUL
        // sentinel, so compile from a NUL-terminated copy.
        const std::string source_nul(
            reinterpret_cast<const char *>(descriptor.source.data()),
            descriptor.source.size());
        JSValue module = JS_Eval(
            ctx,
            source_nul.c_str(),
            source_nul.size(),
            module_name.c_str(),
            JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
        if (JS_IsException(module)) {
            *error = "binding '" + descriptor.name +
                     "' module compile failed: " +
                     js_error_text(ctx);
            JS_FreeValue(ctx, module);
            return false;
        }
        if (JS_ResolveModule(ctx, module) < 0) {
            *error = "binding '" + descriptor.name +
                     "' module resolution failed: " +
                     js_error_text(ctx);
            JS_FreeValue(ctx, module);
            return false;
        }
        JSModuleDef *definition = static_cast<JSModuleDef *>(
            JS_VALUE_GET_PTR(module));
        JSValue evaluation = JS_EvalFunction(
            ctx, JS_DupValue(ctx, module));
        if (JS_IsException(evaluation)) {
            JS_FreeValue(ctx, module);
            *error = "binding '" + descriptor.name +
                     "' module evaluation failed: " +
                     js_error_text(ctx);
            return false;
        }
        tjs__execute_jobs(ctx);
        if (JS_PromiseState(ctx, evaluation) ==
            JS_PROMISE_REJECTED) {
            JSValue reason = JS_PromiseResult(ctx, evaluation);
            *error = "binding '" + descriptor.name +
                     "' module evaluation rejected: " +
                     to_string(ctx, reason);
            JS_FreeValue(ctx, reason);
            JS_FreeValue(ctx, evaluation);
            JS_FreeValue(ctx, module);
            return false;
        }
        if (JS_PromiseState(ctx, evaluation) ==
            JS_PROMISE_PENDING) {
            *error = "binding '" + descriptor.name +
                     "' top-level await must settle without external I/O";
            JS_FreeValue(ctx, evaluation);
            JS_FreeValue(ctx, module);
            return false;
        }
        JS_FreeValue(ctx, evaluation);
        JSValue module_namespace =
            JS_GetModuleNamespace(ctx, definition);
        JS_FreeValue(ctx, module);
        if (JS_IsException(module_namespace)) {
            *error = "binding '" + descriptor.name +
                     "' namespace resolution failed: " +
                     js_error_text(ctx);
            JS_FreeValue(ctx, module_namespace);
            return false;
        }
        JSValue factory =
            JS_GetPropertyStr(ctx, module_namespace, "default");
        JS_FreeValue(ctx, module_namespace);
        if (!JS_IsFunction(ctx, factory)) {
            JS_FreeValue(ctx, factory);
            *error = "binding '" + descriptor.name +
                     "' default export is not a synchronous factory";
            return false;
        }
        JSValue config = JS_ParseJSON(
            ctx,
            descriptor.config_json.data(),
            descriptor.config_json.size(),
            "binding-config");
        if (JS_IsException(config)) {
            *error = "binding '" + descriptor.name +
                     "' config is not valid JSON: " +
                     js_error_text(ctx);
            JS_FreeValue(ctx, config);
            JS_FreeValue(ctx, factory);
            return false;
        }
        if (!JS_IsObject(config) || JS_IsArray(config)) {
            *error = "binding '" + descriptor.name +
                     "' config must be a JSON object";
            JS_FreeValue(ctx, config);
            JS_FreeValue(ctx, factory);
            return false;
        }
        JSValue secrets = JS_NewObjectProto(ctx, JS_NULL);
        if (JS_IsException(secrets)) {
            *error = "binding '" + descriptor.name +
                     "' secrets object allocation failed";
            JS_FreeValue(ctx, secrets);
            JS_FreeValue(ctx, config);
            JS_FreeValue(ctx, factory);
            return false;
        }
        for (size_t index = 0;
             index < descriptor.secrets.size();
             ++index) {
            JSValue secret_value = JS_NewStringLen(
                ctx,
                reinterpret_cast<const char *>(
                    descriptor.secrets[index].value.data()),
                descriptor.secrets[index].value.size());
            if (JS_IsException(secret_value) ||
                JS_SetPropertyStr(
                    ctx, secrets,
                    descriptor.secrets[index].key.c_str(),
                    secret_value) < 0) {
                *error = "binding '" + descriptor.name +
                         "' secrets object construction failed: " +
                         js_error_text(ctx);
                JS_FreeValue(ctx, secrets);
                JS_FreeValue(ctx, config);
                JS_FreeValue(ctx, factory);
                return false;
            }
        }
        std::vector<std::string> &log_secrets =
            binding_log_secret_values_[descriptor.name];
        log_secrets.clear();
        for (size_t index = 0; index < descriptor.secrets.size(); ++index) {
            const std::vector<uint8_t> &bytes =
                descriptor.secrets[index].value;
            if (!bytes.empty()) {
                log_secrets.push_back(std::string(
                    reinterpret_cast<const char *>(bytes.data()),
                    bytes.size()));
            }
        }
        std::sort(log_secrets.begin(), log_secrets.end(),
                  [](const std::string &left, const std::string &right) {
                      if (left.size() != right.size()) {
                          return left.size() > right.size();
                      }
                      return left < right;
                  });
        log_secrets.erase(
            std::unique(log_secrets.begin(), log_secrets.end()),
            log_secrets.end());
        // §2.4: every plain object in config and secrets is
        // null-prototype and every reachable object/array is frozen. The
        // descriptor walk above never performs JS_GetProperty, so getters
        // cannot execute while the trust boundary is being prepared.
        std::string preparation_error;
        if (!prepare_binding_init_value(
                ctx, config, 1, &preparation_error) ||
            !prepare_binding_init_value(
                ctx, secrets, 1, &preparation_error)) {
            if (JS_HasException(ctx)) {
                preparation_error += ": " + js_error_text(ctx);
            }
            *error = "binding '" + descriptor.name +
                     "' invalid factory input: " + preparation_error;
            JS_FreeValue(ctx, secrets);
            JS_FreeValue(ctx, config);
            JS_FreeValue(ctx, factory);
            return false;
        }
        JSValue log = build_binding_log_object(ctx, descriptor.name);
        if (JS_IsException(log)) {
            *error = "binding '" + descriptor.name +
                     "' log object construction failed: " +
                     js_error_text(ctx);
            JS_FreeValue(ctx, log);
            JS_FreeValue(ctx, secrets);
            JS_FreeValue(ctx, config);
            JS_FreeValue(ctx, factory);
            return false;
        }
        // §2.4: the factory runs outside any binding context — factory
        // initialization is pure JavaScript, so every native gate fails
        // closed during it.
        current_binding_id_.clear();
        current_binding_request_id_ = 0;
        // §2.4: the factory receives one frozen object argument
        // { config, secrets, log }. Duplicates are transferred into `init`;
        // the local references are released immediately after freezing it.
        JSValue init = JS_NewObjectProto(ctx, JS_NULL);
        if (JS_IsException(init) ||
            JS_SetPropertyStr(ctx, init, "config",
                              JS_DupValue(ctx, config)) < 0 ||
            JS_SetPropertyStr(ctx, init, "secrets",
                              JS_DupValue(ctx, secrets)) < 0 ||
            JS_SetPropertyStr(ctx, init, "log",
                              JS_DupValue(ctx, log)) < 0 ||
            JS_FreezeObject(ctx, init) < 0) {
            *error = "binding '" + descriptor.name +
                     "' init object construction failed: " +
                     js_error_text(ctx);
            JS_FreeValue(ctx, init);
            JS_FreeValue(ctx, log);
            JS_FreeValue(ctx, secrets);
            JS_FreeValue(ctx, config);
            JS_FreeValue(ctx, factory);
            return false;
        }
        JS_FreeValue(ctx, log);
        JS_FreeValue(ctx, secrets);
        JS_FreeValue(ctx, config);
        JSValue args[1] = { init };
        JSValue result =
            JS_Call(ctx, factory, JS_UNDEFINED, 1, args);
        JS_FreeValue(ctx, init);
        JS_FreeValue(ctx, factory);
        if (JS_IsException(result)) {
            *error = "binding '" + descriptor.name +
                     "' factory threw: " + js_error_text(ctx);
            JS_FreeValue(ctx, result);
            return false;
        }
        if (!JS_IsObject(result)) {
            JS_FreeValue(ctx, result);
            *error = "binding '" + descriptor.name +
                     "' factory must return an object";
            return false;
        }
        // A Proxy can execute arbitrary ownKeys/getOwnPropertyDescriptor
        // traps during discovery. Reject it before making any reflective
        // call, so even a hostile trap is never invoked.
        if (JS_IsProxy(result)) {
            JS_FreeValue(ctx, result);
            *error = "binding '" + descriptor.name +
                     "' factory returned a Proxy method table";
            return false;
        }

        JSPropertyEnum *tab = NULL;
        uint32_t count = 0;
        if (JS_GetOwnPropertyNames(
                ctx, &tab, &count, result,
                JS_GPN_STRING_MASK | JS_GPN_SYMBOL_MASK |
                    JS_GPN_SET_ENUM) < 0) {
            JS_FreeValue(ctx, result);
            *error = "binding '" + descriptor.name +
                     "' method enumeration failed";
            return false;
        }
        std::vector<std::string> method_names;
        for (uint32_t index = 0; index < count; ++index) {
            JSValue atom_value =
                JS_AtomToValue(ctx, tab[index].atom);
            const bool is_symbol = JS_IsSymbol(atom_value);
            JS_FreeValue(ctx, atom_value);
            if (is_symbol) {
                JS_FreePropertyEnum(ctx, tab, count);
                JS_FreeValue(ctx, result);
                *error = "binding '" + descriptor.name +
                         "' method table contains a symbol property";
                return false;
            }
            const char *name =
                JS_AtomToCString(ctx, tab[index].atom);
            if (name == NULL) {
                JS_FreePropertyEnum(ctx, tab, count);
                JS_FreeValue(ctx, result);
                *error = "binding '" + descriptor.name +
                         "' method name is not printable";
                return false;
            }
            const std::string method_name(name);
            JS_FreeCString(ctx, name);
            JSPropertyDescriptor member_descriptor;
            if (JS_GetOwnProperty(ctx, &member_descriptor,
                                  result, tab[index].atom) != 1) {
                JS_FreePropertyEnum(ctx, tab, count);
                JS_FreeValue(ctx, result);
                *error = "binding '" + descriptor.name +
                         "' method property is unreadable";
                return false;
            }
            const bool is_accessor =
                !JS_IsUndefined(member_descriptor.getter) ||
                !JS_IsUndefined(member_descriptor.setter);
            const bool is_function =
                JS_IsFunction(ctx, member_descriptor.value);
            JS_FreeValue(ctx, member_descriptor.getter);
            JS_FreeValue(ctx, member_descriptor.setter);
            JS_FreeValue(ctx, member_descriptor.value);
            if (is_accessor) {
                JS_FreePropertyEnum(ctx, tab, count);
                JS_FreeValue(ctx, result);
                *error = "binding '" + descriptor.name +
                         "' method table contains an accessor: " +
                         method_name;
                return false;
            }
            if (!tab[index].is_enumerable) {
                continue;
            }
            if (!valid_binding_method_name(method_name)) {
                JS_FreePropertyEnum(ctx, tab, count);
                JS_FreeValue(ctx, result);
                *error = "binding '" + descriptor.name +
                         "' exposes an invalid method name: " +
                         method_name;
                return false;
            }
            if (!is_function) {
                JS_FreePropertyEnum(ctx, tab, count);
                JS_FreeValue(ctx, result);
                *error = "binding '" + descriptor.name +
                         "' method is not a function: " + method_name;
                return false;
            }
            // Method identifiers cross the Runtime boundary only as owned
            // bytes. QuickJS atoms belong to one JSRuntime; retaining a
            // Binding atom and decoding it in the User Runtime can address
            // an unrelated or invalid atom slot.
            method_names.push_back(method_name);
        }
        JS_FreePropertyEnum(ctx, tab, count);
        if (method_names.empty() || method_names.size() > 128) {
            JS_FreeValue(ctx, result);
            *error = "binding '" + descriptor.name +
                     "' must expose between 1 and 128 methods";
            return false;
        }
        // Freeze before any queued Promise job can run. Dispatch resolves
        // methods from this exact object, so a package cannot change the
        // audited surface after factory return.
        if (JS_FreezeObject(ctx, result) < 0) {
            *error = "binding '" + descriptor.name +
                     "' method table could not be frozen: " +
                     js_error_text(ctx);
            JS_FreeValue(ctx, result);
            return false;
        }
        table->id = descriptor.name;
        table->policy = binding_policies_.policy(descriptor.name);
        table->factory_object = JS_DupValue(ctx, result);
        table->method_names = std::move(method_names);
        JS_FreeValue(ctx, result);
        return true;
    }

    bool create_binding_runtime(std::string *error) {
        if (bindings_.empty()) {
            // §7.2: zero bindings keep the exact single-runtime path.
            return true;
        }
        // §5.1: one runtime/context per Binding. Each table owns its
        // runtime, globals, module cache and job queue; nothing crosses
        // between Binding packages except the neutral C++ RPC queues.
        for (size_t index = 0; index < bindings_.size(); ++index) {
            BindingRuntimeMethodTable table;
            table.id = bindings_[index].name;
            table.policy =
                binding_policies_.policy(bindings_[index].name);
            TJSRunOptions options;
            TJS_DefaultOptions(&options);
            options.mem_limit = 64 * 1024 * 1024;  // Binding Heap, §5.3
            options.stack_size = config_.js_stack_size;
            options.skip_run_main = true;
            // The same bootstrap bytecode installs the profile global
            // surface; the bootstrapping_binding_runtime_ flag switches the
            // native layer to the bridge-less Binding shape.
            options.bootstrap = bootstrap;
            options.bootstrap_opaque = this;
            options.shared_loop = TJS_GetLoop(runtime_);
            bootstrapping_binding_runtime_ = true;
            table.runtime = TJS_NewRuntimeOptions(&options);
            bootstrapping_binding_runtime_ = false;
            if (table.runtime == NULL) {
                *error = "binding runtime creation failed for " + table.id;
                return false;
            }
            table.ctx = TJS_GetJSContext(table.runtime);
            if (table.ctx == NULL) {
                release_binding_table(&table);
                *error = "binding runtime has no context: " + table.id;
                return false;
            }
            JS_SetInterruptHandler(
                JS_GetRuntime(table.ctx), binding_interrupt_handler, this);
            // Capture this runtime's intrinsics before any Host package
            // evaluates. A package may mutate its own global, but RPC
            // plumbing for this package must continue to use the original
            // Promise and abort operations rather than attacker-visible
            // property lookups.
            JSValue global = JS_GetGlobalObject(table.ctx);
            table.promise_ctor =
                JS_GetPropertyStr(table.ctx, global, "Promise");
            table.abort_controller_ctor =
                JS_GetPropertyStr(table.ctx, global, "AbortController");
            JS_FreeValue(table.ctx, global);
            if (JS_IsException(table.promise_ctor) ||
                !JS_IsConstructor(table.ctx, table.promise_ctor) ||
                JS_IsException(table.abort_controller_ctor) ||
                !JS_IsConstructor(
                    table.ctx, table.abort_controller_ctor)) {
                release_binding_table(&table);
                *error = "binding runtime intrinsics are unavailable: " +
                         table.id;
                return false;
            }
            table.promise_resolve = JS_GetPropertyStr(
                table.ctx, table.promise_ctor, "resolve");
            JSValue promise_prototype = JS_GetPropertyStr(
                table.ctx, table.promise_ctor, "prototype");
            if (!JS_IsException(promise_prototype)) {
                table.promise_then = JS_GetPropertyStr(
                    table.ctx, promise_prototype, "then");
            }
            JS_FreeValue(table.ctx, promise_prototype);
            JSValue abort_prototype = JS_GetPropertyStr(
                table.ctx, table.abort_controller_ctor, "prototype");
            if (!JS_IsException(abort_prototype)) {
                table.abort = JS_GetPropertyStr(
                    table.ctx, abort_prototype, "abort");
            }
            JS_FreeValue(table.ctx, abort_prototype);
            if (!JS_IsFunction(table.ctx, table.promise_resolve) ||
                !JS_IsFunction(table.ctx, table.promise_then) ||
                !JS_IsFunction(table.ctx, table.abort)) {
                release_binding_table(&table);
                *error = "binding runtime control intrinsics are unavailable: " +
                         table.id;
                return false;
            }
            JS_SetModuleLoaderFunc2(
                JS_GetRuntime(table.ctx),
                binding_normalize_module,
                binding_module_load,
                deny_attributes,
                this);
            install_binding_async_hooks(table.ctx);
            // Each Binding Runtime's job queue is pumped by the User
            // runtime's loop; it never calls TJS_Run itself.
            TJS_StartRuntimeJobs(table.runtime);

            // Resolve imports as this package, while module evaluation,
            // factory execution and method-table inspection retain no
            // native capability/owner token.
            loading_binding_id_ = table.id;
            current_binding_id_.clear();
            current_binding_request_id_ = 0;
            const bool loaded =
                load_binding_package(bindings_[index], &table, error);
            loading_binding_id_.clear();
            current_binding_id_.clear();
            current_binding_request_id_ = 0;
            if (!loaded) {
                release_binding_table(&table);
                return false;
            }
            binding_table_index_[table.id] = binding_tables_.size();
            binding_tables_.push_back(std::move(table));
        }
        return true;
    }

    // --- Binding v1 RPC (§5.2, §7.6) ------------------------------------

    // The synthetic user facade: capsid:binding/<id> resolves to a frozen
    // default export whose methods enqueue neutral-value calls.
    static JSValue rejected_promise(JSContext *ctx, JSValue error) {
        JSValue resolving[2];
        JSValue promise = JS_NewPromiseCapability(ctx, resolving);
        if (JS_IsException(promise)) {
            JS_FreeValue(ctx, error);
            return promise;
        }
        JSValue ignored =
            JS_Call(ctx, resolving[1], JS_UNDEFINED, 1, &error);
        JS_FreeValue(ctx, ignored);
        JS_FreeValue(ctx, error);
        JS_FreeValue(ctx, resolving[1]);
        JS_FreeValue(ctx, resolving[0]);
        return promise;
    }

    static int binding_facade_init(JSContext *ctx, JSModuleDef *module) {
        WorkerRuntime *self = g_worker;
        if (self == NULL) {
            return -1;
        }
        const JSAtom name_atom = JS_GetModuleName(ctx, module);
        const char *name = JS_AtomToCString(ctx, name_atom);
        if (name == NULL) {
            return -1;
        }
        const std::string module_name(name);
        JS_FreeCString(ctx, name);
        JS_FreeAtom(ctx, name_atom);
        if (module_name.compare(0, 15, "capsid:binding/") != 0) {
            return -1;
        }
        const std::string binding_id = module_name.substr(15);
        const std::map<std::string, size_t>::const_iterator found =
            self->binding_table_index_.find(binding_id);
        if (found == self->binding_table_index_.end()) {
            return -1;
        }
        const size_t table_index = found->second;
        const BindingRuntimeMethodTable &table =
            self->binding_tables_[table_index];
        JSValue facade = JS_NewObjectProto(ctx, JS_NULL);
        for (size_t index = 0; index < table.method_names.size(); ++index) {
            JSValue data = JS_NewInt32(
                ctx, static_cast<int32_t>(table_index));
            JSValue function = JS_NewCFunctionData2(
                ctx, binding_facade_call,
                table.method_names[index].c_str(), 1,
                static_cast<int>(index), 1, &data);
            JS_FreeValue(ctx, data);
            JS_SetPropertyStr(ctx, facade,
                              table.method_names[index].c_str(), function);
        }
        JS_FreezeObject(ctx, facade);
        if (JS_SetModuleExport(ctx, module, "default", facade) < 0) {
            return -1;
        }
        return 0;
    }

    uint64_t allocate_binding_call_id() {
        // Zero is reserved.  At most 1024 calls can be live, so probing one
        // more slot than the table size is sufficient even across wraparound.
        for (size_t probe = 0;
             probe <= pending_binding_calls_.size();
             ++probe) {
            const uint64_t candidate = next_binding_call_id_;
            next_binding_call_id_ =
                candidate == std::numeric_limits<uint64_t>::max()
                    ? 1
                    : candidate + 1;
            if (candidate != 0 &&
                pending_binding_calls_.count(candidate) == 0) {
                return candidate;
            }
        }
        return 0;
    }

    static JSValue binding_facade_call(JSContext *ctx,
                                       JSValueConst this_val,
                                       int argc,
                                       JSValueConst *argv,
                                       int magic,
                                       JSValueConst *func_data) {
        (void)this_val;
        (void)argc;
        WorkerRuntime *self = g_worker;
        if (self == NULL || func_data == NULL) {
            return JS_ThrowInternalError(ctx, "worker is not available");
        }
        int table_index = -1;
        if (JS_ToInt32(ctx, &table_index, func_data[0]) != 0 ||
            table_index < 0 ||
            static_cast<size_t>(table_index) >=
                self->binding_tables_.size()) {
            return JS_ThrowInternalError(ctx, "binding facade is corrupt");
        }
        const size_t method_index = static_cast<size_t>(magic);
        const BindingRuntimeMethodTable &table =
            self->binding_tables_[table_index];
        if (method_index >= table.method_names.size()) {
            return JS_ThrowInternalError(ctx, "binding method is corrupt");
        }

        // §2.4: calls require an active request context; anything else
        // resolves to a rejected promise.
        RequestToken *token =
            self->require_active_request(ctx, 0, false, true);
        if (token == NULL) {
            JSValue error = JS_NewError(ctx);
            JS_DefinePropertyValueStr(
                ctx, error, "message",
                JS_NewString(ctx,
                             "binding calls require an active request"),
                JS_PROP_C_W_E);
            return rejected_promise(ctx, error);
        }
        // §5.3 quotas: 64 per request, 1024 per worker.
        if (self->inflight_binding_calls_ >= 1024) {
            JSValue error = JS_NewError(ctx);
            JS_DefinePropertyValueStr(
                ctx, error, "message",
                JS_NewString(ctx, "binding call quota exceeded"),
                JS_PROP_C_W_E);
            return rejected_promise(ctx, error);
        }
        size_t request_calls = 0;
        for (std::map<uint64_t, PendingBindingCall>::const_iterator it =
                 self->pending_binding_calls_.begin();
             it != self->pending_binding_calls_.end();
             ++it) {
            if (it->second.request_id == token->request_id) {
                ++request_calls;
            }
        }
        if (request_calls >= 64) {
            JSValue error = JS_NewError(ctx);
            JS_DefinePropertyValueStr(
                ctx, error, "message",
                JS_NewString(ctx, "binding request quota exceeded"),
                JS_PROP_C_W_E);
            return rejected_promise(ctx, error);
        }

        // §5.3: the input is cloned to a C++ neutral value; a rejected
        // value fails the call before anything crosses.
        capsid::NeutralValue input;
        std::string clone_error;
        JSValueConst input_value =
            argc > 0 ? argv[0] : JS_UNDEFINED;
        if (!capsid::neutral_from_js(
                ctx, input_value, &input, &clone_error)) {
            JSValue error = JS_NewError(ctx);
            JS_DefinePropertyValueStr(
                ctx, error, "message",
                JS_NewString(ctx, clone_error.c_str()),
                JS_PROP_C_W_E);
            return rejected_promise(ctx, error);
        }

        JSValue resolving[2];
        JSValue promise = JS_NewPromiseCapability(ctx, resolving);
        if (JS_IsException(promise)) {
            return promise;
        }

        PendingBindingCall call;
        call.binding_id = table.id;
        call.table_index = static_cast<size_t>(table_index);
        call.method = table.method_names[method_index];
        call.input = std::move(input);
        call.request_id = token->request_id;
        call.deadline_ns = token->deadline_ns;
        call.user_resolve = resolving[0];
        call.user_reject = resolving[1];
        call.abort_controller =
            self->new_binding_abort_controller(table);
        const uint64_t call_id = self->allocate_binding_call_id();
        if (call_id == 0) {
            self->self_finish_binding_call(call);
            JS_FreeValue(ctx, promise);
            return JS_ThrowInternalError(
                ctx, "binding call id space is exhausted");
        }
        self->pending_binding_calls_[call_id] = call;
        self->user_to_binding_.push_back(call_id);
        ++self->inflight_binding_calls_;
        return promise;
    }

    // Creates an AbortController inside the target Binding Runtime; cancel
    // and deadline abort it, and the call object exposes its signal.
    JSValue new_binding_abort_controller(
        const BindingRuntimeMethodTable &table) {
        if (table.ctx == NULL ||
            !JS_IsConstructor(table.ctx, table.abort_controller_ctor)) {
            return JS_UNDEFINED;
        }
        return JS_CallConstructor(
            table.ctx, table.abort_controller_ctor, 0, NULL);
    }

    JSValue binding_abort_signal(const BindingRuntimeMethodTable &table,
                                 JSValueConst controller) {
        JSContext *ctx = table.ctx;
        // txiki's controller constructor creates `signal` as an own data
        // property. Read the descriptor directly so a package cannot replace
        // a prototype getter and execute code inside the RPC setup path.
        JSAtom signal_atom = JS_NewAtom(ctx, "signal");
        JSPropertyDescriptor descriptor;
        const int present = JS_GetOwnProperty(
            ctx, &descriptor, controller, signal_atom);
        JS_FreeAtom(ctx, signal_atom);
        if (present <= 0) {
            return JS_EXCEPTION;
        }
        const bool accessor = !JS_IsUndefined(descriptor.getter) ||
                              !JS_IsUndefined(descriptor.setter);
        JS_FreeValue(ctx, descriptor.getter);
        JS_FreeValue(ctx, descriptor.setter);
        if (accessor) {
            JS_FreeValue(ctx, descriptor.value);
            return JS_EXCEPTION;
        }
        return descriptor.value;
    }

    static JSValue binding_fulfilled(JSContext *ctx,
                                     JSValueConst this_val,
                                     int argc,
                                     JSValueConst *argv,
                                     int magic,
                                     JSValueConst *func_data) {
        (void)this_val;
        (void)magic;
        WorkerRuntime *self = g_worker;
        if (self == NULL || func_data == NULL) {
            return JS_UNDEFINED;
        }
        uint64_t call_id = 0;
        if (!capsid::binding_call_id_from_js(
                ctx, func_data[0], &call_id)) {
            return JS_UNDEFINED;
        }
        std::map<uint64_t, PendingBindingCall>::iterator found =
            self->pending_binding_calls_.find(call_id);
        if (found == self->pending_binding_calls_.end() ||
            found->second.state != BindingCallState::kDispatched) {
            return JS_UNDEFINED;
        }
        PendingBindingCall &call = found->second;
        JS_FreeValue(ctx, call.binding_result);
        call.binding_result =
            argc > 0 ? JS_DupValue(ctx, argv[0]) : JS_UNDEFINED;
        JS_FreeValue(ctx, call.binding_error);
        call.binding_error = JS_UNDEFINED;
        call.state = BindingCallState::kSettled;
        self->binding_to_user_.push_back(call_id);
        return JS_UNDEFINED;
    }

    static JSValue binding_rejected(JSContext *ctx,
                                    JSValueConst this_val,
                                    int argc,
                                    JSValueConst *argv,
                                    int magic,
                                    JSValueConst *func_data) {
        (void)this_val;
        (void)magic;
        WorkerRuntime *self = g_worker;
        if (self == NULL || func_data == NULL) {
            return JS_UNDEFINED;
        }
        uint64_t call_id = 0;
        if (!capsid::binding_call_id_from_js(
                ctx, func_data[0], &call_id)) {
            return JS_UNDEFINED;
        }
        std::map<uint64_t, PendingBindingCall>::iterator found =
            self->pending_binding_calls_.find(call_id);
        if (found == self->pending_binding_calls_.end() ||
            found->second.state != BindingCallState::kDispatched) {
            return JS_UNDEFINED;
        }
        PendingBindingCall &call = found->second;
        JS_FreeValue(ctx, call.binding_error);
        call.binding_error =
            argc > 0 ? JS_DupValue(ctx, argv[0]) : JS_UNDEFINED;
        JS_FreeValue(ctx, call.binding_result);
        call.binding_result = JS_UNDEFINED;
        call.state = BindingCallState::kSettled;
        self->binding_to_user_.push_back(call_id);
        return JS_UNDEFINED;
    }

    // Takes ownership of `error`. Only a live dispatched call may enter the
    // result queue; cancellation erases the table entry, so a late Promise
    // reaction is an idempotent no-op.
    void settle_binding_call_error(uint64_t call_id,
                                   const BindingRuntimeMethodTable &table,
                                   JSValue error) {
        JSContext *ctx = table.ctx;
        std::map<uint64_t, PendingBindingCall>::iterator found =
            pending_binding_calls_.find(call_id);
        if (found == pending_binding_calls_.end() ||
            found->second.state != BindingCallState::kDispatched) {
            JS_FreeValue(ctx, error);
            return;
        }
        PendingBindingCall &call = found->second;
        JS_FreeValue(ctx, call.binding_error);
        call.binding_error = error;
        JS_FreeValue(ctx, call.binding_result);
        call.binding_result = JS_UNDEFINED;
        call.state = BindingCallState::kSettled;
        binding_to_user_.push_back(call_id);
    }

    JSValue take_binding_exception(const BindingRuntimeMethodTable &table,
                                   const char *fallback) {
        JSContext *ctx = table.ctx;
        JSValue exception = JS_GetException(ctx);
        if (!JS_IsUndefined(exception) && !JS_IsNull(exception)) {
            return exception;
        }
        JS_FreeValue(ctx, exception);
        JSValue error = JS_NewError(ctx);
        JS_DefinePropertyValueStr(
            ctx, error, "message",
            JS_NewString(ctx, fallback), JS_PROP_C_W_E);
        return error;
    }

    // Dispatches one queued call inside the Binding Runtime (this runs on
    // the worker's single thread, between User phases — the same-thread
    // sequential entry the scheduler guarantees).
    void pump_binding_calls() {
        if (binding_tables_.empty() || user_to_binding_.empty()) {
            return;
        }
        const uint64_t call_id = user_to_binding_.front();
        user_to_binding_.pop_front();
        std::map<uint64_t, PendingBindingCall>::iterator found =
            pending_binding_calls_.find(call_id);
        if (found == pending_binding_calls_.end()) {
            return;
        }
        PendingBindingCall &call = found->second;
        if (call.state != BindingCallState::kQueued) {
            return;
        }
        call.state = BindingCallState::kDispatched;
        BindingDispatchScope dispatch_scope(this);
        current_binding_call_id_ = call_id;
        // The dispatch window sets the binding context the egress gate
        // (and later native gates) read; all return paths restore it.
        BindingWindowGuard binding_window(
            current_binding_id_, current_binding_request_id_,
            call.binding_id, call.request_id);
        const BindingRuntimeMethodTable &table =
            binding_tables_[call.table_index];
        JSContext *ctx = table.ctx;
        JSValue method =
            JS_GetPropertyStr(ctx, table.factory_object,
                              call.method.c_str());
        if (JS_IsException(method)) {
            JS_FreeValue(ctx, method);
            settle_binding_call_error(call_id, table,
                take_binding_exception(table, "binding method lookup failed"));
            return;
        }
        if (!JS_IsFunction(ctx, method)) {
            JS_FreeValue(ctx, method);
            JSValue error = JS_NewError(ctx);
            JS_DefinePropertyValueStr(
                ctx, error, "message",
                JS_NewString(ctx, "binding method is unavailable"),
                JS_PROP_C_W_E);
            settle_binding_call_error(call_id, table, error);
            return;
        }
        JSValue input = capsid::neutral_to_js(ctx, call.input);
        JSValue call_object = JS_NewObjectProto(ctx, JS_NULL);
        if (JS_IsException(input) || JS_IsException(call_object)) {
            JS_FreeValue(ctx, input);
            JS_FreeValue(ctx, call_object);
            JS_FreeValue(ctx, method);
            settle_binding_call_error(call_id, table,
                take_binding_exception(table, "binding call setup failed"));
            return;
        }
        bool call_object_ok =
            JS_SetPropertyStr(
                ctx, call_object, "requestId",
                JS_NewBigUint64(ctx, call.request_id)) >= 0 &&
            JS_SetPropertyStr(
                ctx, call_object, "deadline",
                JS_NewBigUint64(ctx, call.deadline_ns)) >= 0;
        if (!JS_IsUndefined(call.abort_controller)) {
            JSValue signal = binding_abort_signal(table, call.abort_controller);
            if (JS_IsException(signal)) {
                JS_FreeValue(ctx, signal);
                call_object_ok = false;
            } else if (JS_SetPropertyStr(
                           ctx, call_object, "signal", signal) < 0) {
                call_object_ok = false;
            }
        } else {
            call_object_ok =
                JS_SetPropertyStr(ctx, call_object, "signal",
                                  JS_UNDEFINED) >= 0 &&
                call_object_ok;
        }
        call_object_ok = call_object_ok &&
                         JS_FreezeObject(ctx, call_object) >= 0;
        if (!call_object_ok) {
            JS_FreeValue(ctx, input);
            JS_FreeValue(ctx, call_object);
            JS_FreeValue(ctx, method);
            settle_binding_call_error(call_id, table,
                take_binding_exception(table, "binding call metadata failed"));
            return;
        }
        JSValue args[2] = { input, call_object };
        JSValue result = JS_Call(
            ctx, method, table.factory_object, 2, args);
        JS_FreeValue(ctx, input);
        JS_FreeValue(ctx, call_object);
        JS_FreeValue(ctx, method);
        if (JS_IsException(result)) {
            const bool interrupted =
                binding_interrupted_call_id_ == call_id;
            JS_FreeValue(ctx, result);
            settle_binding_call_error(call_id, table,
                take_binding_exception(table, "binding call threw"));
            if (interrupted) {
                // A synchronous CPU loop is an uninterruptible-cooperation
                // violation: the call is settled as an error and the whole
                // worker enters the bounded poison path (§5.3).
                enter_poison("binding call interrupted");
            }
            return;
        }
        // §5.2: Promise.resolve() unifies sync and async returns.
        JSValue resolved = JS_Call(
            ctx, table.promise_resolve,
            table.promise_ctor, 1, &result);
        JS_FreeValue(ctx, result);
        if (JS_IsException(resolved)) {
            JS_FreeValue(ctx, resolved);
            settle_binding_call_error(call_id, table,
                take_binding_exception(table, "binding result normalization failed"));
            return;
        }
        JSValue data =
            capsid::binding_call_id_to_js(ctx, call_id);
        JSValue on_fulfilled = JS_NewCFunctionData2(
            ctx, binding_fulfilled, "onFulfilled", 1, 0, 1,
            &data);
        JSValue on_rejected = JS_NewCFunctionData2(
            ctx, binding_rejected, "onRejected", 1, 0, 1, &data);
        JS_FreeValue(ctx, data);
        if (JS_IsException(on_fulfilled) ||
            JS_IsException(on_rejected)) {
            JS_FreeValue(ctx, on_fulfilled);
            JS_FreeValue(ctx, on_rejected);
            JS_FreeValue(ctx, resolved);
            settle_binding_call_error(call_id, table,
                take_binding_exception(table, "binding settlement setup failed"));
            return;
        }
        JSValue handlers[2] = { on_fulfilled, on_rejected };
        JSValue then_result = JS_Call(
            ctx, table.promise_then,
            resolved, 2, handlers);
        JS_FreeValue(ctx, on_fulfilled);
        JS_FreeValue(ctx, on_rejected);
        JS_FreeValue(ctx, resolved);
        if (JS_IsException(then_result)) {
            JS_FreeValue(ctx, then_result);
            settle_binding_call_error(call_id, table,
                take_binding_exception(table, "binding settlement failed"));
            return;
        }
        JS_FreeValue(ctx, then_result);
        // The .then handlers settle the call on a later job tick.
    }

    // Resolves/rejects the User-side promise from a settled Binding call
    // (runs in the User phase; values are cloned across runtimes).
    void pump_binding_results() {
        while (!binding_to_user_.empty()) {
            const uint64_t call_id = binding_to_user_.front();
            binding_to_user_.pop_front();
            std::map<uint64_t, PendingBindingCall>::iterator found =
                pending_binding_calls_.find(call_id);
            if (found == pending_binding_calls_.end()) {
                continue;
            }
            PendingBindingCall &call = found->second;
            if (call.state != BindingCallState::kSettled) {
                continue;
            }
            const BindingRuntimeMethodTable &table =
                binding_tables_[call.table_index];
            JSContext *binding_ctx = table.ctx;
            JSValue handler =
                JS_IsUndefined(call.binding_error)
                    ? call.user_resolve
                    : call.user_reject;
            JSValue payload = JS_UNDEFINED;
            if (JS_IsUndefined(call.binding_error)) {
                // Clone the Binding value into the User runtime.
                capsid::NeutralValue neutral;
                std::string clone_error;
                if (!capsid::neutral_from_js(
                        binding_ctx, call.binding_result, &neutral,
                        &clone_error)) {
                    payload = JS_NewError(ctx_);
                    JS_DefinePropertyValueStr(
                        ctx_, payload, "message",
                        JS_NewString(
                            ctx_,
                            ("binding result is not cloneable: " +
                             clone_error)
                                .c_str()),
                        JS_PROP_C_W_E);
                    handler = call.user_reject;
                } else {
                    payload =
                        capsid::neutral_to_js(ctx_, neutral);
                }
            } else {
                // Errors cross as message text only.
                // A raw rejection reason may arrive as an exception
                // placeholder whose text is unrecoverable; fail with a
                // stable message in that case.
                std::string error_text;
                if (JS_IsException(call.binding_error)) {
                    error_text = "binding call failed";
                    JS_FreeValue(binding_ctx, call.binding_error);
                    call.binding_error = JS_UNDEFINED;
                } else {
                    const char *text =
                        JS_ToCString(binding_ctx, call.binding_error);
                    error_text = text ? text : "binding call failed";
                    if (text != NULL) {
                        JS_FreeCString(binding_ctx, text);
                    }
                }

                payload = JS_NewError(ctx_);
                JS_DefinePropertyValueStr(
                    ctx_, payload, "message",
                    JS_NewString(ctx_, error_text.c_str()),
                    JS_PROP_C_W_E);
            }
            JSValue settle = JS_Call(ctx_, handler, JS_UNDEFINED, 1,
                                     &payload);
            JS_FreeValue(ctx_, settle);
            JS_FreeValue(ctx_, payload);
            finish_binding_call(found);
        }
    }

    // Frees every cross-runtime JSValue a pending call owns.
    void self_finish_binding_call(PendingBindingCall &call) {
        if (call.table_index < binding_tables_.size()) {
            JSContext *ctx = binding_tables_[call.table_index].ctx;
            if (ctx != NULL) {
                if (!JS_IsUndefined(call.abort_controller)) {
                    JS_FreeValue(ctx, call.abort_controller);
                    call.abort_controller = JS_UNDEFINED;
                }
                JS_FreeValue(ctx, call.binding_result);
                call.binding_result = JS_UNDEFINED;
                JS_FreeValue(ctx, call.binding_error);
                call.binding_error = JS_UNDEFINED;
            }
        }
        JS_FreeValue(ctx_, call.user_resolve);
        call.user_resolve = JS_UNDEFINED;
        JS_FreeValue(ctx_, call.user_reject);
        call.user_reject = JS_UNDEFINED;
    }

    void finish_binding_call(
        std::map<uint64_t, PendingBindingCall>::iterator found) {
        if (found == pending_binding_calls_.end()) {
            return;
        }
        self_finish_binding_call(found->second);
        pending_binding_calls_.erase(found);
        if (inflight_binding_calls_ > 0) {
            --inflight_binding_calls_;
        }
    }

    // §5.2: request cancel/deadline aborts matching calls — undispatched
    // calls are dropped, dispatched ones have their controllers aborted
    // and their late results discarded.
    void cancel_binding_calls(uint64_t request_id) {
        for (std::map<uint64_t, PendingBindingCall>::iterator it =
                 pending_binding_calls_.begin();
             it != pending_binding_calls_.end();) {
            PendingBindingCall &call = it->second;
            if (call.request_id != request_id) {
                ++it;
                continue;
            }
            if (call.state == BindingCallState::kQueued) {
                // §5.2: an undispatched call is dropped immediately — the
                // queue entry and the inflight quota are released now,
                // not when the pump would have reached it.
                for (std::deque<uint64_t>::iterator q =
                         user_to_binding_.begin();
                     q != user_to_binding_.end(); ++q) {
                    if (*q == it->first) {
                        user_to_binding_.erase(q);
                        break;
                    }
                }
            }
            if (call.state == BindingCallState::kDispatched &&
                call.table_index < binding_tables_.size() &&
                !JS_IsUndefined(call.abort_controller)) {
                const BindingRuntimeMethodTable &table =
                    binding_tables_[call.table_index];
                if (table.ctx != NULL) {
                    // AbortSignal listeners run synchronously. They are Binding
                    // code and may close owned sockets/files, so restore the
                    // same immutable owner token used by the original dispatch.
                    BindingWindowGuard binding_window(
                        current_binding_id_, current_binding_request_id_,
                        call.binding_id, call.request_id);
                    JSValue ignored = JS_Call(
                        table.ctx, table.abort,
                        call.abort_controller, 0, NULL);
                    if (JS_IsException(ignored)) {
                        JSValue exception = JS_GetException(table.ctx);
                        JS_FreeValue(table.ctx, exception);
                    }
                    JS_FreeValue(table.ctx, ignored);
                }
            }
            JSValue error = JS_NewError(ctx_);
            JS_DefinePropertyValueStr(
                ctx_, error, "message",
                JS_NewString(ctx_, "binding call was canceled"),
                JS_PROP_C_W_E);
            JSValue settle =
                JS_Call(ctx_, call.user_reject, JS_UNDEFINED, 1, &error);
            JS_FreeValue(ctx_, settle);
            JS_FreeValue(ctx_, error);
            std::map<uint64_t, PendingBindingCall>::iterator doomed = it++;
            finish_binding_call(doomed);
        }
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
        // §5.2: cancel/deadline aborts every binding call of the request.
        cancel_binding_calls(id);
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
            // Forced reclamation is independent of Binding cooperation and
            // of whether the transport can queue the timeout terminal yet.
            cancel_binding_calls(id);
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
            shutdown();
            return;
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
    // Binding v1 descriptors in arrival order; empty for zero-binding
    // workers. No Binding Runtime is created when this stays empty.
    std::vector<capsid::WorkerBindingDescriptor> bindings_;
    // §7.3: per-Binding policies, compiled from the descriptors above and
    // fully separate from the User policy in config_.
    capsid::BindingPolicySet binding_policies_;
    std::vector<BindingRuntimeMethodTable> binding_tables_;
    // Native capability identity. Non-empty only inside method dispatch or
    // an async continuation captured from that dispatch.
    std::string current_binding_id_;
    uint64_t current_binding_request_id_ = 0;
    // Plaintext exists only in the worker's Binding startup snapshot and is
    // used here solely to redact Binding log message/fields before IPC.
    std::map<std::string, std::vector<std::string>>
        binding_log_secret_values_;
    // Import authorization identity used only while a package graph is
    // linked/evaluated. Native resource and operation gates never consult it.
    std::string loading_binding_id_;
    // True while TJS_NewRuntimeOptions bootstraps the Binding Runtime, so
    // the shared bootstrap() switches to the bridge-less native shape.
    bool bootstrapping_binding_runtime_;
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
