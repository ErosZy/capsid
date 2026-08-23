# Bytecode AOT Optimizer: Compile-Time Backend Optimization

This document is the design and gate-keeping contract for the compile-time bytecode
optimizer on the trusted-bytecode path. It is a post-serialization, capsid-side
optimizer: it rewrites standard quickjs-ng bytecode (BC_VERSION 26) produced by
`capsid-bytecode-compile` before attestation, using a CFG + stack-state dataflow
(constant lattice) pass pipeline. It never changes the vendored VM, the serialized
format, or the bytecode compatibility identity; unmodified runtimes load its output.

## 1. Scope and Non-Goals

What this work is:

- a deterministic rewrite of the serialized `.qjsb` buffer between `JS_WriteObject`
  and the sha256/attestation step in `tools/capsid-bytecode-compile.cc`;
- v1 = CFG construction + exact-stack-height/lattice dataflow + within-BB peepholes
  + constant-branch folding + jump threading + unreachable-block elimination
  + short-opcode re-shrink, iterated to a fixed point;
- fail-closed: any input the optimizer cannot fully understand aborts the compile
  with no output files, matching the attestation contract's fail-closed philosophy.

What this work is not:

- no vendored code changes (`vendor/`, `patches/`, the overlay manifest, and the
  bytecode compatibility ID stay untouched; existing signed attestations keep
  verifying);
- no VM/runtime changes: `JS_ReadObject` and the interpreter consume the output
  as-is, with no load-time fixups;
- no new cpool entries ever: folding only produces immediates representable by
  existing opcodes (`push_minus1..push_7`, `push_i8/i16/i32`, `push_this/false/
  true/null/undefined/empty_string`, ...);
- not a JIT, not partial evaluation, not a format change: quickjs-aot (+36%),
  quickjit, and CPython-3.11-style specialization all change the VM or the output
  format and are out of the "unmodified runtime" contract;
- v1 deliberately stops short of full SSA (SCCP/GVN) — see §8.

## 2. Why This Exists

The sablejs backend experience shows compile-time CFG/SSA-style optimization pays
off, but on an interpreter the ceiling is bounded: quickjs-ng dispatch is ~5-25% of
runtime, and AOT bytecode rewriting only reduces dynamic instruction count, never
per-instruction cost. `resolve_labels` (quickjs.c:34937-35860) already performs
single-pass peepholes (constant-test folding, push/drop elimination, short-opcode
shrinking, dead-code skipping, jump threading). The optimizer's incremental value
is arithmetic constant folding, cross-block constant propagation, dup/swap/rot
cleanup, whole unreachable-block elimination, and re-shrinking after edits.

The realistic expectation is documented in §7: single-digit to low-double-digit
percent on compute-bound paths, ~0 on load/spawn/IO. Whether that is worth
continuing is decided by the gates in §7, not by hope.

## 3. Format Facts (implementation contract)

All offsets reference `deps/quickjs/quickjs.c` at the pinned commit
`bf8988fc401e737f9946cd10a3463b48aab0fd7e` (v0.15.1-11-gbf8988f).

- Header: u8 `BC_VERSION` (=26, :37686) + u32 checksum (`bc_csum(buf+5, size-5)`,
  :37836-37860, little-endian) + LEB128 atom count + atom table (:38423-38441).
- Function record (`JS_WriteFunctionTag` :37965-38058): tag u8, u16 flags
  (bit 10 = has_debug_info; capsid does not strip), u8 strict, atom func_name,
  LEB128 × 8 (arg_count, var_count, defined_arg_count, **stack_size**,
  var_ref_count, closure_var_count, cpool_count, **byte_code_len**), vardefs,
  closure vars, **cpool first**, code blob, debug block (filename atom, line,
  col, pc2line_len + blob, source_len + source blob).
- Module record (`JS_WriteModule` :38060-38108): tag, module_name, req/export/
  star/import tables, has_tla u8, then the top-level function record.
- `BCTagEnum` :37660-37684: the reader must skip every tag exactly (INT32 =
  sleb128, FLOAT64 = 8 bytes LE, STRING = width-aware, BIG_INT, SYMBOL,
  TEMPLATE_OBJECT/ARRAY/OBJECT = recursive, REGEXP, TYPED_ARRAY, ARRAY_BUFFER,
  MAP/SET, ...). `BC_TAG_FUNCTION_BYTECODE` entries are nested function records —
  recurse. Unknown tag → fail closed.
- Jump semantics: uniformly `target = operand_start + signed_offset` (interpreter
  :18694-18828). The only statically visible exception edges are `OP_catch`
  operands (:18789). `OP_ret`'s static targets are the instructions immediately
  following `OP_gosub` — those positions are roots.
- Opcode table: `#define DEF(id,sz,pop,push,f)` + `#include "quickjs-opcode.h"`
  (same method as quickjs.c:21856; the header self-undefines). Final opcodes are
  byte values 0..184, short opcodes 185..251 (numerically overlapping the
  never-serialized temp opcodes 185..203), `OP_COUNT` = 252; op == 0 or >= 252
  is invalid.
- pc2line (`compute_pc2line_info` :34735-34780, constants :767-770): cumulative
  (pc_delta, line_delta, col_delta) triplets; short form
  `byte = (line_delta+1) + pc_delta*5 + 1` when line_delta ∈ [-1,4) and
  pc_delta ≤ 50, else `0x00 + uleb128(pc_delta) + sleb128(line_delta)`, always
  a trailing `sleb128(col_delta)`.
- Re-shrink reference: the `resolve_labels` shrink loop :35748-35828 (memmove +
  label/jump/source-loc address fixups), implemented as a whole-buffer rebuild.
- Verifier reference: `compute_stack_size` :35907-36101 (per-pc stack height
  consistency, n_pop ≤ h with npop/npop_u16/npopx variable pops).

## 4. Architecture

A post-serialization buffer optimizer implemented entirely on the capsid side:

1. a strict, bounds-checked standalone reader over `(uint8_t*, size_t)` — no
   quickjs internals, no JS runtime;
2. recursive optimization of every function record (module top-level function →
   cpool-nested `BC_TAG_FUNCTION_BYTECODE` children);
3. rewriting only: code blobs, the function record's `byte_code_len` LEB128, the
   debug block's `pc2line_len` LEB128 + blob, and the header checksum. Everything
   else — atom table, vardefs, closure vars, cpool, module tables, source blob —
   is copied verbatim;
4. output is standard quickjs-ng bytecode, loadable by the unmodified runtime.

Files:

- new `tools/bytecode_optimize.h` — `namespace capsid::bytecode;
  bool optimize(const std::vector<uint8_t>&, std::vector<uint8_t>*, std::string*)`;
- new `tools/bytecode_optimize.cc` — the implementation (opcode tables, LEB128/
  checksum, reader + cpool skip, pc2line decode/encode, CFG, dataflow, passes,
  emitter/splicer, verifier, orchestration); C++11, `-Werror`;
- new `tests/test_bytecode_optimizer.cc`, `tests/fuzz/fuzz_bytecode_opt.cc`;
- new `bench/exec-throughput.sh`, `bench/bytecode-raw.cc`, `bench/analyze.cc`,
  `bench/fixtures/cascade-rt.js` (fixtures reuse `bench/gen-cold-start-apps.py`);
- changed `tools/capsid-bytecode-compile.cc` — one call between `compile_module`
  and the sha256 step, fail-closed abort on error;
- changed `cmake/build_worker.cmake` — a `capsid_bytecode_opt` static library
  linked into `capsid-bytecode-compile`;
- changed `cmake/build_tests.cmake` — new ctest entries + fuzzer registration.

Not touched: `vendor/`, `patches/`, `cmake/ComputeBuildIdentity.cmake`,
`docs/txiki-upgrade-baseline.json`, the worker, `src/worker_runtime.cc`.

## 5. Pass Pipeline (v1)

```text
P0  Parse + validate (one pass over the buffer; version/checksum/atom checks; fail closed)
P1  Per-function decode → Insn list + leader set + CFG
    leaders = {0} ∪ all label/label8/16, atom_label_u8/u16 targets ∪ post-gosub insns
    succ: unconditional {target}; conditional {target,next}; gosub/catch {target,next};
          ret {} (covered by roots)
    roots = entry ∪ all catch targets ∪ all post-gosub instructions
P2  Stack-height + lattice dataflow (worklist in pc order; join heights must match)
    lattice: kUnknown / kUndef / kNull / kBool / kSmallInt(int32) / kThis
    cross-BB gate: functions containing with_*/eval/apply_eval or any variable
    write op get Unknown at joins (values must be produced and consumed in one BB)
P3  Within-BB peepholes (never across a leader; line of the last replaced insn wins)
    ① push push binop → push_short (JS semantics; skip on i32 overflow / div-by-zero /
       non-integer division — those need float/cpool)
    ② const const strict_eq/neq from {kNull,kUndef,kBool,kSmallInt} → push_false/true
    ③ null strict_eq → is_null; undefined strict_eq → is_undefined (propagated, not
       only literal forms)
    ④ push_* drop → deleted (exception-free push forms only)
    ⑤ dup drop; dup2 drop → dup1; swap swap (quickjs-ng PR#143 precedent);
       rot3l rot3r mutual cancel
    ⑥ constant conditional: const if_true/if_false L → goto L or deleted entirely
    ⑦ push_const/atom_value + drop combos (parser residue)
P4  Threading: goto L (L is goto M) → goto M; if_* with target == next → drop
P5  Unreachable whole-block elimination (reachability closure from roots; only whole
    blocks, never bytes another insn's label points into)
P6  Re-shrink: push_i32 → push_minus1/0..7/i8/i16; push_const → const8;
    get/put/set_loc|arg|var_ref → *8/0..3; call argc<4 → call0..3; goto/if_* → 8/16
    bit forms by distance; whole-buffer rebuild with shortest-encoding offset rewrite
P7  pc2line remap: decode old table, map old→new pc during emission, re-encode with
    the exact compute_pc2line_info rules
P8  Single-pass emission: verbatim copy with three patch points (byte_code_len,
    code blob, pc2line_len + blob), incremental offset tracking, checksum recompute
Fixed point: P2-P6 repeat until no byte changes, capped at 16 iterations
Verifier (after every pass + final output): every op decodes, all label targets land
on op boundaries, per-pc stack height consistent, no underflow, max ≤ stored stack_size
```

As built after the G4 trims (2026-08-23, §11): the deployed pipeline is P0-P2 +
P3① + P11 + P14 + P6-P8. P3②-⑦, P4, and P5 were deleted at the v1 G4 — each
attributed <1% on the committed corpus, and quickjs-ng's `resolve_labels`
already covers push+drop, dup/swap/rot3, and same-block constant conditions.
The tier-2 SSI suite (P9-P15') was built, measured, and deleted at the tier-2
G4 (commit 8078d04); P11 (copy propagation + dead-store materialization) and
P14 (literal `get_field` fold) are the surviving tier-2 passes — see §11.
Tier-2b (2026-08-23, §11) added P16 (TDZ-sound dead-store elimination):
P16 runs inside the P2/P3.1 fixpoint, after the tier-2 direct passes.

## 6. Soundness and Correctness

- Every rewrite stays inside one basic block, never across a leader or try/catch
  boundary; only table-driven side-effect-free ops are deleted or folded;
- cpool entries and atom indexes are never added, removed, or reordered —
  `const`/`const8`/`fclosure` indexes stay valid;
- determinism: pure function of the input bytes; no unordered containers on any
  output-affecting path; pc-order iteration everywhere;
- fail-closed contract: bad version, bad checksum, unknown tag/opcode, malformed
  LEB128, out-of-bounds read, stack underflow, non-monotonic pc2line, convergence
  cap exceeded, depth cap exceeded → `optimize()` returns false, the tool prints
  to stderr and exits 1 with no output files. No silent passthrough in v1.

## 7. Effectiveness Gates (Go/No-Go)

Step 0 runs before any rewrite pass is written: an **analyze-only mode** (reader +
CFG + dataflow statistics only) measures per-function foldable-instruction density
and shrinkable bytes on real bundles, producing the theoretical ceiling. A
three-state benchmark (source vs unoptimized bytecode vs optimized bytecode) runs
compute-bound fixtures on pinned cores, ≥5 rounds, median.

| Gate | Standard | On failure |
| --- | --- | --- |
| G1 Correctness (hard) | RED/round-trip/differential/fuzzer all green; bit-for-bit determinism | fix, no discussion |
| G2 No regression (hard) | cold-start load/spawn within noise (<2%); no throughput regression on non-compute paths | fix or cut the offending pass |
| G3 Effectiveness (go/no-go) | compute-bound median improvement vs **unoptimized bytecode** ≥ max(3%, 50% × measured static ceiling) | stop, report honestly, no tier-2 |
| G4 Per-pass attribution | each pass ≥1% standalone (except prerequisites) | built-in pass switches for per-pass measurement |
| G5 Ceiling explanation | static shrink rate vs wall-clock gain; large gap = dispatch-amortization evidence | decides tier-2 |

The comparison baseline is **unoptimized bytecode**, never source: source compares
mix in parse skipping (the existing 8.34→7.29 ms cold-start delta) and would
overstate the optimizer. Report-mode statistics go to stderr, keeping the frozen
CLI output contract and determinism intact.

**Theoretical ceiling model (prior, to be calibrated by Step 0 measurement):**
wall-clock gain ≈ Σ (eliminated dynamic instructions × their cost share). Layer
priors: CFG+folding+DCE+threading → 3-8% on compute-bound code (sablejs O0→O1
measured ≈3%; quickjs's single-pass residue is fatter but the eliminated ops are
cheap); +SCCP/GVN → ~0 (stack values are consumed immediately, cross-block CSE is
stack-neutrality-constrained); +LICM → +5-15% on property-dense loops (the only
pass that removes expensive `get_field` lookups, but alias-proof soundness barriers
reject most real loops). Full-stack ceiling ~10-20% (optimistic 25%), IO-bound
paths ~0-3%. This is consistent with the historical range of no-JIT interpreter
AOT (compute-bound 5-20%, general 0-5%). Not comparable: quickjs-aot +36% (removes
dispatch, changes format), CPython 3.11 10-60% (in-VM inline specialization),
sablejs 57x (codegen lowering + host JIT).

## 8. Tier-2 Feasibility (decided later, not committed)

Stack-slot SSA/SCCP is technically feasible (SSI form, Soot Shimple, Java
bytecode optimizer precedents), and v1's CFG + exact stack-height invariants are
its prerequisite infrastructure — an incremental upgrade, not a rewrite. But
marginal gains are diminishing: quickjs bytecode values are consumed immediately
(no register namespace), cross-block GVN/CSE is stack-neutrality-constrained, and
LICM mostly hoists already-cheap `push_const`. Priority order after v1 evidence:
local copy propagation, literal `get_field`/`get_array_el` folding, cpool-resident
constant folding — then re-evaluate SCCP with real numbers.

## 9. Testing

- `bytecode_optimizer` unit tests: reader (every cpool tag, per-byte truncation,
  bad LEB128/version/checksum), LEB128/checksum golden values, CFG (catch/gosub/
  with operands, post-gosub roots), every deployed peephole input→output (P3.1
  golden bytes; the G4-trimmed passes' tests were removed with the passes),
  cross-BB propagation and with/eval gates, pc2line remap, double-run
  determinism, and
  full round-trips (JS_Eval COMPILE_ONLY → JS_WriteObject → optimize →
  JS_ReadObject → JS_EvalFunction, comparing values/exceptions/stack-trace lines
  against the unoptimized path, covering try/catch/finally, for-of, generators,
  classes, closures, with, eval, template literals, bigint, compute-heavy code);
- the frozen RED `runtime_bytecode_compiler_round_trip` must keep passing
  unchanged (it now exercises optimized bytecode end-to-end, including the
  bit-for-bit determinism check);
- differential test across all `tests/fixtures/*.js` (optimized-bytecode worker
  vs source worker, response equality);
- `fuzz-bytecode-opt` under ASan/UBSan (corpus = deterministically compiled
  `.qjsb` fixtures); invariants: no crash, output always parseable, checksum
  valid, `optimize(optimize(x))` stable.

## 10. Risks

| Risk | Mitigation |
| --- | --- |
| Format drift on quickjs upgrade | BC_VERSION == 26 gate fails closed; compatibility ID untouched |
| cpool tag parse error → corrupt output | full tag enumeration with exact extents; unknown tag aborts; JS_ReadObject round-trips prove parse alignment |
| pc2line mismatch → wrong stack-trace lines | decode/encode unit tests + runtime backtrace smoke test |
| parser overflow / DoS | bounds-checked reader everywhere (precedent: quickjs-ng#1416), 5-byte LEB128 cap, depth/iteration/instruction-count caps |
| jump-offset bugs after re-shrink | verifier asserts every target lands on a leader/op boundary; RED + differential cover if/goto-dense code |
| determinism regression | no unordered containers in output paths; bit-for-bit RED check; idempotence test |

## 11. Execution Status

Tracked against the implementation order in the plan (`/home/eroszhao/.claude/plans/swirling-cooking-rose.md`).

| Step | Status | Evidence |
| --- | --- | --- |
| 0. Ceiling analysis + analyze-only | done | analyze-only + three-state harness; see G5 caveat (scan under-reports chains) |
| 1. Reader skeleton + validation | done | RED round-trip green; fail-closed matrix on truncated/bad checksum inputs |
| 2. CFG + dataflow + verifier | done | per-pass verifier; stack-height invariant; with/eval gates |
| 3. Core peepholes + pc2line + emission | done | full round-trip green |
| 4. Threading + dead blocks + re-shrink | done, then trimmed | P4/P5 deleted at G4 (below); re-shrink + emitter retained |
| 5. Cross-BB constant lattice + scope gates | done | two dataflow bugs found and fixed during Step 8 verification (see G1) |
| 6. Fixpoint + report mode + pass switches | done | always-on report; corpus green |
| 7. Differential + ctest + fuzzer | done (2026-08-23) | 10/10 ctest gates; fuzzer 20k runs under ASan/UBSan |
| 8. Benchmarks + G1-G5 verdict + docs | done (2026-08-23) | this section |
| 9. Tier-2 decision | reversed (2026-08-23): built + adjudicated | §11 below: SSI suite trimmed at G4, P11/P14 kept |

### As-built pipeline (post-G4 trim, commit 47b9369)

Deployed pipeline: P0 parse → P1 decode/CFG → P2 cross-BB lattice → P3.1 const
binop fold → fixpoint → P6 re-shrink → emit → verifier → P7 pc2line → P8
splice/checksum. P3.2-P3.7, P4, P5 were deleted at G4: they attribute <1% on the
committed corpus, and quickjs-ng's own `resolve_labels` already removes push+drop,
dup/swap/rot3, and same-block constant conditions at emission. The pass-switch
API (`kPassP2` / `kPassP31`) remains for attribution; `kPassAll = P2|P31`.
Format-level passes (P6 re-shorten, compaction, verification) always run.

### G1 Correctness — PASS (hard gate)

- 10/10 ctest gates on the final tree: `host_bytecode_attestation`,
  `runtime_bytecode_compiler_round_trip` (frozen RED, includes bit-for-bit
  determinism over optimized output), `bytecode_optimizer` (unit suite),
  `bytecode_opt_differential` (all `tests/fixtures/*.js`), the trusted-bytecode
  and build-identity matrix.
- Fuzzer `fuzz-bytecode-opt` (ASan/UBSan, corpus = deterministically compiled
  bundles): 20,000 runs, no invariant violations, output always parseable.
- Two dataflow bugs found by the differential body checks during Step 8 and
  fixed with regression fixtures before the final evidence run:
  1. cross-BB propagation never escaped the entry block — the first predecessor's
     all-`kP2Unknown` join absorbed the exit state, so only the entry block was
     ever processed; fixed with per-block `in_seen` adoption (commit ea8702e);
  2. slot aliasing — loc/arg slot indexes were read from the push-immediate
     field, aliasing every slot to slot 0 and folding a stale constant into
     `acc` (arith-rt body 152898696 vs correct 7074999600000); fixed with
     `loc_index()` reading aux/opcode only (commit 59b6d4c); regression fixture
     `tests/fixtures/opt-slot-reuse.js` (fails on the pre-fix tree).

### G2 No regression — PASS (hard gate)

- Zero vendor/runtime/format changes by construction: only code blobs,
  `byte_code_len`, the pc2line blob, and the header checksum of standard
  BC_VERSION 26 output are rewritten; `runtime_build_identity` and the
  attestation matrix are green; compatibility identity is untouched.
- Non-compute fixtures (matrix/sieve/string/fib/json) produce **byte-identical**
  optimizer output (0% static ceiling), so their load/execution path is unchanged
  by construction; measured G3 deltas on them are ±5% noise on 0.4-25 ms runs.
- The optimizer runs at compile time only; the worker's load path is untouched.
  (cold-start.sh needs the M1D fixture toolchain that does not exist on this
  machine; the source-vs-bytecode cold-start delta of §7 is unchanged because the
  format contract is.)

### G3 Effectiveness — PASS (go/no-go)

Final run `bench/results/exec-throughput-20260823T042708` (commit cab458d,
Release, taskset 0-3, 1 warmup + 5 rounds, median; manifest + sha256 recorded):

| fixture | raw ms | opt ms | G3 opt vs raw | static insns | ceiling | threshold | verdict |
| --- | ---: | ---: | ---: | --- | --- | --- | --- |
| arith-rt | 44.487 | 27.164 | **+38.94%** | 145→85 (41.4%) | 41.4% | 20.7% | PASS 1.9× |
| cascade-rt | 16.135 | 11.441 | **+29.09%** | 100→76 (24.0%) | 24.0% | 12.0% | PASS 2.4× |
| matrix-rt | 4.832 | 4.638 | +4.01% | 0% | 0% | 3% | ceiling-limited, noise |
| sieve-rt | 24.998 | 25.613 | -2.46% | 0% | 0% | 3% | ceiling-limited, noise |
| string-rt | 0.440 | 0.459 | -4.32% | 0% | 0% | 3% | ceiling-limited, sub-ms noise |
| fib-rt | 15.207 | 15.086 | +0.80% | 0% | 0% | 3% | ceiling-limited, noise |
| json-rt | 1.775 | 1.763 | +0.68% | 0% | 0% | 3% | ceiling-limited, noise |

"Static ceiling" = the deployed pipeline's own before→after insn reduction
(analyze_only's static scan under-reports const chains — see G5).

### G4 Per-pass attribution — trim executed

Final measurement (20-bundle corpus, 12,345 raw insns, every bundle counted via
the always-on report): full pipeline 12,345→12,233 (0.91%).

| pass | corpus attribution | verdict |
| --- | --- | --- |
| P2 cross-BB propagation | 92 insns (0.75%) | **kept** — prerequisite pass (P2 alone removes 0 insns corpus-wide; it only materializes through P3.1's folds) |
| P3.1 const binop fold | 112 insns (0.91%) | **kept** — the only removal pass; 92/299 insns (30.8%) on the compute-dense subset, matching G3 |
| P3.2 const strict_eq/neq | 0.00% | trimmed |
| P3.4 push+drop | 0.00% | trimmed — `resolve_labels` already removes these |
| P3.5 dup/swap/rot3 | 0.00% | trimmed — `resolve_labels` already removes these |
| P3.6 const condition | 0.00% | trimmed — consumes only P3.2's output |
| P4 threading | 0.00% | trimmed |
| P5 dead blocks | 0.00% | trimmed — marginal removal double-counted in fold stats |

Both kept passes sit marginally below the nominal 1% bar on the mixed corpus
(workload bundles such as wasm-edge-cases, 2,913 insns, dilute it); P2 qualifies
for the gate's prerequisite exemption, and trimming either pass zeroes the
pipeline (P2-only: 0 insns removed; P3.1-only: 20). G3 is the direct evidence
that the pair fires on its target population.

### G5 Ceiling explanation — PASS

- Wall-clock tracks **instruction removal**, not byte removal: arith-rt removes
  41.4% of insns but only 19.2% of code bytes, and the wall-clock gain (38.94%)
  matches the insn rate — the removed instructions are full dispatches of
  mul/add/shift in the hot loop, and interpreter dispatch is the loop's cost
  unit. This is the dispatch-amortization evidence the plan asked to archive:
  bytes are not the currency of interpreter cost; dispatches are.
- cascade-rt's +29.09% slightly exceeds its 24.0% insn ceiling (smaller loop
  footprint + noise); consistent.
- The prior "compute-dense 3-8%" model was conservative for const-chain-dense
  loops (measured 29-39%): such loops are pure dispatch+op-execution with nothing
  outside the loop, so insn elimination translates ~1:1.
- analyze_only caveat: its static scan reports 0.00% foldable on arith-rt because
  const chains need P2's dataflow to become adjacent pushes; the honest ceiling is
  the pipeline's own before→after report, which the G3 thresholds use.
- Non-compute workloads remain at 0% static ceiling → ~0 wall-clock by
  construction, consistent with the IO-bound prior.

### Step 9 (tier-2) — decided: not started (2026-08-23)

Decision: **do not start SCCP; the v1 pipeline is complete as built.** Rationale,
from the measured evidence rather than the §8 prior:

- The entire remaining pie is small: the full pipeline removes 0.91% of corpus
  insns; G4 already trimmed six passes that failed the 1% attribution bar.
- The §8 low-hanging list was evaluated against the same bar and rejected:
  - *local copy propagation* — P2 already is the constant form (get_loc→push);
    variable-to-variable copies remove one cheap dispatch each and need
    def-use chains across BBs, i.e. the P2 soundness surface for <P2's own 0.75%;
  - *literal `get_field`/`get_array_el` folding* — requires modeling object-literal
    property reads at compile time (prototype chain, accessors), crossing from
    stack dataflow into heap semantics; corpus field-access values are runtime
    arguments/JSON, not compile-time constants;
  - *cpool constant folding* — P3.1's lattice already folds the small-int domain
    where the compute-dense fixtures live; the residual (double/bigint/string
    constants) is not in the fixture population that G3 measures.
- G5's dispatch-amortization evidence shows the mechanism is instruction
  elimination in hot loops; the remaining hot-loop dispatches are loop-carried
  loads/compare/branch that stack dataflow cannot fold. Expected tier-2 value is
  a fraction of the 0.91% already banked, below the plan's own gate.

If the corpus changes (new compute-dense bundles) the pass switches
(`kPassP2`/`kPassP31`, attribution via report mode) make any single-pass
re-measurement a one-line harness change; nothing in the frozen CLI or format
contract needs to change to revisit this.

### Step 9 reversed — tier-2 built and adjudicated (2026-08-23)

The "not started" decision above was reversed by the user the same day, with
two explicit instructions: (1) follow the sablejs reference pipeline shape
(fixed-order PassManager: constant folding → constant branches → unreachable
code → **SSA SCCP** → **SSA copy propagation** → **LICM** → **GVN** → SSA DCE →
peephole rerun); (2) **build and measure the complete HIR/MIR/SSA flow before
any trimming decision** — pass trimming happens after measurement (G4), not
before. The full tier-2 plan is archived at
`/home/eroszhao/.claude/plans/swirling-cooking-rose.md`.

### Tier-2 as-built pipeline (commit 4465f36)

```
P0 parse → P1 decode/CFG (HIR role)
├─ direct layer: P11 copy-prop + dead-store materialization → P14 literal
│  get_field/get_array_el fold (both kept)
├─ SSI layer (P9 build → P10 SCCP → P11' copy prop → P12' SSA DCE →
│  P13' LICM (stack-neutral subset) → P14' form-(b) fold → P15 slot-read CSE)
│  — naming-only SSI: slot versions + join-named stack values, never changes
│  the stack layout, so every rewrite is stack-neutral (built and deleted)
└─ v1 tail: P2/P3.1/P6 fixpoint + verifier + per-pass report line
```

The SSI model names per-block slot versions and join-resident stack values
(Soot-Shimple style); no de-SSA pass is needed because naming never moves
values. Soundness gates carried over from P2's precedent: dynamic scope
(with/eval) disables the suite; captured slots are BOTTOM; `set_loc_uninitialized`
(TDZ marker) slots are excluded from store removal — the TDZ marker is an
observable effect that value-flow liveness cannot see.

### Tier-2 G1 Correctness — PASS (hard gate)

- 9/9 fixture differential bodies byte-identical vs source workers; 10/10
  ctest gates incl. the frozen RED `runtime_bytecode_compiler_round_trip`
  (bit-for-bit determinism over optimized output); 40k fuzzer runs under
  ASan/UBSan, no invariant violations.
- TDZ bug found and fixed during G1: P12' deleted `dst = 0` init stores whose
  value was only read after an in-loop rewrite, leaving the TDZ marker behind
  so the first `put_loc_check` threw ("dst is not initialized"). Fix: any slot
  with a `set_loc_uninitialized` anywhere in the function is excluded from
  store removal entirely (commit 4465f36).
- ver_uses vs v_uses kept distinct throughout (slot-version space vs stack-value
  space); a debug disassembler in /tmp misdecoded 1-byte loc8 operands as
  varints — tool-only, no optimizer impact.

### Tier-2 G2 No regression — PASS (hard gate)

- Zero vendor/runtime/format changes by construction, as in v1; only code
  blobs of standard BC_VERSION 26 output are rewritten; build-identity and
  attestation matrix green on the final tree.
- Non-target fixtures byte-identical (fib/string/json/matrix/sieve at full
  config; matrix differs from raw only inside noise). A -19.74% cse-loop-rt
  reading was traced to machine interference (a leftover bisect worker at
  98.7% CPU); after kill, interleaved A/B showed parity, and the final-config
  run shows +0.61%.

### Tier-2 G3 Effectiveness — PASS (go/no-go)

Final run `bench/results/exec-throughput-20260823T140906` (commit 8078d04 —
post-G4-trim deployed pipeline, Release, taskset 0-3, 1 warmup + 5 rounds,
median; manifest + sha256 recorded). Threshold = max(3%, 50% × measured static
ceiling from the compiler report):

| fixture | raw ms | opt ms | G3 | static insns | ceiling | threshold | verdict |
| --- | ---: | ---: | ---: | --- | --- | --- | --- |
| arith-rt | 47.083 | 28.103 | **+40.31%** | 145→85 (41.4%) | 41.4% | 20.7% | PASS 1.9× |
| cascade-rt | 16.605 | 11.409 | **+31.29%** | 100→76 (24.0%) | 24.0% | 12.0% | PASS 2.6× |
| prop-hoist-rt | 6.005 | 3.498 | **+41.75%** | 53→46 (13.2%) | 13.2% | 6.6% | PASS 6.3× |
| copy-chain-rt | 7.754 | 6.723 | **+13.30%** | 48→44 (8.3%) | 8.3% | 4.2% | PASS 3.2× |
| prop-loop-rt | 33.705 | 31.194 | **+7.45%** | 53→46 (13.2%) | 13.2% | 6.6% | PASS 1.1× |
| matrix-rt | 4.666 | 4.720 | -1.16% | 0% | 0% | 3% | ceiling-limited, noise |
| sieve-rt | 25.677 | 25.936 | -1.01% | 0% | 0% | 3% | ceiling-limited, noise |
| string-rt | 0.457 | 0.448 | +1.97% | 0% | 0% | 3% | ceiling-limited, sub-ms noise |
| fib-rt | 15.726 | 15.434 | +1.86% | 0% | 0% | 3% | ceiling-limited, noise |
| json-rt | 1.799 | 1.791 | +0.44% | 0% | 0% | 3% | ceiling-limited, noise |
| branch-const-rt | 4.375 | 4.200 | +4.00% | 0% | 0% | 3% | ceiling-limited, noise |
| cse-loop-rt | 7.547 | 7.501 | +0.61% | 0% | 0% | 3% | ceiling-limited, noise |
| licm-rt | 4.690 | 4.596 | +2.00% | 0% | 0% | 3% | ceiling-limited, noise |

The five moving fixtures all clear their thresholds. licm-rt's 0% ceiling is
itself the tier-2 headline: the fixture was built as the P13' LICM anchor, and
the pass hoisted nothing (see G4). The pre-trim full-config run
(`exec-throughput-20260823T135432`, commit 4465f36) is equivalent on every
moving fixture except sieve-rt (±2 insns, noise-band).

### Tier-2 G4 Per-pass attribution — trim executed

Final measurement (26-bundle corpus — the original 20 plus prop-loop-rt,
prop-hoist-rt, copy-chain-rt, branch-const-rt, cse-loop-rt, licm-rt — 12,645
raw insns; every bundle counted via the always-on report, per-pass via the
pass-switch API):

| mask | corpus insns | attribution |
| --- | ---: | --- |
| raw | 12,645 | — |
| full (0xFFF, v1 + tier-2) | 12,513 | 1.04% total |
| v1-only + direct (0x00F = P2\|P31\|P11\|P14) | 12,515 | 1.03% |
| SSI suite net | — | **2 insns (0.016%)** |

| pass | corpus attribution | verdict |
| --- | --- | --- |
| P11 copy-prop + dead-store materialization | 4 insns (0.032%), copy-chain-rt only | **kept** — direct layer; no substitute; drives copy-chain's G3 |
| P14 literal get_field fold | 0 net alone (P14' is a byte-identical substitute) | **kept** — the pair's joint effect is 14 insns / 50 bytes on prop-loop + prop-hoist; P14 carries it in the deployed pipeline; trimming the pair zeroes both G3 wins (P3.1-precedent) |
| P10 SCCP | 2 insns (0.016%), sieve-rt only | trimmed — 22 of its arith-rt folds duplicate P2's (identical 145→85 with P10 off) |
| P9 SSI construction | prerequisite of P10; materializes 0.016% | trimmed with the suite |
| P11' SSA copy prop | 0.00% | trimmed |
| P12' SSA DCE | 0.00% — fires nowhere in the corpus; its only unique behavior is the synthetic p2-crossbb dead store, which the golden now keeps | trimmed |
| P13' LICM | 0.00% — **zero hoists even on the licm-rt anchor fixture** | trimmed — the stack-neutral subset is empty in practice (plan's risk table named this outcome) |
| P14' form-(b) fold | 0.00% net — byte-identical substitute of P14 (both 53→46, md5 equal) | trimmed — carries no unique effect in production (P14 on) |
| P15 slot-read CSE | 0.00% insns; -2 bytes on cse-loop-rt | trimmed |

Both numbers the plan asked for are reported: static corpus attribution for
the static classes, and the dynamic share for the pure-dynamic classes
(P13'/P14') — which is 0 by construction, since with the substitute pass
enabled (P14) or with nothing hoisting (P13'), the output is byte-identical
and the dynamic instruction stream is unchanged.

Execution: commit 8078d04 deleted the 1,696-line SSI suite (P9-P15' incl.
their section) from the tree, trimmed `PassFlags` to
`kPassAll = P2|P31|P11|P14`, and reverted the p2-crossbb golden to
`bb cf bc 28` (the dead store x=1 stays — removing it was P12's unique
synthetic-buffer effect). The full suite remains in git history (4465f36) for
re-measurement on any changed corpus; the pass-switch API keeps the four
deployed passes individually measurable.

### Tier-2 G5 Ceiling explanation — PASS

- Opcode-frequency histograms (`bench/tools/ophist.py`, raw vs optimized,
  final config; module bootstrap excluded — constant 9 insns both states):

  | fixture | raw | opt | removed | residue composition |
  | --- | --- | --- | --- | --- |
  | arith-rt | 136 | 76 | all 21 arith ops (add/mul/shl/sar/and/xor/sub/or) → 17 push_i32 | 32 dead-store scaffolding insns (18 set_loc_uninitialized + 14 put_loc8, TDZ-guarded), 7 loop-carried (get_loc/put_loc/dup/drop/add), 4 loop control, 2 calls |
  | cascade-rt | 91 | 67 | add ×6 → push_i32; 17 push_i32 total | 7 set_loc_uninitialized, 6 if_false8 (branch chain kept), 3 strict_eq, loop control |
  | prop-hoist-rt | 44 | 37 | get_field ×3 (P14), add ×3 (P3.1 cascade) | 3 define_field (object rebuild per iteration), loop control |

  The wall-clock ↔ static gap: in optimized arith-rt the loop body is 76
  insns of which 32 are dead init stores (every `let x = <folded const>`
  still emits set_loc_uninitialized + push + put_loc8). Removing them needs
  store-removal (P12'), which the G4 trim deleted for 0.016% corpus effect;
  the dispatch floor after folding is the ~13-insn loop-carried skeleton
  (get_loc/put_loc/dup/drop/add + lt/if_false/goto16/post_inc), not the
  arithmetic. The remaining gap is therefore *not* dispatch amortization
  but dead-store scaffolding on a fold-shaped corpus — the ceiling the
  histogram was built to locate. **Resolved by tier-2b (below): P16
  deleted the scaffolding (measured ~46 insns, not the 32 estimated
  here), taking arith-rt 76 → 26 insns; the surviving residue is the
  loop-carried skeleton this paragraph predicted.**
- prop-hoist-rt removes 13.2% of insns but gains +41.75% wall-clock: the
  removed instructions are `get_field` property lookups on hoisted literals —
  the most expensive dispatch in the loop — so elimination translates
  >>1:1 there (v1's arith/cascade 1:1 reading holds for cheap ops; P14's
  removals are expensive ops).
- P14's 3 folds on prop-hoist are the whole fixture's story: without the
  P14/P14' pair the output is byte-identical to raw (53 insns), and the
  downstream 2 P3.1 folds exist only because P14's rewrite made the binop
  operands adjacent constants — a fold cascade, not independent work.
- The SSI suite's 0.016% net confirms the §8 prior quantitatively: SCCP
  duplicates the P2 lattice on this corpus, GVN's materialized form is dup
  (byte-neutral), and LICM's stack-neutral subset is empty. The "direction
  question" the tier-2 plan was built to answer is answered with measured
  data: **on this corpus, the remaining value is the direct layer (P11/P14),
  and the SSA layer adds nothing measurable** — a corpus of compute-dense
  const-chain loops and literal-property loops, not register-starved
  algorithms. New corpus classes (e.g. accessor-heavy loops, inlined
  collection traversals) would re-open the measurement via the API switches
  and git-history suite, not by uncommitted experimentation.

### Deployed pipeline (post tier-2 G4, commit 8078d04)

P0 parse → P1 decode/CFG → P11 → P14 → P2/P3.1/P6 fixpoint (≤16 rounds) →
P7 pc2line remap → P8 splice/checksum → verifier. `kPassAll` = P2|P31|P11|P14;
the report line covers P2/P3.1/P11/P14 folds + shrinks.

### Tier-2b: P16 TDZ-sound dead-store elimination (2026-08-23, commit 40c3d8a)

The tier-2 G5 exit report identified the remaining ceiling on the corpus:
the marker/producer/store scaffolding every `let x = <folded const>` emits
(`set_loc_uninitialized` + push + `put_loc8`). The tier-2 G4 had trimmed
P12' (SSA DCE) for 0.016% corpus effect, but its whole-slot TDZ guard made
it structurally unable to delete those stores. Tier-2b rebuilt the concept
as P16 with precise backward slot liveness (`docs/plans/
tier-2b-tdz-sound-dce.md`).

Design: backward live-slot analysis over the CFG (worklist, live_in
propagated across edges); a store (put_loc/put_loc8/put_loc0-3) whose
slot is dead after it is removed together with a pure-push producer;
`set_loc_uninitialized` markers are removed when overwritten before any
read. TDZ soundness comes from the liveness itself — the marker writes the
JS_UNINITIALIZED slot value, so plain value liveness is sound — plus three
conservative exclusions: check-form stores (`put_loc_check`/
`put_loc_check_init`) are never candidates, read+write ops (check stores,
inc/dec/add_loc/close_loc) keep the slot live (the read wins over the
write's kill), and captured slots (vardef flag 0x40) plus eval/special-object
barriers keep everything. Deleted set is only ever stores, markers, and
pure producers — never a read, never a check-form store, never an aliased
slot, never a captured slot.

#### G1 Correctness — PASS (hard gate)

ctest `bytecode|attestation|build_identity|host_managed_trusted_bytecode`
10/10; RED round-trip and `bytecode_opt_differential` green; 40k fuzz runs
(ASan+UBSan, `fuzz_bytecode_opt` entry + corpus seeds) clean; deterministic
(triple-run byte-identical). 9 new golden tests plus the updated p2-crossbb
golden cover: dead triple (marker+store+producer), dead store without
marker, read-after-store keep, marker-read keep, loop-carried liveness
(store kept across the backedge), captured-slot keep, side-effect producer
keep, eval barrier keep, and check-form store keep. (Full-suite ctest has
4 pre-existing environmental failures outside the gate scope: the docs
audit expects the tier-2b plan link — fixed in this commit — WPT is not
configured, and the package smoke flags a miniconda `libcrypto.so.3`
interception that predates P16; none touched by this work.)

#### G2 No regression — PASS (hard gate)

Non-triggering fib-rt is byte-identical to the 20260823T140906 opt output
(same sha256). The other seven pre-P16 byte-identical (0%-ceiling) fixtures
(matrix/sieve/string/json/branch-const/cse-loop/licm-rt) turned out to
already contain dead stores — 0% ceiling meant "no foldable code", not "no
dead stores", since v1 had no store-removal pass. Their P16 deletions
(3-16 insns each, all markers/dead stores) are differential-clean and
covered by the fuzzer; per plan §7 the exception is reported separately
(evidence in `bench/results/p16-evidence/g2-nontrigger-sha256.md`).

#### G3 Effectiveness — PASS (go/no-go)

exec-throughput (13 fixtures, taskset 0-3, median of 5, Release 40c3d8a
vs pre-P16 8078d04; three runs — T152705, T153323, T153344 — because
the machine carried concurrent sessions: a test262 conformance run at
125% CPU during the first, a sablejs Crypto benchmark at ~115% CPU
during the retests):

| fixture | pre-P16 | run 1 | run 2 | run 3 |
| --- | ---: | ---: | ---: | ---: |
| arith-rt | +40.31% | **+84.09%** | +83.80% | +84.70% |
| cascade-rt | +31.29% | +56.56% | +56.17% | +57.22% |
| prop-hoist-rt | +41.75% | +42.77% | +39.86% | +40.71% |
| copy-chain-rt | +13.30% | +21.39% | +26.60% | +21.07% |
| prop-loop-rt | +7.45% | +18.54% | +18.67% | +17.26% |
| branch-const-rt | +4.00% | +9.83% | -0.23% | -1.82% |
| matrix-rt | -1.16% | +5.95% | -4.59% | +1.84% |
| cse-loop-rt | +0.61% | +2.36% | +6.77% | -4.97% |
| sieve-rt | -1.01% | +0.05% | -0.58% | +0.70% |
| fib-rt | +1.86% | +0.05% | -7.49% | +0.25% |
| json-rt | +0.44% | -0.92% | -18.03% | +8.51% |
| string-rt | +1.97% | -2.46% | +4.37% | -3.34% |
| licm-rt | +2.00% | -2.52% | -16.21% | -2.31% |

Gate: arith-rt ≥ +43.31% → **+83.8…84.7% across all three runs,
1.94× the gate** (the plan's +55-70% expectation was itself exceeded:
marker deletion is not loop-amortized — every deleted entry instruction
is paid once per function call, so the win lands on every invocation,
unlike fold wins). Gain vs pre-P16 is +43.5…44.4pp, far above the +3pp
trim threshold. The millisecond-scale fixtures (string-rt/json-rt/
licm-rt) swing within noise across runs as the concurrent load varies;
their dips are single samples under the sablejs load, contradicted by
the other runs, and structurally impossible on a strictly-smaller
output (licm-rt 96→84 code bytes — P16 deletes only dead markers
there). No fixture shows a consistent >2% regression, so the trim
condition does not trigger.

#### G4 Per-pass attribution — PASS (dynamic criterion)

Switch matrix (kPassAll vs kPassAll∖P16, per-fixture, same raw inputs):

| fixture | P16 insn delta | P16 byte delta |
| --- | ---: | ---: |
| arith-rt | -50 | -166 |
| cascade-rt | -17 | -56 |
| matrix-rt | -16 | -48 |
| sieve-rt | -7 | -21 |
| json-rt | -5 | -15 |
| copy-chain-rt | -5 | -15 |
| string-rt | -4 | -12 |
| licm-rt | -4 | -12 |
| branch-const-rt | -3 | -9 |
| prop-loop-rt | -3 | -9 |
| prop-hoist-rt | -3 | -9 |
| cse-loop-rt | -3 | -9 |
| fib-rt | 0 | 0 |

Corpus static attribution (13-fixture bench corpus): 120/1076 = 11.15%.
The 26-bundle tests/fixtures corpus (15 raw bundles, 12,233 insns):
88 insns deleted = 0.72% — within the plan's sub-1% static expectation
(fixtures carry a few dead markers; host-single-worker, wasm-edge-cases,
and the seed bundles are the main contributors). The bench corpus is
marker-dense by construction, and the plan's adjudication rule for P16 is
the dynamic one: every deleted instruction is paid once per request on
the hot path (~300k executions per request, plan §1), so the dynamic
share is the full 50/145 = 34.5% of arith-rt's instructions — ≫ the 1%
dynamic gate. **Keep: P16 stays in `kPassAll`.** No trim condition
triggered (gain +43.78pp ≥ +3pp; no fixture shows a verified >2%
regression).

#### G5 Ceiling explanation — PASS (revised)

arith-rt optimized loop: 76 insns (pre-P16) → 26 insns (post-P16),
224 → 58 code bytes. The deleted set is exactly the scaffolding the tier-2
G5 report predicted: 18× `set_loc_uninitialized` + 17× `push_i32` + 14×
`put_loc8` ≈ 46 insns — the earlier "32 dead-store scaffolding insns"
estimate is corrected upward to the measured ~46, and the plan's
"≈44-46" prediction is confirmed. The 26 surviving insns
(get_loc_check ×4, put_loc_check ×2, dup/drop/get_var ×2, lt/if_false8/
add/post_inc/goto8/call1/call_constructor/return, push_0/push_i32/put_loc0/
put_loc1) are the true dispatch floor the v1 histogram misattributed. The
wall-clock ↔ static gap is closed: the removed scaffolding was paid once
per call, and its elimination is what the +43.78pp arith-rt gain measures.
Full histograms and per-fixture pre/post insn counts archived in
`bench/results/p16-evidence/`.

### Deployed pipeline (post tier-2b G4, commit 40c3d8a)

P0 parse → P1 decode/CFG → fixpoint (≤16 rounds, per round: P11/P14 →
P16 → P2 → P3.1, then re-shorten) → P7 pc2line remap → P8
splice/checksum → verifier. P16 runs inside the fixpoint after the
tier-2 direct passes so their dead-store materializations are visible;
it only deletes instructions (never rewrites), so it cannot feed the
lattice and termination is unchanged. `kPassAll` = P2|P31|P11|P14|P16;
the report line covers P2/P3.1/P11/P14/P16 folds + shrinks.
