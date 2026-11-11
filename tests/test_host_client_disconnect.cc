#include "host/client_disconnect.h"
#include <boost/asio/io_context.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/write.hpp>
#include <chrono>
#include <cstdio>
#include <cstdlib>

using capsid::host::ClientReadProbe;
using capsid::host::probe_client_read;
namespace asio = boost::asio;
using tcp = asio::ip::tcp;
void require(bool value, const char* message) {
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
void await_readable(asio::io_context& io, tcp::socket& socket) {
    // TCP write/shutdown completion does not imply peer readability yet.
    // Wait for an actual event; the probe itself must still return at once.
    asio::steady_timer deadline(io, std::chrono::seconds(2));
    bool ready = false;
    socket.async_wait(tcp::socket::wait_read, [&](boost::system::error_code error) {
        ready = !error;
        deadline.cancel();
    });
    deadline.async_wait([&](boost::system::error_code error) {
        if (!error) {
            boost::system::error_code ignored;
            socket.cancel(ignored);
        }
    });
    io.restart();
    io.run();
    require(ready, "bounded real TCP readability before data/EOF probe");
}
int main() {
    asio::io_context io;
    tcp::acceptor listener(io, {asio::ip::make_address("127.0.0.1"), 0});
    tcp::socket client(io), server(io);
    client.connect(listener.local_endpoint()); listener.accept(server);
    require(!server.non_blocking(), "accepted socket starts in user blocking mode");
    const auto began = std::chrono::steady_clock::now();
    require(probe_client_read(server) == ClientReadProbe::kRetry,
            "stale readiness/no data remains retryable");
    require(std::chrono::steady_clock::now() - began < std::chrono::milliseconds(100),
            "peek never blocks the listener loop");
    asio::write(client, asio::buffer("G", 1));
    await_readable(io, server);
    require(probe_client_read(server) == ClientReadProbe::kData,
            "available pipelined data is distinct from EAGAIN");
    char byte = 0;
    boost::system::error_code read_error;
    const auto read_bytes = asio::read(server, asio::buffer(&byte, 1), read_error);
    require(!read_error && read_bytes == 1 && byte == 'G',
            "probe never consumes HTTP parser bytes");
    require(probe_client_read(server) == ClientReadProbe::kRetry,
            "consumed readiness cannot be mistaken for disconnect");
    client.shutdown(tcp::socket::shutdown_send);
    await_readable(io, server);
    require(probe_client_read(server) == ClientReadProbe::kEof,
            "client send half-close is distinct from reset/error");
    asio::write(server, asio::buffer("ok", 2));
    char response[2]; asio::read(client, asio::buffer(response));
    require(response[0] == 'o' && response[1] == 'k',
            "half-closed client can still receive its response");
    server.close();
    require(probe_client_read(server) == ClientReadProbe::kError,
            "closed descriptor fails without attempting a blocking read");
    std::puts("client probe: stale readiness, nonblocking peek, data preservation and half-close PASS");
}
