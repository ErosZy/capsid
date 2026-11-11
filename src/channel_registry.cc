#include "channel_registry.h"

#include <algorithm>
#include <limits>

namespace capsid {

namespace {

bool checked_add_u64(uint64_t a, uint64_t b, uint64_t *out) {
    if (b > std::numeric_limits<uint64_t>::max() - a) {
        return false;
    }
    *out = a + b;
    return true;
}

uint64_t deadline_after(uint64_t now, uint32_t delay) {
    uint64_t deadline = 0;
    return checked_add_u64(now, delay, &deadline)
        ? deadline : std::numeric_limits<uint64_t>::max();
}

}  // namespace

ChannelRegistry::ChannelRegistry() : options_() {}

ChannelRegistry::ChannelRegistry(const Options &options)
    : options_(options) {}

// ---- wire stream ----------------------------------------------------

ChannelRegistry::ApplyResult ChannelRegistry::apply(ChannelSide from,
                                                    const protocol::Frame
                                                        &frame) {
    namespace p = protocol;
    ApplyResult result;
    if (expired_) {
        result.error = "channel confirmation timeout";
        return result;
    }
    if (!hello_seen_) {
        if (frame.version != p::kVersionV4) {
            result.error = "unsupported protocol version";
            return result;
        }
        if (frame.type != p::kHello) {
            result.error = "first frame must be HELLO";
            return result;
        }
        if (from != ChannelSide::Host) {
            result.error = "first frame must be HELLO";
            return result;
        }
        return apply_hello(frame);
    }
    if (frame.version != wire_version_) {
        result.error = "unsupported protocol version";
        return result;
    }
    if (frame.type == p::kHello) {
        result.error = "duplicate HELLO";
        return result;
    }
    if (frame.type >= p::kChannelOpen && frame.type <= p::kConnectionLifetime &&
            frame.request_id != 0) {
        result.error = "invalid channel payload";
        return result;
    }
    if (!ready_seen_) {
        if (frame.type != p::kReady) {
            result.error = "expected READY";
            return result;
        }
        if (from != ChannelSide::Worker) {
            result.error = "expected READY";
            return result;
        }
        return apply_ready(frame);
    }
    switch (frame.type) {
    case p::kReady:
        result.error = "duplicate READY";
        return result;
    case p::kChannelOpen:
        return apply_open(from, frame);
    case p::kChannelAccept:
        return apply_accept(from, frame);
    case p::kChannelReject:
        return apply_reject(from, frame);
    case p::kChannelData:
        return apply_data(from, frame);
    case p::kChannelCredit:
        return apply_credit(from, frame);
    case p::kChannelHalfClose:
        return apply_half_close(from, frame);
    case p::kChannelClose:
        return apply_close(from, frame);
    case p::kChannelCancel:
        return apply_cancel(from, frame);
    case p::kChannelAck:
        return apply_ack(from, frame);
    case p::kConnectionLifetime:
        return apply_connection_lifetime(from, frame);
    case p::kRequestHead:
        return apply_request_head(from, frame);
    case p::kResponseHead:
        return apply_response_head(from, frame);
    default:
        result.accepted = true;  // carriage frames pass through
        return result;
    }
}

ChannelRegistry::ApplyResult ChannelRegistry::apply_hello(
    const protocol::Frame &frame) {
    namespace p = protocol;
    ApplyResult result;
    if (frame.payload.size() < p::kHelloV4TailSize) {
        result.error = "invalid hello capabilities";
        return result;
    }
    const uint8_t *tail =
        frame.payload.data() + frame.payload.size() -
        p::kHelloV4TailSize;
    const uint32_t caps = p::load_u32_at(tail);
    const uint32_t reserved = p::load_u32_at(tail + 4);
    if (reserved != 0 || !p::capabilities_satisfy_closure(caps)) {
        result.error = "invalid hello capabilities";
        return result;
    }
    announced_[static_cast<int>(ChannelSide::Host)] = caps;
    hello_seen_ = true;
    wire_version_ = frame.version;
    result.accepted = true;
    return result;
}

ChannelRegistry::ApplyResult ChannelRegistry::apply_ready(
    const protocol::Frame &frame) {
    namespace p = protocol;
    ApplyResult result;
    if (frame.payload.size() < p::kHelloV4TailSize) {
        result.error = "invalid hello capabilities";
        return result;
    }
    const uint8_t *tail =
        frame.payload.data() + frame.payload.size() -
        p::kHelloV4TailSize;
    const uint32_t caps = p::load_u32_at(tail);
    const uint32_t reserved = p::load_u32_at(tail + 4);
    if (reserved != 0 || !p::capabilities_satisfy_closure(caps)) {
        result.error = "invalid hello capabilities";
        return result;
    }
    announced_[static_cast<int>(ChannelSide::Worker)] = caps;
    effective_caps_ = announced_[0] & announced_[1];
    ready_seen_ = true;
    result.accepted = true;
    return result;
}

ChannelEntry *ChannelRegistry::live_entry(uint32_t channel_id,
                                          std::string *error) {
    std::map<uint32_t, ChannelEntry>::iterator it =
        channels_.find(channel_id);
    if (it == channels_.end() ||
            phase_terminal(it->second.phase)) {
        if (error) {
            *error = "unknown channel";
        }
        return nullptr;
    }
    return &it->second;
}

void ChannelRegistry::release_entry_reservations(
    const ChannelEntry &entry) {
    for (int side = 0; side < 2; ++side) {
        if (entry.direction[side].message_open) {
            // reserved bytes return to the aggregate budget
            reassembly_reserved_ -=
                entry.direction[side].message_bytes;
        }
    }
}

void ChannelRegistry::mark_terminal(ChannelEntry *entry,
                                    ChannelPhase phase) {
    if (phase_terminal(entry->phase)) {
        return;
    }
    release_entry_reservations(*entry);
    const uint32_t id = entry->channel_id;
    (void)phase;
    channels_.erase(id);
    terminal_deadlines_.erase(id);
}

void ChannelRegistry::release_direction(ChannelEntry *entry, int side) {
    ChannelDirection &direction = entry->direction[side];
    if (direction.message_open) reassembly_reserved_ -= direction.message_bytes;
    direction.message_open = false;
    direction.message_bytes = 0;
}

bool ChannelRegistry::terminal_complete(const ChannelEntry &entry) const {
    bool requested = false;
    for (int side = 0; side < 2; ++side) {
        if (entry.direction[side].terminal_sent) {
            requested = true;
            if (!entry.direction[side].terminal_acked) return false;
        }
    }
    return requested;
}

ChannelRegistry::ApplyResult ChannelRegistry::apply_open(
    ChannelSide from, const protocol::Frame &f) {
    namespace p = protocol;
    ApplyResult result;
    if (!has_capability(p::kCapDuplexChannel)) {
        result.error = "channel frame without capability";
        return result;
    }
    if (from != ChannelSide::Worker) {
        result.error = "channel open direction";
        return result;
    }
    p::ChannelOpenPayload open;
    if (!p::decode_channel_open(f.payload.data(), f.payload.size(),
                                &open, &result.error, effective_caps_)) {
        return result;
    }
    if (open.channel_id != 0 || open.dial_id == 0) {
        result.error = "channel open direction";
        return result;
    }
    if (open.dial_id <= last_dial_id_) {
        result.error = "channel sequence";
        return result;
    }
    for (const auto &channel : channels_) {
        if (channel.second.dial_id == open.dial_id) {
            result.error = "channel sequence";
            return result;
        }
    }
    // Bound pending correlation state as well as admitted channels.
    if (pending_dials_.size() >= options_.max_channels) {
        result.error = "channel sequence";
        return result;
    }
    // Refused dials hold only bounded correlation state until REJECT;
    // they reserve no live channel or message buffer. This lets the
    // host send a legal rejection without admitting an over-budget dial.
    if (open.owner_kind == p::kOwnerTailTask ||
            live_count() >= options_.max_channels ||
            std::min(open.max_message_bytes, options_.max_message_bytes) >
                options_.reassembly_budget_bytes) {
        result.accepted = true;
        result.dial_should_reject = true;
        result.dial_reject_reason =
            open.owner_kind == p::kOwnerTailTask
                ? 0  // unspecified: unsupported owner in v0.3.0
                : 8;  // resource-limits
    }
    ChannelEntry entry;
    entry.channel_id = 0;
    entry.dial_id = open.dial_id;
    entry.owner_kind = open.owner_kind;
    entry.channel_kind = open.channel_kind;
    entry.fetch_upgrade = open.channel_kind == p::kChannelKindFetchUpgrade;
    entry.owner_request_id = open.owner_request_id;
    entry.max_message_bytes =
        std::min(open.max_message_bytes, options_.max_message_bytes);
    entry.phase = ChannelPhase::Dialing;
    PendingDial pending;
    pending.entry = entry;
    pending.must_reject = result.dial_should_reject;
    pending.deadline = deadline_after(clock_ms_, options_.dial_timeout_ms);
    pending_dials_.emplace(open.dial_id, pending);
    last_dial_id_ = open.dial_id;
    result.accepted = true;
    return result;
}

bool ChannelRegistry::pending_dial(uint64_t id, ChannelEntry *entry,
                                   bool *cancelled, bool *unresolved) const {
    auto found = pending_dials_.find(id);
    if (found == pending_dials_.end()) return false;
    if (entry) *entry = found->second.entry;
    if (cancelled) *cancelled = found->second.cancelled;
    if (unresolved) *unresolved = found->second.result == PendingDial::Unresolved;
    return true;
}
bool ChannelRegistry::dial_must_reject(uint64_t id, uint32_t *reason) const {
    auto found=pending_dials_.find(id);
    if(found==pending_dials_.end() || !found->second.must_reject) return false;
    if(reason) *reason=found->second.entry.owner_kind==protocol::kOwnerTailTask ? 0 : 8;
    return true;
}

ChannelRegistry::ApplyResult ChannelRegistry::apply_accept(
    ChannelSide from, const protocol::Frame &f) {
    namespace p = protocol;
    ApplyResult result;
    if (!has_capability(p::kCapDuplexChannel)) {
        result.error = "channel frame without capability";
        return result;
    }
    if (from != ChannelSide::Host) {
        result.error = "channel open direction";
        return result;
    }
    p::ChannelAcceptPayload accept;
    if (!p::decode_channel_accept(f.payload.data(), f.payload.size(),
                                  &accept, &result.error, effective_caps_)) {
        return result;
    }
    auto it = pending_dials_.find(accept.dial_id);
    if (it == pending_dials_.end()) {
        result.error = "unknown channel";
        return result;
    }
    ChannelEntry entry = it->second.entry;
    // Optional metadata belongs to a particular negotiated fetch OPEN.
    // It must neither disappear from that result nor attach to an ordinary
    // WebSocket dial, even when both endpoints support the capability.
    if (entry.fetch_upgrade != accept.has_http_response) {
        result.error = "invalid channel payload";
        return result;
    }
    if (it->second.must_reject || it->second.result != PendingDial::Unresolved ||
            (it->second.cancelled && options_.local_side == ChannelSide::Host)) {
        result.error = "channel sequence";
        return result;
    }
    if (accept.channel_id <= last_channel_id_) {
        result.error = "channel sequence";
        return result;
    }
    entry.channel_id = accept.channel_id;
    if (entry.fetch_upgrade) {
        entry.http_status = accept.http_response.status;
        entry.channel_kind = static_cast<uint8_t>(accept.http_response.status == 101
            ? p::kChannelKindWebSocket : p::kChannelKindHttpBody);
    }
    entry.max_message_bytes =
        std::min(entry.max_message_bytes,
                 accept.accepted_max_message_bytes);
    const bool confirm = has_capability(p::kCapChannelConfirm);
    entry.phase = confirm ? ChannelPhase::AwaitingAck : ChannelPhase::Open;
    if (it->second.cancelled) {
        entry.phase = ChannelPhase::Cancelling;
        entry.direction[1].terminal_sent = true;
    }
    channels_.emplace(accept.channel_id, entry);
    last_channel_id_ = accept.channel_id;
    result.discard = it->second.cancelled;
    if (confirm) {
        it->second.entry = entry;
        it->second.result = PendingDial::Accepted;
        if (!it->second.cancelled)
            it->second.deadline = deadline_after(clock_ms_, options_.close_timeout_ms);
    } else {
        pending_dials_.erase(it);
    }
    result.accepted = true;
    return result;
}

ChannelRegistry::ApplyResult ChannelRegistry::apply_reject(
    ChannelSide from, const protocol::Frame &f) {
    namespace p = protocol;
    ApplyResult result;
    if (!has_capability(p::kCapDuplexChannel)) {
        result.error = "channel frame without capability";
        return result;
    }
    if (from != ChannelSide::Host) {
        result.error = "channel open direction";
        return result;
    }
    p::ChannelRejectPayload reject;
    if (!p::decode_channel_reject(f.payload.data(), f.payload.size(),
                                  &reject, &result.error)) {
        return result;
    }
    auto it = pending_dials_.find(reject.dial_id);
    if (it == pending_dials_.end()) {
        result.error = "unknown channel";
        return result;
    }
    if (reject.channel_id != 0) {
        result.error = "invalid channel payload";
        return result;
    }
    if (it->second.result != PendingDial::Unresolved ||
            (it->second.cancelled && options_.local_side == ChannelSide::Host)) {
        result.error = "channel sequence";
        return result;
    }
    result.discard = it->second.cancelled;
    if (has_capability(p::kCapChannelConfirm)) {
        it->second.result = PendingDial::Rejected;
        if (!it->second.cancelled)
            it->second.deadline = deadline_after(clock_ms_, options_.close_timeout_ms);
    } else {
        pending_dials_.erase(it);
    }
    result.accepted = true;
    return result;
}

ChannelRegistry::ApplyResult ChannelRegistry::apply_data(
    ChannelSide from, const protocol::Frame &f) {
    namespace p = protocol;
    ApplyResult result;
    if (!has_capability(p::kCapDuplexChannel)) {
        result.error = "channel frame without capability";
        return result;
    }
    uint32_t id = 0;
    if (!p::decode_channel_data_id(f.payload.data(), f.payload.size(),
                                   &id, &result.error)) {
        return result;
    }
    ChannelEntry *entry = live_entry(id, &result.error);
    if (entry == nullptr) {
        return result;
    }
    if (entry->phase != ChannelPhase::Open &&
            entry->phase != ChannelPhase::Closing &&
            entry->phase != ChannelPhase::Cancelling) {
        result.error = "channel sequence";
        return result;
    }
    const int side = static_cast<int>(from);
    if (entry->direction[side].close_sent ||
            entry->direction[side].half_closed ||
            entry->direction[side].terminal_sent ||
            entry->direction[side].terminal_ack_sent ||
            (from == options_.local_side && entry->phase == ChannelPhase::Cancelling)) {
        result.error = "channel sequence";
        return result;
    }
    result.discard = entry->phase == ChannelPhase::Cancelling;
    const uint64_t message_bytes =
        f.payload.size() - sizeof(uint32_t);
    const bool start = (f.flags & p::kFlagStart) != 0;
    const bool end = (f.flags & p::kFlagEnd) != 0;
    const bool text = (f.flags & p::kFlagChanText) != 0;
    if (entry->channel_kind == p::kChannelKindHttpBody &&
            (from != ChannelSide::Host || text || entry->http_status == 204 ||
             entry->http_status == 205 || entry->http_status == 304)) {
        result.error = "channel sequence";
        return result;
    }
    ChannelDirection &sender = entry->direction[side];
    // fragmentation first, then cap, then credit (the reference
    // sequence order from T05)
    if (!start && !sender.message_open) {
        result.error = "fragmentation";
        return result;
    }
    if (start && sender.message_open) {
        result.error = "fragmentation";
        return result;
    }
    uint64_t accumulated = 0;
    if (!checked_add_u64(start ? 0 : sender.message_bytes,
                         message_bytes, &accumulated)) {
        result.error = "message size exceeded";
        return result;
    }
    if (accumulated > entry->max_message_bytes) {
        result.error = "message size exceeded";
        return result;
    }
    if (!start && text != sender.message_text) {
        result.error = "fragmentation";
        return result;
    }
    if (sender.credit < message_bytes) {
        result.error = "channel credit underflow";
        return result;
    }
    // reserve BEFORE mutating: the aggregate reassembly budget must
    // cover every held fragment; a completed message releases its
    // accumulated bytes on the spot
    uint64_t after = 0;
    if (!checked_add_u64(reassembly_reserved_, message_bytes, &after) ||
            after > options_.reassembly_budget_bytes) {
        result.error = "message size exceeded";
        return result;
    }
    // all checks passed: commit the mutation
    reassembly_reserved_ = after;
    sender.credit -= message_bytes;
    if (start) {
        sender.message_open = true;
        sender.message_text = text;
        sender.message_bytes = message_bytes;
    } else {
        sender.message_bytes = accumulated;
    }
    if (end) {
        sender.message_open = false;
        reassembly_reserved_ -= sender.message_bytes;
        bytes_reassembled_ += sender.message_bytes;
        sender.message_bytes = 0;
        ++messages_completed_;
    }
    result.accepted = true;
    return result;
}

ChannelRegistry::ApplyResult ChannelRegistry::apply_credit(
    ChannelSide from, const protocol::Frame &f) {
    namespace p = protocol;
    ApplyResult result;
    if (!has_capability(p::kCapDuplexChannel)) {
        result.error = "channel frame without capability";
        return result;
    }
    p::ChannelCreditPayload grant;
    if (!p::decode_channel_credit(f.payload.data(), f.payload.size(),
                                  &grant, &result.error)) {
        return result;
    }
    ChannelEntry *entry = live_entry(grant.channel_id, &result.error);
    if (entry == nullptr) {
        return result;
    }
    if (entry->channel_kind == p::kChannelKindHttpBody && from != ChannelSide::Worker) {
        result.error = "channel sequence";
        return result;
    }
    const ChannelDirection &emitter = entry->direction[static_cast<int>(from)];
    if (entry->phase == ChannelPhase::AwaitingAck || entry->phase == ChannelPhase::Pending ||
            emitter.close_sent || emitter.terminal_sent || emitter.terminal_ack_sent ||
            (from == options_.local_side && entry->phase == ChannelPhase::Cancelling)) {
        result.error = "channel sequence";
        return result;
    }
    result.discard = entry->phase == ChannelPhase::Cancelling;
    const ChannelSide beneficiary =
        from == ChannelSide::Host ? ChannelSide::Worker : ChannelSide::Host;
    uint64_t after = 0;
    if (!checked_add_u64(
            entry->direction[static_cast<int>(beneficiary)].credit,
            grant.credit, &after)) {
        result.error = "channel credit overflow";
        return result;
    }
    entry->direction[static_cast<int>(beneficiary)].credit = after;
    result.accepted = true;
    return result;
}

ChannelRegistry::ApplyResult ChannelRegistry::apply_half_close(
    ChannelSide from, const protocol::Frame &f) {
    namespace p = protocol;
    ApplyResult result;
    if (!has_capability(p::kCapHalfClose)) {
        result.error = "channel frame without capability";
        return result;
    }
    uint32_t id = 0;
    if (!p::decode_channel_data_id(f.payload.data(), f.payload.size(),
                                   &id, &result.error) ||
            f.payload.size() != sizeof(uint32_t)) {
        result.error = "invalid channel payload";
        return result;
    }
    ChannelEntry *entry = live_entry(id, &result.error);
    if (entry == nullptr) {
        return result;
    }
    const int side = static_cast<int>(from);
    if (entry->channel_kind == p::kChannelKindHttpBody ||
            (entry->phase != ChannelPhase::Open && entry->phase != ChannelPhase::Closing &&
            entry->phase != ChannelPhase::Cancelling) ||
            entry->direction[side].half_closed ||
            entry->direction[side].close_sent ||
            entry->direction[side].terminal_sent ||
            entry->direction[side].terminal_ack_sent ||
            (from == options_.local_side && entry->phase == ChannelPhase::Cancelling) ||
            entry->direction[side].message_open) {
        result.error = "channel sequence";
        return result;
    }
    entry->direction[side].half_closed = true;
    if (!has_capability(p::kCapChannelConfirm) && entry->phase != ChannelPhase::Cancelling &&
            (entry->direction[0].half_closed || entry->direction[0].close_sent) &&
            (entry->direction[1].half_closed || entry->direction[1].close_sent)) {
        mark_terminal(entry, ChannelPhase::Closed);
    }
    result.accepted = true;
    return result;
}

ChannelRegistry::ApplyResult ChannelRegistry::apply_close(
    ChannelSide from, const protocol::Frame &f) {
    namespace p = protocol;
    ApplyResult result;
    if (!has_capability(p::kCapDuplexChannel)) {
        result.error = "channel frame without capability";
        return result;
    }
    p::ChannelClosePayload close;
    if (!p::decode_channel_close(f.payload.data(), f.payload.size(),
                                 &close, &result.error)) {
        return result;
    }
    if (close.close_kind == p::kCloseFinal && !has_capability(p::kCapChannelCloseFinal)) {
        result.error = "channel frame without capability";
        return result;
    }
    ChannelEntry *entry = live_entry(close.channel_id, &result.error);
    if (entry == nullptr) {
        return result;
    }
    const int side = static_cast<int>(from);
    if (entry->channel_kind == p::kChannelKindHttpBody &&
            (from != ChannelSide::Host ||
             (close.close_kind != p::kCloseFinal && close.close_kind != p::kCloseTransport) ||
             (close.close_kind == p::kCloseFinal &&
              (entry->direction[side].message_open || close.flags || close.code || !close.reason.empty())))) {
        result.error = "channel sequence";
        return result;
    }
    const bool confirm_transport = (close.close_kind == p::kCloseTransport || close.close_kind == p::kCloseFinal) &&
        has_capability(p::kCapChannelConfirm);
    if (entry->direction[side].close_sent || entry->direction[side].terminal_sent ||
            entry->direction[side].terminal_ack_sent || entry->phase == ChannelPhase::AwaitingAck ||
            (from == options_.local_side && entry->phase == ChannelPhase::Cancelling)) {
        result.error = "channel sequence";
        return result;
    }
    entry->direction[side].close_sent = true;
    release_direction(entry, side);
    if (confirm_transport) {
        entry->direction[side].terminal_sent = true;
        entry->direction[side].terminal_kind = p::kAckTransportClose;
        entry->phase = ChannelPhase::Cancelling;
        terminal_deadlines_.emplace(entry->channel_id,
            deadline_after(clock_ms_, options_.close_timeout_ms));
    } else if ((close.close_kind == p::kCloseTransport) ||
            (entry->phase != ChannelPhase::Cancelling &&
             (has_capability(p::kCapChannelConfirm)
                ? (entry->direction[0].close_sent && entry->direction[1].close_sent)
                : ((entry->direction[0].close_sent || entry->direction[0].half_closed) &&
                   (entry->direction[1].close_sent || entry->direction[1].half_closed))))) {
        mark_terminal(entry, ChannelPhase::Closed);
    } else {
        // Close ends only its SENDER's stream. The opposite direction
        // may still contain a fragmented message queued before it knew
        // about this Close (RFC 6455 sections 5.1 and 5.5.1).
        if (entry->phase != ChannelPhase::Cancelling) entry->phase = ChannelPhase::Closing;
        terminal_deadlines_.emplace(entry->channel_id,
            deadline_after(clock_ms_, options_.close_timeout_ms));
    }
    result.accepted = true;
    return result;
}

ChannelRegistry::ApplyResult ChannelRegistry::apply_cancel(
    ChannelSide from, const protocol::Frame &f) {
    namespace p = protocol;
    ApplyResult result;
    if (!has_capability(p::kCapDuplexChannel)) {
        result.error = "channel frame without capability";
        return result;
    }
    p::ChannelCancelPayload cancel;
    if (!p::decode_channel_cancel(f.payload.data(), f.payload.size(),
                                  &cancel, &result.error)) {
        return result;
    }
    if (cancel.channel_id == 0) {
        if (!has_capability(p::kCapChannelConfirm)) {
            result.error = "channel frame without capability";
            return result;
        }
        if (from != ChannelSide::Worker) {
            result.error = "channel open direction";
            return result;
        }
        auto dial = pending_dials_.find(cancel.dial_id);
        if (dial == pending_dials_.end()) {
            result.error = "unknown channel";
            return result;
        }
        if (dial->second.cancelled ||
                (options_.local_side == ChannelSide::Worker &&
                 dial->second.result != PendingDial::Unresolved)) {
            result.error = "channel sequence";
            return result;
        }
        dial->second.cancelled = true;
        dial->second.deadline = deadline_after(clock_ms_, options_.close_timeout_ms);
        if (dial->second.result == PendingDial::Accepted) {
            ChannelEntry &entry = channels_.at(dial->second.entry.channel_id);
            entry.phase = ChannelPhase::Cancelling;
            entry.direction[1].terminal_sent = true;
        }
        result.accepted = true;
        return result;
    }
    ChannelEntry *entry = live_entry(cancel.channel_id, &result.error);
    if (entry == nullptr) {
        return result;
    }
    if (!has_capability(p::kCapChannelConfirm)) {
        mark_terminal(entry, ChannelPhase::Cancelled);
    } else {
        const int side = static_cast<int>(from);
        if (entry->phase == ChannelPhase::AwaitingAck ||
                entry->direction[side].terminal_sent || entry->direction[side].terminal_ack_sent ||
                entry->direction[side].close_sent) {
            result.error = "channel sequence";
            return result;
        }
        entry->direction[side].terminal_sent = true;
        entry->direction[side].terminal_kind = p::kAckCancel;
        entry->direction[side].terminal_reason = cancel.terminal_reason;
        release_direction(entry, side);
        entry->phase = ChannelPhase::Cancelling;
        terminal_deadlines_.emplace(entry->channel_id,
            deadline_after(clock_ms_, options_.close_timeout_ms));
    }
    result.accepted = true;
    return result;
}

ChannelRegistry::ApplyResult ChannelRegistry::apply_ack(
    ChannelSide from, const protocol::Frame &f) {
    namespace p = protocol;
    ApplyResult result;
    if (!has_capability(p::kCapChannelConfirm)) {
        result.error = "channel frame without capability";
        return result;
    }
    p::ChannelAckPayload ack;
    if (!p::decode_channel_ack(f.payload.data(), f.payload.size(), &ack, &result.error))
        return result;
    if (ack.kind == p::kAckDialResult || (ack.kind == p::kAckCancel && ack.dial_id != 0)) {
        if ((ack.kind == p::kAckDialResult && from != ChannelSide::Worker) ||
                (ack.kind == p::kAckCancel && from != ChannelSide::Host)) {
            result.error = "channel open direction";
            return result;
        }
        auto dial = pending_dials_.find(ack.dial_id);
        if (dial == pending_dials_.end()) {
            result.error = "unknown channel";
            return result;
        }
        const PendingDial &pending = dial->second;
        if (ack.kind == p::kAckDialResult) {
            const uint32_t expected = pending.result == PendingDial::Accepted
                ? pending.entry.channel_id : 0;
            if (pending.cancelled || pending.result == PendingDial::Unresolved || ack.channel_id != expected) {
                result.error = "channel sequence";
                return result;
            }
            if (expected != 0) channels_.at(expected).phase = ChannelPhase::Open;
        } else {
            if (!pending.cancelled) {
                result.error = "channel sequence";
                return result;
            }
            if (pending.result == PendingDial::Accepted) {
                ChannelEntry *entry = live_entry(pending.entry.channel_id, &result.error);
                if (!entry) return result;
                mark_terminal(entry, ChannelPhase::Cancelled);
            }
        }
        pending_dials_.erase(dial);
    } else {
        ChannelEntry *entry = live_entry(ack.channel_id, &result.error);
        if (!entry) return result;
        const int side = static_cast<int>(from);
        ChannelDirection &requester = entry->direction[1 - side];
        if (!requester.terminal_sent || requester.terminal_acked || requester.terminal_kind != ack.kind) {
            result.error = "channel sequence";
            return result;
        }
        requester.terminal_acked = true;
        entry->direction[side].terminal_ack_sent = true;
        release_direction(entry, side);
        if (terminal_complete(*entry)) mark_terminal(entry, ChannelPhase::Cancelled);
    }
    result.accepted = true;
    return result;
}

ChannelRegistry::ApplyResult ChannelRegistry::apply_connection_lifetime(
    ChannelSide from, const protocol::Frame &f) {
    namespace p=protocol;ApplyResult result;
    if (!has_capability(p::kCapConnectionLifetime)) {
        result.error="channel frame without capability";return result;
    }
    if (from!=ChannelSide::Worker || f.flags || f.request_id) {
        result.error="invalid connection lifetime";return result;
    }
    p::ConnectionLifetimePayload lifetime;
    if (!p::decode_connection_lifetime(f.payload.data(),f.payload.size(),&lifetime,&result.error))return result;
    if (lifetime.retained) {
        const auto found=channels_.find(lifetime.channel_id);
        if (found==channels_.end() || found->second.dial_id ||
                found->second.owner_kind!=p::kOwnerDuplexConnection ||
                !found->second.direction[0].terminal_sent ||
                found->second.direction[0].terminal_acked ||
                found->second.direction[0].terminal_kind!=p::kAckCancel ||
                found->second.direction[0].terminal_reason!=p::kCancelDisconnect ||
                retained_owners_.count(lifetime.channel_id) ||
                uint64_t(retained_owners_.size())>=uint64_t(options_.max_channels)*2) {
            result.error="invalid connection lifetime";return result;
        }
        retained_owners_.insert(lifetime.channel_id);
    } else {
        const auto found=retained_owners_.find(lifetime.channel_id);
        if (found==retained_owners_.end()) {result.error="invalid connection lifetime";return result;}
        retained_owners_.erase(found);
    }
    result.accepted=true;return result;
}

ChannelRegistry::ApplyResult ChannelRegistry::apply_request_head(
    ChannelSide from, const protocol::Frame &f) {
    namespace p = protocol;
    ApplyResult result;
    if ((f.flags & p::kFlagRequestUpgrade) == 0) {
        result.accepted = true;   // ordinary request, not upgrade
        return result;
    }
    if (!has_capability(p::kCapUpgrade)) {
        result.error = "channel frame without capability";
        return result;
    }
    if (from != ChannelSide::Host) {
        result.error = "invalid upgrade response";
        return result;
    }
    p::UpgradeTail tail;
    if (!p::decode_upgrade_head(f, &tail, &result.error)) {
        return result;
    }
    if (tail.channel_id <= last_channel_id_) {
        result.error = "channel sequence";
        return result;
    }
    if (live_count() >= options_.max_channels ||
            std::min(tail.max_message_bytes, options_.max_message_bytes) >
                options_.reassembly_budget_bytes) {
        // reservation impossible under the budgets: the host must not
        // have offered the upgrade; treat as a wire violation
        result.error = "channel sequence";
        return result;
    }
    ChannelEntry entry;
    entry.channel_id = tail.channel_id;
    entry.owner_kind = p::kOwnerHandshake;
    entry.owner_request_id = f.request_id;
    entry.max_message_bytes =
        std::min(tail.max_message_bytes, options_.max_message_bytes);
    entry.phase = ChannelPhase::Pending;
    channels_[tail.channel_id] = entry;
    last_channel_id_ = tail.channel_id;
    result.accepted = true;
    return result;
}

ChannelRegistry::ApplyResult ChannelRegistry::apply_response_head(
    ChannelSide from, const protocol::Frame &f) {
    namespace p = protocol;
    ApplyResult result;
    const bool upgrade_flag =
        (f.flags & p::kFlagResponseUpgrade) != 0;
    const bool fixed_body =
        (f.flags & p::kFlagResponseFixedBody) != 0;
    if (!upgrade_flag) {
        if (f.payload.size() < sizeof(uint32_t)) {
            result.accepted = true;   // carriage response, pass through
            return result;
        }
        const uint16_t status = p::load_u16_at(f.payload.data());
        if (status == 101) {
            result.error = "invalid upgrade response";
            return result;
        }
        // a non-101 answer to an upgrade-flagged request releases the
        // pending reservation for the same request id (contract §2)
        std::vector<uint32_t> released;
        for (std::map<uint32_t, ChannelEntry>::iterator it =
                 channels_.begin();
             it != channels_.end(); ++it) {
            if (it->second.phase == ChannelPhase::Pending &&
                    it->second.owner_request_id == f.request_id) {
                released.push_back(it->first);
            }
        }
        for (size_t i = 0; i < released.size(); ++i) {
            channels_.erase(released[i]);
        }
        result.accepted = true;
        return result;
    }
    if (!has_capability(p::kCapUpgrade)) {
        result.error = "channel frame without capability";
        return result;
    }
    if (from != ChannelSide::Worker) {
        result.error = "invalid upgrade response";
        return result;
    }
    if (fixed_body) {
        result.error = "invalid upgrade response";
        return result;
    }
    if (f.payload.size() < sizeof(uint32_t) +
            sizeof(uint32_t) + sizeof(uint64_t)) {
        result.error = "invalid channel payload";
        return result;
    }
    const uint16_t status = p::load_u16_at(f.payload.data());
    if (status != 101) {
        result.error = "invalid upgrade response";
        return result;
    }
    p::UpgradeTail tail;
    if (!p::decode_upgrade_head(f, &tail, &result.error)) {
        return result;
    }
    ChannelEntry *entry = live_entry(tail.channel_id, &result.error);
    if (entry == nullptr) {
        return result;
    }
    if (entry->phase == ChannelPhase::Cancelling && entry->owner_kind == p::kOwnerHandshake &&
            entry->owner_request_id == f.request_id && from == ChannelSide::Worker &&
            entry->direction[0].terminal_sent) {
        entry->max_message_bytes = std::min(entry->max_message_bytes, tail.max_message_bytes);
        result.accepted = true; result.discard = true;
        return result;
    }
    if (entry->phase != ChannelPhase::Pending ||
            entry->owner_request_id != f.request_id) {
        result.error = "unknown channel";
        return result;
    }
    // the single handshake -> duplex ownership handover (§3.4)
    entry->owner_kind = p::kOwnerDuplexConnection;
    entry->max_message_bytes =
        std::min(entry->max_message_bytes, tail.max_message_bytes);
    entry->phase = ChannelPhase::Open;
    result.accepted = true;
    return result;
}

// ---- lifecycle --------------------------------------------------------

void ChannelRegistry::teardown_handshake(uint64_t request_id) {
    for (auto it=channels_.begin();it!=channels_.end();) {
        if (it->second.phase==ChannelPhase::Pending && it->second.owner_request_id==request_id) it=channels_.erase(it);
        else ++it;
    }
}

void ChannelRegistry::teardown_all(uint32_t terminal_reason) {
    (void)terminal_reason;
    std::map<uint32_t, ChannelEntry> live;
    live.swap(channels_);
    for (std::map<uint32_t, ChannelEntry>::iterator it = live.begin();
         it != live.end(); ++it) {
        if (!phase_terminal(it->second.phase)) {
            release_entry_reservations(it->second);
        }
    }
    pending_dials_.clear();
    terminal_deadlines_.clear();
    retained_owners_.clear();
}

void ChannelRegistry::teardown_channel(uint32_t channel_id,
                                       uint32_t terminal_reason) {
    (void)terminal_reason;
    std::map<uint32_t, ChannelEntry>::iterator it =
        channels_.find(channel_id);
    if (it == channels_.end()) {
        return;   // idempotent for unknown/terminal ids
    }
    if (!phase_terminal(it->second.phase)) {
        release_entry_reservations(it->second);
    }
    channels_.erase(it);
    terminal_deadlines_.erase(channel_id);
    for (auto dial = pending_dials_.begin(); dial != pending_dials_.end();) {
        if (dial->second.result == PendingDial::Accepted &&
                dial->second.entry.channel_id == channel_id) {
            dial = pending_dials_.erase(dial);
        } else {
            ++dial;
        }
    }
}

bool ChannelRegistry::advance_time(uint64_t elapsed_ms) {
    if (expired_) return false;
    uint64_t after = 0;
    clock_ms_ = checked_add_u64(clock_ms_, elapsed_ms, &after)
        ? after : std::numeric_limits<uint64_t>::max();
    for (const auto &dial : pending_dials_) {
        if (clock_ms_ >= dial.second.deadline) expired_ = true;
    }
    for (const auto &terminal : terminal_deadlines_) {
        if (clock_ms_ >= terminal.second) expired_ = true;
    }
    if (expired_) teardown_all(protocol::kCancelTimeout);
    return !expired_;
}

// ---- queries -----------------------------------------------------------

bool ChannelRegistry::find(uint32_t channel_id, ChannelEntry *out)
    const {
    std::map<uint32_t, ChannelEntry>::const_iterator it =
        channels_.find(channel_id);
    if (it == channels_.end() || phase_terminal(it->second.phase)) {
        return false;
    }
    if (out) {
        *out = it->second;
    }
    return true;
}

bool ChannelRegistry::register_authority(uint32_t id, const ChannelAuthority &authority) {
    auto found = channels_.find(id);
    if (found == channels_.end() || found->second.authority_registered ||
            phase_terminal(found->second.phase)) return false;
    found->second.authority = authority;
    found->second.authority_registered = true;
    return true;
}

size_t ChannelRegistry::live_count() const {
    size_t count = channels_.size();
    for (const auto &dial : pending_dials_) {
        if (dial.second.result == PendingDial::Unresolved && !dial.second.must_reject) ++count;
    }
    return count;
}

}  // namespace capsid
