#ifndef CAPSID_HOST_CHANNEL_LEDGER_H
#define CAPSID_HOST_CHANNEL_LEDGER_H

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace capsid {
namespace host {
struct ChannelBudgetLimits {
    std::uint64_t channels = 1024;
    std::uint64_t pending_dials = 128;
    std::uint64_t queue_bytes = 16u * 1024u * 1024u;
};
struct ChannelBudgetSnapshot {
    struct Resource { std::uint64_t used = 0, limit = 0, high_water = 0, rejected = 0; };
    Resource channels, dials, queue;
    std::uint64_t active = 0, http_inflight = 0;
};
std::string channel_budget_metrics_json(const ChannelBudgetSnapshot &snapshot);

// Service-wide channel budget shared by every listener/executor
// (implementation design §5: the host caps ALL connections at the
// listener/service level, never just per channel). Counters are
// atomics: listener loops may run on separate threads.
class ServiceChannelBudget : public std::enable_shared_from_this<ServiceChannelBudget> {
public:
    explicit ServiceChannelBudget(ChannelBudgetLimits limits = {},
        std::shared_ptr<ServiceChannelBudget> parent = nullptr);
    // 0 disables the ceiling (unbounded), matching ListenerLimits
    // conventions; a nonzero ceiling makes reservations fail closed.
    void set_channel_ceiling(std::uint64_t ceiling);
    void set_dial_ceiling(std::uint64_t ceiling);

    bool try_reserve_channel();
    void release_channel();
    bool try_reserve_dial();
    void release_dial();

    std::uint64_t channels() const;
    std::uint64_t dials() const;
    // Adapter-owned network buffers, including zero-byte frame metadata.
    // A lease follows an in-flight write after its session queue is cleared.
    void set_queue_ceiling(std::uint64_t ceiling) { queue_.limit.store(ceiling); }
    std::shared_ptr<void> reserve_queue(std::uint64_t bytes);
    std::uint64_t queued_bytes() const { return queue_.used.load(); }
    ChannelBudgetSnapshot snapshot() const;
    const ServiceChannelBudget *parent() const { return parent_.get(); }
    void activate_channel();
    void deactivate_channel();
    void begin_http();
    void end_http();

private:
    struct Counter {
        std::atomic<std::uint64_t> used{0}, limit{0}, high_water{0}, rejected{0};
    };
    static bool reserve(Counter &counter, std::uint64_t amount);
    static bool release(Counter &counter, std::uint64_t amount);
    static void reject(Counter &counter);
    std::shared_ptr<ServiceChannelBudget> parent_;
    Counter channels_, dials_, queue_, active_, http_;
};

// Per-listener capacity ledger (GLM plan T08): active channels,
// pending dials, and HTTP inflight are counted SEPARATELY. A
// committed upgrade releases its HTTP slot but keeps occupying a
// channel slot, connection budget, and pool reference; health checks,
// admission, retire, and recovery observe WebSocket liveness through
// these counters instead of pretending an open channel is an
// unfinished HTTP response.
//
// Single-threaded by design (one ledger per listener loop); the
// service-level budget is the shared, thread-safe parent.
class ChannelLedger {
public:
    struct Limits {
        std::uint64_t max_http_inflight = 0;    // 0 = unbounded
        std::uint64_t max_channels = 0;
        std::uint64_t max_pending_dials = 0;
    };

    ChannelLedger() = default;
    explicit ChannelLedger(const Limits &limits,
                           ServiceChannelBudget *service = nullptr);

    // Reservation style: try_* only mutates on success, so a failed
    // reservation can never leak capacity (plan Red: 失败预留恢复容量).
    bool try_reserve_http();
    void release_http();
    bool try_reserve_dial();
    void release_dial();
    // Open/committed channel (dial accepted or upgrade committed by
    // the embedder calling the C ABI).
    bool try_commit_channel();
    void release_channel();
    // Upgrade commit point: the HTTP slot goes back, the channel slot
    // is taken. Fails (changing nothing) when the channel budget is
    // exhausted; on success the connection keeps occupying pool and
    // service-budget references through the channel counter.
    bool commit_upgrade_from_http();

    // Retire: the listener stops accepting NEW channels and dials;
    // existing reservations stay observable until they drain.
    void retire();
    bool retired() const { return retired_; }

    std::uint64_t http_inflight() const { return http_inflight_; }
    std::uint64_t pending_dials() const { return pending_dials_; }
    std::uint64_t active_channels() const { return active_channels_; }
    void activate_channel() { if (service_) service_->activate_channel(); }
    void deactivate_channel() { if (service_) service_->deactivate_channel(); }

private:
    Limits limits_;
    ServiceChannelBudget *service_ = nullptr;
    std::uint64_t http_inflight_ = 0;
    std::uint64_t pending_dials_ = 0;
    std::uint64_t active_channels_ = 0;
    bool retired_ = false;
};

// Default service cap is finite and shared by local shards and managed
// listeners in this process. Deployment overrides can pass their own parent.
std::shared_ptr<ServiceChannelBudget> default_channel_budget();
class ChannelLease {
public:
    ChannelLease(ChannelLedger &ledger, bool upgrade) : ledger_(ledger) {
        http_ = ledger_.try_reserve_http();
        if (http_ && upgrade) {
            channel_ = ledger_.try_commit_channel();
            if (!channel_) { ledger_.release_http(); http_ = false; }
        }
    }
    ~ChannelLease() { if (http_) ledger_.release_http(); if (active_) ledger_.deactivate_channel(); if (channel_) ledger_.release_channel(); }
    ChannelLease(const ChannelLease &) = delete;
    ChannelLease &operator=(const ChannelLease &) = delete;
    bool valid() const { return http_; }
    void commit_upgrade() {
        if (http_) { ledger_.release_http(); http_ = false; }
        if (channel_ && !active_) { ledger_.activate_channel(); active_ = true; }
    }
    void reject_upgrade() {
        if (active_) { ledger_.deactivate_channel(); active_=false; }
        if (channel_) { ledger_.release_channel(); channel_=false; }
    }
private:
    ChannelLedger &ledger_;
    bool http_ = false, channel_ = false, active_ = false;
};

}  // namespace host
}  // namespace capsid

#endif
