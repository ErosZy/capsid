// I2 region census gate (tier-3 plan docs/quickjs-ng-cfg-ssa-shape-ic.md
// §4): matches multi-instruction candidate fusion regions (the §4.2
// template catalog) on the SSA form and reports static plus exact-site
// dynamically weighted coverage, guard requirements,
// slow-path duplication, and the §4.1 predicted cost, selecting the
// at-most-two first templates. Part A drives the bundle-level
// region_round_trip walker on hand-built canonical BC26 function blobs
// and asserts the exact per-template aggregates: i32 arith chains at a
// join (the both-immediates exclusion), shape-guard chains over
// literal objects, array get/update sites with provably-int indices,
// the 8-instruction region cap, the handler-boundary exclusion, and
// the at-most-two selection. Part B covers the bundle walker's
// rejected-coverage contract. Part C compiles representative module
// sources through the real quickjs-ng compiler and asserts ZERO
// rejected functions / insns — the mandatory 0-rejection gate over the
// corpus — plus real candidate demand.
//
// Byte values are the serialized opcode space (quickjs-opcode.h
// physical order, temps excluded): object=11, drop=14, return_undef=41,
// get_field=64, put_field=66, get_array_el=70, get_array_el2=71,
// put_array_el=72, put_loc0=207, catch=107, nip_catch=110, add=156, dup=17, push_0=186,
// push_1=187, push_i8=194, if_false8=240, if_true8=241, goto8=242.
// Jump targets: pc + size + signed aux (catch aux is the 4-byte diff at
// pc+1; if_true8/goto8 aux is the u8 at pc+1). get_field/put_field
// carry a 4-byte atom operand; the census never reads it.

#include "bytecode_optimizer/bytecode_optimizer.h"
#include "bytecode_optimizer/ir/cfg.h"
#include "bytecode_optimizer/ir/region.h"
#include "bytecode_optimizer/ir/ssa.h"
#include "quickjs.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,  \
                         #cond);                                          \
            g_failures++;                                                 \
        }                                                                 \
    } while (0)

namespace ir = capsid::bytecode::ir;

void put_leb(std::vector<std::uint8_t>* v, std::uint32_t x) {
    while (x >= 0x80) {
        v->push_back(static_cast<std::uint8_t>((x & 0x7f) | 0x80));
        x >>= 7;
    }
    v->push_back(static_cast<std::uint8_t>(x));
}

void put_u16(std::vector<std::uint8_t>* v, std::uint16_t x) {
    v->push_back(static_cast<std::uint8_t>(x & 0xff));
    v->push_back(static_cast<std::uint8_t>(x >> 8));
}

void put_u32(std::vector<std::uint8_t>* v, std::uint32_t x) {
    for (int i = 0; i < 4; i++) {
        v->push_back(static_cast<std::uint8_t>(x & 0xff));
        x >>= 8;
    }
}

std::uint32_t bc_csum(const std::uint8_t* p, std::size_t n) {
    std::uint32_t h = 0;
    std::size_t i = 0;
    for (; i + 4 < n; i += 4) {
        h += static_cast<std::uint32_t>(p[i]) |
             (static_cast<std::uint32_t>(p[i + 1]) << 8) |
             (static_cast<std::uint32_t>(p[i + 2]) << 16) |
             (static_cast<std::uint32_t>(p[i + 3]) << 24);
        h *= 0x9e370001u;
    }
    std::uint32_t a = 0, b = 0, c = 0;
    switch (n - i) {
    case 3: c = p[i + 2];  // fallthrough
    case 2: b = p[i + 1];  // fallthrough
    case 1: a = p[i];
    }
    h += a | (b << 8) | (c << 16);
    h *= 0x9e370001u;
    return h;
}

std::vector<std::uint8_t> make_bundle(
    const std::vector<std::uint8_t>& code, std::uint32_t stack_size,
    const std::vector<std::uint8_t>* debug = nullptr) {
    std::vector<std::uint8_t> b;
    b.push_back(26);  // BC_VERSION
    put_u32(&b, 0);   // checksum, patched below
    put_leb(&b, 1);   // atom count
    b.push_back(0);   // atom 0: const 0
    put_u32(&b, 0);
    b.push_back(13);  // BC_TAG_MODULE
    put_leb(&b, 0);   // module name atom
    put_leb(&b, 0);   // req / export / star / import counts
    put_leb(&b, 0);
    put_leb(&b, 0);
    put_leb(&b, 0);
    b.push_back(0);   // has_tla
    b.push_back(12);  // BC_TAG_FUNCTION_BYTECODE
    put_u16(&b, debug != nullptr ? (1u << 11) : 0u);  // allow_debug
    b.push_back(1);   // strict
    put_leb(&b, 0);   // function name atom
    put_leb(&b, 0);   // arg_count
    put_leb(&b, 0);   // var_count
    put_leb(&b, 0);   // defined_arg_count
    put_leb(&b, stack_size);
    put_leb(&b, 0);   // var_ref_count
    put_leb(&b, 0);   // closure_var_count
    put_leb(&b, 0);   // cpool_count
    put_leb(&b, static_cast<std::uint32_t>(code.size()));
    put_leb(&b, 0);   // vardef count
    b.insert(b.end(), code.begin(), code.end());
    if (debug != nullptr) {
        b.insert(b.end(), debug->begin(), debug->end());
    }
    std::uint32_t c = bc_csum(b.data() + 5, b.size() - 5);
    b[1] = static_cast<std::uint8_t>(c);
    b[2] = static_cast<std::uint8_t>(c >> 8);
    b[3] = static_cast<std::uint8_t>(c >> 16);
    b[4] = static_cast<std::uint8_t>(c >> 24);
    return b;
}

std::vector<std::uint8_t> make_debug(
    const std::vector<std::uint8_t>& pc2line) {
    std::vector<std::uint8_t> d;
    put_leb(&d, 0);  // filename atom
    put_leb(&d, 1);  // line
    put_leb(&d, 1);  // col
    put_leb(&d, static_cast<std::uint32_t>(pc2line.size()));
    d.insert(d.end(), pc2line.begin(), pc2line.end());
    put_leb(&d, 0);  // source_len
    return d;
}

// Bundle-level census on a synthetic function blob.
bool census_blob(const std::vector<std::uint8_t>& code,
                 std::uint32_t stack_size, ir::RegionCensusReport* out,
                 std::string* err) {
    std::vector<std::uint8_t> b = make_bundle(code, stack_size);
    return ir::region_round_trip(b.data(), b.size(), out, err);
}

// Assert template t's exact aggregates and every other template's zero.
void check_tmpl(const ir::RegionCensusReport& rep, ir::Template t,
                std::uint64_t cand, std::uint64_t insns,
                std::int64_t pred_total, std::int64_t pred_best) {
    const size_t k = static_cast<size_t>(t);
    CHECK(rep.candidates[k] == cand);
    CHECK(rep.insns_covered[k] == insns);
    CHECK(rep.predicted_total[k] == pred_total);
    CHECK(rep.predicted_best[k] == pred_best);
    for (size_t i = 0; i < static_cast<size_t>(ir::Template::COUNT); i++) {
        if (i == k) continue;
        CHECK(rep.candidates[i] == 0);
        CHECK(rep.insns_covered[i] == 0);
        CHECK(rep.predicted_total[i] == 0);
    }
}

// ---------------------------------------------------------------------------
// Part A: the synthetic matrix.
// ---------------------------------------------------------------------------

// r1: a sound i32 chain. The first `and` folds two immediate-tag inputs
// conceptually and is excluded; its result has no immediate, so the next
// two adjacent bitwise ops form a two-node candidate. Unlike add/sub/mul,
// bitwise normal results cannot overflow out of INT32.
void test_r1_i32_chain() {
    // push_0; push_1; push_2; push_3; and; and; and; return_undef.
    const std::vector<std::uint8_t> code = {186, 187, 188, 189,
                                            161, 161, 161, 41};
    std::string err;
    ir::RegionCensusReport rep;
    CHECK(census_blob(code, 4, &rep, &err));
    if (err.empty() && rep.functions == 0) return;
    CHECK(rep.functions == 1);
    CHECK(rep.rejected_functions == 0);
    // One candidate run [add, add]: 2 insns, 0 guards, slow = 2 bytes,
    // predicted = 2*2 - 0 - 1 - 2/8 = 3.
    check_tmpl(rep, ir::Template::I32_ARITH_CHAIN, 1, 2, 3, 3);
}

// r2: shape-guard chain over a literal object — two adjacent
// get_fields on an OBJECT_SHAPES object form one 2-node candidate.
void test_r2_shape_chain() {
    // object; get_field; get_field; return_undef.
    const std::vector<std::uint8_t> code = {11,  64, 0, 0, 0, 0,
                                            64,  0,  0, 0, 0, 41};
    std::string err;
    ir::RegionCensusReport rep;
    CHECK(census_blob(code, 1, &rep, &err));
    if (err.empty() && rep.functions == 0) return;
    CHECK(rep.functions == 1);
    CHECK(rep.rejected_functions == 0);
    // One run [gf, gf]: 2 insns, 1 guard, slow = 10 bytes, predicted =
    // 2*2 - 1 - 1 - 10/8 = 1.
    check_tmpl(rep, ir::Template::SHAPE_GET_OWN, 1, 2, 1, 1);
}

// r3: isolated array gets are quickening candidates, not OP_ext fusion
// regions. The push between them prevents a multi-instruction run.
void test_r3_array_get() {
    // object; push_0; get_array_el; push_1; get_array_el2; return_undef.
    const std::vector<std::uint8_t> code = {11, 186, 70, 187, 71, 41};
    std::string err;
    ir::RegionCensusReport rep;
    CHECK(census_blob(code, 3, &rep, &err));
    if (err.empty() && rep.functions == 0) return;
    CHECK(rep.functions == 1);
    CHECK(rep.rejected_functions == 0);
    for (size_t i = 0; i < static_cast<size_t>(ir::Template::COUNT); i++)
        CHECK(rep.candidates[i] == 0);
}

// r4: one array update is likewise not a fusion region.
void test_r4_array_update() {
    // object; push_0; push_1; put_array_el; return_undef.
    const std::vector<std::uint8_t> code = {11, 186, 187, 72, 41};
    std::string err;
    ir::RegionCensusReport rep;
    CHECK(census_blob(code, 3, &rep, &err));
    if (err.empty() && rep.functions == 0) return;
    CHECK(rep.functions == 1);
    CHECK(rep.rejected_functions == 0);
    for (size_t i = 0; i < static_cast<size_t>(ir::Template::COUNT); i++)
        CHECK(rep.candidates[i] == 0);
}

// r5: the 8-instruction region cap — ten adjacent get_fields split
// into an 8-node candidate and a 2-node candidate.
void test_r5_cap() {
    // object; get_field x10; return_undef.
    std::vector<std::uint8_t> code = {11};
    for (int i = 0; i < 10; i++) {
        code.push_back(64);
        code.push_back(0);
        code.push_back(0);
        code.push_back(0);
        code.push_back(0);
    }
    code.push_back(41);
    std::string err;
    ir::RegionCensusReport rep;
    CHECK(census_blob(code, 1, &rep, &err));
    if (err.empty() && rep.functions == 0) return;
    CHECK(rep.functions == 1);
    CHECK(rep.rejected_functions == 0);
    // 8-run: 2*8 - 1 - 1 - 40/8 = 9; 2-run: 2*2 - 1 - 1 - 10/8 = 1.
    check_tmpl(rep, ir::Template::SHAPE_GET_OWN, 2, 10, 10, 9);
}

// r6: the handler-boundary exclusion — get_array_el inside a try body
// has an exception successor and is never a candidate member.
void test_r6_handler() {
    // object; catch +9; push_0; get_array_el; drop; drop; return_undef;
    // [handler] nip_catch; return_undef.
    const std::vector<std::uint8_t> code = {11,  107, 9, 0, 0, 0, 186, 70,
                                            14,  14,  41, 110, 41};
    std::string err;
    ir::RegionCensusReport rep;
    CHECK(census_blob(code, 3, &rep, &err));
    if (err.empty() && rep.functions == 0) return;
    CHECK(rep.functions == 1);
    CHECK(rep.rejected_functions == 0);
    // No candidates anywhere.
    for (size_t i = 0; i < static_cast<size_t>(ir::Template::COUNT); i++) {
        CHECK(rep.candidates[i] == 0);
        CHECK(rep.insns_covered[i] == 0);
        CHECK(rep.predicted_total[i] == 0);
    }
    CHECK(rep.first_templates[0] == ir::Template::COUNT);
    CHECK(rep.first_templates[1] == ir::Template::COUNT);
}

// r7: one shape put belongs to an IC/quickening path, not fusion.
void test_r7_shape_put() {
    // object; push_0; put_field; return_undef.
    const std::vector<std::uint8_t> code = {11, 186, 66, 0, 0, 0, 0, 41};
    std::string err;
    ir::RegionCensusReport rep;
    CHECK(census_blob(code, 2, &rep, &err));
    if (err.empty() && rep.functions == 0) return;
    CHECK(rep.functions == 1);
    CHECK(rep.rejected_functions == 0);
    for (size_t i = 0; i < static_cast<size_t>(ir::Template::COUNT); i++)
        CHECK(rep.candidates[i] == 0);
}

// r8: the at-most-two selection — a bundle with a shape chain
// (predicted 1) and an i32 chain (predicted 3) selects the i32 chain
// first, the shape chain second, and nothing else.
void test_r8_selection() {
    // object; get_field; get_field; push_0..push_3; and; and; and;
    // return_undef. The first and is the both-immediate exclusion.
    const std::vector<std::uint8_t> code = {11,  64,  0,  0,  0,  0,  64,
                                            0,   0,   0,  0,  186, 187,
                                            188, 189, 161, 161, 161, 41};
    std::string err;
    ir::RegionCensusReport rep;
    CHECK(census_blob(code, 5, &rep, &err));
    if (err.empty() && rep.functions == 0) return;
    CHECK(rep.functions == 1);
    CHECK(rep.rejected_functions == 0);
    CHECK(rep.candidates[static_cast<size_t>(ir::Template::SHAPE_GET_OWN)] ==
          1);
    CHECK(rep.candidates[static_cast<size_t>(ir::Template::I32_ARITH_CHAIN)] ==
          1);
    CHECK(rep.first_templates[0] == ir::Template::I32_ARITH_CHAIN);
    CHECK(rep.first_predicted[0] == 3);
    CHECK(rep.first_templates[1] == ir::Template::SHAPE_GET_OWN);
    CHECK(rep.first_predicted[1] == 1);
    CHECK(rep.first_candidates[0] == 1);
    CHECK(rep.first_candidates[1] == 1);
}

// r9: the same static candidates reverse order under exact dynamic weights:
// the shape chain runs 1000 times while the i32 chain runs 10. Missing exact
// site evidence gives a region weight of zero instead of silently using the
// static count.
void test_r9_dynamic_weighting() {
    const std::vector<std::uint8_t> code = {11,  64,  0,  0,  0,  0,  64,
                                            0,   0,   0,  0,  186, 187,
                                            188, 189, 161, 161, 161, 41};
    std::vector<std::uint8_t> bundle = make_bundle(code, 5);
    ir::RegionExecutionProfile profile;
    profile.sites.push_back({0, 1, 1000});
    profile.sites.push_back({0, 6, 1000});
    profile.sites.push_back({0, 16, 10});
    profile.sites.push_back({0, 17, 10});
    ir::RegionCensusReport rep;
    std::string err;
    CHECK(ir::region_round_trip_profiled(bundle.data(), bundle.size(),
                                          profile, &rep, &err));
    CHECK(rep.has_dynamic_profile);
    CHECK(rep.missing_profile_sites == 0);
    const size_t shape = static_cast<size_t>(ir::Template::SHAPE_GET_OWN);
    const size_t i32 = static_cast<size_t>(ir::Template::I32_ARITH_CHAIN);
    CHECK(rep.dynamic_candidates[shape] == 1000);
    CHECK(rep.dynamic_insns_covered[shape] == 2000);
    CHECK(rep.dynamic_predicted_total[shape] == 1000);
    CHECK(rep.dynamic_candidates[i32] == 10);
    CHECK(rep.dynamic_predicted_total[i32] == 30);
    CHECK(rep.first_templates[0] == ir::Template::SHAPE_GET_OWN);
    CHECK(rep.first_templates[1] == ir::Template::I32_ARITH_CHAIN);

    profile.sites.erase(profile.sites.begin() + 1);  // shape pc 6 missing
    CHECK(ir::region_round_trip_profiled(bundle.data(), bundle.size(),
                                          profile, &rep, &err));
    CHECK(rep.missing_profile_sites == 1);
    CHECK(rep.dynamic_candidates[shape] == 0);
    CHECK(rep.first_templates[0] == ir::Template::I32_ARITH_CHAIN);
    CHECK(rep.first_templates[1] == ir::Template::COUNT);
}

void test_region_blobs() {
    test_r1_i32_chain();
    test_r2_shape_chain();
    test_r3_array_get();
    test_r4_array_update();
    test_r5_cap();
    test_r6_handler();
    test_r7_shape_put();
    test_r8_selection();
    test_r9_dynamic_weighting();
}

// ---------------------------------------------------------------------------
// Part B: bundle-level round trip (rejected coverage is counted, never
// skipped).
// ---------------------------------------------------------------------------

void test_region_bundles() {
    std::string err;
    ir::RegionCensusReport rep;

    // Canonical bundle with a valid debug block: the walker covers the
    // function with zero rejections and exact counts.
    std::vector<std::uint8_t> dbg = make_debug({2, 0});
    std::vector<std::uint8_t> good =
        make_bundle({11, 186, 70, 41}, 2, &dbg);
    CHECK(ir::region_round_trip(good.data(), good.size(), &rep, &err));
    CHECK(rep.functions == 1);
    CHECK(rep.rejected_functions == 0);
    CHECK(rep.rejected_insns == 0);
    // object + push_0 + one get_array_el has no multi-op fusion region.
    CHECK(rep.candidates[static_cast<size_t>(
             ir::Template::FAST_ARRAY_GET_I32)] == 0);
    // The public entry agrees.
    CHECK(capsid::bytecode::region_census(good, &err));
    CHECK(err.empty());

    // Malformed pc2line (truncated sleb): the function cannot decode,
    // so it is counted as rejected coverage — never a silent skip.
    std::vector<std::uint8_t> bad_dbg = make_debug({2, 0x80});
    std::vector<std::uint8_t> bad = make_bundle({11, 41}, 1, &bad_dbg);
    CHECK(ir::region_round_trip(bad.data(), bad.size(), &rep, &err));
    CHECK(rep.rejected_functions == 1);
}

// ---------------------------------------------------------------------------
// Part C: the mandatory 0-rejection gate over the real corpus.
// ---------------------------------------------------------------------------

void test_corpus_regions() {
    JSRuntime* rt = JS_NewRuntime();
    CHECK(rt != nullptr);
    JSContext* ctx = JS_NewContext(rt);
    CHECK(ctx != nullptr);
    if (ctx == nullptr) return;

    // The test_ssa corpus (the four analysis-correctness fixes plus the
    // I1 coverage matrix) — the census rides the same walker, so the
    // 0-rejection gate carries over unchanged.
    const char* sources[] = {
        "globalThis.__r = (1 + 2) * (3 + 4);",
        "let acc = 0; for (let i = 0; i < 1000; i++) acc += i; "
        "globalThis.__r = acc;",
        "function fib(n) { return n < 2 ? n : fib(n-1) + fib(n-2); } "
        "globalThis.__r = fib(10);",
        "let r = 0; try { r = 1; throw new Error('x'); } catch (e) { "
        "r = 2; } finally { r += 10; } globalThis.__r = r;",
        "function* g() { yield 1; yield 2; } let s = 0; "
        "for (const x of g()) s += x; globalThis.__r = s;",
        "async function f() { const p = Promise.resolve(3); "
        "return await p + 1; } globalThis.__r = typeof f();",
        "async function f() { const g = { async *[Symbol.asyncIterator]() "
        "{ yield 1; yield 2; } }; let s = 0; for await (const x of g) "
        "s += x; return s; } globalThis.__r = typeof f();",
        "class A { constructor() { this.v = 3; } m() { return this.v * 2; }"
        "} class B extends A { constructor() { super(); } }"
        "globalThis.__r = new B().m();",
        "globalThis.__r = eval('1 + 2') * 3;",
        "globalThis.__r = typeof import('x').then;",
        "function f(x) { switch (x) { case 1: return 'a'; "
        "case 2: return 'b'; default: return 'c'; } }"
        "globalThis.__r = f(2);",
        "function outer() { let x = 1; return () => ++x; }"
        "const inc = outer(); inc(); globalThis.__r = inc();",
        "let s = 0; for (const [a, b] of [[1, 2], [3, 4]]) s += a + b; "
        "globalThis.__r = s;",
        "const o = { a: 1, b: 2 }; let s = 0; "
        "for (const k in o) s += o[k]; globalThis.__r = s;",
        "const o = { x: { y: 5 } }; globalThis.__r = o.x?.y ?? 0;",
        "function tag(s, ...v) { return v.length; }"
        "globalThis.__r = tag`a${1}b${2}c` + `${1 + 1}-${'x'.repeat(2)}`"
        ".length;",
        "globalThis.__r = Number(1n + 2n);",
        "function f(a, b, c) { return a + b + c; }"
        "const a = [1, 2, 3]; const b = [...a, 4];"
        "globalThis.__r = f(...b) + b.length;",
        "using x = { [Symbol.dispose]() {} };"
        "let s = 0; using y = { [Symbol.dispose]() { s = 1; } };"
        "globalThis.__r = s;",
        "let n = 0; outer: for (let i = 0; i < 3; i++) { "
        "for (let j = 0; j < 3; j++) { if (j === 1) continue outer; "
        "n++; } } globalThis.__r = n;",
        "const o = { get x() { return 1; }, set x(v) {} };"
        "o.x = 2; globalThis.__r = o.x;",
        "let r = 0; try { throw 1; } catch { r = 1; }"
        "globalThis.__r = r;",
        "const o = { v: 3, m() { return (() => this.v)(); } };"
        "globalThis.__r = o.m();",
        "globalThis.__r = /ab+c/.test('xabbc') ? 1 : 0;",
        "const { a = 5, b = 6 } = { a: 1 };"
        "const [p, ...qs] = [1, 2, 3]; globalThis.__r = a + b + qs.length;",
        "const k = 'n'; const o = { n: 42 };"
        "globalThis.__r = o[k] + [1, 2, 3][1];",
        "let y; let x; y = (x = 5); x = 6; globalThis.__r = y;",
        "const o = { a: 1 }; let s = 0; "
        "for (let i = 0; i < 10; i++) { o.a = o.a + 1; s += o.a; }"
        "globalThis.__r = s;",
        "let r = 0; try { try { throw 1; } catch { throw 2; } } "
        "catch { r = 1; } globalThis.__r = r;",
        "let r = 0; try { if (globalThis.__c) { r = 1; } "
        "else { r = 2; } } catch { r = 3; } globalThis.__r = r;",
        "let x; if (globalThis.__c) { x = 1; } x = 5; "
        "globalThis.__r = x;",
    };
    for (const char* src : sources) {
        JSValue module = JS_Eval(ctx, src, std::strlen(src), "region.js",
                                 JS_EVAL_TYPE_MODULE |
                                     JS_EVAL_FLAG_COMPILE_ONLY);
        if (JS_IsException(module)) {
            JSValue ex = JS_GetException(ctx);
            const char* s = JS_ToCString(ctx, ex);
            std::fprintf(stderr, "FAIL: source failed to compile: %s\n%s\n",
                         s ? s : "<no message>", src);
            JS_FreeCString(ctx, s);
            JS_FreeValue(ctx, ex);
            g_failures++;
            continue;
        }
        std::size_t size = 0;
        std::uint8_t* data =
            JS_WriteObject(ctx, &size, module, JS_WRITE_OBJ_BYTECODE);
        JS_FreeValue(ctx, module);
        if (data == nullptr) {
            std::fprintf(stderr, "FAIL: serialize failed for: %s\n", src);
            g_failures++;
            continue;
        }
        std::vector<std::uint8_t> buf(data, data + size);
        js_free(ctx, data);

        std::string err;
        ir::RegionCensusReport rep;
        if (!ir::region_round_trip(buf.data(), buf.size(), &rep, &err)) {
            std::fprintf(stderr, "FAIL: region gate failed for: %s\n  %s\n",
                         src, err.c_str());
            g_failures++;
            continue;
        }
        if (rep.rejected_functions != 0 || rep.rejected_insns != 0) {
            std::fprintf(stderr,
                         "FAIL: rejected coverage for: %s\n  "
                         "rejected %llu functions / %llu insns\n",
                         src,
                         static_cast<unsigned long long>(
                             rep.rejected_functions),
                         static_cast<unsigned long long>(rep.rejected_insns));
            g_failures++;
            continue;
        }
        CHECK(rep.functions > 0);
        // The public entry agrees on every corpus bundle.
        CHECK(capsid::bytecode::region_census(buf, &err));
        if (g_failures > 5) break;  // don't flood past the first few
    }
    // Candidate demand is asserted by the synthetic matrix. Zero candidates
    // in this corpus is a valid decision result and must block inventing an
    // OP_ext fusion without broader dynamic evidence.
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
}

}  // namespace

int main() {
    test_region_blobs();
    test_region_bundles();
    test_corpus_regions();
    if (g_failures != 0) {
        std::fprintf(stderr, "test_regions: %d failure(s)\n", g_failures);
        return 1;
    }
    std::fprintf(stderr, "test_regions: all checks passed\n");
    return 0;
}
