#ifndef CAPSID_CHANNEL_STATE_H
#define CAPSID_CHANNEL_STATE_H

#include "protocol.h"

#include <cstdint>
#include <string>

namespace capsid {

// FetchRPC v4 A2 (docs/fetchrpc-v4.md §§3.1, 3.4, 3.5): the per-channel
// state shared by the host and worker state machines. Both sides embed
// the same ChannelRegistry so their rules cannot drift.

enum class ChannelSide { Host = 0, Worker = 1 };

// Lifecycle phases per the contract §3.5.
enum class ChannelPhase {
    Pending,     // inbound upgrade reserved, awaiting the 101 commit
    Dialing,     // outbound dial requested, awaiting ACCEPT/REJECT
    AwaitingAck, // v4.2 ACCEPT is provisional until the worker ACK
    Open,        // committed, data flows in both directions
    Closing,     // one side sent Close, awaiting the peer's Close
    Cancelling,  // v4.2 terminal marker awaits the peer's ACK
    Closed,      // both Close directions observed (terminal)
    Rejected,    // host refused the dial (terminal)
    Cancelled    // Cancel / teardown terminated the channel (terminal)
};

inline bool phase_terminal(ChannelPhase phase) {
    return phase == ChannelPhase::Closed ||
           phase == ChannelPhase::Rejected ||
           phase == ChannelPhase::Cancelled;
}

// One direction of a channel (indexed by the SENDING side). The
// direction's credit is what that side may still send; message
// assembly tracks the message currently being received from it.
struct ChannelDirection {
    uint64_t credit = protocol::kInitialChannelCredit;
    bool half_closed = false;
    bool close_sent = false;
    bool terminal_sent = false;
    uint32_t terminal_kind = protocol::kAckCancel;
    uint32_t terminal_reason = 0;
    bool terminal_acked = false;
    bool terminal_ack_sent = false; // this direction has drained its producer
    // receive-side message assembly for this direction's stream
    bool message_open = false;
    bool message_text = false;
    uint64_t message_bytes = 0;
};

// Immutable permission origin, recorded at open and never widened by
// ownership transfers (implementation design §4): the worker-side
// User/Binding authority that created the channel. Lifetime ownership
// lives on the entry itself (owner_kind).
struct ChannelAuthority {
    bool binding = false;          // false = User authority
    uint64_t binding_id = 0;       // binding slot when binding = true
};

// One channel entry; the registry is the only writer.
struct ChannelEntry {
    uint32_t channel_id = 0;       // host-assigned; 0 only while dialing
    uint64_t dial_id = 0;          // worker-assigned; 0 for inbound
    uint8_t owner_kind = protocol::kOwnerDuplexConnection;
    uint8_t channel_kind = protocol::kChannelKindWebSocket;
    // Retained after ACCEPT chooses WebSocket or a one-way HTTP body.
    // A fetch channel keeps its original fetch permission authority.
    bool fetch_upgrade = false;
    uint16_t http_status = 0;       // 0 for constructor/inbound WebSockets
    uint64_t owner_request_id = 0;
    uint64_t max_message_bytes = 0;  // effective negotiated cap
    ChannelPhase phase = ChannelPhase::Pending;
    ChannelDirection direction[2];   // indexed by ChannelSide
    ChannelAuthority authority;
    bool authority_registered = false;
};

}  // namespace capsid

#endif
