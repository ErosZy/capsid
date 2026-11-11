#include "channel_io.h"

#include <algorithm>
#include <cstring>
#include <limits>

namespace capsid {
namespace {
// Also charge a fixed allowance for node/message/lease metadata. Empty
// fragments consume this budget, so zero-byte messages cannot grow a queue
// without bound. Payload allocations themselves have an exact byte size.
const uint64_t kFragmentCharge = 256;
const uint64_t kMessageCharge = 512;
const uint64_t kSendCharge = 1024;
bool fits(uint64_t used, uint64_t added, uint64_t limit) {
    return added <= limit && used <= limit - added;
}
}

struct ChannelIO::Quota {
    uint64_t used[2] = {0, 0};
    std::map<std::pair<uint32_t, bool>, uint64_t> channels;
    struct Charge {
        std::shared_ptr<Quota> quota;
        std::pair<uint32_t, bool> key;
        uint64_t bytes = 0;
        bool active = false;
        ~Charge() {
            if (!active) return;
            quota->used[key.second ? 1 : 0] -= bytes;
            auto it = quota->channels.find(key);
            it->second -= bytes;
            if (!it->second) quota->channels.erase(it);
        }
    };
};

ChannelIO::ChannelIO(const ChannelRegistry::Options &options)
    : options_(options), registry_(options), quota_(std::make_shared<Quota>()) {}

std::shared_ptr<void> ChannelIO::reserve(uint32_t id, uint64_t bytes, bool sending) {
    const auto key = std::make_pair(id, sending);
    const auto found = quota_->channels.find(key);
    const uint64_t channel_used = found == quota_->channels.end() ? 0 : found->second;
    const uint64_t ceiling = sending ? options_.send_budget_bytes : options_.reassembly_budget_bytes;
    if (!fits(quota_->used[sending ? 1 : 0], bytes, ceiling) ||
            !fits(channel_used, bytes, options_.per_channel_queue_bytes)) return nullptr;
    auto charge = std::make_shared<Quota::Charge>();
    charge->quota = quota_;
    charge->key = key;
    charge->bytes = bytes;
    auto inserted = quota_->channels.emplace(key, 0);
    inserted.first->second += bytes;
    quota_->used[sending ? 1 : 0] += bytes;
    charge->active = true;
    return charge;
}

void ChannelIO::discard_channel(uint32_t id) {
    assembly_.erase(id);
    sending_.erase(id);
    for (auto it = ready_.begin(); it != ready_.end();) {
        if ((*it)->channel_id() == id) it = ready_.erase(it);
        else ++it;
    }
}

ChannelRegistry::ApplyResult ChannelIO::receive(const protocol::Frame &frame,
                                               bool quarantine_on_pressure) {
    namespace p = protocol;
    ChannelRegistry next = registry_;
    const ChannelSide remote = options_.local_side == ChannelSide::Host ? ChannelSide::Worker : ChannelSide::Host;
    auto result = next.apply(remote, frame);
    if (!result.accepted) return result;
    const uint32_t data_id = frame.type == p::kChannelData ? p::load_u32_at(frame.payload.data()) : 0;
    if (data_id && quarantined_.count(data_id)) result.discard = true;
    if (frame.type == p::kChannelData && !result.discard) {
        const uint32_t id = p::load_u32_at(frame.payload.data());
        const size_t size = frame.payload.size() - 4;
        const bool start = (frame.flags & p::kFlagStart) != 0;
        const bool end = (frame.flags & p::kFlagEnd) != 0;
        ChannelMessage::Chunk chunk;
        chunk.charge_ = reserve(id, uint64_t(size) + kFragmentCharge + (start ? kMessageCharge : 0), false);
        if (!chunk.charge_) {
            if (quarantine_on_pressure) {
                // Keep the validator's fragment/credit progress even when
                // storage is refused. Later continuations remain valid and
                // are drained without allocating or dispatching an event.
                quarantined_.insert(id);
                discard_channel(id);
                registry_ = std::move(next);
                result.discard = true;
                result.resource_exhausted = true;
                return result;
            }
            result.accepted = false;
            result.error = "channel receive budget exceeded";
            return result;
        }
        chunk.bytes_.reset(size ? new uint8_t[size] : nullptr);
        chunk.size_ = size;
        if (size) std::memcpy(chunk.bytes_.get(), frame.payload.data() + 4, size);
        std::shared_ptr<ChannelMessage> message;
        if (start) {
            message = std::make_shared<ChannelMessage>();
            next.find(id, &message->entry_);
            message->text_ = (frame.flags & p::kFlagChanText) != 0;
        } else {
            message = assembly_.at(id);
        }
        // Allocate the completion queue node before mutating an existing
        // assembly. If the chunk-node allocation throws, roll it back.
        if (end) ready_.push_back(message);
        try {
            message->chunks_.push_back(std::move(chunk));
        } catch (...) {
            if (end) ready_.pop_back();
            throw;
        }
        message->size_ += size;
        if (end) assembly_.erase(id);
        else if (start) assembly_.emplace(id, message);
    }
    if ((frame.type == p::kChannelClose || frame.type == p::kChannelCancel ||
         frame.type == p::kChannelAck) && frame.payload.size() >= 4) {
        uint32_t id = p::load_u32_at(frame.payload.data());
        if (id) {
            ChannelEntry entry;
            if (!next.find(id, &entry) || entry.phase == ChannelPhase::Cancelling)
                discard_channel(id);
            else if (frame.type == p::kChannelClose)
                assembly_.erase(id);
            if (!next.find(id, nullptr)) quarantined_.erase(id);
        }
    }
    registry_ = std::move(next);
    return result;
}

bool ChannelIO::transmit(const protocol::Frame &frame, Writer writer, void *opaque,
                         std::string *error) {
    ChannelRegistry next = registry_;
    const auto result = next.apply(options_.local_side, frame);
    if (!result.accepted) { if (error) *error = result.error; return false; }
    if (!writer || !writer(frame, opaque)) return false;
    registry_ = std::move(next);
    if ((frame.type == protocol::kChannelClose || frame.type == protocol::kChannelCancel ||
         frame.type == protocol::kChannelHalfClose) && frame.payload.size() >= 4) {
        const uint32_t id = protocol::load_u32_at(frame.payload.data());
        if (frame.type == protocol::kChannelHalfClose ||
                (frame.type == protocol::kChannelClose && frame.payload[4] != protocol::kCloseTransport &&
                 frame.payload[4] != protocol::kCloseFinal && registry_.find(id, nullptr))) sending_.erase(id);
        else discard_channel(id);
    }
    if (frame.type == protocol::kChannelAck && frame.payload.size() >= 4 &&
            !registry_.find(protocol::load_u32_at(frame.payload.data()), nullptr))
        quarantined_.erase(protocol::load_u32_at(frame.payload.data()));
    return true;
}

bool ChannelIO::transmit_fragment(Writer writer, void *opaque, std::string *error, size_t payload_capacity) {
    protocol::Frame frame;
    if (!next_fragment(&frame, payload_capacity)) return false;
    ChannelRegistry next = registry_;
    const auto result = next.apply(options_.local_side, frame);
    if (!result.accepted) { if (error) *error = result.error; return false; }
    if (!writer || !writer(frame, opaque)) return false;
    registry_ = std::move(next);
    const uint32_t id = protocol::load_u32_at(frame.payload.data());
    auto it = sending_.find(id);
    Send &send = it->second.front();
    send.offset += frame.payload.size() - 4;
    if (send.offset == send.size) {
        it->second.pop_front();
        if (it->second.empty()) sending_.erase(it);
    }
    cursor_ = id;
    return true;
}

ChannelRegistry::ApplyResult ChannelIO::commit_local(const protocol::Frame &frame) {
    ChannelRegistry next = registry_;
    auto result = next.apply(options_.local_side, frame);
    if (result.accepted) registry_ = std::move(next);
    if (result.accepted && (frame.type == protocol::kChannelClose || frame.type == protocol::kChannelCancel) &&
            frame.payload.size() >= 4) {
        const uint32_t id = protocol::load_u32_at(frame.payload.data());
        if (id) {
            if (frame.type == protocol::kChannelClose && frame.payload[4] != protocol::kCloseTransport &&
                    frame.payload[4] != protocol::kCloseFinal && registry_.find(id, nullptr)) sending_.erase(id);
            else discard_channel(id);
        }
    }
    if (result.accepted && frame.type == protocol::kChannelHalfClose)
        sending_.erase(protocol::load_u32_at(frame.payload.data()));
    return result;
}

bool ChannelIO::queue_message(uint32_t id, const uint8_t *data, size_t size,
                             bool text, std::string *error) {
    ChannelEntry entry;
    const int side = static_cast<int>(options_.local_side);
    if ((!data && size) || !registry_.find(id, &entry) ||
            (entry.phase != ChannelPhase::Open && entry.phase != ChannelPhase::Closing) ||
            entry.direction[side].close_sent || entry.direction[side].half_closed ||
            (entry.channel_kind == protocol::kChannelKindHttpBody &&
             (options_.local_side != ChannelSide::Host || text || entry.http_status == 204 ||
              entry.http_status == 205 || entry.http_status == 304))) {
        if (error) *error = "channel sequence";
        return false;
    }
    if (uint64_t(size) > entry.max_message_bytes || uint64_t(size) > std::numeric_limits<uint64_t>::max() - kSendCharge) {
        if (error) *error = "message size exceeded";
        return false;
    }
    Send send;
    send.charge = reserve(id, uint64_t(size) + kSendCharge, true);
    if (!send.charge) {
        if (error) *error = "channel send budget exceeded";
        return false;
    }
    send.bytes.reset(size ? new uint8_t[size] : nullptr);
    send.size = size;
    send.text = text;
    if (size) std::memcpy(send.bytes.get(), data, size);
    auto inserted = sending_.emplace(id, std::deque<Send>());
    try {
        inserted.first->second.push_back(std::move(send));
    } catch (...) {
        if (inserted.second) sending_.erase(inserted.first);
        throw;
    }
    return true;
}

bool ChannelIO::forward_message(uint32_t id, std::shared_ptr<const ChannelMessage> message,
                                std::string *error) {
    ChannelEntry entry;
    const int side = static_cast<int>(options_.local_side);
    if (!message || !registry_.find(id, &entry) ||
            (entry.phase != ChannelPhase::Open && entry.phase != ChannelPhase::Closing) ||
            entry.direction[side].close_sent || entry.direction[side].half_closed ||
            entry.channel_kind != protocol::kChannelKindWebSocket) {
        if (error) *error = "channel sequence";
        return false;
    }
    if (message->size() > entry.max_message_bytes ||
            message->size() > std::numeric_limits<size_t>::max() ||
            message->size() > std::numeric_limits<uint64_t>::max() - kSendCharge) {
        if (error) *error = "message size exceeded";
        return false;
    }
    const uint64_t charge = message->size() + kSendCharge;
    if (charge > options_.send_budget_bytes || charge > options_.per_channel_queue_bytes) {
        if (error) *error = "message exceeds channel send budget";
        return false;
    }
    Send send;
    send.charge = reserve(id, charge, true);
    if (!send.charge) {
        if (error) *error = "channel send budget exceeded";
        return false;
    }
    send.size = static_cast<size_t>(message->size());
    send.text = message->text();
    send.message = std::move(message);
    auto inserted = sending_.emplace(id, std::deque<Send>());
    try { inserted.first->second.push_back(std::move(send)); }
    catch (...) { if (inserted.second) sending_.erase(inserted.first); throw; }
    return true;
}

bool ChannelIO::next_fragment(protocol::Frame *frame, size_t payload_capacity) {
    if (!frame || sending_.empty()) return false;
    auto it = sending_.upper_bound(cursor_);
    for (size_t tried = 0; tried < sending_.size(); ++tried) {
        if (it == sending_.end()) it = sending_.begin();
        ChannelEntry entry;
        if (registry_.find(it->first, &entry) &&
                (entry.phase == ChannelPhase::Open || entry.phase == ChannelPhase::Closing) &&
                !entry.direction[static_cast<int>(options_.local_side)].close_sent &&
                !entry.direction[static_cast<int>(options_.local_side)].half_closed) {
            const Send &send = it->second.front();
            const uint64_t credit = entry.direction[static_cast<int>(options_.local_side)].credit;
            const size_t amount = static_cast<size_t>(std::min<uint64_t>(
                std::min<size_t>(send.size - send.offset, std::min<size_t>(payload_capacity, protocol::kMaxPayloadSize - 4)), credit));
            if (amount || send.size == 0) {
                protocol::Frame next;
                next.type = protocol::kChannelData;
                next.version = protocol::kVersionV4;
                next.flags = (send.offset == 0 ? protocol::kFlagStart : 0) |
                    (send.offset + amount == send.size ? protocol::kFlagEnd : 0) |
                    (send.text ? protocol::kFlagChanText : 0);
                protocol::append_u32(&next.payload, it->first);
                if (amount && send.message) {
                    size_t offset = send.offset, remaining = amount;
                    for (const auto &chunk : send.message->chunks()) {
                        if (offset >= chunk.size()) { offset -= chunk.size(); continue; }
                        const size_t count = std::min(remaining, chunk.size() - offset);
                        next.payload.insert(next.payload.end(), chunk.data() + offset, chunk.data() + offset + count);
                        remaining -= count; offset = 0;
                        if (!remaining) break;
                    }
                    if (remaining) return false;
                } else if (amount) next.payload.insert(next.payload.end(), send.bytes.get() + send.offset,
                    send.bytes.get() + send.offset + amount);
                *frame = std::move(next);
                return true;
            }
        }
        ++it;
    }
    return false;
}

bool ChannelIO::commit_fragment(const protocol::Frame &frame, std::string *error) {
    protocol::Frame expected;
    if (!next_fragment(&expected) || frame.type != expected.type || frame.flags != expected.flags ||
            frame.request_id != 0 || frame.version != expected.version || frame.payload != expected.payload) {
        if (error) *error = "channel sequence";
        return false;
    }
    const auto result = registry_.apply(options_.local_side, frame);
    if (!result.accepted) {
        if (error) *error = result.error;
        return false;
    }
    const uint32_t id = protocol::load_u32_at(frame.payload.data());
    auto it = sending_.find(id);
    Send &send = it->second.front();
    send.offset += frame.payload.size() - 4;
    if (send.offset == send.size) {
        it->second.pop_front();
        if (it->second.empty()) sending_.erase(it);
    }
    cursor_ = id;
    return true;
}

std::shared_ptr<const ChannelMessage> ChannelIO::take_message() {
    if (ready_.empty()) return nullptr;
    auto message = ready_.front();
    ready_.pop_front();
    return message;
}
void ChannelIO::teardown_all(uint32_t reason) {
    registry_.teardown_all(reason);
    quarantined_.clear();
    assembly_.clear();
    ready_.clear();
    sending_.clear();
}
bool ChannelIO::advance_time(uint64_t elapsed_ms) {
    if (registry_.advance_time(elapsed_ms)) return true;
    teardown_all(protocol::kCancelTimeout);
    return false;
}
uint64_t ChannelIO::receive_reserved() const { return quota_->used[0]; }
uint64_t ChannelIO::send_reserved() const { return quota_->used[1]; }
} // namespace capsid
