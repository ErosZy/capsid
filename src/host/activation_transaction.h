// §9.3 activation transaction types.
//
// A deploy is a three-phase transaction around the durable active-state
// write (the coordinator's persist), and the type invariants make the
// impossible states impossible:
//
//   prepare  — runs BEFORE the persist, may fail. The plan it returns
//              owns the new pool, the complete new routing snapshot and
//              a reference to the replaced pool. A failed prepare must
//              already have destroyed every warmed worker it was handed
//              (create_adopted's failure contract).
//   persist  — the coordinator writes active.json. May fail (disk):
//              the plan is aborted and the old active.json and the old
//              route stay untouched.
//   commit   — runs AFTER the persist, only when it succeeded. Must
//              never fail, allocate, do file I/O, wait on a lock, or
//              call back into the management layer: it is the atomic
//              publication (routing snapshot swap) plus the drain signal
//              plus the ledger category switch. A process crash between
//              persist and commit is allowed — restart recovers from
//              active.json (the durable document, not the memory).
//
// Because commit cannot fail, the operation can never report Failed
// while the disk already points at the new generation — the state the
// pre-PR-10 activate_* callbacks could reach (persist succeeded, then
// the data plane rejected the handoff).

#ifndef CAPSID_HOST_ACTIVATION_TRANSACTION_H
#define CAPSID_HOST_ACTIVATION_TRANSACTION_H

#include "host/generation_pool.h"
#include "host/routing_snapshot.h"

#include <cstdint>
#include <memory>
#include <string>

namespace capsid::host {

// Owned by the prepare callback; consumed by commit or abort. commit may
// move the plan's members out; abort must release the new pool.
struct ActivationPlan {
    std::string application;
    std::string version;
    std::string generation_digest;
    // The new generation, adopted over the warmed fleet (owning).
    std::shared_ptr<GenerationPool> new_pool;
    // The COMPLETE new route map (live routes + tombstones), built at
    // prepare time so commit is a single atomic snapshot swap.
    std::shared_ptr<const RoutingSnapshot> new_snapshot;
    // The replaced generation (null for a fresh deploy): commit signals
    // its drain; the reaper-finished hook releases its ledger count.
    std::shared_ptr<GenerationPool> old_pool;
    std::uint64_t new_workers = 0;
    std::uint64_t old_workers = 0;
};

// Owned by the prepare_retire callback; consumed by commit_retire or
// abort_retire. The pool reference is the serving generation being
// retired — commit drains it and the ledger moves its count to surge.
struct RetirePlan {
    std::string application;
    std::shared_ptr<GenerationPool> pool;  // may be null (already retired)
    std::uint64_t workers = 0;
    // The tombstone snapshot (the retired route removed), built at
    // prepare time so commit_retire is a single atomic snapshot swap.
    std::shared_ptr<const RoutingSnapshot> new_snapshot;
    // True when prepare mutated the route view (pool removed / tombstone
    // inserted); abort_retire only rolls back a mutation it made, so an
    // idempotent retire of an already-tombstoned App leaves the view
    // untouched on failure.
    bool view_mutated = false;
};

}  // namespace capsid::host

#endif
