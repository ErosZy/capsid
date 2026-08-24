// Exact-PC monomorphic field-IC gate. SHADOW provides collision-free
// eligibility evidence; ADAPTIVE quickens a hot five-byte get_field to the
// runtime-only opcode 253 after two same-shape own-data observations. The
// stable serving hits are read-only, snapshots are canonicalized, and eight
// consecutive misses park the runtime opcode in an observation-free generic
// state until mode change or teardown restores get_field. The bounded sidecar
// retains the 16-byte entry layout and this state machine:
//
//   COLD -> TRAINING -> MONO -> MEGAMORPHIC / DISABLED
//
// The MVP is deliberately monomorphic: polymorphic training never quickens;
// eight misses after MONO dequicken. Compiled against CONFIG_SHAPE_GUARD_IC (which requires
// CONFIG_SHAPE_GUARD_ID32, forwarded from CAPSID_ENABLE_SHAPE_GUARD_IC
// + CAPSID_ENABLE_SHAPE_GUARD_ID32).
//
// Locality microcases cover mono, distinct exact PCs, polymorphic denial,
// dequickening, serialization, cold/OFF controls, budget, and ID32 wrap.
//
// `--bench` additionally prints the hit-rate/memory adjudication
// measurements as `bench <name> <value>` lines for the §14 record.
#include "tjs.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

bool g_fail = false;

void check_row(const char* name, bool ok) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok)
        g_fail = true;
}

#if defined(__x86_64__)
__attribute__((always_inline)) inline uint64_t rdtsc() {
    unsigned int lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}
#endif

// --- harness ---------------------------------------------------------------

struct Ctx {
    JSRuntime* rt;
    JSContext* ctx;
};

Ctx make_ctx(JSICMode mode = JS_IC_MODE_SHADOW) {
    Ctx c;
    c.rt = JS_NewRuntime();  // default malloc functions (JS_NewRuntime2
                             // requires a non-null JSMallocFunctions)
    if (!c.rt)
        std::abort();
    JS_ICSetMode(c.rt, mode);
    c.ctx = JS_NewContext(c.rt);
    if (!c.ctx)
        std::abort();
    return c;
}

void free_ctx(Ctx c) {
    JS_FreeContext(c.ctx);
    JS_FreeRuntime(c.rt);
}

// Run `src` as a global script; return its completion value (caller
// frees), or JS_EXCEPTION on failure with *err filled.
JSValue run(Ctx c, const char* src, std::string* err) {
    JSValue v = JS_Eval(c.ctx, src, std::strlen(src), "ic.js",
                        JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(v)) {
        JSValue ex = JS_GetException(c.ctx);
        const char* s = JS_ToCString(c.ctx, ex);
        *err = s ? s : "<no message>";
        JS_FreeCString(c.ctx, s);
        JS_FreeValue(c.ctx, ex);
    }
    return v;
}

// The integer value of the program's completion value, as a string.
std::string run_int(Ctx c, const char* src, bool* ok) {
    std::string err;
    JSValue v = run(c, src, &err);
    if (JS_IsException(v)) {
        std::fprintf(stderr, "FAIL: source failed: %s\n  %s\n", err.c_str(),
                     src);
        *ok = false;
        return "";
    }
    int32_t i;
    if (JS_ToInt32(c.ctx, &i, v) != 0) {
        std::fprintf(stderr, "FAIL: completion value not an int\n");
        JS_FreeValue(c.ctx, v);
        *ok = false;
        return "";
    }
    JS_FreeValue(c.ctx, v);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%d", i);
    return buf;
}

void dump_report(const JSICShadowReport& rep) {
    std::fprintf(stderr,
                 "  report: functions=%u bytes=%llu obs=%llu hits=%llu "
                 "misses=%llu trains=%llu transitions=%llu mega=%llu "
                 "disabled=%llu quickened=%llu dequickened=%llu "
                 "budget_fail=%llu\n",
                 rep.functions, (unsigned long long)rep.bytes,
                 (unsigned long long)rep.observations,
                 (unsigned long long)rep.hits,
                 (unsigned long long)rep.misses,
                 (unsigned long long)rep.trains,
                 (unsigned long long)rep.transitions,
                 (unsigned long long)rep.megamorphic,
                 (unsigned long long)rep.disabled,
                 (unsigned long long)rep.quickened,
                 (unsigned long long)rep.dequickened,
                 (unsigned long long)rep.budget_failures);
    for (int i = 0; i < 8; i++) {
        if (rep.sites[i].hits || rep.sites[i].misses ||
            rep.sites[i].trains)
            std::fprintf(stderr,
                         "  site[%d]: state=%u hits=%llu misses=%llu "
                         "trains=%u\n",
                         i, rep.sites[i].state,
                         (unsigned long long)rep.sites[i].hits,
                         (unsigned long long)rep.sites[i].misses,
                         rep.sites[i].trains);
    }
}

// --- microcases ------------------------------------------------------------

// 1. Layout gate (§5.2.1): the entry sizes are contract, surfaced to
// the test through the report.
void test_layout_gate() {
    Ctx c = make_ctx(JS_IC_MODE_SHADOW);
    JSICShadowReport rep;
    JS_ICGetShadowReport(c.rt, &rep);
    check_row("layout_entry_size_16", rep.entry_size == 16);
    check_row("layout_sites_per_function_8", rep.sites_per_function == 8);
    check_row("mode_get", JS_ICGetMode(c.rt) == JS_IC_MODE_SHADOW);
    free_ctx(c);
}

// 2. One monomorphic site in a tight loop: everything trains to MONO,
// all but the two training observations are would-hits, exactly one
// transition, one function state.
void test_mono_tight_loop() {
    Ctx c = make_ctx(JS_IC_MODE_SHADOW);
    bool ok = true;
    std::string r =
        run_int(c,
                "function hot() { const o = {a: 5}; let s = 0; "
                "for (let i = 0; i < 100000; i++) s += o.a; return s; } "
                "globalThis.__r = hot();",
                &ok);
    check_row("mono_result", ok && r == "500000");
    JSICShadowReport rep;
    JS_ICGetShadowReport(c.rt, &rep);
    check_row("mono_functions", rep.functions == 1);
    check_row("mono_hits", rep.hits == 99998);
    check_row("mono_training_misses", rep.misses == 2);
    check_row("mono_trains", rep.trains == 1);
    check_row("mono_transitions", rep.transitions == 1);
    check_row("mono_no_mega", rep.megamorphic == 0);
    check_row("mono_bytes", rep.bytes > 0 && rep.bytes <= 64 * 1024);
    bool seen_mono = false;
    for (int i = 0; i < 8; i++) {
        if (rep.sites[i].trains == 1) {
            seen_mono = rep.sites[i].state == 2 &&  // MONO
                        rep.sites[i].hits == 99998 &&
                        rep.sites[i].misses == 2;
        }
    }
    check_row("mono_site_state", seen_mono);
    if (!seen_mono)
        dump_report(rep);
    free_ctx(c);
}

// 3. Two unrelated PCs using the same atom with different shapes: no
// pollution. Exact bytecode PCs must not collide — the source
// layout is irrelevant: exact PC mapping cannot collide.
void test_two_pcs_same_atom_no_pollution() {
    Ctx c = make_ctx(JS_IC_MODE_SHADOW);
    bool ok = true;
    std::string r = run_int(
        c,
        "const a = {x: 1, y: 2}; const b = {x: 3, z: 4}; "
        "function f(a, b) { "
        "let s = 0; "
        "for (let i = 0; i < 20000; i++) s += a.x; "
        "let t = 0; for (let j = 0; j < 100; j++) "
        "t = t * 2 + j - (j >> 2) * 3 + (j % 7); "
        "s += t; "
        "for (let i = 0; i < 20000; i++) s += b.x; "
        "return s; "
        "} globalThis.__r = f(a, b);",
        &ok);
    check_row("two_pc_result", ok && r != "");
    JSICShadowReport rep;
    JS_ICGetShadowReport(c.rt, &rep);
    int trained = 0;
    bool each_mono = true;
    for (int i = 0; i < 8; i++) {
        if (rep.sites[i].trains == 1) {
            trained++;
            if (rep.sites[i].state != 2 || rep.sites[i].hits != 19998 ||
                rep.sites[i].misses != 2)
                each_mono = false;
        }
        if (rep.sites[i].trains > 1)
            each_mono = false;  // collision: one site trained twice
    }
    check_row("two_pc_distinct_sites", trained == 2);
    check_row("two_pc_no_pollution", each_mono);
    if (!(trained == 2 && each_mono))
        dump_report(rep);
    free_ctx(c);
}

// 4. Stable alternating shapes never satisfy the two-consecutive proof.
void test_alternating_shapes() {
    Ctx c = make_ctx(JS_IC_MODE_SHADOW);
    bool ok = true;
    std::string r = run_int(
        c,
        "function hot() { const a = {v: 1}; const b = {v: 2, w: 3}; "
        "let s = 0; for (let i = 0; i < 100000; i++) "
        "s += (i % 2 ? a : b).v; return s; } "
        "globalThis.__r = hot();",
        &ok);
    check_row("alternating_result", ok && r == "150000");
    JSICShadowReport rep;
    JS_ICGetShadowReport(c.rt, &rep);
    check_row("alternating_hits", rep.hits == 0);
    check_row("alternating_misses", rep.misses == 100000);
    check_row("alternating_transitions", rep.transitions == 0);
    check_row("alternating_no_mega", rep.megamorphic == 0);
    bool seen_training = false;
    for (int i = 0; i < 8; i++) {
        if (rep.sites[i].trains == 100000)
            seen_training = rep.sites[i].state == 1;  // TRAINING
    }
    check_row("alternating_site_state", seen_training);
    if (!seen_training)
        dump_report(rep);
    free_ctx(c);
}

// 5. A three-shape rotation also stays in TRAINING; it never becomes an IC.
void test_three_shape_megamorphic() {
    Ctx c = make_ctx(JS_IC_MODE_SHADOW);
    bool ok = true;
    std::string r = run_int(
        c,
        "function hot() { const a = {v: 1}; const b = {v: 2, w: 3}; "
        "const c = {v: 3, z: 4, q: 5}; let s = 0; "
        "for (let i = 0; i < 100000; i++) s += [a, b, c][i % 3].v; "
        "return s; } globalThis.__r = hot();",
        &ok);
    // 33333 full cycles of (1+2+3) + one extra a.v (i=99999 -> 0):
    // 33333*6 + 1 = 199999.
    check_row("mega_result", ok && r == "199999");
    JSICShadowReport rep;
    JS_ICGetShadowReport(c.rt, &rep);
    check_row("mega_count", rep.megamorphic == 0);
    bool seen_training = false;
    for (int i = 0; i < 8; i++) {
        if (rep.sites[i].state == 1 && rep.sites[i].trains == 100000)
            seen_training = true;
    }
    check_row("mega_site_denied", seen_training);
    if (!seen_training)
        dump_report(rep);
    free_ctx(c);
}

// 6. Eight consecutive misses become megamorphic even with only two
// shapes: train on a, then hit 8 in a row on b.
void test_eight_consecutive_misses() {
    Ctx c = make_ctx(JS_IC_MODE_SHADOW);
    bool ok = true;
    std::string r = run_int(
        c,
        "function g(o) { let s = 0; for (let i = 0; i < 8; i++) s += o.v; "
        "return s; } "
        "const a = {v: 1}; const acc = { get v() { return 2; } }; "
        "globalThis.__r = g(a) + g(acc);",
        &ok);
    check_row("streak_result", ok && r == "24");
    JSICShadowReport rep;
    JS_ICGetShadowReport(c.rt, &rep);
    check_row("streak_hits", rep.hits == 6);
    check_row("streak_misses", rep.misses == 10);
    check_row("streak_mega", rep.megamorphic == 1);
    if (!(rep.hits == 6 && rep.misses == 10 && rep.megamorphic == 1))
        dump_report(rep);
    free_ctx(c);
}

// 7. Always-miss control: an accessor-only site is never cacheable,
// never trains and stays cold; the miss streak applies only after MONO.
void test_always_miss_accessor() {
    Ctx c = make_ctx(JS_IC_MODE_SHADOW);
    bool ok = true;
    std::string r = run_int(
        c,
        "function hot() { const o = { get v() { return 1; } }; "
        "let s = 0; for (let i = 0; i < 1000; i++) s += o.v; "
        "return s; } globalThis.__r = hot();",
        &ok);
    check_row("accessor_result", ok && r == "1000");
    JSICShadowReport rep;
    JS_ICGetShadowReport(c.rt, &rep);
    check_row("accessor_no_train", rep.trains == 0 && rep.hits == 0);
    check_row("accessor_all_miss", rep.misses == 1000);
    check_row("accessor_no_mega", rep.megamorphic == 0);
    if (!(rep.trains == 0 && rep.hits == 0 && rep.misses == 1000))
        dump_report(rep);
    free_ctx(c);
}

// 8. Emitted-cold control: a compiled function whose site never runs
// allocates no state; a program with no property access allocates
// nothing at all.
void test_cold_no_alloc() {
    Ctx c = make_ctx(JS_IC_MODE_SHADOW);
    bool ok = true;
    std::string r = run_int(
        c,
        "function cold(o) { return o.v; } "
        "globalThis.__r = 1 + 2;",
        &ok);
    check_row("cold_result", ok && r == "3");
    JSICShadowReport rep;
    JS_ICGetShadowReport(c.rt, &rep);
    check_row("cold_no_state", rep.functions == 0 && rep.bytes == 0 &&
                                   rep.observations == 0);
    if (rep.functions != 0)
        dump_report(rep);
    free_ctx(c);
}

// 9. OFF control: code compiled, no site trains or allocates.
void test_off_no_state() {
    Ctx c = make_ctx(JS_IC_MODE_OFF);
    bool ok = true;
    std::string r = run_int(
        c,
        "const o = {a: 5}; let s = 0; "
        "for (let i = 0; i < 1000; i++) s += o.a; "
        "globalThis.__r = s;",
        &ok);
    check_row("off_result", ok && r == "5000");
    JSICShadowReport rep;
    JS_ICGetShadowReport(c.rt, &rep);
    check_row("off_no_state", rep.functions == 0 && rep.bytes == 0 &&
                                  rep.observations == 0);
    free_ctx(c);
}

// 10. SHADOW-only control: the generic result is always used, so OFF
// and SHADOW produce identical results on a mixed workload (own-data,
// proto-inherited, accessor, megamorphic).
void test_shadow_generic_authoritative() {
    const char* src =
        "function hot() { "
        "const a = {x: 1}; const p = {v: 9}; Object.setPrototypeOf(a, p); "
        "const b = {v: 2, w: 3}; const c = {v: 3, z: 4, q: 5}; "
        "const g = { get v() { return 7; } }; "
        "let s = 0; "
        "for (let i = 0; i < 1000; i++) { "
        "s += (i % 3 == 0) ? a.v : (i % 3 == 1) ? g.v : (i % 4 == 2) ? b.v "
        ": c.v; "
        "} s += g.v + a.v; return s; } globalThis.__r = hot();";
    Ctx off = make_ctx(JS_IC_MODE_OFF);
    bool ok_off = true, ok_shadow = true;
    std::string r_off = run_int(off, src, &ok_off);
    JSICShadowReport rep_off;
    JS_ICGetShadowReport(off.rt, &rep_off);
    free_ctx(off);
    Ctx shadow = make_ctx(JS_IC_MODE_SHADOW);
    std::string r_shadow = run_int(shadow, src, &ok_shadow);
    JSICShadowReport rep_shadow;
    JS_ICGetShadowReport(shadow.rt, &rep_shadow);
    check_row("authoritative_off_ok", ok_off);
    check_row("authoritative_shadow_ok", ok_shadow);
    check_row("authoritative_identical", ok_off && ok_shadow &&
                                             r_off == r_shadow && r_off != "");
    check_row("authoritative_off_no_state",
              rep_off.functions == 0 && rep_off.observations == 0);
    check_row("authoritative_shadow_observed",
              rep_shadow.functions >= 1 && rep_shadow.observations >= 1000);
    // the mixed workload is genuinely mixed: own-data hits exist and
    // non-cacheable misses exist
    check_row("authoritative_mixed",
              rep_shadow.hits >= 1 && rep_shadow.misses >= 1);
    if (rep_shadow.hits < 1 || rep_shadow.misses < 1)
        dump_report(rep_shadow);
    free_ctx(shadow);
}

// 11. Budget cap: 300 hot functions exceed the 64 KiB runtime budget;
// later functions are denied, earlier ones keep their state.
void test_budget_cap() {
    Ctx c = make_ctx(JS_IC_MODE_SHADOW);
    bool ok = true;
    char src[256];
    for (int k = 0; k < 300; k++) {
        std::snprintf(src, sizeof(src),
                      "var o%d = {v: %d}; function f%d() { let s = 0; "
                      "for (let i = 0; i < 20; i++) s += o%d.v; "
                      "globalThis.__r = s; } f%d();",
                      k, k, k, k, k);
        std::string err;
        JSValue v = run(c, src, &err);
        if (JS_IsException(v)) {
            std::fprintf(stderr, "FAIL: budget source %d failed: %s\n", k,
                         err.c_str());
            ok = false;
        } else {
            JS_FreeValue(c.ctx, v);
        }
        if (k > 0 && k % 100 == 0) {
            // the global f0.. f%d function objects stay alive and hold
            // their bytecode + sidecar; the budget is consumed for good
        }
    }
    JSICShadowReport rep;
    JS_ICGetShadowReport(c.rt, &rep);
    check_row("budget_ok", ok);
    check_row("budget_allocated", rep.functions > 0);
    check_row("budget_denied", rep.budget_failures >= 100);
    check_row("budget_bytes_capped",
              rep.bytes <= 64 * 1024 && rep.functions < 300);
    if (!(rep.budget_failures >= 100 && rep.bytes <= 64 * 1024))
        dump_report(rep);
    free_ctx(c);
}

// 12. ID32 wrap disables identity checking: a wrapped runtime cannot
// train — sites go DISABLED and stay there (semantics preserved).
void test_id32_wrap_disables() {
    Ctx c = make_ctx(JS_IC_MODE_SHADOW);
    JS_ICShapeGuardSetCounterForTest(c.rt, UINT32_MAX - 1);
    bool ok = true;
    std::string r = run_int(
        c,
        "function hot() { const p = {a: 1}; const q = {b: 2}; "
        "const o = {v: 1}; let s = 0; "
        "for (let i = 0; i < 100; i++) s += o.v; return s; } "
        "globalThis.__r = hot();",
        &ok);
    check_row("wrap_result", ok && r == "100");
    JSICShadowReport rep;
    JS_ICGetShadowReport(c.rt, &rep);
    check_row("wrap_disabled", rep.disabled >= 1);
    check_row("wrap_no_train", rep.trains == 0 && rep.hits == 0);
    if (!(rep.disabled >= 1 && rep.trains == 0 && rep.hits == 0))
        dump_report(rep);
    free_ctx(c);
}

// 13. ADAPTIVE: 128 cold observations are allocation-free, the next two
// train, and all remaining accesses run through opcode 253 without hit-counter
// writes. Switching OFF restores canonical get_field in place.
void test_adaptive_mono_quickens() {
    Ctx c = make_ctx(JS_IC_MODE_ADAPTIVE);
    bool ok = true;
    std::string r = run_int(
        c,
        "function hot() { const o = {a: 5}; let s = 0; "
        "for (let i = 0; i < 100000; i++) s += o.a; return s; } "
        "globalThis.__r = hot();",
        &ok);
    check_row("adaptive_result", ok && r == "500000");
    JSICShadowReport rep;
    JS_ICGetShadowReport(c.rt, &rep);
    check_row("adaptive_quickened", rep.quickened == 1);
    check_row("adaptive_not_dequickened", rep.dequickened == 0);
    check_row("adaptive_two_training_observations",
              rep.observations == 2 && rep.misses == 2 && rep.hits == 0);
    JS_ICSetMode(c.rt, JS_IC_MODE_OFF);
    JS_ICGetShadowReport(c.rt, &rep);
    check_row("adaptive_mode_restore", rep.dequickened == 1 &&
                                           JS_ICGetMode(c.rt) == JS_IC_MODE_OFF);
    r = run_int(c, "globalThis.__r = hot();", &ok);
    check_row("adaptive_restored_result", ok && r == "500000");
    free_ctx(c);
}

// 14. Eight shape misses in the direct handler permanently deny that site.
// The runtime opcode remains parked so later accesses reuse the full generic
// get_field handler without re-entering the observer or writing counters.
void test_adaptive_dequickens_on_miss_streak() {
    Ctx c = make_ctx(JS_IC_MODE_ADAPTIVE);
    bool ok = true;
    std::string r = run_int(
        c,
        "function g(o) { return o.v; } const a = {v: 1}; "
        "const b = {v: 2, w: 0}; let s = 0; "
        "for (let i = 0; i < 140; i++) s += g(a); "
        "for (let i = 0; i < 8; i++) s += g(b); "
        "for (let i = 0; i < 10; i++) s += g(a); "
        "globalThis.__r = s;",
        &ok);
    check_row("dequicken_result", ok && r == "166");
    JSICShadowReport rep;
    JS_ICGetShadowReport(c.rt, &rep);
    check_row("dequicken_counts", rep.quickened == 1 &&
                                      rep.dequickened == 1 &&
                                      rep.megamorphic == 1);
    const uint64_t observations_after_park = rep.observations;
    const uint64_t misses_after_park = rep.misses;
    r = run_int(
        c,
        "let tail = 0; for (let i = 0; i < 100000; i++) tail += g(a); "
        "globalThis.__r = tail;",
        &ok);
    check_row("dequicken_terminal_result", ok && r == "100000");
    JS_ICGetShadowReport(c.rt, &rep);
    check_row("dequicken_terminal_observation_free",
              rep.observations == observations_after_park &&
                  rep.misses == misses_after_park &&
                  rep.dequickened == 1 && rep.megamorphic == 1);
    if (!(rep.observations == observations_after_park &&
          rep.misses == misses_after_park && rep.dequickened == 1 &&
          rep.megamorphic == 1))
        dump_report(rep);
    free_ctx(c);
}

// 15. Serializing a live quickened function rewrites only the writer's copy
// to canonical get_field + atom. A fresh OFF runtime can load and execute it.
void test_adaptive_snapshot_is_canonical() {
    Ctx c = make_ctx(JS_IC_MODE_ADAPTIVE);
    std::string err;
    const char* src =
        "function g(o) { return o.v; } const o = {v: 7}; let s = 0; "
        "for (let i = 0; i < 140; i++) s += g(o); globalThis.__r = s;";
    JSValue compiled = JS_Eval(c.ctx, src, std::strlen(src), "snapshot.js",
                               JS_EVAL_TYPE_GLOBAL |
                                   JS_EVAL_FLAG_COMPILE_ONLY);
    bool ok = !JS_IsException(compiled);
    JSValue eval_result = ok ? JS_EvalFunction(c.ctx,
                                                JS_DupValue(c.ctx, compiled))
                             : JS_EXCEPTION;
    ok = ok && !JS_IsException(eval_result);
    JS_FreeValue(c.ctx, eval_result);
    JSICShadowReport rep;
    JS_ICGetShadowReport(c.rt, &rep);
    check_row("snapshot_source_quickened", ok && rep.quickened == 1);
    std::size_t size = 0;
    std::uint8_t* data = ok ? JS_WriteObject(
                                  c.ctx, &size, compiled, JS_WRITE_OBJ_BYTECODE)
                            : nullptr;
    check_row("snapshot_write", data != nullptr && size > 0);

    Ctx fresh = make_ctx(JS_IC_MODE_OFF);
    JSValue loaded = data ? JS_ReadObject(fresh.ctx, data, size,
                                          JS_READ_OBJ_BYTECODE)
                          : JS_EXCEPTION;
    JSValue ret = !JS_IsException(loaded)
                      ? JS_EvalFunction(fresh.ctx, loaded)
                      : JS_EXCEPTION;
    loaded = JS_UNDEFINED;  // JS_EvalFunction consumed the bytecode value.
    int32_t result = 0;
    bool loaded_ok = !JS_IsException(ret) &&
                     JS_ToInt32(fresh.ctx, &result, ret) == 0 && result == 980;
    check_row("snapshot_fresh_runtime_exec", loaded_ok);

    JS_FreeValue(fresh.ctx, ret);
    JS_FreeValue(fresh.ctx, loaded);
    free_ctx(fresh);
    if (data)
        js_free(c.ctx, data);
    JS_FreeValue(c.ctx, compiled);
    free_ctx(c);
}

// 16. Adjudication measurements (--bench): cycles per observed access
// on the mono tight loop, bytes per function state, observations.
void bench_mono() {
#if defined(__x86_64__)
    Ctx c = make_ctx(JS_IC_MODE_SHADOW);
    bool ok = true;
    const char* src =
        "function hot() { const o = {a: 5}; let s = 0; "
        "for (let i = 0; i < 2000000; i++) s += o.a; return s; } "
        "globalThis.__r = hot();";
    // cold start: the observation path includes the lazy allocation
    uint64_t t0 = rdtsc();
    std::string err;
    JSValue v = run(c, src, &err);
    uint64_t dt = rdtsc() - t0;
    if (JS_IsException(v)) {
        std::fprintf(stderr, "FAIL: bench source failed: %s\n", err.c_str());
        JS_FreeValue(c.ctx, v);
        ok = false;
    } else {
        JS_FreeValue(c.ctx, v);
    }
    JSICShadowReport rep;
    JS_ICGetShadowReport(c.rt, &rep);
    if (ok && rep.observations == 2000000)
        std::printf("bench shadow_ic_mono_cycles_per_obs %llu\n",
                    (unsigned long long)(dt / rep.observations));
    std::printf("bench shadow_ic_mono_observations %llu\n",
                (unsigned long long)rep.observations);
    if (rep.functions > 0)
        std::printf("bench shadow_ic_bytes_per_function %llu\n",
                    (unsigned long long)(rep.bytes / rep.functions));
    // In SHADOW every observation writes the per-site counters — the
    // cost of measuring. The serving modes keep the hit path read-only.
    if (rep.hits > 0)
        std::printf("bench shadow_ic_counter_writes_per_hit 1\n");
    free_ctx(c);
#else
    std::printf("bench shadow_ic_mono_cycles_per_obs 0\n");
#endif
}

}  // namespace

int main(int argc, char** argv) {
    test_layout_gate();
    test_mono_tight_loop();
    test_two_pcs_same_atom_no_pollution();
    test_alternating_shapes();
    test_three_shape_megamorphic();
    test_eight_consecutive_misses();
    test_always_miss_accessor();
    test_cold_no_alloc();
    test_off_no_state();
    test_shadow_generic_authoritative();
    test_budget_cap();
    test_id32_wrap_disables();
    test_adaptive_mono_quickens();
    test_adaptive_dequickens_on_miss_streak();
    test_adaptive_snapshot_is_canonical();
    if (argc > 1 && std::strcmp(argv[1], "--bench") == 0)
        bench_mono();
    if (g_fail) {
        std::fprintf(stderr, "test_shadow_ic: %d failure(s)\n", g_fail ? 1 : 0);
        return 1;
    }
    std::fprintf(stderr, "test_shadow_ic: all green\n");
    return 0;
}
