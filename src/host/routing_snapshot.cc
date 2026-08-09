// RoutingSnapshot / RoutingTable implementation — see routing_snapshot.h.

#include "host/routing_snapshot.h"

namespace capsid::host {

std::shared_ptr<const RoutingSnapshot> RoutingSnapshot::build(
    std::vector<std::pair<std::string, std::shared_ptr<GenerationPool>>>
        routes) {
    std::shared_ptr<RoutingSnapshot> snapshot(new RoutingSnapshot());
    // Tombstones first: a later live entry for the same name overrides the
    // tombstone, so a redeploy after a retire revives the route regardless
    // of the publisher's entry order (§9.6-6).
    for (auto& route : routes) {
        if (route.second == nullptr && !route.first.empty()) {
            snapshot->tombstones_.insert(route.first);
        }
    }
    for (auto& route : routes) {
        if (route.second == nullptr) {
            continue;  // already collected as a tombstone
        }
        // Duplicates would make find() ambiguous; the coordinator must
        // never publish them, so the last writer is NOT silently honored.
        if (snapshot->routes_.find(route.first) != snapshot->routes_.end()) {
            continue;
        }
        snapshot->routes_.emplace(std::move(route.first),
                                  std::move(route.second));
        // A live pool beats a tombstone for the same name, whichever order
        // the publisher used.
        snapshot->tombstones_.erase(route.first);
    }
    return snapshot;
}

std::shared_ptr<GenerationPool> RoutingSnapshot::find(
    std::string_view application) const {
    const auto it = routes_.find(std::string(application));
    if (it == routes_.end()) {
        return nullptr;
    }
    return it->second;
}

bool RoutingSnapshot::retired(std::string_view application) const {
    return tombstones_.find(std::string(application)) != tombstones_.end();
}

void RoutingTable::publish(std::shared_ptr<const RoutingSnapshot> snapshot) {
    snapshot_.store(std::move(snapshot), std::memory_order_release);
    generation_.fetch_add(1, std::memory_order_relaxed);
}

std::shared_ptr<const RoutingSnapshot> RoutingTable::load() const {
    return snapshot_.load(std::memory_order_acquire);
}

}  // namespace capsid::host
