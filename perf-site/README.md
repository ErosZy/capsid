# Performance History Site

Updated 2026-11-17. The site defaults to the latest **accepted measured
snapshot**, not to an assumed benchmark of the latest release. Stable remains
v0.2.2; beta.2 is published, but has no matching complete performance session.
The current measured snapshot is `485b29b`, completed 2026-11-16 under WSL2.
See [current status](../docs/current-status.md) and
[performance methodology/data](../docs/performance-benchmarks.md).

## Files and Data Authority

| Path | Role |
| --- | --- |
| `index.html` | Self-contained SVG charts and explicit source/verification labels |
| `data.js` | Collector-generated immutable historical points plus the validated latest point |
| `status.js` | Generated release/source/measurement context from `tools/refresh-documentation-data.py` |
| `archive-index.json` / generated `archive-index.js` | Hashed compressed transport/part index and its UI projection for complete sealed evidence |
| `archives/` | Full original lane bytes in deterministic gzip/TAR transports, split below GitHub's per-file limit |

The latest point's original archive label is `v0.3.0`, but its tag is empty and
its measured commit is `485b29be6cc8d0dcdb653ee6226956ddece02948`. The UI must
call it a development snapshot, not a stable v0.3.0 or beta.2 result. The
historical v0.2.2 point remains unchanged and unverified under the current full
contract. Do not draw a qualified release trend or paired optimization claim
across those different records.

## Prepare the Site

From the repository root with Python 3.12+:

```sh
python3 tools/refresh-documentation-data.py --check
python3 tests/test_perf_archive.py
python3 bench/prepare-perf-site.py --repo . --output /path/to/new-site-output
```

The output must be new and outside `perf-site`. Allow at least 8 GiB of scratch
space: the complete evidence contains 5123 files and about 2.74 GB of raw bytes,
transported in about 163 MB. The compressed records are not reduced samples.
The stager restores the original portable namespace and invokes the ordinary
complete lane/point validators. It checks source/binary/session identity,
posture, raw rounds, responses, PSS/ownership/cleanup, checksums, metric replay
and exact downloadable transport bytes before publishing anything.

Extraction accepts only bounded regular portable members. Links, traversal,
absolute/drive/ADS paths, duplicate/case-aliased names, missing parts, conflicting
digests, extra/omitted files and oversized streams fail. Scratch is newly owned
and retained for inspection; the tool does not delete or repair original
archives. Successful transport extraction alone is not performance acceptance.

Output includes all compressed parts, `evidence-index.json` with original file
digests, and the charts/data. It does not inflate the published site to the raw
archive size or invent individual raw-file URLs that were not staged.

## Restore and Independently Check an Existing Point

This is **replay**, not a new measurement:

```sh
python3 bench/perf_archive.py --repo . --output /path/to/new-archive-root
ARCHIVES=/path/to/new-archive-root
SESSION=local-wsl-v030-headers-ready-20261115T235007Z-7368b4ec
GENERATED_AT="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
python3 bench/collect-perf-point.py --repo "$ARCHIVES" \
  --cold-start "$ARCHIVES/bench/results/${SESSION}-cold" \
  --qps "$ARCHIVES/bench/results/${SESSION}-qps" \
  --density "$ARCHIVES/bench/results/${SESSION}-density" \
  --version 0.3.0 --release v0.3.0 --generated-at "$GENERATED_AT" --check
```

The frozen `--release v0.3.0` here matches the archive's original label; do not
replace it with beta.2 to relabel a different binary. On Windows use the full
Python runtime and PowerShell paths/variables; the CLI itself is portable.
The `runtime-nonportable.tar` inside an original density record remains an
opaque verified member: its Linux generation names are streamed/checked by the
ordinary validator, never extracted as Windows filenames.

## Producing a New Measurement

The cold, QPS and density controllers are implemented. Their
[protocol](../bench/SPEC.md) and [evidence contract](../bench/perf-evidence.md)
remain mandatory: complete same-source/same-session lanes, actual clean build
and binary provenance, exact responses, original budgets, quiet windows,
strict Linux posture and owned cleanup. A short observation, synthetic archive,
metadata file or selected CTest success is not a publishable point.

No new benchmark was run for this documentation refresh. A later code/build
needs its own complete session; it cannot inherit the snapshot's performance
through a version string, source-tree equality or release CI.

## Publishing

`.github/workflows/perf-pages.yml` publishes an Actions artifact, not a
`gh-pages` branch. Its push filter follows **`dev/v0.3.0`** and includes the
site, data, archive/staging and status-generation inputs. It checks generated
status, restores/revalidates evidence, uploads the staged site and deploys it.
GitHub Pages must use **GitHub Actions** as its source; environment protection
must allow the intended development branch.

A successful local build is not proof of deployment. Verify the actual Pages
run and live data/source labels after push. Browser checks cover four viewport
widths, evidence links, no console errors, and historical/current selection.

## Preservation Rules

1. Preserve earlier point values and original sealed archive bytes.
2. Use the collector/validators, not hand-edited chart numbers.
3. Keep measured source, release source, platform, binary and protocol separate.
4. State genuine failures, skips, limits and unmeasured releases explicitly.
5. Never silently retry a failed measurement or use compression to repair it.
