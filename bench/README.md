# Bench Directory Layout

Everything under `bench/results/` is immutable run evidence referenced by
[docs/performance-benchmarks.md](../docs/performance-benchmarks.md); it is
never cleaned up or regenerated.

## Active harness (current release lanes)

| Path | Purpose |
| --- | --- |
| `loadgen/` | Frozen closed-loop load generator (Go): keep-alive, fixed inflight, exact-body verdicts, warmup/measured phases. Build into `bin/loadgen` via `build.sh`. |
| `cold-start.sh` + `cold-start.cc` + `gen-cold-start-apps.py` | Cold/warm start and worker-memory lane: capsid source/bytecode vs Node vs Deno vs workerd at 10k/100k/1M bundle sizes. |
| `qps-lane.sh` | Serving-throughput lane: capsid static-pool(2) vs workerd vs FastAPI/uvicorn(2) vs Flask/gunicorn(2) vs PHP Slim php-fpm×2 + nginx on the frozen matrix workloads; records per-stack serving PSS. |
| `collect-perf-point.py` | Merges the two lanes above into `perf-site/data.js` (one append-only point per release). |
| `bench-apps/` | Reference-stack fixtures for the QPS lane (FastAPI, Flask, PHP Slim with vendored deps). |
| `build.sh` | Builds `bin/loadgen` (active lanes) and `bin/baseline-gateway` (A/B harness below). |

## A/B evidence harness (exercised by tests)

| Path | Purpose |
| --- | --- |
| `run-ab.sh` + `evidence.py` + `fake/` + `wrappers/` | The A/B evidence harness driven and validated by `tests/test_host_single_worker_ab_evidence.mjs`. Do not modify without updating that test. |

## Conventions

- Lanes pin every server and the client to `SUT_CPUSET` (4-core protocol).
- Every run directory records `manifest.txt` (environment, versions,
  SHA-256 of binaries/fixtures/runner) and a final `sha256sums.txt`.
- Superseded session scripts (the v0.2.0 four-stack QPS session and its
  fixtures) were removed; git history and the recorded artifacts under
  `results/` preserve the published v0.2.0 tables.
