// Channel C ABI conformance (GLM plan T07).
//
// Drives the frozen channel entry points against a FAKE raw peer
// (tests/fake_worker_peer.cc) spawned through capsid_worker_spawn_ex:
// HELLO/READY negotiation with live capability bits, typed channel
// events on the pump, exact wire bytes for every host operation (the
// peer logs each received frame), and the negative controls (bad
// READY caps, short tail, closure violation; v3 callers gated).
#include "capsid/runtime.h"

#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

int failures = 0;

void require(bool condition, const std::string &message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message.c_str());
        ++failures;
    }
}

std::string peer_path;
std::string fixture_directory;
std::vector<std::string> fixture_files;

std::string fixture_log_path(const char* path) {
    const std::string original(path);
    return fixture_directory + "/" + original.substr(original.find_last_of('/') + 1);
}

std::string shell_quote(const std::string& value) {
    std::string quoted = "'";
    for (char c : value) {
        if (c == '\'') quoted += "'\\''";
        else quoted += c;
    }
    return quoted + "'";
}

std::string peer_launcher(const char* scenario, const char* log_path) {
    const std::string path = fixture_directory + "/peer-" +
        std::to_string(fixture_files.size()) + ".sh";
    const std::string log = fixture_log_path(log_path);
    std::ofstream file(path);
    file << "#!/bin/sh\nexec " << shell_quote(peer_path)
         << " --test-scenario " << shell_quote(scenario)
         << " --test-log " << shell_quote(log) << " \"$@\"\n";
    file.close();
    require(static_cast<bool>(file) && chmod(path.c_str(), 0700) == 0,
            "create explicit peer launcher");
    fixture_files.push_back(path); fixture_files.push_back(log);
    return path;
}

std::string read_log(const char *path) {
    std::ifstream in(fixture_log_path(path));
    require(static_cast<bool>(in), "peer wire log exists for this run");
    std::stringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

capsid_worker_config base_config(const char *worker_path) {
    capsid_worker_config config;
    capsid_worker_config_init(&config);
    config.worker_path = worker_path;
    config.js_heap_limit = 64u * 1024u * 1024u;
    config.max_queued_bytes = 4u * 1024u * 1024u;
    config.max_header_bytes = 64u * 1024u;
    return config;
}

// Spawns the fake peer with full channel capabilities and a scenario.
capsid_worker *spawn_peer(const char *scenario, const char *log_path,
                          uint32_t max_queued_bytes = 4u * 1024u * 1024u,
                          bool confirm = false, bool legacy = false) {
    const std::string launcher = peer_launcher(scenario, log_path);
    capsid_worker_config config = base_config(launcher.c_str());
    config.max_queued_bytes = max_queued_bytes;
    capsid_protocol_options options;
    capsid_protocol_options_init(&options, sizeof(options));
    options.wire_version = 4;
    options.capabilities = CAPSID_PROTOCOL_CAP_CHANNEL_LAYOUT_A2 |
                           CAPSID_PROTOCOL_CAP_DUPLEX_CHANNEL |
                           CAPSID_PROTOCOL_CAP_UPGRADE |
                           CAPSID_PROTOCOL_CAP_HALF_CLOSE;
    if (confirm) options.capabilities |= CAPSID_PROTOCOL_CAP_CHANNEL_CONFIRM;
    capsid_worker *worker = nullptr;
    const capsid_result spawned = legacy
        ? capsid_worker_spawn(&config, &worker)
        : capsid_worker_spawn_ex(&config, &options, &worker);
    if (spawned != CAPSID_OK) {
        return nullptr;
    }
    return worker;
}

capsid_result wait_event(capsid_worker *worker, capsid_event *event) {
    for (int i = 0; i < 400; ++i) {
        const auto result = capsid_worker_next_event(worker, event);
        if (result != CAPSID_WOULD_BLOCK) return result;
        struct timespec pause = {0, 5 * 1000 * 1000};
        nanosleep(&pause, nullptr);
    }
    return CAPSID_WOULD_BLOCK;
}

void flush_all(capsid_worker *worker) {
    for (int i = 0; i < 400; ++i) {
        const auto result = capsid_worker_flush(worker);
        if (result == CAPSID_OK) return;
        if (result != CAPSID_WOULD_BLOCK) break;
        struct timespec pause = {0, 5 * 1000 * 1000};
        nanosleep(&pause, nullptr);
    }
    require(false, "flush completes before its deadline");
}

void test_queue_refusal_is_transactional() {
    capsid_worker *worker = spawn_peer("queue-flow", "/tmp/capsid-fake-peer-queue.log", 65560);
    require(worker != nullptr, "small-queue peer spawns");
    if (!worker) return;
    require(capsid_worker_channel_send(worker, 1, nullptr, 0,
            CAPSID_CHANNEL_FLAG_START | CAPSID_CHANNEL_FLAG_END) == CAPSID_WOULD_BLOCK,
            "channel ops wait for READY without consuming state");
    flush_all(worker);
    capsid_event event = {};
    event.struct_size = sizeof(event);
    require(wait_event(worker, &event) == CAPSID_OK && event.type == CAPSID_EVENT_READY,
            "queue peer READY");
    require(wait_event(worker, &event) == CAPSID_OK && event.type == CAPSID_EVENT_CHANNEL_DIAL,
            "queue peer DIAL");
    require(capsid_worker_dial_accept(worker, 41, 1, 1024 * 1024, "chat") == CAPSID_OK,
            "queue peer accepts first channel id");
    capsid_result grant = CAPSID_OK;
    for (unsigned i = 0; i < 4000 && grant == CAPSID_OK; ++i) {
        grant = capsid_worker_channel_grant(worker, 1, 1);
    }
    require(grant == CAPSID_WOULD_BLOCK, "control frames fill bounded queue");
    std::vector<uint8_t> message(60000, 'q');
    const uint32_t flags = CAPSID_CHANNEL_FLAG_START | CAPSID_CHANNEL_FLAG_END;
    require(capsid_worker_channel_send(worker, 1, message.data(), message.size(), flags) ==
            CAPSID_WOULD_BLOCK, "data refuses a full queue");
    flush_all(worker);
    require(capsid_worker_channel_send(worker, 1, message.data(), message.size(), flags) ==
            CAPSID_OK, "retry still has all original credit after queue refusal");
    flush_all(worker);
    require(wait_event(worker, &event) == CAPSID_OK && event.type == CAPSID_EVENT_CHANNEL_DATA,
            "retried data reaches real peer");
    capsid_channel_event decoded;
    require(capsid_channel_event_decode(&event, &decoded, sizeof(decoded)) == CAPSID_OK &&
            decoded.message.size == message.size() &&
            memcmp(decoded.message.data, message.data(), message.size()) == 0,
            "retried data is byte-exact");
    capsid_worker_destroy(worker);
}

void test_real_upgrade_head_and_lifetime() {
    for (const char *scenario : {"upgrade-flow", "upgrade-reject"}) {
        capsid_worker *worker = spawn_peer(scenario, "/tmp/capsid-fake-peer-upgrade.log");
        require(worker != nullptr, "upgrade peer spawns");
        if (!worker) continue;
        flush_all(worker);
        capsid_event event = {};
        event.struct_size = sizeof(event);
        require(wait_event(worker, &event) == CAPSID_OK && event.type == CAPSID_EVENT_READY,
                "upgrade peer READY");
        require(capsid_worker_upgrade_begin(worker, 19, 1, 1024 * 1024) == CAPSID_OK,
                "upgrade reservation succeeds");
        require(capsid_worker_channel_cancel(worker, 1, CAPSID_CANCEL_REASON_DISCONNECT) ==
                CAPSID_OK, "unannounced reservation can be cancelled");
        require(capsid_worker_upgrade_begin(worker, 20, 2, 1024 * 1024) == CAPSID_OK,
                "next upgrade reservation succeeds");
        require(capsid_worker_begin_bodyless_request(worker, 20, "GET",
                "https://peer.test/ws", nullptr, 0) == CAPSID_OK,
                "real request head queues its upgrade flag and tail");
        flush_all(worker);
        require(wait_event(worker, &event) == CAPSID_OK && event.type == CAPSID_EVENT_RESPONSE_HEAD,
                "upgrade peer response arrives");
        require(event.request_id == 20, "upgrade response preserves request id");
        size_t count = 0;
        require(capsid_response_header_count(&event, &count) == CAPSID_OK && count == 1,
                "ordinary header helpers exclude the upgrade tail");
        const bool accepted = strcmp(scenario, "upgrade-flow") == 0;
        require(event.status == (accepted ? 101u : 403u), "upgrade response status");
        const uint8_t byte = 'x';
        const auto sent = capsid_worker_channel_send(worker, 2, &byte, 1,
            CAPSID_CHANNEL_FLAG_START | CAPSID_CHANNEL_FLAG_END | CAPSID_CHANNEL_FLAG_TEXT);
        require(sent == (accepted ? CAPSID_OK : CAPSID_PROTOCOL_ERROR),
                "101 commits independent channel; non-101 releases reservation");
        if (accepted) {
            flush_all(worker);
            require(wait_event(worker, &event) == CAPSID_OK && event.type == CAPSID_EVENT_CHANNEL_DATA &&
                    event.request_id == 0, "channel event survives HTTP terminal");
        }
        if (accepted) {
            require(capsid_worker_upgrade_begin(worker, 30, 3, 1024 * 1024) == CAPSID_OK &&
                    capsid_worker_upgrade_begin(worker, 40, 4, 1024 * 1024) == CAPSID_OK,
                    "concurrent upgrade reservations are bounded independently");
            require(capsid_worker_begin_bodyless_request(worker, 40, "GET",
                    "https://peer.test/ws", nullptr, 0) == CAPSID_OK,
                    "higher reservation announces first");
            require(capsid_worker_begin_bodyless_request(worker, 30, "GET",
                    "https://peer.test/ws", nullptr, 0) == CAPSID_PROTOCOL_ERROR,
                    "late lower id is refused before queuing any wire bytes");
            require(capsid_worker_channel_cancel(worker, 3, CAPSID_CANCEL_REASON_LOCAL) == CAPSID_OK,
                    "out-of-order unannounced reservation can be reclaimed");
            flush_all(worker);
            require(wait_event(worker, &event) == CAPSID_OK && event.status == 101 && event.request_id == 40,
                    "announced reservation still commits");
        }
        const std::string log = read_log("/tmp/capsid-fake-peer-upgrade.log");
        require(log.find("type=4 flags=48 rid=20") != std::string::npos,
                "peer observed bodyless-upgrade request on the wire");
        require(log.find("type=26") == std::string::npos,
                "off-wire reservation cancellation emits no unknown-id frame");
        capsid_worker_destroy(worker);
    }
}

// Typed decoder units on fabricated events (no process needed).
void test_typed_decoder_units() {
    capsid_event event;
    memset(&event, 0, sizeof(event));
    event.struct_size = sizeof(event);
    event.type = CAPSID_EVENT_CHANNEL_CREDIT;
    // payload: channel 7, credit 4096 (A2 layout, LE)
    const uint8_t payload[] = {0x07, 0x00, 0x00, 0x00,
                               0x00, 0x10, 0x00, 0x00,
                               0x00, 0x00, 0x00, 0x00};
    event.payload.data = payload;
    event.payload.size = sizeof(payload);

    capsid_channel_event decoded;
    capsid_channel_event_init(&decoded);
    require(decoded.struct_size == (uint32_t)sizeof(decoded),
            "channel event init stamps caller size");
    require(capsid_channel_event_decode(&event, &decoded,
                                        sizeof(decoded)) == CAPSID_OK,
            "credit event decodes");
    require(decoded.kind == CAPSID_EVENT_CHANNEL_CREDIT &&
                decoded.channel_id == 7 && decoded.credit == 4096,
            "credit event fields");

    // wrong event kind is rejected
    capsid_event plain;
    memset(&plain, 0, sizeof(plain));
    plain.struct_size = sizeof(plain);
    plain.type = CAPSID_EVENT_LOG;
    require(capsid_channel_event_decode(&plain, &decoded,
                                        sizeof(decoded)) ==
                CAPSID_INVALID_ARGUMENT,
            "non-channel event rejected by the decoder");

    // undersized caller buffer is rejected (two-way negotiation)
    capsid_channel_event small;
    require(capsid_channel_event_decode(
                &event, &small,
                offsetof(capsid_channel_event, kind)) ==
                CAPSID_INVALID_ARGUMENT,
            "caller_size below the fixed prefix rejected");

    // Each supported prefix must leave all following bytes untouched.
    for (uint32_t prefix = offsetof(capsid_channel_event, kind) + 4;
         prefix < sizeof(decoded); ++prefix) {
        memset(&decoded, 0xA5, sizeof(decoded));
        require(capsid_channel_event_decode(&event, &decoded, prefix) == CAPSID_OK,
                "credit decodes into prefix");
        const uint8_t *bytes = reinterpret_cast<const uint8_t *>(&decoded);
        for (size_t i = prefix; i < sizeof(decoded); ++i) {
            require(bytes[i] == 0xA5, "typed decoder preserves caller-size canary");
        }
    }
    // The reason must borrow from the original event, never a temporary string.
    uint8_t close_payload[] = {7, 0, 0, 0, 0, 1, 0xe8, 3, 4, 0,
                               'd', 'o', 'n', 'e'};
    capsid_event close_event = event;
    close_event.type = CAPSID_EVENT_CHANNEL_CLOSE;
    close_event.payload.data = close_payload;
    close_event.payload.size = sizeof(close_payload);
    require(capsid_channel_event_decode(&close_event, &decoded, sizeof(decoded)) == CAPSID_OK,
            "close decodes");
    require(decoded.diagnostic.data == close_payload + 10 && decoded.diagnostic.size == 4,
            "close reason borrows original payload");
    close_payload[4] = 3;
    require(capsid_channel_event_decode(&close_event, &decoded, sizeof(decoded)) == CAPSID_OK &&
            decoded.close_kind == 3 && decoded.code == 1000 && decoded.diagnostic.size == 4,
            "typed final Close keeps its explicit kind and accepted code/reason");
    close_payload[4] = 4;
    require(capsid_channel_event_decode(&close_event, &decoded, sizeof(decoded)) == CAPSID_PROTOCOL_ERROR,
            "typed decoder still rejects an unknown Close kind");

    const uint8_t cancel_payload[] = {7, 0, 0, 0, 2, 0, 0, 0};
    capsid_event cancelled = event;
    cancelled.type = CAPSID_EVENT_CHANNEL_CANCEL;
    cancelled.payload.data = cancel_payload;
    cancelled.payload.size = sizeof(cancel_payload);
    uint32_t terminal_reason = 99;
    require(capsid_channel_event_decode(&cancelled, &decoded, sizeof(decoded)) == CAPSID_OK &&
            decoded.channel_id == 7,
            "assigned channel cancellation decodes its id");
    require(capsid_channel_cancel_reason(&cancelled, &terminal_reason) == CAPSID_OK &&
            terminal_reason == CAPSID_CANCEL_REASON_TIMEOUT, "typed cancellation reason getter");

    const uint8_t pending_payload[] = {0, 0, 0, 0, 0, 0, 0, 0, 7, 0, 0, 0, 1, 0, 0, 0};
    cancelled.payload.data = pending_payload;
    cancelled.payload.size = sizeof(pending_payload);
    require(capsid_channel_event_decode(&cancelled, &decoded, sizeof(decoded)) == CAPSID_OK &&
            decoded.channel_id == 0 && decoded.dial_id == 4294967303ULL,
            "pending cancellation keeps full u64 dial identity");
    const uint8_t ack_payload[] = {7, 0, 0, 0, 7, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0};
    capsid_event acknowledged = event;
    acknowledged.type = CAPSID_EVENT_CHANNEL_ACK;
    acknowledged.payload.data = ack_payload;
    acknowledged.payload.size = sizeof(ack_payload);
    require(capsid_channel_event_decode(&acknowledged, &decoded, sizeof(decoded)) == CAPSID_OK &&
            decoded.channel_id == 7 && decoded.dial_id == 4294967303ULL,
            "confirmation typed view uses existing id fields without layout growth");
    for (uint32_t prefix = offsetof(capsid_channel_event, kind) + 4; prefix < sizeof(decoded); ++prefix) {
        memset(&decoded, 0xA5, sizeof(decoded));
        require(capsid_channel_event_decode(&acknowledged, &decoded, prefix) == CAPSID_OK,
                "ACK decodes into caller prefix");
        const auto *bytes = reinterpret_cast<const uint8_t *>(&decoded);
        for (size_t i = prefix; i < sizeof(decoded); ++i)
            require(bytes[i] == 0xA5, "ACK preserves every caller-size canary");
    }
    uint32_t ack_kind = 99;
    acknowledged.payload.size = sizeof(ack_payload) - 1;
    require(capsid_channel_ack_kind(&acknowledged, &ack_kind) == CAPSID_PROTOCOL_ERROR && ack_kind == 99,
            "malformed ACK leaves scalar output untouched");

    // malformed payload is a protocol error, not a crash
    capsid_event broken;
    memset(&broken, 0, sizeof(broken));
    broken.struct_size = sizeof(broken);
    broken.type = CAPSID_EVENT_CHANNEL_CREDIT;
    const uint8_t junk[] = {0x01};
    broken.payload.data = junk;
    broken.payload.size = sizeof(junk);
    require(capsid_channel_event_decode(&broken, &decoded,
                                        sizeof(decoded)) ==
                CAPSID_PROTOCOL_ERROR,
            "truncated credit payload rejected");
}

void test_v3_caller_gated() {
    // A plain v3 spawn (legacy entry) never negotiates channels: the
    // ops answer CAPSID_PROTOCOL_ERROR and the pump can never emit a
    // channel event (the v3 parser rejects channel frame types).
    capsid_worker *worker = spawn_peer("v3", "/tmp/capsid-fake-peer-v3.log",
                                       4u * 1024u * 1024u, false, true);
    require(worker != nullptr, "v3 spawn of the fake peer");
    if (worker == nullptr) {
        return;
    }
    require(capsid_worker_flush(worker) == CAPSID_OK,
            "flush the v3 HELLO");
    capsid_event event;
    memset(&event, 0, sizeof(event));
    event.struct_size = sizeof(event);
    {
        capsid_result pumped = CAPSID_WOULD_BLOCK;
        for (int attempt = 0; attempt < 400 &&
                 pumped == CAPSID_WOULD_BLOCK;
             ++attempt) {
            pumped = capsid_worker_next_event(worker, &event);
            if (pumped == CAPSID_WOULD_BLOCK) {
                struct timespec pause = {0, 5 * 1000 * 1000};
                nanosleep(&pause, NULL);
            }
        }
        require(pumped == CAPSID_OK && event.type == CAPSID_EVENT_READY,
                "v3 READY arrives");
    }
    const uint8_t byte = 0x41;
    require(capsid_worker_channel_send(worker, 1, &byte, 1,
                                       CAPSID_CHANNEL_FLAG_START |
                                           CAPSID_CHANNEL_FLAG_END |
                                           CAPSID_CHANNEL_FLAG_TEXT) ==
                CAPSID_PROTOCOL_ERROR,
            "v3 channel send is gated");
    require(capsid_worker_channel_grant(worker, 1, 1024) ==
                CAPSID_PROTOCOL_ERROR,
            "v3 grant is gated");
    require(capsid_worker_channel_close(worker, 1, 1000, "x") ==
                CAPSID_PROTOCOL_ERROR,
            "v3 close is gated");
    capsid_protocol_info info;
    require(capsid_worker_get_protocol_info(worker, &info,
                                            sizeof(info)) == CAPSID_OK,
            "protocol info after v3 spawn");
    require(info.wire_version == 3 &&
                info.effective_capabilities == 0,
            "v3 info carries no capabilities");
    capsid_worker_destroy(worker);
}

bool logged_find(const std::string &haystack, const char *prefix,
                const std::string &payload_hex) {
    const std::string needle =
        std::string(prefix) + " flags=0 rid=0 payload=" + payload_hex;
    return haystack.find(needle) != std::string::npos;
}

void test_negotiation_and_dial_flow() {
    const char *log = "/tmp/capsid-fake-peer-dial.log";
    std::remove(fixture_log_path(log).c_str());
    capsid_worker *worker = spawn_peer("dial-flow", log);
    require(worker != nullptr, "spawn_ex with channel caps");
    // the HELLO is queued at spawn: flush drives it onto the socket
    require(capsid_worker_flush(worker) == CAPSID_OK, "flush HELLO");

    // READY arrives with the worker's full bitset
    capsid_event event;
    memset(&event, 0, sizeof(event));
    event.struct_size = sizeof(event);
    {
        // the pump is non-blocking: retry on WOULD_BLOCK with a
        // bounded deadline (the peer is a real process on the other
        // end of the socketpair)
        capsid_result pumped = CAPSID_WOULD_BLOCK;
        for (int attempt = 0; attempt < 400 &&
                 pumped == CAPSID_WOULD_BLOCK;
             ++attempt) {
            pumped = capsid_worker_next_event(worker, &event);
            if (pumped == CAPSID_WOULD_BLOCK) {
                struct timespec pause = {0, 5 * 1000 * 1000};
                nanosleep(&pause, NULL);
            }
        }
        require(pumped == CAPSID_OK &&
                    event.type == CAPSID_EVENT_READY,
                "READY event arrives");
    }
    capsid_protocol_info info;
    require(capsid_worker_get_protocol_info(worker, &info,
                                            sizeof(info)) == CAPSID_OK,
            "protocol info succeeds after READY");
    require(info.wire_version == 4 &&
                (info.host_capabilities & 0xf) == 0xf &&
                (info.worker_capabilities & 0xf) == 0xf &&
                (info.effective_capabilities & 0xf) == 0xf,
            "negotiated intersection is FULL");

    // the peer dials; the typed DIAL event decodes
    {
        capsid_result pumped = CAPSID_WOULD_BLOCK;
        for (int attempt = 0; attempt < 400 &&
                 pumped == CAPSID_WOULD_BLOCK;
             ++attempt) {
            pumped = capsid_worker_next_event(worker, &event);
            if (pumped == CAPSID_WOULD_BLOCK) {
                struct timespec pause = {0, 5 * 1000 * 1000};
                nanosleep(&pause, NULL);
            }
        }
        require(pumped == CAPSID_OK &&
                    event.type == CAPSID_EVENT_CHANNEL_DIAL,
                "DIAL event arrives");
    }
    capsid_channel_event dial;
    capsid_channel_event_init(&dial);
    require(capsid_channel_event_decode(&event, &dial,
                                        sizeof(dial)) == CAPSID_OK,
            "DIAL decodes");
    require(dial.dial_id == 41 && dial.url.size == 20 &&
                std::string(reinterpret_cast<const char *>(
                                dial.url.data),
                            dial.url.size) ==
                    "wss://peer.test/echo",
            "DIAL carries dial id and borrowed url");

    // accept the dial; the peer must see the exact ACCEPT wire bytes
    require(capsid_worker_dial_accept(worker, 41, 7,
                                      1024u * 1024u, "chat") ==
                CAPSID_OK,
            "dial accept queues");

    // send a text fragment; the peer echoes it back
    const char *message = "hello-peer";
    require(capsid_worker_channel_send(
                worker, 7,
                reinterpret_cast<const uint8_t *>(message),
                std::string(message).size(),
                CAPSID_CHANNEL_FLAG_START |
                    CAPSID_CHANNEL_FLAG_END |
                    CAPSID_CHANNEL_FLAG_TEXT) == CAPSID_OK,
            "channel send queues");
    require(capsid_worker_flush(worker) == CAPSID_OK,
            "flush after send");
    {
        capsid_result pumped = CAPSID_WOULD_BLOCK;
        for (int attempt = 0; attempt < 400 &&
                 pumped == CAPSID_WOULD_BLOCK;
             ++attempt) {
            pumped = capsid_worker_next_event(worker, &event);
            if (pumped == CAPSID_WOULD_BLOCK) {
                struct timespec pause = {0, 5 * 1000 * 1000};
                nanosleep(&pause, NULL);
            }
        }
        require(pumped == CAPSID_OK &&
                    event.type == CAPSID_EVENT_CHANNEL_DATA,
                "echo DATA arrives");
    }
    capsid_channel_event data_event;
    capsid_channel_event_init(&data_event);
    require(capsid_channel_event_decode(&event, &data_event,
                                        sizeof(data_event)) ==
                CAPSID_OK,
            "DATA decodes");
    require(data_event.channel_id == 7 &&
                data_event.message.size ==
                    std::string(message).size() &&
                std::string(reinterpret_cast<const char *>(
                                data_event.message.data),
                            data_event.message.size) == message,
            "borrowed echo bytes match");

    // grant + close + cancel ordering: close then cancel is illegal on
    // the same channel, so verify grant and close here
    require(capsid_worker_channel_grant(worker, 7, 4096) == CAPSID_OK,
            "grant queues");
    require(capsid_worker_channel_close(worker, 7, 1000, "done") ==
                CAPSID_OK,
            "close queues");
    require(capsid_worker_flush(worker) == CAPSID_OK,
            "flush after grant+close");
    // the peer echoes nothing for close (scenario only echoes DATA);
    // a second close from the host is a sequencing violation
    require(capsid_worker_channel_close(worker, 7, 1000, "again") ==
                CAPSID_PROTOCOL_ERROR,
            "duplicate host close rejected by the registry");
    capsid_worker_destroy(worker);

    // exact wire assertions from the peer log: the ACCEPT frame the
    // host queued must carry dial 41 -> channel 7 at 1 MiB with the
    // selected protocol; the DATA frame the host sent must carry
    // channel 7 + the message bytes; the CLOSE frame kind 0 code 1000
    const std::string logged = read_log(log);
    // dial_id 41, channel 7, cap 1MiB (LE), selected "chat"
    require(logged_find(logged, "type=20",
                        "2900000000000000" "07000000"
                        "0000100000000000" "0400" "63686174"),
            "ACCEPT wire bytes exact");
    require(logged.find(
                "type=22 flags=7 rid=0 payload="
                "07000000" "68656c6c6f2d70656572") !=
                std::string::npos,
            "DATA wire bytes exact (start|end|text)");
    require(logged.find(
                "type=25 flags=0 rid=0 payload="
                "07000000" "00" "01" "e803" "0400" "646f6e65") !=
                std::string::npos,
            "CLOSE wire bytes exact (initiated, 1000, \"done\")");
    require(logged.find("type=23 flags=0 rid=0 payload="
                        "07000000" "0010000000000000") !=
                std::string::npos,
            "CREDIT wire bytes exact (4096)");
}

void test_negative_controls() {
    struct Case {
        const char *scenario;
        const char *label;
    } cases[] = {
        {"ready-bad-caps", "unknown READY capability bit"},
        {"ready-short", "short READY tail"},
        {"ready-a2-without-duplex", "READY closure violation"},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        capsid_worker *worker =
            spawn_peer(cases[i].scenario,
                       "/tmp/capsid-fake-peer-neg.log");
        require(worker != nullptr,
                std::string("spawn: ") + cases[i].label);
        if (worker == nullptr) {
            continue;
        }
        require(capsid_worker_flush(worker) == CAPSID_OK,
                std::string("flush: ") + cases[i].label);
        capsid_event event;
        memset(&event, 0, sizeof(event));
        event.struct_size = sizeof(event);
        capsid_result result = CAPSID_WOULD_BLOCK;
        for (int attempt = 0; attempt < 400 &&
                 result == CAPSID_WOULD_BLOCK;
             ++attempt) {
            result = capsid_worker_next_event(worker, &event);
            if (result == CAPSID_WOULD_BLOCK) {
                struct timespec pause = {0, 5 * 1000 * 1000};
                nanosleep(&pause, NULL);
            }
        }
        require(result == CAPSID_PROTOCOL_ERROR,
                std::string("negative control fails the pump: ") +
                    cases[i].label);
        capsid_worker_destroy(worker);
    }
}

void test_confirmation_races_on_real_ipc() {
    for (const char *scenario : {"confirm-pending", "confirm-crossed"}) {
        capsid_worker *worker = spawn_peer(scenario, "/tmp/capsid-fake-peer-confirm.log", 65560, true);
        require(worker != nullptr, "confirmation peer spawns");
        if (!worker) continue;
        flush_all(worker);
        capsid_event event = {};
        event.struct_size = sizeof(event);
        require(wait_event(worker, &event) == CAPSID_OK && event.type == CAPSID_EVENT_READY, "confirmation READY");
        require(wait_event(worker, &event) == CAPSID_OK && event.type == CAPSID_EVENT_CHANNEL_DIAL, "confirmation DIAL");
        require(capsid_worker_dial_accept(worker, 41, 1, 1024 * 1024, "") == CAPSID_OK, "provisional ACCEPT queued");
        require(capsid_worker_channel_send(worker, 1, nullptr, 0, 3) == CAPSID_PROTOCOL_ERROR,
                "ABI prevents DATA before result confirmation");
        flush_all(worker);
        const bool pending = strcmp(scenario, "confirm-pending") == 0;
        if (!pending) {
            require(wait_event(worker, &event) == CAPSID_OK && event.type == CAPSID_EVENT_CHANNEL_ACK,
                    "result confirmation is observable");
            uint32_t kind = 99;
            require(capsid_channel_ack_kind(&event, &kind) == CAPSID_OK && kind == CAPSID_CHANNEL_ACK_DIAL_RESULT,
                    "result ACK scalar kind");
            require(capsid_worker_channel_cancel(worker, 1, 0) == CAPSID_OK, "host CANCEL crosses queued worker DATA/CANCEL");
            flush_all(worker);
        }
        require(wait_event(worker, &event) == CAPSID_OK && event.type == CAPSID_EVENT_CHANNEL_CANCEL,
                "pump suppresses crossed DATA but preserves peer CANCEL");
        capsid_channel_event decoded;
        capsid_channel_event_init(&decoded);
        require(capsid_channel_event_decode(&event, &decoded, sizeof(decoded)) == CAPSID_OK &&
                decoded.channel_id == (pending ? 0u : 1u) && decoded.dial_id == (pending ? 41u : 0u),
                "typed CANCEL distinguishes pending dial from assigned channel");
        if (pending) {
            // One maximum-sized ordinary head fills the IPC writer while
            // the cancellation correlation is still held. A refused ACK
            // must remain retryable after the writer drains.
            const std::string url = "https://" + std::string(65525 - 8, 'q');
            require(capsid_worker_begin_bodyless_request(worker, 99, "GET", url.c_str(), nullptr, 0) == CAPSID_OK,
                    "ordinary carriage fills bounded writer");
            require(capsid_worker_channel_confirm(worker, 0, 41, CAPSID_CHANNEL_ACK_CANCEL) == CAPSID_WOULD_BLOCK,
                    "terminal ACK refusal consumes no cancellation state");
            flush_all(worker);
        }
        require(capsid_worker_channel_confirm(worker, pending ? 0 : 1, pending ? 41 : 0,
                CAPSID_CHANNEL_ACK_CANCEL) == CAPSID_OK, "producer stopped, terminal ACK queued");
        flush_all(worker);
        if (!pending) {
            require(wait_event(worker, &event) == CAPSID_OK && event.type == CAPSID_EVENT_CHANNEL_ACK,
                    "both crossed CANCELs receive a terminal acknowledgement");
        }
        require(capsid_worker_channel_send(worker, 1, nullptr, 0, 3) == CAPSID_PROTOCOL_ERROR,
                "cancelled channel cannot revive after confirmation");
        require(wait_event(worker, &event) == CAPSID_OK && event.type == CAPSID_EVENT_CHANNEL_DIAL,
                "unrelated subsequent dial survives on same IPC stream");
        require(capsid_worker_dial_reject(worker, 42, 8, "limits") == CAPSID_OK, "subsequent dial can reject");
        flush_all(worker);
        require(wait_event(worker, &event) == CAPSID_OK && event.type == CAPSID_EVENT_CHANNEL_ACK,
                "rejection correlation is confirmed and reclaimed");
        uint32_t kind = 99;
        require(capsid_channel_ack_kind(&event, &kind) == CAPSID_OK && kind == CAPSID_CHANNEL_ACK_DIAL_RESULT,
                "REJECT uses the same result barrier");
        capsid_worker_destroy(worker);
    }
}

}  // namespace

int main(int argc, char **argv) {
    if (argc > 1) {
        peer_path = argv[1];
    } else {
        peer_path = CAPSID_FAKE_PEER_PATH;
    }
    if (peer_path.empty()) {
        std::fprintf(stderr, "fake peer path missing\n");
        return 2;
    }
    char directory[] = "/tmp/capsid-channel-abi-XXXXXX";
    if (!mkdtemp(directory)) { std::perror("fixture directory"); return 2; }
    fixture_directory = directory;
    // These ambient values must neither select the peer mode nor its log.
    // The production spawn's empty environment remains part of the test.
    setenv("CAPSID_FAKE_PEER_SCENARIO", "ambient-must-not-control-fixture", 1);
    setenv("CAPSID_FAKE_PEER_LOG", "/ambient-must-not-be-written", 1);
    require(capsid_worker_channel_send(nullptr, 1, nullptr, 0, 0) == CAPSID_INVALID_ARGUMENT,
            "null worker is an argument error");
    test_typed_decoder_units();
    test_queue_refusal_is_transactional();
    test_real_upgrade_head_and_lifetime();
    test_v3_caller_gated();
    test_negotiation_and_dial_flow();
    test_negative_controls();
    test_confirmation_races_on_real_ipc();
    if (failures > 0) {
        std::fprintf(stderr, "%d failure(s); fixture logs: %s\n", failures,
                     fixture_directory.c_str());
        return 1;
    }
    for (const auto& path : fixture_files) unlink(path.c_str());
    require(rmdir(fixture_directory.c_str()) == 0, "fixture cleanup");
    if (failures) return 1;
    std::printf("channel ABI: typed events + fake-peer negotiation "
                "PASS\n");
    return 0;
}
