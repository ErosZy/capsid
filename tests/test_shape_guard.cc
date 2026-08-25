// S0 measurement gate (docs/quickjs-optimization.md §3/§6): drives the
// compile-gated shape-guard A/B backends
// through the invalidation matrix and micro-benchmarks. One binary,
// compiled against whichever backend the build enabled (quickjs
// CONFIG_SHAPE_GUARD_ID32 or CONFIG_SHAPE_GUARD_STRONG_REF, forwarded
// from CAPSID_ENABLE_SHAPE_GUARD_ID32/STRONG_REF).
//
// Matrix contract (§5.1.1): every mutation row must MISS — a guard must
// never hand out a stale identity. Rows that touch object identity:
//
//   warm -> mutate (add prop)          -> miss
//   warm -> delete -> re-add same atom -> miss
//   warm -> data -> accessor           -> miss
//   warm -> freeze -> write            -> miss
//   warm -> prototype replacement      -> miss
//   warm -> array fast->slow transition -> miss
//   warm -> slow-array length change   -> miss
//   warm -> sibling object mutates     -> hit (the guarded object's
//                                         shape is genuinely unchanged)
//   warm -> GC (no mutation)           -> hit (guard survives GC)
//   warm -> free owner -> GC -> re-create same props
//         ID32:      miss — fresh shape id, monotonic ids are never
//                    recycled, so an address reuse cannot stale-hit.
//         STRONG_REF: hit — the site dup keeps the guarded shape alive;
//                    the re-created object shares it; the address was
//                    never freed, so it was never reusable.
//
// ID32-only directed wrap test: the counter set one below UINT32_MAX
// wraps on the next assignment and disables identity checking for the
// runtime (JS_ICShapeGuardEnabled() == 0, new sites cannot train).
//
// `--bench` additionally runs the S0 selection measurements (cycles per
// check/update/mutation, retained bytes per live site) and prints them
// as `bench <name> <value>` lines for the across-build verdict.
#include "tjs.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

bool g_fail = false;

void check_row(const char* name, bool ok) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok)
        g_fail = true;
}

// no-op C function used as a property getter in the data->accessor row.
JSValue noop_cfunc(JSContext* ctx, JSValueConst this_val, int argc,
                   JSValueConst* argv) {
    (void)ctx;
    (void)this_val;
    (void)argc;
    (void)argv;
    return JS_UNDEFINED;
}

#if defined(__x86_64__)
__attribute__((always_inline)) inline uint64_t rdtsc() {
    unsigned int lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}
#endif

// --- matrix harness -------------------------------------------------------

struct Ctx {
    JSRuntime* rt;
    JSContext* ctx;
};

Ctx make_ctx() {
    Ctx c;
    c.rt = JS_NewRuntime();  // default malloc functions (JS_NewRuntime2
                             // requires a non-null JSMallocFunctions)
    if (!c.rt)
        std::abort();
    c.ctx = JS_NewContext(c.rt);
    if (!c.ctx)
        std::abort();
    return c;
}

void free_ctx(Ctx c) {
    JS_FreeContext(c.ctx);
    JS_FreeRuntime(c.rt);
}

JSValue mk_obj(Ctx c, const char* props[][2], int nprops) {
    JSValue o = JS_NewObject(c.ctx);
    for (int i = 0; i < nprops; i++) {
        JSAtom a = JS_NewAtom(c.ctx, props[i][0]);
        JSValue v = JS_NewInt32(c.ctx, std::strtol(props[i][1], nullptr, 10));
        JS_DefinePropertyValue(
            c.ctx, o, a, v,
            JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE | JS_PROP_ENUMERABLE);
        JS_FreeAtom(c.ctx, a);
        JS_FreeValue(c.ctx, v);
    }
    return o;
}

int run_matrix(Ctx c) {
    // 1. baseline: warm + immediate hit on the same object.
    {
        JSValue o = JS_NewObject(c.ctx);
        JSICShapeGuardSite* s = JS_ICShapeGuardNew(c.rt);
        check_row("baseline_warm", JS_ICShapeGuardUpdate(c.ctx, s, o) == 0);
        check_row("baseline_hit", JS_ICShapeGuardCheck(s, o) == 1);
        JS_ICShapeGuardRelease(c.rt, s);
        JS_FreeValue(c.ctx, o);
    }
    // 2. warm -> add prop -> miss.
    {
        JSValue o = JS_NewObject(c.ctx);
        JSICShapeGuardSite* s = JS_ICShapeGuardNew(c.rt);
        JS_ICShapeGuardUpdate(c.ctx, s, o);
        JSAtom a = JS_NewAtom(c.ctx, "x");
        JS_DefinePropertyValue(
            c.ctx, o, a, JS_NewInt32(c.ctx, 1),
            JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE | JS_PROP_ENUMERABLE);
        JS_FreeAtom(c.ctx, a);
        check_row("mutate_add_prop_miss", JS_ICShapeGuardCheck(s, o) == 0);
        // retrain and re-hit: the site is still usable.
        JS_ICShapeGuardUpdate(c.ctx, s, o);
        check_row("mutate_retrain_hit", JS_ICShapeGuardCheck(s, o) == 1);
        JS_ICShapeGuardRelease(c.rt, s);
        JS_FreeValue(c.ctx, o);
    }
    // 3. warm -> delete -> re-add the same atom -> miss both times.
    {
        const char* props[][2] = {{"x", "1"}, {"y", "2"}};
        JSValue o = mk_obj(c, props, 2);
        JSAtom ax = JS_NewAtom(c.ctx, "x");
        JSICShapeGuardSite* s = JS_ICShapeGuardNew(c.rt);
        JS_ICShapeGuardUpdate(c.ctx, s, o);
        JS_DeleteProperty(c.ctx, o, ax, 0);
        check_row("mutate_delete_miss", JS_ICShapeGuardCheck(s, o) == 0);
        JS_ICShapeGuardUpdate(c.ctx, s, o);
        JS_DefinePropertyValue(
            c.ctx, o, ax, JS_NewInt32(c.ctx, 3),
            JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE | JS_PROP_ENUMERABLE);
        check_row("mutate_readd_same_atom_miss",
                  JS_ICShapeGuardCheck(s, o) == 0);
        JS_ICShapeGuardRelease(c.rt, s);
        JS_FreeAtom(c.ctx, ax);
        JS_FreeValue(c.ctx, o);
    }
    // 4. warm -> data prop -> accessor -> miss.
    {
        const char* props[][2] = {{"x", "1"}};
        JSValue o = mk_obj(c, props, 1);
        JSAtom ax = JS_NewAtom(c.ctx, "x");
        JSICShapeGuardSite* s = JS_ICShapeGuardNew(c.rt);
        JS_ICShapeGuardUpdate(c.ctx, s, o);
        JSValue getter = JS_NewCFunction(c.ctx, noop_cfunc, "g", 0);
        JS_DefineProperty(c.ctx, o, ax, JS_UNDEFINED, getter, JS_UNDEFINED,
                          JS_PROP_HAS_GET | JS_PROP_HAS_CONFIGURABLE |
                              JS_PROP_HAS_ENUMERABLE);
        JS_FreeValue(c.ctx, getter);
        check_row("mutate_data_to_accessor_miss",
                  JS_ICShapeGuardCheck(s, o) == 0);
        JS_ICShapeGuardRelease(c.rt, s);
        JS_FreeAtom(c.ctx, ax);
        JS_FreeValue(c.ctx, o);
    }
    // 5. warm -> freeze (non-writable non-configurable) -> miss; a
    //    subsequent write attempt cannot resurrect the stale guard.
    {
        const char* props[][2] = {{"a", "1"}, {"b", "2"}};
        JSValue o = mk_obj(c, props, 2);
        JSAtom aa = JS_NewAtom(c.ctx, "a");
        JSICShapeGuardSite* s = JS_ICShapeGuardNew(c.rt);
        JS_ICShapeGuardUpdate(c.ctx, s, o);
        // quickjs define-property: absent C/W/E bits mean false
        // attributes, i.e. the property becomes frozen.
        JS_DefinePropertyValue(
            c.ctx, o, aa, JS_NewInt32(c.ctx, 1),
            JS_PROP_HAS_VALUE | JS_PROP_HAS_CONFIGURABLE |
                JS_PROP_HAS_WRITABLE | JS_PROP_HAS_ENUMERABLE);
        check_row("mutate_freeze_miss", JS_ICShapeGuardCheck(s, o) == 0);
        JS_SetProperty(c.ctx, o, aa, JS_NewInt32(c.ctx, 99));
        check_row("mutate_freeze_then_write_still_miss",
                  JS_ICShapeGuardCheck(s, o) == 0);
        JS_ICShapeGuardRelease(c.rt, s);
        JS_FreeAtom(c.ctx, aa);
        JS_FreeValue(c.ctx, o);
    }
    // 6. warm -> prototype replacement -> miss.
    {
        JSValue o = JS_NewObject(c.ctx);
        JSValue p1 = JS_NewObject(c.ctx);
        JSValue p2 = JS_NewObject(c.ctx);
        JS_SetPrototype(c.ctx, o, p1);
        JSICShapeGuardSite* s = JS_ICShapeGuardNew(c.rt);
        JS_ICShapeGuardUpdate(c.ctx, s, o);
        JS_SetPrototype(c.ctx, o, p2);
        check_row("mutate_proto_replace_miss",
                  JS_ICShapeGuardCheck(s, o) == 0);
        JS_ICShapeGuardRelease(c.rt, s);
        JS_FreeValue(c.ctx, p1);
        JS_FreeValue(c.ctx, p2);
        JS_FreeValue(c.ctx, o);
    }
    // 7. arrays: fast->slow transition and slow-array length set.
    {
        JSValue arr = JS_NewArray(c.ctx);
        JSAtom alen = JS_NewAtom(c.ctx, "length");
        JSICShapeGuardSite* s = JS_ICShapeGuardNew(c.rt);
        JS_ICShapeGuardUpdate(c.ctx, s, arr);
        // attribute-annotated element (non-writable) -> fast->slow
        // conversion.
        JSAtom a0 = JS_NewAtom(c.ctx, "0");
        JS_DefinePropertyValue(
            c.ctx, arr, a0, JS_NewInt32(c.ctx, 7),
            JS_PROP_HAS_VALUE | JS_PROP_HAS_CONFIGURABLE |
                JS_PROP_HAS_WRITABLE | JS_PROP_HAS_ENUMERABLE);
        JS_FreeAtom(c.ctx, a0);
        check_row("mutate_array_fast_to_slow_miss",
                  JS_ICShapeGuardCheck(s, arr) == 0);
        // slow array: setting length is a plain value update on the
        // existing writable "length" slot (set_array_length -> set_value
        // in quickjs): no shape change, so a structural guard still hits
        // after retraining.
        JS_ICShapeGuardUpdate(c.ctx, s, arr);
        JS_SetProperty(c.ctx, arr, alen, JS_NewInt32(c.ctx, 64));
        check_row("mutate_slow_array_length_value_hit",
                  JS_ICShapeGuardCheck(s, arr) == 1);
        // structural slow-array mutation: adding a new property changes
        // the shape -> miss.
        JSAtom az = JS_NewAtom(c.ctx, "z");
        JS_DefinePropertyValue(
            c.ctx, arr, az, JS_NewInt32(c.ctx, 9),
            JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE | JS_PROP_ENUMERABLE);
        JS_FreeAtom(c.ctx, az);
        check_row("mutate_slow_array_add_prop_miss",
                  JS_ICShapeGuardCheck(s, arr) == 0);
        JS_ICShapeGuardRelease(c.rt, s);
        JS_FreeAtom(c.ctx, alen);
        JS_FreeValue(c.ctx, arr);
    }
    // 8. sibling mutation: objA and objB share a shape; mutating objB
    //    must not invalidate the guard on the untouched objA.
    {
        const char* props[][2] = {{"x", "1"}, {"y", "2"}};
        JSValue a = mk_obj(c, props, 2);
        JSValue b = mk_obj(c, props, 2);
        JSICShapeGuardSite* s = JS_ICShapeGuardNew(c.rt);
        JS_ICShapeGuardUpdate(c.ctx, s, a);
        JSAtom az = JS_NewAtom(c.ctx, "z");
        JS_DefinePropertyValue(
            c.ctx, b, az, JS_NewInt32(c.ctx, 9),
            JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE | JS_PROP_ENUMERABLE);
        JS_FreeAtom(c.ctx, az);
        check_row("sibling_mutate_untouched_hit",
                  JS_ICShapeGuardCheck(s, a) == 1);
        check_row("sibling_mutate_mutated_miss",
                  JS_ICShapeGuardCheck(s, b) == 0);
        JS_ICShapeGuardRelease(c.rt, s);
        JS_FreeValue(c.ctx, a);
        JS_FreeValue(c.ctx, b);
    }
    // 9. GC survival: guard trained, no mutation, full GC — the guard
    //    must keep working on the still-live object.
    {
        const char* props[][2] = {{"x", "1"}, {"y", "2"}};
        JSValue o = mk_obj(c, props, 2);
        JSICShapeGuardSite* s = JS_ICShapeGuardNew(c.rt);
        JS_ICShapeGuardUpdate(c.ctx, s, o);
        JS_RunGC(c.rt);
        check_row("gc_no_mutation_hit", JS_ICShapeGuardCheck(s, o) == 1);
        JS_ICShapeGuardRelease(c.rt, s);
        JS_FreeValue(c.ctx, o);
    }
    // 10. owner freed + GC + same-props re-creation. The invariant is
    //     "no stale hit through address recycling":
    //     ID32: the old shape is collected; the re-created object gets
    //           a fresh monotonic id -> miss (even if the allocator
    //           reuses the exact same address).
    //     STRONG_REF: the site dup keeps the guarded shape (and its
    //           proto) alive, so the re-created object shares it and
    //           the hit is the same genuine shape, not a recycled one.
    {
        const char* props[][2] = {{"x", "1"}, {"y", "2"}};
        JSValue o = mk_obj(c, props, 2);
        JSICShapeGuardSite* s = JS_ICShapeGuardNew(c.rt);
        JS_ICShapeGuardUpdate(c.ctx, s, o);
        JS_FreeValue(c.ctx, o);
        JS_RunGC(c.rt);
        // churn to make the allocator reuse addresses where possible
        for (int i = 0; i < 1000; i++) {
            JSValue t = mk_obj(c, props, 2);
            JS_FreeValue(c.ctx, t);
        }
        JS_RunGC(c.rt);
        JSValue o2 = mk_obj(c, props, 2);
#if defined(CONFIG_SHAPE_GUARD_ID32)
        check_row("gc_free_owner_recreate_miss",
                  JS_ICShapeGuardCheck(s, o2) == 0);
#else
        check_row("gc_free_owner_recreate_hit",
                  JS_ICShapeGuardCheck(s, o2) == 1);
#endif
        JS_ICShapeGuardRelease(c.rt, s);
        JS_FreeValue(c.ctx, o2);
    }
    // 11. non-object values: update fails, check never hits; the site
    //     remains trainable on a real object afterwards.
    {
        JSValue o = JS_NewObject(c.ctx);
        JSValue n = JS_NewInt32(c.ctx, 7);
        JSICShapeGuardSite* s = JS_ICShapeGuardNew(c.rt);
        check_row("non_object_update_rejected",
                  JS_ICShapeGuardUpdate(c.ctx, s, n) == -1);
        check_row("non_object_check_zero", JS_ICShapeGuardCheck(s, n) == 0);
        JS_ICShapeGuardUpdate(c.ctx, s, o);
        check_row("non_object_recover_train",
                  JS_ICShapeGuardUpdate(c.ctx, s, o) == 0);
        check_row("non_object_recover_hit", JS_ICShapeGuardCheck(s, o) == 1);
        JS_ICShapeGuardRelease(c.rt, s);
        JS_FreeValue(c.ctx, n);
        JS_FreeValue(c.ctx, o);
    }
    // 12. retrain switches the guarded shape. Two plain JS_NewObject()
    //     share the cached empty-object shape, so the old/new guards
    //     must train on objects with distinct shapes.
    {
        const char* pa[][2] = {{"x", "1"}};
        const char* pb[][2] = {{"y", "2"}};
        JSValue a = mk_obj(c, pa, 1);
        JSValue b = mk_obj(c, pb, 1);
        JSICShapeGuardSite* s = JS_ICShapeGuardNew(c.rt);
        JS_ICShapeGuardUpdate(c.ctx, s, a);
        check_row("retrain_first_hit", JS_ICShapeGuardCheck(s, a) == 1);
        JS_ICShapeGuardUpdate(c.ctx, s, b);
        check_row("retrain_second_hit", JS_ICShapeGuardCheck(s, b) == 1);
        check_row("retrain_first_miss_after", JS_ICShapeGuardCheck(s, a) == 0);
        JS_ICShapeGuardRelease(c.rt, s);
        JS_FreeValue(c.ctx, a);
        JS_FreeValue(c.ctx, b);
    }
#if defined(CONFIG_SHAPE_GUARD_ID32)
    // 13. ID32 directed wrap: counter one below UINT32_MAX; the next
    //     shape assignment wraps and disables identity checking.
    {
        JS_ICShapeGuardSetCounterForTest(c.rt, UINT32_MAX);
        JSValue o = JS_NewObject(c.ctx);  // triggers the wrap
        check_row("id32_wrap_disabled", JS_ICShapeGuardEnabled(c.rt) == 0);
        JSICShapeGuardSite* s = JS_ICShapeGuardNew(c.rt);
        check_row("id32_wrap_update_rejected",
                  JS_ICShapeGuardUpdate(c.ctx, s, o) == -1);
        check_row("id32_wrap_check_zero", JS_ICShapeGuardCheck(s, o) == 0);
        JS_ICShapeGuardRelease(c.rt, s);
        JS_FreeValue(c.ctx, o);
    }
#endif
    return 0;
}

// --- benchmarks -----------------------------------------------------------

// resident bytes, from /proc/self/statm (RSS * page size).
long resident_bytes() {
    long rss_pages = 0;
    FILE* f = std::fopen("/proc/self/statm", "r");
    if (!f)
        return -1;
    long size_pages = 0;
    if (std::fscanf(f, "%ld %ld", &size_pages, &rss_pages) != 2)
        rss_pages = 0;
    std::fclose(f);
    return rss_pages * 4096;
}

int run_bench(Ctx c) {
    const int kChecks = 1 << 22;
    const int kUpdates = 1 << 18;
    const int kMutations = 1 << 16;
    const int kSites = 20000;

    // check-hit path.
    {
        JSValue o = JS_NewObject(c.ctx);
        JSICShapeGuardSite* s = JS_ICShapeGuardNew(c.rt);
        JS_ICShapeGuardUpdate(c.ctx, s, o);
        volatile int sink = 0;
        for (int i = 0; i < 4096; i++)  // warm up
            sink += JS_ICShapeGuardCheck(s, o);
#if defined(__x86_64__)
        uint64_t t0 = rdtsc();
        for (int i = 0; i < kChecks; i++)
            sink += JS_ICShapeGuardCheck(s, o);
        uint64_t t1 = rdtsc();
        std::printf("bench check_hit_cycles %.3f\n",
                    (double)(t1 - t0) / kChecks);
#endif
        auto a = std::chrono::steady_clock::now();
        for (int i = 0; i < kChecks; i++)
            sink += JS_ICShapeGuardCheck(s, o);
        auto b = std::chrono::steady_clock::now();
        double ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count();
        std::printf("bench check_hit_ns %.3f\n", ns / kChecks);
        (void)sink;
        JS_ICShapeGuardRelease(c.rt, s);
        JS_FreeValue(c.ctx, o);
    }
    // update (retrain) path.
    {
        JSValue o = JS_NewObject(c.ctx);
        JSICShapeGuardSite* s = JS_ICShapeGuardNew(c.rt);
        volatile int sink = 0;
        for (int i = 0; i < 4096; i++)
            sink += JS_ICShapeGuardUpdate(c.ctx, s, o);
#if defined(__x86_64__)
        uint64_t t0 = rdtsc();
        for (int i = 0; i < kUpdates; i++)
            sink += JS_ICShapeGuardUpdate(c.ctx, s, o);
        uint64_t t1 = rdtsc();
        std::printf("bench update_cycles %.3f\n",
                    (double)(t1 - t0) / kUpdates);
#endif
        auto a = std::chrono::steady_clock::now();
        for (int i = 0; i < kUpdates; i++)
            sink += JS_ICShapeGuardUpdate(c.ctx, s, o);
        auto b = std::chrono::steady_clock::now();
        double ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count();
        std::printf("bench update_ns %.3f\n", ns / kUpdates);
        (void)sink;
        JS_ICShapeGuardRelease(c.rt, s);
        JS_FreeValue(c.ctx, o);
    }
    // mutation cost with a live guard: add/delete a prop per iteration
    // (exercises prepare_update + add_shape_property / delete funnels).
    {
        JSValue o = JS_NewObject(c.ctx);
        JSICShapeGuardSite* s = JS_ICShapeGuardNew(c.rt);
        JS_ICShapeGuardUpdate(c.ctx, s, o);
        JSAtom a = JS_NewAtom(c.ctx, "m");
        for (int i = 0; i < 256; i++) {  // warm up
            JS_DefinePropertyValue(
                c.ctx, o, a, JS_NewInt32(c.ctx, i),
                JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE | JS_PROP_ENUMERABLE);
            JS_DeleteProperty(c.ctx, o, a, 0);
        }
#if defined(__x86_64__)
        uint64_t t0 = rdtsc();
        for (int i = 0; i < kMutations; i++) {
            JS_DefinePropertyValue(
                c.ctx, o, a, JS_NewInt32(c.ctx, i),
                JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE | JS_PROP_ENUMERABLE);
            JS_DeleteProperty(c.ctx, o, a, 0);
        }
        uint64_t t1 = rdtsc();
        std::printf("bench mutation_cycles_per_pair %.3f\n",
                    (double)(t1 - t0) / kMutations);
#endif
        JS_ICShapeGuardRelease(c.rt, s);
        JS_FreeAtom(c.ctx, a);
        JS_FreeValue(c.ctx, o);
    }
    // retained memory: kSites distinct-shape guards, owners freed, GC.
    {
        std::vector<JSICShapeGuardSite*> sites;
        sites.reserve(kSites);
        // one shared proto so the retained delta is shapes + sites only
        JSValue proto = JS_NewObject(c.ctx);
        for (int i = 0; i < kSites; i++) {
            char name[24];
            std::snprintf(name, sizeof(name), "p%d", i);
            JSValue o = JS_NewObjectProto(c.ctx, proto);
            JSAtom a = JS_NewAtom(c.ctx, name);
            JS_DefinePropertyValue(
                c.ctx, o, a, JS_NewInt32(c.ctx, i),
                JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE | JS_PROP_ENUMERABLE);
            JS_FreeAtom(c.ctx, a);
            JSICShapeGuardSite* s = JS_ICShapeGuardNew(c.rt);
            JS_ICShapeGuardUpdate(c.ctx, s, o);
            sites.push_back(s);
            JS_FreeValue(c.ctx, o);  // site holds the only live ref to the shape
        }
        JS_RunGC(c.rt);
        long rss = resident_bytes();
        std::printf("bench retained_bytes_per_site %ld\n",
                    rss > 0 ? rss / kSites : -1);
        for (JSICShapeGuardSite* s : sites)
            JS_ICShapeGuardRelease(c.rt, s);
        JS_FreeValue(c.ctx, proto);
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);  // keep row order on crash
    Ctx c = make_ctx();
    bool bench = argc > 1 && std::strcmp(argv[1], "--bench") == 0;
    int rc = run_matrix(c);
    if (bench)
        rc |= run_bench(c);
    free_ctx(c);
    std::printf(g_fail ? "RESULT FAIL\n" : "RESULT PASS\n");
    return g_fail ? 1 : 0;
}
