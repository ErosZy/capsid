// Real C ABI response-body limit transport and one-slot retirement probe.
#include "capsid/runtime.h"
#include "response_credit_probe.h"
#include "client_channel_state.h"
#include "protocol.h"
#include "win32_compat.h"
#include "graceful_worker_exit.h"
#include "worker_exit_status.h"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void check(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
void ok(capsid_result value, const char* message) { check(value == CAPSID_OK, message); }
struct Peer {
    capsid_worker* worker = nullptr;
    capsid_test::ResponseCreditProbe response_credit;
    ~Peer() { capsid_worker_destroy(worker); }
    capsid_event next() {
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        for (;;) {
            const auto flushed = capsid_worker_flush(worker);
            check(flushed == CAPSID_OK || flushed == CAPSID_WOULD_BLOCK, "flush");
            capsid_event event{};
            event.struct_size = sizeof(event);
            const auto result = capsid_worker_next_event(worker, &event);
            if (result == CAPSID_OK) {
                if (event.type == CAPSID_EVENT_ERROR)
                    std::fprintf(stderr, "worker error: %.*s\n", int(event.payload.size),
                                 event.payload.data);
                check(event.type != CAPSID_EVENT_ERROR && event.type != CAPSID_EVENT_EXIT,
                      "worker remains healthy");
                return event;
            }
            check(result == CAPSID_WOULD_BLOCK && std::chrono::steady_clock::now() < end,
                  "event deadline");
            capsid_pollfd fd{};
            fd.fd = capsid_worker_fd(worker);
            fd.events = POLLIN | (flushed == CAPSID_WOULD_BLOCK ? POLLOUT : 0);
            capsid::win32::capsid_poll(&fd, 1, 10);
        }
    }
};
struct Case {
    const char* method;
    const char* route;
    bool known;
    size_t length;
    bool fragmented;
    const char* action;
};
// RFC1952 gzip with stored DEFLATE blocks. Each transport fragment ends
// at a byte-aligned block boundary; the last fragment owns CRC32 and ISIZE.
// Node's separate zlib decoder verifies every full wire vector before use.
std::vector<std::vector<uint8_t>> gzip_segments(size_t length, bool fragmented) {
    std::vector<size_t> sizes;
    if (!length) sizes = {0};
    else if (fragmented) sizes = {2, 3, length - 5};
    else sizes = {length};
    std::vector<std::vector<uint8_t>> segments;
    uint32_t crc = 0xffffffffu;
    size_t position = 0;
    for (size_t part = 0; part < sizes.size(); ++part) {
        std::vector<uint8_t> bytes;
        if (!part) bytes = {31, 139, 8, 0, 0, 0, 0, 0, 0, 3};
        const auto count = static_cast<uint16_t>(sizes[part]);
        if (count) {
            const auto complement = static_cast<uint16_t>(count ^ 0xffffu);
            bytes.insert(bytes.end(), {0, static_cast<uint8_t>(count), static_cast<uint8_t>(count >> 8),
                static_cast<uint8_t>(complement), static_cast<uint8_t>(complement >> 8)});
            for (size_t index = 0; index < count; ++index) {
                const auto byte = static_cast<uint8_t>(++position);
                bytes.push_back(byte); crc ^= byte;
                for (unsigned bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ ((crc & 1u) ? 0xedb88320u : 0);
            }
        }
        if (part + 1 == sizes.size()) {
            bytes.insert(bytes.end(), {1, 0, 0, 255, 255});
            const auto checksum = crc ^ 0xffffffffu;
            for (unsigned shift = 0; shift < 32; shift += 8) bytes.push_back(static_cast<uint8_t>(checksum >> shift));
            for (unsigned shift = 0; shift < 32; shift += 8) bytes.push_back(static_cast<uint8_t>(length >> shift));
        }
        segments.push_back(std::move(bytes));
    }
    return segments;
}

void exercise(const char* path, uint64_t limit, const std::string& bundle, bool encoded_headers, bool small_window) {
    capsid_worker_config config;
    capsid_worker_config_init(&config);
    config.worker_path = path;
    config.max_fetch_response_body_bytes = limit;
    if (small_window) config.initial_stream_window = 256;
    capsid_egress_rule rule;
    capsid_egress_rule_init(&rule);
    rule.action = CAPSID_EGRESS_ALLOW;
    rule.target = "example.test";
    capsid_egress_policy policy;
    capsid_egress_policy_init(&policy);
    policy.rules = &rule;
    policy.rule_count = 1;
    config.egress_policy = &policy;
    capsid_protocol_options options;
    capsid_protocol_options_init(&options, sizeof(options));
    options.wire_version = 4;
    options.capabilities = 123;
    options.max_channels = 1;
    Peer peer;
    peer.response_credit = capsid_test::ResponseCreditProbe(small_window ? 256 : 0, limit);
    ok(capsid_worker_spawn_ex(&config, &options, &peer.worker), "actual configured-limit spawn");
    ok(capsid_worker_load_bundle(peer.worker, reinterpret_cast<const uint8_t*>(bundle.data()),
                                 bundle.size()),
       "load");
    check(peer.next().type == CAPSID_EVENT_READY, "READY");
    std::vector<Case> cases;
    for (const char* method : {"default", "byob", "least", "high"})
        for (const char* route : {"getReader", "direct"})
            for (const bool known : {true, false}) {
                for (const size_t length : {0u, 7u, 8u, 9u})
                    for (const bool fragmented : {false, true})
                        cases.push_back({method, route, known, length, fragmented, "full"});
                for (const char* action : {"cancel", "pending"})
                    cases.push_back({method, route, known, 7, true, action});
            }
    check(cases.size() == 160, "independent complete ABI limit inventory");
    uint64_t request = 0;
    uint32_t channel = 0;
    for (const auto& scenario : cases) {
        ++request;
        ++channel;
        const std::string query =
            std::string("https://app.test/?method=") + scenario.method +
            "&route=" + scenario.route + "&known=" + (scenario.known ? "1" : "0") +
            "&length=" + std::to_string(scenario.length) + "&action=" + scenario.action +
            (encoded_headers && scenario.known ? "&encoding=1" : "");
        ok(capsid_worker_begin_bodyless_request(peer.worker, request, "GET", query.c_str(), nullptr,
                                                0),
           "request");
        auto event = peer.next();
        check(event.type == CAPSID_EVENT_CHANNEL_DIAL, "real fetch OPEN");
        capsid::protocol::ChannelOpenPayload dial;
        std::string why;
        check(capsid::protocol::decode_channel_open(event.payload.data, event.payload.size, &dial,
                                                    &why, 123),
              "negotiated fetch OPEN decode");
        const bool encoded = encoded_headers && scenario.known;
        const bool full = std::strcmp(scenario.action, "full") == 0;
        std::vector<std::vector<uint8_t>> fragments;
        if (encoded) fragments = gzip_segments(scenario.length, scenario.fragmented);
        else {
            std::vector<uint8_t> plain(full ? scenario.length : 5);
            for (size_t index = 0; index < plain.size(); ++index) plain[index] = static_cast<uint8_t>(index + 1);
            if (!scenario.fragmented || plain.empty()) fragments.push_back(plain);
            else {
                fragments.emplace_back(plain.begin(), plain.begin() + 2);
                fragments.emplace_back(plain.begin() + 2, plain.begin() + 5);
                if (plain.size() > 5) fragments.emplace_back(plain.begin() + 5, plain.end());
            }
        }
        size_t wire_length = 0;
        for (const auto& fragment : fragments) wire_length += fragment.size();
        const std::string size = std::to_string(encoded ? wire_length : scenario.length), name = "Content-Length",
                          status = "OK", target = "https://example.test/body";
        capsid_header header{{reinterpret_cast<const uint8_t*>(name.data()), name.size()},
                             {reinterpret_cast<const uint8_t*>(size.data()), size.size()}};
        const std::string encoding_name = "Content-Encoding", encoding_value = "gzip";
        capsid_header headers[2] = {header,
            {{reinterpret_cast<const uint8_t*>(encoding_name.data()), encoding_name.size()},
             {reinterpret_cast<const uint8_t*>(encoding_value.data()), encoding_value.size()}}};
        capsid_fetch_upgrade_response response{};
        response.struct_size = sizeof(response);
        response.version = CAPSID_FETCH_UPGRADE_RESPONSE_VERSION;
        response.status = 200;
        response.status_text = {reinterpret_cast<const uint8_t*>(status.data()), status.size()};
        response.url = {reinterpret_cast<const uint8_t*>(target.data()), target.size()};
        if (scenario.known) {
            response.headers = headers;
            response.header_count = encoded_headers ? 2 : 1;
        }
        ok(capsid_worker_dial_accept_http(peer.worker, dial.dial_id, channel, 65536, &response),
           "actual native HTTP body metadata");
        event = peer.next();
        check(event.type == CAPSID_EVENT_CHANNEL_ACK, "actual fetch result ACK");
        const size_t fragment_count = encoded && !full ? 2 : fragments.size();
        size_t sent = 0;
        uint64_t credits = 0;
        for (size_t index = 0; index < fragment_count; ++index) {
            // A transport frame is not itself a decoded read boundary. Hold
            // each full-body gzip fragment until native consumption returns
            // the preceding input's real credit. This creates a source read
            // opportunity without assuming TCP or IPC packet boundaries.
            if (encoded && full && scenario.fragmented && index) {
                while (credits < sent) {
                    event = peer.next();
                    check(event.type == CAPSID_EVENT_CHANNEL_CREDIT, "encoded fragment waits for actual native input credit");
                    capsid_channel_event marker;
                    capsid_channel_event_init(&marker);
                    ok(capsid_channel_event_decode(&event, &marker, sizeof(marker)), "gzip source gate typed credit");
                    check(marker.channel_id == channel && marker.credit <= sent - credits, "gzip gate has exact channel and byte authority");
                    credits += marker.credit;
                }
            }
            const auto& fragment = fragments[index];
            if (fragment.empty()) continue;
            sent += fragment.size();
            ok(capsid_worker_channel_send(peer.worker, channel, fragment.data(), fragment.size(), 3),
               "real raw or gzip transport fragment");
        }
        if (full)
            ok(capsid_worker_channel_close_ex(peer.worker, channel, 3, 0, ""), "body EOF");
        bool head = false, ended = false;
        std::string text;
        unsigned cancels = 0;
        for (;;) {
            capsid::ChannelEntry entry;
            if (ended && !capsid::client_channel_snapshot(peer.worker, channel, &entry))
                break;
            event = peer.next();
            if (event.type == CAPSID_EVENT_CHANNEL_CANCEL) {
                capsid_channel_event view;
                capsid_channel_event_init(&view);
                ok(capsid_channel_event_decode(&event, &view, sizeof(view)), "cancel decode");
                check(view.channel_id == channel, "cancel owner");
                ok(capsid_worker_channel_confirm(peer.worker, channel, 0,
                                                 CAPSID_CHANNEL_ACK_CANCEL),
                   "stopped producer terminal ACK");
                ++cancels;
            } else if (event.type == CAPSID_EVENT_CHANNEL_ACK ||
                       event.type == CAPSID_EVENT_CHANNEL_CREDIT) {
                capsid_channel_event view;
                capsid_channel_event_init(&view);
                ok(capsid_channel_event_decode(&event, &view, sizeof(view)), "typed marker");
                check(view.channel_id == channel, "marker owner");
                if (event.type == CAPSID_EVENT_CHANNEL_CREDIT) {
                    check(view.credit <= sent - credits,
                          "credit cannot exceed actual source bytes");
                    credits += view.credit;
                }
            } else {
                check(event.request_id == request, "HTTP response owner");
                if (event.type == CAPSID_EVENT_RESPONSE_HEAD) {
                    check(!head && event.status == 200, "one observation head");
                    head = true;
                } else if (event.type == CAPSID_EVENT_RESPONSE_BODY) {
                    check(head && !ended, "response order");
                    text.append(reinterpret_cast<const char*>(event.payload.data),
                                event.payload.size);
                    peer.response_credit.consume(peer.worker, event);
                } else if (event.type == CAPSID_EVENT_RESPONSE_END) {
                    check(head && !ended, "complete response");
                    ended = true;
                } else
                    check(false, "no unexpected event");
            }
        }
        peer.response_credit.complete(request, text);
        check(cancels <= 1, "one cancellation marker");
        if (!full)
            check(cancels == 1, "held transport stops before cancellation ACK");
        std::printf("LIMIT_ROW "
                    "{\"limit\":%llu,\"fragmented\":%s,\"channel\":%u,\"sent\":%zu,"
                    "\"credit\":%llu,\"cancels\":%u,\"live\":false,\"observation\":%s}\n",
                    static_cast<unsigned long long>(limit), scenario.fragmented ? "true" : "false",
                    static_cast<unsigned>(channel), sent, static_cast<unsigned long long>(credits),
                    cancels, text.c_str());
    }
    // destroy is abortive cleanup. Observe graceful shutdown and the real
    // process exit before giving that cleanup function ownership of the peer.
    const auto exited = capsid_test::shutdown_and_wait(peer.worker, 2500);
    const auto cleanup = capsid::destroy_worker_with_status(peer.worker);
    peer.worker = nullptr;
    std::printf("LIMIT_WORKER_EXIT "
                "{\"limit\":%llu,\"shutdownResult\":%d,\"flushResult\":%d,"
                "\"eventResult\":%d,\"channelClosed\":%s,\"errorEvents\":%u,"
                "\"processObserved\":%s,\"exitStatus\":%d,\"waitError\":%d,"
                "\"destroyForced\":%s}\n",
                static_cast<unsigned long long>(limit), static_cast<int>(exited.shutdown_result),
                static_cast<int>(exited.flush_result), static_cast<int>(exited.event_result),
                exited.channel_closed ? "true" : "false", exited.error_events,
                exited.reaped ? "true" : "false", exited.status, exited.wait_error,
                cleanup.destroy_forced ? "true" : "false");
    check(exited.clean(), "shutdown drained and natural exit observed");
    check(!cleanup.destroy_forced, "cleanup did not force an already stopped worker");
    std::printf("LIMIT_WORKER_COMPLETE limit=%llu rows=160 slots=1 naturalExit=true\n",
                static_cast<unsigned long long>(limit));
}
} // namespace
int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::strcmp(argv[1], "--gzip-source-vectors") == 0) {
            for (const size_t length : {0u, 7u, 8u, 9u}) for (const bool fragmented : {false, true}) {
                const auto parts = gzip_segments(length, fragmented);
                std::printf("{\"length\":%zu,\"fragmented\":%s,\"parts\":[", length, fragmented ? "true" : "false");
                for (size_t part = 0; part < parts.size(); ++part) {
                    std::printf("%s[", part ? "," : "");
                    for (size_t index = 0; index < parts[part].size(); ++index) std::printf("%s%u", index ? "," : "", unsigned(parts[part][index]));
                    std::printf("]");
                }
                std::printf("]}\n");
            }
            return 0;
        }
        check(argc >= 3 && argc <= 5, "worker, fixture, optional encoded-header/small-window controls");
        bool encoded_headers = false, small_window = false;
        for (int i = 3; i < argc; ++i) {
            if (std::strcmp(argv[i], "--encoded-headers") == 0 && !encoded_headers) encoded_headers = true;
            else if (std::strcmp(argv[i], "--small-response-window") == 0 && !small_window) small_window = true;
            else check(false, "known unique response-limit option");
        }
        std::ifstream file(argv[2], std::ios::binary);
        check(file.is_open(), "fixture opened");
        const std::string bundle((std::istreambuf_iterator<char>(file)),
                                 std::istreambuf_iterator<char>());
        check(!bundle.empty() && bundle.size() < 65536, "bounded real application fixture");
        exercise(argv[1], 8, bundle, encoded_headers, small_window);
        exercise(argv[1], 0, bundle, encoded_headers, small_window);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
