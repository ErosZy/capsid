#ifndef CAPSID_HOST_WEBSOCKET_SESSION_H
#define CAPSID_HOST_WEBSOCKET_SESSION_H
#include "host/channel_ledger.h"
#include "host/worker_executor.h"
#include "host/ws_stream.h"
#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <array>
#include <functional>

namespace capsid::host {
class WebSocketSession : public std::enable_shared_from_this<WebSocketSession> {
public:
    using Sender = std::function<bool(Command)>;
    using Completion = std::function<void(boost::beast::error_code, size_t)>;
    // The pump owns the same bounded codec/queues for accepted TCP,
    // outbound TCP and verified TLS streams. Callbacks retain the stream.
    struct Network {
        boost::asio::any_io_executor executor;
        std::function<void(boost::asio::mutable_buffer, Completion)> read;
        std::function<void(boost::asio::const_buffer, Completion)> write;
        std::function<void()> close;
    };
    WebSocketSession(boost::beast::tcp_stream &stream, std::shared_ptr<void> anchor,
        std::shared_ptr<ChannelLease> lease, uint32_t id, const capsid::protocol::ChannelLimits &limits,
        uint64_t write_timeout_ms, Sender sender,
        std::function<void()> done, std::shared_ptr<ServiceChannelBudget> budget = nullptr);
    WebSocketSession(std::shared_ptr<Network> network, std::shared_ptr<void> anchor,
        uint32_t id, const capsid::protocol::ChannelLimits &limits,
        uint64_t write_timeout_ms, Sender sender, std::function<void()> done,
        std::shared_ptr<ServiceChannelBudget> budget = nullptr);
    void start(std::string handshake, std::vector<uint8_t> leftovers);
    void start_client(std::vector<uint8_t> leftovers);
    void on_event(WorkerEvent event);
    // Send Going Away and allow a bounded peer Close reply before teardown.
    void shutdown();
    void abort(bool notify_worker = true);
private:
    struct Write { std::shared_ptr<std::vector<uint8_t>> bytes; std::shared_ptr<void> charge; uint64_t credit = 0; bool close = false, handshake = false, pong = false; };
    bool submit(Command command);
    void pump_input();
    void read();
    void write();
    bool queue_frame(ws::Frame frame, uint64_t credit = 0, bool pong = false);
    void release_credit();
    void protocol_failure(uint16_t code);
    void arm_close_timer(uint32_t maximum_ms = 0);
    void touch_idle();
    void finish();
    std::shared_ptr<Network> network_;
    ws::Role role_ = ws::kServer;
    std::shared_ptr<void> anchor_;
    std::shared_ptr<ChannelLease> lease_;
    std::shared_ptr<ServiceChannelBudget> budget_ = default_channel_budget();
    uint32_t id_;
    uint64_t credit_ = 65536;
    uint32_t close_timeout_ms_;
    uint64_t write_timeout_ms_;
    uint64_t queue_limit_;
    uint32_t idle_timeout_ms_;
    Sender sender_;
    std::function<void()> done_;
    ws::StreamDecoder decoder_;
    std::array<uint8_t, 16384> read_bytes_{};
    std::vector<uint8_t> input_;
    size_t input_offset_ = 0;
    std::optional<ws::StreamDecoder::Item> pending_input_;
    // Outbound channel fragments are IPC chunking, not WebSocket
    // framing: reassemble per message so the public wire carries one
    // frame per send() exactly like workerd.
    std::vector<uint8_t> pending_message_;
    size_t pending_credit_ = 0;
    bool pending_text_ = false;
    // Slow-consumer backpressure: credit returning from completed
    // socket writes is withheld while the write queue is deep, then
    // released once it drains below half the limit.
    uint64_t withheld_credit_ = 0;
    std::deque<Write> writes_;
    size_t queued_write_bytes_ = 0;
    bool reading_ = false, writing_ = false, upgraded_ = false, stopped_ = false;
    bool host_closed_ = false, peer_closed_ = false, worker_closed_ = false;
    bool shutting_down_ = false;
    bool close_written_ = false, fail_after_write_ = false;
    bool confirm_transport_after_write_ = false;
    boost::asio::steady_timer close_timer_, write_timer_, retry_timer_, idle_timer_;
};
// Sanitized response fields only. Transport-owned fields cannot be chosen
// by the application. Subprotocol selection must match an offered token.
bool websocket_handshake(const std::string &key, const std::string &offered,
    const std::vector<std::pair<std::string,std::string>> &headers, std::string *out);
}
#endif
