#ifndef CAPSID_HOST_WEBSOCKET_DIALER_H
#define CAPSID_HOST_WEBSOCKET_DIALER_H
#include "egress_policy.h"
#include "host/worker_executor.h"
#include <memory>

namespace capsid::host {
enum class DialPhase { kDns, kTcp, kTls, kHandshake, kProvisional, kOpen, kStopped };
struct WebSocketDialerOptions {
    std::shared_ptr<ServiceChannelBudget> budget;
    // Native diagnostics/test configuration, never derived from worker
    // payloads. Empty DNS servers preserves system DNS configuration.
    std::string dns_servers;
    std::function<void(uint64_t, DialPhase)> on_phase;
};
// One network owner per executor, used by every first-party host mode.
// Only the executor's thread accesses the C ABI. This owner receives
// copied events and submits bounded commands back to that thread.
class WebSocketDialer {
public:
    WebSocketDialer(EgressPolicy authority, protocol::ChannelLimits limits,
        std::function<bool(Command)> sender, std::function<void(size_t)> release,
        WebSocketDialerOptions options = {});
    ~WebSocketDialer();
    void post(WorkerEvent event);
    void request_stop();
    void stop_and_join();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
#endif
