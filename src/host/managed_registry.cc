// Bounded operation registry and per-App operation locks (spec §13.4).
// See managed_registry.h for the contract.

#include "host/managed_registry.h"

#include <map>
#include <memory>

#include "host/managed_host.h"

namespace capsid::host {

namespace {

// Operation records: one minute shorter than a practical admin polling
// window, so a status page never lies about an evicted operation for long.
constexpr std::chrono::minutes kOperationRecordTtl(60);
// Hard cap: a deploy/retire/recover burst within one TTL window must not
// grow the registry without bound.
constexpr std::size_t kMaxOperationRecords = 1024;

struct OperationRecord {
    OperationStatus status;
    std::chrono::steady_clock::time_point recorded_at;
};

std::mutex& operation_registry_mutex() {
    static std::mutex mutex;
    return mutex;
}

std::map<std::string, OperationRecord>& operation_registry() {
    static std::map<std::string, OperationRecord> registry;
    return registry;
}

// Caller holds operation_registry_mutex().
void prune_operation_registry_locked() {
    const std::chrono::steady_clock::time_point now =
        std::chrono::steady_clock::now();
    std::map<std::string, OperationRecord>& registry = operation_registry();
    for (std::map<std::string, OperationRecord>::iterator it =
             registry.begin();
         it != registry.end();) {
        if (now - it->second.recorded_at >= kOperationRecordTtl) {
            it = registry.erase(it);
        } else {
            ++it;
        }
    }
    // TTL alone bounds steady state; a burst of records within one TTL
    // window still needs a hard cap. Evict the oldest recorded entries.
    while (registry.size() > kMaxOperationRecords) {
        std::map<std::string, OperationRecord>::iterator oldest =
            registry.begin();
        for (std::map<std::string, OperationRecord>::iterator it =
                 registry.begin();
             it != registry.end(); ++it) {
            if (it->second.recorded_at < oldest->second.recorded_at) {
                oldest = it;
            }
        }
        registry.erase(oldest);
    }
}

// Per-App operation lock slots (§13.4). A slot exists while the table
// needs it: AppOperationLock pins it (users > 0) so a concurrent
// eviction can never remove a slot that is being held; slots with no
// users are reclaimed when the table exceeds its cap.
constexpr std::size_t kMaxAppOperationSlots = 256;

struct AppOperationSlot {
    std::mutex mutex;
    std::size_t users = 0;
    std::chrono::steady_clock::time_point last_used;
};

std::mutex& app_operation_table_mutex() {
    static std::mutex mutex;
    return mutex;
}

std::map<std::string, std::unique_ptr<AppOperationSlot>>&
app_operation_table() {
    static std::map<std::string, std::unique_ptr<AppOperationSlot>> table;
    return table;
}

// Caller holds app_operation_table_mutex(). Reclaims every slot no
// operation is currently holding until the table is within the cap.
// Slots with users > 0 are never touched: an in-flight state transition
// keeps its mutex valid for its whole critical section.
void prune_app_operation_table_locked() {
    std::map<std::string, std::unique_ptr<AppOperationSlot>>& table =
        app_operation_table();
    while (table.size() > kMaxAppOperationSlots) {
        std::map<std::string, std::unique_ptr<AppOperationSlot>>::iterator
            evict = table.end();
        for (std::map<std::string, std::unique_ptr<AppOperationSlot>>::
                 iterator it = table.begin();
             it != table.end(); ++it) {
            if (it->second->users > 0) {
                continue;
            }
            if (evict == table.end() ||
                it->second->last_used < evict->second->last_used) {
                evict = it;
            }
        }
        if (evict == table.end()) {
            // Every slot is pinned; there is nothing safe to reclaim.
            // Bounded by the concurrent-operation maximum in practice.
            return;
        }
        table.erase(evict);
    }
}

}  // namespace

void record_operation(const std::string& operation_id,
                      const OperationStatus& status) {
    std::lock_guard<std::mutex> lock(operation_registry_mutex());
    OperationRecord record;
    record.status = status;
    record.recorded_at = std::chrono::steady_clock::now();
    operation_registry()[operation_id] = std::move(record);
    prune_operation_registry_locked();
}

bool lookup_operation(const std::string& operation_id,
                      OperationStatus* out) {
    std::lock_guard<std::mutex> lock(operation_registry_mutex());
    prune_operation_registry_locked();
    const std::map<std::string, OperationRecord>::const_iterator found =
        operation_registry().find(operation_id);
    if (found == operation_registry().end()) {
        return false;
    }
    if (out) {
        *out = found->second.status;
    }
    return true;
}

AppOperationLock::AppOperationLock(const std::string& application)
    : slot_(nullptr), lock_() {
    {
        std::lock_guard<std::mutex> table_lock(app_operation_table_mutex());
        std::unique_ptr<AppOperationSlot>& slot =
            app_operation_table()[application];
        if (!slot) {
            slot = std::make_unique<AppOperationSlot>();
        }
        slot->users += 1;
        slot->last_used = std::chrono::steady_clock::now();
        slot_ = slot.get();
    }
    // Lock outside the table mutex: waiting on another App's transition
    // must not block table bookkeeping.
    lock_ = std::unique_lock<std::mutex>(
        static_cast<AppOperationSlot*>(slot_)->mutex);
}

AppOperationLock::~AppOperationLock() {
    lock_.unlock();
    std::lock_guard<std::mutex> table_lock(app_operation_table_mutex());
    AppOperationSlot* slot = static_cast<AppOperationSlot*>(slot_);
    slot->users -= 1;
    prune_app_operation_table_locked();
}

std::size_t operation_record_count() {
    std::lock_guard<std::mutex> lock(operation_registry_mutex());
    return operation_registry().size();
}

std::size_t app_operation_slot_count() {
    std::lock_guard<std::mutex> lock(app_operation_table_mutex());
    return app_operation_table().size();
}

}  // namespace capsid::host
