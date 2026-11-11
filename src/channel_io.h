#ifndef CAPSID_CHANNEL_IO_H
#define CAPSID_CHANNEL_IO_H

#include "channel_registry.h"

#include <deque>
#include <list>
#include <map>
#include <memory>
#include <set>

namespace capsid {

// An immutable native payload. Its quota follows the object, including
// when a dispatcher keeps it after removing it from the endpoint's queue.
class ChannelMessage {
public:
    class Chunk {
    public:
        const uint8_t *data() const { return bytes_.get(); }
        size_t size() const { return size_; }
    private:
        friend class ChannelIO;
        std::unique_ptr<uint8_t[]> bytes_;
        size_t size_ = 0;
        std::shared_ptr<void> charge_;
    };
    uint32_t channel_id() const { return entry_.channel_id; }
    uint64_t size() const { return size_; }
    bool text() const { return text_; }
    const ChannelEntry &origin() const { return entry_; }
    const std::list<Chunk> &chunks() const { return chunks_; }
private:
    friend class ChannelIO;
    ChannelEntry entry_;
    bool text_ = false;
    uint64_t size_ = 0;
    std::list<Chunk> chunks_;
};

// Endpoint-specific ownership and bounded queues, above the wire state
// machine and below the JavaScript/network adapters. No borrowed IPC
// payload or JS ArrayBuffer survives a call. This class is single-threaded.
class ChannelIO {
public:
    explicit ChannelIO(const ChannelRegistry::Options &options);
    ChannelIO(const ChannelIO &) = delete;
    ChannelIO &operator=(const ChannelIO &) = delete;
    ChannelRegistry::ApplyResult receive(const protocol::Frame &frame,
                                         bool quarantine_on_pressure = false);
    // Call ONLY after the complete frame was accepted by the bounded IPC
    // writer. A writer returning WOULD_BLOCK must not call commit_local.
    ChannelRegistry::ApplyResult commit_local(const protocol::Frame &frame);
    typedef bool (*Writer)(const protocol::Frame &, void *);
    // Validate/allocate BEFORE the writer sees bytes, then commit without
    // allocation only if it accepted the entire frame. False means retry.
    bool transmit(const protocol::Frame &frame, Writer writer, void *opaque,
                  std::string *error);
    bool transmit_fragment(Writer writer, void *opaque, std::string *error,
                           size_t payload_capacity = protocol::kMaxPayloadSize - 4);
    bool register_authority(uint32_t id, const ChannelAuthority &authority) {
        return registry_.register_authority(id, authority);
    }
    bool has_pending_send(uint32_t id) const { return sending_.count(id) != 0; }
    // Synchronous copy, with deterministic rejection before allocation.
    // Pending message storage stays charged until its final fragment was
    // committed (partial progress does not free the underlying buffer).
    bool queue_message(uint32_t id, const uint8_t *data, size_t size,
                       bool text, std::string *error);
    // Native proxy handover keeps the original receive lease and immutable
    // authority. Outgoing quota is reserved before publishing the send; no
    // intermediate JS buffer or full payload copy is allocated.
    bool forward_message(uint32_t id, std::shared_ptr<const ChannelMessage> message,
                         std::string *error);
    bool next_fragment(protocol::Frame *frame, size_t payload_capacity = protocol::kMaxPayloadSize - 4);
    bool commit_fragment(const protocol::Frame &frame, std::string *error);
    std::shared_ptr<const ChannelMessage> take_message();
    void teardown_all(uint32_t reason);
    bool advance_time(uint64_t elapsed_ms);
    void teardown_handshake(uint64_t request_id) { registry_.teardown_handshake(request_id); }
    uint64_t receive_reserved() const;
    uint64_t send_reserved() const;
    size_t queued_messages() const { return ready_.size(); }
    const ChannelRegistry &registry() const { return registry_; }
private:
    struct Quota;
    struct Send {
        std::unique_ptr<uint8_t[]> bytes;
        std::shared_ptr<const ChannelMessage> message;
        std::shared_ptr<void> charge;
        size_t size = 0;
        size_t offset = 0;
        bool text = false;
    };
    std::shared_ptr<void> reserve(uint32_t id, uint64_t bytes, bool sending);
    void discard_channel(uint32_t id);
    ChannelRegistry::Options options_;
    ChannelRegistry registry_;
    std::shared_ptr<Quota> quota_;
    std::map<uint32_t, std::shared_ptr<ChannelMessage>> assembly_;
    std::deque<std::shared_ptr<ChannelMessage>> ready_;
    std::map<uint32_t, std::deque<Send>> sending_;
    std::set<uint32_t> quarantined_;
    uint32_t cursor_ = 0;
};
} // namespace capsid
#endif
