#include "ipc_validation.h"
#include "protocol.h"
#include "capsid/runtime.h"

#include <stdint.h>

#include <cstdlib>
#include <string>
#include <vector>

namespace {

capsid::protocol::Frame valid_hello() {
    capsid::protocol::Frame frame;
    frame.type = capsid::protocol::kHello;
    frame.flags = 0;
    frame.request_id = 0;
    capsid::protocol::append_u32(&frame.payload, CAPSID_ABI_VERSION);
    capsid::protocol::append_u64(
        &frame.payload, UINT64_C(64) * 1024 * 1024);
    capsid::protocol::append_u64(&frame.payload, 0);
    capsid::protocol::append_u32(&frame.payload, 64);
    capsid::protocol::append_u64(&frame.payload, 5000);
    capsid::protocol::append_u32(&frame.payload, 1024 * 1024);
    capsid::protocol::append_u32(&frame.payload, 4);
    capsid::protocol::append_u32(&frame.payload, 1024);
    capsid::protocol::append_u32(&frame.payload, 64 * 1024);
    capsid::protocol::append_u32(
        &frame.payload, 4 * 1024 * 1024);
    frame.payload.push_back(0);
    capsid::protocol::append_u32(&frame.payload, 0);
    capsid::protocol::append_u32(&frame.payload, 0);
    capsid::protocol::append_u16(&frame.payload, 0);
    capsid::protocol::append_u64(&frame.payload, 0);
    capsid::protocol::append_u64(&frame.payload, 0);
    capsid::protocol::append_u32(
        &frame.payload, CAPSID_EGRESS_DENY);
    capsid::protocol::append_u32(&frame.payload, 0);
    frame.payload.push_back(0);
    capsid::protocol::append_u32(&frame.payload, 0);
    capsid::protocol::append_u16(&frame.payload, 0);
    capsid::protocol::append_u16(&frame.payload, 0);
    capsid::protocol::append_u16(&frame.payload, 0);
    capsid::protocol::append_u32(
        &frame.payload, CAPSID_EGRESS_DENY);
    capsid::protocol::append_u32(&frame.payload, 0);
    return frame;
}

void initialize(capsid::WorkerStartupState *state) {
    std::string error;
    if (!state->consume(valid_hello(), &error)) {
        std::abort();
    }
}

void assign_payload(capsid::protocol::Frame *frame,
                    const uint8_t *data,
                    size_t size) {
    if (size != 0) {
        frame->payload.assign(data, data + size);
    }
}

void exercise_startup(const uint8_t *data, size_t size) {
    std::string error;

    capsid::protocol::Frame mutated_hello = valid_hello();
    for (size_t index = 0;
         index < size && index < mutated_hello.payload.size();
         ++index) {
        mutated_hello.payload[index] ^= data[index];
    }
    capsid::WorkerStartupState hello_state;
    hello_state.consume(mutated_hello, &error);

    capsid::protocol::Frame raw_hello;
    raw_hello.type = capsid::protocol::kHello;
    raw_hello.flags = size == 0 ? 0 : data[0];
    raw_hello.request_id = size < 2 ? 0 : data[1];
    assign_payload(&raw_hello, data, size);
    capsid::WorkerStartupState raw_hello_state;
    raw_hello_state.consume(raw_hello, &error);

    capsid::protocol::Frame bundle;
    bundle.type = capsid::protocol::kLoadBundle;
    bundle.flags = size == 0
        ? 0
        : static_cast<uint32_t>(
              data[0] &
              (capsid::protocol::kFlagStart |
               capsid::protocol::kFlagEnd |
               capsid::protocol::kFlagBundleName));
    bundle.request_id = size < 2 ? 0 : data[1];
    assign_payload(&bundle, data, size);
    capsid::WorkerStartupState bundle_state;
    initialize(&bundle_state);
    bundle_state.consume(bundle, &error);

    bundle.flags =
        capsid::protocol::kFlagStart |
        capsid::protocol::kFlagEnd |
        capsid::protocol::kFlagBundleName;
    bundle.request_id = 0;
    capsid::WorkerStartupState named_state;
    initialize(&named_state);
    named_state.consume(bundle, &error);

    bundle.flags =
        capsid::protocol::kFlagStart |
        capsid::protocol::kFlagEnd;
    capsid::WorkerStartupState raw_state;
    initialize(&raw_state);
    if (!raw_state.consume(bundle, &error) ||
        !raw_state.bundle_complete() ||
        raw_state.bundle().size() != size) {
        std::abort();
    }
}

void exercise_request(const uint8_t *data, size_t size) {
    capsid::protocol::Frame frame;
    frame.type = capsid::protocol::kRequestHead;
    frame.flags = 0;
    frame.request_id = 1;
    assign_payload(&frame, data, size);
    capsid::WorkerRequestHead request;
    std::string error;
    capsid::decode_worker_request_head(
        frame, capsid::protocol::kMaxPayloadSize, &request, &error);

    capsid::protocol::Frame structured;
    structured.type = capsid::protocol::kRequestHead;
    structured.flags = 0;
    structured.request_id = 1;
    const char method[] = "POST";
    capsid::protocol::append_u16(
        &structured.payload, sizeof(method) - 1);
    structured.payload.insert(
        structured.payload.end(), method, method + sizeof(method) - 1);
    const size_t url_size = size < 128 ? size : 128;
    capsid::protocol::append_u32(
        &structured.payload, static_cast<uint32_t>(url_size + 1));
    structured.payload.push_back('/');
    for (size_t index = 0; index < url_size; ++index) {
        structured.payload.push_back(
            static_cast<uint8_t>('a' + data[index] % 26));
    }
    capsid::protocol::append_u16(&structured.payload, 1);
    const char name[] = "x-fuzz";
    capsid::protocol::append_u16(
        &structured.payload, sizeof(name) - 1);
    structured.payload.insert(
        structured.payload.end(), name, name + sizeof(name) - 1);
    capsid::protocol::append_u32(
        &structured.payload, static_cast<uint32_t>(url_size));
    if (url_size != 0) {
        structured.payload.insert(
            structured.payload.end(),
            data,
            data + url_size);
    }
    if (!capsid::decode_worker_request_head(
            structured,
            capsid::protocol::kMaxPayloadSize,
            &request,
            &error)) {
        std::abort();
    }
}

void exercise_framing(const uint8_t *data, size_t size) {
    capsid::protocol::Parser parser;
    capsid::WorkerStartupState startup;
    size_t offset = 0;
    while (offset < size) {
        const size_t desired =
            1 + static_cast<size_t>(data[offset] % 127);
        const size_t remaining = size - offset;
        const size_t chunk =
            desired < remaining ? desired : remaining;
        if (!parser.append(data + offset, chunk)) {
            return;
        }
        offset += chunk;
        for (;;) {
            capsid::protocol::Frame frame;
            const capsid::protocol::ParseResult result =
                parser.next(&frame);
            if (result == capsid::protocol::kParseError) {
                return;
            }
            if (result == capsid::protocol::kParseNeedMore) {
                break;
            }
            std::string error;
            if (!startup.bundle_complete()) {
                startup.consume(frame, &error);
            } else if (frame.type ==
                       capsid::protocol::kRequestHead) {
                capsid::WorkerRequestHead request;
                capsid::decode_worker_request_head(
                    frame,
                    capsid::protocol::kMaxPayloadSize,
                    &request,
                    &error);
            }
        }
    }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    exercise_startup(data, size);
    exercise_request(data, size);
    exercise_framing(data, size);
    return 0;
}
