#include "host/async_resolver.h"
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include "host_dns_provider.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <map>
#include <string_view>
#ifndef _WIN32
#include <poll.h>
#endif

namespace capsid::host {
namespace asio = boost::asio;
namespace {
bool reserved_invalid_name(std::string_view name) {
    if (!name.empty() && name.back() == '.') name.remove_suffix(1);
    constexpr std::string_view suffix = "invalid";
    if (name.size() < suffix.size()) return false;
    const auto offset = name.size() - suffix.size();
    if (offset && name[offset - 1] != '.') return false;
    for (size_t i = 0; i < suffix.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(name[offset + i]);
        if (c >= 'A' && c <= 'Z') c = static_cast<unsigned char>(c + ('a' - 'A'));
        if (c != static_cast<unsigned char>(suffix[i])) return false;
    }
    return true;
}
struct Library {
    int status = ares_library_init(ARES_LIB_INIT_ALL);
    ~Library() { if (status == ARES_SUCCESS) ares_library_cleanup(); }
};
std::shared_ptr<Library> library() {
    static auto instance = std::make_shared<Library>();
    return instance;
}
#ifdef _WIN32
using PollFd = WSAPOLLFD;
int poll_ready(std::vector<PollFd> &fds) {
    return WSAPoll(fds.data(), static_cast<ULONG>(fds.size()), 0);
}
#else
using PollFd = pollfd;
int poll_ready(std::vector<PollFd> &fds) { return ::poll(fds.data(), fds.size(), 0); }
#endif
}
struct AsyncResolver::Query : std::enable_shared_from_this<Query> {
    asio::io_context &io;
    asio::steady_timer timer;
    ares_channel channel = nullptr;
    Completion done;
    std::shared_ptr<Library> library_owner;
    std::map<ares_socket_t, short> sockets;
    bool canceled = false, completed = false;
    explicit Query(asio::io_context &context, Completion callback)
        : io(context), timer(context), done(std::move(callback)) {}
    ~Query() { cancel(); }
    static void socket_state(void *arg, ares_socket_t fd, int read, int write) {
        auto &self = *static_cast<Query *>(arg);
        if (read || write) self.sockets[fd] = (read ? POLLIN : 0) | (write ? POLLOUT : 0);
        else self.sockets.erase(fd);
    }
    void destroy_channel() {
        if (!channel) return;
        auto old = channel; channel = nullptr;
        ares_destroy(old); // synchronously frees DNS sockets and pending queries
        sockets.clear();
    }
    void cancel() {
        canceled = true; done = {};
        timer.cancel(); destroy_channel();
    }
    void complete(boost::system::error_code error, std::vector<asio::ip::tcp::endpoint> endpoints = {}) {
        if (canceled || completed) return;
        completed = true;
        // c-ares can complete inline, under its channel lock. Leave that
        // callback before destroying the channel or calling the consumer.
        asio::post(io, [self = shared_from_this(), error, endpoints = std::move(endpoints)]() mutable {
            self->timer.cancel(); self->destroy_channel();
            auto callback = std::move(self->done);
            if (!self->canceled && callback) callback(error, std::move(endpoints));
        });
    }
    static void resolved(void *arg, int status, int, ares_addrinfo *result) {
        auto &self = *static_cast<Query *>(arg);
        std::vector<asio::ip::tcp::endpoint> endpoints;
        boost::system::error_code error;
        if (status != ARES_SUCCESS) error = asio::error::host_not_found;
        if (!self.canceled && result && !error) {
            for (auto node = result->nodes; node; node = node->ai_next) {
                asio::ip::tcp::endpoint endpoint;
                const size_t size = node->ai_addrlen;
                if (!node->ai_addr || size > endpoint.capacity() ||
                    (node->ai_family != AF_INET && node->ai_family != AF_INET6)) continue;
                std::memcpy(endpoint.data(), node->ai_addr, size); endpoint.resize(size);
                if (endpoints.size() == 64) { error = asio::error::no_buffer_space; endpoints.clear(); break; }
                endpoints.push_back(endpoint);
            }
        }
        if (result) ares_freeaddrinfo(result);
        if (!error && endpoints.empty()) error = asio::error::host_not_found;
        self.complete(error, std::move(endpoints));
    }
    void start(const std::string &host, uint16_t port, const std::string &servers) {
        // RFC6761 section6.4: the invalid. zone is always nonexistent.
        // Do not depend on an external DNS reply or consume the dial deadline.
        // complete() still posts delivery, preserving cancellation ordering.
        if (reserved_invalid_name(host)) { complete(asio::error::host_not_found); return; }
        boost::system::error_code numeric_error;
        auto address = asio::ip::make_address(host, numeric_error);
        if (!numeric_error) { complete({}, {asio::ip::tcp::endpoint(address, port)}); return; }
        library_owner = library();
        if (library_owner->status != ARES_SUCCESS) { complete(asio::error::host_not_found); return; }
        ares_options options{};
        options.sock_state_cb = socket_state; options.sock_state_cb_data = this;
        // Disable HOSTALIASES; DNS configuration and hosts remain native
        // Host inputs. No event thread or background config reloader.
        options.flags = ARES_FLAG_NOALIASES;
        int status = ares_init_options(&channel, &options, ARES_OPT_SOCK_STATE_CB | ARES_OPT_FLAGS);
        if (status == ARES_SUCCESS && !servers.empty()) status = ares_set_servers_ports_csv(channel, servers.c_str());
        if (status != ARES_SUCCESS) { complete(asio::error::host_not_found); return; }
        ares_addrinfo_hints hints{};
        hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM; hints.ai_protocol = IPPROTO_TCP;
        // Sorting normally probes resolved addresses with connect(). That
        // would run before our egress decision, so it MUST be disabled.
        hints.ai_flags = ARES_AI_NUMERICSERV | ARES_AI_NOSORT;
        ares_getaddrinfo(channel, host.c_str(), std::to_string(port).c_str(), &hints, resolved, this);
        if (!completed) tick();
    }
    void tick() {
        if (canceled || completed) return;
        std::vector<PollFd> fds;
        for (const auto &[fd, events] : sockets) fds.push_back({fd, events, 0});
        if (!fds.empty()) {
            if (poll_ready(fds) < 0) { complete(asio::error::host_not_found); return; }
            for (const auto &fd : fds) {
                if (!fd.revents || !sockets.count(fd.fd)) continue;
                ares_process_fd(channel, fd.revents & (POLLIN | POLLERR | POLLHUP | POLLNVAL) ? fd.fd : ARES_SOCKET_BAD,
                                fd.revents & POLLOUT ? fd.fd : ARES_SOCKET_BAD);
                if (completed || canceled) return;
            }
        }
        ares_process_fd(channel, ARES_SOCKET_BAD, ARES_SOCKET_BAD);
        if (completed || canceled) return;
        // Nonblocking poll; one bounded wakeup per active dial. The dialer's
        // negotiated deadline owns the overall timeout, including DNS.
        timer.expires_after(std::chrono::milliseconds(10));
        timer.async_wait([self = shared_from_this()](boost::system::error_code error) { if (!error) self->tick(); });
    }
};
AsyncResolver::AsyncResolver(asio::io_context &io) : io_(io) {}
AsyncResolver::~AsyncResolver() { cancel(); }
void AsyncResolver::resolve(const std::string &host, uint16_t port, Completion done, const std::string &servers) {
    cancel(); query_ = std::make_shared<Query>(io_, std::move(done)); query_->start(host, port, servers);
}
void AsyncResolver::cancel() { if (query_) { query_->cancel(); query_.reset(); } }
}
