#ifndef CAPSID_HOST_STATIC_POOL_H
#define CAPSID_HOST_STATIC_POOL_H

#include <cstdint>
#include <map>
#include <optional>
#include <vector>

namespace capsid::host {

// Opaque worker identifier in the Host pool namespace.
using PoolWorkerId = std::uint32_t;

// M2 fixed-pool state controller. Owns only the pool state machine: a fixed
// target count, per-worker registration that binds an immutable owner shard,
// READY transitions with unknown/duplicate rejection, and activation that is
// allowed only once every target worker is READY. Queueing, load selection,
// Runtime calls and the SSE permit are deliberately out of scope (later M2
// batches).
class StaticPoolState {
public:
    explicit StaticPoolState(std::uint32_t target_workers)
        : target_workers_(target_workers) {}

    std::uint32_t target_workers() const { return target_workers_; }

    // Registers a new starting worker under its owner shard. Rejects when
    // the fixed target is already reached or when the id is already
    // registered; a rejected registration never changes the owner binding.
    bool register_starting(PoolWorkerId id, std::uint32_t shard);

    // Transitions a registered worker to READY. Unknown ids and duplicate
    // READY transitions reject without changing the READY count.
    bool mark_ready(PoolWorkerId id);

    std::size_t registered_workers() const { return workers_.size(); }
    std::size_t ready_workers() const { return ready_count_; }

    // The immutable shard a worker was registered under; nullopt for
    // unknown ids.
    std::optional<std::uint32_t> owner_shard(PoolWorkerId id) const;

    // READY workers of exactly one shard, in stable id order. An empty (or
    // unknown) shard returns an empty set: a shard never borrows a worker
    // owned by another shard.
    std::vector<PoolWorkerId> ready_workers_for_shard(
        std::uint32_t shard) const;

    // Activation gate: a positive target, every target worker registered
    // and READY, and the pool not already active. A zero-target pool never
    // activates, and activation is a one-shot transition.
    bool can_activate() const {
        return target_workers_ > 0 && !active_ &&
               workers_.size() == target_workers_ &&
               ready_count_ == target_workers_;
    }

    bool active() const { return active_; }

    // Activates the pool; fails without changing state when the pool is
    // already active or not all target workers are READY yet.
    bool activate() {
        if (!can_activate()) {
            return false;
        }
        active_ = true;
        return true;
    }

private:
    struct Worker {
        std::uint32_t shard;
        bool ready = false;
    };

    std::uint32_t target_workers_;
    // Sorted map: stable iteration order for ready_workers_for_shard and a
    // canonical id namespace for unknown-id rejection.
    std::map<PoolWorkerId, Worker> workers_;
    std::size_t ready_count_ = 0;
    bool active_ = false;
};

}  // namespace capsid::host

#endif
