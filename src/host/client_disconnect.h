#ifndef CAPSID_HOST_CLIENT_DISCONNECT_H
#define CAPSID_HOST_CLIENT_DISCONNECT_H

#include <boost/asio/buffer.hpp>
#include <boost/asio/ip/tcp.hpp>

namespace capsid::host {
enum class ClientReadProbe { kRetry, kData, kEof, kError };

// A readiness notification may be stale after an async HTTP read consumes
// the bytes. Never block the listener loop or treat EAGAIN as a disconnect.
// Asio's user nonblocking mode also works on Windows without MSG_DONTWAIT.
inline ClientReadProbe probe_client_read(boost::asio::ip::tcp::socket& socket) {
    boost::system::error_code error;
    if (!socket.non_blocking()) socket.non_blocking(true, error);
    if (error) return ClientReadProbe::kError;
    char scratch = 0;
    socket.receive(boost::asio::buffer(&scratch, 1),
                   boost::asio::socket_base::message_peek, error);
    if (error == boost::asio::error::would_block ||
        error == boost::asio::error::try_again ||
        error == boost::asio::error::interrupted) return ClientReadProbe::kRetry;
    if (error == boost::asio::error::eof) return ClientReadProbe::kEof;
    return error ? ClientReadProbe::kError : ClientReadProbe::kData;
}
}  // namespace capsid::host
#endif
