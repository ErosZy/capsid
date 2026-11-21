# Performance History Site

The [performance site](https://rowanellis-113.github.io/capsid/) defaults to the latest **accepted measured
snapshot**, not to an assumed benchmark of the latest published release. Stable
remains `v0.2.2`; published `v0.3.0-beta.5` has no matching complete performance
session. The latest accepted local measurement is `0.3.0-beta.4` development
snapshot `82eaab9` (`capsid-perf-lane-v3`, pure-JS seven-stack + `dynamic-api`),
completed 2026-11-20 under WSL2. The archived `v1` cross-language measurement
(`485b29b`, 2026-11-16) and historical unverified `v0.2.2` observation remain
preserved and selectable in the top record switcher.
See [current status](../docs/current-status.md) and
[performance methodology/data](../docs/performance-benchmarks.md).

## Files and Data Authority

| Path | Role |
| --- | --- |
| `index.html` | Self-contained SVG charts, three-tier reference UX, scenario guide, and explicit source/verification labels |
| `data.js` | Collector-generated immutable historical points plus verified `v1` (`485b29b`) and `v3` (`82eaab9`) points |
| `status.js` | Generated release/source/measurement context from `tools/refresh-documentation-data.py` |
| `archive-index.json` / generated `archive-index.js` | Hashed compressed transport/part index and its UI projection for complete sealed evidence |
| `archives/` | Full original lane bytes for both `v3` and archived `v1` in deterministic gzip/TAR transports, split `<= 40 MiB` |

The latest point's archive label is `v0.3.0-beta.4`, with an empty `sourceTag`
and measured commit `82eaab999cfd628dbe1529eb553f63c0ee52a9ea`. The UI labels it
as a development snapshot; later releases do not change that identity. The
archived `v1` point (`485b29be6cc8d0dcdb653ee6226956ddece02948`) and historical
`v0.2.2` point remain unchanged. Do not draw a qualified release trend or paired
optimization claim across different matrix contracts (`v1` vs `v3`) or
unverified historical records.

## Prepare the Site

From the repository root with Python 3.12+:

```sh
python3 tools/refresh-documentation-data.py --check
python3 tests/test_perf_archive.py
python3 bench/prepare-perf-site.py --repo . --output /path/to/new-site-output
```

The output must be new and outside `perf-site`. Allow at least 8 GiB of scratch
space: the combined dual-version evidence contains `9656` files (`4533` `v3` +
`5123` `v1`) and about `5.49 GiB` of raw bytes, transported in about `310 MiB`
across 6 compressed bundles. The compressed records are not reduced samples.
The stager restores the original portable namespace and invokes the ordinary
complete lane/point validators for every verified point. It checks
source/binary/session identity, posture, raw rounds, responses,
PSS/ownership/cleanup, checksums, metric replay and exact downloadable
transport bytes before publishing anything.

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
SESSION=local-wsl-v030-beta4-v3-20261120T052109Z-82eaab99
GENERATED_AT="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
python3 bench/collect-perf-point.py --repo "$ARCHIVES" \
  --cold-start "$ARCHIVES/bench/results/${SESSION}-cold" \
  --qps "$ARCHIVES/bench/results/${SESSION}-qps" \
  --density "$ARCHIVES/bench/results/${SESSION}-density" \
  --version 0.3.0-beta.4 --release v0.3.0-beta.4 --generated-at "$GENERATED_AT" --check
```

To replay the archived `v1` cross-language point, pass
`SESSION=local-wsl-v030-headers-ready-20261115T235007Z-7368b4ec`,
`--version 0.3.0`, and `--release v0.3.0` (matching its original frozen archive
label). On Windows use the full Python runtime and PowerShell paths/variables;
the CLI itself is portable. The `runtime-nonportable.tar` inside an original
density record remains an opaque verified member: its Linux generation names are
streamed/checked by the ordinary validator, never extracted as Windows filenames.

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
