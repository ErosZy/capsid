#include "host/websocket_session.h"
#include <cstdio>
#include <cstdlib>
#include <deque>

namespace {
namespace asio = boost::asio;
using capsid::host::Command;
using capsid::host::CommandType;
using capsid::host::WebSocketSession;
using capsid::host::WorkerEvent;
void require(bool ok, const char *message) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
struct Pump {
    asio::io_context io;
    std::shared_ptr<WebSocketSession::Network> network = std::make_shared<WebSocketSession::Network>();
    std::shared_ptr<capsid::host::ServiceChannelBudget> budget = std::make_shared<capsid::host::ServiceChannelBudget>();
    std::shared_ptr<WebSocketSession> session;
    WebSocketSession::Completion read_done, write_done;
    std::vector<std::vector<uint8_t>> wire;
    std::vector<Command> commands;
    unsigned closes = 0, completions = 0, confirms = 0;
    explicit Pump(uint64_t write_timeout = 1000) {
        network->executor = io.get_executor();
        network->read = [this](asio::mutable_buffer, WebSocketSession::Completion done) {
            require(!read_done, "at most one read in flight"); read_done = std::move(done);
        };
        network->write = [this](asio::const_buffer buffer, WebSocketSession::Completion done) {
            require(!write_done && closes == 0, "no overlapping or post-close write");
            const auto *bytes = static_cast<const uint8_t *>(buffer.data());
            wire.emplace_back(bytes, bytes + buffer.size()); write_done = std::move(done);
        };
        network->close = [this] { ++closes; };
        capsid::protocol::ChannelLimits limits;
        limits.close_timeout_ms = 100;
        session = std::make_shared<WebSocketSession>(network, nullptr, 1, limits, write_timeout,
            [this](Command command) {
                if (command.type == CommandType::kChannelConfirm) {
                    require(closes == 1, "terminal ACK follows actual socket teardown");
                    require(command.channel_id == 1 && command.channel_flags == 2,
                            "final Close confirms transport termination exactly");
                    ++confirms;
                }
                commands.push_back(std::move(command)); return true;
            }, [this] { ++completions; }, budget);
        session->start_client({});
    }
    void data(const std::string &text = "a-before") {
        WorkerEvent event; event.type = WorkerEvent::Type::kChannelData;
        event.channel_flags = CAPSID_CHANNEL_FLAG_START | CAPSID_CHANNEL_FLAG_END | CAPSID_CHANNEL_FLAG_TEXT;
        event.body.assign(text.begin(), text.end());
        session->on_event(std::move(event));
    }
    void close(unsigned kind = 3, uint16_t code = 1000) {
        WorkerEvent event; event.type = WorkerEvent::Type::kChannelClose;
        event.close_kind = kind; event.close_code = code;
        if (code) event.text = "done";
        session->on_event(std::move(event));
    }
    void wrote(boost::system::error_code error = {}) {
        require(bool(write_done), "a real write is pending");
        auto done = std::move(write_done); write_done = {};
        done(error, error ? 0 : wire.back().size());
    }
    void terminal() {
        require(closes == 1 && completions == 1 && confirms == 1,
                "one socket close, one completion and one transport ACK");
        require(budget->queued_bytes() == 0, "finished write queue releases service reservation");
        close();
        if (read_done) { auto done = std::move(read_done); done(asio::error::operation_aborted, 0); }
        io.restart(); io.poll();
        require(closes == 1 && completions == 1 && confirms == 1,
                "late events and canceled read/timers cannot repeat termination");
    }
};
void flush_then_finish() {
    Pump pump; pump.data(); pump.close();
    require(pump.wire.size() == 1 && pump.closes == 0 && pump.confirms == 0,
            "accepted Close waits for the earlier data write");
    pump.wrote();
    require(pump.wire.size() == 2 && pump.closes == 0 && pump.confirms == 0,
            "transport stays alive until the accepted Close write completes");
    pump.wrote(); pump.terminal();
    capsid::host::ws::StreamDecoder decoder(1024, capsid::host::ws::kServer);
    for (size_t i = 0; i < pump.wire.size(); ++i) {
        capsid::host::ws::StreamDecoder::Item item; size_t used = 0;
        require(decoder.consume(pump.wire[i].data(), pump.wire[i].size(), &used, 65536, &item) ==
                capsid::host::ws::StreamDecoder::ItemReady && used == pump.wire[i].size(),
                "the completed masked client write is an entire valid frame");
        if (i == 0) require(item.opcode == capsid::host::ws::kText &&
                item.payload == std::vector<uint8_t>({'a','-','b','e','f','o','r','e'}), "accepted text preserved");
        else require(item.opcode == capsid::host::ws::kClose && item.code == 1000 &&
                item.payload == std::vector<uint8_t>({3,232,'d','o','n','e'}), "accepted Close code/reason preserved");
    }
}
void empty_close() {
    Pump pump; pump.close(3, 0); pump.wrote(); pump.terminal();
    require((pump.wire[0][1] & 127) == 0, "final Close without a code stays empty on the wire");
}
void write_failure(bool close_write) {
    Pump pump; pump.data(); pump.close();
    if (close_write) pump.wrote();
    pump.wrote(asio::error::connection_reset); pump.terminal();
}
void write_timeout() {
    Pump pump(10); pump.data(); pump.close();
    pump.io.run_for(std::chrono::milliseconds(40));
    // The in-flight callback owns its buffer until cancellation completes.
    pump.wrote(asio::error::operation_aborted); pump.terminal();
}
void ordinary_close_still_waits() {
    Pump pump; pump.close(0); pump.wrote(); pump.io.poll();
    require(pump.closes == 0 && pump.confirms == 0 && pump.completions == 0,
            "ordinary Close retains the peer handshake");
    pump.session->abort(false);
    require(pump.closes == 1 && pump.confirms == 0 && pump.completions == 1,
            "ordinary teardown does not invent a terminal ACK");
}
void transport_close_drains_accepted_text() {
    Pump pump; pump.data("queued"); pump.data("late"); pump.close(2,1006);
    require(pump.closes == 0 && pump.confirms == 0 && pump.wire.size() == 1,
            "transport terminal retains in-flight and queued accepted text before teardown");
    pump.wrote();
    require(pump.closes == 0 && pump.confirms == 0 && pump.wire.size() == 2,
            "accepted late text starts before transport teardown");
    pump.wrote(); pump.terminal();
    require((pump.wire[0][0] & 15) == 1 && (pump.wire[1][0] & 15) == 1,
            "abnormal transport termination adds no WebSocket Close frame");
}
void transport_close_empty_and_failed_write() {
    Pump empty; empty.close(2,1006); empty.terminal();
    require(empty.wire.empty(), "empty abnormal termination emits no synthetic network Close");
    Pump failed; failed.data("queued"); failed.close(2,1006);
    failed.wrote(asio::error::connection_reset); failed.terminal();
}
}
int main() {
    flush_then_finish(); empty_close(); write_failure(false); write_failure(true);
    write_timeout(); ordinary_close_still_waits();
    transport_close_drains_accepted_text();
    transport_close_empty_and_failed_write();
    std::puts("WebSocket final Close session lifecycle PASS");
}
