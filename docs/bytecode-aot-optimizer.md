# Bytecode AOT Optimizer

This document is the maintained contract for Capsid's deployed compile-time
bytecode optimizer. Historical implementation plans, intermediate pipelines,
and rejected passes live in git history; current benchmark samples live in
[Performance Evidence](performance-benchmarks.md).

## 1. Current Contract

`capsid-bytecode-compile` serializes standard quickjs-ng bytecode, runs this
optimizer, and only then computes the bundle digest and attestation. The
optimizer:

- accepts pinned quickjs-ng `BC_VERSION 26` bytecode;
- rewrites code blobs without changing the VM, wire format, atom table, cpool,
  module records, source blob, or compatibility identity;
- is deterministic and idempotent for identical input and pass mask;
- fails closed on every unsupported or malformed input, producing no output;
- exposes pass switches only through the internal API for attribution; the
  production compiler always uses `kPassAll`.

It is not a JIT, a partial evaluator, or a general SSA optimizer. Product
output remains BC26. The measured-negative R0 emitter is removed, and the
reviewed ext34 backend exists only in an explicit compile-gated measurement
build; default builds contain no live ext34 ids or handlers. Profiling,
CFG+SSA, IC, fusion decisions, and the next implementation gate are maintained
in [QuickJS Optimization](quickjs-optimization.md).

Implementation ownership is intentionally separate from command-line tooling:

- `src/bytecode_optimizer/` contains the product library and its internal API;
- `tools/capsid-bytecode-compile.cc` is only a consumer and CLI boundary;
- `tests/bytecode_optimizer/` contains unit and end-to-end differential gates;
- `tests/fuzz/fuzz_bytecode_opt.cc` and `tests/fuzz/corpus/bytecode_opt/` remain
  in the repository-wide fuzz harness.

## 2. Wire-Format Facts

The parser mirrors the pinned quickjs-ng commit
`bf8988fc401e737f9946cd10a3463b48aab0fd7e`:

- header: `u8 BC_VERSION` (=26), little-endian `u32` checksum over `buf+5`,
  atom count, then the atom table;
- function: tag, `u16` flags, strict byte, function-name atom, eight LEB128
  counts, vardefs, closure vars, cpool, code blob, then optional debug data;
- module: module tables followed by its top-level function record;
- nested `BC_TAG_FUNCTION_BYTECODE` values are parsed recursively from cpool;
- jump target = operand start + signed relative offset;
- `OP_catch` targets and post-`OP_gosub` instructions are CFG roots;
- serialized ordinary opcodes occupy 0..251; values >=252 are invalid in
  BC26. The default runtime reserves 252 for `OP_ext` with no live ids and uses
  253 only as a non-serialized in-memory field-IC opcode; an explicit ext34
  measurement build assigns ids 2/3 and records that ABI difference in its
  compatibility identity;
- pc2line is decoded, mapped through old-to-new instruction offsets, and
  re-encoded with quickjs-ng's cumulative delta format.

Every BCTag, LEB128, operand, span, recursion depth, instruction count, jump
target, and pc2line boundary is bounds checked. Unknown values abort instead of
falling back to passthrough.

## 3. Deployed Pipeline

```text
P0 parse and validate the recursive bundle
P1 decode each function and construct CFG/stack-height invariants
fixpoint, at most 16 rounds:
  P11 copy propagation
  P14 literal property folding
  P16 TDZ-sound dead-store elimination
  P2  cross-basic-block constant propagation
P3.1 integer constant folding
P17 proven-TDZ check removal
P18 redundant to_propkey removal
  P6  compact and select shortest encodings
P7 remap pc2line
P8 rebuild records and checksum
final recursive parse + bytecode verifier
```

`kPassAll = P2 | P31 | P11 | P14 | P16 | P17 | P18` (`0x7f`). A round that changes the
program deletes at least one instruction; hitting the iteration cap is an
error. The output remains ordinary BC26 and is loaded by the unmodified VM.

The adjacent `ir/` implementation is an analyze-only successor foundation.
Its SSA rules conservatively model numeric overflow, and its region census
only considers same-block runs of two to eight matching instructions. The
profiled API weights a region by the minimum exact-site execution count of all
members; missing evidence gives zero weight rather than falling back to a
static guess. No region is lowered or emitted today. This boundary prevents a
single expensive opcode—the failure mode of R0—from being mislabeled as
multi-instruction fusion.

### Pass definitions

| Pass | Rewrite | Conservative boundary |
| --- | --- | --- |
| P2 | Forward per-slot lattice; replace a proven local read with an equivalent constant push | Calls, getters/proxies, iterators, suspension, dynamic scope, globals and other user-code barriers drop facts; stack facts do not cross unknown joins |
| P3.1 | Fold adjacent int32 pushes plus supported arithmetic/bitwise operations | Skip overflow, non-integer division and every result not representable by an existing immediate opcode |
| P11 | In straight-line functions, track `get_loc s; put_loc t` aliases, rename later reads, and remove an unread stack-neutral load/store pair | No control-flow edges, later destination writes, captured slots, check-form stores, dynamic scope, frame objects or escaping references |
| P14 | Track literal object/array construction bound to a local and fold proven own data-property reads | Exact cpool/tag parsing; stop at aliasing, mutation, accessor/proxy, opaque calls or uncertain construction |
| P16 | Backward slot liveness; remove a dead `set_loc_uninitialized` marker or a dead store together with its pure producer | Captured slots, check-form stores, jump-targeted store pairs, read-write mutations, eval/with/special objects and unanalyzable environments remain untouched |
| P6 | Re-encode immediates, slots, calls and branches with the shortest valid opcode | Rebuild offsets globally and verify every target after compaction |

P16 is TDZ-sound because `set_loc_uninitialized` writes the observable
`JS_UNINITIALIZED` slot value: ordinary value liveness decides whether that
write can be observed. For read+write instructions, gen wins over kill.
`put_loc_check` and `put_loc_check_init` are reads of the old TDZ state and are
never deletion candidates.

## 4. Output and Soundness Invariants

- Cpool entries and atom indexes are never added, removed, or reordered.
- Every deletion is paired so stack effect remains unchanged.
- Captured locals are identified from vardef flag `0x40` and excluded where an
  invisible closure access could invalidate the proof.
- Dynamic scope, suspension, user coercion, property access and callback
  barriers are classified explicitly; unknown operations lose facts.
- All label targets land on emitted instruction boundaries.
- Per-PC stack heights agree at joins, never underflow, and do not exceed the
  serialized `stack_size`.
- pc2line entries remain on instruction boundaries and preserve the source
  location selected for a replacement.
- The atom table, module metadata, vardefs, closures, cpool and source text are
  copied byte-for-byte.
- The final buffer is reparsed and verified before the compiler can attest it.

Bad version/checksum, malformed LEB128, unsupported tag/opcode, out-of-range
operand, invalid stack effect, bad jump, invalid pc2line, depth/count limit, or
non-convergence makes `optimize()` return false with `out` untouched.

## 5. Verification Gates

The maintained correctness gate consists of:

- synthetic reader and golden-byte tests for tags, truncation, checksum,
  LEB128, CFG roots, each deployed pass, compaction and pc2line;
- compile-only → serialize → optimize → deserialize → execute round trips,
  including exceptions and stack-trace locations;
- the frozen `runtime_bytecode_compiler_round_trip` RED test, including
  deterministic output;
- source-worker versus optimized-bytecode-worker differential tests over all
  fixtures;
- ASan+UBSan `fuzz-bytecode-opt`, seeded with deterministically compiled
  bundles; output must parse, verify and remain stable under a second optimize;
- build identity, attestation and managed trusted-bytecode integration tests.

Performance attribution always compares optimized bytecode with unoptimized
bytecode. Source execution is reported separately because it also measures the
parse/compile path.

## 6. Final Pass Decisions and Evidence

The deployed set is the result of three measured rounds of implementation and
trimming; only the final decisions are maintained here.

| Decision | Evidence | Result |
| --- | --- | --- |
| Keep P2/P3.1 | Constant-chain fixtures removed 24%..41% of instructions and cleared their pre-registered gates | Deployed |
| Keep P11/P14 | P11 uniquely moved copy-chain; P14 removed expensive literal `get_field` operations and enabled downstream folds | Deployed |
| Remove v1 residue passes | strict-equality, push/drop, stack cleanup, constant branch, threading and dead-block passes each attributed <1%; quickjs-ng already removed most patterns | Deleted |
| Remove P9-P15' SSI suite | Full suite added 2 instructions of net reduction over the direct layer on the 12,645-instruction corpus (0.016%); LICM hoisted nothing even on its anchor fixture | Deleted in `8078d04` |
| Keep P16 | 88 instructions removed from the 12,233-instruction bundle corpus (0.72%); 120/1,076 on the deliberately marker-dense benchmark corpus; correctness and dynamic gate passed | Deployed in `40c3d8a` |

P16's synthetic `arith-rt` loop moved from 76 to 26 optimized instructions;
the three recorded runs measured optimized-vs-raw at +83.8%..+84.7%. This is a
mechanism test for entry scaffolding, not a broad product prediction. The real
bundle corpus's 0.72% static attribution is the relevant breadth signal.

Correctness evidence for the final tree: 10/10 targeted ctest groups, full
round-trip and fixture differential coverage, 40,000 ASan/UBSan fuzz cases,
and byte-for-byte determinism. Raw P16 histograms, switch matrices and samples
are stored under `bench/results/p16-evidence/`; consolidated performance tables
are in [Performance Evidence](performance-benchmarks.md).

## 7. Interpretation and Next Boundary

CFG construction, join handling, stack verification, forward propagation, and
backward liveness remain useful and deployed. What was removed is the generic
SSI/SCCP/GVN/LICM layer: it added only 2 removals over the direct passes on a
12,645-instruction corpus (0.016%), and LICM moved nothing on its anchor
fixture.

That result is specific to lowering back into unchanged BC26. quickjs-ng has
already removed many shallow stack-bytecode redundancies; the expensive
remainder depends on runtime tags, property shapes, callees, coercion, and
exceptional paths. Further BC26 proposals therefore need an analyze-only
ceiling and a new corpus class. New opcodes, runtime specialization, and
multi-instruction lowering follow the gates in
[QuickJS Optimization](quickjs-optimization.md).
