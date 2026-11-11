#ifndef CAPSID_TEST_HOST_WS_PEER_H
#define CAPSID_TEST_HOST_WS_PEER_H
#include "host/ws_codec.h"
#include <boost/asio.hpp>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <string>
#include <thread>

namespace capsid_test {
namespace asio = boost::asio;
using tcp = asio::ip::tcp;
using Clock = std::chrono::steady_clock;
inline void require(bool value, const std::string &message) {
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", message.c_str()); std::exit(1); }
}
inline bool wait_until(const std::function<bool()> &predicate,
                       std::chrono::milliseconds timeout = std::chrono::seconds(3)) {
    const auto end = Clock::now() + timeout;
    do {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (Clock::now() < end);
    return predicate();
}
// An independent, deliberately passive TCP peer. It only produces a WS
// handshake when the test asks, and never automatically acknowledges Close.
struct Peer {
    asio::io_context io;
    tcp::acceptor acceptor{io, {asio::ip::address_v4::loopback(), 0}};
    std::shared_ptr<tcp::socket> socket;
    std::string head;
    Peer() { acceptor.non_blocking(true); }
    uint16_t port() const { return acceptor.local_endpoint().port(); }
    bool accept() {
        if (socket) return true;
        auto candidate = std::make_shared<tcp::socket>(io);
        boost::system::error_code error; acceptor.accept(*candidate, error);
        if (error == asio::error::would_block || error == asio::error::try_again) return false;
        require(!error, "peer accept: " + error.message());
        candidate->non_blocking(true); socket = std::move(candidate); return true;
    }
    bool read_head() {
        if (!accept()) return false;
        char bytes[4096]; boost::system::error_code error;
        const auto size = socket->read_some(asio::buffer(bytes), error);
        if (size) head.append(bytes, size);
        require(!error || error == asio::error::would_block || error == asio::error::try_again,
                "peer read handshake: " + error.message());
        return head.find("\r\n\r\n") != std::string::npos;
    }
    void upgrade() {
        require(wait_until([&] { return read_head(); }), "peer received handshake");
        const auto key_start = head.find("Sec-WebSocket-Key: ");
        require(key_start != std::string::npos, "handshake key present");
        const auto first = key_start + 19, end = head.find("\r\n", first);
        const auto accept_key = capsid::host::ws::compute_accept_key(head.substr(first, end-first));
        const std::string response = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " + accept_key + "\r\n\r\n";
        boost::system::error_code error; const auto size = asio::write(*socket, asio::buffer(response), error);
        require(!error && size == response.size(), "peer wrote complete handshake");
    }
    bool closed() {
        if (!socket) return false;
        char bytes[4096]; boost::system::error_code error;
        socket->read_some(asio::buffer(bytes), error);
        return error == asio::error::eof || error == asio::error::connection_reset;
    }
};
}
#endif
