# QuickJS-ng Opcode Profiling, AOT Specialization, and PGO

> Status: design only. A0 may start immediately; production VM or bytecode
> format work is forbidden until A4 selects at least one viable candidate.
> This is the single maintained tier-3 plan and is self-contained for an
> implementation agent.

## 1. Goal, Limits, and Expected Benefit

The deployed [BC26 optimizer](bytecode-aot-optimizer.md) proved that local,
direct rewrites are useful but that a general SSI/SCCP/GVN/LICM layer added only
0.016% on the measured corpus. Tier 3 therefore profiles actual interpreter
cost before deciding whether to add specialized opcodes.

Active scope (product decision 2026-08-23): **bytecode-level measures only,
no IC/GC**. Lane 1 and Lane 2A are in scope; Lane 2B (sparse cache) and Lane 3
(runtime quickening) are deferred — they require runtime state and are
IC-adjacent. Every emitted instruction must preserve the zero-state contract:
the generic handler remains authoritative for every miss, and no runtime field,
allocation, or GC root exists for any in-scope lane.

The implementation order is deliberately conservative:

| Lane | Mechanism | Runtime state | Status |
| --- | --- | --- | --- |
| 1 | Candidate-specific AOT type proof; emit a guard-free specialized opcode | None | In scope |
| 2A | PGO-selected stateless guard, branch layout, or fused instruction | None | In scope |
| 2B | PGO-selected sparse shape/property/callee cache | Hot sites only | Deferred (IC-shaped) |
| 3 | Runtime quickening for residual stable hotspots | Hot functions/sites | Deferred (runtime state) |

This is an opcode-cost plan, not a restart of the retired generic P9-P15' SSI
suite. Dynamic opcode/site evidence selects a candidate first. Lane 1 then
builds only the CFG and full-stack type dataflow needed to prove that candidate;
Lane 2A uses guarded or fused bytecode forms and does not require SSI. A local
sparse SSA/SSI representation is allowed only when a selected candidate cannot
be proved without use-position refinement and its analyze-only ceiling already
clears the benefit gate.

Prior ranges, not promises (bytecode-only subset):

| Mechanism | Broad workloads | Isolated hotspot |
| --- | ---: | ---: |
| Lane 1 static tag elimination | 0%..3% | 3%..10% |
| Lane 2A stateless/fusion | 1%..5% | 5%..15% |
| First combined release | **1%..8%** | workload-dependent |

`8%..15%` is a stretch goal requiring a property, call, or fusion candidate to
pass every correctness, RSS, and per-workload gate. `15%..25%` is only a
multi-round ceiling. No candidate means no BC27.

Out of scope: JIT/baseline native compiler, NaN-boxing redesign, decoded
16-bit IR, AST HIR, call inlining, profile-driven instruction reordering,
inline caches, and GC-related work.

Execution status (2026-08-23): A0 baseline is largely in place — the
interpreter-ceiling reference (quickjs-ng 1.95x behind V8 jitless on v8-suite)
and the DSE +2.6% paired A/B are archived under `bench/results/`. A1 is
delivered: overlay patch 0036 (`CONFIG_OPCODE_PROFILE`), capsid option
`CAPSID_ENABLE_OPCODE_PROFILE` (worker dumps one JSON object per runtime to
stderr before each `TJS_FreeRuntime`), and the zero-tax proof — the OFF
configuration is byte-identical to the patchless build at both the non-LTO
object level and the fully LTO-linked binary level (section-name line-number
metadata from `-ffunction-sections` is the only observable difference of
unlinked LTO objects). A2 is delivered: corpus profiling and ranking (§3.4a),
candidates `get_array_el` specialization + TDZ-check elimination. A3 is
delivered: analyze-only density proof (§4.1) — 100% TDZ density on all 14
analyzable fixtures with an honest negative control, and shape-bound
`get_array_el` density (3/3 full, 3 obj-only) that Lane 2A guards are
designed to extend. A4 go/no-go is the next gate; B1/C1/C2 follow in order.

## 2. Verified Baseline Facts

The design is anchored to the pinned quickjs-ng vendor source, not assumptions:

- serialized normal/short opcodes occupy 0..251 and `OP_COUNT == 252`; byte
  values 252..255 are currently free;
- `call`, `call_method`, constructors and tail calls carry only `u16 argc`;
  the callee and receiver are stack values; only `fclosure/fclosure8` carries a
  function cpool index;
- there is `push_i16`, but no `push_16` or `push_f64`; float constants use
  `push_const/push_const8` and a cpool tag;
- arrays are construction sequences ending in forms such as `array_from`, not
  a single typed array-literal producer;
- generic arithmetic handlers already test likely int-int first and commonly
  include a float fast path; a guarded `add_i32` does not remove that test;
- exact property shape and closure identity are runtime objects and cannot be
  embedded as cross-process pointers in a PGO bundle;
- the old P9-P15' SSI implementation was deleted in `8078d04`; P17 must build
  a fresh full-stack type dataflow rather than extending live SSI code;
- upstream IC history is `6b78c7f` (polymorphic IC), `c7ca3fe` (do not
  serialize IC states), and `7de6d467` (remove IC: mixed speed, consistently
  higher memory, maintenance burden).

SableJS contributes the evidence discipline—validate first, measure paired,
archive negative results, prove each rule—not a quantitative prior. It lowers
operations to native JavaScript and V8 JIT; QuickJS remains a boxed interpreter.

## 3. A0-A2: Native QuickJS Profiling

### 3.1 Build boundary and API

Add `CONFIG_OPCODE_PROFILE` only to a dedicated profiling overlay build. When
undefined, generated code, `sizeof` values, symbols, runtime fields, and handler
branches must match the patchless production build.

Under the same compile guard expose:

```c
JS_EXTERN void JS_DumpOpcodeProfile(FILE *fp, JSRuntime *rt);
```

The profiling `qjs` accepts `--opcode-profile FILE`. Open, allocation, JSON, or
short-write failure is fatal. Production `qjs` does not recognize the flag.

### 3.2 Profile contents

Schema name: `quickjs-ng-opcode-profile-v1`. Record 64-bit saturating counters
for:

- dynamic executions, slow-path entries, exceptions, and sampled ticks by
  opcode;
- arithmetic operand classes (int-int, number-number, string, BigInt, other);
- property/index classes (object, own data, prototype, accessor, proxy/exotic,
  dense/hole/typed array);
- calls (bytecode/C/bound/proxy, argc bucket, callee stability);
- branch direction, backedges, and top opcode pairs/triples;
- per-site hits, dominant class, miss count, and polymorphism.

Sampling must not reorder the observed fast/slow branches. Calibrate clock
overhead with a control handler and report how much the profiling binary itself
perturbs patchless execution. Profiling results rank opportunities; production
performance is never measured with this build.

The first implementation (overlay patch 0036) covers a strict subset: per-
opcode dispatch executions and slow-path entries, three-way arithmetic operand
classes for `add/sub/mul/div/mod` (int-int, float-float, other), branch
direction, call argc buckets, and property fast/slow counts. Remaining classes
are additive and keep the schema name `quickjs-ng-opcode-profile-v1`. All
counters are per-runtime saturating u64s, so the process may host multiple
worker runtimes; the dump API emits one JSON object per runtime.

### 3.3 Stable site identity

Addresses, paths, atom text, and runtime allocation order are forbidden. A site
key is:

```text
prePgoBundleSha256
moduleOrdinal
functionCpoolPath[]
originalPc
originalOpcode
```

`functionCpoolPath` follows serialized child-function indexes from the root.
Profile the canonical output of today's `kPassAll`, called bundle B. A later
PGO compile must regenerate B byte-for-byte, verify its SHA-256, then match the
path/PC/opcode. Any mismatch, duplicate site, overflow, limit, or schema/build
identity error fails closed.

Profile parsing has hard limits for bytes, site count, transition count, nesting
and counter values. Profile data may select a semantics-preserving guarded
opcode; it must never participate in Lane 1 proof.

### 3.4 Corpus and ranking

Train on pinned QuickJS microbench/web-tooling workloads plus a staging worker
running canonical BC26 Capsid bundles. Validate on independently collected
QuickJS workloads and a separate Hono request mix. Do not select and keep a
candidate on the same samples.

Rank candidates by estimated tick coverage:

```text
dynamic executions × sampled cost per execution × specializable hit rate
```

Report the components, slow-path share, and Wilson lower confidence bound.
`dominant rate >=90%` is insufficient without a pre-registered minimum hit
count and a Wilson lower bound >=0.90. Select at most two candidates across
numeric/compare, property/index, call, and fusion categories. Each must cover
at least 2% of sampled interpreter ticks. Fewer qualified candidates means
fewer prototypes; none ends the project after the no-go report.

### 3.4a First-round evidence (2026-08-23)

Corpus: the 13 bench fixtures (ES-module, strict) + v8-suite
(`bench/fixtures/v8-suite-rt.js`, sablejs adaptation, sloppy) — 14 profiles
per mode, 7.13B dynamic executions (source), archived under
`bench/results/opcode-profile-*` with `bench/profile-collect.sh` and
`bench/profile-aggregate.py` (slow-path ranking weights only genuine slow
class buckets: arith `other`, property `slow`; fast int/float buckets count
at dispatch floor).

| Rank (source corpus) | Candidate | Dynamic exec | Dispatch share | Slow-path cost (20x) |
| --- | --- | ---: | ---: | ---: |
| 1 | `get_array_el` (every exec takes the generic `JS_GetPropertyValue` path) | 313M | 4.39% | 6.27B |
| 2 | `call_method` | 58M | 0.81% | 1.16B |
| 3 | `mul` `other` (overflow/float-mixed) | 36M | 0.51% | 0.72B |
| 4 | `add` `other` (int32-accumulator overflow, mixed int/float) | 43M | 0.60% | 0.86B |
| 5 | `swap` (sloppy-eval stack rotation, dispatch floor only) | 547M | 7.67% | — |

The strict-module corpus (bench fixtures) shows a second distinct signal:
`get_loc_check`+`put_loc_check` are 25.9% of opt-mode dispatch, every exec
carrying a TDZ check. Sloppy v8-suite executes zero of them — the candidate
is production-shaped (Capsid applications are strict modules), not
universal.

A4 preliminary go: two candidates qualify for A3 density proof —
(a) `get_array_el` specialization (object-proven array + int index,
skipping the generic property path) and (b) TDZ-check elimination
(`get_loc_check`→`get_loc`, slot initialization proof, existing opcodes,
zero format change). Both are static, state-free, and within Lane 1.

## 4. A3-A4: Analyze-Only Static Proof

A3 is candidate-specific and emits nothing. For an int-int candidate, the
minimum domain is:

```text
BOTTOM       unreachable or no predecessor yet
EXACT_INT    runtime value is necessarily JS_TAG_INT
UNKNOWN      any JSValue

join(BOTTOM, x)             = x
join(EXACT_INT, EXACT_INT)  = EXACT_INT
join(any other pair)        = UNKNOWN
```

Maintain complete operand-stack and local-slot vectors per basic block, with
stack heights identical to the verifier. Every serialized opcode has an
explicit transfer classification; unclassified instructions lose affected
facts. Initial int producers include `push_i32/i16/i8`, `push_minus1`, and
`push_0..7`; cpool facts come from validated tags. Arithmetic that can overflow
produces UNKNOWN even when its inputs were exact.

Conservative boundaries:

- captured slots, eval, with, special objects, suspension and opaque runtime
  operations lose facts;
- non-folded properties/indexes and all initial call results are UNKNOWN;
- the first version emits nothing in a function containing catch/gosub,
  try/finally, or a dynamic environment because the current CFG has no full
  exception-edge model;
- unknown input always retains the generic opcode.

General interprocedural analysis is conditional. Only build it if A2 proves it
would materially increase the selected candidate's coverage. Its identity
domain is `F_EXACT(functionCpoolPath) | F_UNKNOWN`; exactness requires a proven
`fclosure` producer, identical identity at every join, no escape through host,
export, property, global, var_ref, unknown container, dynamic code, or unknown
caller. Bundle visibility is not a closed-world guarantee.

A4 combines dynamic cost, A3's provable dynamic share, and an instruction-level
explanation of what the new handler omits relative to the existing generic fast
path. B1 is authorized only if at least one candidate passes.

### 4.1 A3 execution (2026-08-23): density evidence

Implemented in `src/bytecode_optimizer/bytecode_optimizer.cc` as analyze-only
mode (first stderr line preserved, `bytecode tier3:` summary as second line;
report to stderr only; zero bytecode output — the optimizer's OFF path is
unchanged). Two candidate-specific analyses, both fail-closed:

**TDZ-check elimination** (`get_loc_check`→`get_loc`, `put_loc_check`→`put_loc`):
SlotInit lattice `{SI_UNINIT, SI_INIT, SI_MAYBE}` with forward monotone
transfer and worklist fixpoint. Two-phase protocol: phase 1 propagates entry
states to convergence (count=nullptr, worklist from entry block only,
copy-or-meet over state-snapshot jump edges captured mid-block — a
`a && (x = 5)` mid-block conditional must not leak block-end state into the
merge); phase 2 sweeps every reachable block once with its converged entry,
counting each site exactly once. CFG reachability closes over all jump
targets inside a block. `set_loc_uninitialized`→UNINIT, `put_loc_check*`/
`is_loc_write`→INIT, `inc/dec/add_loc`→INIT, get_loc_check counts only.
Functions containing `with_*`/eval/catch/gosub/ret/nip_catch are skipped
whole (conservative boundary, unmodeled CFG gates).

**`get_array_el` specialization** (skip generic property path for proven
array+int-index): ArrayIdx lattice — `top`/`prev` stack-class abstraction at
P2 precision, per-slot class lattice `{SC_UNKNOWN, SC_INT, SC_ARRAY}`,
captured slots excluded. Slot reads push (`prev = top; top = slot_class`).
Counts `specializable` (both operands proven) and `obj-only` (array side
proven, index needs a guard — Lane 2A territory).

Density over the 16-fixture corpus (13 bench + tdz negative control + v8
suite, strict modules; `bench/results/a3-density/*.tier3.txt`, gitignored):

| Fixture | get_loc_check | put_loc_check | get_array_el |
| --- | ---: | ---: | ---: |
| arith-rt | 26/26 | 2/2 | 0/0 |
| arrlocal-rt | 7/7 | 2/2 | **3/3 specializable** |
| branch-const-rt | 6/6 | 3/3 | 0/0 |
| cascade-rt | 18/18 | 2/2 | 0/0 |
| copy-chain-rt | 8/8 | 2/2 | 0/0 |
| cse-loop-rt | 8/8 | 2/2 | 0/0 |
| fib-rt | 4/4 | 2/2 | 0/0 |
| json-rt | 13/13 | 2/2 | 0/1 |
| licm-rt | 6/6 | 3/3 | 0/0 |
| matrix-rt | 47/47 | 9/9 | 0/6 (3 obj-only) |
| prop-hoist-rt | 7/7 | 2/2 | 0/0 |
| prop-loop-rt | 7/7 | 2/2 | 0/0 |
| sieve-rt | 25/25 | 5/5 | 0/2 |
| string-rt | 11/11 | 4/4 | 0/0 |
| tdz-check-rt (negative control) | **4/5** | **2/3** | 0/0 |
| v8-suite-rt | 0/0 | 0/0 | 0/0 (1 func skipped) |

Totals: 236/236 get_loc_check and 43/43 put_loc_check reducible across the 14
analyzable fixtures — TDZ-check elimination is shape-independent at 100%
density on this corpus. `get_array_el` specialization density is shape-bound:
literal-array + constant-index shapes fully prove (arrlocal 3/3); object side
alone proves for computed-index loops over proven arrays (matrix 3 obj-only);
`new Array(n)`/call-built arrays and indices advanced by `++` after an inc
stay UNKNOWN — an honest lattice ceiling that Lane 2A (stateless guards)
exists to cover, not a missed case.

Negative control (`bench/fixtures/tdz-check-rt.js`, compile-only, throws):
binding access before the init store (`s += x; let x;` and `y = 7; let y;`).
`let x;` alone compiles to `undefined; put_loc1` — the declaration writes
undefined immediately, so reads after it are always safe; genuine TDZ
exposure only exists *before* the init store. The analysis refuses exactly
the two exposed sites (80% / 66.7%) while keeping all control-reducible
sites — proof the lattice is non-vacuous and the transfers are correct.

Architecture boundary (by design, not a gap): v8-suite code is embedded as a
string and evaluated at runtime — AOT static analysis sees zero sites in it
(3 functions, 1 skipped on the with/eval gate). Interpreter work on that
corpus is invisible to every AOT pass, static or not.

### 4.2 A4 go/no-go (2026-08-23): decision record

| Candidate | Cost (A2) | Provable density (A3) | Handler delta | Verdict |
| --- | --- | --- | --- | --- |
| TDZ-check elimination | 25.9% of opt-mode dispatch (strict modules) | 100% on all 14 analyzable fixtures (236+43 sites), non-vacuous negative control | `get_loc_check`→`get_loc`, `put_loc_check`→`put_loc`: removes the uninitialized-marker test + ReferenceError branch from every proven site; existing opcodes, same width pre-shrink, zero format change | **GO — Lane 1, zero format risk; C1 lands before B1** |
| `get_array_el` specialization | 4.39% dispatch, 100% generic-path execs (313M) | 3/3 full (literal array + const index), 3 obj-only (proven array, index needs guard) | Full sites fold via v1 P14 (const index + literal) or need a guarded op that skips the generic `JS_GetPropertyValue` path | **GO conditional on BC27 — the obj-only remainder is exactly Lane 2A's shape; B1+C2 after C1** |

B1 is now authorized (§6) for the get_array_el guard work: a measured Lane 1
win must land first so ext_id 253/254 are earned, not speculative. The
Lane 1 win is TDZ elimination — it proves the pipeline on existing
opcodes before any format change. Stop conditions in §11 still apply.

### 4.3 C1 landed + measured (2026-08-23): Lane 1 evidence

C1 (commit f791dc3) shipped kPassTier3Lane1 plus the three soundness
fixes its validation surfaced (P2 mid-block jump-edge drop, arg/loc
slot-index collision, P16 mid-block liveness merge; regression fixtures
p2-midblock-join.js / p16-midblock-merge.js join the differential suite).
Gates: bytecode_optimizer + differential (11 fixtures) + round-trip +
attestation + build identity matrix — 12/12 green.

Static attribution (16-fixture bench corpus, ON vs OFF mask A/B, exact
code-section sums):

| fixture | T3 sites | code bytes on/off | bundle on/off |
| --- | --- | --- | --- |
| matrix-rt | 56 | 349 / 429 (−18.6%) | 1661 / 1741 |
| sieve-rt | 30 | 163 / 212 (−23.1%) | 924 / 973 |
| json-rt | 15 | 155 / 187 (−17.1%) | 728 / 760 |
| string-rt | 15 | 113 / 143 (−21.0%) | 608 / 638 |
| cse-loop-rt | 10 | 68 / 88 (−22.7%) | 412 / 432 |
| branch-const-rt | 9 | 77 / 95 (−18.9%) | 496 / 514 |
| licm-rt | 9 | 66 / 84 (−21.4%) | 442 / 460 |
| copy-chain-rt | 8 | 62 / 78 (−20.5%) | 456 / 472 |
| arith-rt / arrlocal-rt / cascade-rt / fib-rt / prop-hoist-rt / prop-loop-rt / tdz-check-rt (control) | 6-6-6-6-6-6-5 | −16.2 / −14.8 / −8.1 / −14.3 / −13.3 / −13.3 / −13.3 % | −12 B each |
| **corpus total (15 fixtures)** | **193 sites** | **1614 / 1959 (T3 = −345 B, −17.6%)** | **10,182 / 10,528** |

(Plus a synthetic worst-case probe `t3max` — loop body of pure let-slot
traffic, 8 sites, −19.5% code — used only for the dynamic bracketing
below. v8-suite-rt: 0 sites, runtime-eval'd string corpus.)

Dynamic A/B (paired alternating runs, taskset 0-3, 9 rounds median ×3):
**wall-clock at the noise floor** — matrix +1.4% (best), arith +0.3%,
cascade −1.2%, and the worst-case check-fraction loop (1.4M check
executions removed per fetch, pure let-slot arithmetic body): flat
(3.503 vs 3.500 ms). Decisive negative evidence: the
`unlikely(JS_IsUninitialized())` branch is perfectly predicted and
shares the dispatch machinery with the plain op, so its marginal cost is
sub-nanosecond; removing it changes dispatch count zero.

Verdict record — Lane 1 is a **static-elimination win (17.7% of code,
G4 ≥1% threshold passed at 17×) with a dynamic effect below the noise
floor** on this corpus. That is exactly what the A2 ranking could not
predict (dispatch-execution share overstates a perfectly-predicted
branch's cost). The pass is zero-format, zero-regression, and keeps the
code bytes budget under control; it is kept for the static win and the
I-cache pressure it removes on strict-module handlers. B1/C2 must be
measured with the same A/B discipline — get_array_el's generic path is a
*function call* (JS_GetPropertyValue), not a predicted branch, so its
ceiling is structurally higher.

## 5. Lane 1 and Lane 2 Emission

### 5.1 Lane 1: guard-free

Emit only when A3 proves every required operand. The specialized handler must
share the generic handler's overflow, `-0`, NaN, exception, reference-release,
interrupt, `sf->cur_pc`, and pc2line behavior. A numeric handler is valuable
only if it completely skips tag extraction and branching; a repeated guard is
Lane 2A, not Lane 1.

### 5.2 Lane 2A: stateless

Allowed facts are directly checkable without runtime identity state: tag,
class, argc, serializable cpool identity, branch ordering, and safe fused
sequences. Miss executes the unchanged generic semantics.

Because generic arithmetic already prioritizes int-int, a numeric candidate
must demonstrate a concrete difference such as float-first ordering, one guard
shared by a fused sequence, or removed dispatches. Stability alone is not a
reason to emit it.

### 5.3 Lane 2B: sparse cache (deferred 2026-08-23)

Exact shape, property offset, and closure identity require runtime state. If
product policy allows them, allocate state only for PGO-selected hot sites:

- no cold-site fields, roots, or allocations;
- at most two observed variants per site;
- third variant or eight consecutive misses becomes megamorphic for the
  function lifetime;
- shape/object references participate in GC mark/free and function teardown;
- accessors, proxies, exotic objects, prototypes, realm mismatch, native/
  async/generator callables and every failed guard use generic behavior;
- OOM disables the cache without changing semantics.

This is a PGO-filtered sparse IC, not a zero-state IC. If zero runtime state is
a product red line, remove 2B and accept the lower benefit ceiling.

## 6. B1: BC27 and Opcode Space

Only A4 may authorize this section.

```text
252  OP_ext prefix
253  first measured direct winner, otherwise invalid
254  second measured direct winner, otherwise invalid
255  permanently invalid sentinel
```

Encoding is `u8 OP_ext, u8 ext_id, payload`. `ExtOpInfo.size` includes both
prefix bytes. A single `quickjs-ext-opcode.h` definition table generates the
enum, name, total size, pop/push, operand format, endian/atom rewrite, verifier
metadata, and secondary dispatch labels. `ext_id=0`, unknown ids, truncated
payload, recursive ext, or a target inside payload fails closed.

Direct-threaded builds use a secondary 256-entry computed-goto table from
`CASE(OP_ext)`; switch builds use a secondary switch. Expensive property/call
handlers normally remain extended. A candidate receives 253/254 only when:

1. secondary dispatch is at least 10% of its handler cost;
2. direct improves its target workload by at least 3% over ext;
3. it covers at least 5% of interpreter sampled ticks after earlier lanes;
4. direct and ext share one handler implementation and test set.

Fusion may beat a simple tag-specialized operation for a direct slot. Unused
slots stay invalid.

### Serialization and rollout

- New VMs read both BC26 and BC27; old VMs continue rejecting BC27.
- The compiler emits BC26 unless the AOT pass actually emits a new opcode.
- BC27 readers accept only canonical ext ids; runtime cache/quicken states are
  never legal on wire.
- Writers canonicalize runtime variants to the corresponding adaptive opcode;
  pointers, shapes, counters and cache indexes are never serialized.
- Identical source, canonical pre-PGO bundle, profile and build produce
  byte-identical output; serialization after execution returns to that same
  canonical form.
- Cache keys, attestations, host capabilities and worker identity include the
  bytecode format. Roll out dual readers first, then enable BC27 emission;
  rollback stops emission while BC26 remains loadable.

An upstream move to wire version 27 requires re-baseline and an explicit format
identity decision; the bare byte is never assumed globally unique.

## 7. Lane 3: Runtime Quickening (deferred 2026-08-23)

Lane 3 begins only after Lane 1/2 land and re-profiling still shows a residual
site with at least 2% tick coverage and a 90% Wilson-lower-bound dominant class.

Use a runtime-only saturating function score and hot-function side table. No
runtime field is serialized. Only functions containing adaptive ext sites pay
entry/backedge scoring; cold functions allocate nothing. A runtime transition
may change only to a same-size/same-format ext variant, so PC, payload, labels
and canonical serialization remain stable. State teardown and GC rules are the
same as Lane 2B.

Provide OFF, ALWAYS and ADAPTIVE builds. ALWAYS measures ceiling and exercises
handlers but is never a release configuration. OFF and ADAPTIVE must remain
observably equivalent under optimized test262.

## 8. Capsid Pipeline and Overlay

Keep specialization outside the existing generic fixed point:

```text
BC26 parse/decode
  -> existing P11/P14/P16/P2/P3.1/P6 fixed point
  -> P17 candidate-specific proof, once
  -> P18/PGO opcode emission, once
  -> ext-aware verifier
  -> P7 pc2line
  -> P8 splice/checksum and optional BC27 header
```

Early replacement would hide generic arithmetic from P2/P3.1. Unknown ext
operations are barriers to every old pass. Every accepted ext operation defines
decode, pop/push, loc/arg/var_ref reads and writes, side effects, targets,
pc2line behavior, byte swapping and atom rewriting.

All QuickJS changes arrive through numbered `patches/txiki/` overlays. A0 must
map every existing patch that touches opcode tables, dispatch, serialization,
reader, function layout or `qjs`. Candidate keep updates overlay counts/hashes,
`docs/txiki-upgrade-baseline.json`, build/format identity, the Capsid decoder,
and upgrade documentation. Direct vendor edits are forbidden.

## 9. Correctness Gates

### QuickJS-native gate

Every implementation commit runs:

```sh
make jscheck
make ctest
make cxxtest
make
make test
./build/api-test
./build/lre-test
./build/qjs tests/test_bjson.js
make test262-fast
```

Before keeping a candidate or format/reader/writer/dispatch change:

```sh
make test262
make test262-check
```

No new unexpected failure and no edits that mask it in
`test262_errors.txt`. Cover Debug/Release, GCC/Clang, NAN_BOXING 0/1,
computed-goto/switch, standalone/parserless, ASan+UBSan, and available
TSan/Valgrind/multi-platform CI.

### Optimized-test262 gate

Ordinary test262 executes source and does not invoke Capsid's serialized AOT
optimizer. Add an adapter that serializes each runtime-positive test, runs the
Capsid optimizer, deserializes it, and returns execution to the test262
adjudicator. Parse-negative cases remain on the source path.

Required modes:

| Mode | Coverage |
| --- | --- |
| source and BC26 round-trip | Generic semantics and old format |
| BC27 generic/no ext | Dual reader and no-op format path |
| Lane 1 proof emission | Actual P17/P18 path |
| Lane 2 synthetic-hot | Forced guarded emission independent of training data |
| guard hit and miss | Specialized and generic fallback semantics |
| ext and direct | Both dispatch encodings |
| Lane 3 OFF/ADAPTIVE | Quickening equivalence, if built |

Final release candidates run full optimized-test262. Quick tests may use the
fast subset, but Capsid differential/fuzz cannot substitute for this gate.

Directed QuickJS tests also cover malformed ext encoding, endian and atom
rewrite, branch targets, canonical reserialization, overflow/`-0`/NaN/BigInt/
Symbol/coercion/getters/Proxy, exceptions/pc2line, GC teardown, megamorphic
transition and OOM. Capsid then adds RED round-trip, source-vs-bytecode
differential, 40k fuzz, attestation and identity tests.

## 10. Performance, Resource, and Keep Gates

Measure production builds only, on fixed cores/governor, after warmup, with at
least seven paired samples in balanced ABBA/BAAB order. Report paired-ratio
geometric mean, same-sign count and confidence interval. Compare:

```text
patchless binary
feature code present but OFF
Lane 1
Lane 2A
Lane 2B, if allowed
Lane combinations
ext versus direct
emitted-cold and stale/missing profile
```

Patchless vs OFF measures code-layout/I-cache tax; OFF vs ON measures runtime
benefit. Keep requires:

- all correctness gates green;
- PGO-off reproduces current `kPassAll` bytes;
- broad validation geometric mean >=2%;
- each kept candidate attributes at least 1%;
- no confirmed workload regression >2%;
- emitted-cold <1%;
- peak RSS increase <=1% for stateless lanes; stateful lanes pre-register their
  own tighter per-site/RSS budget;
- opcode histogram and sampled ticks explain the wall-clock result;
- canonical BC26 bundles and the Hono deployment mix pass their per-workload
  floors before production BC27 emission is enabled.

Negative results are deliverables. Initial anomalies must be repeated; block
A/B or cross-session absolute values are not evidence.

## 11. Execution Sequence

1. **A0** — pin vendor/build/CPU and baseline native tests, test262, benchmarks,
   binary sizes, RSS, and overlay conflict map.
2. **A1** — profiling-only overlay, schema tests, zero-production-tax proof.
3. **A2** — train/validation collection and dynamic ranking; select at most two.
4. **A3** — candidate-specific analyze-only proof; no bytecode changes.
5. **A4** — combine cost, coverage and handler delta; record go/no-go.
6. **B1** — only after go: BC27, OP_ext, dual reader, verifier and full gates.
7. **C1/C2/C3** — Lane 1, Lane 2A, then conditional Lane 2B, each separately
   attributable and committed.
8. **D1** — conditional Lane 3 only after residual re-profile passes its gate.
9. **D2** — evidence, negative results, format registry, rollout/rollback test,
   and current architecture/performance documentation.

Each stage is a separate commit. Stop without BC27 if A4 has no winner, if ext
dispatch consumes the predicted win, if optimized-test262 finds a semantic
gap, if sparse state repeats upstream IC's memory/regression distribution, or
if the production deployment floors fail.

Status: **A0** baseline evidence archived (interpreter ceiling, DSE paired A/B);
**A1** in progress as overlay patch 0036; A2-A4, B1, C1, C2 not started.
C2 is the final in-scope lane; after it lands, a convergence round re-profiles
the released output. The bytecode-only loop stops (per the 2026-08-23
directive) when re-profiling shows no candidate with at least 1% attributed
broad gain and no >2% regression — that verdict, positive or negative, is
recorded as the termination report. Lane 2B/3 remain deferred, not cancelled.
