#include "capsid/runtime.h"
#include "response_credit_probe.h"
#include "client_channel_state.h"
#include "protocol.h"
#include "win32_compat.h"
#include "worker_exit_assertions.h"
#include "graceful_worker_exit.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
void ok(capsid_result value, const char* message) {
    require(value == CAPSID_OK, message);
}
struct Peer {
    capsid_worker* worker = nullptr;
    capsid_test::ResponseCreditProbe response_credit;
    ~Peer() { capsid_worker_destroy(worker); }
    capsid_event next(bool flush = true) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        for (;;) {
            capsid_result written = CAPSID_OK;
            if (flush) {
                written = capsid_worker_flush(worker);
                require(written == CAPSID_OK || written == CAPSID_WOULD_BLOCK, "flush queued IPC");
            }
            capsid_event event{}; event.struct_size = sizeof(event);
            const auto result = capsid_worker_next_event(worker, &event);
            if (result == CAPSID_OK) {
                if (event.type == CAPSID_EVENT_ERROR)
                    std::fprintf(stderr, "worker error: %.*s\n", int(event.payload.size), event.payload.data);
                require(event.type != CAPSID_EVENT_ERROR && event.type != CAPSID_EVENT_EXIT, "worker remains healthy");
                return event;
            }
            require(result == CAPSID_WOULD_BLOCK && std::chrono::steady_clock::now() < deadline, "worker event deadline");
            capsid_pollfd descriptor{}; descriptor.fd = capsid_worker_fd(worker);
            descriptor.events = POLLIN | (written == CAPSID_WOULD_BLOCK ? POLLOUT : 0);
            capsid::win32::capsid_poll(&descriptor, 1, 10);
        }
    }
    capsid_channel_event channel(capsid_event_type type) {
        const auto event = next(); require(event.type == type, "channel event order");
        capsid_channel_event view; capsid_channel_event_init(&view);
        if (type == CAPSID_EVENT_CHANNEL_DIAL) {
            capsid::protocol::ChannelOpenPayload open; std::string error;
            require(capsid::protocol::decode_channel_open(event.payload.data, event.payload.size,
                    &open, &error, 123), "decode negotiated fetch OPEN");
            view.channel_id = open.channel_id; view.dial_id = open.dial_id;
        } else ok(capsid_channel_event_decode(&event, &view, sizeof(view)), "decode real worker channel event");
        return view;
    }
    void begin(uint64_t id, const char* path) {
        const std::string url = std::string("https://app.test") + path;
        ok(capsid_worker_begin_bodyless_request(worker, id, "GET", url.c_str(), nullptr, 0), "HTTP request");
    }
    std::string response(uint64_t id) {
        bool head = false; std::string text;
        for (;;) {
            const auto event = next(); require(event.request_id == id, "original HTTP owner");
            if (event.type == CAPSID_EVENT_RESPONSE_HEAD) {
                require(!head && event.status == 200, "one successful response head"); head = true;
            } else if (event.type == CAPSID_EVENT_RESPONSE_BODY) {
                require(head, "body follows head");
                text.append(reinterpret_cast<const char*>(event.payload.data), event.payload.size);
                response_credit.consume(worker, event);
            } else if (event.type == CAPSID_EVENT_RESPONSE_END) {
                require(head, "response completes after head");
                response_credit.complete(id, text); return text;
            } else require(false, "no unexpected events after cancellation acknowledgement");
        }
    }
};
constexpr char bundle[] = R"JS(
export default {async fetch(request) {
    const mode = new URL(request.url).pathname.slice(1);
    if (mode === 'healthy') return new Response('healthy');
    const response = await fetch('https://example.test/body', {headers:{Upgrade:'websocket'}});
    const reader = response.body.getReader(mode === 'default' ? undefined : {mode:'byob'});
    const first = mode === 'default' ? await reader.read() : mode === 'minimum' ?
        await reader.readAtLeast(3, new Uint8Array(8)) : await reader.read(new Uint8Array(8));
    let closed = false; reader.closed.then(() => {closed = true});
    const cancelled = await reader.cancel('finished');
    await Promise.resolve(); reader.releaseLock();
    return Response.json({done:first.done,bytes:Array.from(first.value),closed,
        cancelled:cancelled === undefined,locked:response.body.locked});
}};
)JS";

void run(const char* worker_path, bool small_window) {
    capsid_worker_config config; capsid_worker_config_init(&config); config.worker_path = worker_path;
    if (small_window) config.initial_stream_window = 16;
    capsid_egress_rule rule; capsid_egress_rule_init(&rule);
    rule.action = CAPSID_EGRESS_ALLOW; rule.target = "example.test";
    capsid_egress_policy policy; capsid_egress_policy_init(&policy);
    policy.rules = &rule; policy.rule_count = 1; config.egress_policy = &policy;
    capsid_protocol_options options; capsid_protocol_options_init(&options, sizeof(options));
    options.wire_version = 4; options.capabilities = 123; options.max_channels = 1;
    Peer peer;
    peer.response_credit = capsid_test::ResponseCreditProbe(small_window ? 16 : 0);
    ok(capsid_worker_spawn_ex(&config, &options, &peer.worker), "spawn actual worker");
    ok(capsid_worker_load_bundle(peer.worker, reinterpret_cast<const uint8_t*>(bundle), std::strlen(bundle)), "load bundle");
    require(peer.next().type == CAPSID_EVENT_READY, "actual worker READY");
    const char* modes[] = {"/default", "/byob", "/minimum"};
    uint64_t last_dial = 0;
    for (uint32_t round = 0; round < 12; ++round) {
        const uint64_t request = round * 2 + 1;
        const uint32_t channel = round + 1;
        peer.begin(request, modes[round % 3]);
        const auto dial = peer.channel(CAPSID_EVENT_CHANNEL_DIAL);
        require(dial.dial_id > last_dial && dial.channel_id == 0, "fresh dial uses reclaimed one-slot capacity");
        last_dial = dial.dial_id;
        capsid_fetch_upgrade_response response{}; response.struct_size = sizeof(response);
        response.version = CAPSID_FETCH_UPGRADE_RESPONSE_VERSION; response.status = 200;
        const char status[] = "OK", url[] = "https://example.test/body";
        response.status_text = {reinterpret_cast<const uint8_t*>(status), sizeof(status) - 1};
        response.url = {reinterpret_cast<const uint8_t*>(url), sizeof(url) - 1};
        ok(capsid_worker_dial_accept_http(peer.worker, dial.dial_id, channel, 65536, &response), "admit live HTTP response body");
        const auto accepted = peer.channel(CAPSID_EVENT_CHANNEL_ACK);
        require(accepted.channel_id == channel && accepted.dial_id == dial.dial_id, "result ACK opens exact channel");
        const uint8_t first[] = {7, 8, 9}, extra[] = {100, 101};
        ok(capsid_worker_channel_send(peer.worker, channel, first, sizeof(first), 3), "first body read");
        ok(capsid_worker_flush(peer.worker), "deliver only first read before staging crossing frames");
        // These are valid writes made before the host observes CANCEL. Hold
        // their IPC bytes locally while the worker consumes the first read.
        // next(false) never flushes, so the cancellation must reach the host
        // before the held DATA reaches the cancelling worker.
        if (round % 4 == 0) {
            ok(capsid_worker_channel_send(peer.worker, channel, extra, sizeof(extra), 3), "queued complete DATA crosses cancel");
        } else if (round % 4 == 1) {
            ok(capsid_worker_channel_send(peer.worker, channel, extra, 1, 1), "queued fragment START crosses cancel");
            ok(capsid_worker_channel_send(peer.worker, channel, extra + 1, 1, 2), "queued fragment END crosses cancel");
        } else if (round % 4 == 2) {
            ok(capsid_worker_channel_send(peer.worker, channel, nullptr, 0, 3), "queued empty DATA crosses cancel");
        } else {
            ok(capsid_worker_channel_send(peer.worker, channel, extra, 1, 1), "unfinished fragment crosses cancel");
        }
        bool cancelled = false, head = false, ended = false, credited = false; std::string text;
        while (!cancelled) {
            const auto event = peer.next(false);
            if (event.type == CAPSID_EVENT_CHANNEL_CANCEL) {
                capsid_channel_event view; capsid_channel_event_init(&view);
                ok(capsid_channel_event_decode(&event, &view, sizeof(view)), "decode actual reader cancellation");
                require(view.channel_id == channel && view.dial_id == 0, "cancellation retains assigned channel");
                cancelled = true;
            } else if (event.type == CAPSID_EVENT_CHANNEL_CREDIT) {
                capsid_channel_event view; capsid_channel_event_init(&view);
                ok(capsid_channel_event_decode(&event, &view, sizeof(view)), "decode consumed first-read credit");
                require(!credited && view.channel_id == channel && view.credit == sizeof(first),
                        "only consumed first-read bytes replenish credit before cancellation");
                credited = true;
            } else if (event.type == CAPSID_EVENT_RESPONSE_HEAD) {
                require(event.request_id == request && event.status == 200 && !head, "own response head before ACK"); head = true;
            } else if (event.type == CAPSID_EVENT_RESPONSE_BODY) {
                require(event.request_id == request && head && !ended, "own response bytes before ACK");
                text.append(reinterpret_cast<const char*>(event.payload.data), event.payload.size);
                peer.response_credit.consume(peer.worker, event);
            } else if (event.type == CAPSID_EVENT_RESPONSE_END) {
                require(event.request_id == request && head && !ended, "own response terminal before ACK"); ended = true;
            } else require(false, "cancelled reader emits no unexpected events");
        }
        require(capsid_worker_channel_send(peer.worker, channel, extra, 1, 3) == CAPSID_PROTOCOL_ERROR,
                "new DATA remains rejected after cancellation is observed");
        ok(capsid_worker_channel_confirm(peer.worker, channel, 0, CAPSID_CHANNEL_ACK_CANCEL), "ACK is queued after all pre-cancel DATA");
        ok(capsid_worker_flush(peer.worker), "deliver crossing DATA and final ACK in FIFO order");
        while (!ended) {
            const auto event = peer.next();
            require(event.request_id == request, "remaining response belongs to cancelled read");
            if (event.type == CAPSID_EVENT_RESPONSE_HEAD) {
                require(!head && event.status == 200, "one response head"); head = true;
            } else if (event.type == CAPSID_EVENT_RESPONSE_BODY) {
                require(head, "body follows own head");
                text.append(reinterpret_cast<const char*>(event.payload.data), event.payload.size);
                peer.response_credit.consume(peer.worker, event);
            } else if (event.type == CAPSID_EVENT_RESPONSE_END) {
                require(head, "complete response"); ended = true;
            } else require(false, "discarded fragments do not dispatch callbacks");
        }
        require(text == "{\"done\":false,\"bytes\":[7,8,9],\"closed\":true,\"cancelled\":true,\"locked\":false}",
                "only the first read is delivered and cancellation releases reader ownership");
        peer.response_credit.complete(request, text);
        capsid::ChannelEntry entry;
        require(!capsid::client_channel_snapshot(peer.worker, channel, &entry), "ACK retires channel reservation");
        peer.begin(request + 1, "/healthy");
        require(peer.response(request + 1) == "healthy", "next real HTTP request survives discarded in-flight DATA");
    }
    // destroy is abortive. Drain the graceful stop and observe the actual
    // process exit before handing ownership to that cleanup API.
    const auto exited = capsid_test::shutdown_and_wait(peer.worker, 2500);
    const auto cleanup = capsid::destroy_worker_with_status(peer.worker); peer.worker = nullptr;
    if (!exited.clean() || cleanup.destroy_forced) {
        std::fprintf(stderr, "CHANNEL_DISCARD_EXIT clean=%d shutdown=%d flush=%d event=%d closed=%d reaped=%d status=%d wait=%d forced=%d errors=%u\n",
            exited.clean() ? 1 : 0, int(exited.shutdown_result), int(exited.flush_result), int(exited.event_result),
            exited.channel_closed ? 1 : 0, exited.reaped ? 1 : 0, exited.status, exited.wait_error,
            cleanup.destroy_forced ? 1 : 0, exited.error_events);
    }
    require(exited.clean() && !cleanup.destroy_forced, "worker exits naturally after all cancelled channels");
    std::puts("PASS: 12 deterministic reader-cancel crossings; complete, fragmented, empty and unfinished DATA; one-slot reuse; natural exit");
}
}
int main(int argc, char** argv) {
    try { require(argc == 2 || (argc == 3 && std::strcmp(argv[2], "--small-response-window") == 0),
                  "worker and optional small response window");
        run(argv[1], argc == 3); return 0; }
    catch (const std::exception& error) { std::fprintf(stderr, "FAIL: %s\n", error.what()); return 1; }
}
