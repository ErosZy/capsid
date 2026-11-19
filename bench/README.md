# Benchmark Tools, Records and Publication

Updated 2026-11-18. All three producers are implemented. The latest complete
accepted local session is `local-wsl-v030-headers-ready-20261115T235007Z-7368b4ec`,
measured snapshot `485b29b`, not the published 0.3.0-beta binary. See
[current status](../docs/current-status.md) and
[current performance data](../docs/performance-benchmarks.md).

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
| `qps-lane.sh` / `qps-lane.py` / `qps_probe.py` / `qps_inputs.py` | Pure-JS four-stack (`capsid`, `node`, `node-jitless`, `workerd`) managed/strict serving producer, fresh trickle then loaded forests, raw rounds and recursive PSS |
| `density-lane.sh` / `density-lane.py` / `density_probe.py` | Managed/strict cold/warm pool search (`capsid`, `workerd`), per-instance coverage, PSS, original budgets, explicit cap/budget result |
| `density_conditioning.py` / `density_evidence.py` | Owned dispatch/resume barriers and independent replay of responses, memberships, counters, cleanup and original request ceilings |
| `collect-perf-point.py` / `perf_evidence.py` | Complete sealed-lane/point validation (`capsid-perf-lane-v2`, plus historical `capsid-perf-lane-v1` replay) and deterministic metric derivation |
| `perf_archive.py` | Bounded hashed transport restoration; never grants acceptance or repairs a record |
| `prepare-perf-site.py` | Revalidate complete original evidence and stage charts plus real downloadable archives |
| `bench-apps/` | Pure-JS QPS applications (`qps-worker.mjs` for Capsid/workerd and `qps-node.mjs` for Node/Node `--jitless`) |

The public controllers expose `--help`; formal mode requires actual clean
build/session metadata and the full declared protocol. `--observation` is
explicitly restricted and cannot generate a qualifying publication envelope.
There is no instruction here to hand-author a quiet-session or binary identity
for an old/dirty build.

## Source and Platform Prerequisites

Use the [source-build guide](../docs/building.md) and exact pinned Node and
workerd inputs (`node-jitless` runs the same pinned `node` binary with
`--jitless`). Actual process/PSS/Go/protocol
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

The current site includes the original three-lane record in compressed,
hash-indexed transports rather than a 2.74 GB raw Git/Pages payload. Restoring
those 5123 files and replaying the existing validators is distinct from running
a SUT again. See [site replay instructions](../perf-site/README.md).

```sh
python3 tests/test_perf_archive.py
python3 bench/prepare-perf-site.py --repo . --output /path/to/new-site-output
```

At least 8 GiB of scratch space is recommended. Complete archives retain raw
samples, failure/search probes, discarded cold runs, exact programs, process and
response evidence, dependency identities and checksums. No part is redacted,
normalized or resealed to turn a failed run into success.

The schema-1 collector has one active point per version and historically
replaces an existing version. This is **not permission to rewrite published
history**: this update adds the first 0.3.0 record and preserves the earlier
0.2.x point exactly. Future remeasurement of an already accepted version needs
a reviewed separate series/schema/archive plan that retains its old result.
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
