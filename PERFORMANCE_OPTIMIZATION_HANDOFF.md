# Performance optimization handoff (reviewed 2026-08-25)

This is the authoritative handoff for branch `r0-array-ext-template`. The
production result is deliberately conservative: keep the already accepted
mixed-number multiplication fast path, keep CFG/SSA and profiling as analysis
infrastructure, stop the field IC, and retain ext34 only as an explicitly
compiled experimental backend. Default builds contain no ext34 reader rows,
handlers, observer, or code-layout change and emit BC26.

## Final decisions

| Work item | Decision | Reason |
| --- | --- | --- |
| mixed numeric `mul` fast path | keep | +3.366% equal-weight gain on the selected classic corpus, CI [+1.262%, +5.515%] |
| exact-PC field IC | stop/default OFF | PATCHLESS fresh-receiver latency regressed +7.31%, while mono and Hono had no significant win; feature-built OFF also carried a 2–3% centre tax |
| CFG+SSA | keep analyze-only | useful for correctness, census and selection; the current lowering vocabulary does not itself create a speedup |
| ext34 loc-read + array access fusion | experimental, compile-gated OFF | corrected same-binary results prove large target wins, but compiling the handlers into an otherwise-OFF runtime regresses the broad sample |
| more ext handlers now | stop | first remove or isolate enabled-binary layout cost and select a candidate from a workload that will measure it |

“OFF” is not merely a pass mask. `CAPSID_ENABLE_EXT_FUSION34=OFF` now removes
the live ext ids, operand formats, reader validation and runtime handlers at
preprocessing time. The optimizer masks the reserved ext34 pass bits. The ON
state is included in `bytecodeCompileFlags`, so an ON compiler/runtime cannot
silently exchange BC27 with an OFF runtime under the same compatibility ID.

## Why the old ext34 keep record is invalid

The earlier +0.66%/+0.86% record was collected with a memory-unsafe matcher.
Its three-byte `slots` array let `get_loc0_loc1` write two decoded slots at
`slots[total]` even when only one element remained. A sequence containing two
slot pairs could overwrite the stack and, in optimized builds, delete values
that preceded the legal suffix. That invalidates all pre-fix ext34 timing as a
keep decision, even when a particular corpus happened not to expose a visible
wrong result.

The review also found two independent safety/semantic gaps:

- BC27 accepted tagged local/argument indexes without checking them against
  the containing function's frame sizes; the handlers then indexed the frame
  arrays directly.
- the fused slow path set `sf->cur_pc` differently from the original
  `get_array_el`, which could change exception source/backtrace behaviour.

The matcher now decodes each slot instruction into a two-byte temporary,
checks the combined width, and only then copies it. The strict optimizer
reader, CFG decoder and QuickJS reader share/equivalently enforce slot payload
length and argument/local bounds. The handler uses the original
`get_array_el` PC convention. Directed tests cover pair+pair, singleton+pair,
invalid frame indexes, getters, slow coercion paths and exact exception stack
equality.

## Corrected ext34 performance evidence

Pinned corpus revisions:

- Kraken 1.1: `77ef4e08af23c131166762adad8cb460c49160e8`
- Octane 2.0: `570ad1ccfe86e3eecba0636c8f932ac08edec517`
- SunSpider/JetStream resources: `7769b693502fa80f28a97bbfacd3296e0513acc5`

The corrected same-binary A/B compares pass mask `0x7f` with `0x1ff` in one
feature-capable runtime, using seven balanced ABBA/BAAB pairs per program
(224 timed samples):

| Program | Candidate gain | 95% CI | Positive pairs |
| --- | ---: | ---: | ---: |
| Kraken Beat Detection | +9.72% | [+8.80%, +10.66%] | 7/7 |
| Kraken FFT | +9.66% | [+8.70%, +10.63%] | 7/7 |
| Kraken Oscillator | +0.04% | crosses zero | — |
| Kraken Darkroom | +0.59% | crosses zero | — |
| Octane Box2D | +0.36% | crosses zero | — |
| Octane Gameboy | +0.05% | crosses zero | — |
| Octane Navier-Stokes | +3.22% | [+2.39%, +4.07%] | 7/7 |
| Octane Richards | +0.63% | crosses zero | — |
| Equal-weight geomean | +2.96% | program CI [-0.46%, +6.49%] | — |

This answers the instruction-fusion question: the operation itself works.
When its exact loc-read/array windows are hot, removing two or three primary
dispatches has a material and repeatable benefit. Neutral programs do not show
a significant same-binary regression.

The required direct PATCHLESS-vs-feature-built-OFF attribution compares two
different runtimes while both compile and execute mask `0x7f`; every one of
the eight bytecode pairs is byte-identical. The feature-capable binary still
measured an equal-weight **-1.44%** regression, program-dispersion CI
**[-2.50%, -0.37%]**. Beat, FFT, Box2D, Navier-Stokes and Richards had
significant regressions. Therefore “serialized BC26 is identical” never proved
zero layout tax; it proved only output identity.

The final product-shaped comparison does not subtract those two sessions. It
directly compares the pre-0045/PATCHLESS `0x7f` binary with the corrected
feature binary running `0x1ff`:

| Program | Net enabled-binary gain | 95% CI |
| --- | ---: | ---: |
| Kraken Beat Detection | **+8.30%** | [+6.63%, +10.00%] |
| Kraken FFT | **+8.26%** | [+6.40%, +10.15%] |
| Kraken Oscillator | -1.14% | crosses zero |
| Kraken Darkroom (bytecode-identical control) | **+1.65%** | [+0.91%, +2.39%] |
| Octane Box2D | **-2.21%** | [-2.90%, -1.51%] |
| Octane Gameboy | +0.18% | crosses zero |
| Octane Navier-Stokes | +0.91% | crosses zero |
| Octane Richards | **-3.22%** | [-4.42%, -2.02%] |
| Equal-weight geomean | +1.51% | program CI [-2.07%, +5.22%] |

The enabled binary therefore has real workload-specific wins, but not a broad
keep: its aggregate interval crosses zero and two Octane programs regress
significantly. Darkroom emits no ext and has byte-identical bytecode, yet moves
significantly in the opposite direction, directly demonstrating that runtime
machine-code layout effects are workload-dependent and cannot be inferred from
serialized bytes.

The compile gate removes that tax from production. A standalone default-OFF
QuickJS build was compared with the true pre-0045 source using identical flags:

| Artifact | Result |
| --- | --- |
| `quickjs.c.o` | byte-identical, SHA-256 `d7a4e5ab30874cb447bb23e15c3804ff007493cacefd2dabc2559a58fa9d88b8` |
| `libqjs.a` | byte-identical, SHA-256 `909eb7cad319784505552213915a6723c487f01874dfca601f61c3037fd6f5aa` |
| `qjs` executable | byte-identical, SHA-256 `e1fe59f0e5a211f53e61fce88bb9a5ea97308dfbdded304b1276b39bd50352f3` |

This is stronger than another noisy OFF timing run: the production machine
code is the patchless machine code.

Raw corrected sessions are local under:

- `bench/results/ext34-fixed-review-20260825/` — corrected same-binary fusion;
- `bench/results/ext34-off-tax-fixed-review-20260825/` — compiled-in OFF tax;
- `/tmp/capsid-ext34-fixed-net-review-20260825-r2/` — direct patchless-to-enabled
  net comparison.

The three `summary.json` SHA-256 values in that order are
`2ec9ea79208a47fb7f389bdc920c88396b80497bc12356a7f3623b8975d74a7d`,
`407a714f35afebdeaf6f6e90ae29582beb97b4362eee4f4080e9257a09700392`,
and `a77ffe5448b0cf7f9fa864961bb1bfd1ef655ca84fa7da95734eb2cb0d82cd0a`.

`bench/classic-suite-ab.py --require-bytecode-identical` and
`bench/ext34-off-tax-ab.sh` now make the runtime-tax protocol reproducible.
`bench/layout-tax-ext34.sh` is explicitly described as a serialized-output
identity check, not a runtime layout measurement.

## Correctness and validation state

The following are green in both relevant configurations:

- default-OFF optimizer, ext reader and round-trip tests;
- feature-ON optimizer goldens and live runtime differential tests;
- feature-ON CFG and BC27 round-trip tests;
- invalid/truncated/unknown/out-of-range BC27 reader rejection;
- getter, property-key coercion, missing value, null exception and backtrace
  equivalence;
- ASan/UBSan optimizer/runtime run, including the sequence that found the old
  overwrite.

The classic-suite harness checks successful completion and each suite's own
embedded assertions. Its appended `__capsidSuiteOk` marker is not a general
output oracle, so the report must not call it “zero semantic mismatches.” The
strong semantic evidence for ext34 is the directed base/optimized differential
suite; the classic corpus supplies performance and execution-completion
evidence.

## What CFG+SSA can and cannot do here

The limitation is not missing AST. QuickJS bytecode already contains enough
control flow and dataflow to construct CFG and stack/local SSA. Lowering is
also possible. The issue is economic: if SSA lowers back to the same BC26
operations, QuickJS's C interpreter has already implemented most obvious local
fast paths, so analysis alone saves little. A new fused opcode pays ext decode
and secondary dispatch; it must eliminate enough primary dispatches,
materialization/refcount work, or a genuinely expensive generic lookup to pay
for that cost.

ext34 is useful evidence because it crosses that threshold at hot sites. R0's
single array opcode and `get_arg0 + get_field` did not. Thus CFG+SSA should
remain the proof and selection layer, not be sold as the optimization itself.

## Why V8/Hermes can gain much more from ICs

QuickJS already puts common tag/array/property cases close to the interpreter
opcode. Wrapping that C path in another cache can save lookup work, but also
adds training, guards, state, misses and code-layout pressure. V8 uses feedback
to select or patch much lower-level specialized code paths; its hit path can
become a small sequence of loads, compares and a direct branch instead of a
trip through generic C dispatch. Hermes likewise designs bytecode/runtime
specialization as part of the VM architecture rather than as an extra table
around an already compact QuickJS handler.

The old QuickJS-ng atom-shared four-shape ring avoided exact-PC lookup but
mixed same-name sites and retained heavier per-function state. Repeating that
design is not the next step. If field IC work resumes, allocate a slot per
bytecode site at compile time, encode the slot operand directly, allocate state
only for proven-hot functions, and benchmark the final enabled binary directly
against PATCHLESS. A large V8-like gain still requires using the feedback for
lower-level specialization or wider fusion; a C-level property cache alone is
unlikely to produce it.

## Next plan

1. Do not enable ext34 in production and do not add another handler to the
   default binary. The compile gate is the accepted containment fix.
2. Preserve ext34 as a reproducible experiment because the corrected Beat/FFT
   wins are real. Use it to evaluate handler placement/outlining and remove the
   Box2D/Richards regressions; do not ship the current enabled binary.
3. Keep profiling Kraken/Octane/SunSpider plus framework workloads. Rank by
   dynamic executions *and* number of programs, then measure one candidate at
   a time. Do not let Octane zlib's single-program dominance choose the whole
   catalog.
4. Prefer fusions such as `add -> dup -> put_loc -> drop` only after an exact
   cross-suite and framework census. It can reuse QuickJS's complete add slow
   path and transfer the successful result directly, avoiding speculative
   type guards/deopt, but it still needs final-binary A/B evidence.
5. Revisit object IC only as a compile-time per-site-slot design with a zero-
   tax default and a direct PATCHLESS gate. Stop immediately if the enabled
   binary cannot beat PATCHLESS on both the intended stable-shape workload and
   at least one production-shaped framework workload.

The practical conclusion is: instruction fusion is viable, CFG+SSA is useful
to find and prove it, and the current field IC is not viable. The current
ext34 implementation is valid experimental evidence but not a production
optimization because its enabled runtime layout cost is too broad.
