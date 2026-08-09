// RoutingSnapshot / RoutingTable implementation — see routing_snapshot.h.

#include "host/routing_snapshot.h"

namespace capsid::host {

std::shared_ptr<const RoutingSnapshot> RoutingSnapshot::build(
    std::vector<std::pair<std::string, std::shared_ptr<GenerationPool>>>
        routes) {
    std::shared_ptr<RoutingSnapshot> snapshot(new RoutingSnapshot());
    for (auto& route : routes) {
        if (route.second == nullptr) {
            continue;  // a null pool entry is an empty route, never a crash
        }
        // Duplicates would make find() ambiguous; the coordinator must
        // never publish them, so the last writer is NOT silently honored.
        if (snapshot->routes_.find(route.first) != snapshot->routes_.end()) {
            continue;
        }
        snapshot->routes_.emplace(std::move(route.first),
                                  std::move(route.second));
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

void RoutingTable::publish(std::shared_ptr<const RoutingSnapshot> snapshot) {
    snapshot_.store(std::move(snapshot), std::memory_order_release);
    generation_.fetch_add(1, std::memory_order_relaxed);
}

std::shared_ptr<const RoutingSnapshot> RoutingTable::load() const {
    return snapshot_.load(std::memory_order_acquire);
}

}  // namespace capsid::host
