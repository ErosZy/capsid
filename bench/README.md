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
| `qps-lane.sh` | Serving-throughput lane: capsid (managed coordinator + strict sandbox, one-worker pool) vs workerd vs FastAPI vs Flask vs PHP Slim — one serving process per stack, frozen matrix workloads, trickle + loaded process-tree PSS. |
| `cold-start.sh` + `cold-start.cc` + `gen-cold-start-apps.py` | Cold-start lane: spawn → first validated response; capsid source/bytecode (`--strict`) vs Node vs Deno vs workerd at 10k/100k/1M sizes. |
| `density-lane.sh` | Fill-to-budget density lane: grow a capsid strict-sandbox static pool (or workerd cells) until the descendant-tree PSS hits the budget; cold and warmed (100 req/worker) phases, binary-searched boundary with a jitter guard. |
| `collect-perf-point.py` | Validates and merges the lanes into `perf-site/data.js` (one append-only point per release; duplicate cells and uncorroborated identities abort). |
| `bench-apps/` | Reference-stack fixtures for the QPS lane (FastAPI, Flask, PHP Slim with vendored deps). |
| `build.sh` | Builds `bin/loadgen` (active lanes) and `bin/baseline-gateway` (A/B harness below). |

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
