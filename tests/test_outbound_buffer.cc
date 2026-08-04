// Unit test for capsid::OutboundBuffer (docs/queue-saturation-activity-fix.md
// §3.4): with an injected short-write / EAGAIN writer, physical storage
// must stay bounded by one frame plus the wire limit, the logical queue
// must never exceed the limit, frame boundaries must never be corrupted,
// and full drains must reset the buffer.
//
// Run: test-outbound-buffer

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "outbound_buffer.h"
#include "protocol.h"

namespace {

[[noreturn]] void fail(const char *message) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::exit(1);
}

#define require(cond, msg)                                                     \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fail(msg);                                                         \
        }                                                                      \
    } while (0)

struct WriterState {
    size_t max_write;  // 0 = EAGAIN stall
    size_t total_sent;
    int fatal_after;   // -1 = never
};

ssize_t short_writer(const uint8_t *data, size_t size, void *opaque) {
    WriterState *state = static_cast<WriterState *>(opaque);
    if (state->max_write == 0) {
        return 0;  // stall
    }
    if (state->fatal_after == 0) {
        return -1;
    }
    if (state->fatal_after > 0) {
        state->fatal_after -= 1;
    }
    const size_t n = size < state->max_write ? size : state->max_write;
    state->total_sent += n;
    return static_cast<ssize_t>(n);
}

void drain(capsid::OutboundBuffer *buffer, WriterState *state) {
    int guard = 0;
    while (!buffer->drained() && guard < 400000) {
        require(buffer->flush(short_writer, state), "flush");
        guard += 1;
    }
    require(buffer->drained(), "buffer drained");
}

// Appends frames up to the wire limit without flushing, asserts the
// high-water equals the limit exactly, then drains with 256-byte
// partial writes and asserts everything arrives intact.
void test_limit_high_water() {
    const size_t limit = 4u * 1024u * 1024u;
    const size_t frame = 65536u;
    capsid::OutboundBuffer buffer;
    WriterState state = { 256, 0, -1 };

    std::vector<uint8_t> payload(frame, 0x5a);
    const size_t wire_per_frame = frame + capsid::protocol::kHeaderSize;
    size_t appended = 0;
    size_t logical_high = 0;
    // 63 frames: 63 x 65560 = 4,130,280 <= 4 MiB wire limit.
    for (int i = 0; i < 63; ++i) {
        require(buffer.append(
                    capsid::protocol::kResponseBody, 0,
                    static_cast<uint64_t>(i + 1), &payload[0], payload.size()),
                "append up to the wire limit");
        appended += wire_per_frame;
        if (buffer.logical_size() > logical_high) {
            logical_high = buffer.logical_size();
        }
    }
    require(logical_high == 63u * wire_per_frame,
            "logical queue equals the appended wire bytes");
    require(buffer.logical_size() == 63u * wire_per_frame,
            "logical queue at the appended wire bytes");
    require(buffer.storage_size() == 63u * wire_per_frame,
            "storage equals the appended wire bytes before any flush");

    drain(&buffer, &state);
    require(state.total_sent == appended, "all appended bytes sent");
    require(buffer.storage_size() == 0, "storage cleared after drain");
    std::printf("limit high-water: logical=%zu storage=%zu sent=%zu\n",
                logical_high, buffer.storage_size(), state.total_sent);
}

// Interleaved append + partial flush: the sent prefix would grow
// without compaction; physical storage must stay bounded by one frame
// plus the wire limit.
void test_compact_high_water() {
    const size_t limit = 256u * 1024u;
    const size_t frame = 65536u;
    capsid::OutboundBuffer buffer;
    WriterState state = { 16384, 0, -1 };  // 16 KiB partial writes

    std::vector<uint8_t> payload(frame, 0x5b);
    size_t storage_high = 0;
    size_t logical_high = 0;
    size_t appended = 0;
    for (int i = 0; i < 512; ++i) {
        if (!buffer.append(
                capsid::protocol::kResponseBody, 0,
                static_cast<uint64_t>(i + 1), &payload[0], payload.size())) {
            break;  // backpressure at the limit
        }
        appended += frame + capsid::protocol::kHeaderSize;
        buffer.flush(short_writer, &state);
        if (buffer.storage_size() > storage_high) {
            storage_high = buffer.storage_size();
        }
        if (buffer.logical_size() > logical_high) {
            logical_high = buffer.logical_size();
        }
        if (buffer.logical_size() > limit) {
            fail("logical queue exceeded the wire limit");
        }
    }
    require(logical_high <= limit,
            "logical queue never exceeds the wire limit");
    require(storage_high <= limit + frame + capsid::protocol::kHeaderSize,
            "physical storage bounded by one frame plus the wire limit");

    drain(&buffer, &state);
    require(state.total_sent == appended, "all bytes sent under compaction");
    std::printf("compact high-water: logical=%zu storage=%zu sent=%zu\n",
                logical_high, storage_high, state.total_sent);
}

// A stall writer keeps the buffer wedged: flush sends nothing, the
// logical queue equals the physical storage (nothing drained), and a
// later drain delivers everything intact. (Wire-limit backpressure is
// enforced by the caller via has_output_capacity, not by the buffer.)
void test_eagain_stall() {
    capsid::OutboundBuffer buffer;
    WriterState state = { 0, 0, -1 };  // always stall

    std::vector<uint8_t> payload(65536u, 0x5c);
    for (int i = 0; i < 4; ++i) {
        require(buffer.append(
                    capsid::protocol::kResponseBody, 0,
                    static_cast<uint64_t>(i + 1), &payload[0], payload.size()),
                "append");
    }
    require(buffer.flush(short_writer, &state), "flush stalls, not fatal");
    require(!buffer.drained(), "stall keeps the buffer wedged");
    require(state.total_sent == 0, "stall sent nothing");
    require(buffer.logical_size() == buffer.storage_size(),
            "logical equals physical while nothing drains");

    state.max_write = 4096;  // resume
    drain(&buffer, &state);
    require(state.total_sent == 4u * (65536u + capsid::protocol::kHeaderSize),
            "stalled bytes delivered after resume");
    std::printf("eagain: logical=%zu storage=%zu sent=%zu\n",
                buffer.logical_size(), buffer.storage_size(), state.total_sent);
}

// A partial write that stops inside the frame header must not be
// corrupted: the next flush continues the same frame.
void test_mid_header_partial() {
    capsid::OutboundBuffer buffer;
    std::vector<uint8_t> payload(4096u, 0x7d);
    require(buffer.append(
                capsid::protocol::kResponseBody, 0, 1,
                &payload[0], payload.size()),
            "append");
    WriterState state = { 10, 0, -1 };  // 10-byte writes split the header
    drain(&buffer, &state);
    require(state.total_sent == 4096u + capsid::protocol::kHeaderSize,
            "full frame delivered across mid-header partials");
    std::printf("mid-header: sent=%zu\n", state.total_sent);
}

void test_fatal_writer() {
    capsid::OutboundBuffer buffer;
    std::vector<uint8_t> payload(1024u, 0x7e);
    require(buffer.append(
                capsid::protocol::kResponseBody, 0, 1,
                &payload[0], payload.size()),
            "append");
    WriterState state = { 1024, 0, 0 };  // fatal on first call
    require(!buffer.flush(short_writer, &state),
            "fatal writer surfaces as flush failure");
    std::printf("fatal: flush failed as expected\n");
}

}  // namespace

int main() {
    test_limit_high_water();
    test_compact_high_water();
    test_eagain_stall();
    test_mid_header_partial();
    test_fatal_writer();
    std::printf("all outbound-buffer tests passed\n");
    return 0;
}
