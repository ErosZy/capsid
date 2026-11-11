// Real native HTTP headers survive a valid positive short socket write.
// GNU ld wrapping is confined to this test executable. The forced prefix is
// actually sent to the loopback peer; no product callbacks or data are replaced.
extern "C" {
#include "tjs.h"
#include "utils.h"
#include <uv.h>
void tjs__execute_jobs(JSContext *ctx);
int capsid_tjs_set_cookie_jar_path(TJSRuntime *, const char *);
int capsid_tjs_set_egress_policy(TJSRuntime *,
    int (*)(void *, const char *, uint16_t, const struct sockaddr *, socklen_t, char *, size_t), void *);
}
#include "win32_compat.h"
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
bool force_short = false;
unsigned write_calls = 0, forced_calls = 0;
std::size_t requested = 0;
int prefix_sent = 0;
void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}
void evaluate(JSContext *ctx, const std::string &source) {
    JSValue result = JS_Eval(ctx, source.c_str(), source.size(), "short-write.js", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(result)) {
        JSValue error = JS_GetException(ctx);
        const char *message = JS_ToCString(ctx, error);
        const std::string detail = message ? message : "unprintable";
        JS_FreeCString(ctx, message); JS_FreeValue(ctx, error);
        throw std::runtime_error("JavaScript: " + detail);
    }
    JS_FreeValue(ctx, result);
}
bool completed(JSContext *ctx) {
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue value = JS_GetPropertyStr(ctx, global, "complete");
    const bool result = JS_ToBool(ctx, value) != 0;
    JS_FreeValue(ctx, value); JS_FreeValue(ctx, global); return result;
}
JSValue noop(JSContext *, JSValueConst, int, JSValueConst *) { return JS_UNDEFINED; }
int bootstrap(TJSRuntime *, JSContext *ctx, void *) {
    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "dispatchEvent", JS_NewCFunction(ctx, noop, "dispatchEvent", 1));
    JS_SetPropertyStr(ctx, global, "PromiseRejectionEvent",
        JS_NewCFunction2(ctx, noop, "PromiseRejectionEvent", 0, JS_CFUNC_constructor, 0));
    JS_FreeValue(ctx, global); return 0;
}
class Peer {
  public:
    explicit Peer(std::size_t target_bytes) {
        listener_ = socket(AF_INET, SOCK_STREAM, 0); require(listener_ >= 0, "owned listener");
        sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        try {
            require(bind(listener_, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0, "bind actual ephemeral listener");
            require(listen(listener_, 1) == 0, "listen"); socklen_t size = sizeof(address);
            require(getsockname(listener_, reinterpret_cast<sockaddr *>(&address), &size) == 0, "actual bound port");
            port_ = ntohs(address.sin_port); target_ = "/" + std::string(target_bytes - 1, 'x');
            body_ = "native-short-write-" + std::to_string(port_) + "-" + std::to_string(target_bytes);
            thread_ = std::thread([this] { receive(); });
        } catch (...) {close(listener_); throw;}
    }
    ~Peer() {
        stop_ = true; const int client = client_.load();
        if (client >= 0) shutdown(client, SHUT_RDWR);
        shutdown(listener_, SHUT_RDWR); if (thread_.joinable()) thread_.join();
        const int final_client = client_.load(); if (final_client >= 0) close(final_client); close(listener_);
    }
    unsigned port() const { return port_; }
    const std::string &target() const { return target_; }
    const std::string &body() const { return body_; }
    bool closed() const { return closed_.load(); }
    void verify() const {
        require(closed_ && error_ == 0, "peer FIN and error-free actual source");
        const auto end = wire_.find("\r\n");
        require(end != std::string::npos && wire_.substr(0, end) == "GET " + target_ + " HTTP/1.1", "every actual request-target byte arrives");
        require(wire_.size() >= 4 && wire_.compare(wire_.size() - 4, 4, "\r\n\r\n") == 0 &&
                    wire_.find("\r\n\r\n") == wire_.size() - 4,
                "one complete request head, no duplicated tail or body");
        require(response_sent_ == body_.size(), "entire independent challenge body sent");
    }
  private:
    void receive() {
        capsid_pollfd descriptor{}; descriptor.fd = listener_; descriptor.events = POLLIN;
        if (capsid::win32::capsid_poll(&descriptor, 1, 3000) <= 0) {error_ = 1; return;}
        const int client = accept(listener_, nullptr, nullptr); client_ = client;
        if (client < 0) {error_ = 2; return;}
        timeval timeout{0, 100000};
        if (setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0) {error_ = 3; return;}
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        bool replied = false;
        while (!stop_ && std::chrono::steady_clock::now() < deadline) {
            char buffer[1024]; const auto count = recv(client, buffer, sizeof(buffer), 0);
            if (count == 0) {closed_ = true; return;}
            if (count < 0) {
                if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
                error_ = 4; return;
            }
            wire_.append(buffer, static_cast<std::size_t>(count));
            if (wire_.size() > 32768) {error_ = 5; return;}
            if (!replied && wire_.find("\r\n\r\n") != std::string::npos) {
                replied = true;
                const std::string response = "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(body_.size()) +
                    "\r\nConnection: close\r\n\r\n" + body_;
                std::size_t sent = 0;
                while (sent < response.size()) {
                    const auto result = send(client, response.data() + sent, response.size() - sent, MSG_NOSIGNAL);
                    if (result < 0 && errno == EINTR) continue;
                    if (result <= 0) {error_ = 6; return;}
                    sent += static_cast<std::size_t>(result);
                }
                response_sent_ = body_.size(); shutdown(client, SHUT_WR);
            }
        }
    }
    int listener_ = -1;
    unsigned port_ = 0;
    std::atomic<int> client_{-1}, error_{0};
    std::atomic<bool> stop_{false}, closed_{false};
    std::atomic<std::size_t> response_sent_{0};
    std::string target_, body_, wire_;
    std::thread thread_;
};
int allow_peer(void *opaque, const char *host, uint16_t port, const sockaddr *, socklen_t, char *reason, size_t size) {
    if (reason && size) reason[0] = '\0';
    return host && std::strcmp(host, "127.0.0.1") == 0 && port == static_cast<Peer *>(opaque)->port();
}
}

struct lws;
extern "C" int __real_lws_ssl_capable_write(struct lws *, unsigned char *, size_t);
extern "C" int __wrap_lws_ssl_capable_write(struct lws *wsi, unsigned char *data, size_t size) {
    ++write_calls;
    if (force_short && !forced_calls && size > 4 && std::memcmp(data, "GET ", 4) == 0) {
        ++forced_calls; requested = size;
        prefix_sent = __real_lws_ssl_capable_write(wsi, data, size / 2);
        return prefix_sent;
    }
    return __real_lws_ssl_capable_write(wsi, data, size);
}

int main(int argc, char **argv) {
    try {
        require(argc == 3 && (std::string(argv[1]) == "normal" || std::string(argv[1]) == "short"), "mode and target bytes");
        force_short = std::string(argv[1]) == "short";
        const auto bytes = std::stoul(argv[2]); require(bytes == 64 || bytes == 8192, "reviewed request-target boundary");
        TJS_Initialize(argc, argv);
        require(uv_os_setenv("no_proxy", "127.0.0.1") == 0 && uv_os_setenv("NO_PROXY", "127.0.0.1") == 0, "local proxy bypass");
        Peer peer(bytes); TJSRunOptions options; TJS_DefaultOptions(&options); options.skip_run_main = true; options.bootstrap = bootstrap;
        std::unique_ptr<TJSRuntime, decltype(&TJS_FreeRuntime)> runtime(TJS_NewRuntimeOptions(&options), TJS_FreeRuntime);
        require(runtime != nullptr, "actual native runtime"); JSContext *ctx = TJS_GetJSContext(runtime.get());
        require(capsid_tjs_set_cookie_jar_path(runtime.get(), "") == 0 && capsid_tjs_set_egress_policy(runtime.get(), allow_peer, &peer) == 0, "native loopback policy");
        JSValue global = JS_GetGlobalObject(ctx); JSValue core = TJS_GetInternalCore(runtime.get());
        JS_SetPropertyStr(ctx, global, "NativeHttpClient", JS_GetPropertyStr(ctx, core, "HttpClient")); JS_FreeValue(ctx, global);
        evaluate(ctx, "globalThis.complete=false;globalThis.calls=0;globalThis.status=0;globalThis.body='';globalThis.failure=null;"
            "globalThis.client=new NativeHttpClient();client.onstatus=value=>status=value;client.onheadersend=()=>client.sendData(null);"
            "client.ondata=bytes=>{for(const value of new Uint8Array(bytes))body+=String.fromCharCode(value);};"
            "client.oncomplete=error=>{calls++;failure=error;complete=true;};client.open('GET','http://127.0.0.1:" + std::to_string(peer.port()) + peer.target() + "');");
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while ((!completed(ctx) || !peer.closed()) && std::chrono::steady_clock::now() < deadline) {
            uv_run(TJS_GetLoop(runtime.get()), UV_RUN_NOWAIT); tjs__execute_jobs(ctx);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        require(completed(ctx), "actual native response completes after socket prefix write");
        peer.verify();
        evaluate(ctx, "if(calls!==1||failure!==null||status!==200||body!=='" + peer.body() + "')throw new Error('native response and single completion');client=null;");
        require(write_calls > 0, "link control actually intercepted native socket writes");
        require(!force_short || (forced_calls == 1 && prefix_sent > 0 && static_cast<std::size_t>(prefix_sent) < requested), "actual socket accepts a strict positive prefix");
        JS_RunGC(JS_GetRuntime(ctx)); runtime.reset();
        std::cout << "PASS native HTTP " << (force_short ? "short" : "normal") << " target=" << bytes
                  << " requested=" << requested << " prefix=" << prefix_sent << std::endl;
        return 0;
    } catch (const std::exception &error) {std::cerr << "FAIL: " << error.what() << std::endl; return 1;}
}
