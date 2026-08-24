// F0 ext foundation gate. R0's single-op array specialization regressed the
// paired benchmark and is retired: ext id 1 is a permanent size-zero hole.
// Until a profile-weighted multi-instruction template clears the evidence
// gates, the only canonical output is BC26 and every BC27 input fails closed.
// This test locks that reserved-id policy plus the production BC26-only gate.

#include "bytecode_optimizer/bytecode_optimizer.h"
#include "bytecode_optimizer/ir/cfg.h"
#include "bytecode_optimizer/ir/ext.h"
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

// One-module bundle with a single function whose code blob is `code`.
// `version` is the wire version byte (26 or 27); the checksum covers
// everything after it, so flipping the version alone never breaks it.
std::vector<std::uint8_t> make_bundle(
    const std::vector<std::uint8_t>& code, std::uint32_t stack_size,
    uint8_t version,
    const std::vector<std::uint8_t>* debug = nullptr) {
    std::vector<std::uint8_t> b;
    b.push_back(version);
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

// Bundle-level ext round trip on a synthetic function blob.
bool ext_blob(const std::vector<std::uint8_t>& code,
              std::uint32_t stack_size, uint8_t version,
              ir::ExtRoundTripReport* out, std::string* err) {
    std::vector<std::uint8_t> b = make_bundle(code, stack_size, version);
    return ir::ext_round_trip(b.data(), b.size(), out, err);
}

// ---------------------------------------------------------------------------
// Part A: the synthetic matrix.
// ---------------------------------------------------------------------------

// a1: BC26 baseline — plain code parses, no ext, no rejections; the
// public entry agrees.
void test_a1_bc26_baseline() {
    const std::vector<std::uint8_t> code = {186, 41};  // push_0; return_undef
    std::string err;
    ir::ExtRoundTripReport rep;
    CHECK(ext_blob(code, 1, 26, &rep, &err));
    CHECK(rep.functions == 1);
    CHECK(rep.ext_instructions == 0);
    CHECK(rep.rejected_functions == 0);
    CHECK(rep.rejected_insns == 0);
    CHECK(capsid::bytecode::ext_round_trip(
        make_bundle(code, 1, 26), &err));
}

// a2: BC26 must reject OP_ext outright (E0 reader contract), even with
// a valid checksum.
void test_a2_bc26_rejects_ext() {
    const std::vector<std::uint8_t> code = {252, 1, 186, 41};
    std::string err;
    ir::ExtRoundTripReport rep;
    CHECK(!ext_blob(code, 1, 26, &rep, &err));
    CHECK(err.find("ext instruction in BC26") != std::string::npos);
}

// a3: retired R0 id 1 is permanently reserved. Keeping the hole prevents an
// archived BC27/R0 blob from silently changing meaning in a later release.
void test_a3_bc27_reserved_r0_id() {
    const std::vector<std::uint8_t> code = {186, 187, 252, 1, 41};
    std::string err;
    ir::ExtRoundTripReport rep;
    CHECK(!ext_blob(code, 2, 27, &rep, &err));
    CHECK(err.find("invalid ext id 1") != std::string::npos);
}

// a4: BC27 with an unknown ext id fails closed at the reader.
void test_a4_bc27_unknown_id() {
    const std::vector<std::uint8_t> code = {252, 2, 186, 41};
    std::string err;
    ir::ExtRoundTripReport rep;
    CHECK(!ext_blob(code, 1, 27, &rep, &err));
    CHECK(err.find("invalid ext id 2") != std::string::npos);
}

// a5: ext id 0 is the reserved invalid id — rejected.
void test_a5_bc27_id_zero() {
    const std::vector<std::uint8_t> code = {252, 0, 186, 41};
    std::string err;
    ir::ExtRoundTripReport rep;
    CHECK(!ext_blob(code, 1, 27, &rep, &err));
    CHECK(err.find("invalid ext id 0") != std::string::npos);
}

// a6: truncation — OP_ext as the last byte has no id byte.
void test_a6_bc27_truncated() {
    const std::vector<std::uint8_t> code = {252};
    std::string err;
    ir::ExtRoundTripReport rep;
    CHECK(!ext_blob(code, 0, 27, &rep, &err));
    CHECK(err.find("truncated instruction") != std::string::npos);
}

// a7: BC27 without any ext instruction is noncanonical (the writer
// emits BC26 otherwise) — rejected.
void test_a7_bc27_noncanonical() {
    const std::vector<std::uint8_t> code = {186, 41};
    std::string err;
    ir::ExtRoundTripReport rep;
    CHECK(!ext_blob(code, 1, 27, &rep, &err));
    CHECK(err.find("noncanonical") != std::string::npos);
}

// a8: production gates reject BC27 input and keep accepting BC26. There is no
// deployed ext emitter in the optimizer.
void test_a8_production_gates() {
    std::string err;
    std::vector<std::uint8_t> b27 =
        make_bundle({186, 187, 252, 1, 41}, 2, 27);
    std::vector<std::uint8_t> out;
    CHECK(!capsid::bytecode::optimize(b27, &out, capsid::bytecode::kPassAll,
                                      false, &err));
    CHECK(err.find("not re-optimizable") != std::string::npos);
    std::string err2;
    CHECK(!capsid::bytecode::analyze_only(b27, &err2));
    CHECK(err2.find("not re-analyzable") != std::string::npos);

    std::string err3;
    std::vector<std::uint8_t> b26 = make_bundle({186, 41}, 1, 26);
    CHECK(capsid::bytecode::optimize(b26, &out,
                                     capsid::bytecode::kPassAll, false,
                                     &err3));
}

// ---------------------------------------------------------------------------
// Part B: the mandatory 0-rejection gate over the real corpus.
// ---------------------------------------------------------------------------

void test_corpus_ext() {
    JSRuntime* rt = JS_NewRuntime();
    CHECK(rt != nullptr);
    JSContext* ctx = JS_NewContext(rt);
    CHECK(ctx != nullptr);
    if (ctx == nullptr) return;

    // The established corpus subset (test_regions Part C). BC26 sources
    // never contain ext instructions (the writer emits BC26 without a
    // quickened site), so the gate is: zero rejections, zero ext, and
    // the public entry agrees on every bundle.
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
    };
    uint64_t functions = 0;
    for (const char* src : sources) {
        JSValue module = JS_Eval(ctx, src, std::strlen(src), "ext.js",
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
        ir::ExtRoundTripReport rep;
        if (!ir::ext_round_trip(buf.data(), buf.size(), &rep, &err)) {
            std::fprintf(stderr, "FAIL: ext gate failed for: %s\n  %s\n",
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
                         static_cast<unsigned long long>(
                             rep.rejected_insns));
            g_failures++;
            continue;
        }
        CHECK(rep.functions > 0);
        CHECK(rep.ext_instructions == 0);  // BC26 sources: no ext
        functions += rep.functions;
        // The public entry agrees on every corpus bundle.
        CHECK(capsid::bytecode::ext_round_trip(buf, &err));
        if (g_failures > 5) break;  // don't flood past the first few
    }
    CHECK(functions > 0);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
}

}  // namespace

int main() {
    test_a1_bc26_baseline();
    test_a2_bc26_rejects_ext();
    test_a3_bc27_reserved_r0_id();
    test_a4_bc27_unknown_id();
    test_a5_bc27_id_zero();
    test_a6_bc27_truncated();
    test_a7_bc27_noncanonical();
    test_a8_production_gates();
    test_corpus_ext();
    if (g_failures != 0) {
        std::fprintf(stderr, "test_ext_round_trip: %d failure(s)\n",
                     g_failures);
        return 1;
    }
    std::fprintf(stderr, "test_ext_round_trip: all green\n");
    return 0;
}
