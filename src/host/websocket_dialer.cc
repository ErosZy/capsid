#include "host/websocket_dialer.h"
#include "host/async_resolver.h"
#include "host/websocket_session.h"
#include <boost/asio/ssl.hpp>
#include <boost/beast/http.hpp>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <map>
#include <limits>
#include <mutex>
#include <set>
#include <string_view>
#include <thread>

namespace capsid::host {
namespace asio=boost::asio;
namespace beast=boost::beast;
namespace http=beast::http;
using tcp=asio::ip::tcp;
using Error=beast::error_code;
namespace {
struct Target { bool tls=false; std::string host,authority,path; uint16_t port=0; };
bool parse_target(const std::string &url,Target *target,bool fetch=false) {
    *target=Target();
    if (url.size()>16384 || url.find_first_of(" #")!=std::string::npos ||
        std::any_of(url.begin(),url.end(),[](unsigned char c){return c<0x20 || c==0x7f;})) return false;
    size_t begin=0;
    if (url.starts_with(fetch ? "http://" : "ws://")) { begin=fetch ? 7 : 5; target->port=80; }
    else if (url.starts_with(fetch ? "https://" : "wss://")) { begin=fetch ? 8 : 6; target->port=443; target->tls=true; }
    else return false;
    auto end=url.find_first_of("/?",begin); if (end==std::string::npos) end=url.size();
    target->authority=url.substr(begin,end-begin);
    if (target->authority.empty() || target->authority.find('@')!=std::string::npos) return false;
    std::string port;
    if (target->authority[0]=='[') {
        auto close=target->authority.find(']'); if(close==std::string::npos) return false;
        target->host=target->authority.substr(1,close-1);
        if (close+1<target->authority.size()) {
            if(target->authority[close+1]!=':') return false;
            port=target->authority.substr(close+2); if(port.empty()) return false;
        }
    } else {
        auto colon=target->authority.find(':');
        target->host=target->authority.substr(0,colon);
        if (colon!=std::string::npos) { port=target->authority.substr(colon+1); if(port.empty()) return false; }
    }
    if (target->host.empty()) return false;
    if (!port.empty()) {
        unsigned value=0;
        for (unsigned char c:port) { if(!std::isdigit(c)) return false; value=value*10+c-'0'; if(value>65535) return false; }
        if(!value) return false;
        target->port=static_cast<uint16_t>(value);
    }
    target->path=end==url.size() ? "/" : url[end]=='?' ? "/"+url.substr(end) : url.substr(end);
    return true;
}
bool has_token(beast::string_view input,const char *expected) {
    std::string text(input); std::transform(text.begin(),text.end(),text.begin(),[](unsigned char c){return char(std::tolower(c));});
    size_t begin=0;
    while(begin<text.size()) {
        auto end=text.find(',',begin); if(end==std::string::npos) end=text.size();
        auto token=text.substr(begin,end-begin); auto first=token.find_first_not_of(" \t"); auto last=token.find_last_not_of(" \t");
        if(first!=std::string::npos && token.substr(first,last-first+1)==expected) return true;
        begin=end+1;
    }
    return false;
}
// Slots are reserved before DNS/socket creation and retained through
// provisional ACCEPT. The dial slot returns on result; the channel slot
// follows the network session, including cancellation before result ACK.
struct Slots {
    std::shared_ptr<ServiceChannelBudget> budget;
    explicit Slots(std::shared_ptr<ServiceChannelBudget> value) : budget(std::move(value)) {}
    bool dial=false,channel=false,active=false;
    ~Slots() { release_dial(); if(active) budget->deactivate_channel(); if(channel) budget->release_channel(); }
    bool reserve() {
        dial=budget->try_reserve_dial(); if(!dial) return false;
        channel=budget->try_reserve_channel(); if(!channel) release_dial();
        return channel;
    }
    void release_dial() { if(dial) { budget->release_dial(); dial=false; } }
};
}

struct WebSocketDialer::Impl {
    struct Dial : std::enable_shared_from_this<Dial> {
        Impl &owner;
        uint64_t id;
        uint32_t channel=0;
        bool stopped=false,result_sent=false;
        bool fetch=false,body_active=false,body_reading=false,body_finished=false;
        uint64_t body_credit=protocol::kInitialChannelCredit;
        std::string url;
        protocol::FetchUpgradeRequest fetch_request;
        std::shared_ptr<void> metadata_reservation;
        std::array<uint8_t,16384> body_bytes{};
        Target target;
        std::vector<std::string> protocols;
        uint64_t max_message;
        bool refused;
        uint32_t refused_reason;
        std::shared_ptr<Slots> slots;
        AsyncResolver resolver;
        asio::steady_timer timer;
        std::shared_ptr<beast::tcp_stream> plain;
        std::shared_ptr<asio::ssl::stream<beast::tcp_stream>> secure;
        std::vector<tcp::endpoint> endpoints;
        beast::flat_buffer buffer{65536};
        http::request<http::empty_body> request;
        http::response_parser<http::buffer_body> response;
        std::string key,selected;
        std::shared_ptr<WebSocketSession> session;
        Dial(Impl &parent,WorkerEvent event) : owner(parent),id(event.dial_id),protocols(std::move(event.protocols)),
            max_message(event.channel_max_message),refused(event.dial_refused),refused_reason(event.channel_flags),resolver(parent.io),timer(parent.io) {
            fetch=event.channel_kind==protocol::kChannelKindFetchUpgrade;
            url=std::move(event.text); fetch_request=std::move(event.http_request);
            if (!parse_target(url,&target,fetch)) target.host.clear();
            response.header_limit(fetch ? static_cast<uint32_t>(protocol::kMaxChannelHttpHeaderBytes) : 16384);
            response.body_limit(std::numeric_limits<uint64_t>::max());
            if (!fetch) response.skip(true);
        }
        beast::tcp_stream &lowest() { return secure ? secure->next_layer() : *plain; }
        void phase(DialPhase value) { if(owner.network_options.on_phase) owner.network_options.on_phase(id,value); }
        void stop() {
            if(stopped) return;
            stopped=true; resolver.cancel(); timer.cancel();
            if(session) { session->abort(false); session.reset(); }
            if(secure || plain) { Error error; lowest().socket().cancel(error); lowest().close(); }
            slots.reset();
            phase(DialPhase::kStopped);
        }
        void reject(uint32_t reason,std::string message) {
            if(stopped || result_sent) return;
            stop(); result_sent=true;
            Command command; command.type=CommandType::kDialReject; command.dial_id=id;
            command.channel_flags=reason; command.close_reason=std::move(message);
            owner.send(std::move(command));
        }
        void start() {
            if(refused) { reject(refused_reason,"WebSocket channel reservation refused"); return; }
            if(target.host.empty()) { reject(6,"Invalid WebSocket URL"); return; }
            std::set<std::string> seen;
            for(const auto &protocol:protocols) {
                const bool valid=!protocol.empty() && std::all_of(protocol.begin(),protocol.end(),[](unsigned char c) {
                    return c>=0x21 && c<=0x7e && std::string_view("()<>@,;:\\\"/[]?={}").find(c)==std::string_view::npos;
                });
                if(!valid || !seen.insert(protocol).second) { reject(6,"Invalid WebSocket subprotocol offer"); return; }
            }
            if(!owner.authority.decide_host(target.host,target.port).allowed) { reject(1,"WebSocket egress denied"); return; }
            slots=std::make_shared<Slots>(owner.network_options.budget);
            if(!slots->reserve()) { reject(8,"WebSocket service capacity exhausted"); return; }
            if (fetch) {
                size_t bytes=protocol::kMaxChannelHttpHeaderBytes+body_bytes.size()+url.size();
                for (const auto &header:fetch_request.headers) bytes+=header.first.size()+header.second.size()+64;
                metadata_reservation=slots->budget->reserve_queue(bytes);
                if (!metadata_reservation) {reject(8,"HTTP upgrade header budget exhausted");return;}
            }
            timer.expires_after(std::chrono::milliseconds(std::max<uint32_t>(1,owner.limits.dial_timeout_ms>100 ? owner.limits.dial_timeout_ms-100 : 1)));
            timer.async_wait([self=shared_from_this()](Error error) { if(!error) self->reject(7,"WebSocket dial timed out"); });
            phase(DialPhase::kDns);
            resolver.resolve(target.host,target.port,[self=shared_from_this()](Error error,std::vector<tcp::endpoint> results) {
                if(self->stopped) return;
                if(error) { self->reject(error==asio::error::no_buffer_space ? 8 : 2,"WebSocket DNS failed"); return; }
                // These are this owner's actual resolution results. No
                // worker-supplied numeric address enters this check.
                for (const auto &endpoint:results) {
                    if(self->owner.authority.decide_resolved_address_authoritative(
                        endpoint.data(),static_cast<socklen_t>(endpoint.size()),self->target.port).allowed) self->endpoints.push_back(endpoint);
                }
                if(self->endpoints.empty()) { self->reject(1,"WebSocket resolved addresses denied"); return; }
                self->connect();
            },owner.network_options.dns_servers);
        }
        void connect() {
            if(target.tls) {
                if(!owner.tls_ready) { reject(4,"WebSocket TLS trust initialization failed"); return; }
                secure=std::make_shared<asio::ssl::stream<beast::tcp_stream>>(owner.io,owner.tls);
                secure->set_verify_mode(asio::ssl::verify_peer);
                secure->set_verify_callback(asio::ssl::host_name_verification(target.host));
                Error numeric_error; asio::ip::make_address(target.host,numeric_error);
                if((fetch || numeric_error) && SSL_set_tlsext_host_name(secure->native_handle(),target.host.c_str())!=1) { reject(4,"WebSocket TLS SNI failed"); return; }
            } else plain=std::make_shared<beast::tcp_stream>(owner.io);
            phase(DialPhase::kTcp);
            asio::async_connect(lowest().socket(),endpoints,[self=shared_from_this()](Error error,const tcp::endpoint &) {
                if(self->stopped) return;
                if(error) { self->reject(3,"WebSocket connect failed"); return; }
                if(self->secure) {
                    self->phase(DialPhase::kTls);
                    self->secure->async_handshake(asio::ssl::stream_base::client,[self](Error tls_error) {
                    if(self->stopped) return;
                    if(tls_error) self->reject(4,"WebSocket TLS verification failed"); else self->handshake();
                });
                } else self->handshake();
            });
        }
        void handshake() {
            phase(DialPhase::kHandshake);
            unsigned char nonce[16],encoded[25];
            if(RAND_bytes(nonce,sizeof(nonce))!=1 || EVP_EncodeBlock(encoded,nonce,sizeof(nonce))!=24) { reject(8,"WebSocket nonce generation failed"); return; }
            key.assign(reinterpret_cast<const char *>(encoded),24);
            request.version(11); request.method(http::verb::get); request.target(target.path);
            request.set(http::field::host,target.authority); request.set(http::field::upgrade,"websocket"); request.set(http::field::connection,"Upgrade");
            request.set(http::field::sec_websocket_key,key); request.set(http::field::sec_websocket_version,"13");
            if (fetch) for (const auto &header:fetch_request.headers) {
                std::string name=header.first;
                std::transform(name.begin(),name.end(),name.begin(),[](unsigned char c){return char(std::tolower(c));});
                // These fields describe the actual new handshake and its
                // bodyless GET. Caller fields such as Origin, User-Agent,
                // Content-Type and Sec-WebSocket-Protocol remain intact.
                if (name=="upgrade" || name=="connection" || name=="sec-websocket-key" || name=="sec-websocket-version" ||
                    name=="content-length" || name=="transfer-encoding") continue;
                if (name=="host") request.set(http::field::host,header.second);
                else request.insert(header.first,header.second);
            }
            std::string offered;
            for(const auto &protocol:protocols) { if(!offered.empty()) offered+=", "; offered+=protocol; }
            if(!offered.empty()) request.set(http::field::sec_websocket_protocol,offered);
            auto done=[self=shared_from_this()](Error error,size_t) {
                if(self->stopped) return;
                if(error) { self->reject(5,"WebSocket handshake write failed"); return; }
                self->read_head();
            };
            if(secure) http::async_write(*secure,request,std::move(done)); else http::async_write(*plain,request,std::move(done));
        }
        void read_head() {
            auto done=[self=shared_from_this()](Error error,size_t) {
                if(self->stopped) return;
                if(error) { self->reject(5,"WebSocket handshake read failed"); return; }
                const auto &head=self->response.get();
                if(head.result_int()!=101 && !self->fetch) {
                    self->reject(5,"Failed to establish the WebSocket connection: expected server to reply with HTTP status code 101 (switching protocols), but received " +
                        std::to_string(head.result_int()) + " instead.");
                    return;
                }
                if(head.result_int()==101 && (head.version()!=11 || head.count(http::field::upgrade)!=1 || head.count(http::field::connection)!=1 ||
                   !has_token(head[http::field::upgrade],"websocket") || !has_token(head[http::field::connection],"upgrade") ||
                   head.count(http::field::sec_websocket_accept)!=1 || head[http::field::sec_websocket_accept]!=ws::compute_accept_key(self->key) ||
                   head.count(http::field::sec_websocket_protocol)>1 || head.count(http::field::sec_websocket_extensions) || head.count(http::field::transfer_encoding) ||
                   (head.count(http::field::content_length) && head[http::field::content_length]!="0"))) {
                    self->reject(6,"Invalid WebSocket handshake response"); return;
                }
                if (!self->fetch) self->selected=std::string(head[http::field::sec_websocket_protocol]);
                if(!self->fetch && head.count(http::field::sec_websocket_protocol) && (self->selected.empty() ||
                   std::find(self->protocols.begin(),self->protocols.end(),self->selected)==self->protocols.end())) {
                    self->reject(6,"Unrequested WebSocket subprotocol"); return;
                }
                Command command; command.type=CommandType::kDialAccept; command.dial_id=self->id;
                command.channel_max_message=self->max_message; command.close_reason=self->selected;
                if (self->fetch) {
                    command.fetch_upgrade=true;
                    auto &metadata=command.http_response;
                    metadata.status=static_cast<uint16_t>(head.result_int()); metadata.status_text=std::string(head.reason()); metadata.url=self->url;
                    for (const auto &header:head.base()) metadata.headers.emplace_back(std::string(header.name_string()),std::string(header.value()));
                    protocol::ChannelAcceptPayload probe;probe.dial_id=self->id;probe.channel_id=1;probe.accepted_max_message_bytes=self->max_message;
                    probe.has_http_response=true;probe.http_response=metadata;
                    std::vector<uint8_t> encoded;
                    if (!protocol::encode_channel_accept(probe,&encoded,123)) {self->reject(6,"Invalid HTTP upgrade response metadata");return;}
                    command.channel_reservation=self->slots->budget->reserve_queue(encoded.size()+1024);
                    if (!command.channel_reservation) {self->reject(8,"HTTP upgrade result queue exhausted");return;}
                }
                self->timer.cancel(); self->result_sent=true; self->slots->release_dial();
                self->phase(DialPhase::kProvisional);
                self->owner.send(std::move(command));
            };
            if(secure) http::async_read_header(*secure,buffer,response,std::move(done)); else http::async_read_header(*plain,buffer,response,std::move(done));
        }
        void confirmed() {
            if(stopped || session || body_active || !channel) return;
            phase(DialPhase::kOpen);
            slots->budget->activate_channel(); slots->active=true;
            if (fetch && response.get().result_int()!=101) {
                // Beast retains its initial 512-byte dynamic-buffer capacity
                // after each read. Reserve the body window so available TLS
                // records reach the byte reader together; async_read_some
                // still delivers a held short body without filling the window.
                buffer.reserve(body_bytes.size());
                body_active=true; read_body(); return;
            }
            auto network=std::make_shared<WebSocketSession::Network>(); network->executor=owner.io.get_executor();
            if(secure) {
                auto stream=secure;
                network->read=[stream](asio::mutable_buffer bytes,WebSocketSession::Completion done) { stream->async_read_some(bytes,std::move(done)); };
                network->write=[stream](asio::const_buffer bytes,WebSocketSession::Completion done) { asio::async_write(*stream,bytes,std::move(done)); };
                network->close=[stream] { Error error; stream->next_layer().socket().cancel(error); stream->next_layer().close(); };
            } else {
                auto stream=plain;
                network->read=[stream](asio::mutable_buffer bytes,WebSocketSession::Completion done) { stream->async_read_some(bytes,std::move(done)); };
                network->write=[stream](asio::const_buffer bytes,WebSocketSession::Completion done) { asio::async_write(*stream,bytes,std::move(done)); };
                network->close=[stream] { Error error; stream->socket().cancel(error); stream->close(); };
            }
            std::vector<uint8_t> leftovers(buffer.size()); asio::buffer_copy(asio::buffer(leftovers),buffer.data()); buffer.consume(buffer.size());
            auto options=owner.limits; options.max_message_bytes=std::min(options.max_message_bytes,max_message);
            std::weak_ptr<Dial> weak=shared_from_this();
            session=std::make_shared<WebSocketSession>(network,slots,channel,options,60000,
                [this](Command command) { return owner.sender(std::move(command)); },
                [weak] { if(auto self=weak.lock()) { self->slots.reset(); self->owner.channels.erase(self->channel); self->owner.dials.erase(self->id); } },slots->budget);
            session->start_client(std::move(leftovers));
        }
        void end_body(bool failed, bool parse_failed=false) {
            if (stopped || body_finished) return;
            body_finished=true;
            Error ignored;lowest().socket().shutdown(tcp::socket::shutdown_both,ignored);lowest().socket().cancel(ignored);lowest().close();
            Command terminal;terminal.type=CommandType::kChannelClose;terminal.channel_id=channel;
            terminal.close_kind=failed ? protocol::kCloseTransport : protocol::kCloseFinal;
            terminal.close_code=failed ? 1006 : 0;
            if (failed) terminal.close_reason=parse_failed ? "HTTP response body parse failed" : "Network connection lost.";
            owner.send(std::move(terminal));
        }
        void read_body() {
            if (stopped || body_reading || body_finished || !body_credit) return;
            if (response.is_done()) {end_body(false);return;}
            const size_t capacity=static_cast<size_t>(std::min<uint64_t>(std::min<uint64_t>(body_bytes.size(),max_message),body_credit));
            response.get().body().data=body_bytes.data();response.get().body().size=capacity;body_reading=true;
            auto done=[self=shared_from_this(),capacity](Error error,size_t) {
                self->body_reading=false;if (self->stopped || self->body_finished) return;
                const size_t produced=capacity-self->response.get().body().size;
                if (error==http::error::need_buffer) error={};
                if (error) {
                    const bool parse_failed=error.category()==http::make_error_code(http::error::bad_chunk).category() &&
                        error!=http::error::partial_message && error!=http::error::end_of_stream;
                    self->end_body(true,parse_failed);return;
                }
                if (produced) {
                    Command data;data.type=CommandType::kChannelData;data.channel_id=self->channel;
                    data.channel_flags=protocol::kFlagStart|protocol::kFlagEnd;
                    data.channel_reservation=self->slots->budget->reserve_queue(produced+1024);
                    if (!data.channel_reservation) {self->end_body(true);return;}
                    data.body.assign(self->body_bytes.begin(),self->body_bytes.begin()+produced);
                    self->body_credit-=produced;self->owner.send(std::move(data));
                }
                if (self->response.is_done()) self->end_body(false);else self->read_body();
            };
            if (secure) http::async_read_some(*secure,buffer,response,std::move(done));else http::async_read_some(*plain,buffer,response,std::move(done));
        }
        void body_event(const WorkerEvent &event) {
            if (event.type==WorkerEvent::Type::kChannelCredit && !body_finished) {
                if (event.channel_credit>std::numeric_limits<uint64_t>::max()-body_credit) {end_body(true);return;}
                body_credit+=event.channel_credit;read_body();
            } else if (event.type==WorkerEvent::Type::kChannelCancel) {
                stop();Command ack;ack.type=CommandType::kChannelConfirm;ack.channel_id=channel;ack.channel_flags=protocol::kAckCancel;owner.send(std::move(ack));
                owner.channels.erase(channel);owner.dials.erase(id);
            } else if (event.type==WorkerEvent::Type::kChannelAck && event.ack_kind==protocol::kAckTransportClose) {
                stop();owner.channels.erase(channel);owner.dials.erase(id);
            }
        }
    };
    EgressPolicy authority;
    protocol::ChannelLimits limits;
    WebSocketDialerOptions network_options;
    std::function<bool(Command)> sender;
    std::function<void(size_t)> release;
    asio::io_context io;
    asio::ssl::context tls{asio::ssl::context::tls_client};
    bool tls_ready=false;
    asio::executor_work_guard<asio::io_context::executor_type> guard{io.get_executor()};
    asio::steady_timer retry{io};
    std::thread thread;
    std::atomic<bool> stopping{false};
    std::mutex mutex;
    std::deque<WorkerEvent> events;
    std::deque<Command> commands;
    std::map<uint64_t,std::shared_ptr<Dial>> dials;
    std::map<uint32_t,std::shared_ptr<Dial>> channels;
    Impl(EgressPolicy policy,protocol::ChannelLimits options,std::function<bool(Command)> send,std::function<void(size_t)> free,WebSocketDialerOptions network)
        : authority(std::move(policy)),limits(options),network_options(std::move(network)),sender(std::move(send)),release(std::move(free)) {
        if(!network_options.budget) network_options.budget=default_channel_budget();
        Error error; tls.set_default_verify_paths(error);
        tls_ready=!error && SSL_CTX_set_min_proto_version(tls.native_handle(),TLS1_2_VERSION)==1;
        thread=std::thread([this] { io.run(); });
    }
    void send(Command command) { if(!stopping) { commands.push_back(std::move(command)); pump_commands(); } }
    void pump_commands() {
        while(!commands.empty() && sender(commands.front())) commands.pop_front();
        if(!commands.empty() && !stopping) {
            retry.expires_after(std::chrono::milliseconds(5)); retry.async_wait([this](Error error) { if(!error) pump_commands(); });
        }
    }
    void consume(WorkerEvent event) {
        if(event.type==WorkerEvent::Type::kChannelDial) {
            auto dial=std::make_shared<Dial>(*this,std::move(event)); dials.emplace(dial->id,dial);
            if(dials.size()>limits.max_channels) dial->reject(8,"WebSocket worker dial capacity exhausted"); else dial->start();
            return;
        }
        if(event.type==WorkerEvent::Type::kChannelReady) {
            auto found=dials.find(event.dial_id); if(found==dials.end()) return;
            if(!event.channel_id) { found->second->stop(); return; }
            found->second->channel=event.channel_id; channels.emplace(event.channel_id,found->second); return;
        }
        auto found=event.channel_id ? channels.find(event.channel_id) : channels.end();
        std::shared_ptr<Dial> dial=found==channels.end() ? nullptr : found->second;
        if(!dial && event.dial_id) { auto pending=dials.find(event.dial_id); if(pending!=dials.end()) dial=pending->second; }
        if(!dial) return;
        if(event.type==WorkerEvent::Type::kChannelAck && event.ack_kind==0) {
            if(event.channel_id) dial->confirmed(); else dials.erase(dial->id);
        } else if(dial->body_active) dial->body_event(event);
        else if(event.type==WorkerEvent::Type::kChannelCancel && !dial->session) {
            dial->stop();
            Command ack; ack.type=CommandType::kChannelConfirm; ack.channel_id=event.channel_id;
            ack.dial_id=event.channel_id ? 0 : event.dial_id; ack.channel_flags=1; send(std::move(ack));
            if(dial->channel) channels.erase(dial->channel);
            dials.erase(dial->id);
        } else if(dial->session) dial->session->on_event(std::move(event));
    }
    void post(WorkerEvent event) {
        if(stopping) { release(event.channel_charge); return; }
        bool wake;
        { std::lock_guard<std::mutex> lock(mutex);
          if(stopping) { release(event.channel_charge); return; }
          if(event.type==WorkerEvent::Type::kChannelCredit) for(auto &queued:events)
              if(queued.type==event.type && queued.channel_id==event.channel_id &&
                 event.channel_credit<=std::numeric_limits<uint64_t>::max()-queued.channel_credit) { queued.channel_credit+=event.channel_credit; return; }
          wake=events.empty(); events.push_back(std::move(event));
        }
        if(wake) asio::post(io,[this] {
            std::deque<WorkerEvent> local; { std::lock_guard<std::mutex> lock(mutex); local.swap(events); }
            for(auto &item:local) { const size_t charge=item.channel_charge; if(!stopping) consume(std::move(item)); release(charge); }
        });
    }
    void request_stop() {
        if(stopping.exchange(true)) return;
        asio::post(io,[this] {
            retry.cancel(); commands.clear();
            auto pending=std::move(dials); channels.clear();
            for(auto &item:pending) item.second->stop();
            std::deque<WorkerEvent> local; { std::lock_guard<std::mutex> lock(mutex); local.swap(events); }
            for(const auto &item:local) release(item.channel_charge);
            guard.reset();
        });
    }
};
WebSocketDialer::WebSocketDialer(EgressPolicy policy,protocol::ChannelLimits options,std::function<bool(Command)> sender,std::function<void(size_t)> release,WebSocketDialerOptions network)
    : impl_(std::make_unique<Impl>(std::move(policy),options,std::move(sender),std::move(release),std::move(network))) {}
WebSocketDialer::~WebSocketDialer() { stop_and_join(); }
void WebSocketDialer::post(WorkerEvent event) { impl_->post(std::move(event)); }
void WebSocketDialer::request_stop() { impl_->request_stop(); }
void WebSocketDialer::stop_and_join() { impl_->request_stop(); if(impl_->thread.joinable()) impl_->thread.join(); }
}
