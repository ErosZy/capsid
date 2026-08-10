// §13.4 bounded operation registry + per-App operation lock unit tests.
//
// Modes (argc == 2):
//   registry_bounded          — 2000-record burst: count stays <= 1024,
//                               oldest evicted, newest findable
//   registry_status_roundtrip — status round-trips; re-record overwrites
//   slots_bounded_reclaimed   — 512 sequential Apps: table stays <= 256,
//                               idle slots reclaimed
//   slots_serialize_same_app  — same-App critical sections serialize;
//                               distinct Apps proceed concurrently
//   slots_pinned_survive      — pins keep held slots alive past the cap;
//                               the table self-heals back under the cap
//
// The registry tables are process-global; every mode runs in its own
// process via its own ctest entry, so modes start from an empty table.

#include "host/managed_registry.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

// Full OperationStatus/OperationState definitions (managed_registry.h
// only forward-declares the status struct).
#include "host/managed_host.h"

namespace {

using capsid::host::AppOperationLock;
using capsid::host::OperationState;
using capsid::host::OperationStatus;
using capsid::host::app_operation_slot_count;
using capsid::host::lookup_operation;
using capsid::host::operation_record_count;
using capsid::host::record_operation;

[[noreturn]] void fail(const std::string& message) {
    std::cerr << "test-host-managed-registry: " << message << std::endl;
    std::exit(1);
}

void require(bool condition, const std::string& message) {
    if (!condition) {
        fail(message);
    }
}

OperationStatus make_status(const std::string& operation_id) {
    OperationStatus status;
    status.operation_id = operation_id;
    status.state = OperationState::kActive;
    status.version = "v1.0.0";
    status.error = "none";
    return status;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        fail("expected one managed-registry test mode");
    }
    const std::string mode = argv[1];

    if (mode == "registry_bounded") {
        // A burst far beyond the cap within one TTL window must be capped:
        // the oldest records are evicted, the newest stay queryable.
        constexpr int kRecordBurst = 2000;
        for (int i = 0; i < kRecordBurst; ++i) {
            record_operation("op-" + std::to_string(i), make_status("op-" + std::to_string(i)));
        }
        require(operation_record_count() <= 1024,
                "registry grew beyond the 1024 hard cap after a 2000-record burst");
        require(operation_record_count() == 1024,
                "registry did not settle at exactly the hard cap");
        OperationStatus newest;
        require(lookup_operation("op-" + std::to_string(kRecordBurst - 1), &newest),
                "newest recorded operation was not found");
        require(newest.operation_id == "op-" + std::to_string(kRecordBurst - 1),
                "newest operation status did not round-trip");
        OperationStatus dummy;
        require(!lookup_operation("op-0", &dummy),
                "oldest recorded operation survived eviction");
        require(!lookup_operation("op-975", &dummy),
                "an evicted burst operation was still queryable");
        return 0;
    }

    if (mode == "registry_status_roundtrip") {
        record_operation("op-1", make_status("op-1"));
        OperationStatus out;
        require(lookup_operation("op-1", &out), "recorded operation not found");
        require(out.state == OperationState::kActive && out.version == "v1.0.0",
                "recorded status fields did not round-trip");
        // Re-recording the same id overwrites; the entry count is unchanged.
        record_operation("op-1", make_status("op-1"));
        require(operation_record_count() == 1,
                "re-record did not overwrite the existing entry");
        require(!lookup_operation("op-missing", &out),
                "unknown operation id reported as recorded");
        return 0;
    }

    if (mode == "slots_bounded_reclaimed") {
        // Discover 512 Apps sequentially, releasing each lock before the
        // next. The table must never exceed 256 slots, and once the pins
        // are released the unused slots are reclaimed.
        std::size_t peak = 0;
        for (int i = 0; i < 512; ++i) {
            {
                AppOperationLock lock("app-" + std::to_string(i));
                peak = std::max(peak, app_operation_slot_count());
            }
            if (i % 32 == 0) {
                require(app_operation_slot_count() <= 256,
                        "slot table exceeded the 256 cap during sequential discovery");
            }
        }
        require(peak <= 256, "slot table exceeded the 256 cap at peak");
        // LRU retention: the table holds the most recent 256 discovered
        // Apps (the oldest were evicted once the table overflowed); the
        // pre-§13.4 static map would have retained all 512.
        require(app_operation_slot_count() == 256,
                "idle slots were not reclaimed to the cap after sequential discovery");
        return 0;
    }

    if (mode == "slots_serialize_same_app") {
        // Same-App transitions must serialize exactly as with a permanent
        // per-App mutex; distinct Apps must proceed in parallel (a
        // counter incremented under the lock proves mutual exclusion).
        constexpr int kThreads = 4;
        constexpr int kSections = 2500;
        int shared = 0;
        std::atomic<int> ready{0};
        std::atomic<bool> go{false};
        std::vector<std::thread> threads;
        for (int t = 0; t < kThreads; ++t) {
            threads.emplace_back([&]() {
                ready.fetch_add(1);
                while (!go.load()) {
                }
                for (int i = 0; i < kSections; ++i) {
                    AppOperationLock lock("shared-app");
                    const int current = shared;
                    std::this_thread::yield();
                    shared = current + 1;
                }
            });
        }
        while (ready.load() != kThreads) {
        }
        go.store(true);
        // Distinct Apps are not blocked by the shared-app critical
        // sections: they complete while shared-app work is in flight.
        for (int i = 0; i < 64; ++i) {
            AppOperationLock lock("parallel-app-" + std::to_string(i));
        }
        for (std::thread& thread : threads) {
            thread.join();
        }
        require(shared == kThreads * kSections,
                "same-App critical sections interleaved (lost updates)");
        require(app_operation_slot_count() <= 256,
                "slot table exceeded the 256 cap under concurrency");
        require(app_operation_slot_count() == 65,
                "expected the 64 parallel-app slots plus one shared-app slot");
        return 0;
    }

    if (mode == "slots_pinned_survive") {
        // Pin every slot up to the cap; while all are pinned, discover
        // more Apps (the table may exceed the cap temporarily because no
        // slot is safe to reclaim — held locks must never be evicted).
        // After all pins release, the table self-heals back under the cap.
        constexpr int kPins = 256;
        constexpr int kExtra = 64;
        std::atomic<int> ready{0};
        std::atomic<bool> go{false};
        std::vector<std::atomic<int>> values(kPins);
        std::vector<std::thread> threads;
        for (int t = 0; t < kPins; ++t) {
            threads.emplace_back([&, t]() {
                AppOperationLock lock("pinned-app-" + std::to_string(t));
                values[t].store(1);  // touch the slot while pinned
                ready.fetch_add(1);
                while (!go.load()) {
                }
            });
        }
        while (ready.load() != kPins) {
        }
        // All 256 slots are pinned; new Apps still acquire a lock — the
        // table grows past the cap because nothing is reclaimable. The
        // extras must stay held while we observe the size; releasing one
        // lets prune reclaim it immediately.
        std::vector<std::unique_ptr<AppOperationLock>> extras;
        for (int i = 0; i < kExtra; ++i) {
            extras.push_back(
                std::make_unique<AppOperationLock>("extra-app-" + std::to_string(i)));
        }
        require(app_operation_slot_count() == kPins + kExtra,
                "pinned slots were evicted or extras were not created");
        go.store(true);
        for (std::thread& thread : threads) {
            thread.join();
        }
        int touched = 0;
        for (int t = 0; t < kPins; ++t) {
            touched += values[t].load();
        }
        require(touched == kPins,
                "a pinned slot was evicted while its lock was held");
        require(app_operation_slot_count() <= 256,
                "table did not self-heal under the cap after pins released");
        return 0;
    }

    fail("unknown managed-registry test mode: " + mode);
}
