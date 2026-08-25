# QuickJS-ng CFG+SSA, Shape IC, and Extended Opcode Plan

> Status: implementation re-audited 2026-08-24. Sections 2–15 retain the
> original design and measurement record; section 16 is the authoritative
> current state. R0 is retired after a −12.69% target regression, product AOT
> emits BC26 only, and ext id 1 is permanently reserved. Source-attributed
> exact-PC profile v3,
> a compile-gated monomorphic `get_field` runtime quickening experiment, and a
> profile-weighted multi-instruction region decision layer are implemented.
> The repaired IC is stopped after its direct PATCHLESS comparison showed a
> significant fresh-receiver regression and no mono/Hono win. It remains
> compile-gated/default OFF; no ext handler is emitted in production.
>
> R1 (ext34, 2026-08-25): a fused loc-read + `get_array_el` ext template was
> measured and KEPT — +0.66% equal-weight geomean with a significant positive
> cluster (audio-beat-detection, audio-fft, navier-stokes, all 7/7 pairs) and
> zero regressions beyond noise (section 17). The candidate remains gated
> behind pass bits outside the deployed mask; the product pipeline is
> unchanged BC26.

## 1. Decision, Evidence, and Target

The previous 0.016% SSI result does not reject this project. It rejected a
slot-oriented SSI/SCCP/GVN/LICM layer that lowered back to the unchanged BC26
instruction vocabulary and mostly duplicated direct passes. The new project
changes the lowering target and cost model:

```text
canonical BC26 after kPassAll
  -> lossless CFG + operand-stack/local SSA (analyze only)
  -> sound type/effect facts + exact-site execution evidence
  -> multi-instruction region census and dynamic cost ranking
  -> no emission until a region clears the paired A/B gate

runtime get_field (compile-gated experiment)
  -> exact-PC training after the function is hot
  -> same-size runtime-only monomorphic opcode 253
  -> generic fallback and observation-free parking on an 8-miss streak
  -> canonical get_field when serialized
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
re-measures against them. **Vehicle validity: rt cannot measure the AOT
optimizer at all** — the fixture wraps the entire suite body in a string
literal and runs it via indirect eval, so the AOT artifact is a 141-byte
loader shell (52 insns, 0 folds) plus the suite as an opaque cpool constant;
opt and raw blobs are byte-identical (sha256 `439fd625…` both arms). rt's
+4.51% is a noise/ordering artifact of that null vehicle (per-round spread
±7%) and is **retracted** as an optimizer signal (§19.4). mod (real
module-level code, 17365 insns) is the valid vehicle: −0.20% here, ≈ −0.04%
on the frozen stack — the optimizer removes 30/17365 insns (0.17%) on this
corpus, unmeasurable against ±7% noise; consistent with the v1 finding that
optimizer wins concentrate in fixtures that carry the target patterns.
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

## 13. S0 Shape Guard A/B Record (2026-08-24)

### 13.1 Deliverables (§10 item 7)

- Overlay `patches/txiki/0039-capsid-shape-guard-ab.patch` (3 files, 18 hunks):
  the two mutually exclusive, compile-gated measurement backends
  (`xoption(CONFIG_SHAPE_GUARD_ID32)` / `xoption(CONFIG_SHAPE_GUARD_STRONG_REF)`
  in quickjs's CMakeLists, FATAL when both are set) plus the public
  `JS_ICShapeGuard*` API. The site holds a monotonic `uint32_t shape_id`
  (ID32) or a duplicated `JSShape *` (STRONG_REF). Overlay key relocked to
  `f5718ebb…` (manifest `f1386e39…`).
- `tests/test_shape_guard.cc` (`test-shape-guard`, Debug-only, links `tjs`):
  27-row invalidation/allocator-reuse/GC matrix (24 common rows, 3 ID32-only
  directed wrap rows) + `--bench` selection measurements. Both backends green
  on their own build (`build-s0-id32`, `build-s0-strong`, Debug, OpenSSL via
  miniconda, `taskset -c 2-3`).

### 13.2 Matrix coverage vs §5.1.1

| §5.1.1 row | Test row(s) |
| --- | --- |
| same-property value assign | `baseline_hit`, `mutate_slow_array_length_value_hit` |
| add own property | `mutate_add_prop_miss`, `mutate_slow_array_add_prop_miss`, `sibling_mutate_*` |
| delete + re-add same atom | `mutate_delete_miss`, `mutate_readd_same_atom_miss` |
| defineProperty data/accessor kind | `mutate_data_to_accessor_miss` |
| freeze / writable change | `mutate_freeze_miss`, `mutate_freeze_then_write_still_miss` |
| setPrototypeOf on receiver | `mutate_proto_replace_miss` |
| fast→slow array / storage transition | `mutate_array_fast_to_slow_miss` |
| shape free + allocator address reuse | `gc_free_owner_recreate_miss` (ID32) / `…_hit` (STRONG_REF), after forced churn + GC |
| GC survival, non-object, retrain, wrap | `gc_no_mutation_hit`, `non_object_*`, `retrain_*`, `id32_wrap_*` |

### 13.3 STRONG_REF COW fixes found by the matrix

The matrix exposed three places where stock quickjs's copy-on-write discipline
assumes a non-hashed shape reaching a mutation funnel is exclusively owned
(`ref_count == 1`). A STRONG_REF site's `js_dup_shape` breaks that assumption
(QuickJS's shape hash table holds no reference, so `ref_count > 1` previously
implied a hashed shape). Debug asserted; Release would have mutated a shared
shape in place (phantom properties on sibling objects). All three fixes are
gated `#if defined(CONFIG_SHAPE_GUARD_STRONG_REF)` and compile out of every
other configuration:

1. `add_property` — clone the shape when a non-hashed shape has `ref_count > 1`
   (was: `assert(ref_count == 1)`).
2. `js_shape_prepare_update` — clone whenever `ref_count != 1` regardless of
   `is_hashed` (was: clone only hashed shapes; non-hashed shared fell through).
3. `JS_NewObjectFrom` (object-literal path) — clone for exclusive use when the
   empty shape is shared (was: `assert(ref_count == 1)` + unlink/relink).

ID32 needed no such fixes: it adds no references, so stock COW semantics are
unchanged.

### 13.4 Selection measurements (`--bench`, taskset 2-3, final patch)

| Metric | ID32 | STRONG_REF | Winner |
| --- | --- | --- | --- |
| check_hit | 3.48 cy / 1.30 ns | 4.86 cy / 1.95 ns | ID32 (1.4–1.5×) |
| update (train) | 6.70 cy / 2.37 ns | 17.80 cy / 6.11 ns | ID32 (2.6×) |
| mutation pair (add+delete) | 308.8 cy | 291.3 cy | parity (allocator-bound) |
| retained memory / site | 284 B | 517 B | ID32 (STRONG_REF retains 1.8×) |

Locality: ID32's check is a single `u32` load + compare with no refcount
touches; STRONG_REF's update does a dup + release pair (refcount inc/dec and,
on last release, shape alloc/free traffic) — the retained-memory row above is
its footprint proxy, measured over 20k distinct-shape sites sharing one proto
(RSS delta from `/proc/self/statm`).

### 13.5 Verdict

**ID32 is selected.** It wins on all three §5.1.1 axes — cycles (check 1.4×,
update 2.6×), locality (one u32 load; no refcount/allocator traffic), and
retained memory (284 vs 517 B/site). STRONG_REF also required three COW
patches for memory safety, a structural cost of external references into
QuickJS's shape lifetime; ID32 required none. Both backends remain compile-
gated measurement builds; production keeps both OFF (zero tax, §12.6
patchless hashes unchanged — `CONFIG_OPCODE_PROFILE`-style gate). There is
still no active cache consumer: S1 (SHADOW IC) is the next step.

## 14. S1 SHADOW IC Record (2026-08-24)

### 14.1 Deliverables (§10 item 8)

- Overlay `patches/txiki/0040-capsid-shadow-ic.patch` (3 files, 686 lines):
  the SHADOW IC measurement backend, compile-gated
  `xoption(CONFIG_SHAPE_GUARD_IC)` with a FATAL dependency on
  `CONFIG_SHAPE_GUARD_ID32` (IC guards are ID32 shape ids), and the public
  `JS_ICSetMode` / `JS_ICGetMode` / `JS_ICGetShadowReport` /
  `JS_DumpICShadowReport` API. Lazy sidecar per function bytecode:
  cold functions allocate neither the site slab nor the counters;
  `JSICFunctionState` (sites[8] × 16 B + poly[8] × 16 B, cache-line aligned,
  ~535 B) is attached on the first observation and released with the
  bytecode. Budget: 64 KiB per runtime, bounded deny (`b->ic_denied`) for the
  function lifetime. Overlay key relocked to `23da3ce9…` (manifest
  `595da218…`), patch count 40 → 41.
- `tests/test_shadow_ic.cc` (`test-shadow-ic`, Debug-only, links `tjs`,
  compile-defs `CONFIG_SHAPE_GUARD_IC=1 CONFIG_SHAPE_GUARD_ID32=1`):
  50-row locality/hit-rate matrix (16 B entry size, 8 sites/function,
  mono tight loop, two-PC same-atom separation, POLY2 alternation, 3-shape
  megamorphism, 8-consecutive-miss rule, accessor control, cold/OFF
  controls, generic-authoritative A/B, 300-function budget cap, ID32 wrap)
  + `--bench` adjudication measurements. All 50 rows green in
  `build-s0-id32` (Debug, taskset 2-3); ctest `bytecode_shadow_ic` +
  `bytecode_shape_guard` pass.

### 14.2 Key findings

1. **Top-level bytecode teardown.** The first versions of the microcases ran
   their hot loops at top level (`JS_EVAL_TYPE_GLOBAL`). The top-level
   closure is transient — its bytecode, and with it the sidecar, is freed
   when `JS_Eval` returns — so the report saw `functions=0` after 100000
   traced observations. Hot loops must live in named functions (kept by the
   global object) for their sidecars to survive to report time. This
   explains the earlier "16 observations" misread: that dump was the streak
   microcase (8+8 = 16 iterations, pc=27 → site id (27>>2)&7 = 6), not the
   mono loop (pc=75 → site id 2).
2. **The miss-streak rule fires only on non-cacheable misses.** With two
   cacheable shapes the first distinct-shape miss trains the POLY2 overflow
   variant and subsequent same-shape accesses hit; 8 consecutive misses
   cannot accumulate on cacheable accesses. The rule is exercised by the
   accessor control (getter → non-cacheable, 1000 misses → MEGAMORPHIC) and
   by the directed streak microcase (train MONO on own-data, then 8 getter
   accesses → MEGAMORPHIC).

### 14.3 Hit-rate / memory adjudication (before MONO)

| Microcase | observations | hits / misses | transitions | site state | memory |
| --- | --- | --- | --- | --- | --- |
| mono tight loop (100000) | 100000 | 100000 / 0 | 1 | MONO | 535 B / function |
| two-PC same atom (2 × 20000) | 40000 | 40000 / 0 | 2 | 2 × MONO | 535 B |
| POLY2 alternation (100000) | 100000 | 99999 / 1 | 1 | POLY2 | 535 B |
| 3-shape megamorphic (100000) | 100000 | — | — | 1 MEGA + array site MONO | 535 B |
| accessor (1000) | 1000 | 0 / 1000 | 0 | MEGAMORPHIC | 535 B |
| budget cap (300 functions) | — | — | — | ≥100 denied | ≤ 64 KiB runtime |

`--bench` (2M-iteration mono, taskset 2-3, Debug): 308 cycles/observation
(cold start includes the lazy allocation), 535 B/function state, one counter
write per hit. The hit-rate ceiling at a stable mono site is 100% with a
single transition; the memory footprint is one ~535 B state per hot function
bounded by the 64 KiB runtime cap, with denial confined to the function
lifetime. These numbers are the input to the R2 gate; SHADOW never serves —
the generic result remains authoritative (verified by the OFF/SHADOW
identical-result A/B on a mixed own-data/proto/accessor/megamorphic
workload).

### 14.4 Verdict

S1 gates green: layout contract (16 B entries, 8 sites, direct indexing),
bounded lazy allocation, budget enforcement, ID32-wrap disable path, and
generic-authoritative results all verified by the 50-row matrix. **Proceed
to R0/R1/R2 sequencing as planned**; MONO enabling (R2) remains conditional
on SHADOW clearing the same hit-rate/memory gates in production-shaped
bundles, not on these synthetic microcases.

## 15. R0 Fast-Array/Int-Index Ext Template Record (2026-08-24)

Status: **R0 measured — negative result archived**. The emission was built,
verified, and A/B'd exactly as specced (§10 item 9); the paired measurement
rejects the template's premise on this runtime. Full evidence below, per the
tier-3 "measure first, decide later" discipline.

### 15.1 Historical deliverables at measurement time (§10 item 9)

- **Optimizer emission** (commit `3be7c0b`): `kPassExtFastArrayGet = 1u<<7`
  converts every `get_array_el` of the final stream to `OP_ext` +
  `EXT_get_array_el` (quickjs-ext-opcode.h id 1; pop 2 / push 1 — identical
  stack effect, +1 byte/site measured). Gated by `CAPSID_AOT_EMIT_EXT`
  (INTERFACE compile definition on `capsid_bytecode_opt` so every kPassAll
  consumer — compiler, bench tools, tests — sees the same mask; default OFF).
- **BC27 output contract**: an emission-ON build emits canonical BC27 (≥1
  ext; version byte 27 is outside the checksummed range and is patched after
  the checksum write). BC27 input fails closed in both `optimize()` and
  `analyze_only()` ("ext sites have no foldability consumer"); `get_array_el2`
  stays BC26. An emission-OFF build is byte-identical BC26 (§7 rollback —
  verified byte-identical in tests and fuzz gates; `arrlocal-rt` control
  sizes are 361/361).
- **Runtime**: no new overlay patch. The ext dispatch (0037 ext foundation,
  0038 ext-get-array-el handler, 0040 cacheability predicate) has been in the
  overlay since F0/S1. Overlay key unchanged (`23da3ce9…` / manifest
  `595da218…`).
- **Tests** (Debug, ON and OFF builds, 11/11 suites green): `test_r0_ext_emission`
  (ON/OFF goldens, BC27 reparse round-trip, `get_array_el2` preservation,
  BC27-input rejection, determinism), `bytecode_ext_round_trip` (updated
  fail-closed messages), `bytecode_opt_differential` (BC27 bodies byte-for-
  byte match), `runtime_bytecode_compiler_round_trip`, `ext_bytecode_directed`,
  and the fuzz gate (BC27 outputs must fail closed on every re-entry).
- **Bench tooling**: `bench/r0-paired-ab.sh` — paired A/B runner: two
  compiler builds (CAPSID_AOT_EMIT_EXT on/off) produce the opt26/opt27
  bundles; the SAME worker binary (dual reader accepts 26/27) executes both;
  ≥7 interleaved ABBA/BAAB samples per arm; medians; `--expect-body`
  byte-for-byte cross-check doubles as correctness; manifest records binary
  hashes.

### 15.2 Ext site census (17 fixtures)

| ext sites | fixtures |
| --- | --- |
| 209 | `v8-suite-mod` (module-level suite code) |
| 6 | `matrix-rt` |
| 2 | `sieve-rt` |
| 1 | `json-rt` |
| 0 | the other 13 (arith, arrlocal, branch-const, cascade, copy-chain, cse-loop, fib, licm, prop-hoist, prop-loop, string, tdz-check, v8-suite-rt) |

Two structural zeros worth recording: `arrlocal-rt` has 0 because P14 folds
every literal-index access before R0 sees the stream (foldability consumes
the sites — no residual); `v8-suite-rt` has 0 because the suite is
`eval`'d from a string at runtime — the optimizer sees only the 141-byte
top-level wrapper (`52 -> 52 insns, 141 -> 141 code bytes`), so that fixture
can never exercise ext emission by construction. `v8-suite-mod` is the
module-level (non-eval) variant and carries the real 209-site surface.

### 15.3 Paired A/B results (Release, `taskset -c 2-3`, 7 pairs ABBA/BAAB, median of 14 samples/arm)

| Fixture | ext | ver26/27 | size26 → 27 | opt26 ms | opt27 ms | Δ% (BC27 vs BC26) |
| --- | --- | --- | --- | --- | --- | --- |
| matrix-rt | 6 | 26/27 | 1661 → 1667 | 4.712 | 5.310 | **−12.69%** |
| sieve-rt | 2 | 26/27 | 923 → 925 | 23.744 | 24.170 | −1.79% |
| json-rt | 1 | 26/27 | 728 → 729 | 1.763 | 1.734 | +1.65% (noise) |
| arrlocal-rt | 0 (control) | 26/26 | 361 → 361 | 3.175 | 3.195 | −0.63% (noise) |
| v8-suite-mod | 209 | 26/27 | 309989 → 310202 | 17481.7 | 17018.4 | +2.65% (wall; inconclusive) |

- **Independent reproduction**: an earlier 7-pair run on the same method
  measured matrix-rt −12.858% (4.993 vs 5.635) — the two runs agree within
  0.2 pp. The matrix-rt sample distributions barely overlap (arm26 4.562–
  6.392 ms, arm27 5.082–7.132 ms) with interleaved sampling, so the −12.7%
  is not drift or noise.
- **v8-suite-mod is inconclusive at the ±2–3% level and reported with both
  of its conflicting signals**: wall time favors BC27 (+2.65% by the
  runner's median, +2.2% by my re-extraction), but the suite's *own*
  per-benchmark self-scores — the direct measurement of hot-loop speed
  (the suite runs each benchmark in 1 s time slices and scores runs/sec) —
  favor BC26 by −2.0% (medians 1476 vs 1446, interquartile ranges
  non-overlapping, 11/14 arm26 samples above arm27's median). The suite's
  time-budgeted design makes wall time a weak proxy (budgeted seconds are
  wall-invariant; the wall difference lives in the non-budgeted fraction
  and its direction is unexplained by the dispatch model), so the direct
  score signal is the one to read: at 209 low-density sites the direction
  matches matrix-rt (−2% vs −12.7%), consistent with per-site dispatch
  overhead scaling with site density in hot loops. The body varies run to
  run by design (self-timing), so byte-for-byte expect-body is unavailable;
  the runner uses a structural marker check and interleaved samples.

### 15.4 Root cause — the generic path already is the fast path

The R0 design premise: an inlined tag-specialized template at `get_array_el`
sites avoids the generic entry's call + tag-check cost. Measurement rejects
that premise on this runtime:

- The generic path (`CASE(OP_get_array_el)`, quickjs.c:20512) direct-
  dispatches into `JS_GetPropertyValue` (quickjs.c:10637), whose entry
  already contains the identical fast path: `tag == JS_TAG_OBJECT` and
  index tag `JS_TAG_INT` → `js_get_fast_array_element` (quickjs.c:10651) →
  immediate return. The savings the template was designed to capture do not
  exist.
- The ext site (`CASE(OP_ext)`, quickjs.c:20544) pays per execution:
  `ext_opcode_info_or_invalid(ext_id)` (bounds-checked table lookup, quickjs.c:1351)
  + `pc += ei->size - 1` + `goto *ext_dispatch_table[ext_id]` (a second
  indirect jump) — then re-runs the identical predicate with a retained
  generic fallback.
- matrix-rt's inner loop hits the fast path at ~100% of its ext sites and
  still measures **−12.7%**: even at a perfect hit rate, the ext dispatch
  indirection is pure overhead against the direct-dispatched generic. The
  template cannot win at any hit rate on this runtime, because the
  "generic entry" it beats around is already a direct dispatch into the
  fast path.

### 15.5 Correctness evidence

- `--expect-body` byte-for-byte: matrix/sieve/json/arrlocal both arms —
  BC26 and BC27 executions produce identical bodies (the A/B doubles as a
  correctness check).
- `bytecode_opt_differential` (BC27 bodies byte-identical), directed ext
  matrix, fuzz BC27 fail-closed invariants, `test_r0_ext_emission` goldens —
  all green on both ON and OFF builds.
- **Native test262 on the overlay runtime** (the build containing the ext
  dispatch 0037/0038/0040): `make test262` → **96/81152 errors, 4925
  excluded, 5954 skipped — exactly the E1 pristine-tree baseline** (§12.2);
  error-list parity `run-test262 -m -c test262.conf -E -a` → "Result:
  96/96 errors", exit 0 (same as §12.2). The ext runtime adds zero test262
  regressions. (test262 cannot exercise BC27 itself — its harness compiles
  source JS with the pristine compiler — but it bounds the runtime side of
  the emission.)

### 15.6 Reproducibility (bench/results is gitignored; archives live on disk)

- First matrix-rt run (7 pairs): `bench/results/r0-paired/` (matrix-rt −12.858%).
- Four-fixture run (7 pairs): `bench/results/r0-paired-full/` (matrix-rt
  −12.691%, arrlocal-rt −0.630%, sieve-rt −1.794%, json-rt +1.645%).
- v8-suite-mod run (7 pairs): `bench/results/r0-paired-mod/`.
- Runner: `bench/r0-paired-ab.sh` sha256 `df5d9a54…60a64` (the committed
  version; it produced the v8-suite-mod numbers and reproduces the others —
  each dir's manifest records the exact revision that ran: the first run and
  the four-fixture run used earlier revisions whose only differences are the
  body-extraction and fixture-guard changes described in the bench commit;
  the expect-body measurement path is behavior-identical across them).
- test262 overlay run: `bench/results/test262-overlay-r0.console.log`
  ("Result: 96/81152 errors, 4925 excluded, 5954 skipped").
- Compiler/worker/throughput hashes per run are in each dir's `manifest.txt`.

### 15.7 Verdict — R0 premise rejected by measurement

§9 gates: ≥7 interleaved ABBA/BAAB samples ✓; emission-OFF rollback
byte-identical ✓ (sizes, bodies, control arm); candidate improvement ≥1%
broad / ≥5% target ✗ — the target fixture measures **−12.69%** (matrix-rt,
6 sites, ~100% fast-path hit rate), sieve −1.79% (2 sites), v8-suite-mod
inconclusive at ±2% with the direct self-score signal at −2.0% (209 sites),
json +1.65% (within noise on 1.76 ms runs, 1 site), control −0.63% (noise).

**The fast-array/int-index ext template is slower than the generic opcode on
this runtime, at any hit rate**, because quickjs-ng's generic path already
inlines the identical fast path and direct dispatch reaches it without the
ext table lookup + second indirect jump. R0 is a measured negative; the
evidence is archived per the tier-3 discipline ("不论结果如何" — the number,
not the hoped sign, is the deliverable). The negative scales with site
density in hot loops (−12.7% at 6 high-density sites, −2.0% self-score at
209 low-density sites) and is zero at the control (−0.63%, identical
binaries), which is exactly the signature of a per-execution dispatch tax,
not of fixture noise.

What R0 leaves behind is the versioned ext foundation, fail-closed reader,
and paired A/B tooling. The R0 emitter and handler themselves are retired in
§16.1.

**R1/R2 implications**: do not quicken the array operation to another copy of
its existing fast path. A field IC remains a distinct experiment because a
validated shape+offset hit can bypass property lookup; it still must prove
that its guard, sidecar, and miss costs are worthwhile in uninstrumented A/B.

## 16. Re-audit Implementation Record (2026-08-24)

This section supersedes the implementation-status claims in sections 2–15.
The earlier design and R0 measurements remain useful evidence, but they no
longer describe what the branch emits or serves.

### 16.1 R0 cleanup and wire policy

- `kPassExtFastArrayGet`, `CAPSID_AOT_EMIT_EXT`, blanket `get_array_el`
  rewriting, and the runtime array-ext handler have been removed.
- The product optimizer accepts and emits canonical BC26 only. BC26 rejects
  `OP_ext`; with no live ext catalog entry, every current BC27 input also
  fails closed.
- `OP_ext` remains byte 252 as future multi-instruction infrastructure. Ext
  id 1 is a size-zero `reserved_array_get_r0` hole and must never be reused;
  archived R0 bytecode can therefore never acquire a new meaning.
- Opcode 253 is `get_field_ic`, an in-memory interpreter opcode only. Bytes
  254 and 255 remain invalid. The bytecode reader rejects opcode 253, and the
  writer converts a quickened site back to ordinary `get_field` plus its atom
  on a copied serialization buffer. Runtime cache state, shape ids, and
  sidecar indexes never enter the wire format.

Overlay patch `0041-capsid-direct-field-ic.patch` applies these rules. Patch
`0042-capsid-opcode-profile-source.patch` adds diagnostic source provenance;
the resulting 43-patch overlay is locked to key
`1064f0cdb59a96de8c178709963cd9522be72de1111c39d58be55a4b3088aba8`
and manifest
`1dd19a9279e655763a88f3842bc0510e680edea3dacdd469d9a9219fbaa215c4`.

### 16.2 Source-attributed exact-PC profile v3

`CONFIG_OPCODE_PROFILE` now emits `quickjs-ng-opcode-profile-v3`. A bounded
65,536-entry per-runtime table records runtime-local function id, exact original
PC, opcode, source-filename hash, and saturating execution count for every
observed instruction; overflow is explicit. Property sites additionally record
the path actually taken:

```text
direct
prototype_or_int_fallback
missing_or_key_fallback
accessor_or_generic
primitive_or_nullish
```

The array classifier mirrors the side-effect-free dense-array class and bounds
checks before the helper call, fixing v1's false conclusion that every
`get_array_el` execution was slow. `bench/profile-aggregate.py` reads both
archived v1/v2 and current v3 dumps, ranks true generic-path entries, and marks
high-volume monomorphic `get_field` sites. The profiling build remains
diagnostic; its timing is never product performance evidence.

The v3 function id is deliberately runtime-local. Together with source
provenance it is sufficient for within-run hotspot ranking and IC eligibility,
but it is not yet a stable PGO
bundle key. A future offline emitter must bind the observation to bundle hash
+ function cpool path + PC and reject missing or ambiguous mappings. It must
not assume that v3 runtime function ids equal serialized preorder indexes.

This provenance field was added after an unfiltered dynamic census ranked
`get_loc_check > get_loc_check > get_length > lt` as a 4%–9% dispatch
opportunity even though the same template had zero static application
occurrences in three framework bundles and only two in a fourth. A minimal app
reproduced 77,872 dynamic executions while its application bundle had zero
static occurrences, proving that bootstrap bytecode dominated the ranking.
Source-filtered sequence selection now excludes such rows rather than treating
runtime-wide heat as an application AOT opportunity.

### 16.3 Monomorphic own-data field IC

When `CONFIG_SHAPE_GUARD_IC` and ID32 shape guards are compiled in, the
runtime exposes OFF, SHADOW, and ADAPTIVE modes. Production defaults to OFF.
The implementation has these bounded semantics:

- at most eight exact-PC sites per function and 64 KiB of sidecars per
  runtime; allocation failure or budget exhaustion denies the function;
- only canonical `get_field` participates; arrays, put operations, accessors,
  prototypes, primitives, proxies/exotics, and polymorphic serving remain on
  the generic path;
- after the function reaches 128 executions, two consecutive hits for the
  same shape and own data-property offset replace the five-byte `get_field`
  instruction with same-size opcode 253 plus a sidecar id;
- a direct hit revalidates object tag, shape ID, offset bounds, and property
  flags before duplicating the value. Stable hits perform no policy/counter
  writes; the first hit after a miss only clears the consecutive-miss streak;
- a miss enters the shared generic `get_field` handler with the original atom.
  Eight consecutive misses park the runtime opcode in a terminal state; later
  executions use the generic handler without calling the observer or writing
  replacement-policy counters;
- changing away from ADAPTIVE or freeing the bytecode restores all quickened
  instructions before atom walking. Snapshot/serialization always produces
  canonical, cache-free bytecode.

Directed tests cover exact-PC separation, mono training, accessor and shape
misses, quicken/dequicken, OFF restoration, budget bounds, and serialization
into a fresh OFF runtime. This proves semantics and rollback. The subsequent
O3 + `NDEBUG` + LTO paired A/B rejected enabling this implementation; see
§16.5 and the maintained performance record.

### 16.4 CFG+SSA multi-instruction decision layer

The SSA lattice is now conservative around JavaScript numeric semantics:
constant int overflow becomes `FLOAT64`; unproven int add/sub/mul produces
`NUMBER`; division/mod/pow remain numeric; unsigned shift is not claimed as
`INT32`; and BigInt-capable bitwise operators produce `INT32` only when their
operands are proven Numbers. This closes the earlier unsound routes from an
int-looking input to an overflowed JavaScript number and from an unknown input
to a normal BigInt result.

Region selection no longer treats a single expensive opcode as fusion. A
candidate contains at least two same-block operations, never crosses a call,
safepoint, suspension, handler boundary, or unmatched effect, and is capped at
eight original instructions. With `RegionExecutionProfile`, its dynamic
weight is the minimum exact-site execution count across all member
instructions. Any missing member site gives the region zero dynamic weight;
there is no static fallback that could invent a hot region. Selection ranks
the saturated product of per-execution savings and dynamic executions, and
keeps at most two positive templates.

This is intentionally analyze-only. The next ext candidate must first provide
a stable profile-to-bundle mapping, demonstrate that it removes multiple
dispatches or shares a guard unavailable to the generic handler, then pass
the same correctness, byte-size, memory, and interleaved paired A/B gates that
rejected R0. Until then, zero candidates and zero BC27 emission are valid and
preferred outcomes.

### 16.5 Direct field IC paired A/B verdict

Seven balanced OFF/ADAPTIVE pairs with the same optimized worker produced:

| case | ADAPTIVE gain, paired 95% CI | sign | site result |
| --- | ---: | ---: | ---: |
| module-lifetime exact receiver | +2.82% [+2.16%, +3.49%] | 7/7 wins | 3 quickened, 0 restored |
| fresh receiver per request | -18.98% [-29.11%, -7.39%] | 0/7 wins | 3 quickened, 3 restored |
| sequential Hono JSON | -0.63% [-2.05%, +0.82%] | 2/7 wins | 32 quickened, 9 restored |

The same-binary host + two-worker Hono screen gave QPS centers of +2.56% for
JSON, +0.80% for static bytes 4k, and -3.86% for stream 4k; every interval
crossed zero and their geometric-mean center was -0.20%. Each adaptive worker
used 47,956 bytes of the 64 KiB cap and quickened 153 sites, of which 111/112
were restored. A same-source PATCHLESS versus feature-built OFF comparison
measured a +1.92% [+0.72%, +3.14%] latency tax on the monomorphic property
fixture, so the apparently ideal +2.82% win is not a sufficient product win.
Correctness was clean throughout.

The directed regression exposed a missing terminal fast exit. Once the direct
opcode reached eight misses, the old path restored `get_field`, but its generic
hook continued to invoke `js_ic_observe` and write terminal counters forever.
The repaired path parks opcode 253 and jumps into the shared generic handler
with observation disabled. A regression test performs another 100,000
terminal accesses and proves observations, misses, dequickens, and
megamorphic transitions remain fixed. All directed correctness, serialization,
round-trip, differential, and overlay tests pass.

The repaired same-binary screen used 21 balanced pairs:

| case | ADAPTIVE gain, paired 95% CI | sign | site result |
| --- | ---: | ---: | ---: |
| module-lifetime exact receiver | +1.40% [-1.83%, +4.73%] | 12/21 wins | 3 quickened, 0 parked |
| fresh receiver per request | -2.77% [-6.96%, +1.62%] | 6/21 wins | 3 quickened, 3 parked |
| sequential Hono JSON | -0.60% [-2.36%, +1.18%] | 9/21 wins | 32 quickened, 9 parked |

The fresh loss shrank from -18.98% to -2.77%, so the terminal-observer defect
was real and the repair worked. It still did not create a statistically clear
same-binary win. The decisive 21-pair PATCHLESS-to-ADAPTIVE comparison uses
latency change (positive means regression): fresh **+7.31%
[+5.15%, +9.51%]**, Hono -0.94% [-2.84%, +0.99%], and mono -0.25%
[-4.70%, +4.40%]. A separate PATCHLESS-to-feature-OFF attribution had centers
of +3.48% fresh, -0.52% Hono, and +2.38% mono; those sessions are not
subtracted, but the centers fail the +/-0.5% OFF gate.

**Final decision: current R2 is stopped and remains compile-gated/default
OFF.** Do not proceed to another terminal opcode, POLY2, prototype caching,
put-field caching, a larger budget, or the full host/resource matrix. The
repaired implementation has a correct terminal path but no product-level
benefit: mono and Hono are neutral against PATCHLESS and fresh receivers
regress significantly. A future IC design must remove observer/layout cost
from the compiled-OFF generic handler and demonstrate its stable-site win
directly against PATCHLESS before any cache expansion.

Evidence is generated by `bench/field-ic-{ab,host-ab,off-tax}.sh`. The initial
screen is retained under the timestamped `bench/results/field-ic-*20260824T16*/`
directories. Repaired 21-pair evidence is in
`field-ic-terminal-ab-20260824T165700-p21`,
`field-ic-terminal-net-20260824T170000-p21`, and
`field-ic-terminal-off-tax-20260824T170100-p21`, each with a manifest, raw
samples, summary, and SHA-256 list.

### 16.6 Cross-suite fusion census and rejected `get_arg0 + get_field`

The source-attributed region census was extended beyond the original fixture
and Hono-only inputs. Four production-shaped framework bundles (Hono, H3,
itty-router, and Elysia) executed 256 warm-up plus 1,000 measured requests each.
Source filtering excluded 79,369 foreign/bootstrap sites and left 5,191,010
application instruction executions. No long, high-coverage sequence was common
to all four applications. The leading common property pair was:

| pattern | executions | programs | static occurrences | dispatch-only ceiling |
| --- | ---: | ---: | ---: | ---: |
| `get_arg0 > get_field` | 53,272 | 4/4 | 38 | 1.026% |
| `get_loc8 > get_field` | 8,131 | 2/4 | — | 0.157% |
| `get_field > get_field` | 17,584 | 2/4 | — | 0.339% |

Kraken, Octane 2, and SunSpider were then used as an independent breadth
check, not as product timing evidence. The checkouts were pinned at Kraken
`77ef4e08af23c131166762adad8cb460c49160e8`, Octane
`570ad1ccfe86e3eecba0636c8f932ac08edec517`, and WebKit/JetStream
`7769b693502fa80f28a97bbfacd3296e0513acc5`. Their original classic-script
harnesses were used because only 9 of 37 compilable module conversions executed
under Capsid; strict-module and frozen-intrinsic compatibility failures are not
performance results. Of 41 classic programs, 34 completed (Kraken 10/14,
Octane 12/15, SunSpider 12/12). `get_arg0 > get_field` was again common: 125.6
million executions, 1,333 occurrences, and five programs. `mul > add` appeared
in three programs but ranked only 38th. The exact-site table overflowed by
24,832,010 insertions, so these legacy-suite counts establish cross-program
presence only; they are not a complete prevalence estimate or a timing claim.

That breadth evidence justified one guarded prototype, not a keep decision. The
prototype replaced `get_arg0; get_field atom` with a six-byte BC27 ext
instruction. It preserved the generic property's full semantics and removed
one primary dispatch plus the cancelling argument duplicate/free. Directed
tests covered own and inherited data, getter, Proxy, missing property, primitive,
null, and throwing getter behavior; all responses and round trips matched.
Static application confirmed that the transform was not a single microbenchmark
artifact:

| application | fused sites | control bytes | candidate bytes |
| --- | ---: | ---: | ---: |
| Hono | 147 | 184,639 | 184,599 |
| H3 | 309 | 215,779 | 215,681 |
| itty-router | 93 | 51,519 | 51,499 |
| Elysia | 1,642 | 944,172 | 942,825 |

The keep gate was 11 paired clusters per application in balanced ABBA/BAAB
order on the same worker and CPUs. Each invocation used 256 warm-up and 1,000
measured requests, giving 2,000 requests per arm, application, and pair. A
cluster-paired 100,000-resample bootstrap produced:

| application | control median | candidate median | candidate gain, 95% CI |
| --- | ---: | ---: | ---: |
| Hono | 0.195 ms | 0.197 ms | -0.95% [-2.00%, +0.20%] |
| H3 | 0.250 ms | 0.253 ms | **-1.13% [-2.11%, -0.08%]** |
| itty-router | 0.328 ms | 0.326 ms | +0.89% [-0.06%, +2.31%] |
| Elysia | 0.431 ms | 0.449 ms | **-3.98% [-4.88%, -3.13%]** |
| equal-weight combined | — | — | **-1.28% [-1.77%, -0.77%]** |

**Decision: reject and remove this fusion.** It reduced serialized size and
occurred in both framework and legacy suites, yet it did not remove the costly
property lookup itself. Its theoretical framework ceiling was about one percent,
while ext-prefix decoding, handler/layout changes, and the still-present generic
lookup were enough to erase that saving. This also rejects mechanically adding
the analogous `get_locN + get_field` catalog: it has the same cost model and no
new eliminated work.

The maintained outcome is therefore BC26 production emission, v3 profiling,
and analyze-only CFG+SSA. A later fusion must remove multiple dispatches and
some helper/coercion/reference-count work, and must have material coverage in
current framework bundles before implementation. A later IC must be a new
zero-tax design: compile-time per-site slot operands, no compiled-OFF observer
or layout cost, lazy state only for proven-hot functions, and a direct
PATCHLESS comparison. The rejected ext implementation is not retained.

Local raw framework samples and analysis are under
`bench/results/arg0-field-fusion-framework-ab-20260824/`; the framework and
legacy-suite census outputs are under
`bench/results/framework-sequence-census-v3-20260824/` and
`bench/results/legacy-suite-sequence-census-20260824/`.

## 17. R1 Loc-Read + get_array_el Ext34 Fusion Record (2026-08-25)

Status: **R1 measured — kept (modest positive)**. A run-based matcher fuses
consecutive local-slot reads ending in `get_array_el` into two new BC27 ext
templates (id 2, two-slot window; id 3, three-slot window). The paired
measurement shows a significant positive cluster on three Kraken programs
and no regression beyond noise elsewhere; the candidate is kept behind its
pass bits, outside the deployed mask.

### 17.1 Deliverables at measurement time (§10 item 10)

- **Optimizer matcher** (commit `c1c09c4`, fixed `2bb34ae`): after the last
  reshrink, scan for a run of ≤3 slot reads (`get_loc0..3` short forms,
  `get_loc8`/`get_loc`, `get_arg0..3`/`get_arg`, and the emitter's fused
  `get_loc0_loc1`) immediately followed by `get_array_el`. Payload bytes are
  tagged: bit 7 selects the argument buffer, low 7 bits the local slot. The
  fused ext's stack effect equals its window's (id 2: pop 2 push 1; id 3:
  pop 2 push 2), so heights, catch offsets, and exception stack shapes are
  preserved; the only CFG constraint is that no jump may land strictly
  inside a window (a landing at the window start is fine). Id-3 windows win
  over id-2 windows at the same start.
- **Runtime handlers** (patch 0045 `capsid-ext-loc-array-fusion`): a guarded
  fast path (object + int-index tag check plus `js_get_fast_array_element`,
  covering arrays, arguments, and typed arrays) with a full slow path that
  duplicates the window's exact `get_loc*; get_loc*; get_array_el` generic
  semantics including exceptions (`sf->cur_pc` restored on throw).
- **Pass bits** (API only, outside the frozen CLI): `kPassExtFuse34 = 1<<7`,
  `kPassExtFuse4 = 1<<8`. The deployed `kPassAll` mask (0x7f) and product
  pipeline are unchanged; the A/B arms differ only in these bits.
- **Semantics coverage** (commits `aa450f1`, `310e43e`): goldens for the
  short-form payloads plus a live base/opt A/B through the real compiler
  output. Two emitter facts had to be learned the hard way: `let` locals
  read via `get_loc_check` (TDZ), which the matcher excludes by design (the
  fused handler performs no TDZ re-check — fail-closed), and `var` locals
  initialized to constants are constant-eliminated by the emitter (uses
  become `push_<const>`, no slot read at all). The live fixtures therefore
  use non-constant `var` initializers, verified end to end to fuse as:
  f `[80 81]`, g `[80 81]` (const read excluded), h `[00 01]`, h3 `[80 03]`
  (the `get_loc3` short-form regression), k id3 `[00 80 01]`.

### 17.2 Layout-tax probe (no code change to the window)

A side-by-side probe compiled the 21-fixture corpus with the ext bits on and
off and compared the serialized outputs. Both arms produced byte-identical
BC26 streams for all 21 fixtures (commit `91e52df`): the ext matcher is a
pure rewrite on the shrunk stream with no layout or offset side effects. The
two arms' execution paths then differ only by the fused-vs-unfused
instructions themselves.

### 17.3 read_slots short-form bug found by the probe

The layout-tax probe exposed a matcher defect: short-form `get_loc1..3`
reads were encoded with slot 0 (the decoder leaves `aux` at 0 for
operand-less short forms; `slot_of()`'s `op - OP_get_loc0` convention was
not mirrored in the matcher). This silently mis-fused windows using
`get_loc1..3` as indices — the exact shape audio-beat-detection's hot loops
emit. The pre-fix checkout failed exactly the two new short-form goldens
(lines 1844/1862) and the micro-reproducer disagreed with the unoptimized
run; the fix (`op - OP_get_loc0`) makes all goldens and the live A/B agree.
Without the end-to-end probes this would have shipped a semantic
mismatch in a deployed-looking arm; the fail-closed round trip caught it.

### 17.4 Paired A/B results (Release, `taskset -c 2-3`, 7 ABBA/BAAB pairs per program)

Runner `bench/ext34-classic-ab.sh` (commit `42694ca`): control mask 0x7f vs
candidate 0x1ff, 8 programs of the classic suite corpus, 14 samples per arm
per program.

| program | gain % | CI95 % | positive pairs |
| --- | ---: | ---: | ---: |
| kraken-audio-beat-detection | **+2.01** | [1.27, 2.75] | 7/7 |
| kraken-audio-fft | **+3.66** | [1.78, 5.58] | 7/7 |
| kraken-audio-oscillator | −1.41 | [−4.49, 1.76] | 1/7 |
| kraken-imaging-darkroom | −0.22 | [−0.75, 0.32] | 3/7 |
| octane-box2d | +0.38 | [−0.69, 1.47] | 4/7 |
| octane-gameboy | −0.46 | [−1.30, 0.40] | 2/7 |
| octane-navier-stokes | **+1.02** | [0.47, 1.56] | 7/7 |
| octane-richards | +0.37 | [−1.66, 2.45] | 5/7 |
| equal-weight geomean | **+0.66** | [−0.65, 1.98] | — |
| kraken geomean / octane geomean | +0.99 / +0.33 | — | — |

All 8/8 programs completed, zero failures, zero semantic mismatches. The
three programs with 7/7 positive pairs form a significant cluster with CIs
clear of zero; darkroom (0 fused sites — clean control), oscillator,
gameboy, box2d, and richards sit within noise. No program shows a
regression beyond its noise band.

### 17.5 Verdict — ext34 kept

The fused template removes dispatch (two or three slot reads + the array
access become one opcode) and the per-slot generic read work; unlike R0's
blanket `get_array_el` rewrite it does not replace a fast path with a
table-indirected one at every site — it only compresses the reads leading
into a `get_array_el` that still executes its own generic path. The
measured signature is exactly that: consistent small wins where
slot-read/array windows are hot (audio DSP kernels, navier-stokes), flat
elsewhere. Kept behind `kPassExtFuse34|kPassExtFuse4`, outside the deployed
mask; the product pipeline remains BC26 (unchanged).

**R2 implication**: the next array fusion should remove the `get_array_el`
generic path itself (the part that R0 and this record both leave intact),
not another copy of its dispatch.

Evidence is archived under `bench/results/ext34-classic-ab-20260825T014012/`
(per-program pair gains, summary.json; bench/results is gitignored).

## 18. Corrected Full-Suite Profile + Candidate Re-Rank Record (2026-08-25)

Status: **the ranked candidate pool is exhausted — no candidate clears the
direct-binary gate; the optimization loop terminates with R1 (ext34) as the
last kept item.** This section archives the corrected profile and the
re-ranking that supports that verdict.

### 18.1 Why a corrected collection (task #74)

The first collection (`bench/results/classic-profile-20260825/`) failed two
ways: `--profile-tool` pointed at the non-profile binary (every program:
"`--opcode-profile` requires a CONFIG_OPCODE_PROFILE build"), and `--passes
0xffffffff` profiled the candidate's BC27 output — which the 45-patch
profile qjs cannot read — instead of the deployed mask `0x7f` (BC26). The
corrected run (`bench/classic-profile-collect.sh`, commit `bba6f60`)
profiles the **deployed pipeline**: the production compiler
(`bench/bin/classic-bytecode`) at the shipped `kPassAll` 0x7f, executed by
a separate CONFIG_OPCODE_PROFILE runner (`bench/bin/
classic-bytecode-profile`) so instrumented timing is never a performance
result. A fail-fast probe (one pinned-cpuset profile of
sunspider-math-partial-sums) runs before collection so a wrong binary
cannot silently produce 53 error files again.

### 18.2 Collection facts

54 of 55 programs completed within the per-program caps on `taskset -c
2-3`. `kraken-imaging-gaussian-blur` exceeds 600 s of instrumented
execution and was recovered at 1800 s (12 min); `octane-mandreel` exceeds
even 1800 s (30 min) and was retried at the maximum 3600 s cap, timing
out there too — **54/55 is the terminal collection state**. Mandreel is
outside the A/B corpus (§17.4), so its absence cannot change the §18.7
verdict; a post-collection re-run of the §18.5 census on the final
54-dump set reproduced every listed number unchanged. The final archive
state is recorded in the results manifest
(`bench/results/classic-profile-20260825T015948/manifest.json`).
Aggregate on the completed dumps: 66,624,971,999 dynamic opcode
executions across 19,967 functions, 849,458 instruction sites.

### 18.3 Opcode aggregate (dispatch cost)

| rank | opcode | execs | share |
| ---: | --- | ---: | ---: |
| 1 | get_loc8 | 7,196,809,648 | 10.80% |
| 2 | swap | 5,896,907,130 | 8.85% |
| 3 | push_0 | 5,298,090,728 | 7.95% |
| 4 | or | 3,941,706,487 | 5.92% |
| 5 | get_array_el | 2,839,775,100 | 4.26% |
| 6 | add | 2,830,132,542 | 4.25% |
| 7 | put_loc8 | 2,644,091,309 | 3.97% |
| 8 | sar | 2,404,645,764 | 3.61% |
| 9 | to_propkey | 1,946,251,103 | 2.92% |
| 10 | push_2 | 1,911,708,835 | 2.87% |
| 11 | push_1 | 1,694,061,975 | 2.54% |
| 12 | get_var_ref0 | 1,603,706,109 | 2.41% |

Slow-path ranking (dispatch + 20× slow) is led by call_method (3.11B),
get_array_el (1.27B), add (761M), get_field2 (642M), sub (436M),
tail_call_method (309M).

### 18.4 Concentration: octane-zlib dominates every hot opcode

Zlib-only aggregate (its single profile dump) as a share of the
suite-wide count:

| opcode | zlib execs | suite execs | zlib share |
| --- | ---: | ---: | ---: |
| or | 3,886,697,304 | 3,941,706,487 | 98.6% |
| push_0 | 5,156,264,079 | 5,298,090,728 | 97.3% |
| sar | 2,265,136,719 | 2,404,645,764 | 94.2% |
| put_loc8 | 2,286,302,129 | 2,644,091,309 | 86.5% |
| get_loc8 | 5,484,476,413 | 7,196,809,648 | 76.2% |
| add | 2,145,788,973 | 2,830,132,542 | 75.8% |
| get_array_el | 2,128,851,967 | 2,839,775,100 | 75.0% |
| swap | 4,233,590,472 | 5,896,907,130 | 71.8% |

One program — octane-zlib's inflate/crc loops — is 72-99% of every hot
opcode in the entire suite. The suite's opcode mix is not a balanced
picture; it is zlib's bit-manipulation loops over the other 54 programs.

### 18.5 Sequence census (adjacent windows, exact PC)

**2-len windows** (cross-program; ext_saved = 0 for all — see the census
tool's dispatch model): get_loc8>add 369M/20 programs, push_1>sub
332M/28, push_1>add 291M/33, get_loc8>get_array_el 214M/21,
get_loc8>get_field 205M/20, get_loc2>get_field 169M/15, mul>add
168M/16, get_field>get_field 147M/20, get_arg0>get_field 143M/16,
get_array_el>get_loc8 137M/16, push_2>add 123M/15, get_array_el>add
106M/19, get_loc0>get_field 103M/20, add>put_loc8 67M/20.

**3-4 len windows** (ext_saved > 0 — these are the only dispatch-saving
shapes): every top rank is an octane-zlib site artifact.

| rank | pattern | region exec | programs | top site |
| ---: | --- | ---: | ---: | --- |
| 1 | get_array_el > push_0 > or | 1,700M | 3 | zlib f52:pc382 |
| 2 | add > push_0 > or | 1,192M | 3 | zlib f52:pc368 |
| 3 | push_2 > sar > get_array_el | 1,128M | 4 | zlib f49:pc2361 |
| 4 | push_0 > or > put_loc8 | 960M | 3 | zlib f52:pc369 |
| 5 | add > push_1 > sar | 376M | 4 | zlib f52:pc931 |
| 6 | push_1 > shl > add | 325M | 3 | zlib f52:pc929 |
| 7 | or > put_loc8 > get_loc8 | 258M | 6 | zlib f49:pc2577 |

**Non-zlib 3-4 len census** (52-dump re-run, zlib removed): the survivors
are (a) ext34's own target shapes — get_loc8-run + get_array_el windows
(66.7M/13 programs, 41.6M/6), which appear here because the deployed
0x7f mask does not carry the kept ext bits; this is exactly the already
implemented, already measured R1 fusion — and (b) windows whose fused
handlers would have to embed a generic slow path: mul > swap > to_propkey
> swap (41.2M/4, navier-stokes top site), get_array_el > swap >
to_propkey > swap (37.1M/14), get_loc8 > mul > get_loc8 (56.1M/5,
octane-crypto JSBN), set_loc8 > push_i8 > sar > get_loc8 (47.6M,
octane-crypto), mul > add > put_loc8 > get_loc2 (47.9M, octane-crypto).

### 18.6 Why no ranked candidate clears the direct-binary gate

1. **Corpus coverage.** The A/B corpus (§17.4) is eight programs:
   audio-beat-detection, audio-fft, audio-oscillator, imaging-darkroom,
   box2d, gameboy, navier-stokes, richards. octane-zlib — the sole source
   of every top 3+ len window — is not among them, nor are mandreel or
   gaussian-blur (the other two profile-dominant programs). A fusion aimed
   at those windows would be measured as noise on the corpus.
2. **Spread 2-len windows save zero dispatches.** Fusing `[A, B]` into
   `OP_ext + ext_id` costs two dispatch slots either way (the census
   model: ext_saved = 0 at 2-len). The only residual gain would be
   dispatch-table overhead, and the structurally identical
   `get_locN + get_field` catalog was already rejected in §16 on the same
   cost model with a measured negative (−1.28% combined).
3. **Generic-path duplication measures negative.** Every surviving
   non-zlib 3+ len candidate requires the fused handler to embed a
   generic slow path — `js_mul` (valueOf/-0/object operands),
   `ToPropertyKey` (ToPrimitive can run user code and throw), or
   `get_array_el`'s own generic machinery. That is precisely the
   structure that measured −12.7% (R0's blanket get_array_el rewrite) and
   −1.28% (arg0>get_field ext): the dispatch is removed but the generic
   work is still executed, inside a longer handler. R2's note in §17.5 is
   the same conclusion: the next step must *remove* the generic path, not
   copy its dispatch — and removing it requires provable shapes, i.e. an
   inline cache, which was explored in S1 (SHADOW IC) and stopped.
4. **call_method** — the only spread opcode-level opportunity (155M,
   0.23% of execs) — is a runtime IC concern, not a bytecode-shape
   concern; it was explored and stopped earlier.

### 18.7 Verdict — the loop terminates with R1 as the last kept item

The corrected profile answers the tier-2 question directly: the deployed
BC26 stream's remaining hot shapes are either zlib-single-site artifacts
(unmeasurable on the A/B corpus), 2-len windows with zero dispatch
saving, or shapes that require embedding a generic slow path — the exact
structure that has measured negative in both prior ext attempts. R1
(ext34) is the last item that cleared the gate (+0.66% geomean, kept).
"Direction exhausted" is the honest result the methodology expects: the
facts above are the evidence, and the next opportunity, if any, is a
corpus/measurement decision (adding zlib-class workloads to the A/B
corpus) before any new implementation — not an implementation decision
made blind.

Evidence is archived under
`bench/results/classic-profile-20260825T015948/` (54 final dumps, manifest
records the two retry attempts; bench/results is gitignored); collection
harness `bench/classic-profile-collect.sh`; ranking tool
`bench/profile_sequences.py`.

## 19. Cumulative Baseline Freeze + Leave-One-Out Record (2026-08-25)

Status: **final validation gate (handoff gate 4/5). The deployed baseline
is frozen at BC26 + optimizer mask 0x7f + patch 0043 (mixed-numeric `mul`
fast path); the only kept-but-off-mask items are R1 ext34's pass bits; the
leave-one-out is the 0x7f-vs-0x1ff paired A/B, reproduced on a quiescent
machine; fresh-directory validation is green.**

### 19.1 What "cumulative baseline" means

Per handoff §6, the loop accumulates small, well-attributed wins into one
shipped configuration. The final configuration:

| layer | item | mask/bits | output |
| --- | --- | --- | --- |
| VM (patch 0043) | mixed numeric `mul` fast path | — | BC26 wire |
| optimizer (deployed) | v1 P0-P8 pipeline | `kPassAll` = 0x7f | BC26 |
| optimizer (off-mask) | R1 ext34 (ext ids 2/3) | `kPassExtFuse34`=1<<7, `kPassExtFuse4`=1<<8 | BC27 (API-only) |
| optimizer (retired) | R0 single-op array ext (id 1) | — | permanent reserved hole |

Per-configuration measured effects (handoff §6 / §17.4 / §12.4):

- `mul` fast path alone (0x7f on both arms, 7 pairs × 8 classic programs,
  224 observations): **+3.366% equal-weight geomean** [1.262, 5.515].
- optimizer alone (v1, deterministic compute fixtures): **+38.94% /
  +29.09%** on arith-rt / cascade-rt (G3, v1 record); on v8-suite-mod the
  deployed pipeline removes 30/17365 insns (0.17%) → opt_vs_raw ≈ −0.2%,
  inside that vehicle's ±7% noise (§12.4 / §19.4). (An earlier v8-suite-rt
  +4.51% claim is retracted — that fixture is an eval shell the optimizer
  cannot see into; §12.4/§19.4.)
- 0x1ff vs 0x7f (ext34 on the patched stack): **+0.66% equal-weight
  geomean** [−0.65, 1.98] — the original leave-one-out; removing the ext
  bits from the candidate returns exactly the deployed 0x7f state.

### 19.2 Final full-suite profile state (task #74)

54/55 dumps terminal. `octane-mandreel` exceeded the 1800 s retry cap and
then the maximum 3600 s cap (two timeouts, no partial dump); it is outside
the A/B corpus, so its absence cannot change the verdict. A post-collection
re-run of the §18.5 census (2-len `--min-programs 15`, 3-4-len
`--min-programs 3`) on the final 54-dump set reproduced every listed number
unchanged (2-len: get_loc8>add 369M/20, push_1>sub 332M/28, push_1>add
291M/33, get_loc8>get_array_el 214M/21, get_loc8>get_field 205M/20,
get_loc2>get_field 169M/15, mul>add 168M/16, get_field>get_field 147M/20,
get_arg0>get_field 143M/16, get_array_el>get_loc8 137M/16, push_2>add
123M/15, get_array_el>add 106M/19, get_loc0>get_field 103M/20, add>put_loc8
67M/20; 3-4-len rank 1 get_array_el>push_0>or 1,700M). The §18.7 verdict is
confirmed on the terminal archive.

### 19.3 Fresh-directory validation (gate 5)

Fresh `build-release-r1` (Release + LTO, CAPSID_BUILD_HOST=ON), configured
with miniconda OpenSSL/Boost + host node:

- ctest 377/380 green; the 3 failures are recorded environmental
  (wpt_conformance_not_configured loud-fail by design; worker_package_smoke
  and worker_package_reproducibility — libcrypto.so.3 undeclared dependency,
  no libssl-dev on this WSL2 host).
- framework correctness groups (hono/elysia/h3/itty-router/framework, 46+
  tests) all green; framework QPS workloads are not runnable here (no Go
  loadgen, no bundle artifacts) — correctness is the ctest gate.
- stale-test find: `test_ext_round_trip` a4 used ext id 2 as its
  unknown-id example; id 2 became a valid R1 template, so the test asserted
  a kept template is invalid. Fixed to id 4 (commit 3664289). This is
  exactly the staleness class the fresh-dir requirement exists to catch
  (the old build dir predated patches 0043/0044; its overlay stamp was
  stale and silently produced wrong test binaries).

### 19.4 Leave-one-out and validation measurements

**ext34 A/B rerun (quiescent 2-3, 7 pairs × 8 programs, §17.4 protocol,
`bench/results/ext34-classic-ab-r1-20260825T0510/`):**

| program | §17.4 gain | rerun gain | CI95 | pos. pairs |
| --- | ---: | ---: | ---: | ---: |
| kraken-audio-beat-detection | +2.01 | **+2.34** | [1.84, 2.83] | 7/7 |
| kraken-audio-fft | +3.66 | **+2.94** | [2.05, 3.84] | 7/7 |
| kraken-audio-oscillator | −1.41 | −0.06 | [−1.21, 1.09] | 4/7 |
| kraken-imaging-darkroom | −0.22 | −0.01 | [−0.48, 0.46] | 3/7 |
| octane-box2d | +0.38 | +0.52 | [−1.35, 2.43] | 5/7 |
| octane-gameboy | −0.46 | −0.52 | [−3.28, 2.33] | 3/7 |
| octane-navier-stokes | +1.02 | **+0.96** | [0.50, 1.42] | 6/7 |
| octane-richards | +0.37 | +0.77 | [−1.95, 3.57] | 5/7 |
| equal-weight geomean | **+0.66** | **+0.86** | [−0.14, 1.87] | — |

The significant cluster (beat, fft, navier-stokes) reproduces with CIs
clear of zero; darkroom is the 0-fusion clean control on both runs; every
other program sits within its noise band. The tainted earlier rerun
(`ext34-classic-ab-r1-20260825T035504/`, cpuset 0-1 concurrent with the
mandreel profile, beat −4.55% 1/7) is archived as invalid; this quiescent
2-3 run is the record.

**0-fusion control verification (static count probe):** at the deployed
mask 0x7f, all 8 A/B corpus programs compile to BC26 with zero OP_ext
(control arm clean). At 0x1ff, ext emission matches the templates exactly:
beat 42 ext34 + 14 ext4, fft 38 + 14, navier-stokes 16 + 4, darkroom 0 + 0
— consistent with the measured winners and with §17.4's archived candidate
blobs (byte-identical).

**v8-suite three-state on the frozen stack** (gate 5 worker check,
`bench/results/exec-throughput-r1-v8-20260825T0455/`): the opt and raw
streams are byte-identical to the §12.4 build's blobs (the deployed
pipeline is deterministic and unchanged), and on v8-suite-rt the two arms
are byte-identical *within* a run too (sha256 `439fd625…` both): that
fixture holds the suite body in a string literal and eval's it at runtime,
so the AOT artifact is a 141-byte loader shell (52 insns, 0 folds).
v8-suite-rt cannot measure the AOT optimizer at all — §12.4's +4.51% and
this stack's ≈ −0.5% (sequential 5-round median −0.40%, interleaved
9-per-mode median −0.64%) are both samples of that null vehicle's ±7%
noise band, not optimizer signal. The earlier "mul fast path overlap"
reading (that patch 0043 recovered raw's ground at mul sites) is
**retracted**: patch 0043 does accelerate runtime-compiled muls, but
equally in both arms of an identical blob. The valid vehicle is
v8-suite-mod (real module-level code, 17365 insns): −0.20% (§12.4 build)
and −0.04% (frozen stack). The deployed optimizer removes 30/17365 insns
(0.17%) there (P14 literal-folding 0, get_array_el specializable 0/199,
TDZ 14) — too little to move a ±7% vehicle, which is exactly why v8-suite
reads ≈ 0 with all optimizations on. v8-suite is 2008-era sloppy code with
none of the optimizer's target patterns; the deterministic evidence for the
shipped stack is the classic-corpus paired A/Bs: mul-only +3.366% (224 obs,
handoff), ext34-only +0.86% (this rerun), v1 compute fixtures
+38.94%/+29.09% (G3).

### 19.5 Final verdict

The optimization loop terminates: every ranked candidate either failed the
direct-binary gate (§18), is a runtime IC concern (call_method), or would
embed a generic slow path measured negative twice (R0, arg0>get_field).
Shipped configuration = BC26 + 0x7f + patch 0043; ext34 available via API
pass bits. Evidence archives: `bench/results/classic-profile-20260825T015948/`
(54 dumps + manifest), `bench/results/ext34-classic-ab-20260825T014012/`
(original §17.4 A/B), `bench/results/ext34-classic-ab-r1-20260825T035504/`
(tainted run, archived), `bench/results/ext34-classic-ab-r1-20260825T0510/`
(clean leave-one-out rerun), `bench/results/exec-throughput-r1-v8-20260825T0455/`
(v8-suite three-state). bench/results is gitignored; the numbers above are
the record.
