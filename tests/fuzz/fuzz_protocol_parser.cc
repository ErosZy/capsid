#include "protocol.h"

#include <stdint.h>

#include <cstdlib>
#include <vector>

namespace {

void check_round_trip(const capsid::protocol::Frame &frame) {
    std::vector<uint8_t> encoded;
    if (!capsid::protocol::encode(frame, &encoded)) {
        std::abort();
    }
    capsid::protocol::Parser parser;
    if (!parser.append(&encoded[0], encoded.size())) {
        std::abort();
    }
    capsid::protocol::Frame decoded;
    if (parser.next(&decoded) != capsid::protocol::kParseFrame ||
        decoded.type != frame.type ||
        decoded.flags != frame.flags ||
        decoded.request_id != frame.request_id ||
        decoded.payload != frame.payload ||
        parser.next(&decoded) != capsid::protocol::kParseNeedMore) {
        std::abort();
    }
}

bool drain(capsid::protocol::Parser *parser) {
    for (;;) {
        capsid::protocol::Frame frame;
        const capsid::protocol::ParseResult result =
            parser->next(&frame);
        if (result == capsid::protocol::kParseFrame) {
            check_round_trip(frame);
            continue;
        }
        return result != capsid::protocol::kParseError;
    }
}

void exercise_valid_frame(const uint8_t *data, size_t size) {
    capsid::protocol::Frame frame;
    frame.type = capsid::protocol::kRequestBody;
    frame.flags = 0;
    frame.request_id = size == 0 ? 1 : data[0];
    if (size != 0) {
        frame.payload.assign(data, data + size);
    }
    std::vector<uint8_t> encoded;
    if (!capsid::protocol::encode(frame, &encoded)) {
        std::abort();
    }

    capsid::protocol::Parser parser;
    size_t offset = 0;
    while (offset < encoded.size()) {
        const size_t desired =
            1 + (offset < size
                     ? static_cast<size_t>(data[offset] % 31)
                     : offset % 31);
        const size_t remaining = encoded.size() - offset;
        const size_t chunk =
            desired < remaining ? desired : remaining;
        if (!parser.append(&encoded[offset], chunk)) {
            std::abort();
        }
        offset += chunk;
        if (!drain(&parser)) {
            std::abort();
        }
    }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    capsid::protocol::Parser parser;
    if (size == 0) {
        parser.append(NULL, 0);
        drain(&parser);
        exercise_valid_frame(data, size);
        return 0;
    }

    size_t offset = 0;
    while (offset < size) {
        const size_t chunk =
            1 + static_cast<size_t>(data[offset] % 63);
        const size_t remaining = size - offset;
        const size_t actual = chunk < remaining ? chunk : remaining;
        if (!parser.append(data + offset, actual)) {
            break;
        }
        offset += actual;
        if (!drain(&parser)) {
            break;
        }
    }
    drain(&parser);
    exercise_valid_frame(data, size);
    return 0;
}
