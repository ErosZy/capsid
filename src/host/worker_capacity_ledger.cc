// WorkerCapacityLedger implementation — see worker_capacity_ledger.h.
// The ledger is the §9.4 single accounting authority: every count change
// happens under one mutex, so concurrent reserves observe each other and
// the absolute ceiling (workers_total + activation_surge_workers) is an
// invariant of the object, not a best-effort check.

#include "host/worker_capacity_ledger.h"

#include <algorithm>

namespace capsid::host {

bool WorkerCapacityLedger::reserve_fresh(const std::string& application,
                                         std::uint64_t pool_size) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (steady_used_ + pool_size > workers_total_) {
        return false;
    }
    entries_[application].steady += pool_size;
    steady_used_ += pool_size;
    return true;
}

bool WorkerCapacityLedger::reserve_replace(
    const std::string& application, std::uint64_t new_pool_size) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = entries_.find(application);
    if (found == entries_.end() || found->second.steady == 0) {
        // Not a replacement: the App has no serving pool. The caller
        // should have used reserve_fresh.
        return false;
    }
    const std::uint64_t old_pool_size = found->second.steady;
    // Steady state after the swap: the old pool stops serving.
    if (steady_used_ - old_pool_size + new_pool_size > workers_total_) {
        return false;
    }
    // The overlap peak: the new pool warms (surge), then the old pool
    // drains (surge) — the surge budget must fit the larger side.
    const std::uint64_t overlap_peak =
        std::max(new_pool_size, old_pool_size);
    if (surge_used_ + overlap_peak > surge_workers_) {
        return false;
    }
    // The new pool warms under surge from the reserve moment; concurrent
    // reserves see the held count.
    found->second.surge += new_pool_size;
    surge_used_ += new_pool_size;
    return true;
}

void WorkerCapacityLedger::commit_fresh(const std::string& application) {
    // The fresh pool was already held as steady by its reserve; the
    // settle only confirms it. (Kept as an explicit call so the caller's
    // accounting mirrors the transaction shape: reserve → settle.)
    (void)application;
}

void WorkerCapacityLedger::commit_replace(
    const std::string& application, std::uint64_t new_pool_size,
    std::uint64_t old_pool_size) {
    std::lock_guard<std::mutex> lock(mutex_);
    Entry& entry = entries_[application];
    // The new pool leaves surge for steady; the old pool enters surge
    // (draining). The held counts never change sum — only the category.
    entry.surge -= new_pool_size;
    surge_used_ -= new_pool_size;
    entry.steady = new_pool_size;
    steady_used_ -= old_pool_size;
    steady_used_ += new_pool_size;
    entry.surge += old_pool_size;
    surge_used_ += old_pool_size;
}

void WorkerCapacityLedger::abort_reserve(const std::string& application,
                                         std::uint64_t pool_size,
                                         bool replacement) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = entries_.find(application);
    if (found == entries_.end()) {
        return;
    }
    if (replacement) {
        found->second.surge -= pool_size;
        surge_used_ -= pool_size;
    } else {
        found->second.steady -= pool_size;
        steady_used_ -= pool_size;
    }
    if (found->second.steady == 0 && found->second.surge == 0) {
        entries_.erase(found);
    }
}

void WorkerCapacityLedger::begin_retire(const std::string& application,
                                        std::uint64_t pool_size) {
    std::lock_guard<std::mutex> lock(mutex_);
    Entry& entry = entries_[application];
    entry.steady -= pool_size;
    steady_used_ -= pool_size;
    entry.surge += pool_size;
    surge_used_ += pool_size;
}

void WorkerCapacityLedger::release_drained(const std::string& application,
                                           std::uint64_t drained) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = entries_.find(application);
    if (found == entries_.end()) {
        return;
    }
    found->second.surge -= drained;
    surge_used_ -= drained;
    if (found->second.steady == 0 && found->second.surge == 0) {
        entries_.erase(found);
    }
}

bool WorkerCapacityLedger::holds(const std::string& application) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = entries_.find(application);
    return found != entries_.end() && found->second.steady > 0;
}

std::uint64_t WorkerCapacityLedger::steady_used() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return steady_used_;
}

std::uint64_t WorkerCapacityLedger::surge_used() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return surge_used_;
}

std::uint64_t WorkerCapacityLedger::steady_of(
    const std::string& application) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = entries_.find(application);
    return found == entries_.end() ? 0 : found->second.steady;
}

std::uint64_t WorkerCapacityLedger::surge_of(
    const std::string& application) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = entries_.find(application);
    return found == entries_.end() ? 0 : found->second.surge;
}

}  // namespace capsid::host
