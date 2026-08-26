# QuickJS Optimization: Current State and Decision Record

This is the single maintained entry point for Capsid-specific QuickJS
optimization. It records the production configuration, the measured decisions
behind it, and the gate for future work. Implementation details for the
deployed BC26 rewriter live in [Bytecode AOT Rewriter](bytecode-aot-rewriter.md);
full benchmark tables live in [Performance Evidence](performance-benchmarks.md).
Historical plans and per-task execution logs belong in git history, not in the
maintained documentation set.

## 1. Production State

| Component | Production state | Reason |
|---|---|---|
| BC26 AOT rewriter (`kPassAll`) | enabled | Sound static reductions; positive classic-suite center and neutral library-suite aggregate |
| Upstream arithmetic/array inline fast paths | enabled | Existing handlers, no bytecode-format change |
| CFG and stack-to-SSA analysis | analyze-only | Useful for proofs and candidate ranking; no lowering today |
| Exact-site opcode profiling | instrumentation build only | Selection tool, never a production default |
| Upstream small-block arena (`9de2921`) | rejected | Direct final-binary portfolio regressed significantly despite identical BC26 |
| Upstream realloc-slack removal (`b16e7bd`) | rejected | BC26 stayed identical, but Classic, Web Tooling, and Hono were all neutral |
| Field inline-cache prototypes | rejected | Runtime quickening and BC26-preserving per-site sidecars both caused significant regressions |
| BC27 `get_arg0 + get_field` fusion | removed | Real-framework combined result was significantly negative |
| ext34 loc-read/array fusion | removed | Targeted wins did not survive enabled-binary product gates |
| Store/reload fusion | removed | Aggregate neutral with a significant FFT regression |

Default output is ordinary quickjs-ng bytecode version 26. Capsid now carries
no custom bytecode version, extension opcode, field-IC opcode, reader, handler,
or feature flag. The opcode profiler remains a separate instrumentation-only
patch and compiles to byte-identical QuickJS code when disabled.

The retained-set attribution—BC26 `kPassAll` plus mixed-number `mul`—measured
**+2.64% equal-weight gain** over eight balanced Kraken/Octane programs, with
an across-program 95% interval of **[-0.04%, +5.39%]**. The 18-program V8 Web
Tooling library portfolio measured **-0.49%**, interval **[-1.34%, +0.37%]**:
neutral overall, not a library-workload win. DFT and UglifyJS each had one
significant negative result; UglifyJS was confirmed as an optimizer-combination
effect, but disable-one tests did not identify a removable pass. This is a
qualified keep decision, not a promise of a gain for every application or HTTP
request. The runtime now carries the broader upstream add/sub/mul/div and
fast-array-read inline patch; the latest final-binary result below isolates the
physical deletion of the rejected custom-bytecode mechanisms.

## 2. What CFG+SSA Can and Cannot Do

The limitation is not the absence of an AST. QuickJS has already lowered the
source AST into stack bytecode, and many useful transformations can be proved
from bytecode CFG, stack height, local-slot flow, effects, and exception edges.
Capsid already uses those facts for constant propagation, copy propagation,
literal property folding, TDZ-sound dead-store removal, and compaction.

The practical constraint is the lowering target. The current tree has no
`OP_ext`, BC27 reader, or extension dispatcher; the analyze-only region score
models a hypothetical **direct** fused opcode only to estimate a local upper
bound. It is not an available lowering path and does not authorize a format
change.

- lowering back into unchanged BC26 can only select existing opcodes or delete
  work; quickjs-ng has already removed many shallow redundancies;
- the expensive remainder depends on runtime tags, shapes, prototypes,
  coercion, callees, accessors, proxies, and exceptions, so sound analysis must
  discard facts across many real program boundaries;
- a new fused opcode can express more, but it changes interpreter layout and
  must earn back its handler/dispatch cost across the whole product binary.

The retired generic SSI/SCCP/GVN/LICM layer illustrates the first point. It
removed only 2 additional instructions on a 12,645-instruction corpus (0.016%)
over the direct passes; LICM moved nothing even on its anchor fixture. CFG
remains valuable, but a general SSA framework is not itself a speedup. This
result rejects that BC26 lowering, not CFG+SSA as a proof and region-selection
framework.

The next useful lowering should collapse material work: several dispatches,
intermediate stack values, reference-count transfers, repeated guards, or a
helper call. Saving one cheap dispatch while still executing the full generic
property helper is below the current threshold.

The analyze-only implementation is also a resource boundary. Its original
ownership fixpoint allocated two dense `value_count` refcount arrays per CFG
block, which reached about 11.7 GiB RSS plus 2.9 GiB swap on Web Tooling
Prettier and caused WSL to kill the whole Codex process group. Refcounts are
now sparse per block and the portfolio runner independently applies a 2 GiB
`RLIMIT_AS`. The same complete Prettier bundle analyzes at about 736 MiB peak
RSS with zero swap (16,962 functions, zero rejection), a roughly 94% peak-RSS
reduction. A rejected parent function no longer hides independently analyzable
cpool children. Future whole-bundle analyses must retain both the internal
fail-closed budget and the external process limit.

### Choose the cheapest execution mechanism

For a common specialization that fits entirely inside one existing opcode,
prefer a short inline fast path in that opcode's interpreter handler. It keeps
old bytecode eligible, needs no compiler recognition or lowering, adds no
bytecode version, avoids an extra `OP_ext`/id dispatch, and can fall through to
the exact existing slow path. quickjs-ng commit `377a25e` is the reference
shape: ordinary `get_array_el{,2}` first checks object + integer index and reads
a regular or typed fast array directly; mixed numeric `add/sub/mul/div` stays
inside the original arithmetic handlers. Capsid backports those runtime paths
without changing its BC26 wire contract.

This is a mechanism preference, not a claim that inline code is layout-free.
It still grows `JS_CallInternal`, can move later hot code, and can hurt the
instruction cache when guards are large or rarely hit. An inline candidate
therefore needs a cheap guard, broad dynamic coverage, and direct final-binary
measurement. Keep rare or complex cases in the shared slow helper.

Use a new fused/superinstruction only when the value comes from crossing
instruction boundaries: removing several dispatches, stack shuffles,
load/store pairs, reference-count transfers, or repeated guards that no single
handler can see. The retired single-op array ext paid a new-format and dispatch
cost for work that belonged in the existing opcode; ext34 was a legitimate
cross-instruction fusion, but its measured wins did not repay final-binary
layout cost across the portfolio. The decision order is therefore:

1. optimize the existing generic helper if all callers benefit;
2. inline a small, common specialization in the existing opcode;
3. propose a direct fused opcode only for proven cross-instruction removable
   work; this is a separate wire/runtime project because no extension path
   exists in the current tree;
4. introduce feedback/IC only when a lower execution tier consumes it more
   cheaply than the generic interpreter path.

## 3. Why V8-Style IC Gains Do Not Transfer Directly

V8 can use feedback to generate or patch low-level code at the access site. A
stable receiver shape can therefore lead to a few machine instructions and a
direct branch to the slow path. The feedback is valuable because a later tier
consumes it.

Capsid's QuickJS path remains a C interpreter. An IC lookup still pays opcode
dispatch, C control flow, cache/state access, guards, and fallback plumbing.
QuickJS's generic property helper already contains dense-array and common own
property fast paths, so a cache wrapped around it may duplicate work rather
than remove it. Adding handlers can also perturb compiler layout enough to
move unrelated programs even when their bytecode is identical.

This does not mean interpreter ICs can never win. It means a future IC must:

1. identify feedback slots per bytecode site, never per atom;
2. keep the BC26 atom operand unchanged and use a direct PC-offset sidecar—no
   exact-PC hash lookup, bytecode mutation, or atom-shared ring;
3. make the compiled-OFF runtime byte/layout equivalent to patchless;
4. prove a direct patchless-to-enabled win on stable and unstable receivers;
5. remain bounded in memory and preserve canonical serialization and fallback
   semantics.

Without lower-level specialization or broader fused execution, a property IC
alone is unlikely to produce V8-scale gains.

### Shape invalidation is part of the cache key

QuickJS already has hidden classes through `JSShape`; a new parallel shape
system would duplicate state and create two invalidation authorities. For an
own ordinary data-property cache, structural mutation must invalidate lazily
through the guard rather than scanning all sites:

| Mutation after training | Required result |
|---|---|
| overwrite the same writable data property | shape and offset stay valid; read the new value |
| add or delete an own property | new/COW shape (or new monotonic shape id); old guard misses |
| change flags or data/accessor kind | old guard misses; accessor/autoinit is never installed |
| freeze, seal, or make non-writable | descriptor/shape change prevents a stale write hit |
| change the receiver prototype | receiver guard changes |
| mutate an object on the prototype chain | receiver shape may not change, so v1 must not cache prototype hits |
| proxy, exotic object, private field, or unknown path | always use generic semantics |
| free and reuse a shape address | raw weak pointers are forbidden; use a non-reused id or owned reference |

Every in-place shape mutation funnel—add, delete/compact, resize, descriptor
change, and prototype change—must advance a monotonic identity before the new
layout is visible. Counter wrap disables the cache. Directed tests must cover
`warm -> delete -> re-add`, data-to-accessor conversion, freeze-then-write,
prototype replacement, GC, and allocator address reuse. Prototype and negative
lookup caches require a separate chain/version design and are not part of the
first own-property experiment.

## 4. Measured Rejections

Only final, decision-relevant numbers are maintained here. Raw manifests and
samples remain under `bench/results/`; superseded intermediate numbers remain
in git history.

| Experiment | Best relevant evidence | Decision |
|---|---|---|
| R0 single-op array ext | Target fixture -12.69%; generic helper already had the same fast path | removed |
| Exact-site field IC | PATCHLESS→enabled fresh latency +7.31% regression, CI [+5.15%, +9.51%]; mono and Hono neutral | removed |
| Sparse BC26 per-site field IC | Eight-program center -1.11% [-5.03%, +2.97%]; Box2D -11.19% [-12.60%, -9.75%] | reverted after three-pair stop gate |
| BC27 `get_arg0 + get_field` | Four-framework equal-weight -1.28%, CI [-1.77%, -0.77%] | removed |
| Corrected ext34, same binary | Beat +9.72%, FFT +9.66%, Navier-Stokes +3.22% | mechanism proven |
| ext34 compiled-in OFF tax | -1.44% equal-weight, program CI [-2.50%, -0.37%] | product gate failed |
| ext34 patchless→enabled net | +1.51%, CI [-2.07%, +5.22%]; Box2D -2.21%, Richards -3.22% | removed |
| `put_loc*; get_loc* -> set_loc*` | +0.28%, CI [-0.68%, +1.25%]; FFT -0.81% significant | removed |
| Upstream small-block arena (`9de2921`) | system: -7.01%, across-program CI [-12.84%, -0.79%]; bounded mimalloc/Hono: -0.897%, paired CI [-1.293%, -0.501%] | removed |
| Upstream realloc-slack removal (`b16e7bd`) | Classic +0.02% [-0.44%, +0.49%]; Web Tooling -0.18% [-1.36%, +1.01%]; Hono paired +0.17% [-2.13%, +2.46%] | removed |
| Retained set on V8 Web Tooling | -0.49%, across-program interval [-1.34%, +0.37%]; UglifyJS -2.66% significant | keep aggregate; profile combination/layout next |

The field-IC prototype also showed why same-binary OFF/ON is insufficient. The
feature-built OFF path itself carried roughly 2-3% centers in directed tests.
After fixing terminal observers, the catastrophic fresh-receiver result
improved, but the direct patchless-to-enabled product comparison still failed.

A second prototype removed those earlier confounders: it preserved BC26
operands and serialization, never mutated bytecode, keyed feedback by exact
PC, used bounded lazy sidecars, and served only MONO/POLY2 own-data hits
guarded by a monotonic shape identity. Its compiled-OFF QuickJS object and
`qjs` binary were byte-identical to patchless, all eight compared bytecode
blobs were identical, and its correctness and sanitizer gates passed. It
still failed the first final-binary stop gate: the eight-program center was
-1.11% (95% across-program interval [-5.03%, +2.97%]), with Box2D
significantly regressing 11.19% ([-12.60%, -9.75%]) and Oscillator regressing
1.40% ([-2.12%, -0.68%]). The seven-pair, Web Tooling, and Hono runs were
therefore not started. Evidence is under
`bench/results/per-site-field-ic-rejected-20260826/`.

These data do not prove that per-site IC is intrinsically ineffective. They
reject the tested implementation: hotness observation remained in the generic
`get_field` path, functions gained lazy IC fields and runtime bookkeeping,
sites quickened by mutating bytecode, and misses/restoration perturbed fresh
receivers. The older quickjs-ng IC was worse for locality: an atom-keyed linked
hash selected a fixed four-shape ring shared by unrelated PCs, retained shape
references, scanned/moduloed the ring, and wrote replacement state. Upstream
removed it because results were mixed and memory was always higher. A future
attempt must avoid placing an observer/probe on every ordinary `get_field` and
must select genuinely hot sites before they enter a serving path.
Encounter-order admission of the first eight eligible sites is not a hot-site
policy. Without such a selection vehicle, field IC is deprioritized; it must
not revive either old structure.

The upstream small-block arena is also rejected on the current pinned runtime,
not assumed beneficial from upstream's headline score. A clean Release+LTO,
system-allocator comparison held the rewriter and eight BC26 blobs identical
and changed only quickjs-ng's arena/object-header layout. The equal-weight
Kraken/Octane result was -7.01%, with an across-program 95% interval of
[-12.84%, -0.79%]. DFT (-10.21%), Oscillator (-14.53%), Darkroom (-2.40%),
Box2D (-9.06%), Navier-Stokes (-1.59%), and Richards (-17.73%) all regressed
significantly; Beat (+0.84%) and FFT (+0.52%) improved.

The production interaction was then measured instead of inferred. Two isolated
Release+LTO workers both used mimalloc 3.2.7 with its initial reserve bounded to
32 MiB; their source trees differed only by patch 0039 and its overlay identity.
Across seven interleaved Hono `/fixed` pairs, the arena worker was slower in all
seven: QPS fell 0.897%, with a paired 95% interval of [-1.293%, -0.501%], while
p50 latency rose 1.285%. All 14 measured rounds had zero errors, timeouts and
response mismatches. Mimalloc therefore reduced the arena loss substantially
but did not reverse it. The stop gate removed patch 0039 without spending more
portfolio time on a mechanism already significantly negative in the production
allocator configuration. Decision evidence is stored in
`bench/results/upstream-arena-20260825/classic/` and
`bench/results/arena-mimalloc-hono-20260826-v2/`.

The realloc-slack feedback removal from upstream `b16e7bd` was also tested in
the production allocator configuration. It eliminated the usable-size query
after QuickJS reallocations without changing opcode definitions or serialized
bytecode: 59/59 Classic and Web Tooling blobs were byte-identical. The final
Release+LTO+mimalloc binary was neutral on all three product views: +0.02% on
eight Classic programs (95% interval [-0.44%, +0.49%]), -0.18% on all 18 Web
Tooling workloads ([-1.36%, +1.01%]), and +0.17% paired Hono QPS
([-2.13%, +2.46%]). Darkroom (+0.87%) and JSHint (+5.12%) were individually
positive, but the pre-registered positive combined interval gate was not met.
The patch was therefore reverted rather than retained for isolated wins. The
decision evidence is under
`bench/results/upstream-drop-realloc-slack-20260826/`.

The corrected ext34 result is equally important: multi-instruction fusion can
win even when its slow path eventually calls the generic helper. Beat and FFT
proved the mechanism. It was rejected because merely compiling the handlers
changed broad runtime performance and the net build significantly regressed
Box2D and Richards. New handlers must pass the final-binary gate, not only a
same-binary pass-mask comparison.

The ext history also separates mechanism from vehicle. R0 was a single-opcode
array specialization that duplicated the generic handler's existing fast path
and added dispatch, so its -12.69% rejects that mechanism. The two-byte
`get_arg0 + get_field` extension saved no dispatch because `OP_ext + id` still
occupied two dispatch slots. Corrected ext34 did remove two or three primary
dispatches and produced repeatable same-binary gains of about 9.7% on Beat and
FFT. Its rejection was a product-binary result: compiled-in handlers shifted
interpreter layout (-1.44% OFF tax), and direct patchless-to-enabled testing
regressed Box2D and Richards. Therefore “all new opcodes lose” is not supported;
the supported rule is that every handler must pay for both local execution and
whole-binary layout, with a true patchless comparison.

The V8-suite record contains a separate measurement failure. `v8-suite-rt`
stores the suite in a string and evaluates it at runtime, so its raw and AOT
bytecode are the same small loader; its old +4.51% optimizer claim was noise.
`v8-suite-mod`, whose benchmark code is present in the serialized module, is
the valid AOT vehicle. Runtime opcode profiling may still use the `-rt` form,
but AOT attribution must use `-mod`.

The rejected extension and IC foundations are physically deleted, not hidden
behind production-off flags. This restores a single BC26 reader/runtime and
keeps experimental layout out of every shipping binary. The deletion-only
final-binary gate was neutral on eight Kraken/Octane programs (+0.20%,
across-program 95% CI [-1.45%, +1.88%]) and significantly positive across all
18 V8 Web Tooling workloads (+1.14%, CI [+0.80%, +1.48%]); no program had a
significant regression, and all 26 BC26 pairs were byte-identical.

## 5. Candidate Selection and Lowering Gate

A proposal is eligible for implementation only when all of the following are
true:

- exact-site profiles bind bundle, function, and bytecode PC and exclude
  bootstrap execution;
- the pattern occurs across representative frameworks or classic suites, not
  only its anchor microbenchmark;
- every member of a proposed region has profile evidence and the region is
  within one basic block unless exceptional control flow is modeled exactly;
- the static ceiling and dynamic hotness can exceed noise after accounting for
  generic-helper cost;
- the lowering preserves exception PC, coercion order, reference counting,
  stack effects, and debug-line mapping;
- the candidate is compared directly against a patchless runtime with balanced
  pairs, correctness, resource measurements, and both-side profiles;
- compiled-but-disabled code has a pre-registered zero-tax gate.

Prefer guard-free semantic fusion over speculative type paths. For example, a
sequence may reuse QuickJS's complete numeric/string/BigInt/object slow path
and only fuse the successful result transfer into a local. Such a lowering
does not need guard/deopt machinery. Before implementing it, however, inspect
the emitted bytecode: QuickJS already lowers common assignment forms to
`add_loc`, and the measured corpus had no residual site for the proposed
`add; dup; put_loc; drop` fusion.

The source-attributed census must include real frameworks, broad classic
suites (Kraken, Octane, and SunSpider where compatible), and library-level
tooling work from V8 Web Tooling Benchmark. Suite results are a portfolio:
partial wins are kept when the final binary has no significant regression,
but benchmark-specific rewrites are not accepted merely because their anchor
improves.

### 2026-08-26 cross-portfolio rerank

Raw execution counts from time-budgeted suites are not comparable. The current
rank therefore orders candidates by portfolio breadth and program breadth,
then reports each program's matched-instruction share using only exact sites
from the selected application source. Across microfixtures, `v8-suite-mod`,
Kraken/Octane/SunSpider, V8 Web Tooling, and four frameworks:

| Candidate | Portfolio/program breadth | Weighted matched-instruction share |
|---|---:|---|
| `get_length; lt` | 5/5, 58 programs | V8 0.035%; classic 1.427%; Web Tooling 1.117%; frameworks 4.546% |
| own-data `get_field` | 5/5, 44 programs | V8 0.687%; classic 2.099%; Web Tooling 0.617%; frameworks 0.090% |
| own-data `put_field` | 4/5, 24 programs | at most 0.025% in any broad portfolio |
| `add; push_0; shr; dup` | 1/5, 3 programs | frameworks 8.899%; absent elsewhere |

This rejects `get_length; lt` as the first broad product optimization despite
its excellent occurrence breadth: a fusion saves only part of the matched
share, so its V8 ceiling is effectively zero and its broad classic/tooling
ceiling is around one percent. The framework-specific unsigned sequence
remains a separate local candidate, not a universal opcode justification.

The own-data field result required a second profile dimension before any IC
implementation. Raw `JSShape *` is not a valid identity: QuickJS mutates
exclusive shapes in place and the allocator reuses freed addresses. The
instrumentation build now assigns a monotonic 32-bit identity on every shape
creation, clone, resize, compact, property addition, and prepare-update
mutation; wrap disables collection rather than permitting a false hit. Each
exact site records its first two identities and classifies any further
identity as megamorphic. This reuses the invalidation model previously proven
by the 27-row shape-guard matrix, but remains profiling-only and compiles out
of production.

The complete stable-ID portfolio is deliberately a selection result, not a
keep decision. Instrumentation completed 36/41 Classic programs (five
oversized programs hit the bounded collection timeout), all 18 Web Tooling
workloads, and all four framework differential workloads:

| Portfolio | completed | Direct own `get_field` share | mono | mono or poly2 |
|---|---:|---:|---:|---:|
| Classic | 36/41 | 7.00% | 65.05% | 66.66% |
| V8 Web Tooling | 18/18 | 2.74% | 51.84% | 61.69% |

Representative programs explain the aggregate:

| Program | Direct own `get_field` / selected-source instructions | mono | mono or poly2 | Top-site concentration |
|---|---:|---:|---:|---:|
| Octane Box2D | 21.39% | 94.56% | 99.26% | 0.70% |
| Web Tooling Babel | 5.13% | 89.73% | 91.19% | 51.40% |
| Web Tooling Esprima | 4.89% | 8.93% | 8.93% | 6.13% |
| Hono differential | 0.29% | 20.83% | 53.83% | 7.03% |
| Elysia differential | 1.80% | 31.92% | 37.62% | 0.88% |
| H3 v2 differential | 0.11% | 38.27% | 44.23% | 2.71% |
| itty-router differential | 1.69% | 7.13% | 7.32% | 68.05% |

The data explain the old mixed IC result: Box2D and Babel contain apparently
valuable stable sites, while Esprima and itty-router are overwhelmingly megamorphic
under mutation-sound identity. A global always-on observer/cache cannot serve
both shapes efficiently. Any next IC experiment must therefore be sparse,
per-PC, allocate only after a hot-site threshold, stop writing after reaching
MONO/POLY2, and permanently bypass megamorphic sites. The isolated prototype
met those structural requirements and the compiled-OFF/BC26 identity gates,
but Box2D then regressed 11.19%. High monomorphism alone is therefore not a
benefit proof: generic own-property lookup is already compact, shape identity
maintenance is global, and probing all `get_field` sites can cost more than
the selected hits save.

## 6. Required Validation

The primary semantic gates are quickjs-ng's own `tests/`, the pinned test262
revision, and Capsid's bytecode/worker tests. Benchmark completion is not a
correctness substitute. Every production change must pass:

- native QuickJS tests and pinned test262 on the patchless and candidate VM;
- an AOT test262 adapter for rewrites/opcode emission: compile each supported
  test and harness include, rewrite the serialized function graph, then execute
  it on the candidate VM. Ordinary source-mode test262 alone does **not** enter
  Capsid's AOT emission path;
- bytecode parse/verify/round-trip and source-versus-bytecode differential
  tests;
- exact exception/backtrace and getter/coercion/proxy cases;
- ASan/UBSan fuzzing and invalid operand/frame-index rejection;
- balanced raw-versus-rewritten or patchless-versus-enabled runtime samples;
- full source-service correctness, resource, cold-start, and separated
  host/worker profile gates when the runtime binary changes;
- deterministic output and compatibility-identity checks.

The repository does not yet contain the optimized-test262 adapter. Until it
does, a new serialized opcode or CFG+SSA lowering is blocked from a keep
decision even if source-mode test262 is green. Initial implementation may run
a documented supported subset, but skips must be classified (module/harness,
host feature, negative parse test) and its baseline must be compared with the
same pinned quickjs-ng revision rather than silently counted as passes.

The classic-suite completion marker proves successful execution, not complete
semantic output equivalence. Correctness claims must come from the dedicated
differential and conformance gates.

## 7. Reproduction Map

| Purpose | Entry point |
|---|---|
| Deployed rewriter tests | `build-m1d/test-bytecode-rewriter` |
| Raw/source/rewritten execution | `bench/exec-throughput.sh` |
| Four-stack product matrix | `bench/compare-four-qps.sh` |
| Host/worker profiles | `bench/profile-four-stacks.sh` |
| Cold start | `bench/cold-start.sh` |
| Classic-suite balanced A/B | `bench/classic-suite-ab.py` |
| Web Tooling corpus | `bench/prepare-web-tooling.py` |
| Web Tooling balanced A/B | `bench/web-tooling-ab.sh` |
| Profile → CFG+SSA portfolio | `bench/profile-region-portfolio.py` |
| Cross-portfolio breadth/share rank | `bench/profile-region-rank.py` |
| Stable-ID shape analysis | `bench/profile-shape-stability.py` |

The Web Tooling preparer records the exact upstream revision, package-lock
identities, resolved top-level dependency versions, bundle hashes, and build
tool versions. It creates one static bundle per workload and calls the
upstream workload function once per fresh process; the outer A/B runner owns
repetition and timing. This avoids both the upstream dynamic-require all-in-one
bundle and nested Benchmark.js samples. A reproducible starting point is:

```sh
git clone https://github.com/v8/web-tooling-benchmark /tmp/web-tooling-benchmark
git -C /tmp/web-tooling-benchmark checkout 4a12828c6a1eed02a70c011bd080445dd319a05f
npm --prefix /tmp/web-tooling-benchmark install --ignore-scripts
python3 bench/prepare-web-tooling.py \
  --web-tooling /tmp/web-tooling-benchmark \
  --out /tmp/capsid-web-tooling-corpus-v1
CORPUS=/tmp/capsid-web-tooling-corpus-v1 bash bench/web-tooling-ab.sh
```

Run the correctness-only corpus gate before a long paired measurement:

```sh
SMOKE_ONLY=1 CORPUS=/tmp/capsid-web-tooling-corpus-v1 \
  bash bench/web-tooling-ab.sh
```

This 2019 suite is useful for parser, compiler, formatter, minifier, and
source-map workloads, but it is not a substitute for current framework and
source-service gates.

The authoritative retained-set evidence is
`bench/results/all-effective-cumulative-20260825/` for Kraken/Octane and
`bench/results/web-tooling-current-20260825/` for V8 Web Tooling. The clean
direct execution, source-service, profile, cold-start, and current rewriter
checksum identities are listed in
[Performance Evidence](performance-benchmarks.md).

## 8. Next Optimization Direction

Do not add another opcode that saves only one cheap dispatch, and do not revive
either field-IC observer. The next round should proceed in this order:

1. profile a representative workload portfolio with exact-site attribution;
2. rank multi-instruction regions by removable runtime work, not frequency
   alone;
3. prefer upstream changes that shorten existing handlers or replace a costly
   C/API implementation without adding a steady-state observer;
4. reject it immediately if compiled-OFF is not patchless-equivalent;
5. keep it only if the direct final-binary portfolio has no significant
   regression and a positive combined interval.

The current evidence does not select a production optimization yet. It rejects
the broad `length_lt` fusion on normalized ceiling, retains the framework-only
unsigned sequence as a local candidate, and now also rejects the
BC26-preserving sparse MONO/POLY2 field IC. Field specialization should not be
retried until a profile-guided or sampled selection vehicle can make
non-selected sites pay no steady-state observer probe. The immediate search
returns to upstream existing-handler and API-level changes that preserve BC26
and can compile out cleanly when rejected.
