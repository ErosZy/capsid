#include "host/async_resolver.h"
#include "host/websocket_dialer.h"
#include "host/channel_ledger.h"
#include "helpers/host_ws_peer.h"
#include <atomic>
#include <map>
#include <mutex>
#include <vector>
#ifdef _WIN32
#include <mstcpip.h>
#endif
#ifdef __linux__
#include <filesystem>
#endif

using namespace capsid_test;
using namespace capsid::host;
namespace protocol = capsid::protocol;
#ifdef CAPSID_TEST_WRAP_CONNECT
namespace {
std::atomic<bool> capture_connects{false};
std::atomic<uint16_t> dns_port{0};
std::atomic<unsigned> non_dns_connects{0};
}
extern "C" int __real_connect(int, const sockaddr *, socklen_t);
extern "C" int __wrap_connect(int fd, const sockaddr *address, socklen_t size) {
    if (capture_connects && address && (address->sa_family == AF_INET || address->sa_family == AF_INET6)) {
        const uint16_t port = address->sa_family == AF_INET ? ntohs(reinterpret_cast<const sockaddr_in *>(address)->sin_port)
                                                        : ntohs(reinterpret_cast<const sockaddr_in6 *>(address)->sin6_port);
        if (port != dns_port) ++non_dns_connects;
    }
    return __real_connect(fd, address, size);
}
#endif
namespace {
struct DnsPeer {
    asio::io_context io;
    asio::ip::udp::socket socket{io, {asio::ip::address_v4::loopback(), 0}};
    struct Packet { std::vector<uint8_t> bytes; asio::ip::udp::endpoint from; };
    DnsPeer() {
#ifdef _WIN32
        // Late replies deliberately target canceled resolver sockets. A UDP
        // server must not turn their ICMP Port Unreachable into a failed
        // receive of its next, unrelated DNS query. Actual packets and every
        // other socket error are still checked below.
        // https://learn.microsoft.com/en-us/windows/win32/winsock/winsock-ioctls#sio_udp_connreset-opcode-setting-i-t3
        BOOL report_unreachable = FALSE;
        DWORD returned = 0;
        require(WSAIoctl(socket.native_handle(), SIO_UDP_CONNRESET,
            &report_unreachable, static_cast<DWORD>(sizeof(report_unreachable)),
            nullptr, 0, &returned, nullptr, nullptr) == 0,
            "configure late-reply DNS peer: " + std::to_string(WSAGetLastError()));
#endif
        socket.non_blocking(true);
    }
    std::string server() { return "127.0.0.1:" + std::to_string(socket.local_endpoint().port()); }
    std::vector<Packet> receive() {
        std::vector<Packet> result;
        for (;;) {
            std::vector<uint8_t> bytes(4096); asio::ip::udp::endpoint from; boost::system::error_code error;
            const auto size = socket.receive_from(asio::buffer(bytes), from, 0, error);
            if (error == asio::error::would_block || error == asio::error::try_again) break;
            require(!error && size >= 12, "received actual DNS query: " +
                std::to_string(error.value()) + "/" + error.message() +
                ", bytes=" + std::to_string(size)); bytes.resize(size);
            result.push_back({std::move(bytes), from});
        }
        return result;
    }
    static std::string question(const Packet &packet) {
        const auto &bytes = packet.bytes;
        std::string name; size_t offset = 12;
        while (offset < bytes.size() && bytes[offset]) {
            const size_t size = bytes[offset++];
            require(size <= 63 && size <= bytes.size() - offset, "valid DNS question label");
            if (!name.empty()) name += '.';
            name.append(reinterpret_cast<const char *>(bytes.data() + offset), size);
            offset += size;
        }
        require(offset + 5 <= bytes.size(), "complete DNS question");
        return name;
    }
    void answer(const Packet &packet, bool nxdomain = false) {
        auto bytes = packet.bytes; size_t end = 12;
        while (end < bytes.size() && bytes[end]) end += bytes[end] + 1;
        require(end + 5 <= bytes.size(), "well formed DNS question");
        const uint16_t type = uint16_t(bytes[end+1]) * 256 + bytes[end+2]; end += 5; bytes.resize(end);
        bytes[2] = 0x81; bytes[3] = nxdomain ? 0x83 : 0x80;
        bytes[6] = 0; bytes[7] = nxdomain ? 0 : 1;
        bytes[8] = bytes[9] = bytes[10] = bytes[11] = 0;
        if (!nxdomain) {
            bytes.insert(bytes.end(), {0xc0, 0x0c, 0, uint8_t(type), 0, 1, 0, 0, 0, 1, 0, uint8_t(type == 28 ? 16 : 4)});
            if (type == 28) { bytes.insert(bytes.end(), 15, 0); bytes.push_back(1); }
            else bytes.insert(bytes.end(), {127, 0, 0, 1});
        }
        boost::system::error_code error; socket.send_to(asio::buffer(bytes), packet.from, 0, error);
        require(!error, "DNS reply sent");
    }
};
#ifdef __linux__
size_t entries(const char *path) { return std::distance(std::filesystem::directory_iterator(path), std::filesystem::directory_iterator{}); }
#endif
void resolver_lifecycle() {
    DnsPeer dns; asio::io_context io; AsyncResolver resolver(io);
#ifdef CAPSID_TEST_WRAP_CONNECT
    dns_port = dns.socket.local_endpoint().port(); capture_connects = true; non_dns_connects = 0;
#endif
    int callbacks = 0; bool success = false;
    auto pump = [&] { io.restart(); io.poll(); };
    auto completed = [&](boost::system::error_code error, std::vector<tcp::endpoint> endpoints) {
        ++callbacks; success = !error && endpoints.size() == 2;
        for (const auto &ep : endpoints) require(ep.port() == 32123, "numeric service port preserved");
    };
    // Controlled DNS questions are absolute. Native search domains must
    // not change the name whose wire packets this peer asserts.
    resolver.resolve("dns.capsid.test.", 32123, completed, dns.server());
    require(wait_until([&] { pump(); for (const auto &packet : dns.receive()) dns.answer(packet); return callbacks == 1; }), "A/AAAA resolution completes");
    require(success, "both actual DNS families returned");
#ifdef CAPSID_TEST_WRAP_CONNECT
    require(non_dns_connects == 0, "resolver does not probe/connect returned addresses before egress authorization");
#endif
    resolver.cancel(); pump();
#ifdef __linux__
    const auto initial_fds = entries("/proc/self/fd"), initial_threads = entries("/proc/self/task");
#endif
    callbacks = 0;
    for (int iteration = 0; iteration < 64; ++iteration) {
        const std::string host = "stalled-" + std::to_string(iteration) + ".capsid.test";
        resolver.resolve(host + ".", 32123, completed, dns.server());
        std::vector<DnsPeer::Packet> packets;
        require(wait_until([&] {
            pump();
            for (auto &packet : dns.receive()) {
                if (DnsPeer::question(packet) == host) packets.push_back(std::move(packet));
                else dns.answer(packet); // an older canceled lookup cannot satisfy this barrier
            }
            return !packets.empty();
        }), "this lookup's actual DNS query is on wire before cancel");
        resolver.cancel();
        for (const auto &packet : packets) dns.answer(packet); // late result must be discarded
        pump(); require(callbacks == 0, "no callback after cancel or late DNS reply");
    }
    resolver.resolve("127.0.0.1", 32123, completed); // completed inline, notification still queued
    resolver.cancel(); pump(); require(callbacks == 0, "cancel suppresses already queued completion");
    resolver.resolve("missing.capsid.test.", 32123, completed, dns.server());
    require(wait_until([&] { pump(); for (const auto &packet : dns.receive()) dns.answer(packet, true); return callbacks == 1; }), "NXDOMAIN completes exactly once");
    require(!success, "NXDOMAIN reported as failure"); resolver.cancel(); pump();
    const auto before = Clock::now(); io.restart(); io.run();
    require(Clock::now() - before < std::chrono::seconds(1), "resolver event loop drains without background lookup");
#ifdef __linux__
    require(entries("/proc/self/fd") == initial_fds, "all DNS descriptors returned after 64 cancellations");
    require(entries("/proc/self/task") == initial_threads, "DNS created no resolver thread");
#endif
#ifdef CAPSID_TEST_WRAP_CONNECT
    capture_connects = false;
#endif
    std::puts("resolver: real A/AAAA, NXDOMAIN, 64 cancel/late replies, queued completion cancellation PASS");
}
struct Recorder {
    std::mutex mutex;
    std::vector<Command> commands;
    std::map<uint64_t, DialPhase> phases;
    std::atomic<size_t> released{0};
    bool send(Command command) { std::lock_guard<std::mutex> lock(mutex); commands.push_back(std::move(command)); return true; }
    void phase(uint64_t id, DialPhase value) { std::lock_guard<std::mutex> lock(mutex); phases[id] = value; }
    bool at(uint64_t id, DialPhase value) { std::lock_guard<std::mutex> lock(mutex); return phases.count(id) && phases[id] == value; }
    std::vector<Command> take() { std::lock_guard<std::mutex> lock(mutex); auto result = std::move(commands); commands.clear(); return result; }
};
capsid::EgressPolicy authority() {
    capsid_egress_rule rules[2]; for (auto &rule : rules) capsid_egress_rule_init(&rule);
    rules[0].action = rules[1].action = CAPSID_EGRESS_ALLOW;
    rules[0].target = "127.0.0.0/8";
    rules[1].target = "stalled.capsid.test";
    capsid_egress_policy policy; capsid_egress_policy_init(&policy); policy.rules = rules; policy.rule_count = 2;
    capsid::EgressPolicy result; std::string error; require(result.configure(&policy, &error), error); return result;
}
enum class Action { Cancel, Timeout, Stop };
void reserved_invalid_lifecycle() {
    DnsPeer dns; asio::io_context io; AsyncResolver resolver(io);
    unsigned callbacks = 0;
    auto pump = [&] { io.restart(); io.poll(); };
    for (const auto *host : {"invalid", "invalid.", "INVALID", "a.invalid", "a.b.InVaLiD.", "does-not-exist.invalid"}) {
        const auto before = callbacks;
        resolver.resolve(host, 32123, [&](auto error, auto endpoints) {
            ++callbacks;
            require(error == asio::error::host_not_found && endpoints.empty(), "reserved invalid name returns a negative result");
        }, dns.server());
        require(callbacks == before, "reserved negative completion is posted, never inline");
        pump(); require(callbacks == before + 1, "one loop dispatch delivers the immediate negative result");
        require(dns.receive().empty(), "reserved invalid name must send no actual DNS query");
        resolver.cancel(); pump();
    }
    resolver.resolve("cancelled.invalid", 32123, [&](auto, auto) { ++callbacks; }, dns.server());
    resolver.cancel(); pump(); require(callbacks == 6, "cancel suppresses an already queued reserved-name result");
    {
        AsyncResolver retired(io);
        retired.resolve("retired.invalid", 32123, [&](auto, auto) { ++callbacks; }, dns.server());
    }
    pump(); require(callbacks == 6 && dns.receive().empty(), "destruction suppresses queued notification without DNS");
    for (const auto *host : {"notinvalid.capsid.test", "invalid.capsid.test", "almost-invalid.capsid.test", "missing.capsid.test"}) {
        const bool negative = std::string(host) == "missing.capsid.test";
        bool done = false; unsigned packets = 0;
        resolver.resolve(std::string(host) + ".", 32123, [&](auto error, auto endpoints) {
            require(negative ? error == asio::error::host_not_found && endpoints.empty() : !error && endpoints.size() == 2,
                    "ordinary names keep actual positive and NXDOMAIN resolution");
            done = true;
        }, dns.server());
        require(!done, "ordinary hostname is not replaced by the reserved-name fast path");
        require(wait_until([&] {
            pump();
            for (const auto &packet : dns.receive()) {
                require(DnsPeer::question(packet) == host, "exact ordinary hostname reaches the local DNS peer");
                ++packets; dns.answer(packet, negative);
            }
            return done;
        }), "controlled A/AAAA or NXDOMAIN completion");
        require(packets == 2, "both actual address-family DNS queries are observed");
        resolver.cancel(); pump();
    }
    // Keep an actual NXDOMAIN response through the complete dialer path.
    // The reserved-zone behavior must not replace this network control.
    Recorder recorder; protocol::ChannelLimits limits; limits.dial_timeout_ms = 1000;
    WebSocketDialerOptions options; options.dns_servers = dns.server();
    const auto budget = default_channel_budget();
    const auto channels = budget->channels(), dials = budget->dials(), bytes = budget->queued_bytes();
    WebSocketDialer dialer(authority(), limits, [&](Command command) { return recorder.send(std::move(command)); },
        [](size_t) {}, std::move(options));
    WorkerEvent event; event.type = WorkerEvent::Type::kChannelDial; event.dial_id = 29;
    event.channel_max_message = 1024 * 1024; event.text = "ws://stalled.capsid.test.:32123/";
    dialer.post(std::move(event)); unsigned packets = 0; std::vector<Command> commands;
    require(wait_until([&] {
        for (const auto &packet : dns.receive()) {
            require(DnsPeer::question(packet) == "stalled.capsid.test", "dialer asks the controlled DNS peer");
            ++packets; dns.answer(packet, true);
        }
        commands = recorder.take(); return !commands.empty();
    }), "real NXDOMAIN reaches the dial result");
    require(packets == 2 && commands.size() == 1 && commands[0].type == CommandType::kDialReject &&
            commands[0].dial_id == 29 && commands[0].channel_flags == 2 && commands[0].close_reason == "WebSocket DNS failed",
            "actual NXDOMAIN reports the exact DNS rejection, never a dial timeout");
    dialer.stop_and_join();
    require(recorder.take().empty() && budget->channels() == channels && budget->dials() == dials && budget->queued_bytes() == bytes,
            "DNS rejection returns its complete resource reservations");
    std::puts("reserved invalid: posted negatives without DNS, boundaries, cancellation, real A/AAAA and NXDOMAIN PASS");
}
void dial_stage(DialPhase stage, Action action) {
    Peer peer; DnsPeer dns;
    std::vector<std::unique_ptr<tcp::socket>> fillers;
    if (stage == DialPhase::kTcp) {
#ifdef __linux__
        // Linux listen(1) has two completed connection slots. Fill both;
        // the dial's next SYN cannot complete until one is accepted.
        peer.acceptor.listen(1);
        for (int i = 0; i < 2; ++i) {
            auto filler = std::make_unique<tcp::socket>(peer.io); bool done = false;
            filler->async_connect({asio::ip::address_v4::loopback(), peer.port()}, [&](boost::system::error_code error) { require(!error, "fill TCP backlog"); done = true; });
            require(wait_until([&] { peer.io.restart(); peer.io.poll(); return done; }), "TCP backlog filler connected");
            fillers.push_back(std::move(filler));
        }
#else
        std::puts("TCP stall fixture is Linux-only"); return;
#endif
    }
    Recorder recorder; protocol::ChannelLimits limits; limits.dial_timeout_ms = 1000; limits.close_timeout_ms = 200;
    WebSocketDialerOptions options; options.dns_servers = dns.server();
    options.on_phase = [&](uint64_t id, DialPhase value) { recorder.phase(id, value); };
    const auto budget = default_channel_budget();
    const auto initial_channels = budget->channels(), initial_dials = budget->dials(), initial_bytes = budget->queued_bytes();
    WebSocketDialer dialer(authority(), limits, [&](Command command) { return recorder.send(std::move(command)); },
                          [&](size_t size) { recorder.released += size; }, std::move(options));
    WorkerEvent event; event.type = WorkerEvent::Type::kChannelDial; event.dial_id = 1; event.channel_charge = 73; event.channel_max_message = 1024*1024;
    event.text = std::string(stage == DialPhase::kTls ? "wss://" : "ws://") +
        (stage == DialPhase::kDns ? "stalled.capsid.test" : "127.0.0.1") + ":" + std::to_string(peer.port()) + "/";
    dialer.post(std::move(event));
    require(wait_until([&] { return recorder.at(1, stage == DialPhase::kProvisional ? DialPhase::kHandshake : stage); }), "dial reached requested stage");
    if (stage == DialPhase::kDns) require(wait_until([&] { return !dns.receive().empty(); }), "DNS query observed before lifecycle action");
    if (stage == DialPhase::kTls || stage == DialPhase::kHandshake || stage == DialPhase::kProvisional)
        require(wait_until([&] { return peer.accept(); }), "actual TCP connection accepted");
    if (stage == DialPhase::kHandshake) require(wait_until([&] { return peer.read_head(); }), "actual HTTP handshake received");
    if (stage == DialPhase::kProvisional) {
        peer.upgrade();
        require(wait_until([&] { return recorder.at(1, stage); }), "provisional ACCEPT reached");
        auto commands = recorder.take(); require(commands.size() == 1 && commands[0].type == CommandType::kDialAccept, "exactly one ACCEPT");
        WorkerEvent ready; ready.type = WorkerEvent::Type::kChannelReady; ready.dial_id = 1; ready.channel_id = 71; dialer.post(std::move(ready));
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        require(peer.socket->available() == 0 && recorder.at(1, DialPhase::kProvisional), "no network WS IO before result ACK");
    }
    if (action == Action::Cancel) {
        WorkerEvent cancel; cancel.type = WorkerEvent::Type::kChannelCancel; cancel.dial_id = 1;
        if (stage == DialPhase::kProvisional) cancel.channel_id = 71;
        dialer.post(std::move(cancel));
        std::vector<Command> commands;
        require(wait_until([&] { commands = recorder.take(); return !commands.empty(); }), "cancel promptly acknowledged");
        require(commands.size() == 1 && commands[0].type == CommandType::kChannelConfirm && commands[0].channel_flags == 1 &&
                commands[0].channel_id == (stage == DialPhase::kProvisional ? 71U : 0U) &&
                commands[0].dial_id == (stage == DialPhase::kProvisional ? 0U : 1U), "correlated cancellation ACK, no result");
        require(budget->channels() == initial_channels && budget->dials() == initial_dials && budget->queued_bytes() == initial_bytes,
                "cancellation returns reservations before owner shutdown");
        // Keep the owner alive past the original deadline. A canceled
        // socket/query/timer must not later produce ACCEPT or REJECT.
        std::this_thread::sleep_for(std::chrono::milliseconds(1050));
        require(recorder.take().empty(), "no late result after cancel at original deadline");
    } else if (action == Action::Timeout) {
        std::vector<Command> commands;
        require(wait_until([&] { commands = recorder.take(); return !commands.empty(); }), "stage times out");
        require(commands.size() == 1 && commands[0].type == CommandType::kDialReject && commands[0].channel_flags == 7 && commands[0].dial_id == 1,
                "negotiated deadline rejects stalled stage exactly once");
        WorkerEvent ack; ack.type = WorkerEvent::Type::kChannelAck; ack.dial_id = 1; ack.ack_kind = 0; dialer.post(std::move(ack));
    }
    const auto before = Clock::now(); dialer.stop_and_join();
    require(Clock::now() - before < std::chrono::seconds(1), "DNS/TCP/TLS/handshake/provisional stop bounded");
    require(budget->channels() == initial_channels && budget->dials() == initial_dials && budget->queued_bytes() == initial_bytes,
            "stage returns all service reservations");
    require(recorder.released == 73, "owned input event charge released once");
    require(recorder.take().empty(), "no result or callback after stop");
    if (peer.socket) require(wait_until([&] { return peer.closed(); }), "real network socket closed");
    std::printf("dial phase=%d action=%d bounded cleanup PASS\n", int(stage), int(action));
}
void open_close_deadline() {
    Peer peer; Recorder recorder; protocol::ChannelLimits limits; limits.close_timeout_ms = 150;
    WebSocketDialerOptions options; options.on_phase = [&](uint64_t id, DialPhase phase) { recorder.phase(id, phase); };
    WebSocketDialer dialer(authority(), limits, [&](Command command) { return recorder.send(std::move(command)); }, [](size_t) {}, std::move(options));
    const auto budget = default_channel_budget(); const auto channels = budget->channels(), dials = budget->dials(), bytes = budget->queued_bytes();
    WorkerEvent dial; dial.type = WorkerEvent::Type::kChannelDial; dial.dial_id = 1; dial.channel_max_message = 1024*1024;
    dial.text = "ws://127.0.0.1:" + std::to_string(peer.port()) + "/"; dialer.post(std::move(dial)); peer.upgrade();
    require(wait_until([&] { return !recorder.take().empty(); }), "open test ACCEPT");
    WorkerEvent ready; ready.type = WorkerEvent::Type::kChannelReady; ready.channel_id = 1; ready.dial_id = 1; dialer.post(std::move(ready));
    WorkerEvent ack; ack.type = WorkerEvent::Type::kChannelAck; ack.channel_id = 1; ack.ack_kind = 0; dialer.post(std::move(ack));
    require(wait_until([&] { return recorder.at(1, DialPhase::kOpen); }), "result ACK starts live network session");
    WorkerEvent close; close.type = WorkerEvent::Type::kChannelClose; close.channel_id = 1; close.close_code = 1000; close.close_kind = 0; dialer.post(std::move(close));
    bool canceled = false;
    require(wait_until([&] { for (const auto &command : recorder.take()) if (command.type == CommandType::kChannelCancel) canceled = true; return canceled; }),
            "unanswered Close cancels transport at close deadline");
    require(wait_until([&] { return peer.closed(); }), "close timeout closes real socket"); dialer.stop_and_join();
    require(budget->channels() == channels && budget->dials() == dials && budget->queued_bytes() == bytes, "live session/timer/queue leases returned");
    std::puts("result ACK activation and unanswered Close deadline PASS");
}
void shared_pending_dial_budget() {
    Peer a,b;Recorder ra,rb;protocol::ChannelLimits limits;
    auto budget=std::make_shared<ServiceChannelBudget>(ChannelBudgetLimits{3,1,65536});
    WebSocketDialerOptions options;options.budget=budget;
    WebSocketDialer da(authority(),limits,[&](Command c) { return ra.send(std::move(c)); },[](size_t) {},options);
    WebSocketDialer db(authority(),limits,[&](Command c) { return rb.send(std::move(c)); },[](size_t) {},options);
    const auto dial=[&](WebSocketDialer &d,Peer &peer,uint64_t id) {
        WorkerEvent event;event.type=WorkerEvent::Type::kChannelDial;event.dial_id=id;event.channel_max_message=1024*1024;
        event.text="ws://127.0.0.1:"+std::to_string(peer.port())+"/";d.post(std::move(event));
    };
    dial(da,a,1);require(wait_until([&] { return a.read_head(); }),"first pending network handshake owns dial reservation");
    dial(db,b,2);std::vector<Command> refused;
    require(wait_until([&] { refused=rb.take();return !refused.empty(); }),"second executor refused by common pending dial ceiling");
    require(refused.size()==1 && refused[0].type==CommandType::kDialReject && refused[0].channel_flags==8 && !b.accept(),"pending dial quota refuses before TCP connect");
    WorkerEvent reject_ack;reject_ack.type=WorkerEvent::Type::kChannelAck;reject_ack.dial_id=2;db.post(std::move(reject_ack));
    WorkerEvent cancel;cancel.type=WorkerEvent::Type::kChannelCancel;cancel.dial_id=1;da.post(std::move(cancel));
    require(wait_until([&] { return budget->dials()==0 && budget->channels()==0; }),"pending cancellation restores common service dial/channel quotas");
    dial(db,b,3);require(wait_until([&] { return b.read_head(); }),"other executor can dial after capacity returns");
    da.stop_and_join();db.stop_and_join();
    require(budget->channels()==0 && budget->dials()==0 && budget->queued_bytes()==0 && budget->snapshot().dials.rejected==1,"stop returns shared resources and records one rejected dial");
    std::puts("shared pending dial quota, refusal before connect and cancel/reuse PASS");
}
}
int main(int argc, char **argv) {
    if (argc == 2 && std::string(argv[1]) == "--reserved-invalid") { reserved_invalid_lifecycle(); return 0; }
    require(argc == 1, "supported DNS lifecycle scenario");
    resolver_lifecycle();
    shared_pending_dial_budget();
    for (auto stage : {DialPhase::kDns, DialPhase::kTcp, DialPhase::kTls, DialPhase::kHandshake})
        for (auto action : {Action::Cancel, Action::Timeout, Action::Stop}) dial_stage(stage, action);
    dial_stage(DialPhase::kProvisional, Action::Cancel); dial_stage(DialPhase::kProvisional, Action::Stop);
    open_close_deadline();
}
