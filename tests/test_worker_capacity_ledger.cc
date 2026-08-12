// §9.4 weighted capacity ledger unit tests.
//
// Modes (argc == 2):
//   ledger_fresh_budget          — fresh reserve/commit/abort + steady gate
//   ledger_replace_surge_gate    — replacement needs surge; overlap peak;
//                                  commit category switch; absolute ceiling
//   ledger_retire_drain_release  — begin_retire / release_drained
//   ledger_no_surge_refuses      — surge=0 refuses zero-downtime replace
//
// These cover the accounting contract; the integration wiring (reserve
// before spawn, reaper-finished release) is exercised by the managed
// Host suite.

#include "host/worker_capacity_ledger.h"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

using capsid::host::WorkerCapacityLedger;

[[noreturn]] void fail(const std::string& message) {
    std::cerr << "test-host-worker-capacity-ledger: " << message
              << std::endl;
    std::exit(1);
}

void require(bool condition, const std::string& message) {
    if (!condition) {
        fail(message);
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        fail("expected one ledger test mode");
    }
    const std::string mode = argv[1];

    if (mode == "ledger_fresh_budget") {
        // workers_total=4, surge=0 (the v1 default): fresh deploys fit
        // the steady budget, the total is never exceeded.
        WorkerCapacityLedger ledger(4, 0);
        require(ledger.reserve_fresh("orders", 3),
                "3-worker fresh deploy did not fit workersTotal 4");
        require(ledger.steady_used() == 3 &&
                    ledger.steady_of("orders") == 3 && ledger.holds("orders"),
                "fresh reserve did not hold the steady count");
        require(!ledger.reserve_fresh("payments", 2),
                "2-worker fresh deploy exceeded the steady budget");
        require(!ledger.reserve_fresh("orders", 2),
                "fresh reserve over the held count was not refused");
        ledger.commit_fresh("orders");
        require(ledger.steady_used() == 3,
                "fresh commit changed the held steady count");

        // A failed fresh deploy rolls its reserve back.
        require(ledger.reserve_fresh("billing", 1),
                "1-worker fresh deploy did not fit the remaining budget");
        ledger.abort_reserve("billing", 1, /*replacement=*/false);
        require(ledger.steady_used() == 3 && !ledger.holds("billing"),
                "abort_reserve did not roll the fresh count back");

        // A second App can still fit the remaining steady budget.
        require(ledger.reserve_fresh("payments", 1),
                "1-worker fresh deploy did not fit after the abort");
        ledger.commit_fresh("payments");
        require(ledger.steady_used() == 4,
                "steady budget was not fully consumed");
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode == "ledger_replace_surge_gate") {
        // workers_total=4, surge=3: a 3→3 replace needs the surge budget
        // for the overlap; the steady state after the swap still fits.
        WorkerCapacityLedger ledger(4, 3);
        require(ledger.reserve_fresh("orders", 3),
                "serving pool did not fit the steady budget");
        ledger.commit_fresh("orders");
        require(ledger.reserve_replace("orders", 3),
                "3→3 replace did not fit surge 3");
        require(ledger.surge_used() == 3 &&
                    ledger.surge_of("orders") == 3 &&
                    ledger.steady_of("orders") == 3,
                "replacement reserve did not hold the new pool under "
                "surge");
        require(ledger.steady_used() + ledger.surge_used() == 6,
                "overlap exceeded the absolute ceiling "
                "(workersTotal + surge)");
        ledger.commit_replace("orders", /*new=*/3, /*old=*/3);
        require(ledger.steady_of("orders") == 3 &&
                    ledger.steady_used() == 3 &&
                    ledger.surge_of("orders") == 3 &&
                    ledger.surge_used() == 3,
                "commit_replace did not switch the categories "
                "(new→steady, old→surge)");

        // The old pool's reaper finished: only now its count is released.
        ledger.release_drained("orders", 3);
        require(ledger.surge_used() == 0 && ledger.steady_used() == 3,
                "release_drained did not release the drained pool");

        // The DRAIN side of the overlap can be the peak: a 3→1 replace
        // keeps the new pool small but must still fit the old pool's
        // surge count (max(1, 3) = 3) while the reaper runs.
        require(ledger.reserve_replace("orders", 1),
                "3→1 replace did not fit the overlap peak max(1,3)=3");
        require(ledger.surge_used() == 1 && ledger.steady_used() == 3,
                "3→1 reserve held the new pool under surge");
        ledger.commit_replace("orders", /*new=*/1, /*old=*/3);
        require(ledger.steady_of("orders") == 1 &&
                    ledger.steady_used() == 1 &&
                    ledger.surge_of("orders") == 3 &&
                    ledger.surge_used() == 3,
                "3→1 commit did not move the old pool into surge");

        // The old pool is still draining (surge 3): a second replacement
        // cannot fit the peak on top of the held surge — the reaper
        // must finish first.
        require(!ledger.reserve_replace("orders", 1),
                "replace over an undrained pool was not refused");
        require(ledger.steady_used() == 1 && ledger.surge_used() == 3,
                "refused replace over an undrained pool held a count");

        // The reaper finished: the drain count is released and the
        // steady budget is free again (the App now serves 1 worker).
        ledger.release_drained("orders", 3);
        require(ledger.surge_used() == 0 && ledger.steady_used() == 1,
                "release_drained did not free the surge budget");
        require(ledger.reserve_fresh("payments", 3),
                "freed budget did not serve a fresh deploy");
        ledger.commit_fresh("payments");
        require(ledger.steady_used() == 4,
                "steady budget was not fully consumed after the drain");
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode == "ledger_retire_drain_release") {
        WorkerCapacityLedger ledger(6, 2);
        require(ledger.reserve_fresh("orders", 4),
                "serving pool did not fit the steady budget");
        ledger.commit_fresh("orders");

        // Retire: the serving pool leaves steady for surge (draining).
        // No budget gate: the process count only shrinks.
        ledger.begin_retire("orders", 4);
        require(ledger.steady_of("orders") == 0 &&
                    ledger.steady_used() == 0 &&
                    ledger.surge_of("orders") == 4 &&
                    ledger.surge_used() == 4 && !ledger.holds("orders"),
                "begin_retire did not move the pool into surge");

        // The reaper finishes: only now the count is released.
        ledger.release_drained("orders", 4);
        require(ledger.surge_of("orders") == 0 &&
                    ledger.surge_used() == 0,
                "release_drained did not return the surge count");

        // The released budget serves a fresh deploy again.
        require(ledger.reserve_fresh("payments", 5),
                "released budget did not become available");
        ledger.commit_fresh("payments");
        require(ledger.steady_used() == 5,
                "fresh deploy after retire did not settle");
        std::cout << "PASS" << std::endl;
        return 0;
    }

    if (mode == "ledger_no_surge_refuses") {
        // v1 default surge = 0: a replace of a serving pool is refused —
        // the zero-downtime overlap cannot exist — and nothing is held.
        WorkerCapacityLedger ledger(4, 0);
        require(ledger.reserve_fresh("orders", 2),
                "serving pool did not fit the steady budget");
        ledger.commit_fresh("orders");
        require(!ledger.reserve_replace("orders", 2),
                "replace without any surge budget was not refused");
        require(ledger.steady_used() == 2 && ledger.surge_used() == 0,
                "refused replace held a count");
        require(ledger.steady_of("orders") == 2,
                "refused replace disturbed the serving pool");

        // A replace whose steady swap fits but whose peak does not is
        // also refused (surge 1: peak 2 > 1).
        WorkerCapacityLedger tight(4, 1);
        require(tight.reserve_fresh("orders", 2),
                "serving pool did not fit the tight budget");
        tight.commit_fresh("orders");
        require(!tight.reserve_replace("orders", 2),
                "replace whose overlap peak exceeded surge was not "
                "refused");

        // And the refused replace left the serving pool untouched.
        require(tight.steady_of("orders") == 2 && !tight.holds("other"),
                "refused replace mutated the ledger");
        std::cout << "PASS" << std::endl;
        return 0;
    }

    fail("unknown ledger test mode: " + mode);
}
