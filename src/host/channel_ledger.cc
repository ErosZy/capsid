#include "channel_ledger.h"

#include <atomic>
#include <limits>

namespace capsid {
namespace host {
std::string channel_budget_metrics_json(const ChannelBudgetSnapshot &snapshot) {
    const auto resource=[](const ChannelBudgetSnapshot::Resource &r) {
        return "{\"used\":"+std::to_string(r.used)+",\"limit\":"+std::to_string(r.limit)+
            ",\"highWater\":"+std::to_string(r.high_water)+",\"rejected\":"+std::to_string(r.rejected)+"}";
    };
    return "{\"active\":"+std::to_string(snapshot.active)+",\"httpInflight\":"+std::to_string(snapshot.http_inflight)+
        ",\"channels\":"+resource(snapshot.channels)+",\"dials\":"+resource(snapshot.dials)+",\"queueBytes\":"+resource(snapshot.queue)+"}";
}
ServiceChannelBudget::ServiceChannelBudget(ChannelBudgetLimits limits, std::shared_ptr<ServiceChannelBudget> parent)
    : parent_(std::move(parent)) {
    channels_.limit = limits.channels; dials_.limit = limits.pending_dials; queue_.limit = limits.queue_bytes;
}
void ServiceChannelBudget::reject(Counter &counter) {
    auto value = counter.rejected.load();
    while (value != UINT64_MAX && !counter.rejected.compare_exchange_weak(value, value+1)) {}
}
bool ServiceChannelBudget::reserve(Counter &counter, uint64_t amount) {
    auto current = counter.used.load();
    for (;;) {
        const auto ceiling = counter.limit.load();
        if (amount > UINT64_MAX-current || (ceiling && (amount>ceiling || current>ceiling-amount))) {
            reject(counter); return false;
        }
        if (counter.used.compare_exchange_weak(current, current+amount)) break;
    }
    auto peak = counter.high_water.load();
    while (peak < current+amount && !counter.high_water.compare_exchange_weak(peak,current+amount)) {}
    return true;
}
bool ServiceChannelBudget::release(Counter &counter, uint64_t amount) {
    auto current = counter.used.load();
    while (current >= amount) {
        if (counter.used.compare_exchange_weak(current,current-amount)) return true;
    }
    return false;
}
std::shared_ptr<void> ServiceChannelBudget::reserve_queue(uint64_t bytes) {
    auto owner = weak_from_this().lock();
    if (!owner) return nullptr;
    struct Lease {
        std::shared_ptr<ServiceChannelBudget> owner;
        std::shared_ptr<void> parent;
        uint64_t bytes = 0;
        ~Lease() { if (bytes) ServiceChannelBudget::release(owner->queue_,bytes); }
    };
    auto lease = std::make_shared<Lease>(); lease->owner = std::move(owner);
    if (!reserve(queue_,bytes)) return nullptr;
    if (parent_) {
        lease->parent = parent_->reserve_queue(bytes);
        if (!lease->parent) { release(queue_,bytes); reject(queue_); return nullptr; }
    }
    lease->bytes = bytes; return lease;
}
std::shared_ptr<ServiceChannelBudget> default_channel_budget() {
    static const auto budget = std::make_shared<ServiceChannelBudget>(); return budget;
}
void ServiceChannelBudget::set_channel_ceiling(uint64_t ceiling) { channels_.limit = ceiling; }
void ServiceChannelBudget::set_dial_ceiling(uint64_t ceiling) { dials_.limit = ceiling; }
bool ServiceChannelBudget::try_reserve_channel() {
    if (!reserve(channels_,1)) return false;
    if (parent_ && !parent_->try_reserve_channel()) { release(channels_,1); reject(channels_); return false; }
    return true;
}
void ServiceChannelBudget::release_channel() { if (release(channels_,1) && parent_) parent_->release_channel(); }
bool ServiceChannelBudget::try_reserve_dial() {
    if (!reserve(dials_,1)) return false;
    if (parent_ && !parent_->try_reserve_dial()) { release(dials_,1); reject(dials_); return false; }
    return true;
}
void ServiceChannelBudget::release_dial() { if (release(dials_,1) && parent_) parent_->release_dial(); }
uint64_t ServiceChannelBudget::channels() const { return channels_.used.load(); }
uint64_t ServiceChannelBudget::dials() const { return dials_.used.load(); }
void ServiceChannelBudget::activate_channel() { if (reserve(active_,1) && parent_) parent_->activate_channel(); }
void ServiceChannelBudget::deactivate_channel() { if (release(active_,1) && parent_) parent_->deactivate_channel(); }
void ServiceChannelBudget::begin_http() { if (reserve(http_,1) && parent_) parent_->begin_http(); }
void ServiceChannelBudget::end_http() { if (release(http_,1) && parent_) parent_->end_http(); }
ChannelBudgetSnapshot ServiceChannelBudget::snapshot() const {
    const auto sample = [](const Counter &counter) {
        return ChannelBudgetSnapshot::Resource{counter.used.load(),counter.limit.load(),counter.high_water.load(),counter.rejected.load()};
    };
    return {sample(channels_),sample(dials_),sample(queue_),active_.used.load(),http_.used.load()};
}

ChannelLedger::ChannelLedger(const Limits &limits,
                             ServiceChannelBudget *service)
    : limits_(limits), service_(service) {}

bool ChannelLedger::try_reserve_http() {
    if (http_inflight_ == std::numeric_limits<std::uint64_t>::max() ||
            (limits_.max_http_inflight != 0 &&
             http_inflight_ >= limits_.max_http_inflight)) {
        return false;
    }
    ++http_inflight_;
    if (service_) service_->begin_http();
    return true;
}

void ChannelLedger::release_http() {
    if (http_inflight_ != 0) {
        --http_inflight_;
        if (service_) service_->end_http();
    }
}

bool ChannelLedger::try_reserve_dial() {
    if (retired_) {
        return false;
    }
    if (pending_dials_ == std::numeric_limits<std::uint64_t>::max() ||
            (limits_.max_pending_dials != 0 &&
             pending_dials_ >= limits_.max_pending_dials)) {
        return false;
    }
    if (service_ != nullptr && !service_->try_reserve_dial()) {
        return false;
    }
    ++pending_dials_;
    return true;
}

void ChannelLedger::release_dial() {
    if (pending_dials_ != 0) {
        --pending_dials_;
        if (service_ != nullptr) {
            service_->release_dial();
        }
    }
}

bool ChannelLedger::try_commit_channel() {
    if (retired_) {
        return false;
    }
    if (active_channels_ == std::numeric_limits<std::uint64_t>::max() ||
            (limits_.max_channels != 0 &&
             active_channels_ >= limits_.max_channels)) {
        return false;
    }
    if (service_ != nullptr && !service_->try_reserve_channel()) {
        return false;
    }
    ++active_channels_;
    return true;
}

void ChannelLedger::release_channel() {
    if (active_channels_ != 0) {
        --active_channels_;
        if (service_ != nullptr) {
            service_->release_channel();
        }
    }
}

bool ChannelLedger::commit_upgrade_from_http() {
    // An upgrade commit originates from a pending HTTP request; with
    // no slot held there is nothing to commit from.
    if (http_inflight_ == 0) {
        return false;
    }
    // all-or-nothing: the channel slot is taken first (restoring it on
    // failure), only then does the HTTP slot return.
    if (!try_commit_channel()) {
        return false;
    }
    release_http();
    return true;
}

void ChannelLedger::retire() {
    retired_ = true;
}

}  // namespace host
}  // namespace capsid
