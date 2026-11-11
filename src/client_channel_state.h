#ifndef CAPSID_CLIENT_CHANNEL_STATE_H
#define CAPSID_CLIENT_CHANNEL_STATE_H
#include "capsid/runtime.h"
#include "channel_state.h"
#include "egress_policy.h"
namespace capsid {
// Host-internal read-only snapshot, on the Runtime handle's owner thread.
// Never reads borrowed event bytes or changes the published ABI layout.
bool client_channel_snapshot(const capsid_worker *worker, uint32_t id, ChannelEntry *out);
bool client_channel_limits(const capsid_worker *worker, protocol::ChannelLimits *out);
bool client_pending_dial(const capsid_worker *worker, uint64_t id, ChannelEntry *entry,
                         bool *cancelled = nullptr, bool *unresolved = nullptr);
bool client_egress_snapshot(const capsid_worker *worker, EgressPolicy *out);
bool client_dial_must_reject(const capsid_worker *worker, uint64_t id, uint32_t *reason);
}
#endif
