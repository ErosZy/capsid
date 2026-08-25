// I0 CFG gate (docs/quickjs-optimization.md §2/§6):
// the bring-up IR's fail-closed matrix (every decoder / CFG builder /
// verifier failure mode must abort with an error), hand-built canonical
// BC26 functions that must round-trip byte-for-byte through
// decode -> CFG -> verify -> emit_identity, and the mandatory identity
// gate over the real corpus: every module the compiler accepts must
// decode, verify, and re-emit identically (zero rejected functions).
//
// Part A drives ir::decode_function / build_cfg / verify_cfg /
// emit_identity directly on synthetic code blobs. Part B runs the
// bundle-level identity_round_trip on synthetic .qjsb bundles (the
// test_optimizer Builder layout, including the debug block). Part C
// compiles representative module sources through the real quickjs-ng
// compiler and runs cfg_identity_round_trip on each serialized bundle.
//
// Byte values are the serialized opcode space (quickjs-opcode.h
// physical order, temps excluded): push_i32=1, push_true=10, drop=14,
// dup=17, return=40, return_undef=41, get_loc=87, put_loc=88,
// if_false=104, if_true=105, goto=106, catch=107, gosub=108, ret=109,
// nip_catch=110, with_put_var=116, with_make_ref=118, add=156,
// nop=184, push_0=186, push_1=187, push_i8=194, push_i16=195,
// get_loc0=203, put_loc0=207, if_false8=240, if_true8=241, goto8=242,
// goto16=243, await=138, OP_ext=252, runtime-only get_field_ic=253,
// OP_COUNT=254. The two added opcodes remain invalid in BC26.

#include "bytecode_optimizer/bytecode_optimizer.h"
#include "bytecode_optimizer/ir/cfg.h"
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

ir::FuncInfo zero_fi(std::uint32_t code_len) {
    ir::FuncInfo fi;
    fi.code_off = 0;
    fi.code_len = code_len;
    fi.dbg_pc2line_off = 0;
    fi.dbg_pc2line_len = 0;
    fi.dbg_line = 0;
    fi.dbg_col = 0;
    fi.stack_size = 0;
    return fi;
}

// Decode + CFG + verify + emit, asserting byte-identical re-emission.
void check_identity_blob(const char* name,
                         const std::vector<std::uint8_t>& code,
                         std::uint32_t stack_size) {
    ir::FuncInfo fi = zero_fi(static_cast<std::uint32_t>(code.size()));
    std::vector<ir::Insn> insns;
    std::string err;
    CHECK(ir::decode_function(code.data(), code.size(), code.data(), fi,
                              &insns, &err));
    if (insns.empty()) return;
    ir::Cfg cfg;
    CHECK(ir::build_cfg(insns, &cfg, &err));
    cfg.recorded_stack_size = stack_size;
    CHECK(ir::verify_cfg(cfg, &err));
    std::vector<std::uint8_t> re;
    if (!ir::emit_identity(insns, code.data(), code.size(), &re, &err)) {
        std::fprintf(stderr, "FAIL: emit_identity (%s): %s\n", name,
                     err.c_str());
        g_failures++;
        return;
    }
    if (re != code) {
        std::fprintf(stderr,
                     "FAIL: re-emission diverged (%s): %zu vs %zu bytes\n",
                     name, re.size(), code.size());
        for (size_t i = 0; i < re.size() && i < code.size(); i++) {
            if (re[i] != code[i]) {
                std::fprintf(stderr, "  first diff at byte %zu: got %u want %u\n",
                             i, static_cast<unsigned>(re[i]),
                             static_cast<unsigned>(code[i]));
                break;
            }
        }
        g_failures++;
    }
}

// Decode must fail closed; `err` must be non-empty.
void check_decode_fails(const std::vector<std::uint8_t>& code) {
    ir::FuncInfo fi = zero_fi(static_cast<std::uint32_t>(code.size()));
    std::vector<ir::Insn> insns;
    std::string err;
    bool ok = ir::decode_function(code.data(), code.size(), code.data(), fi,
                                  &insns, &err);
    if (ok) {
        std::fprintf(stderr, "FAIL: decode accepted [");
        for (size_t i = 0; i < code.size(); i++) {
            std::fprintf(stderr, "%s%u", i ? " " : "",
                         static_cast<unsigned>(code[i]));
        }
        std::fprintf(stderr, "] -> %zu insns\n", insns.size());
        g_failures++;
        return;
    }
    if (err.empty()) {
        std::fprintf(stderr, "FAIL: decode failed with empty error\n");
        g_failures++;
    }
}

// Decode succeeds, but the CFG build or verify must fail closed.
void check_cfg_fails(const std::vector<std::uint8_t>& code,
                     std::uint32_t stack_size) {
    ir::FuncInfo fi = zero_fi(static_cast<std::uint32_t>(code.size()));
    std::vector<ir::Insn> insns;
    std::string err;
    if (!ir::decode_function(code.data(), code.size(), code.data(), fi,
                             &insns, &err)) {
        std::fprintf(stderr, "FAIL: decode unexpectedly failed: %s\n",
                     err.c_str());
        g_failures++;
        return;
    }
    if (insns.empty()) return;
    ir::Cfg cfg;
    bool built = ir::build_cfg(insns, &cfg, &err);
    if (!built) {
        if (err.empty()) {
            std::fprintf(stderr, "FAIL: build failed with empty error\n");
            g_failures++;
        }
        return;
    }
    cfg.recorded_stack_size = stack_size;
    if (ir::verify_cfg(cfg, &err)) {
        std::fprintf(stderr, "FAIL: cfg accepted a malformed function\n");
        g_failures++;
        return;
    }
    if (err.empty()) {
        std::fprintf(stderr, "FAIL: verify failed with empty error\n");
        g_failures++;
    }
}

// ---------------------------------------------------------------------------
// Part A: fail-closed matrix (synthetic code blobs).
// ---------------------------------------------------------------------------

void test_decode_failures() {
    // Runtime-only opcode 253 and opcode 0 are invalid on the wire.
    check_decode_fails({253});
    check_decode_fails({0});
    // Truncated instruction: push_i32 needs 5 bytes.
    check_decode_fails({1, 0, 0});
    // Jump out of range (forward past the blob end).
    check_decode_fails({106, 40, 0, 0, 0});
    // Jump before the blob start.
    check_decode_fails({106, 0xfe, 0xff, 0xff, 0xff});
    // Jump into the middle of an instruction: push_i8 at 0 (2 bytes),
    // goto at 2 with target 6; instruction starts are 0 and 2 only.
    check_decode_fails({194, 5, 106, 3, 0, 0, 0});
}

void test_cfg_failures() {
    // Control falls off the end of the blob (last insn is not a
    // terminator or jump).
    check_cfg_fails({186, 186}, 2);
}

void test_verify_failures() {
    // Stack underflow: drop at height 0.
    check_cfg_fails({14}, 1);
    // Height mismatch at a join: taken edge reaches T at 1, the
    // fallthrough path reaches T at 0.
    //   off 0: push_0; off 1: if_true8 -> 6; off 3: drop;
    //   off 4: goto8 -> 6; off 6: T: return_undef
    check_cfg_fails({186, 241, 4, 14, 242, 1, 41}, 1);
    // Dual entry: a backward goto into the entry block at height 1
    // collides with the entry's 0.
    //   off 0: push_0; off 1: goto8 -> 0
    check_cfg_fails({186, 242, 0xfe}, 1);
    // Recorded stack size too small for the verified max height.
    check_cfg_fails({186, 41}, 0);
}

void test_emit_failures() {
    // Identity lowering with a tampered old_code must fail closed on
    // the final byte comparison.
    ir::FuncInfo fi = zero_fi(2);
    std::vector<ir::Insn> insns;
    std::string err;
    const std::vector<std::uint8_t> good = {186, 41};
    CHECK(ir::decode_function(good.data(), good.size(), good.data(), fi,
                              &insns, &err));
    const std::vector<std::uint8_t> tampered = {186, 42};
    std::vector<std::uint8_t> re;
    CHECK(!ir::emit_identity(insns, tampered.data(), tampered.size(), &re,
                             &err));
    CHECK(!err.empty());
}

#ifdef CAPSID_ENABLE_EXT_FUSION34
void test_ext_operand_bounds() {
    // The independent CFG decoder must enforce the same tagged-frame-slot
    // contract as the strict bundle reader. This exercises the shared check
    // without letting the outer reader reject the malformed bundle first.
    const std::vector<std::uint8_t> code = {252, 2, 0, 1, 41};
    ir::FuncInfo fi = zero_fi(static_cast<std::uint32_t>(code.size()));
    fi.var_count = 2;
    std::vector<ir::Insn> insns;
    std::string err;
    CHECK(ir::decode_function(code.data(), code.size(), code.data(), fi,
                              &insns, &err, true));

    fi.var_count = 1;
    insns.clear();
    err.clear();
    CHECK(!ir::decode_function(code.data(), code.size(), code.data(), fi,
                               &insns, &err, true));
    CHECK(err.find("local slot operand out of range") != std::string::npos);

    const std::vector<std::uint8_t> arg_code = {252, 2, 0x80, 0x81, 41};
    fi = zero_fi(static_cast<std::uint32_t>(arg_code.size()));
    fi.arg_count = 1;
    insns.clear();
    err.clear();
    CHECK(!ir::decode_function(arg_code.data(), arg_code.size(),
                               arg_code.data(), fi, &insns, &err, true));
    CHECK(err.find("argument slot operand out of range") !=
          std::string::npos);
}
#endif

// ---------------------------------------------------------------------------
// Part A: canonical blobs with explicit edge-class assertions.
// ---------------------------------------------------------------------------

void test_edge_classes() {
    std::string err;

    // gosub/catch/finally: GOSUB edge to the ret block, CATCH edge to
    // the handler, FALLTHROUGH to the return point.
    //   off 0: push_0; off 1: gosub -> 13; off 6: catch -> 14;
    //   off 11: drop; off 12: return_undef; off 13: F: ret;
    //   off 14: C: nip_catch; off 15: return_undef
    const std::vector<std::uint8_t> b2 = {186, 108, 11, 0, 0, 0,
                                          107, 7,   0,  0, 0,
                                          14,  41,  109, 110, 41};
    check_identity_blob("b2", b2, 2);
    ir::FuncInfo fi = zero_fi(static_cast<std::uint32_t>(b2.size()));
    std::vector<ir::Insn> insns;
    CHECK(ir::decode_function(b2.data(), b2.size(), b2.data(), fi, &insns,
                              &err));
    ir::Cfg cfg;
    CHECK(ir::build_cfg(insns, &cfg, &err));
    CHECK(cfg.blocks.size() == 4);
    if (cfg.blocks.size() == 4) {
        // B0 = [push_0, gosub]: GOSUB -> B2 (finally), FALLTHROUGH -> B1.
        CHECK(cfg.blocks[0].edges.size() == 2);
        CHECK(cfg.blocks[0].edges[0].kind == ir::EdgeKind::GOSUB);
        CHECK(cfg.blocks[0].edges[0].to == 2);
        CHECK(!cfg.blocks[0].edges[0].backedge);
        CHECK(cfg.blocks[0].edges[1].kind == ir::EdgeKind::FALLTHROUGH);
        CHECK(cfg.blocks[0].edges[1].to == 1);
        // B1 = [catch, drop, return_undef]: CATCH -> B3.
        CHECK(cfg.blocks[1].edges.size() == 1);
        CHECK(cfg.blocks[1].edges[0].kind == ir::EdgeKind::CATCH);
        CHECK(cfg.blocks[1].edges[0].to == 3);
        // The finally block carries ret; the handler does not.
        CHECK(cfg.blocks[2].has_ret);
        CHECK(!cfg.blocks[3].has_ret);
    }

    // with_*: BARRIER edge to the scope-exit label.
    //   off 0: push_0; off 1: push_1; off 2: with_put_var (atom 0,
    //   label -> 15, u8 0); off 12: drop; off 13: goto8 -> 16;
    //   off 15: L: return_undef; off 16: E: return_undef
    const std::vector<std::uint8_t> b3 = {186, 187, 116, 0, 0, 0, 0,
                                          8,   0,   0,   0, 0, 14, 242,
                                          2,   41,  41};
    check_identity_blob("b3", b3, 2);
    fi = zero_fi(static_cast<std::uint32_t>(b3.size()));
    insns.clear();
    CHECK(ir::decode_function(b3.data(), b3.size(), b3.data(), fi, &insns,
                              &err));
    cfg = ir::Cfg();
    CHECK(ir::build_cfg(insns, &cfg, &err));
    CHECK(cfg.blocks.size() == 3);
    if (cfg.blocks.size() == 3) {
        CHECK(cfg.blocks[0].edges.size() == 2);
        CHECK(cfg.blocks[0].edges[0].kind == ir::EdgeKind::BARRIER);
        CHECK(cfg.blocks[0].edges[0].to == 1);
        CHECK(cfg.blocks[0].edges[1].kind == ir::EdgeKind::JUMP);
        CHECK(cfg.blocks[0].edges[1].to == 2);
    }

    // SUSPEND edge: await's cross-block fallthrough. The dead goto8
    // (after the return_undef) marks off 2 as a leader, so the await's
    // fallthrough crosses the block boundary into B1.
    //   off 0: push_0; off 1: await; off 2: return_undef (leader);
    //   off 3: goto8 -> 2 (dead jump: only marks off 2 as a leader);
    //   off 5: push_0; off 6: return_undef (dead)
    const std::vector<std::uint8_t> b4 = {186, 138, 41, 242, 0xfe,
                                          186, 41};
    check_identity_blob("b4", b4, 1);
    fi = zero_fi(static_cast<std::uint32_t>(b4.size()));
    insns.clear();
    CHECK(ir::decode_function(b4.data(), b4.size(), b4.data(), fi, &insns,
                              &err));
    cfg = ir::Cfg();
    CHECK(ir::build_cfg(insns, &cfg, &err));
    CHECK(cfg.blocks.size() == 2);
    cfg.recorded_stack_size = 1;
    CHECK(ir::verify_cfg(cfg, &err));
    if (cfg.blocks.size() == 2) {
        CHECK(cfg.blocks[0].edges.size() == 1);
        CHECK(cfg.blocks[0].edges[0].kind == ir::EdgeKind::SUSPEND);
        CHECK(cfg.blocks[0].edges[0].to == 1);
        CHECK(!cfg.blocks[0].edges[0].backedge);
        // Dead code after the return stays unvisited: B1's linear flow
        // stops at its terminator, so the trailing push/return never
        // height-checks.
        CHECK(cfg.blocks[1].reachable);
    }

    // Loop backedge flag: a self-loop if_true8 edge is a backedge; the
    // entry's fallthrough is not.
    //   off 0: push_0; off 1: L: dup; off 2: if_true8 -> 1;
    //   off 4: drop; off 5: return_undef
    const std::vector<std::uint8_t> b5 = {186, 17, 241, 0xfe, 14, 41};
    check_identity_blob("b5", b5, 2);
    fi = zero_fi(static_cast<std::uint32_t>(b5.size()));
    insns.clear();
    CHECK(ir::decode_function(b5.data(), b5.size(), b5.data(), fi, &insns,
                              &err));
    cfg = ir::Cfg();
    CHECK(ir::build_cfg(insns, &cfg, &err));
    CHECK(cfg.blocks.size() == 2);
    if (cfg.blocks.size() == 2) {
        CHECK(cfg.blocks[0].edges.size() == 1);
        CHECK(cfg.blocks[0].edges[0].kind == ir::EdgeKind::FALLTHROUGH);
        CHECK(!cfg.blocks[0].edges[0].backedge);
        CHECK(cfg.blocks[1].edges.size() == 1);
        CHECK(cfg.blocks[1].edges[0].kind == ir::EdgeKind::JUMP);
        CHECK(cfg.blocks[1].edges[0].to == 1);
        CHECK(cfg.blocks[1].edges[0].backedge);
    }
}

void test_identity_blobs() {
    // Branching across all jump widths with mixed operand classes.
    //   off 0: push_0; 1: dup; 2: put_loc0; 3: push_i8 7;
    //   5: push_i16 300; 8: push_i32 -100000; 13: get_loc0; 14: add;
    //   15: if_false -> 23; 20: drop; 21: goto8 -> 24;
    //   23: T: drop; 24: E: return_undef
    const std::vector<std::uint8_t> b1 = {
        186, 17, 207, 194, 7, 195, 0x2c, 0x01, 1, 0x60, 0x79, 0xfe, 0xff,
        203, 156, 104, 7, 0, 0, 0, 14, 242, 2, 14, 41};
    check_identity_blob("b1", b1, 5);
}

// ---------------------------------------------------------------------------
// Part B: bundle-level identity (synthetic .qjsb, mirrors the
// test_optimizer Builder layout).
// ---------------------------------------------------------------------------

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

void test_identity_bundles() {
    std::string err;
    ir::IdentityReport rep;

    // Canonical bundle with a valid debug block: full gate passes with
    // zero rejections; both entry points agree.
    std::vector<std::uint8_t> dbg = make_debug({2, 0});
    std::vector<std::uint8_t> good =
        make_bundle({186, 41}, 1, &dbg);
    CHECK(ir::identity_round_trip(good.data(), good.size(), &rep, &err));
    CHECK(rep.functions == 1);
    CHECK(rep.insns == 2);
    CHECK(rep.rejected_functions == 0);
    CHECK(rep.rejected_insns == 0);
    CHECK(rep.missing_pc2line == 0);
    CHECK(capsid::bytecode::cfg_identity_round_trip(good, &err));

    // Malformed pc2line (truncated sleb): the function cannot decode,
    // so it is counted as rejected coverage — never a silent skip.
    std::vector<std::uint8_t> bad_dbg = make_debug({2, 0x80});
    std::vector<std::uint8_t> bad =
        make_bundle({186, 41}, 1, &bad_dbg);
    CHECK(ir::identity_round_trip(bad.data(), bad.size(), &rep, &err));
    CHECK(rep.rejected_functions == 1);

    // pc2line entry beyond the code blob: a decodable function whose
    // debug table is out of bounds fails the gate.
    std::vector<std::uint8_t> far_dbg = make_debug({0, 0x64, 0, 0});
    std::vector<std::uint8_t> far =
        make_bundle({186, 41}, 1, &far_dbg);
    CHECK(!ir::identity_round_trip(far.data(), far.size(), &rep, &err));
    CHECK(!err.empty());
}

// ---------------------------------------------------------------------------
// Part C: the mandatory identity gate over the real corpus.
// ---------------------------------------------------------------------------

void test_corpus_identity() {
    JSRuntime* rt = JS_NewRuntime();
    CHECK(rt != nullptr);
    JSContext* ctx = JS_NewContext(rt);
    CHECK(ctx != nullptr);
    if (ctx == nullptr) return;

    const char* sources[] = {
        // Arithmetic + conditional.
        "globalThis.__r = (1 + 2) * (3 + 4);",
        // Counter loop (backedges, loc/arg slots, short jumps).
        "let acc = 0; for (let i = 0; i < 1000; i++) acc += i; "
        "globalThis.__r = acc;",
        // Recursion with a ternary (call forms, tail call in some
        // compilations of the base case).
        "function fib(n) { return n < 2 ? n : fib(n-1) + fib(n-2); } "
        "globalThis.__r = fib(10);",
        // try/catch/finally (gosub/ret, catch edges, nip_catch).
        "let r = 0; try { r = 1; throw new Error('x'); } catch (e) { "
        "r = 2; } finally { r += 10; } globalThis.__r = r;",
        // Generator (initial_yield, yield).
        "function* g() { yield 1; yield 2; } let s = 0; "
        "for (const x of g()) s += x; globalThis.__r = s;",
        // async/await (await).
        "async function f() { const p = Promise.resolve(3); "
        "return await p + 1; } globalThis.__r = typeof f();",
        // for-await-of (async_yield_star, await, iterator protocol).
        "async function f() { const g = { async *[Symbol.asyncIterator]() "
        "{ yield 1; yield 2; } }; let s = 0; for await (const x of g) "
        "s += x; return s; } globalThis.__r = typeof f();",
        // Class with inheritance (check_ctor, init_ctor, brand ops).
        "class A { constructor() { this.v = 3; } m() { return this.v * 2; }"
        "} class B extends A { constructor() { super(); } }"
        "globalThis.__r = new B().m();",
        // Direct eval (OP_eval, npop_u16 fold).
        "globalThis.__r = eval('1 + 2') * 3;",
        // Dynamic import (OP_import).
        "globalThis.__r = typeof import('x').then;",
        // Switch with fallthrough paths (multiple branch edges).
        "function f(x) { switch (x) { case 1: return 'a'; "
        "case 2: return 'b'; default: return 'c'; } }"
        "globalThis.__r = f(2);",
        // Closures with captured slots (var_ref family).
        "function outer() { let x = 1; return () => ++x; }"
        "const inc = outer(); inc(); globalThis.__r = inc();",
        // Destructuring in for-of (array_from, iterator ops).
        "let s = 0; for (const [a, b] of [[1, 2], [3, 4]]) s += a + b; "
        "globalThis.__r = s;",
        // for-in over an object.
        "const o = { a: 1, b: 2 }; let s = 0; "
        "for (const k in o) s += o[k]; globalThis.__r = s;",
        // Optional chaining + nullish coalescing.
        "const o = { x: { y: 5 } }; globalThis.__r = o.x?.y ?? 0;",
        // Template literals (tagged and untagged).
        "function tag(s, ...v) { return v.length; }"
        "globalThis.__r = tag`a${1}b${2}c` + `${1 + 1}-${'x'.repeat(2)}`"
        ".length;",
        // BigInt materialization.
        "globalThis.__r = Number(1n + 2n);",
        // Spread call + array literals.
        "function f(a, b, c) { return a + b + c; }"
        "const a = [1, 2, 3]; const b = [...a, 4];"
        "globalThis.__r = f(...b) + b.length;",
        // Explicit resource management (using_dispose family).
        "using x = { [Symbol.dispose]() {} };"
        "let s = 0; using y = { [Symbol.dispose]() { s = 1; } };"
        "globalThis.__r = s;",
        // Labeled loops with continue (threaded branches).
        "let n = 0; outer: for (let i = 0; i < 3; i++) { "
        "for (let j = 0; j < 3; j++) { if (j === 1) continue outer; "
        "n++; } } globalThis.__r = n;",
        // Getters/setters + property definition machinery.
        "const o = { get x() { return 1; }, set x(v) {} };"
        "o.x = 2; globalThis.__r = o.x;",
        // Optional catch binding + rethrow.
        "let r = 0; try { throw 1; } catch { r = 1; }"
        "globalThis.__r = r;",
        // Arrow with this capture.
        "const o = { v: 3, m() { return (() => this.v)(); } };"
        "globalThis.__r = o.m();",
        // RegExp materialization.
        "globalThis.__r = /ab+c/.test('xabbc') ? 1 : 0;",
        // Destructuring defaults + rest.
        "const { a = 5, b = 6 } = { a: 1 };"
        "const [p, ...qs] = [1, 2, 3]; globalThis.__r = a + b + qs.length;",
        // Object/array property access with computed keys.
        "const k = 'n'; const o = { n: 42 };"
        "globalThis.__r = o[k] + [1, 2, 3][1];",
    };
    for (const char* src : sources) {
        JSValue module = JS_Eval(ctx, src, std::strlen(src), "cfg.js",
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
        ir::IdentityReport rep;
        if (!ir::identity_round_trip(buf.data(), buf.size(), &rep, &err)) {
            std::fprintf(stderr, "FAIL: identity gate failed for: %s\n  %s\n",
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
        CHECK(capsid::bytecode::cfg_identity_round_trip(buf, &err));
        if (g_failures > 5) break;  // don't flood past the first few
    }
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
}

}  // namespace

int main() {
    test_decode_failures();
    test_cfg_failures();
    test_verify_failures();
    test_emit_failures();
#ifdef CAPSID_ENABLE_EXT_FUSION34
    test_ext_operand_bounds();
#endif
    test_edge_classes();
    test_identity_blobs();
    test_identity_bundles();
    test_corpus_identity();
    if (g_failures != 0) {
        std::fprintf(stderr, "test_bytecode_cfg: %d failure(s)\n",
                     g_failures);
        return 1;
    }
    std::fprintf(stderr, "test_bytecode_cfg: all checks passed\n");
    return 0;
}
