// Frame-granular outbound buffer with an injectable writer (design
// §3.4 of docs/queue-saturation-activity-fix.md).
//
// The worker owns one instance; unit tests instantiate one directly
// with a short-write / EAGAIN writer to assert logical and physical
// high-water exactly. Physical storage stays bounded by one frame plus
// the wire limit even under sustained partial writes: the sent prefix
// of fully-sent frames is compacted away, and the current frame's
// header is preserved via frame_start_ (a partial write may stop
// mid-header, so the header bytes already sent cannot be dropped).
//
// The buffer is deliberately free of I/O: the writer callback performs
// the actual write, so tests can inject stalls and partial writes
// without touching a socket.
#ifndef CAPSID_SRC_OUTBOUND_BUFFER_H_
#define CAPSID_SRC_OUTBOUND_BUFFER_H_

#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

#include "protocol.h"

namespace capsid {

class OutboundBuffer {
public:
    // Writer contract: write up to `size` bytes and return the number
    // written (>=0), 0 to stall (EAGAIN), or -1 for a fatal error.
    typedef ssize_t (*Writer)(const uint8_t *data, size_t size,
                              void *opaque);

    OutboundBuffer()
        : write_offset_(0),
          frame_end_(0),
          frame_start_(0),
          tail_frame_start_(kNoFrame),
          tail_type_(0),
          tail_flags_(0),
          tail_request_id_(0),
          tail_payload_size_(0) {}

    // Appends one encoded frame. The caller has already checked wire
    // capacity; the frame-size limit is enforced here. The sent prefix
    // is compacted before the append so physical memory stays bounded
    // even under sustained partial writes.
    bool append(uint16_t type,
                uint32_t flags,
                uint64_t request_id,
                const uint8_t *payload,
                size_t payload_size) {
        if (payload_size > capsid::protocol::kMaxPayloadSize) {
            return false;
        }
        compact_if_needed();
        if (try_coalesce_response_body(
                type, flags, request_id, payload, payload_size)) {
            return true;
        }
        const size_t frame_start = storage_.size();
        if (!capsid::protocol::append_encoded(
                type, flags, request_id, payload, payload_size, &storage_)) {
            return false;
        }
        tail_frame_start_ = frame_start;
        tail_type_ = type;
        tail_flags_ = flags;
        tail_request_id_ = request_id;
        tail_payload_size_ = payload_size;
        return true;
    }

    // Sends whole frames, one frame at a time; stops on a stall, a
    // partially buffered trailing frame, or a fatal writer error.
    // Returns false on fatal error only.
    bool flush(Writer writer, void *opaque) {
        while (write_offset_ < storage_.size()) {
            if (write_offset_ >= frame_end_) {
                frame_start_ = write_offset_;
                frame_end_ = next_frame_end(write_offset_);
                if (frame_end_ <= write_offset_) {
                    break;  // trailing frame not fully buffered yet
                }
            }
            const size_t remaining = frame_end_ - write_offset_;
            const ssize_t count =
                writer(&storage_[write_offset_], remaining, opaque);
            if (count > 0) {
                write_offset_ += static_cast<size_t>(count);
                continue;
            }
            if (count == 0) {
                break;  // stall (EAGAIN)
            }
            return false;  // fatal
        }
        if (write_offset_ == storage_.size()) {
            storage_.clear();
            write_offset_ = 0;
            frame_end_ = 0;
            frame_start_ = 0;
            tail_frame_start_ = kNoFrame;
            tail_type_ = 0;
            tail_flags_ = 0;
            tail_request_id_ = 0;
            tail_payload_size_ = 0;
        }
        return true;
    }

    // Bytes not yet sent (the logical wire queue).
    size_t logical_size() const { return storage_.size() - write_offset_; }
    // Physical bytes held (vector capacity is amortized separately).
    size_t storage_size() const { return storage_.size(); }
    bool drained() const { return write_offset_ == storage_.size(); }

private:
    // Joins only adjacent, wholly unsent response-body frames for one
    // request. This avoids another IPC header, write syscall and Host parser
    // event without waiting for a future stream chunk. Once flush() has
    // opened the tail frame (and cached its boundary), its header is immutable
    // even if the first writer call stalls.
    bool try_coalesce_response_body(uint16_t type,
                                    uint32_t flags,
                                    uint64_t request_id,
                                    const uint8_t *payload,
                                    size_t payload_size) {
        if (type != capsid::protocol::kResponseBody ||
            tail_frame_start_ == kNoFrame ||
            tail_type_ != type ||
            tail_flags_ != flags ||
            tail_request_id_ != request_id ||
            tail_frame_start_ < write_offset_ ||
            (frame_end_ != 0 && tail_frame_start_ < frame_end_) ||
            tail_payload_size_ > capsid::protocol::kMaxPayloadSize ||
            payload_size >
                capsid::protocol::kMaxPayloadSize - tail_payload_size_ ||
            tail_frame_start_ + capsid::protocol::kHeaderSize +
                    tail_payload_size_ !=
                storage_.size()) {
            return false;
        }

        const size_t combined_size = tail_payload_size_ + payload_size;
        const size_t size_offset = tail_frame_start_ + 20;
        storage_[size_offset] = static_cast<uint8_t>(combined_size);
        storage_[size_offset + 1] = static_cast<uint8_t>(combined_size >> 8);
        storage_[size_offset + 2] = static_cast<uint8_t>(combined_size >> 16);
        storage_[size_offset + 3] = static_cast<uint8_t>(combined_size >> 24);
        if (payload_size != 0) {
            storage_.insert(storage_.end(), payload, payload + payload_size);
        }
        tail_payload_size_ = combined_size;
        return true;
    }

    // End offset of the complete frame starting at `offset`, or
    // `offset` itself when the frame is not fully buffered yet.
    size_t next_frame_end(size_t offset) const {
        if (offset + capsid::protocol::kHeaderSize > storage_.size()) {
            return offset;
        }
        const uint8_t *header = &storage_[offset];
        // magic(4) version(2) type(2) flags(4) id(8) size(4), LE.
        const uint32_t payload_size =
            static_cast<uint32_t>(header[20]) |
            (static_cast<uint32_t>(header[21]) << 8) |
            (static_cast<uint32_t>(header[22]) << 16) |
            (static_cast<uint32_t>(header[23]) << 24);
        const size_t frame_size =
            capsid::protocol::kHeaderSize + payload_size;
        if (offset + frame_size > storage_.size()) {
            return offset;
        }
        return offset + frame_size;
    }

    // Drops fully-sent frames ([0, frame_start_)). The current frame —
    // whose header a partial write may have half-sent — is preserved
    // from frame_start_ on, so physical memory is bounded by one frame
    // plus the wire limit even when writes keep stalling mid-frame.
    // The threshold batches the memmove.
    void compact_if_needed() {
        if (frame_start_ == 0) {
            return;
        }
        if (frame_start_ >= kCompactThreshold) {
            const size_t removed = frame_start_;
            storage_.erase(
                storage_.begin(),
                storage_.begin() + static_cast<ptrdiff_t>(removed));
            write_offset_ -= removed;
            frame_end_ -= removed;
            frame_start_ = 0;
            if (tail_frame_start_ != kNoFrame) {
                if (tail_frame_start_ >= removed) {
                    tail_frame_start_ -= removed;
                } else {
                    tail_frame_start_ = kNoFrame;
                    tail_payload_size_ = 0;
                }
            }
        }
    }

    static const size_t kCompactThreshold = 64u * 1024u;
    static constexpr size_t kNoFrame =
        std::numeric_limits<size_t>::max();

    std::vector<uint8_t> storage_;
    size_t write_offset_;
    size_t frame_end_;    // end of the frame being flushed (a boundary)
    size_t frame_start_;  // start of the frame being flushed (kept)
    size_t tail_frame_start_;
    uint16_t tail_type_;
    uint32_t tail_flags_;
    uint64_t tail_request_id_;
    size_t tail_payload_size_;
};

}  // namespace capsid

#endif  // CAPSID_SRC_OUTBOUND_BUFFER_H_
