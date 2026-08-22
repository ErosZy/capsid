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
- new `bench/compile-cold-start-apps.sh`, `bench/exec-throughput.sh`;
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
  with operands, post-gosub roots), every peephole input→output, cross-BB
  propagation and with/eval gates, unreachable blocks (catch targets and
  ret-only-reachable blocks survive), pc2line remap, double-run determinism, and
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
| 0. Ceiling analysis + analyze-only | done | arith-rt 68.1% / cascade 48.1% static insn reduction ceiling measured on real bundles |
| 1. Reader skeleton + validation | done | RED round-trip green; fail-closed matrix on truncated/bad checksum inputs |
| 2. CFG + dataflow + verifier | done | per-pass verifier; stack-height invariant; with/eval gates |
| 3. Core peepholes + pc2line + emission | done | full round-trip green (0.31 s) |
| 4. Threading + dead blocks + re-shrink | done | threads/dead/shrink stats in report mode |
| 5. Cross-BB constant lattice + scope gates | done | arith-rt 15616→4980 insns (-68.1%), 22623→9049 bytes (-60.0%) |
| 6. Fixpoint + report mode + pass switches | done (2026-08-23) | see attribution below; corpus 98/98 pass |
| 7. Differential + ctest + fuzzer | in progress | — |
| 8. Benchmarks + G1-G5 verdict + docs | pending | — |
| 9. Tier-2 decision | pending | — |

### Step 6 G4 attribution (complementary: full set minus the pass; baseline = unoptimized)

| Pass | arith-rt (15616 insns) | cascade (160 insns) | 1% gate |
| --- | --- | --- | --- |
| P3.1 const binop folding | 10636 (68.1%) | 75 (46.9%) | pass |
| P3.2 const strict_eq/neq | 4630 (29.6%) | 63 (39.4%) | pass |
| P3.6 const cond-jump folding | 3830 (24.5%) | 53 (33.1%) | pass |
| P5 dead-block elimination | 3294 (21.1%) | 40 (25.0%) | pass |
| P4 jump threading | 563 (3.6%) | 15 (9.4%) | pass |
| P2 cross-BB propagation (prerequisite) | 236 (1.5%) | 10 (6.3%) | pass |
| P3.4 push+drop removal | 0 on all 98 corpus fixtures | — | **trim candidate (G4)** |
| P3.5 dup/swap/rot3 cancellation | 0 on all 98 corpus fixtures | — | **trim candidate (G4)** |

Pass interaction, confirmed by the matrix: P3.2/P3.6 are silent standalone because
their constant inputs are produced by P3.1/P2 (the `2+3===5` pattern only folds
after P3.1 turns `add` into `push_5`); disabling P3.1 silences both downstream
passes. P3.4/P3.5 never fire on real emitter output — quickjs-ng's own
`resolve_labels` already eliminates push/drop and dup/swap/rot3 pairs at emission
— so they are dead code on the corpus and are candidates for trimming under G4
(decision recorded at Step 8).

Step 6 verification: 98-fixture corpus — optimize success 98/98, cross-process
determinism (byte-identical) 98/98, same-mask idempotence 98/98, exec-equivalence
69/69 (stdout fixtures) + 29/29 (empty-body fixtures, stdout+stderr identical);
mask matrix (10 masks × arith-rt + cascade) exec + idempotence 20/20.
