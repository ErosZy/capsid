# Testing Guide

Use this page for the current validation workflow. The older
[testing journal](testing.md) retains dated implementation rounds, captured
results and legacy commands; its old counts, compiler restrictions and pending
tasks are not current instructions.

A successful configure, build, test listing, selected smoke test or archive
checksum is not full qualification. Report the actual scope and preserve raw
failures and prerequisite skips.

## Choose the Right Check

| Change | Start with | What it does not establish |
| --- | --- | --- |
| Documentation | Documentation/link/archive audit; execute changed examples where possible | Runtime or release qualification |
| Runtime or Host behavior | Relevant CTests, causal regression and adjacent lifetime/policy tests | Unselected configurations or platforms |
| Standards/framework behavior | Pinned references, actual runtime cases and negative controls | Full browser or ECMA-429 conformance |
| Sanitizer changes | Dedicated instrumented build with matching provider coverage | Production RSS or throughput |
| Release | Full required CI, actual platform packages, source-bound review and asset verification | Closure of every historical issue |
| Performance | The complete declared benchmark protocol and session | Acceptance from CI timing or sampled diagnostics alone |

## Documentation Checks

From the repository root, using the project's Node runtime:

```sh
node tests/audit-current-docs.mjs .
node --test tests/test_documentation_archive.mjs
node tests/audit-testing-validity-workflow.mjs .
python3 tools/refresh-documentation-data.py --check
```

The [coverage inventory](documentation-coverage.json) distinguishes current
references, dated plans and exact archives. The data check binds source locks,
release metadata and the accepted measured source; it does not qualify a new
binary. Site/transport changes additionally require the positive/negative
controls and full original-byte replay in [site instructions](../perf-site/README.md).

These check local links, index reachability, security guidance, archived
qualification hashes and workflow contracts. They do not execute every command
block. Do not weaken an audit merely to accommodate stale prose; correct the
facts and keep equivalent security, provenance and archive protections.

## Local Development Tests

Prepare the [build dependencies](building.md), including all locked framework
packages. Use a separate Debug build:

```sh
cmake -S . -B build-dev -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug -DCAPSID_ENABLE_LTO=OFF \
  -DCAPSID_BUILD_HOST=ON -DBUILD_TESTING=ON
cmake --build build-dev --parallel 2
ctest --test-dir build-dev --show-only=json-v1
```

The last command lists registrations; it does not run them. Run relevant tests
with an explicit empty-selection failure:

```sh
ctest --test-dir build-dev --no-tests=error --output-on-failure \
  -R '^(worker_memory_metrics_reclaimed|worker_memory_metrics_retained)$'
```

A broad local development run without an installed WPT checkout can explicitly
exclude only the missing-WPT sentinel:

```sh
ctest --test-dir build-dev --no-tests=error --output-on-failure \
  -E '^wpt_conformance_not_configured$'
```

**This intentionally omits WPT and is not full CI acceptance.** Other required
reference/client inputs can still fail their own checks. Do not add exclusions,
repeat failed tests until green, increase budgets or reinterpret skips as passes.
Windows commands must run natively with the prerequisites in [Windows](windows.md).

## Pinned Web Platform Tests

Obtain the repository and exact revision declared in
[`tests/wpt/manifest.json`](../tests/wpt/manifest.json). Configure its absolute
checkout path:

```sh
cmake -S . -B build-wpt -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug -DCAPSID_ENABLE_LTO=OFF \
  -DCAPSID_BUILD_HOST=ON -DBUILD_TESTING=ON \
  -DCAPSID_WPT_ROOT=/absolute/path/to/pinned/wpt
cmake --build build-wpt --parallel 2
ctest --test-dir build-wpt --no-tests=error --output-on-failure
```

The declared file inventory and source hashes matter, not just the number of
registered tests. The raw ordinary suite may retain known failures. Current
release CI separately checks the pinned workerd compatibility profile, retaining
raw results rather than turning ordinary failures into standards passes.

The [conformance reference](conformance.md) defines the profile; the
[beta qualification record](v0.3.0-current-qualification.md) records the actual
accepted source and known five raw WPT file failures. Do not substitute a green
compatibility result for a claim that every upstream assertion passed.

## WebSocket and Independent Clients

The reference gate uses locked workerd, Node and Python/client identities.
For a Linux development environment with Node 24.14.0 and full Python 3.12.3:

```sh
node tools/workerd-reference.mjs install /tmp/capsid-workerd/workerd
export CAPSID_WORKERD_REF=/tmp/capsid-workerd/workerd
python3.12 tools/setup-ws-interop-python.py /tmp/capsid-ws-clients
export CAPSID_WS_INTEROP_PYTHON=/tmp/capsid-ws-clients/bin/python
export CAPSID_WS_INTEROP_NODE_PACKAGE=/tmp/capsid-ws-clients/node-ws
node tools/run-workerd-ws-gate.mjs build-dev build-dev/workerd-ws-evidence
```

Use short, private paths for native socket fixtures. The gate checks complete
registration, actual reference/product runs, semantic observations, source and
binary identities, negative controls and shutdown. Installing a reference or
running an API-only subset does not complete the gate. Managed backends require
Linux; Windows and macOS have separate native contracts, not pretend managed
passes. The platform-specific setup is in [Windows](windows.md), and behavioral
limits are in the [workerd semantics reference](workerd-websocket-semantics.md).

## Sanitizers, Fuzzing and Isolation

Use dedicated build trees for ASan, UBSan and TSan. Do not reuse their binaries
for ordinary performance evidence or combine TSan with ASan/UBSan. CMake disables
LTO for instrumentation and rejects unsupported allocator/profile combinations.

The current hosted TSan lane uses **Clang on Ubuntu**; the exact compiler,
provider flags, exclusions and budgets are in the
[checked-in workflow](../.github/workflows/testing-validity.yml). Older journal
sections about a local GCC/musl environment are historical, not a universal
Clang prohibition.

Linux has two distinct sandbox profiles:

- `CAPSID_SANDBOX_PROFILE=production`: production isolation policy; required for
  production isolation acceptance.
- `CAPSID_SANDBOX_PROFILE=sanitizer`: diagnostic policy requiring sanitizer
  instrumentation. It permits tooling operations needed by the instruments and
  is not the production security policy.

Instrumented provider compilation must be verified; linking a sanitizer driver
against uninstrumented dependency objects is not complete coverage. Fatal
sanitizer reports and ordinary sanitizer-test failures cannot be waived as RSS
measurement differences. The narrowly scoped instrumented-memory acceptors keep
raw results and require their original conditions.

Fuzz evidence includes the actual selected harnesses, corpus, budgets, fatal
diagnostics and execution outcome. A compile or a later seed passing does not
close an earlier timeout's cause.

For sandbox checks, unavailable cgroup/namespace/filesystem prerequisites must
remain genuine skips or failures. Delegated Linux CI requires real positive
isolation execution; macOS/Windows native passes cannot replace it. Do not
change host security policy or global system settings just to make a test pass.

## Full Qualification and Evidence

The hosted validity workflow covers Release/LTO, ASan/UBSan/TSan, fuzz, native
macOS and Windows, plus a final hosted evidence index. Release packaging depends
on successful validation. Supplemental Debug and uninterrupted soak evidence is
recorded separately for the qualifying source. Follow the actual workflow and
qualification contract, not a copied historical test count.

Keep at least:

- Exact product commit/tree, dependency locks, build configuration and binary
  identities; distinguish the product checkout from a workflow overlay commit.
- Registration inventory, actual exit code, JUnit, `Testing/Temporary/LastTest.log`
  and complete relevant command logs, including failed and skipped results.
- Reference and tool identities, provider instrumentation, real protocol/OS
  observations, original selections and budgets.
- Package archives, original SHA-256 sidecars, metadata/file manifest, SBOM and
  actual native program compatibility checks.
- Separate reviews for archive integrity and semantic acceptance. An index or a
  coordinator's exit code cannot override a real failed test.

Any infrastructure-only retry needs explicit scope and authorization. Retain the
original outcome and attempt provenance; never present a retried execution as
first-attempt evidence or rerun unrelated successful jobs.

For the published beta's exact results, including the distinction between the
branch retry and first-attempt tag qualification, see
[release qualification](v0.3.0-current-qualification.md). For formal
performance acceptance, use the separate [benchmark methodology](performance-benchmarks.md).
