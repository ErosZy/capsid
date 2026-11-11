// Cancel a real native HttpClient after TCP connects, then deliver its
// already-writable socket before the scheduler dispatches the close. This
// is the event order observed in worker_fetch_cancel_lifecycle on Windows.
// The test uses public libuv/socket/lws service APIs; no product hooks,
// synthetic timeout writes, or replacements for network observations.
extern "C" {
#include "tjs.h"
#include "utils.h"
#include <uv.h>
void tjs__execute_jobs(JSContext *ctx);
struct lws_context *tjs__lws_get_context(JSContext *ctx);
int capsid_tjs_set_cookie_jar_path(TJSRuntime *runtime, const char *path);
int capsid_tjs_set_egress_policy(TJSRuntime *runtime,
                                 int (*check)(void *, const char *, uint16_t,
                                              const struct sockaddr *, socklen_t, char *, size_t),
                                 void *opaque);
}
#include "win32_compat.h"
#include <libwebsockets.h>
#if !defined(_WIN32)
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
struct ContextLog {
    int owner_a = 1, owner_b = 2;
    void *active = nullptr;
    int captures_a = 0, captures_b = 0, releases_a = 0, releases_b = 0;
    int native_captures = 0, native_releases = 0, native_denials = 0;
    static int capture(JSContext *, void *opaque, void **out) {
        auto &log = *static_cast<ContextLog *>(opaque);
        *out = log.active;
        if (*out == &log.owner_a) ++log.captures_a;
        if (*out == &log.owner_b) ++log.captures_b;
        return 0;
    }
    static void *enter(JSContext *, void *context, void *opaque) {
        auto &log = *static_cast<ContextLog *>(opaque);
        void *previous = log.active;
        log.active = context;
        return previous;
    }
    static void leave(JSContext *, void *previous, void *opaque) {
        static_cast<ContextLog *>(opaque)->active = previous;
    }
    static void release(void *context, void *opaque) {
        auto &log = *static_cast<ContextLog *>(opaque);
        if (context == &log.owner_a) ++log.releases_a;
        if (context == &log.owner_b) ++log.releases_b;
    }
    static bool matches(void *context, void *owner) { return context == owner; }
    static int capture_native(JSContext *, void *opaque, void **out) {
        auto &log = *static_cast<ContextLog *>(opaque);
        *out = &log.owner_a; ++log.native_captures; return 0;
    }
    static int authorize_native(JSContext *, void *owner, void *opaque) {
        auto &log = *static_cast<ContextLog *>(opaque);
        const bool allowed = owner == &log.owner_a && log.active == owner;
        if (!allowed) ++log.native_denials;
        return allowed;
    }
    static void release_native(void *, void *opaque) {
        ++static_cast<ContextLog *>(opaque)->native_releases;
    }
};
ContextLog *observed_contexts = nullptr;
JSValue current_context(JSContext *ctx, JSValueConst, int, JSValueConst *) {
    const auto &log = *observed_contexts;
    return JS_NewString(ctx, log.active == &log.owner_a ? "a" : log.active == &log.owner_b ? "b" : "none");
}

void require(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}

void evaluate(JSContext *ctx, const std::string &source) {
    JSValue value =
        JS_Eval(ctx, source.c_str(), source.size(), "abort-connect.js", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(value)) {
        JSValue error = JS_GetException(ctx);
        const char *message = JS_ToCString(ctx, error);
        const std::string detail = message ? message : "unprintable";
        JS_FreeCString(ctx, message);
        JS_FreeValue(ctx, error);
        throw std::runtime_error("JavaScript: " + detail);
    }
    JS_FreeValue(ctx, value);
}

JSValue noop(JSContext *, JSValueConst, int, JSValueConst *) { return JS_UNDEFINED; }

int bootstrap(TJSRuntime *, JSContext *ctx, void *) {
    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "dispatchEvent", JS_NewCFunction(ctx, noop, "dispatchEvent", 1));
    JS_SetPropertyStr(
        ctx, global, "PromiseRejectionEvent",
        JS_NewCFunction2(ctx, noop, "PromiseRejectionEvent", 0, JS_CFUNC_constructor, 0));
    JS_FreeValue(ctx, global);
    return 0;
}

class Peer {
  public:
    explicit Peer(bool hold_receive = false) : hold_receive_(hold_receive) {
        listener_ = capsid::win32::create_tcp_socket_fd();
        require(listener_ >= 0, "listener creation");
        try {
            struct sockaddr_in address = {};
            address.sin_family = AF_INET;
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            require(capsid::win32::bind_fd(listener_,
                                           reinterpret_cast<const struct sockaddr *>(&address),
                                           sizeof(address)) == 0,
                    "listener bind");
            require(capsid::win32::listen_fd(listener_, 1) == 0, "listener listen");
            socklen_t length = sizeof(address);
            require(capsid::win32::getsockname_fd(
                        listener_, reinterpret_cast<struct sockaddr *>(&address), &length) == 0,
                    "listener name");
            port_ = ntohs(address.sin_port);
            thread_ = std::thread([this] {
                capsid_pollfd descriptor = {};
                descriptor.fd = listener_;
                descriptor.events = POLLIN;
                if (capsid::win32::capsid_poll(&descriptor, 1, 5000) <= 0)
                    return;
                const int fd = capsid::win32::accept_fd(listener_);
                client_ = fd;
                if (fd < 0)
                    return;
                accepted_ = true;
                while (hold_receive_ && !receive_released_ && !stop_)
                    std::this_thread::yield();
                if (capsid::win32::setsockopt_recv_timeout_fd(fd, 100) != 0) {
                    receive_error_ = errno;
                    return;
                }
                char bytes[1024];
                while (!stop_) {
#if defined(_WIN32)
                    const ssize_t count = capsid::win32::recv_fd(fd, bytes, sizeof(bytes), 0);
#else
                    const ssize_t count = recv(fd, bytes, sizeof(bytes), 0);
#endif
                    if (count == 0) {
                        closed_ = true;
                        return;
                    }
                    if (count > 0) {
                        received_ += static_cast<unsigned>(count);
                        continue;
                    }
                    const int read_error = errno;
                    if (read_error == EINTR || read_error == EAGAIN || read_error == EWOULDBLOCK)
                        continue;
                    if (read_error == ECONNRESET) {
                        closed_ = true;
                        return;
                    }
#if defined(_WIN32)
                    if (WSAGetLastError() == WSAETIMEDOUT)
                        continue;
#endif
                    receive_error_ = read_error;
                    return;
                }
            });
        } catch (...) {
            close(listener_);
            throw;
        }
    }
    ~Peer() {
        stop_ = true;
        const int fd_before_join = client_;
        if (fd_before_join >= 0)
            capsid::win32::shutdown_fd(fd_before_join);
        capsid::win32::shutdown_fd(listener_);
        if (thread_.joinable())
            thread_.join();
        const int fd = client_;
        if (fd >= 0)
            close(fd);
        close(listener_);
    }
    void release_receive() { receive_released_ = true; }
    bool read_queue_empty() const {
        capsid_pollfd descriptor = {};
        descriptor.fd = client_;
        descriptor.events = POLLIN;
        return descriptor.fd >= 0 && capsid::win32::capsid_poll(&descriptor, 1, 0) == 0;
    }
    unsigned port() const { return port_; }
    bool accepted() const { return accepted_; }
    bool closed() const { return closed_; }
    unsigned received() const { return received_; }
    int error() const { return receive_error_; }

  private:
    bool hold_receive_ = false;
    std::atomic<bool> receive_released_{false};
    int listener_ = -1;
    std::atomic<int> client_{-1};
    unsigned port_ = 0;
    std::atomic<bool> accepted_{false}, closed_{false}, stop_{false};
    std::atomic<unsigned> received_{0};
    std::atomic<int> receive_error_{0};
    std::thread thread_;
};

struct ConnectPolicy {
    explicit ConnectPolicy(unsigned target_port) : port(target_port) {}
    unsigned port;
    unsigned calls = 0;
    bool allow_localhost = false;
    ContextLog *contexts = nullptr;
    void *expected_context = nullptr;
    unsigned checks = 0, context_violations = 0;
    bool deny = false;
    static int check(void *opaque, const char *host, uint16_t port, const struct sockaddr *address,
                     socklen_t, char *reason, size_t size) {
        ConnectPolicy &self = *static_cast<ConnectPolicy *>(opaque);
        ++self.checks;
        if (reason && size)
            reason[0] = '\0';
        if (self.contexts && self.contexts->active != self.expected_context) {
            ++self.context_violations;
            if (reason && size) std::strncpy(reason, "unexpected policy context", size - 1);
            return 0;
        }
        if (self.deny) {
            if (reason && size) std::strncpy(reason, "explicit policy denial", size - 1);
            return 0;
        }
        if (!host || (std::strcmp(host, "127.0.0.1") != 0 &&
                      !(self.allow_localhost && std::strcmp(host, "localhost") == 0)) ||
            port != self.port)
            return 0;
        if (address)
            ++self.calls;
        return 1;
    }
};

class PrepareCancel {
  public:
    PrepareCancel(uv_loop_t *loop, JSContext *ctx, Peer &peer, void *context_owner = nullptr)
        : loop_(loop), ctx_(ctx), peer_(peer), context_owner_(context_owner) {
    }
    // A completed connect may be in libuv's pending queue before prepare
    // callbacks. Establish and cancel at the real TCP barrier before the first
    // loop iteration can dispatch that queue. The source receiver stays paused;
    // poll must confirm its actual socket has no queued request bytes. A zero
    // thread-local received count alone would not prove this precondition.
    void before_first_run() {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (!peer_.accepted() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        bytes_before_cancel_ = peer_.received();
        try {
            require(peer_.accepted(), "TCP accept before cancellation");
            require(peer_.read_queue_empty(), "actual TCP receive queue must be empty before cancellation");
            require(bytes_before_cancel_ == 0,
                    "HTTP handshake must still be pending");
            struct Search {
                explicit Search(unsigned target_port) : port(target_port) {}
                unsigned port;
                unsigned matches = 0;
                lws_sockfd_type socket = LWS_SOCK_INVALID;
            } search{peer_.port()};
            uv_walk(
                loop_,
                [](uv_handle_t *candidate, void *opaque) {
                    auto &found = *static_cast<Search *>(opaque);
                    if (candidate->type != UV_POLL || uv_is_closing(candidate))
                        return;
                    uv_os_fd_t raw;
                    if (uv_fileno(candidate, &raw) != 0)
                        return;
#if defined(_WIN32)
                    const lws_sockfd_type fd =
                        static_cast<lws_sockfd_type>(reinterpret_cast<uintptr_t>(raw));
#else
                    const lws_sockfd_type fd = raw;
#endif
                    struct sockaddr_in remote = {};
                    socklen_t length = sizeof(remote);
                    if (getpeername(fd, reinterpret_cast<struct sockaddr *>(&remote),
                                    &length) == 0 &&
                        remote.sin_family == AF_INET &&
                        ntohs(remote.sin_port) == found.port &&
                        remote.sin_addr.s_addr == htonl(INADDR_LOOPBACK)) {
                        found.socket = fd;
                        ++found.matches;
                    }
                },
                &search);
            require(search.matches == 1, "one actual native client poll socket");
            if (context_owner_) {
                tjs_cancel_context_httpclients(ctx_, ContextLog::matches,
                                               context_owner_);
                tjs_cancel_context_httpclients(ctx_, ContextLog::matches,
                                               context_owner_);
            } else {
                evaluate(ctx_, "completeTrace.push('before'); client.abort(); client.abort(); completeTrace.push('after');");
            }
            cancelled_ = true;
            for (int step = 0; step < 2; ++step) {
#if defined(_WIN32)
                WSAPOLLFD ready = {};
                ready.fd = search.socket;
                ready.events = POLLWRNORM;
                const int result = WSAPoll(&ready, 1, 0);
                const bool writable = result > 0 && (ready.revents & POLLWRNORM) != 0;
#else
                struct pollfd ready = {};
                ready.fd = search.socket;
                ready.events = POLLOUT;
                const int result = poll(&ready, 1, 0);
                const bool writable = result > 0 && (ready.revents & POLLOUT) != 0;
#endif
                if (!writable)
                    break;
                struct lws_pollfd progress = {};
                progress.fd = search.socket;
                progress.events = LWS_POLLOUT;
                progress.revents = LWS_POLLOUT;
                require(lws_service_fd(tjs__lws_get_context(ctx_), &progress) ==
                                0 &&
                            progress.revents == 0,
                        "lws must consume the actual writable native socket");
                ++writable_services_;
            }
        } catch (const std::exception &error) {
            std::cerr << "prepare cancellation: " << error.what() << std::endl;
            failed_ = true;
        }
    }
    bool cancelled() const { return cancelled_ && !failed_; }
    unsigned bytes_before_cancel() const { return bytes_before_cancel_; }
    unsigned writable_services() const { return writable_services_; }

  private:
    uv_loop_t *loop_;
    JSContext *ctx_;
    Peer &peer_;
    void *context_owner_;
    bool cancelled_ = false, failed_ = false;
    unsigned bytes_before_cancel_ = 0;
    unsigned writable_services_ = 0;
};
} // namespace

int main(int argc, char **argv) {
    try {
        const bool context_cancel = argc == 2 && std::string(argv[1]) == "--context-cancel";
        const bool dns_cancel = argc == 2 && std::string(argv[1]) == "--dns-context-cancel";
        const bool policy_async = argc == 2 && std::string(argv[1]) == "--policy-async-context";
        const bool policy_binding = argc == 2 && std::string(argv[1]) == "--policy-binding-context";
        const bool policy_unowned = argc == 2 && std::string(argv[1]) == "--policy-unowned-context";
        require(argc == 1 || context_cancel || dns_cancel || policy_async || policy_binding || policy_unowned,
                "supported test arguments");
        ContextLog contexts;
        TJS_Initialize(argc, argv);
        require(uv_os_setenv("no_proxy", "127.0.0.1,localhost") == 0 &&
                    uv_os_setenv("NO_PROXY", "127.0.0.1,localhost") == 0,
                "loopback proxy bypass");
        Peer peer(!dns_cancel && !policy_async && !policy_binding && !policy_unowned);
        TJSRunOptions options;
        TJS_DefaultOptions(&options);
        options.skip_run_main = true;
        options.bootstrap = bootstrap;
        std::unique_ptr<TJSRuntime, decltype(&TJS_FreeRuntime)> runtime(
            TJS_NewRuntimeOptions(&options), TJS_FreeRuntime);
        require(runtime != nullptr, "runtime creation");
        JSContext *ctx = TJS_GetJSContext(runtime.get());
        require(capsid_tjs_set_cookie_jar_path(runtime.get(), "") == 0,
                "cookie storage configuration");
        ConnectPolicy gate{peer.port()};
        require(capsid_tjs_set_egress_policy(runtime.get(), ConnectPolicy::check, &gate) == 0,
                "egress callback installation");
        // TJS_GetInternalCore returns a borrowed reference.
        JSValue core = TJS_GetInternalCore(runtime.get());
        JSValue ctor = JS_GetPropertyStr(ctx, core, "HttpClient");
        JSValue global = JS_GetGlobalObject(ctx);
        JS_SetPropertyStr(ctx, global, "NativeHttpClient", ctor);
        JS_FreeValue(ctx, global);
        if (policy_async || policy_binding || policy_unowned) {
            // Independent hooks deliberately give a Binding resource owner A
            // and async callback owner B. The policy must prefer A; callbacks
            // retain B. Without a Binding owner, policy must use B. A client
            // captured outside any context must preserve its current caller.
            const TJSAsyncContextHooks hooks{ContextLog::capture, ContextLog::enter,
                                             ContextLog::leave, ContextLog::release};
            tjs_set_async_context_hooks(ctx, &hooks, &contexts);
            if (policy_binding) {
                const TJSResourceOwnerHooks owners{ContextLog::capture_native,
                    ContextLog::authorize_native, ContextLog::release_native};
                tjs_set_resource_owner_hooks(ctx, &owners, &contexts);
            }
            observed_contexts = &contexts;
            global = JS_GetGlobalObject(ctx);
            JS_SetPropertyStr(ctx, global, "currentContext", JS_NewCFunction(ctx, current_context, "currentContext", 0));
            JS_FreeValue(ctx, global);
            gate.contexts = &contexts; gate.allow_localhost = true;
            gate.expected_context = policy_binding || policy_unowned ? &contexts.owner_a : &contexts.owner_b;
            const std::string target = "http://localhost:" + std::to_string(peer.port()) + "/policy";
            contexts.active = policy_unowned ? nullptr : &contexts.owner_b;
            evaluate(ctx, "globalThis.denied = new NativeHttpClient();");
            contexts.active = &contexts.owner_a; gate.deny = true;
            evaluate(ctx, "let blocked = false; try {denied.open('GET', '" + target + "');}"
                          "catch (error) {blocked = error instanceof TypeError && error.message === 'explicit policy denial';}"
                          "if (!blocked) throw new Error('policy denial bypassed'); delete globalThis.denied;");
            require(contexts.active == &contexts.owner_a && gate.checks == 1 && gate.calls == 0 &&
                    gate.context_violations == 0 && !peer.accepted(),
                    "denied policy must restore the caller without connecting");
            JS_RunGC(JS_GetRuntime(ctx)); gate.deny = false;
            contexts.active = policy_unowned ? nullptr : &contexts.owner_b;
            evaluate(ctx, "globalThis.client = new NativeHttpClient(); globalThis.policyCompletions = [];");
            contexts.active = &contexts.owner_a;
            evaluate(ctx, "client.oncomplete = error => policyCompletions.push([error, currentContext()]);"
                          "client.open('GET', '" + target + "');");
            require(contexts.active == &contexts.owner_a, "successful open must restore its synchronous caller");
            require(gate.checks == 2 && gate.calls == 0 && gate.context_violations == 0,
                    "hostname policy check must precede asynchronous DNS and address checks");
            contexts.active = nullptr;
            if (policy_unowned) gate.expected_context = nullptr;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            while ((!peer.accepted() || peer.received() == 0) && std::chrono::steady_clock::now() < deadline) {
                uv_run(TJS_GetLoop(runtime.get()), UV_RUN_NOWAIT);
                tjs__execute_jobs(ctx);
                require(contexts.active == nullptr, "asynchronous policy must restore the idle caller");
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            require(peer.accepted() && peer.received() > 0 && gate.calls > 0 && gate.context_violations == 0,
                    "captured policy context must allow a real request after asynchronous DNS");
            if (policy_binding) {
                contexts.active = &contexts.owner_b;
                evaluate(ctx, "let deniedAccess = false; try {void client.timeout;}"
                              "catch (error) {deniedAccess = error.message === 'CAPSID_BINDING_OWNER_MISMATCH';}"
                              "if (!deniedAccess) throw new Error('foreign Binding access accepted');");
                require(contexts.native_denials == 1, "immutable Binding owner must still reject a foreign caller");
            }
            contexts.active = &contexts.owner_a;
            evaluate(ctx, "if (policyCompletions.length) throw new Error('premature completion'); client.abort();"
                          "if (JSON.stringify(policyCompletions) !== "
                          "'[[\"ABORTED\",\"" + std::string(policy_unowned ? "a" : "b") + "\"]]')"
                          "throw new Error('wrong callback context: ' + JSON.stringify(policyCompletions));");
            require(contexts.active == &contexts.owner_a, "synchronous abort callback must restore its caller");
            contexts.active = nullptr;
            while (!peer.closed() && std::chrono::steady_clock::now() < deadline) {
                uv_run(TJS_GetLoop(runtime.get()), UV_RUN_NOWAIT);
                tjs__execute_jobs(ctx);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            require(peer.closed() && peer.error() == 0, "policy positive control must close the actual TCP peer");
            evaluate(ctx, "delete globalThis.client;"); JS_RunGC(JS_GetRuntime(ctx));
            require(contexts.captures_a == contexts.releases_a && contexts.captures_b == contexts.releases_b &&
                    contexts.native_captures == contexts.native_releases,
                    "policy contexts and immutable owners must balance after GC");
            require(policy_unowned ? contexts.captures_b == 0 : contexts.captures_b >= 2,
                    "positive and denied clients must use the intended independent captures");
            require(!policy_binding || contexts.native_captures == 2,
                    "both Binding clients must retain and release their immutable owner");
            std::cout << "PASS native policy owner, caller restoration, DNS, denial and callback context "
                      << argv[1] << std::endl;
            return 0;
        }
        if (dns_cancel) {
            const TJSAsyncContextHooks hooks{ContextLog::capture, ContextLog::enter,
                                             ContextLog::leave, ContextLog::release};
            tjs_set_async_context_hooks(ctx, &hooks, &contexts);
            gate.allow_localhost = true;
            contexts.active = &contexts.owner_a;
            evaluate(ctx, "globalThis.cancelledCompletions = []; (() => {const value = new NativeHttpClient();"
                          "value.oncomplete = error => cancelledCompletions.push(error); value.open('GET',"
                          "'http://localhost:" + std::to_string(peer.port()) + "/cancelled');})();");
            require(contexts.active == &contexts.owner_a, "DNS open must preserve owner A before cancellation");
            // No libuv iteration has run: the asynchronous DNS result cannot
            // have reached the connect path. Collect the abandoned JS wrapper
            // while native DNS still owns its prevent-GC reference.
            tjs_cancel_context_httpclients(ctx, ContextLog::matches, &contexts.owner_a);
            tjs_cancel_context_httpclients(ctx, ContextLog::matches, &contexts.owner_a);
            JS_RunGC(JS_GetRuntime(ctx));
            require(contexts.captures_a == 1 && contexts.releases_a == 1,
                    "DNS cancellation must release one async context without JS delivery");
            contexts.active = &contexts.owner_b;
            evaluate(ctx, "globalThis.liveCompletions = []; globalThis.clientB = new NativeHttpClient();"
                          "clientB.oncomplete = error => liveCompletions.push(error); clientB.open('GET',"
                          "'http://127.0.0.1:" + std::to_string(peer.port()) + "/live');");
            require(contexts.active == &contexts.owner_b, "foreign open must preserve owner B");
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            while ((!peer.accepted() || peer.received() == 0) && std::chrono::steady_clock::now() < deadline) {
                uv_run(TJS_GetLoop(runtime.get()), UV_RUN_NOWAIT);
                tjs__execute_jobs(ctx);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            require(peer.accepted() && peer.received() > 0 && gate.calls == 1,
                    "another owner must send a real HTTP request after DNS cancellation");
            evaluate(ctx, "if (cancelledCompletions.length || liveCompletions.length) throw new Error('premature completion');"
                          "clientB.abort();");
            while (!peer.closed() && std::chrono::steady_clock::now() < deadline) {
                uv_run(TJS_GetLoop(runtime.get()), UV_RUN_NOWAIT);
                tjs__execute_jobs(ctx);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            evaluate(ctx, "if (cancelledCompletions.length || liveCompletions.length !== 1 || liveCompletions[0] !== 'ABORTED')"
                          "throw new Error('DNS owner cancellation or foreign completion'); delete globalThis.clientB;");
            JS_RunGC(JS_GetRuntime(ctx));
            require(peer.closed() && peer.error() == 0 && gate.calls == 1,
                    "cancelled DNS must never connect, while the foreign native peer closes normally");
            require(contexts.captures_a == contexts.releases_a && contexts.captures_b == contexts.releases_b,
                    "DNS and live callback captures must balance after GC");
            std::cout << "PASS owner cancellation before asynchronous DNS completion with live foreign I/O" << std::endl;
            return 0;
        }
        if (context_cancel) {
            const TJSAsyncContextHooks hooks{ContextLog::capture, ContextLog::enter,
                                             ContextLog::leave, ContextLog::release};
            tjs_set_async_context_hooks(ctx, &hooks, &contexts);
            contexts.active = &contexts.owner_a;
            evaluate(ctx, "new NativeHttpClient(); globalThis.retained = new NativeHttpClient();");
            JS_RunGC(JS_GetRuntime(ctx));
            require(contexts.captures_a == 2 && contexts.releases_a == 1,
                    "abandoned native client must unregister and release its owner");
            contexts.active = &contexts.owner_b;
            evaluate(ctx, "globalThis.protectedClient = new NativeHttpClient(); protectedClient.timeout = 42;");
            contexts.active = &contexts.owner_a;
        }
        PrepareCancel cancellation(TJS_GetLoop(runtime.get()), ctx, peer,
                                    context_cancel ? &contexts.owner_a : nullptr);
        evaluate(ctx, "globalThis.client = new NativeHttpClient(); globalThis.completions = []; globalThis.completeTrace = []; "
                      "client.oncomplete = error => {completions.push(error); Promise.resolve().then(() => completeTrace.push('microtask'));}; client.open('GET', "
                      "'http://127.0.0.1:" +
                          std::to_string(peer.port()) + "/hang');");
        if (context_cancel) require(contexts.active == &contexts.owner_a, "native open must preserve its caller");
        cancellation.before_first_run();
        peer.release_receive();
        // Cancellation occurred on the outer JS turn. Drain its jobs here
        // even if the socket already closed before the first loop iteration;
        // the trace still detects any checkpoint nested inside abort().
        tjs__execute_jobs(ctx);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (!peer.closed() && std::chrono::steady_clock::now() < deadline) {
            uv_run(TJS_GetLoop(runtime.get()), UV_RUN_NOWAIT);
            tjs__execute_jobs(ctx);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        std::cout << "OBSERVED calls=" << gate.calls << " cancelled=" << cancellation.cancelled()
                  << " writable=" << cancellation.writable_services()
                  << " before=" << cancellation.bytes_before_cancel()
                  << " accepted=" << peer.accepted() << " closed=" << peer.closed()
                  << " received=" << peer.received() << " errno=" << peer.error() << std::endl;
        if (context_cancel) {
            evaluate(ctx, "if (completions.length !== 0) throw new Error('owner cancellation ran application callbacks');"
                          "if (protectedClient.timeout !== 42) throw new Error('foreign client was changed');"
                          "let blocked = false; try { retained.open('GET', 'http://127.0.0.1:1/'); }"
                          "catch (error) { blocked = error.message === 'HttpClient context has ended'; }"
                          "if (!blocked) throw new Error('cancelled constructed client reopened');");
            require(contexts.captures_a == 3 && contexts.releases_a == 3,
                    "repeated owner cancellation must release each captured owner exactly once");
            require(contexts.captures_b == 1 && contexts.releases_b == 0,
                    "another owner must retain its unfinished client");
            evaluate(ctx, "delete globalThis.protectedClient; delete globalThis.retained; delete globalThis.client;");
            JS_RunGC(JS_GetRuntime(ctx));
            require(contexts.releases_b == 1, "foreign abandoned client must still finalize normally");
        } else {
            evaluate(ctx, "if (completions.length !== 1 || completions[0] !== 'ABORTED') throw new "
                          "Error('expected one ABORTED completion: ' + completions);"
                          "if (completeTrace.join(',') !== 'before,after,microtask') throw new Error('native abort ran nested microtasks: ' + completeTrace);");
        }
        require(gate.calls == 1, "one actual address check");
        require(cancellation.cancelled() && cancellation.bytes_before_cancel() == 0,
                "cancel before HTTP bytes barrier");
        require(cancellation.writable_services() > 0,
                "actual pending writable event before timer dispatch");
        require(peer.accepted(), "actual loopback TCP connection must be accepted");
        require(peer.error() == 0, "peer receive observation must remain active");
        require(peer.closed(), "abort before pending handshake did not close actual TCP peer");
        std::cout << "PASS native " << (context_cancel ? "owner cancellation" : "abort")
                  << " before pending handshake" << std::endl;
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << std::endl;
        return 1;
    }
}
