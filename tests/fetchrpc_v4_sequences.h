#ifndef CAPSID_TESTS_FETCHRPC_V4_SEQUENCES_H
#define CAPSID_TESTS_FETCHRPC_V4_SEQUENCES_H

// Shared driver for the frozen FetchRPC v4 A2 sequences
// (tests/fixtures/fetchrpc-v4/vectors.json, schema v2). Both the
// test-local reference model (test_fetchrpc_v4.cc) and the production
// registry (test_channel_registry.cc) run the SAME sequences through
// this driver; neither run substitutes for the other (GLM plan T05/
// T06).
#include "protocol.h"

#include <jansson.h>

#include <cstdint>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace fetchrpc_sequences {
inline const char *string_value(const json_t *value) {
    const char *text = json_string_value(value);
    return text ? text : "";
}

// A sequence execution target: applies one wire frame that travelled
// in `dir` ("h2w" | "w2h") and returns "ok" or the exact contract
// error string.
class SequenceTarget {
public:
    virtual ~SequenceTarget() {}
    virtual void reset() = 0;
    virtual std::string apply(const std::string &dir,
                              const capsid::protocol::Frame &frame) = 0;
};

// Canonical v3 startup payload (mirrors the fixed fixture in
// tests/test_ipc_validation.cc); HELLO vectors and synthesized HELLOs
// build on it so the REAL startup decoder accepts them.
inline std::vector<uint8_t> canonical_startup_payload() {
    namespace p = capsid::protocol;
    std::vector<uint8_t> payload;
    p::append_u32(&payload, 7);  // CAPSID_ABI_VERSION
    p::append_u64(&payload, 64u * 1024u * 1024u);
    p::append_u64(&payload, 0);
    p::append_u32(&payload, 64);
    p::append_u64(&payload, 5000);
    p::append_u32(&payload, 1024 * 1024);
    p::append_u32(&payload, 4);
    p::append_u32(&payload, 1024);
    p::append_u32(&payload, 64 * 1024);
    p::append_u32(&payload, 4u * 1024u * 1024u);
    payload.push_back(0);
    p::append_u32(&payload, 0);
    p::append_u32(&payload, 0);
    p::append_u16(&payload, 0);
    p::append_u64(&payload, 0);
    p::append_u64(&payload, 0);
    p::append_u32(&payload, 0);
    p::append_u32(&payload, 0);
    payload.push_back(0);
    p::append_u32(&payload, 0);
    p::append_u16(&payload, 0);
    p::append_u16(&payload, 0);
    p::append_u16(&payload, 0);
    p::append_u32(&payload, 0);
    p::append_u32(&payload, 0);
    p::append_u16(&payload, 0);
    return payload;
}

inline std::vector<uint8_t> filler_bytes(size_t count) {
    std::vector<uint8_t> out(count);
    for (size_t i = 0; i < count; ++i) {
        out[i] = static_cast<uint8_t>((i * 31 + 7) & 0xff);
    }
    return out;
}

// Synthesizes one "make" step into a real wire frame (composed with
// the production layout helpers, then re-parsed by the caller).
inline bool synthesize(const json_t *make, capsid::protocol::Frame *frame,
                       std::string *error) {
    namespace p = capsid::protocol;
    const json_t *kind_json = json_object_get(make, "kind");
    if (!json_is_string(kind_json)) {
        *error = "make.kind missing";
        return false;
    }
    const std::string kind = string_value(kind_json);
    const int channel =
        json_integer_value(json_object_get(make, json_object_get(make, "channel") ? "channel" : "channel_id"));
    frame->flags = 0;
    frame->request_id = 0;

    if (kind == "hello" || kind == "ready") {
        const uint32_t caps = static_cast<uint32_t>(
            json_integer_value(json_object_get(make, "caps")));
        frame->type = kind == "hello" ? p::kHello : p::kReady;
        frame->version = p::kVersionV4;
        if (kind == "hello") {
            frame->payload = canonical_startup_payload();
            if ((caps & p::kCapChannelLayoutA2) != 0) {
                p::ChannelLimits limits;
                limits.max_channels = 128;
                limits.max_message_bytes = 1024 * 1024;
                limits.reassembly_budget_bytes = 16 * 1024 * 1024;
                limits.send_budget_bytes = 16 * 1024 * 1024;
                limits.close_timeout_ms = 5000;
                limits.dial_timeout_ms = 10000;
                limits.idle_timeout_ms = 0;
                limits.per_channel_queue_bytes = 4 * 1024 * 1024;
                if (!p::append_hello_a2_extension(&frame->payload,
                                                  limits)) {
                    *error = "a2 extension append failed";
                    return false;
                }
            }
        } else {
            frame->payload.assign(20, 0);
        }
        p::append_u32(&frame->payload, caps);
        p::append_u32(&frame->payload, 0);
        return true;
    }
    if (kind == "open") {
        const uint64_t dial = static_cast<uint64_t>(
            json_integer_value(json_object_get(make, "dial_id")));
        const std::string url = string_value(
            json_object_get(make, "url"));
        p::ChannelOpenPayload open;
        open.channel_id = 0;
        open.dial_id = dial;
        open.owner_kind = p::kOwnerStandalone;
        open.channel_kind = p::kChannelKindWebSocket;
        open.owner_request_id = 0;
        open.max_message_bytes = json_object_get(make, "max_message")
            ? json_integer_value(json_object_get(make, "max_message")) : 1024 * 1024;
        open.url = url;
        frame->type = p::kChannelOpen;
        return p::encode_channel_open(open, &frame->payload);
    }
    if (kind == "accept") {
        const uint64_t dial = static_cast<uint64_t>(
            json_integer_value(json_object_get(make, "dial_id")));
        p::ChannelAcceptPayload accept;
        accept.dial_id = dial;
        accept.channel_id = static_cast<uint32_t>(channel);
        accept.accepted_max_message_bytes = json_object_get(make, "max_message")
            ? json_integer_value(json_object_get(make, "max_message")) : 1024 * 1024;
        accept.selected_protocol = "chat";
        frame->type = p::kChannelAccept;
        return p::encode_channel_accept(accept, &frame->payload);
    }
    if (kind == "reject") {
        const uint64_t dial = static_cast<uint64_t>(
            json_integer_value(json_object_get(make, "dial_id")));
        p::ChannelRejectPayload reject;
        reject.dial_id = dial;
        reject.channel_id = 0;
        reject.reason_code = 3;
        reject.diagnostic = "connect refused";
        frame->type = p::kChannelReject;
        return p::encode_channel_reject(reject, &frame->payload);
    }
    if (kind == "data" || kind == "data-v3") {
        frame->type = p::kChannelData;
        frame->version = kind == "data-v3"
            ? p::kVersion
            : p::kVersionV4;
        const bool text = json_is_true(json_object_get(make, "text"));
        const bool first =
            json_is_true(json_object_get(make, "first"));
        const bool last = json_is_true(json_object_get(make, "last"));
        if (first) {
            frame->flags |= p::kFlagStart;
        }
        if (last) {
            frame->flags |= p::kFlagEnd;
        }
        if (text) {
            frame->flags |= p::kFlagChanText;
        }
        p::append_u32(&frame->payload,
                      static_cast<uint32_t>(channel));
        const json_t *single = json_object_get(make, "single");
        if (json_is_string(single)) {
            const std::string value = string_value(single);
            frame->payload.insert(frame->payload.end(),
                                  value.begin(), value.end());
            if (!first && !last) {
                frame->flags |= p::kFlagStart | p::kFlagEnd;
            }
        } else {
            const size_t count = static_cast<size_t>(
                json_integer_value(json_object_get(make, "bytes")));
            if (count > p::kMaxPayloadSize - sizeof(uint32_t)) {
                *error = "make.data exceeds a wire fragment";
                return false;
            }
            const std::vector<uint8_t> bytes = filler_bytes(count);
            frame->payload.insert(frame->payload.end(),
                                  bytes.begin(), bytes.end());
        }
        return true;
    }
    if (kind == "credit") {
        p::ChannelCreditPayload grant;
        grant.channel_id = static_cast<uint32_t>(channel);
        grant.credit = static_cast<uint64_t>(
            json_integer_value(json_object_get(make, "amount")));
        frame->type = p::kChannelCredit;
        return p::encode_channel_credit(grant, &frame->payload);
    }
    if (kind == "halfclose") {
        frame->type = p::kChannelHalfClose;
        p::append_u32(&frame->payload,
                      static_cast<uint32_t>(channel));
        return true;
    }
    if (kind == "close") {
        p::ChannelClosePayload close;
        close.channel_id = static_cast<uint32_t>(channel);
        const std::string kind_name = string_value(
            json_object_get(make, "kind_name"));
        close.close_kind = kind_name == "observed"
            ? p::kCloseObserved
            : (kind_name == "transport" ? p::kCloseTransport
                                        : p::kCloseInitiated);
        close.flags = p::kCloseFlagCodePresent;
        close.code = static_cast<uint16_t>(
            json_integer_value(json_object_get(make, "code")));
        close.reason = "done";
        frame->type = p::kChannelClose;
        return p::encode_channel_close(close, &frame->payload);
    }
    *error = "unknown make kind: " + kind;
    return false;
}

// Drives every sequence against the target. `failures` accumulates;
// returns the set of executed sequence names.
inline std::set<std::string> run_sequences(
        const json_t *sequences,
        const std::map<std::string, std::vector<uint8_t>> &frame_wire,
        SequenceTarget *target,
        int *failures) {
    std::set<std::string> executed;
    if (!json_is_array(sequences) || json_array_size(sequences) == 0) {
        std::fprintf(stderr, "FAIL: nonempty sequences array required\n");
        ++(*failures);
        return executed;
    }
    size_t index;
    json_t *value;
    json_array_foreach(sequences, index, value) {
        const std::string name =
            string_value(json_object_get(value, "name"));
        const json_t *steps = json_object_get(value, "steps");
        if (name.empty() || !(json_is_array(steps) && json_array_size(steps) >= 1)) {
            std::fprintf(stderr, "FAIL: sequence has steps: %s\n",
                         name.c_str());
            ++(*failures);
            continue;
        }
        if (!executed.insert(name).second) {
            std::fprintf(stderr, "FAIL: duplicate sequence %s\n", name.c_str());
            ++(*failures);
        }
        target->reset();
        size_t step_index;
        json_t *step_json;
        json_array_foreach(steps, step_index, step_json) {
            const std::string expect = string_value(
                json_object_get(step_json, "expect"));
            const std::string dir = string_value(
                json_object_get(step_json, "dir"));
            const json_t *frame_ref =
                json_object_get(step_json, "frame");
            const json_t *make = json_object_get(step_json, "make");
            if (expect.empty() || (dir != "h2w" && dir != "w2h") ||
                    !(frame_ref ? json_is_string(frame_ref) : true) ||
                    !(make ? json_is_object(make) : true) ||
                    ((!frame_ref) == (!make))) {
                std::fprintf(stderr,
                             "FAIL: step shape: %s step %zu\n",
                             name.c_str(), step_index);
                ++(*failures);
                continue;
            }
            capsid::protocol::Frame frame;
            if (frame_ref) {
                const std::string ref = string_value(frame_ref);
                std::map<std::string, std::vector<uint8_t>>
                    ::const_iterator it = frame_wire.find(ref);
                if (it == frame_wire.end()) {
                    std::fprintf(stderr,
                        "FAIL: step frame exists: %s/%s\n",
                        name.c_str(), ref.c_str());
                    ++(*failures);
                    continue;
                }
                capsid::protocol::Parser v4(4);
                v4.append(it->second.data(), it->second.size());
                if (v4.next(&frame) != capsid::protocol::kParseFrame) {
                    std::fprintf(stderr,
                        "FAIL: step frame parses: %s/%s\n",
                        name.c_str(), ref.c_str());
                    ++(*failures);
                    continue;
                }
            } else {
                std::string error;
                if (!synthesize(make, &frame, &error)) {
                    std::fprintf(stderr,
                        "FAIL: synthesize: %s %s\n",
                        name.c_str(), error.c_str());
                    ++(*failures);
                    continue;
                }
                if (frame.version == capsid::protocol::kVersion) {
                    // a v3 frame on the v4 connection: feed it raw
                    const std::string outcome =
                        target->apply(dir, frame);
                    if (outcome != expect) {
                        std::fprintf(stderr,
                            "FAIL: sequence %s step %zu (got %s, "
                            "want %s)\n", name.c_str(), step_index,
                            outcome.c_str(), expect.c_str());
                        ++(*failures);
                    }
                    continue;
                }
                std::vector<uint8_t> wire;
                capsid::protocol::encode_versioned(4, frame, &wire);
                capsid::protocol::Parser v4(4);
                v4.append(wire.data(), wire.size());
                if (v4.next(&frame) !=
                        capsid::protocol::kParseFrame) {
                    std::fprintf(stderr,
                        "FAIL: synthesized frame parses: %s\n",
                        name.c_str());
                    ++(*failures);
                    continue;
                }
            }
            const std::string outcome = target->apply(dir, frame);
            if (outcome != expect) {
                std::fprintf(stderr,
                    "FAIL: sequence %s step %zu (got %s, want %s)\n",
                    name.c_str(), step_index, outcome.c_str(),
                    expect.c_str());
                ++(*failures);
            }
        }
    }
    return executed;
}

}  // namespace fetchrpc_sequences

#endif
