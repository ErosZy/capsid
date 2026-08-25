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
| Field inline cache | removed | Direct patchless comparison regressed fresh receivers |
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

The practical constraint is the lowering target:

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
remains valuable, but a general SSA framework is not itself a speedup.

The next useful lowering should collapse material work: several dispatches,
intermediate stack values, reference-count transfers, repeated guards, or a
helper call. Saving one cheap dispatch while still executing the full generic
property helper is below the current threshold.

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
3. introduce fusion/ext only for proven cross-instruction removable work;
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

1. allocate feedback slots per bytecode site at compile time;
2. encode the slot directly in the operand—no exact-PC lookup or atom-shared
   ring;
3. make the compiled-OFF runtime byte/layout equivalent to patchless;
4. prove a direct patchless-to-enabled win on stable and unstable receivers;
5. remain bounded in memory and preserve canonical serialization and fallback
   semantics.

Without lower-level specialization or broader fused execution, a property IC
alone is unlikely to produce V8-scale gains.

## 4. Measured Rejections

Only final, decision-relevant numbers are maintained here. Raw manifests and
samples remain under `bench/results/`; superseded intermediate numbers remain
in git history.

| Experiment | Best relevant evidence | Decision |
|---|---|---|
| R0 single-op array ext | Target fixture -12.69%; generic helper already had the same fast path | removed |
| Exact-site field IC | PATCHLESS→enabled fresh latency +7.31% regression, CI [+5.15%, +9.51%]; mono and Hono neutral | removed |
| BC27 `get_arg0 + get_field` | Four-framework equal-weight -1.28%, CI [-1.77%, -0.77%] | removed |
| Corrected ext34, same binary | Beat +9.72%, FFT +9.66%, Navier-Stokes +3.22% | mechanism proven |
| ext34 compiled-in OFF tax | -1.44% equal-weight, program CI [-2.50%, -0.37%] | product gate failed |
| ext34 patchless→enabled net | +1.51%, CI [-2.07%, +5.22%]; Box2D -2.21%, Richards -3.22% | removed |
| `put_loc*; get_loc* -> set_loc*` | +0.28%, CI [-0.68%, +1.25%]; FFT -0.81% significant | removed |
| Retained set on V8 Web Tooling | -0.49%, across-program interval [-1.34%, +0.37%]; UglifyJS -2.66% significant | keep aggregate; profile combination/layout next |

The field-IC prototype also showed why same-binary OFF/ON is insufficient. The
feature-built OFF path itself carried roughly 2-3% centers in directed tests.
After fixing terminal observers, the catastrophic fresh-receiver result
improved, but the direct patchless-to-enabled product comparison still failed.

The corrected ext34 result is equally important: multi-instruction fusion can
win even when its slow path eventually calls the generic helper. Beat and FFT
proved the mechanism. It was rejected because merely compiling the handlers
changed broad runtime performance and the net build significantly regressed
Box2D and Richards. New handlers must pass the final-binary gate, not only a
same-binary pass-mask comparison.

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

## 6. Required Validation

Every production change must pass:

- native QuickJS tests and pinned test262 where applicable;
- bytecode parse/verify/round-trip and source-versus-bytecode differential
  tests;
- exact exception/backtrace and getter/coercion/proxy cases;
- ASan/UBSan fuzzing and invalid operand/frame-index rejection;
- balanced raw-versus-rewritten or patchless-versus-enabled runtime samples;
- full source-service correctness, resource, cold-start, and separated
  host/worker profile gates when the runtime binary changes;
- deterministic output and compatibility-identity checks.

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
the atom-shared IC ring. The next round should proceed in this order:

1. profile a representative workload portfolio with exact-site attribution;
2. rank multi-instruction regions by removable runtime work, not frequency
   alone;
3. implement one guard-free fusion or a compile-time per-site feedback-slot
   prototype in an isolated build;
4. reject it immediately if compiled-OFF is not patchless-equivalent;
5. keep it only if the direct final-binary portfolio has no significant
   regression and a positive combined interval.

The current evidence favors multi-instruction semantic fusion before another
property IC attempt.
