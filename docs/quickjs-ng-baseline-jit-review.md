# Independent Review — QuickJS-NG Baseline JIT ("Capsid")

Reviewer: Claude (independent second opinion, requested via
`CLAUDE_REVIEW_REQUEST.md`).
Target: QuickJS-NG v0.16.2, base commit `1ab8676f`, experiment branch
`codex/capsid-baseline-jit-v0.16.2`.
Citations are relative to the review package
(`source/quickjs-ng-0.16.2-working-tree/…` for source,
`evidence/…` for measurements) unless noted.

Every claim is tagged **[F]** verified fact (I read the file/number),
**[I]** inference (my reasoning from facts), or **[S]** suggestion.

---

## Verdict up front

**KEEP AS RESEARCH BASE.**

The STOP decision for the incremental baseline-JIT route is correct and unusually
well-evidenced. ABANDON would throw away a genuinely valuable correctness /
differential / profiling harness. PIVOT-WITH-NEW-ARCHITECTURE is *not yet*
justified: the one direction with real headroom left is unmeasured, and
committing to it now would repeat the exact mistake this project already
diagnosed in itself (building an architecture before proving Amdahl headroom).
The right next action is a single bounded discovery experiment (Q4). If it clears
its gate, *then* pivot — to an interpreter-level change, not a bigger JIT.

---

## What makes the JIT lose (the one-paragraph mechanism)

**[I]** On Babylon the interpreter (`Q0`) spends ~55% of CPU time *inside the
dispatch loop body* and ~25% in the property runtime. A baseline JIT that keeps
interpreter-compatible frames (for precise side exits) and calls the same C
helpers can only *relocate* the dispatch cost — which is cheap — while it
*duplicates* the property cost and *adds* a frame/entry/guard tax. The measured
result is exactly that: `Bcore` moves 7.3 s out of the interpreter body but adds
12.2 s of new work elsewhere, netting +10.9% slower. The JIT is competing with an
already-tight computed-goto interpreter on the same operations, so it starts in a
hole it cannot climb out of by copying more fast paths.

---

## Independent re-profile (this reviewer, callgrind)

I re-profiled to check the story with my own hands. **`perf` is not usable in
this environment** — it is a Firecracker/docker microVM (`uname -r` =
`6.18.44-fc-v24`), no PMU is exposed, `linux-perf` has no install candidate, and
`perf_event_paranoid=2`. So I used **valgrind/callgrind 3.22**, which needs no
PMU and, unlike `perf`, is **deterministic** (instruction reads, `Ir`, not
time-samples). I built the packaged tree myself (`quickjs.c` SHA `23d4a59e…`
verified; RelWithDebInfo; capsid ON) and drove **both** modes from that **one
binary** — `Q0` = `--capsid off`, `H0` = the read/write-IC preset — matching the
authoritative same-binary principle.

**Caveats, stated plainly.** `Ir` is instruction count, not cycles or wall-time
(cache/branch effects differ). And **the frozen Babylon/Terser bundles are not in
the package** (they were git-ignored, referenced only by SHA), so I used
parser/compiler-shaped proxies, not Babylon. This corroborates *mechanism and
family exposure*; it does **not** reproduce the authoritative `-2.493%` throughput
number, which needs Babylon on a clean timing host.

### Finding A — Q0 interpreter decomposition (a recursive-descent parser + AST transform; 16.9 B Ir)

| Bucket | Ir share | What is in it |
| --- | ---: | --- |
| `interp-core` (`JS_CallInternal` self) | **28.4%** | dispatch + **inline** arithmetic and RC inc/dec |
| `property` | **25.1%** | `JS_DefineProperty` 4.5, `add_shape_property` 3.8, `add_property` 3.8, `JS_CreateProperty` 3.2, `find_own_property` 3.0 … |
| `alloc-gc` | 16.5% | arena malloc/realloc/free |
| `arith-other` | 15.9% | `js_relational_slow`, `js_strict_eq*`, coercion, `js_poll_interrupts` … |
| `rc-free` (**out-of-line only**) | **7.2%** | `JS_FreeValue` 2.4, `free_object` 1.2, `js_free_shape` 1.1, `set_value` 1.0, `free_property` 0.9 … |
| `atom-string` | 6.4% | atom intern, string alloc/compare |

Two numbers matter for the direction in Q3/Q4. **Property is ~25%**, matching
codex's Babylon `property-runtime` 24.55% almost exactly — good cross-workload
agreement. **Out-of-line reference counting is 7.2%** — *higher* than codex's
Babylon `rc-free` 5.21%, and this **excludes** the inline inc/dec folded into the
28.4% `JS_CallInternal` self-cost. So total RC traffic is comfortably a ≥5%,
plausibly ≥10% family. That is the empirical basis for the Q4 experiment: RC is
real, large, and — unlike property — never directly attacked.

### Finding B — the JIT's focused best case works (monomorphic `object.value` loop, 4 M reads)

`H0` executes **32% fewer instructions** than `Q0` (1.16 B vs 1.72 B). The
compiled loop runs as generated machine code (71% of `H0`, an anonymous RX
region) with the shape-guard+load **inlined**; `capsid_object_op` is essentially
unused (0.00%, ~4.5 K Ir). This confirms the fast-path mechanism is sound when a
site is monomorphic and stable — exactly codex's focused wins.

### Finding C — the JIT cannot stay resident on realistic parser code

Running `H0` on the parser workload: `seen=8, compiled=2, disabled=8,
side_exits=64, ic_hits=0`. It compiles two functions, immediately side-exits, and
**disables itself**. This independently reproduces the focused-win / macro-loss
split: the JIT helps only where Finding B holds, and real parser/compiler code —
polymorphic shapes, recursion, calls, string work — is Finding C, not B. That is
the same conclusion codex reached on Babylon, arrived at from a different tree and
tool.

Raw bucketed output is saved alongside this file as
`reprofile_q0_buckets.txt` and `reprofile_microbench_and_macro.txt`.

---

## Q1 — Is the STOP decision justified? Any methodological error that could reverse it?

**Yes, justified. No error large enough to reverse it; two caveats that bound its
scope rather than flip it.**

### Why STOP is sound

**[F]** The authoritative comparison is the strongest design available: one
frozen binary (`p0a1-frozen-qjs`, SHA `87ba1ea4…`) runs *both* modes, `Q0` via
`runtime=off` and `H0` as the maximal preset, so binary identity and link layout
are fully controlled (`H0_PRODUCT_REVIEW_20260904.md` §3.1; `EVIDENCE_GUIDE.md`
"Binary roles"). Babylon, CPU 2, ASLR off, 16 scored pairs (8AB/8BA):
median `-2.493161%`, geomean `-2.441255%`, AB `-2.553461%`, BA `-2.252073%`,
stratified-bootstrap 95% CI `[-2.972947%, -1.956343%]`, **15/16 pairs negative**
(§3.3). An independent 4-pair pilot was also negative (`-2.62%` median, §3.2).

**[F]** To reach the product goal of Q0 +2% from here needs +4.608% throughput on
`H0` (`REVIEW_FACTS.json` → `required_h0_gain_for_q0_plus_2_percent`). That is a
4.6-point swing over a result whose entire 95% CI is negative. Nothing in the
evidence closes a gap that size.

**[F]** The route was closed the right way: the last object-budget selector
*passed* its mechanism gate (+50,921,978 read-IC hits, 30 fully-stable named-read
sites) but *failed* the pre-registered throughput gate — pairs
`+2.77 / -4.28 / +3.08 / +1.53%`, geomean `+0.7286%`, AB/BA `+2.93% / -1.38%`,
95% CI `[-0.756%, +2.303%]` — and the negative sample was kept, not discarded
(§6.4). Refusing to delete an unexplained −4.28% outlier or to rerun for a
favorable draw is exactly correct.

### Two caveats (bound the scope; do not reverse the verdict)

1. **[I] Workload scoping is a product decision, not a pure measurement.** Babylon
   is a parser/compiler bundle — megamorphic shapes, low hot-loop fraction, heavy
   allocation and reference-count churn. That is close to the *worst case* for a
   type-feedback JIT, and the best case for an already-fast interpreter. The
   project's own focused numbers show where a JIT *would* shine: dense-array read
   `+58%`, local unboxing `+86%`, compare→branch `+45%` focused
   (`BASELINE_JIT_FINAL_CONCLUSION.md`; `O15_DEEP_RETROSPECTIVE…` §2) — all on
   numeric/loop code, all diluted to ~0 on Babylon. So the honest scope of STOP is:
   *"a helper-parity baseline JIT does not beat the interpreter on
   parser/compiler-shaped Web-Tooling workloads."* It does **not** establish that a
   JIT loses on numeric/compute embeddings. The reversal condition is explicit and
   narrow: **if the real product workload is numeric/compute-bound rather than
   parser-bound, the gate binary is wrong and the conclusion must be re-taken on
   that workload.** Given this repo's actual purpose (an untrusted-code data plane
   for Fetch handlers — see `README.md`), parser/serializer/validator shapes are
   the realistic target, so Babylon is defensible; but the choice should be stated
   as a choice.

2. **[F/I] Unmodeled time drift.** The review itself notes the last 8 pairs were
   more negative than the first 8 and the bootstrap does not model the drift
   (§3.3). Both halves are negative, so the FAIL is safe, but the *magnitude*
   (−2.5%) carries an unmodeled component and should not be quoted to three
   decimals as a stable point estimate.

**[I]** Net: STOP is robust. The only thing that could reverse it is a different
*product workload*, not a different analysis of this one.

---

## Q2 — Largest architectural costs of this baseline JIT (from code + profiles)

Ordered by evidenced magnitude. All Q0 shares from
`evidence/profile_results/r1a_f0_babylon_q0_bcore_cpu2_3pairs_20260904/`
(pair-001 baseline, `cpu-clock:u`, 997 Hz); all Bc−Q0 deltas are 3-pair means
from that directory's `summary.json`.

### 1. Property work is duplicated, not removed — the single biggest cost. **[F]**

QuickJS-NG's interpreter has **no per-site inline cache**. `OP_get_field` does an
*inline* `find_own_property` hashed-shape probe plus prototype walk directly in
the dispatch body (`quickjs.c:21411-21434`); `js_dup`/`JS_FreeValue` are inline.
No call, no atomic, no PC store on the hit path.

The JIT's equivalent, `capsid_object_op`, is an **out-of-line** helper — it is
`CAPSID_TEXT`, i.e. `__attribute__((noinline))` (`quickjs.c:73`, def at `:19440`).
Even a *cache hit* pays: a non-inlined call; an `atomic_load_explicit(…,
memory_order_acquire)` on the site state (`capsid_ic_probe`, `:19427`); a
mandatory `sf->cur_pc = …` store for side-exit safety on **every** call
(`:19519`); then the same `js_dup` + `JS_FreeValue` (`:19530-19533`). A per-site
shape-pointer compare *should* be cheaper than the interpreter's hash probe, but
wrapped in call + atomic + PC-store it is not.

The profile confirms the JIT spends **more** total time on property than the
interpreter: Q0 `property-runtime` = 12.90 s (24.55%); under Bcore
`property-runtime` *rises* +2.05 s **and** a new `capsid-property-helper` adds
+3.64 s — property work goes from ~12.6 s to ~18.3 s
(`R1A_BASELINE_DEBT_FINDINGS.md` "Focused accounting", combined +5.688733 s).
This is the core architectural defect: the fast path does not *replace* the
helper, it *precedes* it, so every non-fast case pays both.

### 2. A structural frame/entry/guard tax that cannot go to zero. **[F/I]**

Because JITted frames must stay interpreter-compatible so any guard can side-exit
into the exact bytecode, the frame cannot be slimmed. Measured on Babylon, pure
`Bcore` adds `capsid-entry-dispatch` +1.40 s, `generated-anonymous-rx` +3.05 s
(the generated code itself), and `capsid-call-helper` +0.57 s — new cost that did
not exist in `Q0`. Same-binary `Q0→Bcore` is ~−6.35% (`CURRENT_HANDOFF…` §1),
i.e. the base tax *before* any specialization is ~6 points. Entry cost is real but
amortizable: `M1_ENTRY_MEASUREMENT.md` gives a ~54 ns/call end-to-end intercept
and a **−3.67 ns per dynamic-op** slope (bigger bodies repay entry). Babylon's
functions are apparently not big enough to repay it.

### 3. The dispatch it removes is the cheap part. **[F/I]**

`Bcore` cuts `interpreter-call-core` by only 7.26 s of its 28.81 s (Q0 54.84%).
The rest of that bucket — inline arithmetic, stack moves, reference counting, and
inline call setup — the JIT *reproduces* with the same semantics
(`SOURCE_AND_CODE_NOTES.md`: "consuming/retaining opcode reference count
semantics"). Removing a computed-goto jump is worth little when the work between
jumps is unchanged.

### 4. Feedback/IC state carries atomic + budget machinery — and it is fragile. **[F]**

The tiering/sampling/budget system (`capsid/tier.c:1226-1309, 1611-1730,
1896-2058`) is where the final selector failed. Its documented root cause is a
pure bookkeeping bug class: a 16-site config still used the original 4-site
512-sample budget; ineligible traffic burned budget *before* eligibility was
known; and a successful recompile's fresh budget was never published back to the
stable slot (`SOURCE_AND_CODE_NOTES.md` "Mechanism root cause"). That the
throughput result swung on a budget-publication detail shows how much end-to-end
performance rides on fragile feedback plumbing.

### 5. Code-size / mapped-memory growth scales with specialization. **[F]**

`h0-sites16` added +65,536 bytes *mapped* for +6,260 bytes *live*
(`H0_PRODUCT_REVIEW…` §6.3); O8 dense-read grew generated code +1.245%
(`BASELINE_JIT_FINAL_CONCLUSION.md`). Every specialization that claws back debt
enlarges the resident code footprint — a real cost for an embedded,
many-worker data plane.

---

## Q3 — Is there a genuinely different architecture that could reach Q0 +2% without unacceptable risk?

**A full optimizing JIT (DFG/SSA/inlining) is the textbook answer and I do *not*
recommend it.** Two independent reasons, both from this evidence, not intuition:

- **[F]** Its main lever has no headroom here. The `O15` plan correctly makes hot
  monomorphic JS-call inlining the consumer for the whole SSA/effect/snapshot
  machinery (`O15_DEEP_RETROSPECTIVE…` §6, R2–R4). But the symbolized 499 Hz
  architecture profile found that the **only** family independently above 5% of
  sampled time on Babylon is `capsid_object_op` (~8.5%); every call/non-call
  generated-code family is below 5% (`H0_PRODUCT_REVIEW…` §6.2). Per the project's
  own R1 go-gate, the call-inlining DFG is gated *out*. And Babylon has 70.7 M
  method calls yet call dispatch is <5% of time — the earlier `call+drop`
  experiment already showed removing the call shell alone yields nothing
  (`O15…` §3 root-cause 3).
- **[I]** It is exactly the risk profile the request excludes: unbounded code
  growth, W^X churn, new GC-rooting surface at every snapshot, and a large
  permanent maintenance burden — for a workload where the profile says the payoff
  is sub-threshold.

**The genuinely different direction that the evidence *does* point to is to stop
generating code at all and optimize the interpreter.** The whole Capsid thesis is
"remove dispatch by emitting native code." The profiles show dispatch removal
doesn't pay. So invert it: attack the two costs the JIT never removed, *in the
interpreter*, where there is no frame tax, no W^X, no new GC rooting, and minimal
code-size cost. Two candidate mechanisms:

- **[I] Reference-count / value-move elision as a static bytecode analysis.**
  QuickJS reference-counts pervasively; `rc-free` alone is 5.21% of Q0 Babylon
  (independently, 7.2% out-of-line on my parser proxy — see *Independent
  re-profile*, Finding A), and the inc/dec arithmetic is *additionally* smeared
  through the 54.84% `interpreter-call-core` bucket. Many operand-stack `js_dup`/`JS_FreeValue` pairs
  provably cancel (e.g. `OP_get_field2` dups a value at `quickjs.c:21467/21490`
  that the very next consuming op frees). A per-basic-block stack-ownership pass,
  run once at `js_create_function`/bytecode-prepare time and consumed by a handful
  of "borrow" opcode variants, could elide the cancelling pairs. It benefits the
  interpreter directly and would *also* benefit any future JIT.
- **[I] Leaner call frames in the interpreter.** The `OP_call`/frame
  setup-teardown inside the 54.84% bucket was never separated from dispatch and
  arithmetic. If arg marshalling / frame init for the ~38.6 M bytecode-function
  calls is a meaningful slice, a specialized fast call path (fixed argc 0/1, no
  closure vars, plain function) could shave it with zero code-gen.

**[I] Honest ceiling.** The property route is effectively tapped: the JIT's own IC
mechanism, when it worked, delivered only ~+0.7% geomean end-to-end, because the
interpreter's inline `find_own_property` on a hashed shape is *already* cheap — an
IC mostly saves the hash computation, a small slice of 24.55%. The **only**
unexamined levers with both plausible size and a "removes work" (not "relocates
work") mechanism are RC-traffic and call-frame overhead, and their elidable
fractions are **unmeasured** because the 54.84% `interpreter-call-core` bucket was
never decomposed in `Q0`. I therefore cannot assert a new architecture *will*
reach +2%. I can assert there is exactly one lever left worth a bounded probe, and
it is not another JIT.

**[I] Note on the decisive profile's frame.** The 499 Hz architecture profile that
justified "no non-object family ≥5%" was recorded in the **H0 JIT execution
model**. That correctly closes the *JIT* search. It says nothing about the
interpreter's own time distribution, where call-frame and RC costs live folded
inside `JS_CallInternal`. Searching for interpreter-level headroom in a JIT
profile is looking under the wrong lamppost — which is why Q4 is a *Q0* profile.

---

## Q4 — One bounded, falsifiable experiment

**Experiment: a symbolized decomposition of `Q0`'s `interpreter-call-core` bucket
on Web-Tooling workloads, to decide whether reference-count/value-move elision (or
a lean call path) has ≥2% Amdahl headroom — before writing any optimization.**

This is a *discovery* experiment in the project's own R1 style. It is neither a
property/length/entry patch nor a rerun to flip a noisy result. It touches the
interpreter measurement only; it writes no optimization until the gate passes.

**Mechanism prediction.** Instrument a profiling-only `Q0` build so that samples
inside `JS_CallInternal` are attributed to disjoint sub-buckets:
(a) opcode dispatch; (b) inline reference counting — `js_dup` inc, `JS_FreeValue`
dec/branch, `set_value`; (c) call-frame setup/teardown / arg marshalling;
(d) arithmetic/comparison; (e) operand-stack moves. In parallel, a static analysis
counts, per executed opcode, how many `js_dup`/`JS_FreeValue` operations are
*provably cancelling* on the operand stack within a basic block (dup immediately
consumed; dead retain before a consuming op). Prediction: RC (b) + a
statically-elidable fraction is the largest addressable slice; dispatch (a) is
small (consistent with the JIT only recovering 7.3 s of 28.8 s).

**Amdahl ceiling.** Using `S = 1 / ((1−p) + p·r + h)` with `p` = the measured
sub-bucket share, `r` = residual after elision (from the provably-cancelling
fraction), `h` = 0 (no new runtime state; a static pre-pass). Report the ceiling
at both `h=0` and a conservative `h`.

**Direct-Q0 gate (pre-registered, frozen before measuring).** Authorize an
interpreter-level implementation **only if** some single sub-bucket has
conservative sampled-time share **≥5%** *and* the provably-elidable fraction of it
yields a conservative Amdahl ceiling **≥2%** on Babylon *and* holds direction on a
second Web-Tooling workload (Bublé or Terser). The eventual implementation is then
gated exactly as the JIT was: same-binary `Q0` vs `Q0+elision`, 16 pairs, median
and geomean ≥ +1.25%, AB/BA same sign, 95% CI lower bound > 0, and byte-identical
output under the existing TOS0/1/2 × generic exact-differential and fast-test262
harness (this is where the retained research base earns its keep — RC elision is a
use-after-free/leak risk and needs precisely that net).

**Explicit stop condition.** If no sub-bucket clears **both** ≥5% share and ≥2%
conservative ceiling, **ABANDON the performance goal**: keep the JIT as a
default-off correctness base and do not open a third front. A high-share bucket
whose elidable fraction is tiny counts as a fail, not an invitation to try anyway.

**Why this and not a DFG.** It probes the one resource the project never
measured, at the lowest possible architectural risk, and it is symmetric: a pass
means a real, low-risk interpreter win *and* a foundation a future JIT could
reuse; a fail definitively closes performance work with a number, not a hunch.

---

## Q5 — Correctness / integration risks missing from the existing tests

The differential, sanitizer, and test262 coverage is strong for *steady-state
value correctness*. The gaps are in lifecycle, concurrency-of-effect, and
embedding edges. **[I]** for each (these are risks I did not find covered in the
packaged `capsid/tests/` matrix or the gate lists in
`O15_DEEP_RETROSPECTIVE…` §9):

1. **Stale IC index across property delete/reshape.** `capsid_ic_candidate`
   caches `prop_index = property − p->prop` (`quickjs.c:19410`) and guards on
   hashed-shape pointer identity (`:19397`). Deleting a property (or any reshape
   that compacts/reallocates `p->prop` while the shape pointer is reused or the
   COW path is hit) can leave a cached index pointing at the wrong slot. Add a
   test that installs a monomorphic site, then `delete`s an earlier property on
   the same shape and re-reads, comparing against the interpreter.

2. **GC/rooting of live values held only in generated-code registers across a
   side exit.** The snapshot recipe is explicitly *not* fully implemented
   (`O15…` §6, R2.3 lists it as future). A GC triggered during side-exit
   materialization could collect a value referenced only from a machine register
   or a not-yet-owned "borrowed" slot. Add a fault-injection test that forces GC
   at each guard/side-exit point and diffs against the interpreter — the very
   harness `O15…` §9 plans but the current base predates.

3. **Same-address code rewrite / ABA under real load.** The 499 Hz profile notes
   "current workload did not trigger same-address rewrite" (`H0_PRODUCT_REVIEW…`
   §6.2) — so the tier-replacement W^X path where a side exit targets
   just-reclaimed-and-rewritten code is **exercised by no benchmark**. ABA on code
   addresses is an acknowledged unsolved risk (P2a rejected partly for it,
   `CURRENT_HANDOFF…` §6). Add a stress test that forces repeated recompile /
   reclaim / re-publish at one site while a side exit is in flight.

4. **Embedding control-flow through JITted frames.** No test in the package
   covers `JS_SetInterruptHandler` latency, `JS_ExecutePendingJob`,
   stack-overflow detection (`js_check_stack_overflow`), or `Error.stack` fidelity
   *from inside generated code*. A JITted hot loop must still deliver interrupts
   promptly and must synthesize correct `JSStackFrame` metadata so a thrown error
   produces the same stack trace as the interpreter (`O15…` R3 flags synthetic
   frame metadata as needed but it is not in the current test matrix). For an
   untrusted-code sandbox this is a denial-of-service and observability surface,
   not a nicety.

5. **SysV ABI / unwind through generated frames.** x86-64 generated code has no
   `.eh_frame` unwind info. A signal taken in JITted code, or a C++/`longjmp`
   unwind passing through a JITted frame, will not unwind correctly; callee-saved
   register and red-zone discipline at every helper call boundary is likewise
   untested against a differential oracle. Add: (a) a signal-in-generated-code
   backtrace test; (b) a register-clobber differential that verifies callee-saved
   preservation across each helper call kind. (`emit_x64.c`, `platform_linux.c`.)

**[I]** These do not change the performance verdict, but they are the risks that
would bite first if the base were ever re-enabled, and they belong in the gate set
*before* any future opt-in — precisely because the base is otherwise good enough to
be tempting to turn on.

---

## Verdict

**KEEP AS RESEARCH BASE.**

Justification: the STOP decision is correct and the measurement discipline behind
it is exemplary (same-binary control, pre-registered gates, kept negative
samples, no cross-stage percentage arithmetic). The baseline JIT is a working,
verified, default-off execution/correctness substrate and a reusable profiling
methodology — worth keeping, not abandoning. But the profiles show the incremental
JIT route is genuinely exhausted: it duplicates the expensive work (property, RC)
while removing only the cheap work (dispatch), and its one remaining ≥5% family
(`capsid_object_op`) was mined to instability. The only lever with plausible size
and a fundamentally different mechanism — reference-count/value-move elision, at
the interpreter level — is unmeasured, so a pivot is not yet earned. Run the one
Q0-decomposition experiment in Q4. If it clears ≥5% share and a ≥2% conservative
ceiling, pivot to an interpreter-level change and reuse this base's correctness
harness as the safety net. If it does not, ABANDON the performance goal and leave
the base frozen and default-off.

*Reviewer's note on the request's framing:* the distinctions it asked me to keep
intact (P0a1's +2.063% is vs old Bcore not Q0; the authoritative result is the
same-binary Q0→H0 −2.493%; the selector failed its throughput gate; percentages
across stages/presets/binaries are not additive; the backend is Linux x86-64 SysV
non-NaN-boxing only) are all honored above and are, in my reading, stated
correctly in the package.
