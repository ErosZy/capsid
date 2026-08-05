// M2 fixed-pool state controller. See static_pool.h.

#include "host/static_pool.h"

namespace capsid::host {

bool StaticPoolState::register_starting(PoolWorkerId id, std::uint32_t shard) {
    if (workers_.size() >= target_workers_) {
        return false;  // the fixed target is already full
    }
    if (workers_.find(id) != workers_.end()) {
        return false;  // duplicate registration is not a transfer
    }
    workers_.emplace(id, Worker{ shard, false });
    return true;
}

bool StaticPoolState::mark_ready(PoolWorkerId id) {
    std::map<PoolWorkerId, Worker>::iterator worker = workers_.find(id);
    if (worker == workers_.end() || worker->second.ready) {
        return false;  // unknown id or duplicate READY transition
    }
    worker->second.ready = true;
    ++ready_count_;
    return true;
}

std::optional<std::uint32_t> StaticPoolState::owner_shard(
    PoolWorkerId id) const {
    const std::map<PoolWorkerId, Worker>::const_iterator worker =
        workers_.find(id);
    if (worker == workers_.end()) {
        return std::nullopt;
    }
    return worker->second.shard;
}

std::vector<PoolWorkerId> StaticPoolState::ready_workers_for_shard(
    std::uint32_t shard) const {
    std::vector<PoolWorkerId> ready;
    for (const std::pair<const PoolWorkerId, Worker>& worker : workers_) {
        if (worker.second.ready && worker.second.shard == shard) {
            ready.push_back(worker.first);
        }
    }
    return ready;
}

}  // namespace capsid::host
