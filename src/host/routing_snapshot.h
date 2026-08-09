// WP-05 §9.2: RoutingSnapshot / RoutingTable — the immutable App→pool map
// the Managed data plane routes against.
//
// A request atomic-loads ONE snapshot and pins the shared_ptr<GenerationPool>
// it found; a later publish never rewrites the map the request holds. The
// snapshot itself never mutates after construction — publish builds a fresh
// object and swaps the pointer.

#ifndef CAPSID_HOST_ROUTING_SNAPSHOT_H
#define CAPSID_HOST_ROUTING_SNAPSHOT_H

#include "host/generation_pool.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace capsid::host {

// Immutable App → GenerationPool map. Empty = admin-only mode: the router
// synthesizes 503 for every request (no configured listener routes).
class RoutingSnapshot {
public:
    RoutingSnapshot() = default;

    // Builds the snapshot from app→pool pairs. Duplicate application ids
    // are an error: a duplicated route is a coordinator bug, not something
    // the router can disambiguate at request time.
    static std::shared_ptr<const RoutingSnapshot> build(
        std::vector<std::pair<std::string, std::shared_ptr<GenerationPool>>>
            routes);

    // nullptr when the App has no route in THIS snapshot.
    std::shared_ptr<GenerationPool> find(std::string_view application) const;

    std::size_t size() const { return routes_.size(); }

private:
    std::map<std::string, std::shared_ptr<GenerationPool>> routes_;
};

// The published-route cell. publish()/load() are lock-free atomic
// shared_ptr operations (C++20); generation() counts publications for
// diagnostics.
class RoutingTable {
public:
    void publish(std::shared_ptr<const RoutingSnapshot> snapshot);
    std::shared_ptr<const RoutingSnapshot> load() const;
    std::uint64_t generation() const { return generation_.load(std::memory_order_relaxed); }

private:
    std::atomic<std::shared_ptr<const RoutingSnapshot>> snapshot_{nullptr};
    std::atomic<std::uint64_t> generation_{0};
};

}  // namespace capsid::host

#endif
