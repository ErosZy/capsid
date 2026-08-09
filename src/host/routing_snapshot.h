// WP-05 §9.2: RoutingSnapshot / RoutingTable — the immutable App→pool map
// the Managed data plane routes against.
//
// A request atomic-loads ONE snapshot and pins the shared_ptr<GenerationPool>
// it found; a later publish never rewrites the map the request holds. The
// snapshot itself never mutates after construction — publish builds a fresh
// object and swaps the pointer.
//
// §9.6-6: a retired App keeps its route as a TOMBSTONE — the name is routed
// but no pool serves it. The router answers 404 for a tombstone, while an
// App that was never routed stays the router's 503. A live pool entry always
// beats a tombstone for the same name (a redeploy after a retire revives the
// route), regardless of the build input order.

#ifndef CAPSID_HOST_ROUTING_SNAPSHOT_H
#define CAPSID_HOST_ROUTING_SNAPSHOT_H

#include "host/generation_pool.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
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

    // Builds the snapshot from app→pool pairs. A null pool entry is a
    // retired tombstone: the name stays routed, no pool serves it. A live
    // pool entry for the same name wins over a tombstone. Duplicate live
    // routes are an error: a duplicated route is a coordinator bug, not
    // something the router can disambiguate at request time.
    static std::shared_ptr<const RoutingSnapshot> build(
        std::vector<std::pair<std::string, std::shared_ptr<GenerationPool>>>
            routes);

    // The live pool for the App, or nullptr when the App has no live route
    // in THIS snapshot (never routed, or retired).
    std::shared_ptr<GenerationPool> find(std::string_view application) const;

    // True when the App's route is a retired tombstone (§9.6-6): routed
    // name, no pool. Never true when a live pool serves the App.
    bool retired(std::string_view application) const;

    // Full live-route map view for the listener's start-time wiring (every
    // pool in the snapshot gets the event sink installed) and diagnostics.
    // Tombstones are deliberately absent: no pool exists to wire. The
    // snapshot is immutable, so the reference is stable for its lifetime.
    const std::map<std::string, std::shared_ptr<GenerationPool>>& routes()
        const {
        return routes_;
    }

    std::size_t size() const {
        return routes_.size() + tombstones_.size();
    }

private:
    std::map<std::string, std::shared_ptr<GenerationPool>> routes_;
    std::set<std::string> tombstones_;
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
