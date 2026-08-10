// Managed-soak memory/token wave client (WP-09 §13.6, audit §10.2-8).
//
// Protocol: spawn the worker with the soak fixture, take a baseline
// memory snapshot, then run WAVE_COUNT waves of REQUESTS_PER_WAVE
// requests that are all canceled mid-flight. Cancellation drives the
// runtime's reclaim path (which ticks GC on retry — the same mechanism
// the audit's diagnostic worker exercised with JS_RunGC), so each wave
// is a full allocate/reclaim/GC round trip. After every wave a fresh
// CAPSID_EVENT_MEMORY_METRICS snapshot is taken.
//
// Invariant: after the warm-up wave the heap metrics must converge —
// post-wave object/property/used counts settle (no monotonic growth
// across the final waves, and the last wave stays within a small slack
// of the warm-up level). A leak of even a few retained objects per wave
// compounds beyond the slack across thousands of soak waves, so the
// 24h/72h run with default parameters tightens this substantially.
//
// Usage: soak-memory-waves <capsid-worker> <fixture.js> [waves] [per-wave]
// Prints one JSON line to stdout: {"baseline":...,"waves":[...],
// "converged":bool,"reason":str}; exits 0 when converged.

#include "capsid/runtime.h"

#include <poll.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

void fail(const std::string &message) {
    std::cerr << "FAIL: " << message << std::endl;
    std::exit(1);
}

void require_result(capsid_result result, const char *operation) {
    if (result != CAPSID_OK) {
        fail(std::string(operation) + ": " + capsid_result_string(result));
    }
}

std::string read_file(const char *path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        fail(std::string("cannot open fixture: ") + path);
    }
    return std::string(std::istreambuf_iterator<char>(input),
                       std::istreambuf_iterator<char>());
}

void wait_for_ready(capsid_worker *worker) {
    const std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(20);
    for (;;) {
        const capsid_result flush = capsid_worker_flush(worker);
        if (flush != CAPSID_OK && flush != CAPSID_WOULD_BLOCK) {
            fail(std::string("startup flush: ") + capsid_result_string(flush));
        }
        capsid_event event = {};
        event.struct_size = sizeof(event);
        const capsid_result result = capsid_worker_next_event(worker, &event);
        if (result == CAPSID_OK) {
            if (event.type == CAPSID_EVENT_READY) {
                return;
            }
            if (event.type == CAPSID_EVENT_ERROR) {
                // Same decode as managed_host.cc: the payload is the
                // worker's ASCII failure reason.
                const std::string detail(
                    reinterpret_cast<const char*>(event.payload.data),
                    event.payload.size);
                fail("worker error before READY: " +
                     (detail.empty() ? "(no detail)" : detail));
            }
            if (event.type == CAPSID_EVENT_EXIT) {
                fail("worker exited before READY");
            }
            continue;
        }
        if (result != CAPSID_WOULD_BLOCK) {
            fail(std::string("startup event: ") + capsid_result_string(result));
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            fail("timed out waiting for READY");
        }
        struct pollfd descriptor = {};
        descriptor.fd = capsid_worker_fd(worker);
        descriptor.events = POLLIN | (flush == CAPSID_WOULD_BLOCK ? POLLOUT : 0);
        poll(&descriptor, 1, 50);
    }
}

// Sends the memory-metrics request and reads the next events until the
// metrics snapshot arrives (or an error/exit ends the channel).
bool request_metrics(capsid_worker *worker, capsid_memory_metrics *out,
                     const char *stage,
                     const std::chrono::steady_clock::time_point &deadline) {
    require_result(capsid_worker_request_memory_metrics(worker),
                   "request memory metrics");
    for (;;) {
        const capsid_result flush = capsid_worker_flush(worker);
        if (flush != CAPSID_OK && flush != CAPSID_WOULD_BLOCK) {
            fail(std::string("metrics flush: ") + capsid_result_string(flush));
        }
        capsid_event event = {};
        event.struct_size = sizeof(event);
        const capsid_result result = capsid_worker_next_event(worker, &event);
        if (result == CAPSID_OK) {
            if (event.type == CAPSID_EVENT_MEMORY_METRICS) {
                // CAPSID_OK is 0; a bare `!` test would fire on success.
                if (capsid_memory_metrics_decode(&event, out) != CAPSID_OK) {
                    fail(std::string("cannot decode memory metrics [") +
                         stage + "] (payload=" +
                         std::to_string(event.payload.size) + ")");
                }
                return true;
            }
            if (event.type == CAPSID_EVENT_EXIT || event.type == CAPSID_EVENT_ERROR) {
                return false;
            }
            continue;
        }
        if (result != CAPSID_WOULD_BLOCK) {
            fail(std::string("metrics event: ") + capsid_result_string(result));
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            fail("timed out waiting for memory metrics");
        }
        struct pollfd descriptor = {};
        descriptor.fd = capsid_worker_fd(worker);
        descriptor.events = POLLIN | (flush == CAPSID_WOULD_BLOCK ? POLLOUT : 0);
        poll(&descriptor, 1, 20);
    }
}

struct Snapshot {
    uint64_t used = 0;
    uint64_t objects = 0;
    uint64_t properties = 0;
    uint64_t atoms = 0;
};

Snapshot snapshot_of(const capsid_memory_metrics &metrics) {
    Snapshot snapshot;
    snapshot.used = metrics.memory_used_size;
    snapshot.objects = metrics.object_count;
    snapshot.properties = metrics.property_count;
    snapshot.atoms = metrics.atom_count;
    return snapshot;
}

// Chunk size for the begin/cancel round-trip: the client caps inflight
// requests (default 128), so a wave is chunked into sub-batches; each
// batch is fully begun, flushed, canceled, flushed and drained to
// quiescence before the next.
static constexpr std::uint64_t kWaveBatch = 64;

// One wave: REQUESTS_PER_WAVE inflight requests, all canceled, then a
// memory snapshot. A driver cancel produces no terminal frame client-side
// (the client erased the request states; late frames for canceled ids are
// dropped), so the drain waits for the channel to go quiet — bounded by a
// deadline — instead of counting terminals. Quiescence means the worker
// processed the cancels (the abort settled each chain; the reclaim tick
// runs after, on the worker's own loop). An EXIT here fails fast.
void run_wave(capsid_worker *worker, std::uint64_t first_id,
              std::uint64_t count) {
    std::uint64_t remaining = count;
    std::uint64_t base = first_id;
    while (remaining > 0) {
        const std::uint64_t chunk = std::min(remaining, kWaveBatch);
        for (std::uint64_t i = 0; i < chunk; ++i) {
            require_result(
                capsid_worker_begin_bodyless_request(
                    worker, base + i, "GET", "https://soak.test/slow?ms=200",
                    NULL, 0),
                "begin soak request");
        }
        require_result(capsid_worker_flush(worker), "flush wave");
        for (std::uint64_t i = 0; i < chunk; ++i) {
            (void)capsid_worker_cancel(worker, base + i);
        }
        require_result(capsid_worker_flush(worker), "flush cancels");
        const std::chrono::steady_clock::time_point deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(30);
        int quiet_rounds = 0;
        while (std::chrono::steady_clock::now() < deadline) {
            capsid_event event = {};
            event.struct_size = sizeof(event);
            const capsid_result result = capsid_worker_next_event(worker, &event);
            if (result == CAPSID_OK) {
                switch (event.type) {
                    case CAPSID_EVENT_EXIT:
                        fail("worker exited mid-wave");
                    default:
                        continue;
                }
            }
            if (result != CAPSID_WOULD_BLOCK) {
                fail(std::string("wave event: ") + capsid_result_string(result));
            }
            struct pollfd descriptor = {};
            descriptor.fd = capsid_worker_fd(worker);
            descriptor.events = POLLIN;
            const int ready = poll(&descriptor, 1, 20);
            if (ready == 0) {
                quiet_rounds += 1;
                if (quiet_rounds >= 2) {
                    break;  // channel idle: the cancels were processed
                }
            } else {
                quiet_rounds = 0;
            }
        }
        base += chunk;
        remaining -= chunk;
    }
}

// Convergence: no monotonic growth over the final three waves and the
// last wave within 2% slack of the warm-up level.
bool converged(const std::vector<Snapshot> &snapshots, std::string *reason) {
    if (snapshots.size() < 4) {
        *reason = "too few waves";
        return false;
    }
    const std::size_t n = snapshots.size();
    const Snapshot &warm = snapshots[1];  // after the first full wave
    const Snapshot &last = snapshots[n - 1];
    for (std::size_t i = n - 3; i <= n - 2; ++i) {
        if (snapshots[i + 1].objects > snapshots[i].objects ||
            snapshots[i + 1].used > snapshots[i].used) {
            *reason = "monotonic object/used growth in the final waves";
            return false;
        }
    }
    if (last.objects > warm.objects * 102 / 100 ||
        last.used > warm.used * 102 / 100) {
        *reason = "last wave exceeded the warm-up level by more than 2%";
        return false;
    }
    return true;
}

}  // namespace

int main(int argc, char **argv) {
    const unsigned long waves =
        argc >= 4 ? std::strtoul(argv[3], nullptr, 10) : 6;
    const unsigned long per_wave =
        argc >= 5 ? std::strtoul(argv[4], nullptr, 10) : 200;
    if (argc < 3 || waves < 4 || per_wave < 1) {
        fail("usage: soak-memory-waves <worker> <fixture.js> [waves>=4] "
             "[per-wave>=1]");
    }
    const std::string bundle = read_file(argv[2]);

    // Bare client spawn: module authorization is membership in the
    // capability policy's allowed_modules (capability_policy.cc), the same
    // gate the managed host grants from host.json + capsid.json. The soak
    // fixture imports capsid:env at load, so the client grants the module
    // and an APP_* env read rule (mirroring what the host grants); the
    // memory protocol itself only exercises /slow.
    const char* module_names[] = {"capsid:env"};
    capsid_permission_rule env_rule;
    capsid_permission_rule_init(&env_rule);
    env_rule.action = CAPSID_PERMISSION_ALLOW;
    env_rule.permission = CAPSID_PERMISSION_ENV;
    env_rule.resource = "APP_*";
    env_rule.rule_id = 1;
    capsid_capability_policy policy;
    capsid_capability_policy_init(&policy);
    policy.allowed_modules = module_names;
    policy.allowed_module_count = 1;
    policy.rules = &env_rule;
    policy.rule_count = 1;

    capsid_worker_config config;
    capsid_worker_config_init(&config);
    config.worker_path = argv[1];
    config.capability_policy = &policy;
    config.strict_sandbox = 0;
    config.request_timeout_ms = 5000;

    capsid_worker *worker = NULL;
    require_result(capsid_worker_spawn(&config, &worker), "spawn worker");
    require_result(
        capsid_worker_load_bundle(
            worker, reinterpret_cast<const uint8_t *>(bundle.data()),
            bundle.size()),
        "load bundle");
    wait_for_ready(worker);

    const std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(60);

    capsid_memory_metrics baseline_metrics;
    capsid_memory_metrics_init(&baseline_metrics);
    if (!request_metrics(worker, &baseline_metrics, "baseline", deadline)) {
        fail("baseline metrics unavailable");
    }
    std::vector<Snapshot> snapshots;
    snapshots.push_back(snapshot_of(baseline_metrics));

    std::uint64_t next_id = 1;
    for (unsigned long w = 1; w <= waves; ++w) {
        run_wave(worker, next_id, per_wave);
        next_id += per_wave;
        capsid_memory_metrics metrics;
        capsid_memory_metrics_init(&metrics);
        const std::string stage = "wave-" + std::to_string(w);
        if (!request_metrics(worker, &metrics, stage.c_str(), deadline)) {
            fail("post-wave metrics unavailable");
        }
        snapshots.push_back(snapshot_of(metrics));
    }

    capsid_worker_destroy(worker);

    std::string reason;
    const bool ok = converged(snapshots, &reason);

    std::printf("{\"baseline\":{\"used\":%llu,\"objects\":%llu,"
                "\"properties\":%llu,\"atoms\":%llu},\"waves\":[",
                static_cast<unsigned long long>(snapshots[0].used),
                static_cast<unsigned long long>(snapshots[0].objects),
                static_cast<unsigned long long>(snapshots[0].properties),
                static_cast<unsigned long long>(snapshots[0].atoms));
    for (std::size_t i = 1; i < snapshots.size(); ++i) {
        if (i > 1) {
            std::printf(",");
        }
        std::printf("{\"used\":%llu,\"objects\":%llu,\"properties\":%llu}",
                    static_cast<unsigned long long>(snapshots[i].used),
                    static_cast<unsigned long long>(snapshots[i].objects),
                    static_cast<unsigned long long>(snapshots[i].properties));
    }
    std::printf("],\"converged\":%s,\"reason\":\"%s\"}\n",
                ok ? "true" : "false", reason.c_str());
    return ok ? 0 : 1;
}
