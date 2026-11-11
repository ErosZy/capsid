#include "host/worker_capacity_ledger.h"
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

using capsid::host::WorkerCapacityLedger;
using capsid::host::WorkerPoolCapacityLease;
void require(bool value, const char* message) {
    if (!value) { std::cerr << "FAIL: " << message << std::endl; std::exit(1); }
}
int main(int argc, char** argv) {
    require(argc == 2, "expected capacity lease scenario"); const std::string mode = argv[1];
    if (mode == "lifecycle") {
        WorkerCapacityLedger ledger(3, 2);
        require(ledger.reserve_fresh("orders", 2), "old warm reserve");
        auto old = std::make_shared<WorkerPoolCapacityLease>(&ledger, "orders", 2); old->activate_fresh();
        require(ledger.reserve_replace("orders", 1), "new warm reserve");
        auto next = std::make_shared<WorkerPoolCapacityLease>(&ledger, "orders", 1); next->activate_replacement(old);
        require(ledger.steady_used() == 1 && ledger.surge_used() == 2, "old and new generations have distinct categories");
        old->reaped(); old->reaped();
        require(ledger.steady_used() == 1 && ledger.surge_used() == 0, "old release is idempotent");
        next->begin_drain(); require(ledger.steady_used() == 0 && ledger.surge_used() == 1, "current drain holds surge until reap");
        next->reaped(); next->begin_drain();
        require(!ledger.holds("orders") && ledger.steady_used() == 0 && ledger.surge_used() == 0, "retired fleet releases all counts once");
    } else if (mode == "reaped-before-activation") {
        WorkerCapacityLedger ledger(1, 1);
        require(ledger.reserve_fresh("orders", 1), "old reserve");
        auto old = std::make_shared<WorkerPoolCapacityLease>(&ledger, "orders", 1); old->activate_fresh();
        require(ledger.reserve_replace("orders", 1), "reserve new warm before old reap");
        old->reaped();
        require(ledger.steady_used() == 0 && ledger.surge_used() == 1, "only the warming pool remains held");
        auto next = std::make_shared<WorkerPoolCapacityLease>(&ledger, "orders", 1); next->activate_replacement(old);
        require(ledger.steady_used() == 1 && ledger.surge_used() == 0, "commit cannot add the already reaped old fleet to surge");
        next->reaped(); require(ledger.steady_used() == 0 && ledger.surge_used() == 0, "active reap releases steady directly");
    } else if (mode == "aborted-warm") {
        WorkerCapacityLedger ledger(1, 1);
        require(ledger.reserve_fresh("orders", 1), "warm reserve");
        auto warm = std::make_shared<WorkerPoolCapacityLease>(&ledger, "orders", 1); warm->reaped();
        require(ledger.steady_used() == 1, "coordinator still owns an unsettled warm reserve");
        warm->abort(false); warm->reaped(); warm->abort(false);
        require(ledger.steady_used() == 0 && ledger.surge_used() == 0, "abort and reap never double-release");
        require(ledger.reserve_fresh("invoices", 1), "aborted actual capacity can serve another App");
    } else if (mode == "commit-reap-race") {
        for (unsigned i = 0; i < 50; ++i) {
            WorkerCapacityLedger ledger(1, 1);
            require(ledger.reserve_fresh("orders", 1), "old race reserve");
            auto old = std::make_shared<WorkerPoolCapacityLease>(&ledger, "orders", 1); old->activate_fresh();
            require(ledger.reserve_replace("orders", 1), "new race reserve");
            auto next = std::make_shared<WorkerPoolCapacityLease>(&ledger, "orders", 1);
            std::atomic<unsigned> ready{0}; std::atomic<bool> go{false};
            const auto barrier = [&] { ++ready; while (!go.load()) std::this_thread::yield(); };
            std::thread reap([&] { barrier(); old->reaped(); });
            std::thread commit([&] { barrier(); next->activate_replacement(old); });
            while (ready.load() != 2) { std::this_thread::yield(); }
            go.store(true); reap.join(); commit.join();
            require(ledger.steady_used() == 1 && ledger.surge_used() == 0, "commit/reap interleaving preserves held capacity");
            next->reaped(); require(ledger.steady_used() == 0 && ledger.surge_used() == 0, "race releases exactly once");
        }
    } else require(false, "known capacity lease scenario");
    std::cout << "PASS: capacity lease " << mode << std::endl;
}
