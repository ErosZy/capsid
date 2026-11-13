# Performance archive contract

`collect-perf-point.py` publishes only complete archives under
`bench/results/`. Its parser helpers are separate from publication. A
`cells.txt` fixture, a historical digest list, or a successful parser test
does not qualify a measurement point.

The current validator and site staging path implement the T16 collection
boundary. All three controllers now produce this contract; **independent build
and actual complete-session qualification remain pending.**
Do not create missing records retrospectively or
mark the 0.3.0 performance work complete based on synthetic contract tests.

## Lane envelope

Each lane writes `lane.json`, `manifest.txt`, `cells.txt`, raw JSONL samples,
exact-response correctness JSON, fixture bytes and an isolation/cleanup
record during its run. Seal every regular file except `sha256sums.txt` with
one lowercase SHA-256 entry, using POSIX paths relative to the lane archive.
Duplicate entries, absolute paths, parent traversal, symlinks, missing files,
unlisted late files and digest mismatches are errors. Old absolute digest
labels are retained as historical evidence; they do not meet this contract.

`lane.json` has these required fields:

| Field | Meaning |
|---|---|
| `schema`, `lane`, `status` | Integer `1`; `cold-start`, `qps` or `density`; `complete` |
| `session` | Shared measurement session identity, recorded by every lane |
| `startedAt`, `completedAt` | Measurement interval with timezone; intervals must not overlap |
| `origin` | Measured capsid `commit`, `tag`, `version`, `release`, `clean: true` |
| `machine` | `cpu`, `kernel`, `architecture`, positive integer `logicalCpus`, stable `hostId` |
| `command` | Exact command string, including the selected environment parameters |
| `posture` | Linux/strict; `managed` for QPS/density, `c-abi` for cold start |
| `protocol` | Frozen lane parameters below, plus explicit `sutCpuSet` |
| `summary`, `isolation` | `cells.txt`; path to the sealed isolation/cleanup JSON |
| `binaries` | Named measured binary identities, described below |
| `fixtures` | Named `{file, sha256}` entries with archived fixture bytes |
| `records` | One record for every required summary cell |
| `memory` | QPS only: path to the sealed memory phase records |
| `runtimeDependencies` | QPS only: sealed Python package/version/code inventory matching the selected interpreter |

`origin.commit` describes the measured build. The collector's checkout and
generation time are stored separately in `generation`, together with hashes
of the collector implementation. A nonempty measured tag must match the
requested release. The capsid binaries, cold tool and load generator must
match the measured source identity. Other binaries require a version plus
source commit or tag and SHA-256. An optional binary `file` is rehashed when
the executable bytes are archived. Otherwise the hash is a **recorded binary
identity**, not a claim that the collector independently rebuilt that binary.

The isolation JSON binds `session`, `origin`, `machine`, `posture`,
`quietSession: true`, and integer `ownedPidsRemaining: 0`. The validator checks
these sealed records for agreement; the measurement controller remains
responsible for observing the actual processes and isolation state.

`manifest.txt` is the readable counterpart. Its `commit`, `tag`, `cpu`,
`uname`, `nproc`, `generated_at`, `command`, `binary.<name>.sha256` and
`fixture.<name>.sha256` fields must agree with `lane.json`. Lane-specific
commands and times remain separate. CPU/kernel, session, source, shared
binary and shared fixture identities must agree across all supplied lanes,
including density. There is no mixed-origin publication mode.

## Protocol and raw records

| Lane | Required coverage | Frozen parameters |
|---|---|---|
| Cold start | capsid source/bytecode, Node, Deno, workerd × 10k/100k/1m | five measured rounds after one discard; 50 warm requests |
| QPS | capsid, workerd, FastAPI, Flask, PHP × JSON 1k/16k/stream | 64 connections/inflight; five rounds of 5 s warmup + 15 s measurement; fresh 10 req/s trickle for 15 s |
| Density | capsid/workerd × 10k/100k/1m × cold/warm | 2048 MiB; 100 requests per worker/cell in warm phase; two stable reads; two verification retries |

Density uses `warmupConcurrencyPolicy: pool-scaled-burst-v1`, base
`parallelRequests: 64` and `warmupConcurrencyFactor: 128`. Each cell's conditioning
limit is `max(64, 128 * count)`; its correctness file records that limit and the
largest actual wave. All conditioning completes before settling and PSS reads.
Cold/warm attempts are bounded by `max(64, 8 * count)` and
`max(64, 32 * count * 100)` respectively, with a 300-second deadline.
Correctness files bind `maximumRequestsAllowed` and `coverageTimeoutSeconds`
to those declared parameters; every response and per-worker target is checked.
This changes density conditioning only; QPS retains 64 connections/in-flight.
Archives using the earlier density protocol remain historical observations;
the collector does not fill the new policy or wave claims into old records.

Each manifest record identifies `stack`, `size` or `workload`, and density
`phase`. It lists `samples`, `correctness`, `binaries`, and `fixtures`.
Every required cell must occur exactly once. Sample and correctness files
cannot be reused across cells. Binary roles are checked for each stack.

QPS manifests require `responseValidation: "exact-bytes-v2"` and
`requestTimeoutMilliseconds: 30000`. Both raw phases and their verdict require
`workload`, `response_validation`, `payload_bytes`, `expected_body_sha256`,
`requests_attempted` and `responses_checked`; each raw row also records
`request_timeout_ms`. The validator independently reconstructs the three frozen
payloads, rejects missing/swapped claims and requires all attempts/responses to
match completed requests with zero errors/timeouts/mismatches. Legacy archives
are not filled in retrospectively. The QPS controller now binds each round to
its actual Go PID/start identity and sealed sample/verdict files, and the
validator requires those identities to match the loaded phase.

- **QPS:** five raw JSONL files, each containing `warmup` then `measured`
  records in the load generator's format. Stack, round, connections,
  inflight, duration, completed requests, zero errors/timeouts and ordered
  p50/p95/p99 must agree. QPS is replayed from completed/duration; summary
  QPS is the median of five. Summary p50/p95 use the explicitly recorded
  `latencyRound: "last"`, matching the producer output. They must not be
  described as medians across rounds. Duration tolerance is 10% for process
  scheduling overhead, never a shortened measurement window.
- **Cold start:** one JSONL file with rounds 1–5, each carrying `ok: true`,
  `warm_n: 50`, `ready_ms`, `total_ms`, `warm_ms`, positive `pss_kb`, and two
  equal `pss_readings_kb`. The summary is replayed from the five rounds;
  total must include readiness and end at the first validated response.
- **Density:** one final verification JSONL record per cell, with explicit
  stack/size/phase, `status: "ok"`, `finalVerification: true`, count/unit,
  two equal positive `pss_readings_kb`, and `requestsPerWorker` keyed by
  worker/cell identity. Every worker must reach one cold or 100 warm
  requests. `limit: "cap"` is distinct from `"budget"`; a capped observation
  cannot establish the largest possible pool. Keep search probes, including
  failures, as additional sealed files. A failed final probe cannot be a cell.
  `protocol.caps` explicitly records positive caps for `capsid` and `workerd`;
  a cap-reached count must equal its configured cap. The generated density
  cell carries this limit status into the page.
- **Correctness:** sealed JSON has `ok: true`, `errors_as_success: false`,
  positive `responses_checked`, zero `mismatches`, `errors`, and `timeouts`.
  Counts must agree with QPS completed requests, cold/warm coverage, or
  density per-worker coverage. Record the actual response validation;
  do not infer correctness from an HTTP status or a summary.
- **QPS memory:** `memory.json` keys every stack, then `lowLoad` and
  `serving`, with `method: "recursive-process-tree-pss"`, pinned roots/members,
  per-process positive PSS samples, role sums, readiness and cleanup evidence.
  Protocol fields require `memoryProtocol: "fresh-owned-forest-v1"`,
  `readinessProtocol: "exact-http-before-traffic-v1"`, two stable reads, at most
  eight reads, 1 s intervals and 1 s settle. Each phase references its sealed
  `resultFile`, whose bytes must reproduce the memory record. Low load has
  `beforeMeasuredLoad: true`, all 150 individually verified JSON-1k requests,
  paced dispatch/completion times, and `duration_s >= 15`. Its exact readiness
  response is separate. Loaded memory excludes the probe and all 15 per-round
  Go clients; those client identities must match the workload records. All ten
  intervals are disjoint, use one controller and fresh service identities, and
  completely reap their owned forests. Totals/roles must reproduce the summary.
  These are observations in the sampling window. Loaded QPS differs across
  stacks; it does not establish equal work performed or a steady-state bound.

All numbers must be finite, correctly typed and within their allowed range.
Failed/malformed rows, `pss_mb=fail`, duplicates and missing rows abort
collection. The validator applies only documented decimal-rounding tolerance
when comparing the shell summary with raw values.

## Collection and site staging

After the updated controllers produce complete archives:

```sh
python3 -B bench/collect-perf-point.py \
  --cold-start bench/results/COLD --qps bench/results/QPS \
  --density bench/results/DENSITY --version 0.3.0 --release v0.3.0 \
  --generated-at 2026-10-20T12:00:00Z --check
```

Remove `--check` to atomically update `perf-site/data.js`. An explicit
generation time makes unchanged inputs reproducible. A same-version point
is replaced; other versions retain their data. Invalid history is an error,
never an empty history fallback. Omitted density is permitted; supplied
density must pass every check and contributes all of its provenance/digests.

`prepare-perf-site.py --output NEW_DIRECTORY` replays every verified point
from its actual archives, rejects edited numbers or changed files, and copies
the exact evidence bytes under `evidence/bench/results/...`. The page links
to those local files. The Pages workflow stages this artifact before upload;
running the staging tool locally does not publish anything.

Historical points without this contract remain **unverified observations**.
The page labels them explicitly and does not turn their old digest aliases
into archive links. No historical raw files or missing provenance are
rewritten. Tests use synthetic complete archives only in temporary
directories; those values never enter the checked-in performance history.

Actual same-session measurements, independent build qualification and the T17
full release matrix remain required. Archive
validation alone is not 0.3.0 release qualification.

## QPS producer

`qps-lane.sh` execs `qps-lane.py`. Its default is the complete five-stack,
three-workload protocol above. Capsid always uses managed/strict with a fixed
one-worker pool. The maintained `qps-worker.mjs` is shared by Capsid and locked
workerd; Python and PHP use the maintained reference fixtures. Parent port
leases, per-phase native temporary paths, private Gunicorn/nginx/FPM control
files and pinned recursive ownership keep unrelated jobs out of measurements
and cleanup. A single subreaper lease covers multiple roots and transient
clients; each tree retains its own private session. Only serving roots enter
PSS. Cleanup continues across roots after an error and cannot claim success
until all owned members have been reaped.

Each stack first runs a fresh trickle forest, completely closes it, then starts
a different loaded forest. Owned listeners and the single-worker topology must
be ready before one exact JSON-1k response is checked. Wrong status/body fails;
there is no retry that hides a wrong response. The readiness response does not
count toward trickle. Trickle uses monotonic 10 Hz scheduling, flushes every
response record and fails if dispatch falls more than 0.1 s behind. Every
measured round starts the real Go load generator in a separate owned session;
both raw rows and its successful verdict are checked before the next round.

Runner bytes, fixtures, PHP vendor/lock, selected executables and installed
Python package versions/code are snapshotted before probes and rechecked at
the end. Package code is archived by digest; the interpreter path preserves
its venv prefix. Isolated imports and private cache/control paths prevent
shared bytecode/configuration from changing a run. Runtime files are retained
with each path component percent-encoded, including managed filenames that
contain a colon. The mapping is recorded in `runtime-files.json`.

For an actual short Linux observation, with a private Python environment that
contains FastAPI, Uvicorn, Flask and Gunicorn, and a PHP fixture whose vendor
directory and composer.lock already exist:

```sh
bash bench/qps-lane.sh --observation --out /tmp/qps-observation \
  --host /path/capsid-host --worker /path/capsid-worker \
  --workerd /path/workerd --loadgen /path/BUILD/benchmark-tools/loadgen \
  --python /path/venv/bin/python --php-fpm /path/php-fpm8.3 --nginx /path/nginx \
  --rounds 1 --warmup 1 --duration 1 --connections 2 --inflight 2 \
  --trickle-seconds 1 --trickle-rate 10
```

OUT must be new. This writes sealed `observation.json`, `memory.json`, verified
`cells.txt`, per-phase results, raw client files and dependencies; it has no
publication `lane.json`. Errors and SIGINT/SIGTERM preserve `failed.json`,
phase logs and cleanup records and cannot publish partial cells as success.

Formal mode uses the frozen defaults, all five stacks/three workloads, four
available CPU indices, `--metadata`/`CAPSID_BENCH_METADATA`, and an OUT below
`bench/results/`. Metadata follows the density schema below and requires actual
identities for `capsid-host`, `capsid-worker`, `loadgen`, `workerd`, `python`,
`php-fpm` and `nginx`; product/loadgen identity must match the clean measured
source. The coordination lock is acquired before dependency preparation and
held through self-validation. It does not prove an otherwise quiet machine.
The formal producer seals and replays the real archive validator before
success. Synthetic 15-cell/75-client tests use private temporary repositories
and explicit injected probes only; actual five-stack short observations and
signal tests are recorded separately. Neither substitutes for the clean
Release/LTO/ALL, full matrix or quiet-session formal performance qualification.

## Density producer

`density-lane.sh` execs the Python controller without command-substitution
ownership. It always launches capsid in managed/strict mode. Its workerd config,
fixture wrapper, requests, counters, PSS samples and cleanup records live in
unique probe directories; generated files never overwrite the shared APPS.
Every response must match the frozen generator's exact body. Benchmark-only
request nonces distinguish these requests from managed health activity, and
worker-local counters are sent through ordinary logs. No diagnostic data is
added to public responses or production IPC. Dropped/malformed counter records
fail coverage.

The current `per-instance-waves-v2` protocol uses one request per worker/cell
in every cold/warm conditioning wave. Capsid briefly pauses only its pinned
worker children until the running Host has dispatched the complete wave, then
resumes all of them before reading responses or sampling memory. workerd uses
the same wave size through its per-cell sockets. The 300-second deadline,
original request ceilings and fixed QPS protocol remain in force.

The archive retains every dispatch/resume wave and signal record. Validation
reparses exact response digests, request ordering, UUID/nonce business counters,
readiness ordering, the pinned process membership and each individual PSS sum.
Resealing altered summaries, incomplete waves or missing resumes does not make
them acceptable. Synthetic validator fixtures and restricted observations
remain separate from formal performance measurements.

For a restricted Linux smoke using already generated fixtures:

```sh
OUT=/tmp/density-observation HOST_BIN=/path/capsid-host \
WORKER=/path/capsid-worker WORKERD_BIN=/path/workerd \
APPS=/path/generated-apps bash bench/density-lane.sh \
  --observation --sizes 10k --max-workers 2 --max-cells 2
```

OUT must be new. The output is `observation.json`, typed `probes.jsonl`,
verified `cells.txt` and a complete digest inventory. It deliberately has no
publication `lane.json`. A measurement failure or cancellation records
`failed.json`; that archive cannot qualify publication. Creating OUT is atomic; a controller that loses
that creation race cannot write failure records into the winner's directory.

Formal mode requires `--metadata /path/run-metadata.json` (or
`CAPSID_BENCH_METADATA`) and an OUT under the selected repository's
`bench/results/`. Required metadata fields are:

- integer `schema: 1`, `session`, `quietSession: true`;
- `sourceRoot`, the actual clean measured-source checkout;
- `origin` with actual `commit`, `tag`, `version`, `release`, `clean: true`;
- `machine`, matching `perf_run.machine_identity()` on the measurement host;
- `binaries`, with commit/tag/version/SHA-256 records for `capsid-host`,
  `capsid-worker`, and `workerd`, derived from the actual build artifacts.

The producer verifies the clean source commit/version/tag and measured binary
bytes before starting. It must not be given a manually invented identity for
an old or dirty binary. The release measurement controller remains responsible
for the build-to-binary provenance and actual quiet-machine prerequisites.
A kernel-backed session lock rejects overlapping coordinated formal lanes;
it does not prove the absence of unrelated machine activity.

Before success, executable bytes and runner/launcher bytes are checked again.
Runner scripts and the fixture generator are archived, with Python/taskset
identities separate from measured product identities. Formal output includes
the isolation record, human/structured manifests, final raw/correctness files
and every search artifact. The producer invokes the real density archive
validator on its own sealed output.

Generated inputs must exactly match the frozen fixture generator. Their
verified bytes are copied into the owned archive before any probe; every probe
uses that snapshot. Changes to shared APPS after that point cannot alter the
active lane, and the private fixture hashes are checked again before success.

The process helper holds an unreaped root identity, observes recursive
descendants, and uses a Linux
[child subreaper](https://man7.org/linux/man-pages/man2/PR_SET_CHILD_SUBREAPER.2const.html)
to reap its orphaned descendants. Cleanup signals only matching PID/start-time
identities, in descendant-before-parent order. Membership is pinned after
readiness and checked through warm-up and both PSS samples; an exited/reaped
worker cannot silently disappear from the total. Interrupted initialization is
also responsible for already created descendants. Unit tests exercise real
three-generation processes, unrelated children/listeners, parent exit and
SIGINT/SIGTERM; portable tests cover ports, parsing and search failures.

Formal producer tests inject synthetic probes only in a temporary repository.
They execute the real metadata checks, search, archive writer and validator,
and never create checked-in performance data. Actual small-pool smoke remains
separate from the still-pending 2 GiB density and three-lane measurements.

## Cold producer

`cold-start.cc` now emits complete per-iteration response verdicts, round IDs,
warm counts/latencies, actual public worker PID/start identities and two equal
positive recursive PSS samples. It rejects wrong status/content, request identity
or ordering, incomplete warm phases, deadlines and changed/missing process members.
Timing ends before warm-up and PSS; strict worker posture is mandatory.

Portable C++ model tests exercise process/response/memory boundaries. Real Linux
tests run valid and deliberately wrong apps through the actual C ABI and check
that the tool itself reaps its worker before any outer test cleanup. A separate
source/trusted-bytecode smoke covers all three generated fixture sizes. These
are observations, not five-round lane archives or workerd comparisons.

`cold-start.sh` now execs the Python producer. The common outer process owner
cleans its complete observed tree, including descendants, on completion, failure
or interruption. For C-ABI probes the measured tree starts at the public worker
PID; the outer C-ABI tool is owned for cleanup and excluded from PSS. HTTP stacks
measure their serving process trees. The CPU selection pins both client and SUT
and restores the caller's affinity on every exit.

Each cell has `samples`/`correctness` for five measured rounds and mandatory
`discardedSamples`/`discardedCorrectness` for its independently launched round 0.
All first and 50 warm responses are validated, including the discarded run.
Raw records include warm latencies, pinned members, PID/start identities and both
recursive PSS samples. A declared total cannot replace those records; measured
root identities must be distinct across all six runs. A `failed.json` marker
always prevents publication, even if a stale complete envelope remains.

The frozen protocol now records `httpClient: python-http-exact-v1`. HTTP clients
use fresh loopback connections without a curl subprocess. IPC and HTTP timings
still have different transport costs; the site renders that actual protocol
and retains the old curl-floor explanation for historical points.

All generated apps are verified and snapshotted. Port-specific listener copies
and configs are private to a probe. Bytecode and its two companion outputs are
freshly compiled on private Linux native temporary storage, then archived and
hashed. The compiler's no-replace contract is preserved on WSL mounts that reject
that filesystem operation. No lane writes shared APPS or reuses old bytecode.

Formal cold metadata follows the density schema above but requires binary
records for `capsid-worker`, `cold-start`, `capsid-bytecode-compile`, `node`,
`deno` and `workerd`. The first three must match the measured clean source
commit/tag/version. Formal mode requires all five stacks, all three sizes,
five rounds, one discarded run, 50 warm requests and four available CPU indices.
Source/binary/runner/input identities are rechecked before sealing. The actual
sealed archive is replayed through the real cold validator before success.

For a restricted smoke, provide actual binary paths and frozen generated apps:

```sh
OUT=/tmp/cold-observation APPS=/path/generated-apps \
WORKER=/path/capsid-worker COLD=/path/cold-start \
COMPILE=/path/capsid-bytecode-compile NODE_BIN=/path/node \
DENO_BIN=/path/deno WORKERD_BIN=/path/workerd \
bash bench/cold-start.sh --observation --sizes 10k --rounds 1 --warm 2
```

OUT must be new. Observation mode writes no `lane.json`; formal mode additionally
requires metadata and an OUT under `bench/results/`. Missing stacks or metadata
fail before any SUT launch. Synthetic full-archive tests stay in temporary
repositories and do not qualify an actual performance point. Same-session T17
measurements and fill-to-budget density remain outstanding.
