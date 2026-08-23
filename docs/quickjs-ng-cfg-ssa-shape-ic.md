# QuickJS-ng CFG+SSA, Shape IC, and Extended Opcode Plan

> Status: active successor plan, product decision 2026-08-24. The completed
> bytecode-only profiling and specialization phase remains the evidence base in
> [QuickJS-ng Opcode Optimization](quickjs-ng-opcode-optimization.md). This
> plan authorizes BC27/`OP_ext`, a new full-stack CFG+SSA IR, shape feedback,
> bounded inline caches, GC integration when required, guarded region fusion,
> and runtime quickening. Authorization is broad; every candidate still has to
> pass the correctness, memory, rollback, and measured-benefit gates below.

## 1. Decision, Evidence, and Target

The previous 0.016% SSI result does not reject this project. It rejected a
slot-oriented SSI/SCCP/GVN/LICM layer that lowered back to the unchanged BC26
instruction vocabulary and mostly duplicated direct passes. The new project
changes the lowering target and cost model:

```text
canonical BC26 after kPassAll
  -> lossless CFG
  -> operand-stack + local SSA
  -> type, effect, ownership, shape, and profile facts
  -> guarded region selection
  -> BC27 OP_ext superinstructions + retained generic slow paths
  -> ext-aware verifier, pc2line remap, serializer, and dual-version reader
```

The measured reasons to proceed are:

- `get_array_el` remains 4.39% of sampled interpreter ticks and every observed
  execution enters the generic property path;
- the strict-module TDZ and `to_propkey` work proved that the Capsid AOT
  pipeline can deliver and validate candidate-specific transformations, but
  its remaining state-free dynamic benefit is at the noise floor;
- NavierStokes is 6.9x slower than the sablejs/V8 reference on the identical
  suite source, dominated by boxed float-array loops; AOT cannot see the
  runtime-evaluated suite, so runtime-side specialization is required;
- upstream quickjs-ng's removed IC (`6b78c7f` through `7de6d467`) mixed all
  functions, four-shape strong references, bytecode mutation, GC, and
  serialization. Its mixed speed and consistently higher memory are a design
  constraint, not a reason to repeat that architecture.

Prior ranges are hypotheses, not release promises:

| Scope | Broad worker workloads | Compute/array hotspot |
| --- | ---: | ---: |
| First ext opcode or monomorphic IC | 1%..5% | 5%..15% |
| SSA region fusion + shape/array feedback | 5%..15% | 10%..30% |
| Register-level unboxing across regions | Not in this plan | Potentially higher |

A broad result above 20% is not assumed. A hotspot result above 20% is
plausible only when a fused region removes several dispatches, repeated tag
checks, and VM-stack materializations. End-to-end HTTP throughput remains a
separate metric from interpreter-core throughput.

Out of scope for this plan: native machine-code generation, a baseline JIT,
arbitrary serialized microcode, a second object representation, NaN-boxing
redesign, speculative inlining of unknown calls, and unbounded polymorphic
caches.

## 2. E0: Audit and Split the Existing 0037 Prototype

The working tree may contain `patches/txiki/0037-capsid-opcode-ext.patch`.
Treat it as an unaccepted prototype. Do not stage or build further work on it
until E0 produces an audit record and separates format infrastructure from the
first candidate handler.

Known blockers in the observed prototype:

- `ext_opcode_info` and the direct-dispatch table must use designated `[id]`
  entries; declaration order is not a valid ext-id mapping;
- `compute_stack_size`, jump-boundary validation, atom freeing/rewriting,
  endian swapping, dumps, disassembly, serialization, the Capsid decoder, and
  fuzz metadata must all derive ext sizes and effects from one table;
- `OP_ext` cannot advertise `0 pop / 0 push` to the ordinary verifier while
  `EXT_get_array_el` actually performs `2 pop / 1 push`;
- BC26 must reject `OP_ext`; BC27 may accept only canonical, known ext ids;
- the writer must select BC27 when canonical ext instructions exist and must
  never write runtime pointers, shape ids, cache indexes, or quickened states;
- reader acceptance, checksum/version identity, atom indexes, pc2line, branch
  targets, and canonical reserialization require explicit tests;
- computed-goto and switch dispatch must share the same handler body and both
  reject id 0, holes, unknown ids, truncation, and recursive prefixes;
- the current `get_array_el` handler belongs in a later candidate patch, not
  in the format-foundation patch.

Required split:

```text
0037  ext format/table/dual-reader/verifier foundation, no winning handler
0038  first measured ext handler and its directed tests
0039+ shape identity, shadow IC, active IC, and quickening in separate patches
```

Each overlay patch updates the overlay count, key, manifest, build identity,
and upgrade audit only after its own tests pass. Direct vendor edits remain
forbidden.

## 3. Internal CFG+SSA Contract

### 3.1 Ownership and file boundary

Keep the IR in the Capsid product module, not in `tools/` and not initially in
the vendored VM:

```text
src/bytecode_optimizer/
  bytecode_optimizer.{h,cc}       existing BC26 pipeline and bundle boundary
  ir/cfg.{h,cc}                   blocks, edges, decoded instructions
  ir/ssa.{h,cc}                   stack/local SSA and block parameters
  ir/effects.{h,cc}               side-effect and exception classification
  ir/region.{h,cc}                cost model, matching, guarded regions
  ir/lower_ext.{h,cc}             canonical BC27 lowering

tests/bytecode_optimizer/
  test_cfg.cc
  test_ssa.cc
  test_effects.cc
  test_regions.cc
  test_ext_round_trip.cc
  test_shape_ic.cc
```

Do not split the existing 3k-line implementation mechanically in the same
commit as a semantic change. Each new component begins behind an analyze-only
entry point and is linked into production only after its identity and
soundness gates pass.

### 3.2 Lossless CFG

Every decoded instruction records original PC, opcode, operands, stack effect,
source location, may-throw status, ownership behavior, and effect class.
Blocks include ordinary fallthrough/jump edges plus:

- catch and exception-handler edges;
- gosub/ret and finally edges;
- suspension/resume edges for generators and async functions;
- dynamic-scope/eval barriers;
- loop backedges and interrupt/safepoint boundaries.

No function-wide skip is allowed in the final architecture. During bring-up,
an unsupported function remains byte-for-byte BC26 and is counted as rejected
coverage. Unknown opcode or edge semantics fail analysis closed.

The first mandatory gate is identity lowering: decode -> CFG -> emit, with all
optimization disabled, must reproduce every canonical BC26 code section,
pc2line table, checksum, and bundle byte-for-byte.

### 3.3 Stack-to-SSA

Use block parameters (phi equivalents) for every live operand-stack position
and non-captured local at a join. The existing verifier's stack height is the
precondition; inconsistent heights remain an error. Arguments and locals have
separate index spaces.

Initial value lattice:

```text
BOTTOM
UNINITIALIZED
INT32
FLOAT64
NUMBER
STRING
FAST_ARRAY
OBJECT_SHAPES({id...}, maximum 2)
EXACT_CLOSURE(functionCpoolPath)
UNKNOWN
```

`UNINITIALIZED` is a runtime value state, not metadata. Joins lose precision
monotonically. Arithmetic overflow, unknown call results, dynamic scope,
captured storage, and unclassified cpool values become `UNKNOWN` unless a
candidate-specific rule proves more.

Captured locals, globals, var refs, object properties, and array elements are
memory operations rather than ordinary SSA locals. Version 1 uses one ordered
world/effect token. This is deliberately conservative: it prevents movement
across calls, coercion, getters, proxies, iterators, allocations, suspension,
and observable exceptions. Split memory SSA may be proposed later only with a
measured candidate.

### 3.4 Effects, exceptions, and ownership

Each IR operation is classified as one of:

```text
PURE
MAY_THROW
HEAP_READ
HEAP_WRITE
CALL_WORLD
CONTROL
SAFEPOINT
```

Effectful nodes consume and produce the world token. Throwing nodes retain the
original PC/source location and an explicit exception successor. Refcount
ownership is represented as borrowed, owned, consumed, or duplicated; lowering
must prove that the fast and slow paths free and retain exactly the same
values. A transformation without an ownership proof is rejected.

Initial fusion is single-basic-block, maximum eight original instructions,
and cannot cross a call, unknown heap effect, exception handler boundary,
suspension, backedge, or safepoint. Cross-block regions are a later stage after
the edge model passes full test262.

## 4. Region Formation and Extended Opcodes

### 4.1 Cost model

The optimizer scores a region using measured production-profile components:

```text
avoided dispatches
+ avoided tag/class checks
+ avoided generic property/call entries
+ avoided VM-stack materializations
- ext secondary dispatch
- guards
- duplicated retained slow path
- code-size / I-cache cost
```

Static instruction count alone cannot authorize a region. Every retained
template must identify the profile sites and sampled ticks it is expected to
remove. Training and validation profiles remain separate.

### 4.2 Template catalog, not arbitrary microcode

`OP_ext` is a prefix and the second byte selects a fixed, generated template.
The definition table supplies, with designated ids:

- name, total size, operand format, canonical wire version;
- normal and per-successor stack effects;
- local/arg/var-ref reads and writes;
- effect, throw, safepoint, and ownership flags;
- branch operands, atom operands, endian rewriting, and dump formatting;
- computed-goto and switch handler labels.

Do not serialize SSA graphs or an open-ended recipe interpreter. The first
catalog may contain only:

```text
EXT_fast_array_get_i32
EXT_fast_array_update_number
EXT_shape_get_own
EXT_shape_put_own
EXT_i32_arith_chain
EXT_f64_arith_chain
```

An id is added only with its handler, verifier metadata, malformed-input tests,
test262 modes, A/B evidence, and canonical serializer support. Removed ids
remain reserved and rejected; ids are never renumbered.

### 4.3 Guarded fast/slow diamonds

For a fused region, preserve the original sequence as the slow block:

```text
entry stack
    |
EXT_guarded_region ---- guard miss ----> original BC sequence
    | guard hit                              |
fused fast body                              |
    +---------------- continuation <---------+
```

All guards execute before the fast path mutates the stack, heap, refcounts, or
exception state. On miss the stack is unchanged. The ext verifier therefore
supports successor-specific stack effects: miss has the entry state; success
has the fused region's final state; both states must match at continuation.

The initial fast bodies must be non-throwing after guards. Generic semantics
remain authoritative for Proxy, accessor, exotic, BigInt, Symbol, coercion,
out-of-bounds, realm mismatch, and every unproved case. pc2line attributes the
fast operation to the first semantically corresponding original PC; retained
slow instructions keep their original locations.

### 4.4 Wire and opcode registry

Retain the registry established by the predecessor design:

```text
252  OP_ext prefix
253  measured direct winner, otherwise invalid
254  second measured direct winner, otherwise invalid
255  permanently invalid sentinel
```

BC27 is canonical only when an ext instruction is present; otherwise the
writer emits BC26. New runtimes read BC26 and BC27. BC26 containing `OP_ext`,
BC27 containing runtime state, an unknown ext id, a target inside payload, or
a noncanonical encoding fails closed.

Runtime cache state is never serialized. Serializing a warmed function emits
the same canonical adaptive ext opcode and operands as before execution.
Compatibility identity, attestation, cache keys, and deployment capability
include the wire version and ext-table identity.

## 5. Existing Shape Model and New Inline Cache

QuickJS already has hidden-class behavior through `JSShape`, hashed shape
transitions, shared shapes, and copy-on-write updates. Do not create a parallel
hidden-class system or change object property representation in the first
release.

### 5.1 Stable shape guards

Use one internal `JSICShapeGuard` abstraction with two mutually exclusive
experimental backends. Never store an unowned weak `JSShape *` whose address
can be recycled:

- **ID32**: add a per-runtime monotonically assigned shape identity and store
  the identity in the cache. This adds no GC root but loads the current shape's
  id on a hit;
- **STRONG_REF**: store a duplicated `JSShape *`, use pointer equality on the
  hot path, and participate in mark/free. This avoids the id load but may retain
  shapes/prototypes and therefore remains under the global IC memory cap.

S0 implements both as measurement builds and selects one before active shape
IC emission. They share the same flat site layout, state machine, semantics,
tests, and wire contract; no pointer or identity is serialized. STRONG_REF is
not a return to the old architecture: it is bounded to two variants at an
independently keyed hot PC and has no ring, atom hash, or hit-path write.

For ID32, a new identity is assigned on creation and every structural mutation,
including in-place add/delete/compact/resize, descriptor-flag change, and
prototype change. Audit at least these funnels and all their callers:

- `js_new_shape_nohash`, `js_clone_shape`;
- `resize_properties`, `compact_properties`, `add_shape_property`;
- `js_shape_prepare_update`;
- define/delete/reconfigure-property and set-prototype paths.

If the identity counter would wrap, the runtime disables ID32 shape IC and all
sites use generic behavior. Debug builds assert the mutation audit; directed
tests force allocator address reuse and verify that neither backend can hit a
new shape through a recycled address. STRONG_REF tests additionally prove
balanced duplication/free, cycle marking, teardown, and its retained-memory
ceiling.

Version 1 caches only an own, ordinary, normal data property at a validated
offset. It excludes prototype-chain hits, accessors, autoinit, var refs,
private fields, proxies, exotic classes, and dictionary-like mutable shapes.
Prototype caching requires a separately reviewed chain/version design.

#### 5.1.1 Invalidation and mutation matrix

Invalidation is guard-based and lazy. A structural mutation does not scan every
function/site; it changes the object's shape guard, so the old entry cannot hit.
The slow path then retains, retrains, promotes, or disables the site. No active
handler may assume that every JavaScript `set` invalidates a cache:

| Operation after warmup | Required behavior |
| --- | --- |
| Assign a new value to the same own writable normal data property | Shape/layout and offset remain valid; cache may continue to hit and reads the new value. Fast `put_field` uses `set_value` only after the same guard. |
| Add an own property | New/COW shape or ID32 mutation bump; old guard misses. Property creation remains generic. |
| `delete obj.x` | Delete/compact path changes the guard before an offset can be reused; old entry misses. Absence is not cached in v1. |
| `Object.defineProperty` changes flags, data/accessor kind, or offset | Reconfigured shape gets a new guard; accessor/autoinit/var-ref result is never installed. |
| `Object.freeze`, `seal`, or descriptor writable change | Descriptor/shape guard changes; a stale write cache cannot bypass non-writable semantics. |
| `Object.setPrototypeOf` / `__proto__` mutation on receiver | Receiver shape/guard changes and old own-property entry misses. |
| Mutation of an object in the receiver's prototype chain | Receiver shape may not change; therefore prototype-chain hits are forbidden in v1. |
| Proxy, exotic object, private field, accessor, autoinit, module var ref | Never cache; always generic. |
| Fast-array element write, delete, hole creation, length/class/storage transition | Do not cache an element pointer or length. Check object tag, class, `fast_array`, index tag, bounds, and current storage state on every hit; otherwise generic. |
| Shape free and allocator address reuse | ID32 identities are never reused; STRONG_REF keeps the guarded shape alive. A weak raw pointer is forbidden. |
| OOM, unknown mutation path, or failed invariant | Disable the site and execute generic semantics. |

For ID32, every path that mutates a shape in place must bump the identity before
the modified layout becomes observable. For STRONG_REF, every shared shape must
copy on write; an in-place mutation of a referenced shape is a correctness bug.
Both backends run directed sequences of `warm -> mutate -> hit attempt`,
including delete/re-add of the same atom, data-to-accessor conversion,
freeze-then-write, prototype replacement, array hole/length transitions, GC,
and allocator address reuse. Tests assert the generic result/exception and that
the old cache entry records a miss rather than serving a stale offset.

### 5.2 Sparse state and modes

State is a lazy sidecar owned by a hot `JSFunctionBytecode`, keyed by original
PC/site id. Cold functions allocate no table. A build may pay one nullable
sidecar pointer per function; that layout and its memory cost must be measured.
Do not replace it with a runtime global weak map without a separate teardown
and lookup-cost proof.

Site state machine:

```text
COLD -> TRAINING -> MONO -> POLY2 -> MEGAMORPHIC
                    \----------------> DISABLED
```

Rules:

- at most two shape variants per site;
- a third distinct shape or eight consecutive misses becomes megamorphic for
  that function lifetime;
- OOM or an invalid invariant disables the site and preserves semantics;
- ID32 uses no shape/object root; STRONG_REF uses only its explicitly bounded
  shape roots. Future callee/prototype caches remain separate GC patches with
  explicit mark/free tests;
- default hard caps: eight active sites per function, 64 KiB IC state per
  runtime, at most 16 bytes per monomorphic site and 32 bytes including its
  poly2 overflow state;
- cache allocation, hit, miss, transition, megamorphic, disabled, and bytes
  are observable in profiling builds, never in the public runtime ABI.

#### 5.2.1 Data layout and CPU-cache contract

Do not reproduce upstream's atom-hash plus linked buckets plus fixed four-way
ring. That implementation allocated one cache slot per distinct atom in a
function, so unrelated PCs using the same property name polluted each other's
shape distribution. Its `JSInlineCacheRingSlot` was 56 bytes, retained four
shape pointers whether the site was monomorphic or not, scanned them with a
ring/modulo loop, and wrote the ring index even on a steady-state hit. The
quickened hit bypassed the linked hash, but still paid the separate function
IC pointer, cache-array access, ring scan, shape retention, and property-array
access; the linked allocation chain remained on construction and update paths.

The replacement is per PC/site, direct indexed, and monomorphic-first:

```c
typedef uintptr_t JSICShapeGuard; /* ID32 value or owned JSShape pointer */

typedef struct JSICMonoEntry {
    JSICShapeGuard guard;
    uint32_t prop_offset;
    uint16_t miss_streak;
    uint8_t state;
    uint8_t flags;
} JSICMonoEntry;         /* exactly 16 bytes */

typedef struct JSICPolyVariant {
    JSICShapeGuard guard;
    uint32_t prop_offset;
    uint32_t reserved;
} JSICPolyVariant;       /* 16 bytes; cold overflow slab */
```

The exact field names may follow vendor style, but the size and access contract
are gates. `JSICFunctionState` owns one contiguous, cache-line-aligned
`JSICMonoEntry sites[]`; the canonical ext payload contains a bounded site id,
so the handler performs direct indexing with no atom lookup, hash, linked list,
or search. The bytecode retains the canonical atom operand for generic fallback
and serialization; the IC does not duplicate it.

MONO hit behavior is read-only: compare the current ID32 value or shape pointer,
load the adjacent offset, and access the property. It performs no counter
increment, LRU/ring write, modulo, allocation, refcount, or GC operation.
Profiling and SHADOW counters live in a separate cold slab compiled only for
those modes. POLY2 keeps the dominant variant in the mono entry and checks one
overflow variant indexed by site id; it never rotates on hit.
Promotion/reordering may occur only on the slow update path.

Load the function's sidecar pointer into a `JS_CallInternal` local once per
activation, as the old VM did, rather than reloading it from bytecode metadata
at every site. A function with no active adaptive site keeps the pointer null
and allocates neither the site slab nor the cold counters. Poly overflow is one
bounded per-function slab, not one allocation per site.

Required locality microcases:

- one monomorphic site in a tight loop;
- two unrelated PCs using the same atom with different shapes (no pollution);
- stable alternating POLY2 shapes;
- three-shape megamorphic transition;
- eight sites spanning multiple cache lines;
- emitted-cold, always-miss, and SHADOW-only controls.

Report cycles/op, instructions/op, L1D load misses, LLC load misses, branch
misses, bytes/site, allocations/function, and writes/hit. Wall-clock remains
the keep metric; hardware counters must explain it. A MONO implementation that
writes on hit, traverses a link, hashes an atom, or exceeds 16 bytes/site fails
the layout gate before broad benchmarking.

Required modes:

| Mode | Behavior |
| --- | --- |
| PATCHLESS | IC/ext code not compiled; layouts and symbols match the old VM |
| OFF | Code compiled, no site trains or allocates |
| SHADOW | Train and report would-hit/miss; generic result is always used |
| MONO | Serve one validated shape |
| POLY2 | Serve at most two validated shapes |
| ADAPTIVE | Production candidate; scoring and bounded quickening enabled |
| ALWAYS | Test/ceiling only; never a release mode |

Mode is selected at worker/runtime creation and does not change per request.
Tests may expose an internal setter before any bytecode executes. Production
workers are replaced to change mode so shared function state cannot leak across
an A/B boundary.

### 5.3 First cache operations

Implement and adjudicate independently:

1. fast-array + int-index read/update, driven by the measured `get_array_el`
   residual; this uses tag/class/fast-array guards and does not require shape
   identity;
2. monomorphic `get_field` for own normal data properties;
3. poly2 `get_field`, only if MONO misses are predominantly a second stable
   shape;
4. monomorphic own writable-data `put_field`;
5. call/prototype caches only after a fresh profile and GC/invalidation design.

The handler miss path performs the original generic operation and may update
the site only after that operation proves the cacheable own-property case.

## 6. Runtime Quickening

Quickening is authorized but does not precede SHADOW and ALWAYS ceilings.
Initially the canonical adaptive ext opcode consults its lazy site sidecar
without mutating its wire operands. Only if dispatch/lookup cost materially
consumes the measured win may the runtime replace it with a same-size,
same-format runtime-only variant.

Runtime variants:

- never change instruction size, branch targets, pc2line, or stack effect;
- never replace atom/site operands with raw pointers;
- are canonicalized by the writer;
- have OFF, ALWAYS, and ADAPTIVE test modes;
- preserve loop interrupt and safepoint frequency;
- reset completely when the function/runtime is freed.

Do not quicken ordinary BC26 opcodes in the first release. This keeps old
bundles canonical and confines stateful behavior to explicitly emitted BC27
adaptive sites.

## 7. Rollback Is a Release Contract

| Layer | Required rollback |
| --- | --- |
| Candidate | Disable one ext id/site class without disabling the ext reader |
| Runtime | Deploy IC OFF and replace/drain workers; all sidecars disappear |
| Emission | Stop BC27 emission; compiler returns to byte-identical BC26 |
| Wire | Keep the dual reader so already attested BC27 bundles remain loadable |
| Source | Revert the last numbered overlay/optimizer commit without editing earlier layers |
| Emergency miss | Reset/disable the site and execute the retained generic path |

The patchless and feature-OFF comparison is mandatory. With the compile option
undefined, generated code, `sizeof` values, symbols, structure layouts, and
fully linked binaries must match the patchless baseline, subject only to the
already documented compiler line/section metadata exception. OFF performs no
IC allocation and emits no BC27.

Rollout order:

1. dual BC26/BC27 readers with emission disabled;
2. SHADOW workers on a canary cohort;
3. one ALWAYS ceiling cohort, never production traffic;
4. one candidate in ADAPTIVE mode on a canary cohort;
5. broaden only after correctness, memory, and per-workload floors pass;
6. rollback drill: stop emission, deploy OFF, drain ON workers, load all
   previously emitted bundles, and verify canonical reserialization.

Do not remove BC27 reader support in the same release that disables emission.

## 8. Correctness Gates

### 8.1 QuickJS-native gate is primary

Every VM/shape/ext patch runs the pinned quickjs-ng native suite:

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

Before keeping a shape mutation, active cache, ext format, verifier, reader,
writer, dispatch, or quickening change:

```sh
make test262
make test262-check
```

Run source test262 in PATCHLESS/OFF/SHADOW/MONO/POLY2/ADAPTIVE as applicable.
No unexpected failure and no error-list edit that hides one is allowed.

### 8.2 Optimized-test262

The serialized adapter remains a hard requirement because normal test262 does
not traverse Capsid AOT emission. Required modes:

| Mode | Coverage |
| --- | --- |
| source PATCHLESS/OFF | generic VM and zero-tax boundary |
| BC26 round-trip | old format and current optimizer |
| BC27 canonical ext / noncanonical no-ext / malformed ext | dual reader and rejection matrix |
| identity CFG+SSA lowering | byte-for-byte no-op path |
| each ext template ALWAYS | handler semantics independent of profile |
| guard hit/miss | fused fast path and retained generic slow path |
| SHADOW/MONO/POLY2/MEGA | IC transitions and result equivalence |
| warm serialize/reload | no runtime state on wire |
| switch/computed-goto | both dispatch implementations |

Add directed tests for add/delete/redefine property, descriptor flags,
`Object.setPrototypeOf`, freeze/seal, Proxy, accessor, autoinit, private field,
non-hashed shape mutation, allocator address reuse, shape-id wrap disable,
cross-realm objects, exceptions and stack traces, OOM at every allocation,
function/runtime teardown, GC stress, interrupt handling, and megamorphic
transition.

Also require Debug/Release, GCC/Clang, NAN_BOXING 0/1, switch/computed-goto,
ASan+UBSan, available TSan/Valgrind, little/big-endian byte-swap fixtures,
Capsid differential tests, attestation/build-identity tests, and at least
40,000 ext-aware optimizer fuzz cases.

## 9. Performance and Resource Gates

Use production builds, fixed cores/governor, warmup, and at least seven paired
ABBA/BAAB samples. Keep source execution, raw BC26, optimized BC26, BC27 ext,
and runtime IC modes separate. Always compare:

```text
patchless
feature compiled but OFF
SHADOW
ALWAYS ceiling
MONO
POLY2
ADAPTIVE
each candidate alone
combined retained candidates
```

Initial keep gates:

- patchless vs OFF throughput delta has confidence interval inside +/-0.5%;
- OFF allocates zero IC bytes and changes median PSS by less than 0.5%;
- each retained candidate attributes at least 1% broad or 5% on its declared
  target with no confirmed workload regression above 2%;
- the first combined CFG+SSA/ext/IC release improves the broad validation
  geometric mean by at least 5%, otherwise retain only independently passing
  components;
- median worker PSS increase is <=1%, peak <=2%, the 64 KiB/runtime cache cap
  is enforced, and GC pause/teardown does not regress above 2%;
- emitted-cold and guard-miss-heavy workloads regress less than 1%;
- sampled ticks, hit/miss states, avoided dispatch/tag/property entries, code
  size, RSS, cycles/op, L1D/LLC load misses, branch misses, and writes/hit
  explain the wall-clock result;
- Hono mix, canonical bundle corpus, v8-suite-mod, runtime-eval v8-suite, and
  negative/megamorphic controls each meet their pre-registered floor.

Absolute results drifting across sessions are not evidence. Negative results,
memory failures, and reverted candidates remain archived as first-class
deliverables.

## 10. Execution Sequence for Another Agent

Each numbered item is a separate commit and may be reverted independently.
Do not combine a format change, IR change, active cache, and benchmark verdict
in one commit.

1. **E0 — 0037 audit/split.** Produce the blocker checklist above, split ext
   foundation from `get_array_el`, and leave emission OFF.
2. **E1 — baseline.** Pin commit/vendor/overlay/CPU, run native test262,
   optimizer/differential/fuzz baselines, binary/layout/RSS measurements, and
   archive patchless hashes.
3. **I0 — lossless CFG.** Add `ir/cfg`, explicit edge classes, identity
   lowering, malformed-CFG tests, and byte-for-byte BC26 corpus round trips.
4. **I1 — full-stack SSA.** Add block parameters, value lattice, effect token,
   exception edges, ownership verification, and analyze-only rejection counts;
   emit nothing.
5. **I2 — region census.** Match candidate regions and report dynamic weighted
   coverage, guard requirements, slow-path duplication, and predicted cost;
   select at most two first templates.
6. **F0 — ext foundation.** Land table-generated OP_ext, BC27 dual reader,
   writer/version logic, verifier, endian/atom/pc2line support, and malformed
   tests with no production emission.
7. **S0 — shape guard A/B.** Add compile-gated ID32 and STRONG_REF measurement
   backends, exhaustive mutation/allocator-reuse/GC tests, and select one by
   cycles, locality, and retained memory; no active cache consumer.
8. **S1 — SHADOW IC.** Add bounded lazy sidecars and state counters. Generic
   results remain authoritative; adjudicate hit rate and memory before MONO.
9. **R0 — first array region.** Emit the measured fast-array/int-index ext
   template with retained slow path; run full test262 and paired A/B.
10. **R1 — numeric array fusion.** Add one maximum-eight-op float or int region
    selected by I2, preserving safepoints and ownership; adjudicate alone.
11. **R2 — shape get MONO.** Enable own-data `get_field` only where SHADOW
    clears coverage and memory gates; run mutation/GC/OOM matrices.
12. **R3 — POLY2 / own put.** Add each independently only when residual misses
    justify it; third shape remains megamorphic.
13. **Q0 — adaptive quickening.** Only if sidecar lookup or dispatch consumes
    a measured part of the win; runtime variants stay same-size and canonical.
14. **D0 — combined validation.** Re-profile released output, run broad and
    hotspot A/B, resource gates, complete native + optimized test262, and trim
    every component that fails independent attribution.
15. **D1 — rollout/rollback drill.** Dual reader first, canary SHADOW, canary
    ADAPTIVE, stop-emission rollback, OFF worker replacement, old/new bundle
    loading, canonical serialization, and final architecture/performance docs.

Stop or trim a candidate if its shadow ceiling is below its guard/dispatch
cost, if identity lowering is not exact, if an ownership/effect proof is
missing, if full test262 exposes a semantic gap, if OFF is not effectively
zero-tax, if IC memory repeats the upstream regression, or if a rollback
requires rewriting deployed bundles. These stop conditions trim candidates;
they do not silently weaken the gates or authorize an unbounded cache/JIT.

## 11. E0 Audit Record (2026-08-24)

Status: **E0 complete**. The 0037 prototype was audited against the §2 blocker
checklist, split into `0037-capsid-ext-foundation.patch` (format/table/dual
reader/verifier foundation, empty catalog) and
`0038-capsid-ext-get-array-el.patch` (first measured handler + its directed
tests), and the split was re-locked into the overlay key/manifest/build
identity (commits `2a61af1`, `87957ce`). Emission stays OFF: the compiler
never produces an OP_ext today; only the directed tests splice one in.

### 11.1 Blocker checklist (§2)

| Blocker | Resolution | Evidence |
| --- | --- | --- |
| Ext tables use designated `[id]` entries, not declaration order | `ext_opcode_info[EXT_COUNT]` uses designated initializers `[ EXT_##name ] = { … }` over a `[ 0 … EXT_COUNT-1 ] = { 0 }` hole-filler; `ext_dispatch_table[256]` does the same with handler labels; `ext_opcode_info_or_invalid()` rejects id 0, holes, and anything past `EXT_COUNT` | quickjs.c:1195-1240 (overlay), quickjs-ext-opcode.h EXT_DEF rows |
| One table derives every consumer's sizes/effects | `quickjs-ext-opcode.h` is the single definition, included with EXT_DEF/EXT_FMT in: interpreter dispatch, `compute_stack_size`, `free_bytecode_atoms`, `dump_byte_code` (both sites), the writer scan (`JS_WriteFunctionBytecode`) and the reader (`JS_ReadFunctionBytecode`). Runtime-side consumers are patched in 0037; the optimizer decoder and fuzz metadata are F0 scope (11.3) | patch 0037 hunks at quickjs.c:36356, 32597, 32743, 32824, 38147, 39215 |
| OP_ext never advertises 0 pop / 0 push | EXT_get_array_el is declared `2 pop / 1 push` (`EXT_DEF(1, get_array_el, 2, 2, 1, none)`); `compute_stack_size` now reads `n_pop`/`n_push` from the ext table instead of the OP_DEF columns | quickjs-ext-opcode.h row 2; quickjs.c:36356-36391 |
| BC26 rejects OP_ext; BC27 accepts only canonical known ids | `JS_ReadFunctionBytecode` fails with `invalid ext opcode in BC26 bytecode` when `s->byte_code_version == BC_VERSION`; unknown/hole/truncated ids fail closed via `ext_opcode_info_or_invalid()` and the `ei->size > bc_len - pos` gate | quickjs.c:39215-39240; test scenarios 2, 5, 6 |
| Writer selects BC27 only when canonical ext exists; never writes runtime pointers/shape ids/cache indexes/quickened states | `JS_WriteFunctionBytecode` sets `s->has_ext_op` when an ext instruction passes the ext table; `JS_WriteObject2` bumps `d->buf[0]` to `BC_VERSION_EXT` afterwards. The wire carries only `u8 OP_ext, u8 ext_id, payload` — nothing runtime; ext-less bundles reserialize as BC26 (scenario 11) | quickjs.c:38147, 38338-38360, 38973-38980 |
| Explicit tests for reader acceptance, checksum/version identity, atom indexes, pc2line, branch targets, canonical reserialization | 11-scenario directed matrix in `tests/test_ext_bytecode.cc` (11.6) | test commit `190edd0`, green on build-dev |
| Computed-goto and switch dispatch share one handler body; both reject id 0, holes, unknown ids, truncation, recursive prefixes | One `EXT_CASE(id, name)` macro expands to a `case_ext_##name:` label (DIRECT_DISPATCH) or a nested `case EXT_##name:` (switch); the handler body appears once. OP_ext as an ext_id is not canonical, so recursive prefixes die at the `ext_opcode_info_or_invalid()` gate in dispatch, reader, writer scan, and atom walk | quickjs.c:17908-17928 (macro), 19749-19780 (case), 0038 handler |
| `get_array_el` belongs in 0038, not 0037 | 0037's catalog holds only `EXT_DEF(0, invalid, 0, 0, 0, none)`; 0038 appends `EXT_DEF(1, get_array_el, 2, 2, 1, none)` plus the handler and the test target wiring | patch files as committed |

### 11.2 Dual-mode compile/read evidence

- Reader accepts `v8 == 26 || v8 == 27` (`JS_ReadObjectAtoms`); the version is
  stored in `s->byte_code_version` and gates ext acceptance per function.
- Writer emits 26 unless `has_ext_op`; the version byte sits outside the
  checksum range (`bc_csum` covers buf[5..end]), so the flip is safe and the
  checksum stays identical across 26/27.
- Scenario 11 proves canonical reserialization both ways: BC27 blob → re-read
  → rewrite is byte-identical (version 27 kept); the no-ext BC26 blob →
  rewrite stays 26. Scenario 2's version-flip variant proves the BC26 gate
  keys on the version byte, not on how the blob was produced.

### 11.3 One-table consumers: status ledger

| Consumer | Runtime (0037) | Optimizer side (F0 scope) |
| --- | --- | --- |
| compute_stack_size | patched: size + n_pop/n_push from ext table | optimizer verifier lands with F0 |
| jump-boundary validation | no runtime jump validator exists; the size walk that positions jumps uses the table | optimizer CFG/jump validator lands with F0 (I0) |
| atom freeing/rewriting | `free_bytecode_atoms` walks by ext table | decoder's atom walk lands with F0 |
| endian swapping | none exists in this fork (fixed little-endian wire); nothing to derive today — any future swap must use the table | same |
| dumps / disassembly | `dump_byte_code` (both passes) walk by ext table, print the ext name | optimizer dump is separate and F0 scope |
| serialization | `JS_WriteFunctionBytecode` scan by ext table + `has_ext_op` | writer side is vendor only; optimizer never serializes |
| Capsid decoder | n/a | bytecode_optimizer.cc has **no** ext handling today; unknown opcode 252 fails closed at decode — F0 adds the table |
| fuzz metadata | n/a | fuzz-bytecode-opt feeds the optimizer; F0 scope |

### 11.4 Emission-OFF declaration

- The quickjs compiler (`BCEmitFunction`) cannot emit OP_ext — the opcode
  table exists for the interpreter/reader, and no emitter produces it.
- `JS_WriteObject2` flips to BC27 only when a function record already
  contains an ext instruction (test-spliced today).
- The optimizer emits only BC26; production bundles are byte-identical to
  patchless output for every BC26 path (G2 constructively).

### 11.5 Directed test evidence (11/11 green, `ext_bytecode_directed`)

1. baseline BC26 loads/runs (42); 2. BC26 rejects ext (splice + version flip);
3. version 28 rejected; 4. BC27 canonical ext runs (42) — dual dispatch and
table-derived size/effects; 5. ext id 0 and 0x7F fail closed; 6. truncated
ext prefix (`… 46 FC` at body end) fails closed; 7. checksum identity —
corrupt atoms/body fail, and a re-checksummed corrupt blob runs with the
index flipped (fast-path miss → undefined), proving the accepted blob is the
corrupted one; 8. atom indexes — `get_field` operand is the wire remap
`first_atom + section_idx` (242 + idx) and resolves to the real property
(45); 9. branch targets — fallthrough 42 / jump -1 over the ext-containing
body with identical record shapes; 10. pc2line — see below; 11. canonical
reserialization both ways.

Scenario 10 evidence (rejected promise reason for the round-tripped module):

```
ex:Error: boom | stack:     at get (ext-test.js:3:33)
    at <anonymous> (ext-test.js:5:3)
    at <anonymous> (ext-test.js:7:1)
```

The middle frame is the spliced ext site `a[0];` on line 5 — pc2line maps the
ext pc to the right source line; the throw frame (:3) and the iife call frame
(:7) bracket it.

### 11.6 Wire-registry confirmation

- `OP_ext = 252` (DEF row after `typeof_is_function`), `OP_COUNT == 253`;
  `FMT(ext)` added. Renumbering any DEF breaks the wire — pinned by the
  opcode-table derivation in the test.
- `BC_VERSION_EXT = 27`; `BC_VERSION = 26`; version byte outside checksum.
- Ext catalog rules enforced by construction: id 0 reserved and invalid;
  ids contiguous from 1, never renumbered; removed ids stay reserved (keep
  the row, size 0 → rejected by the size gate).
- Atom encoding (corrected this session, supersedes any earlier note): this
  fork has `JS_ATOM_END == 242` (241 DEF entries + JS_ATOM_NULL); record
  atoms are `leb128((242 + section_idx) << 1)`; body `OP_FMT_atom` operands
  are raw `u32 242 + section_idx`, rewritten in place by `bc_atom_to_idx`.
- cpool `BC_TAG_INT32` values are **sleb128** (`bc_put_sleb128` /
  `bc_get_sleb128`), not 4 fixed bytes — the walker's skip_value was fixed
  to match.

### 11.7 -Woverride-init explanation

GCC 13 with `-Wextra` (the overlay quickjs build uses `-Wall -Wextra`)
warns `initialized field overwritten` for the two range-init + designated
tables (`ext_opcode_info` at quickjs.c:1235, `ext_dispatch_table` at
quickjs.c:17916). This is intentional and benign: `[ 0 … N-1 ] = { 0 }`
explicitly documents the fail-closed hole contract, and the designated
`[id]` rows are the intended overrides. No `-Werror` is in effect, so the
build is clean. F0 may add `-Wno-override-init` for the quickjs target if
the catalog grows; the table pattern itself stays.

### 11.8 compute_stack_size / reader discovery; K-scenario → F0 verifier

`JS_ReadObject*` never runs `compute_stack_size` (its only call site is the
compile path, `BCEmitFunction`). The runtime trusts compiler-validated
bytecode and enforces only structural gates at read (version, ext id,
truncation, checksum). Consequently the §2 "OP_ext cannot advertise
0 pop / 0 push" blocker — the K-scenario — is enforced on the compiler/
verifier side: `compute_stack_size` now derives `n_pop`/`n_push` from the
ext table (an ext with a wrong effect fails the stack-balance/underflow
check at compile), and the F0 verifier will exercise it on the optimizer
side. The runtime tests therefore assert read-time fail-closed behavior
only, not stack underflow (documented in the test header).

### 11.9 Wire-format discoveries (pinned from writer/reader pairs)

- Serialization is module-only: `JS_WriteObject(BYTECODE)` serializes
  modules and bytecode objects; a function closure must live inside a
  module (the runtime's only public compile path is the module route).
- `BCTagEnum` pinned: `BC_TAG_FUNCTION_BYTECODE = 12`, `BC_TAG_MODULE = 13`;
  module record = tag 13, name atom, 4 counts, has_tla u8, then the module
  function record (tag 12).
- Compiler shapes for fixtures: `var a=[42]` → `push_i8 42; array_from 1`;
  `a[0]` → `push_0 (0xBA); get_array_el (0x46)`; `{x:3}` → `object;
  push_3; define_field x`; `new Proxy([42], {get: fn})` → `get_var Proxy;
  dup; …; call_constructor 2`; `n<0` with arg → `get_arg0; push_0; lt;
  if_false8`.
- Module top-level compiles as an **async function**: `push_this;
  if_false8 → return_undef; …; undefined; return_async`. A body exception
  therefore surfaces as a **rejected promise**, not a JS exception —
  `JS_EvalFunction` returns the promise. Consumers must drain the job
  queue and classify with `JS_PromiseState`/`JS_PromiseResult`
  (worker_runtime.cc:6703-6715 is the production contract;
  `load_and_run` in the test mirrors it).
- Reader failure contract: every gate (version, checksum, BC26-ext, unknown
  ext id, truncation) returns −1 / JS_EXCEPTION from `JS_ReadObject` —
  `JS_IsException` on the read result is the correct load-failure check.

## 12. E1 Baseline Record (2026-08-24)

Status: **E1 complete**. Pin, native test262, optimizer/differential/fuzz
baselines, binary/layout/RSS measurements, and patchless hashes are archived
below (commit `5d3c8779`, tree clean at measurement time). I0 proceeds from
these numbers.

### 12.1 Pinned environment

| Item | Value |
| --- | --- |
| Capsid commit (measurements) | `5d3c8779c64ea52ecf8abac47b02b2e0d70d888b` (clean tree) |
| Vendor txiki.js | `1a230d31183f062fae7a6c4fd2cff466cecc1787` (v26.6.0) |
| Vendor quickjs | `bf8988fc401e737f9946cd10a3463b48aab0fd7e` (v0.15.1-11-gbf8988f) |
| Overlay key / manifest | `4873bb31…810c` / `d548779e…1b887` (0037/0038 locked) |
| CPU / affinity | 12th Gen Intel i5-12400F, WSL2 (Linux 6.18.33.2-microsoft-standard-WSL2), `taskset -c 2-3` |
| v8-suite measurement | `bench/results/e1-v8suite-20260824T022121/` (per-artifact `sha256sums.txt` archived) |
| Post-measurement fix | `9936974` (Debug-LTO link fix; Release flags unchanged — E1 binaries unaffected) |

### 12.2 Native test262 (patchless native build, pinned suite)

`make test262` in the pristine vendor tree: **96/81152 errors, 4925 excluded,
5954 skipped**. Error-list parity: `run-test262 -m -c test262.conf -E -a`
→ "Result: 96/96 errors", exit 0.

### 12.3 Optimizer / differential / fuzz baselines

- `ctest` union subset (13/13, build-dev): `ext_bytecode_directed`
  (11-scenario E0 matrix), `runtime_bytecode_compiler_round_trip` (RED
  differential), `bytecode_opt_differential`, `bytecode_optimizer`,
  `runtime_build_identity`, `worker_build_identity_matrix`, the three
  host-managed bytecode/attestation tests, and the two host-admin round-trips.
- Fuzz: **20,000 cases, no crash** via the standalone gcc ASan+UBSan driver
  (`/tmp/fuzz-bytecode-opt-standalone`, deterministic xorshift seed, corpus
  `tests/fuzz/corpus/bytecode_opt`, `-max_len` equivalent 65536). clang/
  libFuzzer is unavailable on this machine; the CI clang gate
  (`-runs=10000 -max_len=65536`) remains the fuzz release gate.

### 12.4 v8-suite three-state throughput (taskset 2-3, rounds=5, median)

| Suite | source ms | raw ms | opt ms | opt_vs_raw | load_noise |
| --- | --- | --- | --- | --- | --- |
| v8-suite-rt | 17394.614 | 17571.633 | 16778.503 | **+4.51%** | −1.02% |
| v8-suite-mod | 17270.959 | 17258.121 | 17292.552 | −0.20% | +0.07% |

Structural body check (same benchmark-name set across source/raw/opt) passed
for both suites. These are the pre-I0 optimizer baseline; every later gate
re-measures against them. mod's −0.20% is inside run noise (consistent with
the v1 finding that optimizer wins concentrate in compute-heavy fixtures);
rt's +4.51% reproduces the v1-scale win on the deterministic compute fixture.
**Noise note for the §9.2 ±0.5% gate**: v8-suite's own load_noise band spans
−1.02%…+0.07%, so the F0 patchless-vs-OFF A/B cannot be resolved on this
vehicle alone — the F0 A/B must also use deterministic fixtures (arith-rt
class, which support `--expect-body`) alongside v8-suite.

### 12.5 Binary / layout / RSS (build-release)

| Artifact | Size (bytes) | SHA-256 |
| --- | --- | --- |
| capsid-worker | 16,705,128 | `2f70ee50…` |
| capsid-bytecode-compile | 1,406,112 | `83a04bc0…` |
| libtjs_core.a | 4,451,460 | — |
| libcapsid_runtime.a | 1,833,118 | — |
| libcapsid_host_core.a | 6,837,192 | — |
| libcapsid_bytecode_opt.a | 226,080 | — |

Worker peak RSS: **13,980 KB** (v8-suite-mod source mode, 2 rounds + 1
warmup, taskset 2-3).

### 12.6 Patchless hashes (zero-tax reference archive)

The pristine vendor tree's native build (no capsid patches) is the patchless
reference:

| Artifact | Size (bytes) | SHA-256 |
| --- | --- | --- |
| deps/quickjs/build/qjs | 1,364,928 | `d89f070d…` |
| deps/quickjs/build/run-test262 | 1,369,160 | `a4c842cd…` |
| deps/quickjs/build/libqjs.a | 1,532,308 | `5f130168…` |
| quickjs.c (patchless source) | — | `ddab0544…` |
| quickjs.c (overlay source) | — | `39c1105b…` |

**Zero-tax argument (G2, constructive)**: the overlay compiler cannot emit
OP_ext and the writer bumps BC_VERSION_EXT only when `has_ext_op`, so every
compiler-produced bundle is BC26 byte-identical to a patchless compiler's
output — the overlay's `raw.qjsb` hashes in the E1 archive are that corpus.
The runtime-side patchless-vs-OFF throughput A/B lands at the F0 gate
(12.4's noise note governs its resolution); the archive pins both
compiler-side and patchless artifacts for the comparison.

### 12.7 Environment caveats (not E1-gate failures)

- `worker_package_smoke` / `worker_package_reproducibility` cannot pass on
  this machine: `libssl-dev` is not installed (nested reproducibility
  configure fails `Could NOT find OpenSSL`), and miniconda's `libcrypto.so.3`
  leaks into the packaged binaries (smoke allowlist rejects it). Both are
  environmental and pre-existing; no E1 gate touches them.
- Debug + `CAPSID_ENABLE_LTO` mixed-link breakage (GCC 13 dropping
  `always_inline` libstdc++ ctors in LTO partitions) was fixed separately in
  `9936974`; Release builds were already unaffected.
