# Benchmark Tools, Records and Publication

All three benchmark producers are implemented. The [performance charts](https://rowanellis-113.github.io/capsid/)
display source-bound observations, not benchmarks of the published prerelease.
See [current performance data](../docs/performance-benchmarks.md) for the latest
accepted session and the preserved earlier measurements.

The normative [SPEC.md](SPEC.md) defines the protocol; [perf-evidence.md](perf-evidence.md)
defines the sealed record/acceptance contract. Scripts, declarations and test
fixtures are not measurements by themselves. Immutable evidence is preserved;
availability/removal at older capture dates remains documented in the history
rather than being described as still present everywhere.

## Active Tools

| Path | Responsibility |
| --- | --- |
| `loadgen/`, `build.sh` | Frozen closed-loop client; exact response bytes, fixed inflight, warm-up and measured phases |
| `cold-start.sh` / `cold-start.py` / `cold_probe.py` / `cold-start.cc` | Owned strict C-ABI source/bytecode and independent runtime cold probes, discarded + five measured runs, 50 warm requests |
| `qps-lane.sh` / `qps-lane.py` / `qps_probe.py` / `qps_inputs.py` | Pure-JS seven-stack (`capsid`, `workerd`, `deno`, `node-jitless`, `bun-jitless`, `node`, `bun`) × four-workload (`matrix-json-1k`, `matrix-json-16k`, `stream`, `dynamic-api`) managed/strict serving producer, fresh trickle then loaded forests, raw rounds and recursive PSS |
| `density-lane.sh` / `density-lane.py` / `density_probe.py` | Managed/strict cold/warm pool search (`capsid`, `workerd`), per-instance coverage, PSS, original budgets, explicit cap/budget result |
| `density_conditioning.py` / `density_evidence.py` | Owned dispatch/resume barriers and independent replay of responses, memberships, counters, cleanup and original request ceilings |
| `collect-perf-point.py` / `perf_evidence.py` | Complete sealed-lane/point validation (`capsid-perf-lane-v3`, plus historical `capsid-perf-lane-v2` and `capsid-perf-lane-v1` replay) and deterministic metric derivation |
| `perf_archive.py` | Bounded hashed transport restoration; never grants acceptance or repairs a record |
| `prepare-perf-site.py` | Revalidate complete original evidence and stage charts plus real downloadable archives |
| `bench-apps/` | Pure-JS QPS applications (`qps-worker.mjs` for Capsid/workerd, `qps-deno.mjs` for Deno, `qps-node.mjs` for Node/Node `--jitless`, and `qps-bun.mjs` for Bun/Bun `--jitless`) |

The public controllers expose `--help`; formal mode requires actual clean
build/session metadata and the full declared protocol. `--observation` is
explicitly restricted and cannot generate a qualifying publication envelope.
There is no instruction here to hand-author a quiet-session or binary identity
for an old/dirty build.

## Source and Platform Prerequisites

Use the [source-build guide](../docs/building.md) and exact pinned Node, Bun,
Deno and workerd inputs (`node-jitless` runs the same pinned `node` binary with
`--jitless`; `bun-jitless` runs the same pinned `bun` binary with
`BUN_JSC_useJIT=0`; `deno` runs with `--allow-net=127.0.0.1 --no-prompt --no-config`). Actual process/PSS/Go/protocol
measurement requires the Linux posture and CPU/resource prerequisites in the
spec. Windows can run portable models/transport controls, not substitute for
native Linux measurement or native Windows product qualification.

With the corresponding prepared Linux testing build:

```sh
cmake --build BUILD --target benchmark-loadgen
ctest --test-dir BUILD --no-tests=error --output-on-failure \
  -R '^benchmark_(loadgen_correctness|qps_control|qps_producer|qps_probe)$'

cmake --build BUILD --target benchmark-cold-start test-benchmark-cold-measurement
ctest --test-dir BUILD --no-tests=error --output-on-failure \
  -R '^benchmark_cold_(measurement|tool)$'
```

These test selections validate tools/fixtures, not a full benchmark session.
The build-tree clients are `BUILD/benchmark-tools/loadgen` and `BUILD/cold-start`;
existing `bench/bin/*` programs are not silently rebuilt or selected in their
place. A missing target/prerequisite or empty selection is a failure, not a
reason to omit a required gate.

## Existing Accepted Data and Replay

The site preserves complete three-lane records in compressed, hash-indexed
transports. Restoring the original files and replaying the existing validators
does not run the SUT again. See [site replay instructions](../perf-site/README.md).

```sh
python3 tests/test_perf_archive.py
python3 bench/prepare-perf-site.py --repo . --output /path/to/new-site-output
```

At least 8 GiB of scratch space is recommended. Complete archives retain raw
samples, failure/search probes, discarded cold runs, exact programs, process and
response evidence, dependency identities and checksums. No part is redacted,
normalized or resealed to turn a failed run into success.

The schema-1 collector has one point per version and would replace an
existing version. This is **not permission to rewrite published history**.
Append a newly measured version only after verifying earlier points remain
unchanged. Remeasuring an already accepted version requires a separate reviewed
series/schema/archive plan that retains the old result.
An archival release label with no source tag must be displayed as a development
snapshot, never as a measured stable/prerelease artifact.

## Full-Session Requirements

Keep source/build/binary/tool/fixture identities, strict posture, original CPU
set, four quiet windows, exact responses, complete round inventories, recursive
owned-tree PSS and final cleanup together. Do not combine separate sessions,
count errors as successful work, increase budgets, retry a failed lane until
green, or promote diagnostic ABBA samples into a whole-session result. Physical
host exclusivity is not inferred from quiet guest readings.

## A/B Tool Controls

`run-ab.sh`, `evidence.py`, `fake/`, `wrappers/` and `baseline-gateway/` provide
the older A/B evidence harness exercised by
`tests/test_host_single_worker_ab_evidence.mjs`. Fake-component controls validate
the harness and failure detection; they are not product speed measurements.
A genuine missing perf prerequisite stays an explicit skip, not a performance
conclusion.
