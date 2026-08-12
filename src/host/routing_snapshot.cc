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

// The C++11 shared_ptr atomic free functions are a deliberate choice
// (PR-10): std::atomic<std::shared_ptr> is unavailable on the Apple libc++
// toolchain the Host supports. GCC 15 deprecates the free functions — this
// is the documented trade-off, so silence just this diagnostic class here
// instead of weakening the target-wide -Werror.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
void RoutingTable::publish(std::shared_ptr<const RoutingSnapshot> snapshot) {
    std::atomic_store_explicit(&snapshot_, std::move(snapshot),
                               std::memory_order_release);
    generation_.fetch_add(1, std::memory_order_relaxed);
}

std::shared_ptr<const RoutingSnapshot> RoutingTable::load() const {
    return std::atomic_load_explicit(&snapshot_, std::memory_order_acquire);
}
#pragma GCC diagnostic pop

}  // namespace capsid::host
