# Performance benchmark & perf-site specification

This document defines the measurement protocol for the [performance charts](https://rowanellis-113.github.io/capsid/):
the three benchmark lanes, the data pipeline into `perf-site/data.js`, the
evidence rules and the site's rendering conventions. See the [performance reference](../docs/performance-benchmarks.md)
for the latest accepted source and results. A measurement belongs to its
recorded source, not to a later published prerelease.

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
4. **Preserve accepted history.** Schema 1 keeps one active point per version
   and its legacy CLI replaces a repeated version. That is not permission to
   overwrite a published accepted record: a future remeasurement needs a
   reviewed separate series/schema/archive plan retaining the old result.
   The archived `v1` point and historical `0.2.x` values remain unchanged.
   A clean untagged source can qualify the existing archive contract, but must
   be visibly labeled a measured development snapshot, never a released binary.
5. **Production posture.** Serving and density measure capsid through the
   managed coordinator with the mandatory Linux strict sandbox (namespaces,
   Landlock, seccomp). Cold-start uses the public C ABI with strict workers;
   its footprint excludes the C-ABI measurement process. Sandbox-off numbers
   are not publishable.
6. **Symmetric optimality.** Every comparison stack runs its
   framework-optimal single-serving-process configuration; the page
   states the configuration next to the numbers.

## 2. Lanes

### 2.1 Serving lane — `bench/qps-lane.sh`

- **Protocol:** frozen repository loadgen, closed loop, 64 keep-alive
  connections / 64 in-flight, 5 s warm-up + 5 measured rounds of 15 s
  per workload; exact-body verdict every round (a mismatch fails the
  lane); SUT pinned via `taskset` to `SUT_CPUSET` (default `0-3`);
  ports are reserved by the parent and checked against owned listeners.
- **Workloads:** `matrix-json-1k`, `matrix-json-16k`, `stream` (1 KiB
  frozen b/c/d thirds), and `dynamic-api` (per-request `new URL` query parsing,
  request header inspection, async microtask step, 8-item dynamic order payload
  construction + FNV-1a checksum, and `JSON.stringify`). Payload bytes are
  shared across all stacks and verified byte-for-byte per round.
- **Serving topology (three explicit reference tiers):** one serving worker per
  pure-JS stack, with the entire service forest counted —
  **Tier 1 (Isolated / Sandboxed Web-Standard Runtimes)**:
  capsid: managed coordinator + strict Linux OS sandbox (Namespaces, Landlock,
  Seccomp), one-worker fixed pool, Admin-API blue-green deploy, production
  stream-window defaults (`CAPSID_MANAGED=1 CAPSID_STRICT=1`), QuickJS-NG
  interpreter (no JIT / no RWX);
  workerd: single process (V8 Isolate sandbox), minimal capnp sharing
  `qps-worker.mjs` with capsid;
  deno: single process (`Deno.serve`, `qps-deno.mjs`) under Deno's permission
  sandbox (`--allow-net=127.0.0.1 --no-prompt --no-config`, default-deny fs/env/run/ffi);
  **Tier 2 (Non-JIT Interpreter Peers, unsandboxed)**:
  node-jitless: pinned `node` binary (`qps-node.mjs`) with `--jitless`
  (V8 Ignition interpreter only);
  bun-jitless: pinned `bun` binary (`qps-bun.mjs`) with `BUN_JSC_useJIT=0`
  (JSC LLInt interpreter only);
  **Tier 3 (Unsandboxed JIT Ceilings, single-process reference)**:
  node: built-in `node:http` event loop with HTTP/1.1 keep-alive (`qps-node.mjs`);
  bun: built-in `Bun.serve` (`qps-bun.mjs`).
- **Memory:** process-tree PSS (`smaps_rollup`) sampled after the last
  round, decomposed per role; a **trickle phase** (fresh process, 15 s
  at 10 req/s) is measured **before** any load, because V8 growth is
  irreversible and the at-rest footprint must not be contaminated.
- **Output:** `cells.txt` lines `stack [workload] qps_med/p50/p95`,
  `serving_pss_mb[=role:mb;…]`, `lowqps_pss_mb[=…]`.

Serving response evidence uses `exact-bytes-v2`: the two raw phase rows and
correctness verdict identify the workload, exact expected body SHA-256 and byte
length, dispatched requests and fully checked responses. Both counts must equal
completed requests in each successful phase. The frozen three workloads compare
every byte; the stream is exactly 341 b, 341 c and 342 d bytes. Errors, timeouts,
wrong status and wrong/truncated/extra bytes cannot count as success. A failed
warm-up prevents measurement; a failed verdict exits nonzero. Formal samples
record a 30000 ms request timeout; the explicit shorter correctness-fixture
setting is rejected by publication validation.

The shell entry execs the owned Python controller. The full seven-stack pure-JS
producer (`capsid`, `workerd`, `deno`, `node-jitless`, `bun-jitless`, `node`, `bun`) implements these
defaults, requires managed/strict posture and keeps every serving root and Go
client in separate owned sessions. An exact application-readiness response
precedes traffic and is separate from the 150 trickle responses. Low-load
cleanup completes before a fresh loaded forest; after traffic, settle for 1 s
and require two consecutive positive recursive PSS reads within `pssToleranceKb`
(`<= 32 KiB` / `0.03 MiB` in `v3`, exact `0 KiB` in `v1`/`v2`) within twenty-four attempts at 1 s
intervals (`v2`/`v3`; eight in archived `v1`). Every process, role, client, response, raw sample, configuration and
cleanup record is retained and sealed.

Managed QPS and density readiness retain the operation ID returned by the single
Admin deploy request. Read-only polls require that exact operation and version
to reach `state: active` before the existing App-active and owned-listener checks.
The durable `active.json` document alone can precede routing publication and is
not a serving-readiness acknowledgement. This wait stays within the original
absolute startup deadline; neither deploy nor the first exact business request
is retried. Failed or malformed operation responses abort the probe.

Formal archives emit `schema: 3` (`capsid-perf-lane-v3`), requiring clean
measured-build/session metadata, four available CPU indices, all seven pure-JS
stacks, all four workloads (including `dynamic-api`), five rounds and independent
build provenance. Historical `schema: 1` (`capsid-perf-lane-v1`) and `schema: 2`
(`capsid-perf-lane-v2`) archives remain replayable without modification. The producer
self-validates its sealed output. Restricted `--observation` output has no
`lane.json`; short observations do not qualify a formal performance point. See
[QPS producer](perf-evidence.md#qps-producer).

### 2.2 Cold-start lane — `bench/cold-start.sh` + `bench/cold-start.cc`

- **Metric:** process start → first **validated** response. Warm
  rounds and the PSS walk are reported separately and must not leak
  into `total_ms` (historical bug: shell-measured stacks once read
  ~400 ms because a 50-request curl warm loop was inside the timer).
- **Fixtures:** real-shaped JS at 10k/100k/1m, byte-aligned across
  runtimes (`gen-cold-start-apps.py`); `node-jitless` reuses the exact
  `app-node-*.mjs` fixture with `--jitless`, `bun-jitless` reuses the exact
  `app-bun-*.mjs` fixture with `BUN_JSC_useJIT=0`, and `deno` uses
  `app-deno-*.mjs` with a fresh private `DENO_DIR` and `--allow-net=127.0.0.1 --no-prompt --no-config`;
  each cell drops one warm-up run and reports the median of five.
- **capsid:** C-ABI tool with `--strict` (full sandbox in the spawned
  worker); source and trusted-bytecode paths both measured. Deno / Node / Node
  (`--jitless`) / Bun / Bun (`--jitless`): stdout READY plus an owned listener, then the first
  exact HTTP response. workerd: first owned-listener TCP connect (its isolate
  initializes lazily on the first request, so the ready→first-response span
  carries that cost).

The C-ABI probe now gets the spawned child's PID from the public
`capsid_worker_pid()` API. Inherited socketpair credentials identify the socket
creator and must not select the measured process. First and warm responses
require one HTTP 200 head, the correct request ID and the exact frozen body;
any failure aborts that iteration without a success row. The default worker
posture is strict. `total_ms` stops at the first validated response, before
warm-up, settling or memory sampling. Warm latency is the median of every
successful warm request (the mean of the middle pair for an even count).

Worker-tree PSS excludes the C-ABI probe/controller itself. The outer owner
must monitor and clean that launched tree without adding the measurement
process back into the reported worker footprint.

Worker-tree membership is pinned after the first response and before warming.
Recursive PSS includes pinned reparented descendants. Missing/reaped/replaced
or additional members fail; unreadable, zero or overflowing memory fails.
After a one-second settle, the probe takes at most twenty-four PSS reads one second
apart and requires two consecutive positive readings within `pssToleranceKb` (`<= 32 KiB` in `v3`, exact `0 KiB` in `v1`/`v2`). Raw output includes round,
correctness counts, PID/start identities, both PSS samples and warm latencies.
SIGINT/SIGTERM and request failures reap the C-ABI worker through abortive
destroy. This does not qualify graceful shutdown or complete descendant cleanup;
the implemented owned lane controller must also clean its entire launched tree.

`cold-start.sh` execs the Python controller. Every cell launches a fresh process
for one discarded run and five measured rounds. All six runs require exact
first/warm responses, pinned process membership, stable positive PSS and zero
remaining owned processes. Discarded raw/correctness records are sealed and
validated separately; a failed discarded run aborts the cell. The summary uses
only measured rounds. Reusing a measured root PID/start identity fails.

Node, Node (`--jitless`), Bun, and workerd requests use a fresh Python-standard-library loopback HTTP
connection per request (`httpClient: python-http-exact-v1`). The old curl process
startup floor describes historical points only. C-ABI IPC and HTTP transports
remain different and are stated beside warm results. Both the controller/client
and SUT use SUT_CPUSET; formal runs require four available CPU indices. Calling
the producer as a library restores the caller's original affinity, even on error.

Ports are reserved by the parent, and readiness corroborates listener ownership.
Each process tree, log, runtime listener config and output archive belongs to one
controller. The common owner cleans descendants before the parent and reaps
observed orphans on failure/cancellation. C-ABI raw PSS is validated against the
outer owner's observed worker identities without including the tool's memory.

Inputs are verified against the frozen generator and snapshotted before use.
Trusted bytecode, attestation and message are freshly compiled in a private
Linux `/tmp` directory and copied into the archive; shared APPS is never written
and stale `.qjsb` files are never reused. The private native compilation directory
remains the benchmark's isolation boundary. The compiler also supports mounted
output directories via a no-replace `linkat`/`unlinkat` fallback when
RENAME_NOREPLACE is unsupported; ordinary errors remain failures. This does not
make three separate output paths atomic across a process crash or power loss;
deployment still needs an outer staging generation directory.

Formal mode requires clean measured-build/session metadata, all five stacks and
three sizes, complete sealed raw/correctness/identity evidence and self-validation
against the collector. Restricted `--observation` output has no publication
envelope. The accepted 2026-11-16 local snapshot has a complete quiet session;
there is no fresh paired v0.2.2 rebuild/requalification or 0.3.0-beta-specific point.
Controller tests and restricted smoke still do not qualify a performance point.

### 2.3 Density lane — `bench/density-lane.sh`

- **Metric:** an independently verified pool whose **whole descendant-tree
  PSS** stays under the budget (default 2048 MiB), found by doubling to the
  boundary, binary search, and bounded final verification. A configured cap
  reached without a memory boundary is explicitly a lower-bound observation,
  not a measured maximum.
- **Phases:** each cell is reported twice — **cold** (one real request
  per worker/cell; initialization, not static loading) and **warmed**
  (100 requests through each, then settle). A pool that dies under the
  warm-up is a failed probe, never a small reading.
- **Conditioning waves:** `per-instance-waves-v2` sends one real request to
  every instance in each wave: one cold wave or 100 warm waves. The recorded
  concurrency limit remains `max(64, 128 * pool_count)`; the actual wave has
  `pool_count` requests and connection setup uses at most 64 clients. Production
  least-loaded/lowest-slot scheduling is preserved. Cold retains the ceiling
  `max(64, 8 * count)`, warm retains `max(64, 32 * count * 100)`, and conditioning
  retains its 300-second deadline, including the complete dispatch barrier.
- **Capsid dispatch barrier:** pause only the measured Host's worker children,
  selected by executable and pinned PID/start identities. The Host remains
  running. Send the wave through the original exact HTTP client, require the
  service HTTP inflight gauge to reach `pool_count`, then resume every worker.
  Every resume is attempted even if another fails; any failure rejects the
  probe and the owner cleans its whole process tree. Every wave must record
  one business execution on each original worker. Before pausing the next
  Capsid wave, readonly Admin metrics must show exact service HTTP inflight
  zero: receiving a complete client response can precede Host retirement.
  Poll only that observed state within the same original 300-second coverage
  deadline, without a fixed hold, deadline restart or business-request retry.
  Seal every raw service gauge and its reading/admission/pause timestamps;
  revalidate actual zero, previous-wave ownership and the shared deadline.
  workerd uses the same wave
  size through its individual cell sockets. This changes density prewarm
  execution timing; the clients finish before settling and PSS sampling.
  QPS remains fixed at 64 connections/in-flight and the cold-start lane retains
  its own original latency protocol.
- **Conditioning evidence:** seal and revalidate launch ports/nonce, every
  response row and exact body digest, continuous per-worker UUID counters,
  READY-before-traffic ordering, every dispatch/resume wave, and actual resume
  signal records. Every PSS attempt must contain the same pinned process set,
  with positive individual readings adding up to its total. The final pair
  must match those raw attempts and cleanup must retain every measured process
  identity with zero remaining owned PIDs. A summary or resealed aggregate
  cannot substitute for these records. Earlier burst archives do not acquire
  the v2 protocol by changing their metadata.
- **Warm-phase gating:** request failures are counted and fatal (a warm
  pass that cannot reach its targets understates a pool that is quietly
  shedding); the post-settle sample requires two consecutive **equal**
  PSS readings and a positive value. Process membership is pinned after
  readiness; a missing, reaped or replaced member fails the sample instead
  of reducing its total. The recorded count is only ever one
  whose own final probe verified under budget. PSS drift gets at most two
  fresh final-probe retries; persistent drift fails the cell. A stable but
  over-budget final probe steps down at most eight counts and verifies the
  new count again. Wrong content, unreadable/nonpositive PSS, dead processes,
  incomplete coverage or cleanup failure abort. A cell is never recorded with
  a failed reading. Shared-acceptor pools require observed per-worker request
  coverage. Doubling a total request count does not prove that every worker
  reached the target. Use benchmark-only counters; public responses and the
  production protocol must not gain diagnostic fields.
- **Stacks:** formal density compares `capsid` and `workerd` only (budget
  fill-to-2048 MiB). Formal capsid points require managed/strict posture; a
  static-pool diagnostic is an observation and cannot qualify publication.
  workerd uses N cells in one process (minimal capnp,
  cells touched through per-cell sockets).
- **Measurement hygiene:** PSS walks are recursive (grandchildren count).
  A Python control layer owns reserved OS ports and PID/start-time identities;
  consecutive probes use different ports and concurrent controllers cannot
  reserve each other's sockets. Readiness verifies listener ownership as well
  as application activation. Teardown freezes the owned tree, kills descendants
  before the parent and reaps observed orphans. This is benchmark teardown,
  not evidence of graceful product shutdown. Readiness requires real application
  readiness (e.g. celld
  accepts TCP before the app exists — a lesson recorded here after a
  503 storm produced flat lines). workerd also binds sockets before building
  its services. Its `--control-fd=1` `listen` events must identify every
  configured socket and exact port before the first business request; raw OS
  LISTEN ownership alone is insufficient. The same absolute 120-second startup
  deadline applies. No HTTP request, prewarming, fixed sleep or larger request
  deadline substitutes for this control-channel readiness proof.
  Before launching workerd, readonly Linux conntrack snapshots must show room
  for all original per-cell requests plus one wave: count * (target + 1), where
  target is 1 cold or 100 warm. Existing tracked connections from earlier probes
  may expire naturally; the controller does not tune sysctls, flush tables,
  change connection reuse, prewarm, retry business requests or reduce coverage.
  This state-based wait and service initialization share the same original
  120-second absolute startup deadline, starting before fixture preparation;
  the service-ready phase cannot restart that budget. Missing/malformed counters,
  insufficient native capacity or deadline expiry fail closed. Sealed raw
  network-headroom readings and startup/launch/readiness timestamps are
  independently rechecked, including actual admission and the shared deadline.
  Runtime files with legal Linux generation names such as sha256:<64 hex>
  cannot be direct portable-archive paths. Keep their exact original names and
  bytes in runtime-nonportable.tar, with a sealed runtime-nonportable.json member
  index (name, length and SHA-256). Portable runtime fixtures and barrier logs
  retain their direct paths. Validation streams every indexed regular TAR member,
  rejects traversal/drive/ADS names, links, duplicates, omitted members and byte
  mismatches, and never extracts or renames these members on Windows.
  Both observation and formal producers apply the unchanged portable-path rule
  during sealing, so the existing short prerequisite run catches a bad archive
  before a complete density search. Repackaging an already failed measurement
  cannot retroactively qualify it; a corrected source needs a fresh full session.



`density-lane.sh` now execs `density-lane.py`, using injectable control and
process/PSS helpers. Formal mode requires clean measured-build/session metadata
and the complete protocol. `--observation` is explicit and never writes a
publication envelope. Counter instrumentation is confined to generated benchmark
fixtures and their logs; public response bytes/headers and the production
protocol are unchanged. Counters record actual per-worker request totals, with
at least one cold or 100 warm requests each; a shared total is insufficient.

The density producer has small-pool managed/strict/reference smoke and
synthetic complete-archive controls, separate from the accepted real 2026-11-16
fill-to-budget/three-lane record. The latter belongs only to snapshot 485b29b.
Do not promote the smoke or synthetic fixtures into performance conclusions.

## 3. Data pipeline

The publication contract is now specified in
[perf-evidence.md](perf-evidence.md). It supersedes the permissive collection
rules below: all supplied lanes need complete sealed `lane.json` records,
raw samples, correctness and isolation records. The CLI has no legacy or
parser-only publication bypass. All three lanes now have producers for this
contract. The 485b29b record passed independent build/source/binary and
complete-session review. It is not 0.3.0-beta qualification or a newly run benchmark.
Earlier missing-contract points remain unverified historical observations.

- `bench/collect-perf-point.py --cold-start D1 --qps D2 [--density D3]`
  parses the three `cells.txt` formats, validates, and merges the point
  into `perf-site/data.js` (same-version points are replaced).
- **Validations (fail-closed):** duplicates or missing cells in any lane,
  malformed/non-finite values, incomplete raw/correctness records, digest
  mismatches, unknown posture and conflicting machine/source/binary identities
  abort. Density participates in every provenance check. Summary values must
  reconstruct from their sealed raw records.
- **density keying:** `density[stack][phase@size] = {count, unit,
  pssMb}`; the site renders cold and warmed rows per stack and falls
  back to footprint-arithmetic `est` values only for unmeasured sizes.
- **Provenance:** `git` commit/tag identify the measured binaries; collector
  identity and explicit generation time are separate. The site
  shows `release · commit <sha8>` unless a tag verifies the release.
- Do not hand-edit `data.js`; regenerate it.
- `prepare-perf-site.py` revalidates qualified points and publishes actual
  evidence. Directory-backed records retain direct files; complete large records
  may use hashed bounded compressed transports under the original namespace.
  Restore, raw/point replay, every checksum and published transport bytes must
  all pass. Compression cannot grant acceptance or repair a failed record.
  Missing-contract historical points remain unverified, with no invented links.

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
- Deployment: GitHub Actions, pinned action SHAs, artifact upload + deploy-pages;
  the retained `dev/v0.3.0` push branch and the actual site/data/tool paths in
  `.github/workflows/perf-pages.yml` trigger validation and publication.

## 6. Release procedure

1. Build a clean Release tree (the build identity gate requires
   a clean source identity). Prepare a separate clean checkout; do not reset,
   stash or overwrite someone else's work merely to satisfy this gate.
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
- Historical celld diagnostic only (not a current accepted comparison): serving
  and cold lanes produced
  verdict-clean data (json-1k 48,895 QPS; stream 31,769; cold ~9 ms),
  the density comparison was not completed and no celld rows ship.
