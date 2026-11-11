#ifndef CAPSID_HOST_PROTOCOL_LAUNCH_H
#define CAPSID_HOST_PROTOCOL_LAUNCH_H

#include "capsid/runtime.h"

#include "protocol.h"
#include "host/host_config_model.h"

#include <string>

namespace capsid {
namespace host {

// GLM T08: the first-party 0.3.0 hosts launch workers through
// capsid_worker_spawn_ex at wire version 4 (the carriage slice keeps
// v3 semantics). A host.json "channels" section additionally requests
// the channel capability set (DUPLEX | A2 | UPGRADE | HALF_CLOSE — a
// closure-satisfying set); a worker that does not answer the bits
// leaves the effective set without them and channel requests fail
// fast and explicitly. The wire version is never auto-downgraded.
struct ProtocolLaunch {
    bool use_v4 = true;    // first-party hosts: v4 carriage
    capsid_protocol_options options{};

    // The options descriptor must ALWAYS be initialized (two-way size
    // negotiation stamps the caller size); a raw aggregate default
    // would leave struct_size at zero and spawn_ex would refuse.
    ProtocolLaunch() {
        capsid_protocol_options_init(&options, sizeof(options));
        options.wire_version = capsid::protocol::kVersionV4;
        options.capabilities = CAPSID_PROTOCOL_CAP_DUPLEX_CHANNEL |
            CAPSID_PROTOCOL_CAP_UPGRADE | CAPSID_PROTOCOL_CAP_CHANNEL_LAYOUT_A2 |
            CAPSID_PROTOCOL_CAP_CHANNEL_CONFIRM | CAPSID_PROTOCOL_CAP_CHANNEL_CLOSE_FINAL |
            CAPSID_PROTOCOL_CAP_FETCH_UPGRADE | CAPSID_PROTOCOL_CAP_CONNECTION_LIFETIME;
    }

    static ProtocolLaunch from(const ChannelsConfig& channels) {
        ProtocolLaunch launch;
        launch.options.wire_version = 4;
        if (channels.configured) {
            launch.options.capabilities =
                CAPSID_PROTOCOL_CAP_CHANNEL_LAYOUT_A2 |
                CAPSID_PROTOCOL_CAP_DUPLEX_CHANNEL |
                CAPSID_PROTOCOL_CAP_UPGRADE |
                CAPSID_PROTOCOL_CAP_HALF_CLOSE |
                CAPSID_PROTOCOL_CAP_CHANNEL_CONFIRM | CAPSID_PROTOCOL_CAP_CHANNEL_CLOSE_FINAL |
                CAPSID_PROTOCOL_CAP_FETCH_UPGRADE | CAPSID_PROTOCOL_CAP_CONNECTION_LIFETIME;
            launch.options.max_channels = channels.max_channels;
            launch.options.max_message_bytes =
                channels.max_message_bytes;
            launch.options.reassembly_budget_bytes =
                channels.reassembly_budget_bytes;
            launch.options.send_budget_bytes =
                channels.send_budget_bytes;
            launch.options.close_timeout_ms =
                channels.close_timeout_ms;
            launch.options.dial_timeout_ms = channels.dial_timeout_ms;
            launch.options.idle_timeout_ms = channels.idle_timeout_ms;
            launch.options.per_channel_queue_bytes =
                channels.per_channel_queue_bytes;
        }
        return launch;
    }
};

// Spawns through the selected entry point. Existing embedding entries
// keep capsid_worker_spawn (v3) — this helper is host-internal.
inline capsid_result spawn_with_protocol(
    const capsid_worker_config* config,
    const ProtocolLaunch& launch,
    capsid_worker** out) {
    if (launch.use_v4) {
        return capsid_worker_spawn_ex(config, &launch.options, out);
    }
    return capsid_worker_spawn(config, out);
}

}  // namespace host
}  // namespace capsid

#endif
