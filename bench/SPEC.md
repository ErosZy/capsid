# Performance benchmark & perf-site specification

This document is the normative spec for the performance measurement
system behind <https://eroszy.github.io/capsid/>: the three benchmark
lanes, the data pipeline into `perf-site/data.js`, the evidence rules,
and the site's rendering conventions. `docs/performance-benchmarks.md`
holds the current conclusion-level data and cites this spec; when the
two disagree, this file wins and the docs are stale.

## 1. Principles

1. **Frozen protocol.** Every number on the site comes from a lane
   script in this directory, run verbatim. Ad-hoc measurements may be
   reported as "observed samples" but never enter `data.js`.
2. **Fail-closed.** A wrong response body, a missing sample, a pool
   that died under its warm-up, a duplicate cell, or an identity the
   fingerprint cannot corroborate aborts the run or the collection.
   Nothing is retried silently.
3. **Evidence-linked.** Each data point carries git commit/tag, a
   machine fingerprint, and SHA-256 digests of binaries, fixtures, and
   raw samples, archived under `bench/results/`.
4. **Append-only history.** `data.js` keeps one point per version;
   re-measuring a version replaces its point, earlier points are never
   edited.
5. **Production posture.** capsid is measured through the managed
   coordinator with the mandatory Linux strict sandbox (namespaces,
   Landlock, seccomp). Sandbox-off numbers are not publishable.
6. **Symmetric optimality.** Every comparison stack runs its
   framework-optimal single-serving-process configuration; the page
   states the configuration next to the numbers.

## 2. Lanes

### 2.1 Serving lane — `bench/qps-lane.sh`

- **Protocol:** frozen repository loadgen, closed loop, 64 keep-alive
  connections / 64 in-flight, 5 s warm-up + 5 measured rounds of 15 s
  per workload; exact-body verdict every round (a mismatch fails the
  lane); SUT pinned via `taskset` to `SUT_CPUSET` (default `0-3`);
  ports are prechecked and fail closed.
- **Workloads:** `matrix-json-1k`, `matrix-json-16k`, `stream` (1 KiB
  frozen b/c/d thirds). Payload bytes are shared across all stacks and
  verified per round.
- **Serving topology:** exactly one serving process per stack —
  capsid: managed coordinator + strict sandbox, one-worker fixed pool,
  Admin-API blue-green deploy, production stream-window defaults
  (`CAPSID_MANAGED=1`); workerd: single process, minimal capnp;
  FastAPI: uvicorn `--workers 1 --no-access-log` with precomputed
  bodies and async endpoints/generators; Flask: gunicorn sync `-w 1`;
  PHP: php-fpm `pm=static max_children=1` with opcache JIT tracing and
  `fastcgi_keep_conn on` behind nginx.
- **Memory:** process-tree PSS (`smaps_rollup`) sampled after the last
  round, decomposed per role; a **trickle phase** (fresh process, 15 s
  at 10 req/s) is measured **before** any load, because V8 growth is
  irreversible and the at-rest footprint must not be contaminated.
- **Output:** `cells.txt` lines `stack [workload] qps_med/p50/p95`,
  `serving_pss_mb[=role:mb;…]`, `lowqps_pss_mb[=…]`.

### 2.2 Cold-start lane — `bench/cold-start.sh` + `bench/cold-start.cc`

- **Metric:** process start → first **validated** response. Warm
  rounds and the PSS walk are reported separately and must not leak
  into `total_ms` (historical bug: shell-measured stacks once read
  ~400 ms because a 50-request curl warm loop was inside the timer).
- **Fixtures:** real-shaped JS at 10k/100k/1m, byte-aligned across
  runtimes (`gen-cold-start-apps.py`); each cell drops one warm-up run
  and reports the median of five.
- **capsid:** C-ABI tool with `--strict` (full sandbox in the spawned
  worker); source and trusted-bytecode paths both measured. Node/Deno:
  stdout READY then first curl. workerd: first TCP connect (its
  isolate initializes lazily on the first request, so the
  ready→first-response span carries that cost).

### 2.3 Density lane — `bench/density-lane.sh`

- **Metric:** the largest pool whose **whole descendant-tree PSS**
  stays under the budget (default 2048 MB), found by doubling to the
  boundary, binary search, and a jitter guard that steps the recorded
  count down until it verifies under budget.
- **Phases:** each cell is reported twice — **cold** (one real request
  per worker/cell; initialization, not static loading) and **warmed**
  (100 requests through each, then settle). A pool that dies under the
  warm-up is a failed probe, never a small reading.
- **Stacks:** capsid static-pool with the same strict sandbox
  (`CAPSID_STRICT=1`); workerd N cells in one process (minimal capnp,
  cells touched through per-cell sockets).
- **Measurement hygiene:** PSS walks are recursive (grandchildren
  count); probe ports increment in the parent shell (command
  substitution silently drops counters); teardown kills children
  before the parent; readiness waits for real readiness (e.g. celld
  accepts TCP before the app exists — a lesson recorded here after a
  503 storm produced flat lines).

## 3. Data pipeline

- `bench/collect-perf-point.py --cold-start D1 --qps D2 [--density D3]`
  parses the three `cells.txt` formats, validates, and merges the point
  into `perf-site/data.js` (same-version points are replaced).
- **Validations (fail-closed):** duplicate cold cells abort; a point
  may not carry deno/node/workerd results whose identity the
  fingerprint lacks (`machine.<key>` missing or `absent`).
- **density keying:** `density[stack][phase@size] = {count, unit,
  pssMb}`; the site renders cold and warmed rows per stack and falls
  back to footprint-arithmetic `est` values only for unmeasured sizes.
- **Provenance:** `git` commit/tag are recorded verbatim; the site
  shows `release · commit <sha8>` unless a tag verifies the release.
- Do not hand-edit `data.js`; regenerate it.

## 4. Evidence

- Every run directory carries `manifest.txt` (command line, generated
  time, commit/tag, CPU/kernel, versions and SHA-256 of every binary
  and fixture), raw samples (not aggregates), correctness verdicts,
  and `sha256sums.txt`.
- Archive under `bench/results/<lane>-v<version>-<date>/` and whitelist
  the directory in `.gitignore` (results are ignore-by-default,
  opt-in by evidence).
- Machine fingerprints must include every stack identity the cells
  reference (the collector enforces this).

## 5. Site conventions (`perf-site/`)

- Zero external dependencies; dark theme; inline JS is ES5 (no template
  literals, no arrow functions); all wide content lives in `.scroll-x`;
  grid children get `min-width: 0`; labels never wrap.
- Tables highlight capsid rows only (`tr.mine`); no winner/best
  highlighting anywhere. Bar values sit outside bars; axis breaks use
  INKBG slash marks with a `clipped · N× axis` note grouped with the
  value.
- Memory section order: equal-load trickle first, loaded second, then
  marginal worker cost, then measured density (cold → warmed rows with
  the explanation footnote). Chart gutters are measured via canvas
  `measureText`.
- The lane fingerprint panel normalizes `absent`/`n/a` to
  "not measured"; the protocol copy must match the lanes as actually
  run (rounds, warm phases, sandbox posture).
- Deployment: GitHub Actions, actions pinned to release SHAs, artifact
  upload + `deploy-pages`; pushes touching `perf-site/**` deploy.

## 6. Release procedure

1. Build a clean Release tree (the build identity gate requires
   `git status --porcelain` empty; stash CRLF noise, never commit it).
2. Run the three lanes in one quiet session on the release commit;
   capsid rows through the managed/strict posture.
3. Archive evidence, whitelist it, run the collector, audit the site at
   1280/960/820/600 px, commit, push; confirm the Pages action is
   green.
4. Update `docs/performance-benchmarks.md` tables from the new point;
   move the superseded checkpoint to the retired section.

## 7. Current boundaries

- Bytecode loading through the host requires the trusted-key signing
  flow; density numbers are source-bundle numbers.
- Sustained per-tenant traffic density (every resident worker serving
  concurrently) is future work; fill-to-budget counts bound the
  at-rest cost.
- The celld comparison is paused: serving and cold lanes produced
  verdict-clean data (json-1k 48,895 QPS; stream 31,769; cold ~9 ms),
  the density comparison was not completed and no celld rows ship.
