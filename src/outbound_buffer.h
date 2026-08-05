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
#include <sys/uio.h>
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
        : write_offset_(0), frame_end_(0), frame_start_(0) {}

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
        return capsid::protocol::append_encoded(
            type, flags, request_id, payload, payload_size, &storage_);
    }

    // Writev writer: writes `iovcnt` scatter buffers; returns bytes
    // written (>=0), 0 to stall, or -1 fatal. Production uses writev(2)
    // so several complete frames share one syscall.
    typedef ssize_t (*WritevWriter)(const struct iovec *iov, int iovcnt,
                                    void *opaque);

    static const int kMaxBatchFrames = 16;

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
        }
        return true;
    }

    // Batch variant: collects complete frames into iovecs and issues
    // one writer call per batch. A partial write is located to the
    // containing frame so the next call resumes exactly there (frame
    // boundaries and frame_start_ stay exact).
    bool flushv(WritevWriter writer, void *opaque) {
        while (write_offset_ < storage_.size()) {
            struct iovec iov[kMaxBatchFrames];
            size_t frame_start[kMaxBatchFrames];
            size_t frame_end[kMaxBatchFrames];
            int count = 0;
            size_t offset = write_offset_;
            if (frame_end_ > write_offset_) {
                // Resume the frame a partial write stopped inside; its
                // header and absolute end are preserved, so no re-parse.
                frame_start[0] = frame_start_;
                frame_end[0] = frame_end_;
                iov[0].iov_base = &storage_[write_offset_];
                iov[0].iov_len = frame_end_ - write_offset_;
                offset = frame_end_;
                count = 1;
            }
            while (count < kMaxBatchFrames) {
                const size_t end = next_frame_end(offset);
                if (end <= offset) {
                    break;  // trailing frame not fully buffered
                }
                frame_start[count] = offset;
                frame_end[count] = end;
                iov[count].iov_base = &storage_[offset];
                iov[count].iov_len = end - offset;
                offset = end;
                count += 1;
            }
            if (count == 0) {
                break;
            }
            const ssize_t total = writer(iov, count, opaque);
            if (total > 0) {
                write_offset_ += static_cast<size_t>(total);
                // Locate the frame containing the partial write, so the
                // next call resumes it exactly.
                size_t remaining = static_cast<size_t>(total);
                size_t k = 0;
                while (k < static_cast<size_t>(count) &&
                       remaining >= iov[k].iov_len) {
                    remaining -= iov[k].iov_len;
                    k += 1;
                }
                // remaining == 0: the batch was consumed exactly; no
                // frame is mid-write. remaining > 0 and k < count: the
                // partial write stopped inside frame k, whose boundaries
                // are preserved for the next call.
                if (remaining == 0) {
                    frame_start_ = write_offset_;
                    frame_end_ = 0;
                } else {
                    frame_start_ = frame_start[k];
                    frame_end_ = frame_end[k];
                }
                continue;
            }
            if (total == 0) {
                break;  // stall
            }
            return false;  // fatal
        }
        if (write_offset_ == storage_.size()) {
            storage_.clear();
            write_offset_ = 0;
            frame_end_ = 0;
            frame_start_ = 0;
        }
        return true;
    }

    // Bytes not yet sent (the logical wire queue).
    size_t logical_size() const { return storage_.size() - write_offset_; }
    // Physical bytes held (vector capacity is amortized separately).
    size_t storage_size() const { return storage_.size(); }
    bool drained() const { return write_offset_ == storage_.size(); }

private:
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
            storage_.erase(
                storage_.begin(),
                storage_.begin() + static_cast<ptrdiff_t>(frame_start_));
            write_offset_ -= frame_start_;
            frame_end_ -= frame_start_;
            frame_start_ = 0;
        }
    }

    static const size_t kCompactThreshold = 64u * 1024u;

    std::vector<uint8_t> storage_;
    size_t write_offset_;
    size_t frame_end_;    // end of the frame being flushed (a boundary)
    size_t frame_start_;  // start of the frame being flushed (kept)
};

}  // namespace capsid

#endif  // CAPSID_SRC_OUTBOUND_BUFFER_H_
