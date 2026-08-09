// §9.4 weighted capacity ledger — the single accounting authority for the
// worker-count budget.
//
//   workers_total:            the steady-state budget. Every SERVING pool
//                             worker counts against it.
//   activation_surge_workers: the extra budget for the new-warming +
//                             old-draining OVERLAP of a replacement
//                             deploy. v1 default 0: without a surge
//                             budget a replacement deploy is refused
//                             (zero-downtime is physically impossible),
//                             never silently over-spawned.
//   absolute ceiling:         workers_total + activation_surge_workers.
//
// Every live worker process is in EXACTLY one category:
//   steady — serving (an active pool's workers),
//   surge  — overlap (a replacement pool warming, or a replaced/retired
//            pool draining).
//
// Deploy sequence (caller contract):
//   reserve_fresh / reserve_replace BEFORE any spawn — the reserve is the
//   gate and the count is held from that moment (concurrent reserves see
//   it), so a denied reserve means the operation must not proceed;
//   commit_fresh / commit_replace when the operation settles with a live
//   pool; abort_reserve when it settles without one (failed warm, failed
//   persist, refused activation).
// Retire sequence: begin_retire moves the serving pool into surge
// (draining) — the process count only shrinks, so no budget gate applies —
// and release_drained returns the count when the reaper finished.
//
// Replace accounting: the old pool keeps its steady count while it
// serves; the new pool warms under surge; the surge budget must fit the
// OVERLAP PEAK max(new, old) (the warm side, then the drain side).
// commit_replace moves the new pool to steady and the old pool to surge.
// Startup concurrency and worker count are independent limits (the
// startups-concurrent gate lives in the listener; this ledger is only the
// count).

#ifndef CAPSID_HOST_WORKER_CAPACITY_LEDGER_H
#define CAPSID_HOST_WORKER_CAPACITY_LEDGER_H

#include <cstdint>
#include <map>
#include <mutex>
#include <string>

namespace capsid::host {

class WorkerCapacityLedger {
public:
    WorkerCapacityLedger(std::uint64_t workers_total,
                         std::uint64_t activation_surge_workers)
        : workers_total_(workers_total),
          surge_workers_(activation_surge_workers) {}

    // Fresh deploy (no serving pool): reserves `pool_size` against the
    // steady-state budget. False = budget exhausted; nothing is held.
    bool reserve_fresh(const std::string& application,
                       std::uint64_t pool_size);

    // Replacement deploy over a SERVING pool (`pool_size` new workers):
    // the steady state after the swap must fit workers_total and the
    // overlap peak max(new, old) must fit the surge budget. False =
    // zero-downtime replace refused (no surge/headroom); nothing is held.
    bool reserve_replace(const std::string& application,
                         std::uint64_t new_pool_size);

    // Settles a successful reserve. commit_fresh is a no-op on the counts
    // (the fresh pool was already held as steady); commit_replace moves
    // the new pool from surge to steady and the old pool into surge
    // (draining).
    void commit_fresh(const std::string& application);
    void commit_replace(const std::string& application,
                        std::uint64_t new_pool_size,
                        std::uint64_t old_pool_size);

    // Rolls a reserve back when the operation settles without a live
    // pool (warm failure, persist failure, refused activation). Must
    // mirror the reserve that held the count.
    void abort_reserve(const std::string& application,
                       std::uint64_t pool_size, bool replacement);

    // Retire: the serving pool leaves steady for surge (draining). No
    // budget gate — the process count only shrinks, so the absolute
    // ceiling can never be exceeded by a retire.
    void begin_retire(const std::string& application,
                      std::uint64_t pool_size);

    // Reaper finished: `drained` surge workers are gone. The old pool
    // releases its count only here (spec §9.4: after the reaper
    // completed, not when the drain started).
    void release_drained(const std::string& application,
                         std::uint64_t drained);

    // True when the App has a serving pool (steady > 0).
    bool holds(const std::string& application) const;

    std::uint64_t steady_used() const;
    std::uint64_t surge_used() const;
    std::uint64_t steady_of(const std::string& application) const;
    std::uint64_t surge_of(const std::string& application) const;

private:
    struct Entry {
        std::uint64_t steady = 0;  // serving workers
        std::uint64_t surge = 0;   // overlap workers (warming/draining)
    };
    std::map<std::string, Entry> entries_;
    std::uint64_t steady_used_ = 0;
    std::uint64_t surge_used_ = 0;
    const std::uint64_t workers_total_;
    const std::uint64_t surge_workers_;
    mutable std::mutex mutex_;
};

}  // namespace capsid::host

#endif
