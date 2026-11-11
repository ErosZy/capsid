#ifndef CAPSID_CHANNEL_REGISTRY_H
#define CAPSID_CHANNEL_REGISTRY_H

#include "channel_state.h"
#include "protocol.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace capsid {

// Production channel state machine for FetchRPC v4 A2
// (docs/fetchrpc-v4.md). One instance validates the frame stream of
// one IPC connection from BOTH sides (the same rules run on the host
// and in the worker; each side feeds the frames it sends and
// receives), enforces the negotiated budgets, and exposes lifecycle
// operations (upgrade commit, teardown) that embedders call outside
// the wire stream.
//
// Guarantees (GLM plan T06):
//   * a rejected frame leaves NO partial state behind;
//   * reassembly bytes are reserved before any channel mutation;
//   * checked arithmetic covers every credit and budget operation;
//   * internal teardown is idempotent but never excuses a wire frame
//     that references a dead id (`unknown channel`);
//   * after a terminal phase the entry releases all reservations and
//     counters return to their pre-open values.
class ChannelRegistry {
public:
    struct Options {
        ChannelSide local_side = ChannelSide::Host;
        uint32_t max_channels = 128;
        uint64_t reassembly_budget_bytes = 16u * 1024u * 1024u;
        uint64_t send_budget_bytes = 16u * 1024u * 1024u;
        uint64_t per_channel_queue_bytes = 4u * 1024u * 1024u;
        uint64_t max_message_bytes = 1024u * 1024u;
        uint32_t dial_timeout_ms = 10000;
        uint32_t close_timeout_ms = 5000;
    };

    struct ApplyResult {
        // Debug STL move construction can allocate inside noexcept.
        // A throwing copy keeps return-value allocation containable.
        ApplyResult() = default;
        ApplyResult(const ApplyResult&) = default;
        ApplyResult& operator=(const ApplyResult&) = default;

        bool accepted = false;
        std::string error{""};         // exact contract string
        // Dial policy output for the host: OPEN was legal, but the
        // budgets refuse an ACCEPT; the host answers kChannelReject.
        bool dial_should_reject = false;
        uint32_t dial_reject_reason = 0;
        // Valid in-flight bytes must be validated and drained, but must
        // never revive a locally cancelled endpoint or reach JavaScript.
        bool discard = false;
        // Wire-valid DATA was quarantined by the owned IO budget. The
        // adapter must send a channel-scoped resource CANCEL, then drain
        // already queued peer bytes through the terminal barrier.
        bool resource_exhausted = false;
    };

    ChannelRegistry();                    // Options() defaults
    explicit ChannelRegistry(const Options &options);

    // ---- wire stream ------------------------------------------------
    // `from` is the side that emitted the frame. HELLO must be the
    // first host frame; READY the first worker frame; versions never
    // mix on one connection.
    ApplyResult apply(ChannelSide from, const protocol::Frame &frame);

    // ---- lifecycle (embedder-driven, off-wire) ----------------------
    // Marks every live channel terminal with the given reason and
    // releases all reservations. Safe to call repeatedly.
    void teardown_all(uint32_t terminal_reason);
    // Tears one channel down by id (disconnect, timeout, shutdown,
    // replacement). Unknown/terminal ids are a no-op for the internal
    // path; wire frames that reference them still fail as
    // `unknown channel`.
    void teardown_channel(uint32_t channel_id, uint32_t terminal_reason);
    void teardown_handshake(uint64_t request_id);
    // Advance the endpoint's monotonic clock. Missing confirmations or
    // an incomplete close expire the WHOLE IPC connection: reusing it
    // after dropping correlation could misclassify later queued frames.
    bool advance_time(uint64_t elapsed_ms);

    // ---- queries -----------------------------------------------------
    bool find(uint32_t channel_id, ChannelEntry *out) const;
    bool register_authority(uint32_t channel_id, const ChannelAuthority &authority);
    size_t live_count() const;
    size_t retained_owner_count() const { return retained_owners_.size(); }
    uint64_t reassembly_reserved() const { return reassembly_reserved_; }
    uint32_t effective_capabilities() const { return effective_caps_; }
    uint16_t wire_version() const { return wire_version_; }
    bool negotiation_complete() const { return ready_seen_; }
    // Counters for diagnostics and budget assertions.
    uint64_t messages_completed() const { return messages_completed_; }
    uint64_t bytes_reassembled() const { return bytes_reassembled_; }
    size_t pending_dial_count() const { return pending_dials_.size(); }
    bool pending_dial(uint64_t dial_id, ChannelEntry *entry = nullptr,
                      bool *cancelled = nullptr, bool *unresolved = nullptr) const;
    bool dial_must_reject(uint64_t dial_id, uint32_t *reason) const;

private:
    ApplyResult apply_hello(const protocol::Frame &frame);
    ApplyResult apply_ready(const protocol::Frame &frame);
    ApplyResult apply_open(ChannelSide from, const protocol::Frame &f);
    ApplyResult apply_accept(ChannelSide from, const protocol::Frame &f);
    ApplyResult apply_reject(ChannelSide from, const protocol::Frame &f);
    ApplyResult apply_data(ChannelSide from, const protocol::Frame &f);
    ApplyResult apply_credit(ChannelSide from, const protocol::Frame &f);
    ApplyResult apply_half_close(ChannelSide from,
                                 const protocol::Frame &f);
    ApplyResult apply_close(ChannelSide from, const protocol::Frame &f);
    ApplyResult apply_cancel(ChannelSide from, const protocol::Frame &f);
    ApplyResult apply_ack(ChannelSide from, const protocol::Frame &f);
    ApplyResult apply_connection_lifetime(ChannelSide from, const protocol::Frame &f);
    ApplyResult apply_request_head(ChannelSide from,
                                   const protocol::Frame &f);
    ApplyResult apply_response_head(ChannelSide from,
                                    const protocol::Frame &f);

    bool has_capability(uint32_t bit) const {
        return (effective_caps_ & bit) != 0;
    }
    ChannelEntry *live_entry(uint32_t channel_id, std::string *error);
    // Releases every reservation the entry holds; called exactly once
    // per terminal transition.
    void release_entry_reservations(const ChannelEntry &entry);
    void mark_terminal(ChannelEntry *entry, ChannelPhase phase);
    void release_direction(ChannelEntry *entry, int side);
    bool terminal_complete(const ChannelEntry &entry) const;

    Options options_;
    uint32_t announced_[2] = {0, 0};
    uint32_t effective_caps_ = 0;
    uint16_t wire_version_ = 0;
    bool hello_seen_ = false;
    bool ready_seen_ = false;
    std::map<uint32_t, ChannelEntry> channels_;
    // Separate from network entries: ACK releases wire capacity while an
    // incoming local outgoing direction can still pin its worker generation.
    std::set<uint32_t> retained_owners_;
    struct PendingDial {
        ChannelEntry entry;
        bool must_reject = false;
        enum Result { Unresolved, Accepted, Rejected } result = Unresolved;
        bool cancelled = false;
        uint64_t deadline = 0;
    };
    // Unassigned dials never share keys with host-assigned channel ids.
    std::map<uint64_t, PendingDial> pending_dials_;
    // Host ids are allocated in announcement order; a high-water mark
    // refuses reuse without retaining an unbounded tombstone per close.
    uint32_t last_channel_id_ = 0;
    uint64_t last_dial_id_ = 0;
    uint64_t reassembly_reserved_ = 0;
    uint64_t messages_completed_ = 0;
    uint64_t bytes_reassembled_ = 0;
    uint64_t clock_ms_ = 0;
    bool expired_ = false;
    std::map<uint32_t, uint64_t> terminal_deadlines_;
};

}  // namespace capsid

#endif
