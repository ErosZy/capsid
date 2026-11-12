#include "host/websocket_session.h"
#include <algorithm>
#include <cctype>
#include <limits>
#include <openssl/rand.h>

namespace capsid::host {
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace {
std::shared_ptr<WebSocketSession::Network> tcp_network(beast::tcp_stream &stream) {
    stream.expires_never();
    auto network=std::make_shared<WebSocketSession::Network>();
    network->executor=stream.get_executor();
    network->read=[&stream](asio::mutable_buffer buffer, WebSocketSession::Completion done) { stream.async_read_some(buffer,std::move(done)); };
    network->write=[&stream](asio::const_buffer buffer, WebSocketSession::Completion done) { asio::async_write(stream,buffer,std::move(done)); };
    network->close=[&stream] { beast::error_code error; stream.socket().cancel(error); stream.close(); };
    return network;
}
bool encode_frame(const ws::Frame &frame, ws::Role role, std::vector<uint8_t> *bytes) {
    uint32_t mask=0;
    if (role==ws::kClient && RAND_bytes(reinterpret_cast<unsigned char *>(&mask),sizeof(mask))!=1) return false;
    return ws::encode(frame,role,mask,bytes);
}
}
bool websocket_handshake(const std::string &key, const std::string &offered,
    const std::vector<std::pair<std::string,std::string>> &headers, std::string *out) {
    if (!ws::validate_client_key(key)) return false;
    std::string result = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " + ws::compute_accept_key(key) + "\r\n";
    bool selected = false;
    for (const auto &header : headers) {
        std::string name = header.first;
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) {return char(std::tolower(c));});
        if (name == "sec-websocket-protocol") {
            if (selected || header.second.empty() || header.second.find(',') != std::string::npos) return false;
            bool matched = false;
            size_t offset = 0;
            while (offset < offered.size()) {
                auto end = offered.find(',', offset); if (end == std::string::npos) end = offered.size();
                std::string token = offered.substr(offset, end-offset);
                auto begin = token.find_first_not_of(" \t"); auto last = token.find_last_not_of(" \t");
                if (begin != std::string::npos && token.substr(begin,last-begin+1) == header.second) matched = true;
                offset = end+1;
            }
            if (!matched) return false;
            selected = true;
        }
        if (name == "connection" || name == "upgrade" || name == "sec-websocket-accept" ||
            name == "sec-websocket-extensions" || name == "content-length" || name == "transfer-encoding") continue;
        result += header.first + ": " + header.second + "\r\n";
    }
    *out = result + "\r\n";
    return true;
}
WebSocketSession::WebSocketSession(beast::tcp_stream &stream, std::shared_ptr<void> anchor,
    std::shared_ptr<ChannelLease> lease, uint32_t id, const capsid::protocol::ChannelLimits &limits,
    uint64_t write_timeout_ms, Sender sender, std::function<void()> done, std::shared_ptr<ServiceChannelBudget> budget)
    : network_(tcp_network(stream)), anchor_(std::move(anchor)), lease_(std::move(lease)),
      budget_(budget ? std::move(budget) : default_channel_budget()), id_(id),
      close_timeout_ms_(limits.close_timeout_ms), write_timeout_ms_(write_timeout_ms),
      queue_limit_(limits.per_channel_queue_bytes),idle_timeout_ms_(limits.idle_timeout_ms),
      sender_(std::move(sender)), done_(std::move(done)), decoder_(limits.max_message_bytes),
      close_timer_(stream.get_executor()), write_timer_(stream.get_executor()), retry_timer_(stream.get_executor()),idle_timer_(stream.get_executor()) {}

WebSocketSession::WebSocketSession(std::shared_ptr<Network> network, std::shared_ptr<void> anchor,
    uint32_t id, const capsid::protocol::ChannelLimits &limits,
    uint64_t write_timeout_ms, Sender sender, std::function<void()> done, std::shared_ptr<ServiceChannelBudget> budget)
    : network_(std::move(network)), role_(ws::kClient), anchor_(std::move(anchor)),
      budget_(budget ? std::move(budget) : default_channel_budget()), id_(id),
      close_timeout_ms_(limits.close_timeout_ms), write_timeout_ms_(write_timeout_ms),
      queue_limit_(limits.per_channel_queue_bytes), idle_timeout_ms_(limits.idle_timeout_ms),
      sender_(std::move(sender)),done_(std::move(done)),decoder_(limits.max_message_bytes,ws::kClient),
      close_timer_(network_->executor),write_timer_(network_->executor),retry_timer_(network_->executor),idle_timer_(network_->executor) {}

void WebSocketSession::start_client(std::vector<uint8_t> leftovers) {
    if (leftovers.size()>65536) { abort(); return; }
    input_=std::move(leftovers); upgraded_=true; touch_idle(); pump_input();
}

void WebSocketSession::start(std::string handshake, std::vector<uint8_t> leftovers) {
    if (leftovers.size() > 65536) { abort(); return; }
    input_ = std::move(leftovers);
    Write item;
    item.charge=budget_->reserve_queue(handshake.size()+256);
    if (!item.charge) { abort(); return; }
    item.bytes = std::make_shared<std::vector<uint8_t>>(handshake.begin(), handshake.end());
    item.handshake = true;
    queued_write_bytes_ += item.bytes->size() + 256;
    writes_.push_back(std::move(item));
    write();
}
bool WebSocketSession::submit(Command command) {
    command.channel_id = id_;
    return sender_ && sender_(std::move(command));
}
void WebSocketSession::touch_idle() {
    if (!idle_timeout_ms_ || stopped_) return;
    idle_timer_.expires_after(std::chrono::milliseconds(idle_timeout_ms_));
    idle_timer_.async_wait([self=shared_from_this()](beast::error_code error) { if(!error) self->abort(); });
}
void WebSocketSession::arm_close_timer(uint32_t maximum_ms) {
    uint32_t timeout = close_timeout_ms_ ? close_timeout_ms_ : 5000;
    if (maximum_ms) timeout = std::min(timeout, maximum_ms);
    close_timer_.expires_after(std::chrono::milliseconds(timeout));
    close_timer_.async_wait([self=shared_from_this()](beast::error_code error) { if (!error) self->abort(); });
}
void WebSocketSession::on_event(WorkerEvent event) {
    if (stopped_ || shutting_down_) return;
    if (event.type == WorkerEvent::Type::kChannelCredit) {
        if (event.channel_credit > std::numeric_limits<uint64_t>::max() - credit_) { abort(); return; }
        credit_ += event.channel_credit;
        pump_input();
    } else if (event.type == WorkerEvent::Type::kChannelData) {
        // Fragments of one worker send() coalesce into a single
        // WebSocket frame; IPC chunk boundaries must not leak onto
        // the public wire (workerd emits one frame per message).
        if (event.channel_flags & CAPSID_CHANNEL_FLAG_START) {
            pending_message_.clear();
            pending_credit_ = 0;
            pending_text_ = (event.channel_flags & CAPSID_CHANNEL_FLAG_TEXT) != 0;
        }
        pending_message_.insert(pending_message_.end(),
            event.body.begin(), event.body.end());
        pending_credit_ += event.body.size();
        // Early credit (mirrors the HTTP response window): fragment
        // bytes return to the worker at receive time while the write
        // queue is shallow, so a burst never exhausts the worker-side
        // send budget. A deep queue withholds credit until it drains.
        if (!host_closed_) {
            if (withheld_credit_ >
                    std::numeric_limits<uint64_t>::max() - event.body.size()) {
                abort();
                return;
            }
            withheld_credit_ += event.body.size();
            release_credit();
        }
        if (event.channel_flags & CAPSID_CHANNEL_FLAG_END) {
            ws::Frame frame;
            frame.fin = true;
            frame.opcode = pending_text_ ? ws::kText : ws::kBinary;
            frame.payload = std::move(pending_message_);
            pending_message_.clear();
            pending_message_.shrink_to_fit();
            if (!queue_frame(std::move(frame), pending_credit_)) abort();
        }
    } else if (event.type == WorkerEvent::Type::kChannelClose) {
        if (event.close_kind == 2) {
            confirm_transport_after_write_ = true;
            host_closed_ = peer_closed_ = fail_after_write_ = true;
            // Accepted messages can still be waiting for the previous socket
            // write. Preserve their FIFO bytes before acknowledging transport
            // termination; abnormal termination adds no WebSocket Close frame.
            if (writes_.empty()) finish();
            else { arm_close_timer(); write(); }
            return;
        }
        if (worker_closed_) return;
        worker_closed_ = true;
        if (event.close_kind == 3) {
            // The worker atomically committed an accepted Close and a fatal
            // handler outcome. Drain earlier writes and this Close, then
            // stop the socket even when the peer never replies. The ACK
            // follows actual teardown, including write errors/timeouts.
            confirm_transport_after_write_ = true;
            host_closed_ = peer_closed_ = fail_after_write_ = true;
        }
        ws::Frame frame; frame.opcode = ws::kClose;
        if (event.close_code) {
            frame.payload.push_back(uint8_t(event.close_code >> 8)); frame.payload.push_back(uint8_t(event.close_code));
            frame.payload.insert(frame.payload.end(), event.text.begin(), event.text.end());
        }
        if (!queue_frame(std::move(frame))) { abort(); return; }
        arm_close_timer();
    } else if (event.type == WorkerEvent::Type::kChannelCancel) {
        network_->close();
        Command ack; ack.type=CommandType::kChannelConfirm; ack.channel_flags=1;
        submit(std::move(ack)); abort(false);
    }
}
bool WebSocketSession::queue_frame(ws::Frame frame, uint64_t credit, bool pong) {
    if (stopped_) return false;
    if (pong) {
        // RFC 6455 permits 1:1 pong delivery and allows replying to
        // the newest pending Ping. The reference answers pings without
        // aggressive coalescing, so queue pongs one-to-one up to a
        // small window and only then fold into the newest queued pong
        // (never mutating a buffer owned by an in-flight write).
        size_t queued_pongs = 0;
        for (size_t i = writing_ ? 1 : 0; i<writes_.size(); ++i) if (writes_[i].pong) queued_pongs++;
        if (queued_pongs < 8) {
            const size_t charge = frame.payload.size() + 270;
            if (charge <= queue_limit_ && queued_write_bytes_ <= queue_limit_ - charge) {
                Write item; item.charge=budget_->reserve_queue(charge);
                if (item.charge) {
                    item.bytes = std::make_shared<std::vector<uint8_t>>();
                    item.credit=0; item.pong=true;
                    if (encode_frame(frame, role_, item.bytes.get())) {
                        queued_write_bytes_ += item.bytes->size() + 256;
                        writes_.push_back(std::move(item));
                        write();
                        return true;
                    }
                    return false;
                }
            }
        }
        // Replace the newest pending pong. Replacing the first would
        // send the latest payload ahead of older replies still queued.
        const size_t first_pending = writing_ ? 1 : 0;
        for (size_t end = writes_.size(); end > first_pending; --end) {
            const size_t i = end - 1;
            if (!writes_[i].pong) continue;
            auto charge=budget_->reserve_queue(frame.payload.size()+270);
            if (!charge) return false;
            auto bytes = std::make_shared<std::vector<uint8_t>>();
            if (!encode_frame(frame, role_, bytes.get())) return false;
            const size_t previous = writes_[i].bytes->size();
            if (bytes->size() > previous &&
                    bytes->size() - previous > queue_limit_ - queued_write_bytes_) return false;
            queued_write_bytes_ -= previous; queued_write_bytes_ += bytes->size();
            writes_[i].bytes = std::move(bytes); writes_[i].charge=std::move(charge); return true;
        }
    }
    const size_t charge = frame.payload.size() + 270;
    if (charge > queue_limit_ || queued_write_bytes_ > queue_limit_ - charge) return false;
    Write item; item.charge=budget_->reserve_queue(charge);
    if (!item.charge) return false;
    item.bytes = std::make_shared<std::vector<uint8_t>>();
    item.credit=credit; item.close=frame.opcode==ws::kClose; item.pong=pong;
    if (!encode_frame(frame, role_, item.bytes.get())) return false;
    queued_write_bytes_ += item.bytes->size() + 256;
    writes_.push_back(std::move(item));
    write(); return true;
}
void WebSocketSession::release_credit() {
    if (stopped_ || withheld_credit_ == 0) return;
    // High/low watermark: a deep write queue withholds worker credit
    // so a slow consumer exerts backpressure instead of tripping the
    // queue limit; draining below half the limit releases it again.
    if (queued_write_bytes_ > queue_limit_ / 2) return;
    Command grant;
    grant.type = CommandType::kChannelGrant;
    grant.channel_credit = withheld_credit_;
    withheld_credit_ = 0;
    if (!submit(std::move(grant))) abort();
}
void WebSocketSession::write() {
    if (stopped_ || writing_ || writes_.empty()) return;
    writing_ = true;
    const Write item = writes_.front();
    if (write_timeout_ms_) {
        write_timer_.expires_after(std::chrono::milliseconds(write_timeout_ms_));
        write_timer_.async_wait([self=shared_from_this()](beast::error_code error) { if (!error) { if (getenv("CAPSID_DEBUG_CREDIT")) fprintf(stderr, "[dbg] WRITE TIMEOUT abort\n"); self->abort(); } });
    }
    network_->write(asio::buffer(*item.bytes),
        [self=shared_from_this(), item](beast::error_code error, size_t) {
            if (self->stopped_) return;
            self->writing_=false; self->write_timer_.cancel();
            if (error) { if (getenv("CAPSID_DEBUG_CREDIT")) fprintf(stderr, "[dbg] WRITE ERROR abort: %s\n", error.message().c_str()); self->abort(); return; }
            self->queued_write_bytes_ -= item.bytes->size() + 256;
            self->touch_idle();
            self->writes_.pop_front();
            if (item.handshake) { self->upgraded_=true; self->pump_input(); }
            if (item.credit && !self->host_closed_) {
                // Credit was already returned at receive time (early
                // window); completions only reopen the release gate
                // once the queue has drained below the watermark.
                self->release_credit();
            }
            if (item.close) {
                self->close_written_=true;
                if (self->peer_closed_ || self->fail_after_write_) { self->finish(); return; }
            }
            if (self->fail_after_write_ && self->writes_.empty()) { self->finish(); return; }
            self->write();
        });
}
void WebSocketSession::read() {
    if (stopped_ || reading_ || !upgraded_ || peer_closed_ || !credit_ || pending_input_) return;
    reading_=true;
    network_->read(asio::buffer(read_bytes_), [self=shared_from_this()](beast::error_code error, size_t size) {
        self->reading_=false;
        if (self->stopped_) return;
        if (error) { self->abort(); return; }
        self->touch_idle();
        self->input_.assign(self->read_bytes_.begin(), self->read_bytes_.begin()+size);
        self->input_offset_=0; self->pump_input();
    });
}
void WebSocketSession::pump_input() {
    if (stopped_ || !upgraded_ || peer_closed_) return;
    for (size_t quantum=0; quantum<32; ++quantum) {
        if (!pending_input_) {
            ws::StreamDecoder::Item item; size_t used=0;
            const size_t size=input_.size()-input_offset_;
            const auto result=decoder_.consume(size ? input_.data()+input_offset_ : nullptr, size, &used, credit_, &item);
            input_offset_+=used;
            if (result==ws::StreamDecoder::Error) { protocol_failure(decoder_.error_code()); return; }
            if (result==ws::StreamDecoder::NeedMore) break;
            pending_input_=std::move(item);
        }
        const auto &item=*pending_input_;
        if (item.opcode==ws::kPing) {
            ws::Frame pong; pong.opcode=ws::kPong; pong.payload=item.payload;
            if (!queue_frame(std::move(pong),0,true)) { abort(); return; }
        } else if (item.opcode==ws::kClose) {
            Command close; close.type=CommandType::kChannelClose; close.close_code=item.code; close.close_kind=1;
            if (item.payload.size()>2) close.close_reason.assign(item.payload.begin()+2,item.payload.end());
            if (!shutting_down_ && !submit(std::move(close))) break;
            peer_closed_=host_closed_=true;
            if (!shutting_down_) arm_close_timer();
            if (close_written_) { finish(); return; }
        } else if (item.opcode!=ws::kPong) {
            if (shutting_down_) { pending_input_.reset(); continue; }
            Command data; data.type=CommandType::kChannelData; data.body=item.payload;
            data.channel_flags=(item.start?1:0)|(item.end?2:0)|(item.text?4:0);
            if (!submit(std::move(data))) break;
            credit_-=item.payload.size();
        }
        pending_input_.reset();
        if (peer_closed_) return;
    }
    if (input_offset_==input_.size()) { input_.clear(); input_offset_=0; }
    if (pending_input_ || !input_.empty()) {
        // The executor's queue may be full without a byte-credit event
        // (empty messages). A bounded retry retains just this read chunk.
        retry_timer_.expires_after(std::chrono::milliseconds(5));
        retry_timer_.async_wait([self=shared_from_this()](beast::error_code error) { if (!error) self->pump_input(); });
    } else read();
}
void WebSocketSession::protocol_failure(uint16_t code) {
    Command cancel; cancel.type=CommandType::kChannelCancel; cancel.channel_flags=CAPSID_CANCEL_REASON_PROTOCOL_VIOLATION;
    submit(std::move(cancel)); host_closed_=true; peer_closed_=true; fail_after_write_=true;
    ws::Frame close; close.opcode=ws::kClose; close.payload={uint8_t(code>>8),uint8_t(code)};
    if (!queue_frame(std::move(close))) abort(false);
    arm_close_timer();
}
void WebSocketSession::shutdown() {
    if (stopped_ || shutting_down_) return;
    shutting_down_ = true;
    host_closed_ = true;
    pending_message_.clear();
    pending_credit_ = 0;
    // Retain any already queued Close; never put two Close frames on the wire.
    if (!worker_closed_) {
        worker_closed_ = true;
        ws::Frame close;
        close.opcode = ws::kClose;
        close.payload = {0x03, 0xe9};  // 1001 Going Away
        if (!queue_frame(std::move(close))) { abort(false); return; }
    }
    // Normal process stop is bounded even if a peer never replies. Runtime
    // shutdown follows transport cleanup, so worker exit cannot cut off Close.
    arm_close_timer(1000);
    pump_input();
}
void WebSocketSession::abort(bool notify_worker) {
    if (stopped_) return;
    if (notify_worker && !host_closed_) {
        Command cancel; cancel.type=CommandType::kChannelCancel; cancel.channel_flags=CAPSID_CANCEL_REASON_DISCONNECT;
        submit(std::move(cancel));
    }
    finish();
}
void WebSocketSession::finish() {
    if (stopped_) return;
    stopped_=true;
    close_timer_.cancel(); write_timer_.cancel(); retry_timer_.cancel(); idle_timer_.cancel();
    network_->close();
    writes_.clear(); queued_write_bytes_=0; input_.clear(); pending_input_.reset();
    if (confirm_transport_after_write_) {
        confirm_transport_after_write_ = false;
        Command ack; ack.type=CommandType::kChannelConfirm; ack.channel_flags=2;
        submit(std::move(ack));
    }
    lease_.reset();
    // Outbound Network callbacks retain their stream independently. Return
    // its channel reservation at transport teardown even when canceled I/O
    // or timers still retain this session. Accepted streams use anchor_ to
    // own the referenced TCP stream and keep that anchor until destruction.
    if (role_ == ws::kClient) anchor_.reset();
    if (done_) { auto done=std::move(done_); done(); }
}
}
