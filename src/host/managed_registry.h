// Bounded in-process operation registry and per-App operation locks
// (spec §13.4). The registry records deploy/retire/recovery outcomes by
// operation id; the App locks serialize state transitions per application.
// Both tables are process-global but bounded: the registry drops records
// by TTL and hard cap, and the App-lock table reclaims slots that no
// operation is currently holding.
//
// These are internal to the first-party Host (not part of the public ABI).

#ifndef CAPSID_HOST_MANAGED_REGISTRY_H
#define CAPSID_HOST_MANAGED_REGISTRY_H

#include <chrono>
#include <cstddef>
#include <mutex>
#include <string>

namespace capsid::host {

struct OperationStatus;

// Records the settled outcome of a deploy/retire/recovery operation.
// Bounded: entries expire by TTL; a burst within one TTL window is capped
// (oldest records evicted first).
void record_operation(const std::string& operation_id,
                      const OperationStatus& status);

// Reads a recorded operation; false if unknown or already evicted.
bool lookup_operation(const std::string& operation_id,
                      OperationStatus* out);

// RAII lock that pins one per-App mutex slot for the duration of a
// deploy/retire/recovery state transition. Pinning keeps the slot alive
// while it is held; slots with no users are reclaimed when the table
// exceeds its cap, so the table stays bounded under an unbounded set of
// application names. Different Apps proceed in parallel; the same App is
// serialized exactly as before (one mutex per App).
class AppOperationLock {
  public:
    explicit AppOperationLock(const std::string& application);
    ~AppOperationLock();
    AppOperationLock(const AppOperationLock&) = delete;
    AppOperationLock& operator=(const AppOperationLock&) = delete;

  private:
    void* slot_;  // AppOperationSlot*; opaque to keep the header light
    std::unique_lock<std::mutex> lock_;
};

// Test/diagnostic hooks: current table sizes (bounded by the caps).
std::size_t operation_record_count();
std::size_t app_operation_slot_count();

}  // namespace capsid::host

#endif  // CAPSID_HOST_MANAGED_REGISTRY_H
