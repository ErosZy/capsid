#include "protocol.h"

#include <cstdlib>
#include <iostream>
#include <vector>

namespace {

void require(bool condition, const char *message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << std::endl;
        std::exit(1);
    }
}

void test_round_trip_one_byte_at_a_time() {
    capsid::protocol::Frame input;
    input.type = capsid::protocol::kResponseBody;
    input.flags = 0;
    input.request_id = 0x0102030405060708ull;
    input.payload.assign(513, 0x5au);

    std::vector<uint8_t> wire;
    require(capsid::protocol::encode(input, &wire), "frame encodes");
    require(wire.size() == capsid::protocol::kHeaderSize + input.payload.size(), "wire size");
    require(wire[0] == 'W' && wire[1] == 'R' && wire[2] == 'T' && wire[3] == 'C', "wire magic");

    capsid::protocol::Parser parser;
    capsid::protocol::Frame output;
    for (size_t i = 0; i < wire.size(); ++i) {
        require(parser.append(&wire[i], 1), "parser accepts byte");
        const capsid::protocol::ParseResult result = parser.next(&output);
        if (i + 1 < wire.size()) {
            require(result == capsid::protocol::kParseNeedMore, "partial frame waits");
        } else {
            require(result == capsid::protocol::kParseFrame, "complete frame emitted");
        }
    }

    require(output.type == input.type, "type round trips");
    require(output.flags == input.flags, "flags round trip");
    require(output.request_id == input.request_id, "request id round trips");
    require(output.payload == input.payload, "payload round trips");
}

void test_rejects_bad_magic() {
    capsid::protocol::Frame input;
    input.type = capsid::protocol::kHello;
    input.flags = 0;
    input.request_id = 0;

    std::vector<uint8_t> wire;
    require(capsid::protocol::encode(input, &wire), "hello encodes");
    wire[0] = 0;

    capsid::protocol::Parser parser;
    capsid::protocol::Frame output;
    require(parser.append(&wire[0], wire.size()), "bad wire accepted into buffer");
    require(parser.next(&output) == capsid::protocol::kParseError, "bad magic rejected");
}

void test_rejects_oversized_payload_before_allocation() {
    std::vector<uint8_t> wire(capsid::protocol::kHeaderSize, 0);
    wire[0] = 'W';
    wire[1] = 'R';
    wire[2] = 'T';
    wire[3] = 'C';
    wire[4] = 1;
    wire[6] = capsid::protocol::kRequestBody;
    const uint32_t size = capsid::protocol::kMaxPayloadSize + 1;
    wire[20] = static_cast<uint8_t>(size);
    wire[21] = static_cast<uint8_t>(size >> 8);
    wire[22] = static_cast<uint8_t>(size >> 16);
    wire[23] = static_cast<uint8_t>(size >> 24);

    capsid::protocol::Parser parser;
    capsid::protocol::Frame output;
    require(parser.append(&wire[0], wire.size()), "oversized header buffered");
    require(parser.next(&output) == capsid::protocol::kParseError, "oversized payload rejected");
}

void test_accepts_multiple_coalesced_frames() {
    capsid::protocol::Frame first;
    first.type = capsid::protocol::kRequestBody;
    first.flags = 0;
    first.request_id = 41;
    first.payload.assign(capsid::protocol::kMaxPayloadSize, 0x41);

    capsid::protocol::Frame second = first;
    second.request_id = 42;
    second.payload.assign(capsid::protocol::kMaxPayloadSize, 0x42);

    std::vector<uint8_t> first_wire;
    std::vector<uint8_t> second_wire;
    require(capsid::protocol::encode(first, &first_wire), "first frame encodes");
    require(capsid::protocol::encode(second, &second_wire), "second frame encodes");
    first_wire.insert(first_wire.end(), second_wire.begin(), second_wire.end());

    capsid::protocol::Parser parser;
    require(parser.append(&first_wire[0], first_wire.size()), "coalesced frames buffered");

    capsid::protocol::Frame output;
    require(parser.next(&output) == capsid::protocol::kParseFrame, "first frame emitted");
    require(output.request_id == 41 && output.payload == first.payload, "first frame intact");
    require(parser.next(&output) == capsid::protocol::kParseFrame, "second frame emitted");
    require(output.request_id == 42 && output.payload == second.payload, "second frame intact");
    require(parser.next(&output) == capsid::protocol::kParseNeedMore, "parser drained");
}

void test_append_encoded_preserves_prefix_and_rejects_transactionally() {
    const uint8_t payload[] = {0x10, 0x20, 0x30, 0x40};
    std::vector<uint8_t> wire(3, 0xa5);
    require(
        capsid::protocol::append_encoded(
            capsid::protocol::kResponseBody,
            0,
            73,
            payload,
            sizeof(payload),
            &wire),
        "frame appends directly");
    require(
        wire.size() ==
            3 + capsid::protocol::kHeaderSize + sizeof(payload),
        "direct append size");
    require(
        wire[0] == 0xa5 && wire[1] == 0xa5 && wire[2] == 0xa5,
        "direct append preserves prefix");

    capsid::protocol::Parser parser;
    require(
        parser.append(&wire[3], wire.size() - 3),
        "directly appended frame buffered");
    capsid::protocol::Frame decoded;
    require(
        parser.next(&decoded) == capsid::protocol::kParseFrame,
        "directly appended frame parses");
    require(
        decoded.request_id == 73 &&
            decoded.payload ==
                std::vector<uint8_t>(payload, payload + sizeof(payload)),
        "directly appended frame is intact");

    const size_t before = wire.size();
    require(
        !capsid::protocol::append_encoded(
            capsid::protocol::kResponseBody,
            1,
            73,
            payload,
            sizeof(payload),
            &wire),
        "invalid direct append rejected");
    require(wire.size() == before, "rejected append does not mutate output");
}

void test_parser_can_decode_into_reusable_payload_storage() {
    capsid::protocol::Frame input;
    input.type = capsid::protocol::kResponseBody;
    input.flags = 0;
    input.request_id = 91;
    input.payload.assign(4096, 0x6b);
    std::vector<uint8_t> wire;
    require(capsid::protocol::encode(input, &wire), "reusable payload frame encodes");

    capsid::protocol::Parser parser;
    require(parser.append(&wire[0], wire.size()), "reusable payload frame buffered");
    capsid::protocol::Frame metadata;
    std::vector<uint8_t> payload;
    payload.reserve(8192);
    const size_t capacity = payload.capacity();
    require(
        parser.next(&metadata, &payload) == capsid::protocol::kParseFrame,
        "parser emits into reusable payload");
    require(metadata.payload.empty(), "metadata frame does not duplicate payload");
    require(payload == input.payload, "reusable payload is intact");
    require(payload.capacity() == capacity, "reusable payload capacity is retained");
}

void test_parser_can_borrow_payload_storage() {
    capsid::protocol::Frame first;
    first.type = capsid::protocol::kResponseBody;
    first.flags = 0;
    first.request_id = 92;
    first.payload.assign(4096, 0x6c);
    capsid::protocol::Frame second = first;
    second.request_id = 93;
    second.payload.assign(17, 0x7d);

    std::vector<uint8_t> wire;
    require(capsid::protocol::encode(first, &wire), "first view frame encodes");
    std::vector<uint8_t> second_wire;
    require(capsid::protocol::encode(second, &second_wire),
            "second view frame encodes");
    wire.insert(wire.end(), second_wire.begin(), second_wire.end());

    capsid::protocol::Parser parser;
    require(parser.append(&wire[0], wire.size()), "view frames buffered");
    capsid::protocol::Frame metadata;
    const uint8_t *payload = NULL;
    size_t payload_size = 0;
    require(
        parser.next_view(&metadata, &payload, &payload_size) ==
            capsid::protocol::kParseFrame,
        "first payload view emitted");
    require(metadata.request_id == 92 && metadata.payload.empty(),
            "first view metadata intact");
    require(payload_size == first.payload.size() && payload != NULL,
            "first payload view sized");
    require(std::vector<uint8_t>(payload, payload + payload_size) ==
                first.payload,
            "first payload view intact");

    require(
        parser.next_view(&metadata, &payload, &payload_size) ==
            capsid::protocol::kParseFrame,
        "second payload view emitted");
    require(metadata.request_id == 93 &&
                payload_size == second.payload.size() && payload != NULL,
            "second payload view sized");
    require(std::vector<uint8_t>(payload, payload + payload_size) ==
                second.payload,
            "second payload view intact");

    require(
        parser.next_view(&metadata, &payload, &payload_size) ==
            capsid::protocol::kParseNeedMore,
        "view parser drained");
    require(payload == NULL && payload_size == 0,
            "drained view is empty");
}

void test_u64_wire_value_round_trips() {
    const uint64_t expected = 0xfedcba9876543210ull;
    std::vector<uint8_t> wire;
    capsid::protocol::append_u64(&wire, expected);
    require(wire.size() == 8, "u64 wire size");

    const uint8_t *cursor = &wire[0];
    const uint8_t *end = cursor + wire.size();
    uint64_t actual = 0;
    require(capsid::protocol::read_u64(&cursor, end, &actual), "u64 decodes");
    require(actual == expected && cursor == end, "u64 round trips");
}

void test_rejects_unknown_flags() {
    capsid::protocol::Frame input;
    input.type = capsid::protocol::kRequestHead;
    input.flags = 0x80000000u;
    input.request_id = 1;

    std::vector<uint8_t> wire;
    require(!capsid::protocol::encode(input, &wire), "unknown frame flags rejected");
}

void test_ready_sandbox_feature_flags() {
    capsid::protocol::Frame ready;
    ready.type = capsid::protocol::kReady;
    ready.flags = capsid::protocol::kReadySandboxFeatureMask;
    ready.request_id = 0;

    std::vector<uint8_t> wire;
    require(
        capsid::protocol::encode(ready, &wire),
        "known READY sandbox feature flags accepted");
    ready.flags |= 1u << 10;
    require(
        !capsid::protocol::encode(ready, &wire),
        "unknown READY sandbox feature flags rejected");
}

void test_bundle_name_flag_requires_start() {
    capsid::protocol::Frame input;
    input.type = capsid::protocol::kLoadBundle;
    input.flags = capsid::protocol::kFlagBundleName;
    input.request_id = 0;

    std::vector<uint8_t> wire;
    require(
        !capsid::protocol::encode(input, &wire),
        "bundle name flag without start rejected");
    input.flags =
        capsid::protocol::kFlagStart |
        capsid::protocol::kFlagBundleName;
    require(
        capsid::protocol::encode(input, &wire),
        "bundle name flag with start accepted");
}

void test_trusted_bytecode_flag_requires_start() {
    capsid::protocol::Frame input;
    input.type = capsid::protocol::kLoadBundle;
    input.flags = capsid::protocol::kFlagTrustedBytecode;
    input.request_id = 0;

    std::vector<uint8_t> wire;
    require(
        !capsid::protocol::encode(input, &wire),
        "trusted bytecode flag without start rejected");
    input.flags =
        capsid::protocol::kFlagStart |
        capsid::protocol::kFlagEnd |
        capsid::protocol::kFlagTrustedBytecode;
    require(
        capsid::protocol::encode(input, &wire),
        "trusted bytecode flag with start accepted");
}

void test_rejects_null_append() {
    capsid::protocol::Parser parser;
    require(!parser.append(NULL, 1), "null parser input rejected");
}

void test_rejects_unknown_type_and_version() {
    capsid::protocol::Frame frame;
    frame.type = 999;
    frame.flags = 0;
    frame.request_id = 0;
    std::vector<uint8_t> wire;
    require(!capsid::protocol::encode(frame, &wire), "unknown type does not encode");

    frame.type = capsid::protocol::kHello;
    require(capsid::protocol::encode(frame, &wire), "hello encodes");
    const uint16_t unknown_version =
        static_cast<uint16_t>(capsid::protocol::kVersion + 1);
    wire[4] = static_cast<uint8_t>(unknown_version);
    wire[5] = static_cast<uint8_t>(unknown_version >> 8);
    capsid::protocol::Parser parser;
    require(parser.append(&wire[0], wire.size()), "bad version buffered");
    require(
        parser.next(&frame) == capsid::protocol::kParseError,
        "unknown protocol version rejected");
}

void test_memory_metrics_frame_types() {
    capsid::protocol::Frame frame;
    frame.type = capsid::protocol::kMemoryMetricsRequest;
    frame.flags = 0;
    frame.request_id = 0;
    std::vector<uint8_t> wire;
    require(
        capsid::protocol::encode(frame, &wire),
        "memory metrics request encodes");
    frame.type = capsid::protocol::kMemoryMetricsResponse;
    capsid::protocol::append_u32(
        &frame.payload, 1);
    require(
        capsid::protocol::encode(frame, &wire),
        "memory metrics response encodes");
    frame.flags = 1;
    require(
        !capsid::protocol::encode(frame, &wire),
        "memory metrics flags rejected");
}

void test_enforces_total_parser_buffer_limit() {
    std::vector<uint8_t> oversized(
        capsid::protocol::kMaxBufferedBytes + 1, 0);
    capsid::protocol::Parser parser;
    require(
        !parser.append(&oversized[0], oversized.size()),
        "total parser buffer limit enforced");
}

}  // namespace

int main() {
    test_round_trip_one_byte_at_a_time();
    test_rejects_bad_magic();
    test_rejects_oversized_payload_before_allocation();
    test_accepts_multiple_coalesced_frames();
    test_append_encoded_preserves_prefix_and_rejects_transactionally();
    test_parser_can_decode_into_reusable_payload_storage();
    test_parser_can_borrow_payload_storage();
    test_u64_wire_value_round_trips();
    test_rejects_unknown_flags();
    test_ready_sandbox_feature_flags();
    test_bundle_name_flag_requires_start();
    test_trusted_bytecode_flag_requires_start();
    test_rejects_null_append();
    test_rejects_unknown_type_and_version();
    test_memory_metrics_frame_types();
    test_enforces_total_parser_buffer_limit();
    return 0;
}
