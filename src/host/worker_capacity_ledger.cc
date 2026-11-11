// WorkerCapacityLedger implementation — see worker_capacity_ledger.h.
// The ledger is the §9.4 single accounting authority: every count change
// happens under one mutex, so concurrent reserves observe each other and
// the absolute ceiling (workers_total + activation_surge_workers) is an
// invariant of the object, not a best-effort check.

#include "host/worker_capacity_ledger.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace capsid::host {

bool WorkerCapacityLedger::reserve_fresh(const std::string& application,
                                         std::uint64_t pool_size) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (steady_used_ > workers_total_) throw std::logic_error("invalid steady worker count");
    auto available = workers_total_ - steady_used_;
    if (surge_used_ > surge_workers_) {
        const auto excess = surge_used_ - surge_workers_;
        if (excess > available) return false;
        available -= excess;
    }
    if (pool_size > available) {
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
    if (steady_used_ < old_pool_size || steady_used_ - old_pool_size > workers_total_ ||
        new_pool_size > workers_total_ - (steady_used_ - old_pool_size)) {
        return false;
    }
    // The overlap peak: the new pool warms (surge), then the old pool
    // drains (surge) — the surge budget must fit the larger side.
    const std::uint64_t overlap_peak =
        std::max(new_pool_size, old_pool_size);
    if (surge_used_ > surge_workers_ || overlap_peak > surge_workers_ - surge_used_) {
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
    if (entry.surge < new_pool_size || surge_used_ < new_pool_size ||
        entry.steady != old_pool_size || steady_used_ < old_pool_size) {
        throw std::logic_error("replacement capacity category mismatch");
    }
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
        if (found->second.surge < pool_size || surge_used_ < pool_size)
            throw std::logic_error("surge reserve already released");
        found->second.surge -= pool_size;
        surge_used_ -= pool_size;
    } else {
        if (found->second.steady < pool_size || steady_used_ < pool_size)
            throw std::logic_error("steady reserve already released");
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
    if (entry.steady < pool_size || steady_used_ < pool_size)
        throw std::logic_error("retire capacity category mismatch");
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
    if (found->second.surge < drained || surge_used_ < drained)
        throw std::logic_error("drained capacity category mismatch");
    found->second.surge -= drained;
    surge_used_ -= drained;
    if (found->second.steady == 0 && found->second.surge == 0) {
        entries_.erase(found);
    }
}

void WorkerCapacityLedger::release_active(const std::string& application, std::uint64_t reaped) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = entries_.find(application);
    if (found == entries_.end() || found->second.steady < reaped || steady_used_ < reaped)
        throw std::logic_error("reaped active capacity category mismatch");
    found->second.steady -= reaped; steady_used_ -= reaped;
    if (found->second.steady == 0 && found->second.surge == 0) entries_.erase(found);
}

WorkerPoolCapacityLease::WorkerPoolCapacityLease(WorkerCapacityLedger* ledger,
    std::string application, std::uint64_t workers)
    : ledger_(ledger), application_(std::move(application)), workers_(workers) {}

void WorkerPoolCapacityLease::release_reaped_locked() {
    if (!reaped_) return;
    if (stage_ == Stage::kActive) ledger_->release_active(application_, workers_);
    else if (stage_ == Stage::kDraining) ledger_->release_drained(application_, workers_);
    else return;
    stage_ = Stage::kReleased;
}

void WorkerPoolCapacityLease::activate_fresh() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stage_ != Stage::kPrepared) throw std::logic_error("pool capacity already settled");
    ledger_->commit_fresh(application_); stage_ = Stage::kActive; release_reaped_locked();
}

void WorkerPoolCapacityLease::activate_replacement(const std::shared_ptr<WorkerPoolCapacityLease>& old) {
    if (!old || old.get() == this) throw std::logic_error("replacement pool capacity missing");
    std::scoped_lock lock(mutex_, old->mutex_);
    if (stage_ != Stage::kPrepared) throw std::logic_error("pool capacity already settled");
    if (old->ledger_ != ledger_ || old->application_ != application_ || old->stage_ == Stage::kPrepared)
        throw std::logic_error("replacement capacity belongs to another activation");
    const auto active_old = old->stage_ == Stage::kActive ? old->workers_ : 0;
    ledger_->commit_replace(application_, workers_, active_old);
    if (old->stage_ == Stage::kActive) old->stage_ = Stage::kDraining;
    stage_ = Stage::kActive; release_reaped_locked();
}

void WorkerPoolCapacityLease::abort(bool replacement) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stage_ == Stage::kReleased) return;
    if (stage_ != Stage::kPrepared) throw std::logic_error("cannot abort committed capacity");
    ledger_->abort_reserve(application_, workers_, replacement); stage_ = Stage::kReleased;
}

void WorkerPoolCapacityLease::begin_drain() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stage_ == Stage::kActive) {
        ledger_->begin_retire(application_, workers_); stage_ = Stage::kDraining;
    }
    release_reaped_locked();
}

void WorkerPoolCapacityLease::reaped() {
    std::lock_guard<std::mutex> lock(mutex_);
    reaped_ = true; release_reaped_locked();
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
