# Bench Directory Layout

Everything under `bench/results/` is immutable run evidence referenced by
[docs/performance-benchmarks.md](../docs/performance-benchmarks.md); it is
never cleaned up or regenerated.

**The measurement protocol is specified in [SPEC.md](SPEC.md)** — lanes,
data pipeline, evidence rules, and site conventions. When a script and the
spec disagree, fix one of them in the same change.

## Active harness (current release lanes)

| Path | Purpose |
| --- | --- |
| `loadgen/` | Frozen closed-loop load generator (Go): keep-alive, fixed inflight, exact-body verdicts, warmup/measured phases. Build into `bin/loadgen` via `build.sh`. |
| `qps-lane.sh` + `qps-lane.py` + `qps_probe.py` + `qps_inputs.py` | Owned five-stack serving producer: managed/strict one-worker pool, exact responses, fresh trickle/loaded forests, recursive PSS, per-round Go identities and sealed dependency provenance. See [QPS producer](perf-evidence.md#qps-producer). |
| `cold-start.sh` + `cold-start.py` + `cold_probe.py` + `cold-start.cc` | Owned cold-start lane: exact first/warm responses, discarded + measured rounds, recursive stable PSS and sealed contract producer; strict C-ABI source/bytecode vs Node/Deno/workerd. See [cold producer](perf-evidence.md#cold-producer). |
| `density-lane.sh` | Python-controlled managed/strict density lane: recursive owned-process PSS, proved per-worker cold/warm coverage, bounded final verification and explicit cap status. Formal mode requires build/session metadata; restricted `--observation` output cannot qualify publication. See [archive contract](perf-evidence.md#density-producer). |
| `density_conditioning.py` + `density_evidence.py` | One-request-per-instance prewarm waves, owned worker dispatch/resume barriers and independent replay of responses, counters, process membership, whole-tree PSS and cleanup. The original 300-second coverage budget and request ceilings remain. |
| `collect-perf-point.py` | Validates and merges the lanes into `perf-site/data.js` (one append-only point per release; duplicate cells and uncorroborated identities abort). |
| `bench-apps/` | Reference-stack fixtures for the QPS lane (FastAPI, Flask, PHP Slim with vendored deps). |
| `build.sh` | Builds `bin/loadgen` (active lanes) and `bin/baseline-gateway` (A/B harness below). |

On Linux testing configurations with local Go 1.22 or newer, CMake also builds
the repository `benchmark-loadgen` target as part of ALL, without downloading a
toolchain or modules. Run the actual short HTTP correctness fixtures and portable
round rules with:

```sh
cmake --build BUILD --target benchmark-loadgen
ctest --test-dir BUILD -R '^benchmark_(loadgen_correctness|qps_control|qps_producer|qps_probe)$' --no-tests=error --output-on-failure
```

The executable is `BUILD/benchmark-tools/loadgen`. An existing `bench/bin/loadgen`
is not rewritten by this target; select the new path explicitly. Portable QPS rules and producer preflight
checks run on Windows too; actual process/PSS/Go tests require Linux. Synthetic
full-archive tests are explicitly confined to temporary repositories. The
public producer defaults to five rounds, 5 s warmup + 15 s measurement, 64/64,
15 s at 10 Hz trickle and managed/strict posture. `--observation` can restrict
the protocol but produces no publication `lane.json`. Short fixtures, actual
five-stack smoke and synthetic archives are correctness evidence; the formal
quiet-session three-lane qualification remains pending.

The Linux C-ABI cold probe can now be built from the configured testing tree:

```sh
cmake --build BUILD --target benchmark-cold-start test-benchmark-cold-measurement
ctest --test-dir BUILD -R '^benchmark_cold_(measurement|tool)$' --no-tests=error --output-on-failure
```

The resulting probe is `BUILD/cold-start`; select it through `COLD`/`--cold`
(default `$BUILD_DIR/cold-start`). The portable measurement model also runs on Windows.
Actual worker/PSS tests require Linux and a host/worker testing configuration.
The probe verifies exact first/warm responses, public child PID, stable positive
recursive PSS and failure/cancellation cleanup of its C-ABI worker. This is a
probe plus an owned controller and archive producer. Formal cold mode requires
clean measured-build/session metadata and the complete protocol; restricted
`--observation` runs cannot qualify publication. Actual same-session formal three-lane measurements remain pending.

## A/B evidence harness (exercised by tests)

| Path | Purpose |
| --- | --- |
| `run-ab.sh` + `evidence.py` + `fake/` + `wrappers/` + `baseline-gateway/` | The A/B evidence harness driven and validated by `tests/test_host_single_worker_ab_evidence.mjs`. Do not modify without updating that test. |

## Conventions

- Lanes pin every server and the client to `SUT_CPUSET` (4-core protocol).
- Every run directory records `manifest.txt` (environment, versions,
  SHA-256 of binaries/fixtures/runner) and a final `sha256sums.txt`;
  archived under `results/<lane>-v<version>-<date>/` and whitelisted in
  `.gitignore` (results are ignore-by-default, opt-in by evidence).
- Superseded session scripts (the v0.2.0 four-stack QPS session and its
  fixtures) were removed; git history and the recorded artifacts under
  `results/` preserve the published v0.2.0 tables.
