#include "capsid/runtime.h"
#include "protocol.h"
#include "win32_compat.h"
#include "worker_exit_status.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#if !defined(_WIN32)
#include <unistd.h>
#endif

namespace p = capsid::protocol;
namespace {
constexpr uint32_t fetch_caps = 123;
int child_descriptor = -1;
void require(bool value, const char *message) {if (!value) throw std::runtime_error(message);}
std::vector<uint8_t> hex(const std::string &text) {
    require(text.size() % 2 == 0, "even literal hex"); std::vector<uint8_t> bytes;
    for (size_t i = 0; i < text.size(); i += 2) bytes.push_back(static_cast<uint8_t>(std::stoul(text.substr(i, 2), nullptr, 16)));
    return bytes;
}
ptrdiff_t read_bytes(int fd, void *bytes, size_t size) {
#if defined(_WIN32)
    return capsid::win32::read_fd(fd, bytes, size);
#else
    return ::read(fd, bytes, size);
#endif
}
ptrdiff_t write_bytes(int fd, const void *bytes, size_t size) {
#if defined(_WIN32)
    return capsid::win32::send_fd(fd, bytes, size, 0);
#else
    return capsid::win32::send_fd(fd, bytes, size, 0);
#endif
}
struct Peer {
    int fd;
    p::Parser parser{p::kVersionV4};
    explicit Peer(int descriptor) : fd(descriptor) {}
    p::Frame next(uint16_t type) {
        p::Frame value;
        for (;;) {
            const auto result = parser.next(&value);
            if (result == p::kParseFrame) break;
            require(result == p::kParseNeedMore, "peer valid frame parser");
            uint8_t bytes[4096]; const auto count = read_bytes(fd, bytes, sizeof(bytes));
            require(count > 0 && parser.append(bytes, static_cast<size_t>(count)), "peer reads actual IPC bytes");
        }
        require(value.type == type && value.flags == 0 && value.request_id == 0, "peer exact frame type/direction fields"); return value;
    }
    void send(uint16_t type, const std::vector<uint8_t> &payload) {
        p::Frame value; value.version = p::kVersionV4; value.type = type; value.payload = payload;
        std::vector<uint8_t> wire; require(p::encode_versioned(p::kVersionV4, value, &wire), "peer wire encode");
        for (size_t at = 0; at < wire.size();) {
            const auto count = write_bytes(fd, wire.data() + at, wire.size() - at); require(count > 0, "peer writes actual IPC bytes"); at += static_cast<size_t>(count);
        }
    }
    void ack(uint32_t channel, uint64_t dial, uint32_t kind = p::kAckDialResult) {
        p::ChannelAckPayload value; value.channel_id = channel; value.dial_id = dial; value.kind = kind;
        std::vector<uint8_t> payload; require(p::encode_channel_ack(value, &payload), "peer ACK encode"); send(p::kChannelAck, payload);
    }
    int run() {
        const auto hello = next(p::kHello); require(hello.payload.size() >= 8, "peer actual HELLO tail");
        const uint32_t caps = p::load_u32_at(hello.payload.data() + hello.payload.size() - 8);
        require(caps == fetch_caps || caps == (fetch_caps & ~p::kCapFetchUpgrade), "peer explicit capabilities");
        const bool fetch = (caps & p::kCapFetchUpgrade) != 0;
        std::vector<uint8_t> ready(79, 0); p::append_u32(&ready, caps); p::append_u32(&ready, 0); send(p::kReady, ready);
        // Independent fixed-width OPEN and expected ACCEPT payloads. The
        // metadata production encoder/decoder is not the peer's oracle.
        auto open = hex("000000000100000000000000040100000000000000000000000400000000000009000000687474703a2f2f782f0000010001000100010058010079");
        if (!fetch) {open[13] = 0; open.resize(47);}
        send(p::kChannelOpen, open);
        const auto first = next(p::kChannelAccept);
        const std::string base = "0100000000000000" "02000000" "0004000000000000" "0000";
        const std::string fields = "02000a005365742d436f6f6b69650300613d310a005365742d436f6f6b69650300623dff";
        require(first.payload == hex(fetch ? base + "0100650000001300537769746368696e672050726f746f636f6c730900687474703a2f2f782f" + fields : base), "peer exact counted HTTP metadata and non-UTF8 duplicate header bytes");
        ack(2, 1);
        if (fetch) {
            require(next(p::kChannelCancel).payload == hex("0200000000000000"), "peer first channel cancellation"); ack(2, 0, p::kAckCancel);
            open[4] = 2; send(p::kChannelOpen, open);
            require(next(p::kChannelAccept).payload == hex("0200000000000000" "03000000" "0004000000000000" "0000"
                "0100990101000800436f6e666c6963740900687474703a2f2f782f" + fields), "peer exact non-101 status/final URL/redirect metadata");
            ack(3, 2);
            p::Frame body;
            for (;;) {
                const auto result = parser.next(&body);
                if (result == p::kParseFrame) break;
                require(result == p::kParseNeedMore, "peer body parser");
                uint8_t bytes[4096]; const auto count = read_bytes(fd, bytes, sizeof(bytes));
                require(count > 0 && parser.append(bytes, static_cast<size_t>(count)), "peer body read");
            }
            require(body.type == p::kChannelData && body.flags == (p::kFlagStart | p::kFlagEnd) && body.request_id == 0 &&
                body.payload.size() == 516 && p::load_u32_at(body.payload.data()) == 3, "peer binary body DATA frame");
            for (size_t i = 4; i < body.payload.size(); ++i) require(body.payload[i] == static_cast<uint8_t>((i - 4) % 251), "peer copied exact body bytes");
            require(next(p::kChannelClose).payload == hex("03000000030000000000"), "peer body EOF has no WebSocket code/reason");
            ack(3, 0, p::kAckTransportClose);
        }
        // Keep this process alive until the parent has consumed the final
        // result. Windows process teardown can reset a socket with pending
        // writes; an explicit terminal handshake avoids that fixture race.
        send(p::kLog, {'p', 'a', 's', 's'});
        require(next(p::kShutdown).payload.empty(), "parent observed results before peer exit");
        require(capsid::win32::shutdown_fd(fd) == 0, "peer orderly IPC shutdown"); return 0;
    }
};
struct Worker {
    capsid_worker *value = nullptr;
    ~Worker() {capsid_worker_destroy(value);}
    capsid_event next(capsid_event_type expected) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline) {
            const auto flushed = capsid_worker_flush(value); require(flushed == CAPSID_OK || flushed == CAPSID_WOULD_BLOCK, "ABI flush");
            capsid_event event{}; event.struct_size = sizeof(event); const auto result = capsid_worker_next_event(value, &event);
            if (result == CAPSID_OK) {
                if (event.type != expected) {
                    std::cerr << "expected event " << unsigned(expected) << ", got " << unsigned(event.type) << ": ";
                    if (event.payload.data) std::cerr.write(reinterpret_cast<const char *>(event.payload.data), static_cast<std::streamsize>(event.payload.size < 160 ? event.payload.size : 160));
                    std::cerr << '\n'; throw std::runtime_error("ABI exact expected peer event");
                }
                return event;
            }
            require(result == CAPSID_WOULD_BLOCK, "ABI IPC remains live"); std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        throw std::runtime_error("ABI event deadline");
    }
};
capsid_bytes view(const std::string &value) {return {reinterpret_cast<const uint8_t *>(value.data()), value.size()};}
void exercise(const char *executable, bool negotiated) {
    capsid_worker_config config; capsid_worker_config_init(&config); config.worker_path = executable;
    config.strict_sandbox = 0; config.max_queued_bytes = 4096;
    capsid_protocol_options options; require(capsid_protocol_options_init(&options, sizeof(options)) == CAPSID_OK, "ABI protocol options");
    options.wire_version = 4; options.capabilities = negotiated ? fetch_caps : fetch_caps & ~p::kCapFetchUpgrade;
    Worker worker; require(capsid_worker_spawn_ex(&config, &options, &worker.value) == CAPSID_OK, "ABI real peer process spawn");
    std::string status_text = "Switching Protocols", url = "http://x/", name = "Set-Cookie", first = "a=1", second("b=\xff", 3);
    capsid_header headers[2] = {{view(name), view(first)}, {view(name), view(second)}};
    capsid_fetch_upgrade_response response{}; response.struct_size = sizeof(response); response.version = CAPSID_FETCH_UPGRADE_RESPONSE_VERSION;
    response.status = 101; response.status_text = view(status_text); response.url = view(url); response.headers = headers; response.header_count = 2;
    const auto accept_at = [&](const capsid_fetch_upgrade_response *input, uint64_t dial, uint32_t channel) {
        return capsid_worker_dial_accept_http(worker.value, dial, channel, 1024, input);
    };
    const auto accept = [&](const capsid_fetch_upgrade_response *input) {return accept_at(input, 1, 2);};
    require(accept(&response) == CAPSID_WOULD_BLOCK, "HTTP result waits for actual READY"); worker.next(CAPSID_EVENT_READY);
    auto dial = worker.next(CAPSID_EVENT_CHANNEL_DIAL);
    capsid_channel_event typed{};
    require(capsid_channel_event_decode(&dial, &typed, sizeof(typed)) == (negotiated ? CAPSID_PROTOCOL_ERROR : CAPSID_OK),
        "legacy typed view cannot silently erase new fetch metadata");
    if (!negotiated) {
        require(accept(&response) == CAPSID_PROTOCOL_ERROR, "HTTP result requires negotiated capability");
        require(capsid_worker_dial_accept(worker.value, 1, 2, 1024, "") == CAPSID_OK, "legacy dial still succeeds");
        worker.next(CAPSID_EVENT_CHANNEL_ACK);
    } else {
        require(accept(nullptr) == CAPSID_INVALID_ARGUMENT, "null metadata rejected");
        for (unsigned field = 0; field < 12; ++field) {
            auto invalid = response;
            switch (field) {
            case 0: invalid.struct_size = sizeof(response) - 1; break;
            case 1: invalid.version = 99; break;
            case 2: invalid.status = 199; break;
            case 3: invalid.status = 65537; break;
            case 4: invalid.redirected = 2; break;
            case 5: invalid.url = {nullptr, 9}; break;
            case 6: invalid.url.size = 0; break;
            case 7: invalid.status_text = {nullptr, 1}; break;
            case 8: invalid.status_text.size = 1025; break;
            case 9: invalid.header_count = 129; break;
            case 10: invalid.headers = nullptr; break;
            case 11: invalid.url.size = 2049; break;
            }
            require(accept(&invalid) == CAPSID_INVALID_ARGUMENT, "malformed descriptor rejected without reading absent bytes");
        }
        require(accept_at(&response, 99, 2) == CAPSID_PROTOCOL_ERROR, "HTTP metadata cannot accept a foreign dial");
        require(capsid_worker_dial_accept(worker.value, 1, 2, 1024, "") == CAPSID_PROTOCOL_ERROR, "fetch result cannot lose HTTP metadata");
        std::string long_value(5000, 'a'); capsid_header oversized = {view(name), view(long_value)}; auto too_large = response;
        too_large.headers = &oversized; too_large.header_count = 1;
        require(accept(&too_large) == CAPSID_WOULD_BLOCK, "bounded queue refuses oversized valid metadata transactionally");
        uint8_t bad_value[] = {'x', '\n'}; capsid_header control = {view(name), {bad_value, sizeof(bad_value)}}; auto bad_header = response;
        bad_header.headers = &control; bad_header.header_count = 1; require(accept(&bad_header) == CAPSID_INVALID_ARGUMENT, "HTTP field controls rejected");
        require(accept(&response) == CAPSID_OK, "same dial/id retry succeeds after refusals");
        status_text.assign("changed"); url.assign("changed"); first.assign("changed"); second.assign("changed"); name.assign("changed");
        require(capsid_worker_channel_send(worker.value, 2, nullptr, 0, 3) == CAPSID_PROTOCOL_ERROR, "accepted result awaits actual peer ACK");
        worker.next(CAPSID_EVENT_CHANNEL_ACK);
        require(capsid_worker_channel_cancel(worker.value, 2, CAPSID_CANCEL_REASON_LOCAL) == CAPSID_OK, "first accepted fetch channel cancels");
        worker.next(CAPSID_EVENT_CHANNEL_ACK); worker.next(CAPSID_EVENT_CHANNEL_DIAL);
        status_text = "Conflict"; url = "http://x/"; name = "Set-Cookie"; first = "a=1"; second.assign("b=\xff", 3);
        headers[0] = {view(name), view(first)}; headers[1] = {view(name), view(second)};
        response.status = 409; response.redirected = 1; response.status_text = view(status_text); response.url = view(url);
        require(accept_at(&response, 2, 3) == CAPSID_OK, "ordinary HTTP response accepts a body channel"); worker.next(CAPSID_EVENT_CHANNEL_ACK);
        require(capsid_worker_channel_grant(worker.value, 3, 1) == CAPSID_PROTOCOL_ERROR, "host cannot grant a body producer in the reverse direction");
        std::vector<uint8_t> body(512); for (size_t i = 0; i < body.size(); ++i) body[i] = static_cast<uint8_t>(i % 251);
        require(capsid_worker_channel_send(worker.value, 3, body.data(), body.size(), 7) == CAPSID_PROTOCOL_ERROR, "body channel rejects text");
        require(capsid_worker_channel_send(worker.value, 3, body.data(), body.size(), 3) == CAPSID_OK, "body DATA queued"); body.assign(512, 255);
        require(capsid_worker_channel_close_ex(worker.value, 3, p::kCloseFinal, 0, "") == CAPSID_OK, "body EOF queued after DATA");
        worker.next(CAPSID_EVENT_CHANNEL_ACK);
    }
    const auto log = worker.next(CAPSID_EVENT_LOG); require(log.payload.size == 4 && std::memcmp(log.payload.data, "pass", 4) == 0, "independent peer confirmed all actual wire bytes");
    require(capsid_worker_shutdown(worker.value) == CAPSID_OK, "parent requests fixture exit after consuming results");
    worker.next(CAPSID_EVENT_EXIT);
    const auto status = capsid::destroy_worker_with_status(worker.value); worker.value = nullptr; require(status.clean(), "actual peer exits normally and is reaped exactly once");
}
}
int main(int argc, char **argv) {
    try {
#if defined(_WIN32)
        require(capsid::win32::ensure_winsock(), "test Winsock initialization");
#endif
        for (int index = 1; index + 1 < argc; ++index) if (std::strcmp(argv[index], "--ipc-fd") == 0) {
#if defined(_WIN32)
            const int fd = _open_osfhandle(static_cast<intptr_t>(std::strtoull(argv[index + 1], nullptr, 10)), _O_RDWR | _O_BINARY);
#else
            const int fd = std::atoi(argv[index + 1]);
#endif
            require(fd >= 0, "test child inherited actual IPC descriptor"); child_descriptor = fd; return Peer(fd).run();
        }
        require(argc == 1, "test accepts no scenario/environment aliases");
        require(capsid_worker_dial_accept_http(nullptr, 1, 2, 1024, nullptr) == CAPSID_INVALID_ARGUMENT, "null worker rejected");
        exercise(argv[0], false); exercise(argv[0], true);
        std::cout << "PASS: real-process HTTP result ABI, literal wire bytes, copy ownership, bounded retry, ACK and body EOF\n";
        return 0;
    } catch (const std::exception &error) {
        if (child_descriptor >= 0) {
            const std::string message = error.what();
            try {Peer(child_descriptor).send(p::kError, std::vector<uint8_t>(message.begin(), message.end()));} catch (...) {}
        }
        const char *detail = capsid_last_error();
        std::cerr << "FAIL: " << error.what() << " (" << (detail ? detail : "") << ")\n"; return 1;
    }
}
