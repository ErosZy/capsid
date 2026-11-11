#ifndef CAPSID_HOST_ASYNC_RESOLVER_H
#define CAPSID_HOST_ASYNC_RESOLVER_H
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace capsid::host {
// DNS sockets and timers belong to the caller's event loop. Cancellation
// destroys the query, including its sockets; it never leaves a blocking
// getaddrinfo task behind. Call every method on that event loop's thread.
class AsyncResolver {
public:
    using Completion = std::function<void(boost::system::error_code,
        std::vector<boost::asio::ip::tcp::endpoint>)>;
    explicit AsyncResolver(boost::asio::io_context &io);
    ~AsyncResolver();
    AsyncResolver(const AsyncResolver &) = delete;
    AsyncResolver &operator=(const AsyncResolver &) = delete;
    // Empty servers means system DNS configuration and the system hosts
    // file. A native owner may override DNS servers (host[:port], CSV).
    // This option is never supplied by worker IPC or a JavaScript URL.
    void resolve(const std::string &host, uint16_t port, Completion done,
                 const std::string &servers = {});
    void cancel(); // suppresses even an already queued completion
private:
    struct Query;
    boost::asio::io_context &io_;
    std::shared_ptr<Query> query_;
};
}
#endif
