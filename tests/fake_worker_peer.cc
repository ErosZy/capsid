// Fake worker-side IPC peer for the channel ABI tests (GLM T07).
//
// Spawned through capsid_worker_spawn_ex with worker_path pointing at
// this binary: the client treats it as a worker process. The peer
// speaks the worker side of FetchRPC v4 from fd argv[1], drives a
// scenario from explicit test-only argv, and appends one hex line per
// received frame (type, flags, request_id, payload) to the file named
// by --test-log so the test can assert exact wire bytes. No ambient settings
// are inherited across the production spawn boundary.
#include "protocol.h"

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <cstdio>
#include <string>
#include <vector>

namespace {

int ipc_fd = -1;
std::string log_path;
FILE *log_file = nullptr;

void log_frame(const capsid::protocol::Frame &frame) {
    if (log_file == nullptr) {
        return;
    }
    std::fprintf(log_file, "type=%u flags=%u rid=%llu payload=",
                 frame.type, frame.flags,
                 static_cast<unsigned long long>(frame.request_id));
    for (size_t i = 0; i < frame.payload.size(); ++i) {
        std::fprintf(log_file, "%02x", frame.payload[i]);
    }
    std::fprintf(log_file, "\n");
    std::fflush(log_file);
}

void send_frame(uint16_t type, uint32_t flags, uint64_t request_id,
                const std::vector<uint8_t> &payload) {
    capsid::protocol::Frame frame;
    frame.type = type;
    frame.flags = flags;
    frame.request_id = request_id;
    frame.version = capsid::protocol::kVersionV4;
    frame.payload = payload;
    std::vector<uint8_t> wire;
    if (!capsid::protocol::encode_versioned(4, frame, &wire)) {
        _exit(3);
    }
    size_t written = 0;
    while (written < wire.size()) {
        const ssize_t n = write(ipc_fd, wire.data() + written,
                                wire.size() - written);
        if (n <= 0) {
            _exit(4);
        }
        written += static_cast<size_t>(n);
    }
}

void send_ready(uint32_t caps) {
    // 71-byte compatibility id stub + proof stub + capability tail;
    // the client validates the tail, not the proof, at this layer.
    std::vector<uint8_t> payload(71 + 8, 0);
    capsid::protocol::append_u32(&payload, caps);
    capsid::protocol::append_u32(&payload, 0);
    send_frame(capsid::protocol::kReady, 0, 0, payload);
}

bool read_exact(size_t count, std::vector<uint8_t> *out) {
    out->resize(count);
    size_t got = 0;
    while (got < count) {
        const ssize_t n = read(ipc_fd, out->data() + got,
                               count - got);
        if (n <= 0) {
            return false;
        }
        got += static_cast<size_t>(n);
    }
    return true;
}

// Reads one frame (blocking). Returns false on EOF/error.
bool read_frame(capsid::protocol::Frame *frame) {
    std::vector<uint8_t> header;
    if (!read_exact(capsid::protocol::kHeaderSize, &header)) {
        return false;
    }
    const uint32_t payload_size =
        capsid::protocol::load_u32_at(header.data() + 20);
    std::vector<uint8_t> payload;
    if (payload_size > 0 && !read_exact(payload_size, &payload)) {
        return false;
    }
    frame->type = capsid::protocol::load_u16_at(header.data() + 6);
    frame->flags = capsid::protocol::load_u32_at(header.data() + 8);
    frame->request_id =
        capsid::protocol::load_u32_at(header.data() + 12) |
        static_cast<uint64_t>(
            capsid::protocol::load_u32_at(header.data() + 16))
            << 32;
    frame->version = capsid::protocol::load_u16_at(header.data() + 4);
    frame->payload = payload;
    return true;
}

void send_dial(uint64_t dial_id, const char *url) {
    capsid::protocol::ChannelOpenPayload open;
    open.channel_id = 0;
    open.dial_id = dial_id;
    open.owner_kind = capsid::protocol::kOwnerStandalone;
    open.channel_kind = capsid::protocol::kChannelKindWebSocket;
    open.max_message_bytes = 1024u * 1024u;
    open.url = url;
    open.protocols.push_back("chat");
    std::vector<uint8_t> payload;
    if (!capsid::protocol::encode_channel_open(open, &payload)) {
        _exit(5);
    }
    send_frame(capsid::protocol::kChannelOpen, 0, 0, payload);
}

}  // namespace

int main(int argc, char **argv) {
    // The test's private launcher supplies fixture configuration explicitly.
    // The real spawn still supplies --ipc-fd and clears the parent environment.
    std::string early_mode;
    for (int i = 1; i + 1 < argc; ++i) {
        if (strcmp(argv[i], "--ipc-fd") == 0) ipc_fd = std::atoi(argv[++i]);
        else if (strcmp(argv[i], "--test-scenario") == 0) early_mode = argv[++i];
        else if (strcmp(argv[i], "--test-log") == 0) log_path = argv[++i];
    }
    if (ipc_fd < 0 || early_mode.empty() || log_path.empty()) return 2;
    if (std::getenv("CAPSID_FAKE_PEER_SCENARIO") || std::getenv("CAPSID_FAKE_PEER_LOG"))
        return 3;
    log_file = std::fopen(log_path.c_str(), "w");
    if (!log_file) return 4;

    capsid::protocol::Frame frame;
    const bool hello_ok =
        read_frame(&frame) && frame.type == capsid::protocol::kHello &&
        (frame.version == capsid::protocol::kVersionV4 ||
         (early_mode == "v3" &&
          frame.version == capsid::protocol::kVersion));
    if (!hello_ok) {
        _exit(6);
    }
    log_frame(frame);
    // HELLO payload: [startup][A2 ext when announced][tail]; the tail
    // carries the negotiated host capabilities.
    const uint32_t caps = capsid::protocol::load_u32_at(
        frame.payload.data() + frame.payload.size() - 8);

    const std::string mode = early_mode;
    if (mode == "v3") {
        // v3 spawn: plain READY, no capability tail
        std::vector<uint8_t> payload(71, 0);
        capsid::protocol::Frame ready;
        ready.type = capsid::protocol::kReady;
        ready.version = capsid::protocol::kVersion;
        ready.payload = payload;
        std::vector<uint8_t> wire;
        if (!capsid::protocol::encode(ready, &wire)) {
            _exit(7);
        }
        size_t written = 0;
        while (written < wire.size()) {
            const ssize_t n = write(ipc_fd, wire.data() + written,
                                    wire.size() - written);
            if (n <= 0) {
                _exit(4);
            }
            written += static_cast<size_t>(n);
        }
        while (read_frame(&frame)) {
            log_frame(frame);
        }
        return 0;
    }
    if (mode == "ready-bad-caps") {
        send_ready(1u << 9);   // unknown bit -> client protocol error
        pause();
        return 0;
    }
    if (mode == "ready-short") {
        std::vector<uint8_t> payload(5, 0);
        send_frame(capsid::protocol::kReady, 0, 0, payload);
        pause();
        return 0;
    }
    if (mode == "ready-a2-without-duplex") {
        // closure violation on the READY tail
        send_ready(capsid::protocol::kCapChannelLayoutA2);
        pause();
        return 0;
    }

    const uint32_t full = capsid::protocol::kCapChannelLayoutA2 |
                          capsid::protocol::kCapDuplexChannel |
                          capsid::protocol::kCapUpgrade |
                          capsid::protocol::kCapHalfClose;
    const bool confirm = mode == "confirm-pending" || mode == "confirm-crossed";
    send_ready(full | (confirm ? capsid::protocol::kCapChannelConfirm : 0));
    // mirror the host's own announcement so the effective set is the
    // intersection the test expects
    (void)caps;

    if (mode == "dial-flow" || mode == "queue-flow" || confirm) {
        send_dial(41, "wss://peer.test/echo");
    }
    if (mode == "confirm-pending") {
        capsid::protocol::ChannelCancelPayload cancel;
        cancel.dial_id = 41;
        std::vector<uint8_t> payload;
        if (!capsid::protocol::encode_channel_cancel(cancel, &payload)) _exit(11);
        send_frame(capsid::protocol::kChannelCancel, 0, 0, payload);
    }

    while (read_frame(&frame)) {
        log_frame(frame);
        if (mode == "confirm-crossed" && frame.type == capsid::protocol::kChannelAccept) {
            capsid::protocol::ChannelAckPayload ack;
            ack.channel_id = 1;
            ack.dial_id = 41;
            std::vector<uint8_t> payload;
            if (!capsid::protocol::encode_channel_ack(ack, &payload)) _exit(11);
            send_frame(capsid::protocol::kChannelAck, 0, 0, payload);
            payload.clear();
            capsid::protocol::append_u32(&payload, 1);
            payload.push_back('x');
            send_frame(capsid::protocol::kChannelData, 3, 0, payload);
            capsid::protocol::ChannelCancelPayload cancel;
            cancel.channel_id = 1;
            if (!capsid::protocol::encode_channel_cancel(cancel, &payload)) _exit(11);
            send_frame(capsid::protocol::kChannelCancel, 0, 0, payload);
        }
        if (mode == "confirm-crossed" && frame.type == capsid::protocol::kChannelCancel) {
            capsid::protocol::ChannelAckPayload ack;
            ack.channel_id = 1;
            ack.kind = capsid::protocol::kAckCancel;
            std::vector<uint8_t> payload;
            if (!capsid::protocol::encode_channel_ack(ack, &payload)) _exit(11);
            send_frame(capsid::protocol::kChannelAck, 0, 0, payload);
        }
        if (confirm && frame.type == capsid::protocol::kChannelAck) {
            capsid::protocol::ChannelAckPayload ack;
            std::string error;
            if (!capsid::protocol::decode_channel_ack(frame.payload.data(), frame.payload.size(), &ack, &error) ||
                    ack.kind != capsid::protocol::kAckCancel) _exit(11);
            send_dial(42, "wss://peer.test/next");
        }
        if (confirm && frame.type == capsid::protocol::kChannelReject) {
            capsid::protocol::ChannelAckPayload ack;
            ack.dial_id = 42;
            std::vector<uint8_t> payload;
            if (!capsid::protocol::encode_channel_ack(ack, &payload)) _exit(11);
            send_frame(capsid::protocol::kChannelAck, 0, 0, payload);
        }
        if ((mode == "upgrade-flow" || mode == "upgrade-reject") &&
                frame.type == capsid::protocol::kRequestHead) {
            capsid::protocol::UpgradeTail tail;
            std::string error;
            if (!capsid::protocol::decode_upgrade_head(frame, &tail, &error)) _exit(10);
            std::vector<uint8_t> response;
            capsid::protocol::append_u16(&response, mode == "upgrade-flow" ? 101 : 403);
            capsid::protocol::append_u16(&response, 0); // statusText length
            capsid::protocol::append_u16(&response, 1);
            const std::string name = "sec-websocket-protocol", value = "chat";
            capsid::protocol::append_u16(&response, name.size());
            response.insert(response.end(), name.begin(), name.end());
            capsid::protocol::append_u32(&response, value.size());
            response.insert(response.end(), value.begin(), value.end());
            if (mode == "upgrade-flow") capsid::protocol::encode_upgrade_tail(tail, &response);
            send_frame(capsid::protocol::kResponseHead,
                       mode == "upgrade-flow" ? capsid::protocol::kFlagResponseUpgrade : 0,
                       frame.request_id, response);
            if (mode == "upgrade-reject") send_frame(capsid::protocol::kResponseEnd, 0, frame.request_id, {});
        }
        if (mode == "upgrade-flow" && frame.type == capsid::protocol::kChannelData) {
            send_frame(capsid::protocol::kChannelData, frame.flags, 0, frame.payload);
        }
        if ((mode == "dial-flow" || mode == "queue-flow") &&
                frame.type == capsid::protocol::kChannelData) {
            // echo the fragment back on the same channel
            send_frame(capsid::protocol::kChannelData, frame.flags, 0,
                       frame.payload);
        }
    }
    if (log_file != nullptr) {
        std::fclose(log_file);
    }
    return 0;
}
