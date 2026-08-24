# Performance optimization handoff (2026-08-25)

This note records the profile-guided QuickJS work on branch
`r0-array-ext-template`.  It is a handoff, not a claim that the optimization
programme is complete.  The retained production change is the mixed-number
multiplication fast path.  The field IC remains measurement-only, and the
current three/four-instruction property fusion experiment must not be merged
until its binary-layout tax is resolved.

## Method and acceptance rule

The current loop follows the approach used by SableJS:

1. profile pinned Kraken 1.1, Octane 2.0 and SunSpider 1.0 programs, plus the
   framework/worker workloads;
2. rank opcode classes, slow paths and adjacent instruction sequences;
3. implement one mechanically explainable candidate at a time;
4. measure paired ABBA/BAAB samples on a pinned CPU;
5. retain a local win when it is reproducible on the workloads that execute
   the optimized path and does not cause a direct-binary regression elsewhere;
6. periodically measure the cumulative build and perform leave-one-out tests.

A candidate does not need to move the equal-weight aggregate significantly by
itself: small, well-attributed wins can accumulate.  Conversely, a same-binary
service win is insufficient if adding its handler changes the interpreter's
machine-code layout and makes the final binary slower.

The classic-suite runner uses four observations per pair, alternates ABBA and
BAAB ordering, and reports pair-local log ratios with confidence intervals.
Positive `gain` means lower candidate latency.

## Reproducible tooling

The repository now contains:

- `bench/prepare-js-suites.py`: builds a guarded, manifest-driven classic
  suite corpus;
- `bench/classic-bytecode.cc`: compiles optimized QuickJS bytecode and runs it
  without the worker/HTTP layer;
- `bench/classic-suite-ab.py`: paired cross-suite A/B runner;
- `bench/classic-suite-profile.py`: opcode/source-site profile collector;
- `bench/profile_sequences.py`: adjacent-sequence census and ext-dispatch
  cost model;
- field-IC worker/host/off-tax A/B and analysis scripts;
- directed fixtures and tests for opcode profiling, IC correctness and
  optimizer differentials.

The corpus used during this session is
`/tmp/capsid-suite-corpus-v1.RPqKC5`, generated from pinned sources in
`/tmp/capsid-js-suites-20260824`.  The complete pre-fix profile is in
`/tmp/capsid-classic-profile-all.sFNBmo`.  A post-fix recollection was started
in `/tmp/capsid-classic-profile-fixed.QjDVXV` but is incomplete and should be
finished before treating exact sequence totals as final.

The complete profile covers 38 runnable programs and about 12.03 billion
dynamic opcode executions.  Its fixed-size exact-site table overflowed by
about 1.204 billion observations.  Aggregate opcode/class counts are useful;
sequence counts are lower bounds and should be used for candidate selection,
not as exact coverage claims.

The highest-volume opcodes were `get_loc8`, `swap`, `get_field`,
`get_array_el`, `add` and `mul`.  The slow-path cost model ranked
`call_method`, `mul`, `get_array_el`, `sub`, `add` and `div` highest.

## Retained optimization: mixed numeric multiplication

`patches/txiki/0043-quickjs-mixed-number-mul-fast-path.patch` keeps the
existing int/int fast path and adds an inline numeric path when both operands
are int/float values.  Objects, BigInt and coercion cases continue through the
original slow path.

Seven paired rounds over eight programs (224 timed observations) produced:

| Program | Gain | 95% CI / note |
| --- | ---: | --- |
| Kraken Beat Detection | +3.02% | [+2.45%, +3.59%] |
| Kraken DFT | +0.78% | not significant |
| Kraken FFT | +2.95% | [+2.62%, +3.29%] |
| Kraken Oscillator | +5.94% | [+5.11%, +6.79%] |
| Kraken Darkroom | +6.75% | [+5.69%, +7.82%] |
| Octane Box2D | +1.08% | [+0.39%, +1.77%] |
| Octane Navier-Stokes | +6.07% | [+5.56%, +6.58%] |
| Octane Richards | +0.56% | not significant |

The equal-weight gain is **+3.366%**, with program-dispersion 95% CI
**[+1.262%, +5.515%]**.  The raw session output is
`/tmp/capsid-mixed-mul-ab-final.ovvMeF`.

## Rejected arithmetic and short-ext candidates

| Candidate | Equal-weight gain | Decision |
| --- | ---: | --- |
| mixed int/float `OP_sub` | +0.119%, CI [-1.750%, +2.023%] | reject |
| numeric `OP_div` inline path | +0.067%, CI [-1.501%, +1.659%] | reject |
| mixed int/float `OP_add` | +0.092%, CI [-0.537%, +0.724%] | reject |
| float `add_loc` path | +0.260%, CI [-0.565%, +1.092%] | reject |
| ext `get_arg0 + get_field` | -1.28%, CI [-1.77%, -0.77%] | reject |

The two-primary-opcode ext candidate cannot save a dispatch: execution still
performs the primary `OP_ext` dispatch and the secondary ext-id dispatch.  New
ext candidates therefore need at least three original instructions, and their
cost model uses `max(length - 2, 0)` avoided dispatches.

## Field IC result

The exact-PC adaptive field IC and its ID32 guards are compile-gated and OFF by
default.  Repairing the terminal observer reduced the original fresh-path
regression from about 18.98% to 2.77%, confirming the diagnosis, but the final
direct PATCHLESS comparison remained unacceptable:

- fresh: **+7.31% latency regression**, 95% CI [+5.15%, +9.51%];
- monomorphic micro: -0.25%, not significant;
- Hono: -0.94%, not significant;
- an IC-capable binary with the feature runtime-disabled still carried a
  roughly 2-3% centre tax.

Response, serialization, round-trip, optimizer-differential and IC correctness
tests passed.  The implementation remains useful measurement infrastructure,
but production must remain OFF.  An interpreter-level C cache is not equivalent
to V8's use of feedback to emit specialized machine code; observer, site-state
and code-layout costs can consume the lookup saving.

## Optimizer correctness fixes

Two real bugs were found while building the suite harness:

1. P11 aliases crossed branches and CFG joins.  Alias state is now cleared at
   branch instructions and branch targets.
2. QuickJS vardef capture flags are laid out as `[arguments..., locals...]`,
   while local opcodes index only the local suffix.  P11, P14 and P16 now add
   `arg_count` when consulting captured-local state.

Directed branch, captured-local and runtime fixtures cover these cases.

`patches/txiki/0044-quickjs-ext-reader-errors.patch` also makes invalid and
truncated ext encodings install a SyntaxError before deserialization returns
an exception.  Previously the reader could return `JS_EXCEPTION` with no
pending exception, causing the directed test to dereference an uninitialized
value.

## Unresolved three/four-instruction fusion

The experiment is intentionally not present in the repository patch series.
Its temporary sources are:

- QuickJS: `/tmp/capsid-qjs-mul-locarray-ext-candidate`;
- optimizer: `/tmp/capsid-optimizer-locarray-src/bytecode_optimizer`;
- baseline tool: `/tmp/capsid-classic-mul-current`;
- ext-capable tool: `/tmp/capsid-classic-mul-locarray-ext34-candidate`.

It defines:

- ext id 2: `get_loc8 + get_loc8 + get_array_el`, saving one dispatch;
- ext id 3: `get_loc8 + get_loc8 + get_loc8 + get_array_el`, saving two
  dispatches;
- passes `0x7f`: neither fusion; `0xff`: three-instruction fusion;
  `0x1ff`: both fusions.

The matcher initially ran before Tier-3 local loads were re-shrunk and therefore
never saw `get_loc8`.  Moving `apply_reshrink` before matching fixed it.
Array/object/missing-property behavior, property-key coercion order, getters
and null exceptions pass the temporary runtime fixtures.

### Same-binary service measurements

The three-instruction fusion, measured by changing only optimizer passes in the
same ext-capable binary, gave +0.208%, CI [-0.462%, +0.882%].  Beat Detection
was +1.543% and FFT +2.169%, both with positive confidence intervals; the
other programs were neutral.

Adding the four-instruction form over the three-instruction form gave +0.293%,
CI [-0.135%, +0.724%].  FFT centred at +0.563%, Gameboy at +0.291%, and
SunSpider date-format-tofte at +0.973%.  Crypto centred at -0.255% with all
three pairs negative, although its interval narrowly crossed zero.

These are legitimate local service wins and should not be rejected merely
because their cross-program aggregate intervals include zero.

### Direct-binary warning

A direct comparison of the mixed-mul baseline (`0x7f`) with the ext-capable
binary (`0x1ff`) was interrupted after 10 of 11 programs and 120 observations.
The missing program is Box2D.  The partial result was -1.262%, CI
[-3.184%, +0.699%], including:

- Beat Detection +1.62%;
- FFT +0.91%;
- Navier-Stokes +1.97%;
- Darkroom -5.49%, significantly negative;
- Gameboy -1.23%, significantly negative;
- Oscillator -5.91%, all three pairs negative;
- date-format-tofte -1.52%.

The raw file is
`/tmp/capsid-locarray34-direct-screen-20260824/samples.jsonl`; there is no
official `summary.json` because the run was interrupted.  The contrast with
the same-binary results strongly suggests a handler/machine-code layout tax.

Before integrating either fusion, isolate that tax with both arms compiling
ordinary BC26 (`0x7f`):

```sh
mkdir -p /tmp/capsid-locarray34-layout-tax-ds
python3 bench/classic-suite-ab.py \
  --corpus /tmp/capsid-suite-corpus-v1.RPqKC5 \
  --out /tmp/capsid-locarray34-layout-tax-ds \
  --control-tool /tmp/capsid-classic-mul-current \
  --candidate-tool /tmp/capsid-classic-mul-locarray-ext34-candidate \
  --control-passes 0x7f \
  --candidate-passes 0x7f \
  --pairs 7 --timeout 240 --cpuset 3 \
  --program audio-beat-detection \
  --program audio-fft \
  --program audio-oscillator \
  --program imaging-darkroom \
  --program gameboy \
  --program navier-stokes \
  --program richards
```

The two tools produced byte-identical BC26 for the directed fixture, SHA-256
`2b4926d6a8aa55f196d481f3c7e5fd07981b98aae2a7bf35be560ea182b29346`.

If the layout regression reproduces, reject the current handler placement or
move/compact the handlers and remeasure.  Do not merge a service win that makes
the final binary slower.  Use a new output directory for every retry because
the runner truncates `samples.jsonl`.

## Next profile candidates

After resolving the ext-layout question, the strongest cross-program sequence
candidates are:

1. `get_array_el > mul > add`: about 55.99M lower-bound executions over seven
   programs;
2. `mul > add > put_loc8`: about 40.04M over four programs;
3. `add > put_loc8 > get_loc8`: about 25.37M over six programs;
4. `add > dup > put_loc > drop`, after an exact census.

The fourth pattern is attractive because its handler can preserve the complete
QuickJS `add` semantics: int, float, BigInt, string and object coercion can use
the original slow path, while a successful result is transferred directly to
the local.  It does not require speculative guards or deoptimization.

`call_method` has the largest modeled slow-path cost, but it is a substantially
higher-risk semantic change and should follow the simpler fusion work.

## Validation and repository identity

The current txiki series contains 45 patches.  The expected overlay identity is:

- key: `9237a52d906f059931355bbd09d9f62ec51bc836634be1da2c039946d0bf34cf`;
- manifest:
  `1e0e8a0f582a8c3e31bf8b57aa5a2a54d6cd1e0e6c17da02078b113564244650`.

`git diff --check` is clean.  Targeted optimizer, ext-bytecode and overlay
tests passed during development, but the existing build directory was created
before patches 0043/0044 and now has a stale overlay stamp.  Create a fresh
build directory, run the full optimizer/ext tests and then the complete test
suite before declaring the cumulative work finished.

The final completion gate remains:

1. resolve or reject the ext34 experiment;
2. finish the corrected full-suite profile;
3. exhaust the remaining high-value candidates until repeated candidates fail
   their direct-binary gates;
4. freeze a cumulative baseline and run leave-one-out measurements;
5. validate the final build on classic suites, worker/framework workloads and
   the full correctness suite.
