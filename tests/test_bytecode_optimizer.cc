// Step 7 unit gate for the bytecode AOT optimizer
// (docs/bytecode-aot-optimizer.md). Part A drives optimize() on
// synthetic .qjsb buffers: per-peephole golden bytes, the P2 lattice
// with its dynamic-scope gates, dead-block handling (catch targets stay
// live), cpool skipping, pc2line remapping, the fail-closed matrix,
// determinism, and checksum validity — no runtime needed. Part B runs
// the real pipeline (JS_Eval COMPILE_ONLY → JS_WriteObject → optimize →
// JS_ReadObject → JS_EvalFunction) through tjs and compares return
// values, exceptions, and stack-trace line numbers against the
// unoptimized path.
//
// Byte values below are the serialized opcode space (quickjs-opcode.h
// physical order, temps excluded): push_i32=1, push_const=2, push_true=10,
// drop=14, dup=17, swap=27, return=40, return_undef=41, eval=50,
// get_loc=87, put_loc=88, if_true=105, goto=106, catch=107, gosub=108,
// with_get_var=115, add=156, mul=153, sub=157, strict_eq=173,
// strict_neq=174; short forms push_1=187, push_2=188, push_3=189,
// push_i8=194, push_i16=195, get_loc0=203, put_loc0=207.
//
// Note: `with` cannot appear in module sources (modules are always strict
// mode), so the with/eval P2 gates are exercised with synthetic bytes in
// Part A; Part B covers eval through the real compiler.

#include "bytecode_optimize.h"
#include "quickjs.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
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

// ---------------------------------------------------------------------------
// Synthetic .qjsb builder (mirrors JS_WriteObject layout for a module
// with one top-level function; optional vardefs, cpool items, and debug
// block).
// ---------------------------------------------------------------------------

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

// Independent re-implementation of quickjs.c bc_csum (37836-37860):
// h += u32le; h *= 0x9e370001 per 4-byte word, tail folded with one
// final multiply.
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

struct Builder {
    std::vector<std::uint8_t> buf;
    std::vector<std::uint8_t> code;
    std::vector<std::uint8_t> cpool;
    std::uint32_t stack_size = 0;
    std::uint32_t cpool_count = 0;

    void op(std::uint8_t b) { code.push_back(b); }
    void op_i32(std::uint8_t opcode, std::int32_t v) {
        code.push_back(opcode);
        put_u32(&code, static_cast<std::uint32_t>(v));
    }
    void op_u16(std::uint8_t opcode, std::uint16_t v) {
        code.push_back(opcode);
        put_u16(&code, v);
    }
    // with_* family: atom u32 + label offset u32 + is_with u8 (the
    // interpreter reads `atom = get_u32(pc); diff = get_u32(pc+4);
    // is_with = pc[8]; pc += 9`; target = operand_start + diff).
    void op_with(std::uint8_t opcode, std::uint32_t atom,
                 std::uint32_t diff, std::uint8_t is_with) {
        code.push_back(opcode);
        put_u32(&code, atom);
        put_u32(&code, diff);
        code.push_back(is_with);
    }

    // debug block: filename atom 0, line/col 1, single pc2line entry
    // (pc_delta 0, line_delta 0, col_delta 0 -> short byte 2, col sleb 0),
    // empty source.
    static std::vector<std::uint8_t> debug_block() {
        std::vector<std::uint8_t> d;
        put_leb(&d, 0);  // filename atom
        put_leb(&d, 1);  // line
        put_leb(&d, 1);  // col
        put_leb(&d, 2);  // pc2line_len
        d.push_back(2);  // (0,0) short entry
        d.push_back(0);  // col_delta sleb128
        put_leb(&d, 0);  // source_len
        return d;
    }

    void finish(int var_count, std::uint16_t flags = 0,
                const std::vector<std::uint8_t>* debug = nullptr) {
        buf.push_back(26);  // BC_VERSION
        put_u32(&buf, 0);   // checksum, patched below
        put_leb(&buf, 1);   // atom count
        buf.push_back(0);   // atom 0: const 0
        put_u32(&buf, 0);
        buf.push_back(13);  // BC_TAG_MODULE
        put_leb(&buf, 0);   // module name atom
        put_leb(&buf, 0);   // req / export / star / import counts
        put_leb(&buf, 0);
        put_leb(&buf, 0);
        put_leb(&buf, 0);
        buf.push_back(0);   // has_tla
        buf.push_back(12);  // BC_TAG_FUNCTION_BYTECODE
        put_u16(&buf, flags);
        buf.push_back(1);   // strict
        put_leb(&buf, 0);   // function name atom
        put_leb(&buf, 0);   // arg_count
        put_leb(&buf, static_cast<std::uint32_t>(var_count));
        put_leb(&buf, 0);   // defined_arg_count
        put_leb(&buf, stack_size);
        put_leb(&buf, 0);   // var_ref_count
        put_leb(&buf, 0);   // closure_var_count
        put_leb(&buf, cpool_count);
        put_leb(&buf, static_cast<std::uint32_t>(code.size()));
        put_leb(&buf, static_cast<std::uint32_t>(var_count));
        for (int i = 0; i < var_count; i++) {
            put_leb(&buf, 0);  // atom, scope_level, scope_next, flags
            put_leb(&buf, 0);
            put_leb(&buf, 0);
            buf.push_back(0);
        }
        buf.insert(buf.end(), cpool.begin(), cpool.end());
        buf.insert(buf.end(), code.begin(), code.end());
        if (debug != nullptr) {
            buf.insert(buf.end(), debug->begin(), debug->end());
        }
        std::uint32_t c = bc_csum(buf.data() + 5, buf.size() - 5);
        buf[1] = static_cast<std::uint8_t>(c);
        buf[2] = static_cast<std::uint8_t>(c >> 8);
        buf[3] = static_cast<std::uint8_t>(c >> 16);
        buf[4] = static_cast<std::uint8_t>(c >> 24);
    }

    bool optimize_and_code(std::vector<std::uint8_t>* out_code,
                           std::string* err, std::uint32_t passes) {
        std::vector<std::uint8_t> out;
        if (!capsid::bytecode::optimize(buf, &out, passes, false, err)) {
            return false;
        }
        CHECK(out.size() > 5);
        // Header checksum must match an independent recompute.
        std::uint32_t stored = static_cast<std::uint32_t>(out[1]) |
                               (static_cast<std::uint32_t>(out[2]) << 8) |
                               (static_cast<std::uint32_t>(out[3]) << 16) |
                               (static_cast<std::uint32_t>(out[4]) << 24);
        CHECK(stored == bc_csum(out.data() + 5, out.size() - 5));
        // Parse the function record to the code blob (layout fixed for
        // this builder: atom 0, no closure vars, cpool allowed but all
        // tests here use none).
        std::size_t p = 5;
        while (out[p++] & 0x80) { /* atom count leb128 */ }
        p += 1 + 4;  // atom 0 const
        p += 1 + 1;  // module tag + name atom
        for (int k = 0; k < 4; k++) {
            while (out[p++] & 0x80) { /* counts */ }
        }
        p += 1;  // has_tla
        p += 1 + 2 + 1;  // fn tag + flags + strict
        while (out[p++] & 0x80) { /* name atom */ }
        std::uint32_t vals[8] = {0};
        for (int f = 0; f < 8; f++) {
            std::uint32_t x = 0;
            int shift = 0;
            for (;;) {
                x |= static_cast<std::uint32_t>(out[p] & 0x7f) << shift;
                if (!(out[p++] & 0x80)) break;
                shift += 7;
            }
            vals[f] = x;
        }
        std::uint32_t vc = vals[1];
        while (out[p++] & 0x80) { /* vardefs count */ }
        for (std::uint32_t i = 0; i < vc; i++) {
            for (int k = 0; k < 3; k++) {
                while (out[p++] & 0x80) { /* vardef fields */ }
            }
            p += 1;
        }
        out_code->assign(out.begin() + static_cast<std::ptrdiff_t>(p),
                         out.end());
        return true;
    }
};

void expect_code(const char* name, Builder* b, const char* hex) {
    std::vector<std::uint8_t> code;
    std::string err;
    if (!b->optimize_and_code(&code, &err, 0xffffffffu)) {
        std::fprintf(stderr, "FAIL %s: optimize error: %s\n", name,
                     err.c_str());
        g_failures++;
        return;
    }
    // Expected bytes are space-separated hex.
    std::vector<std::uint8_t> want;
    const char* cur = hex;
    while (*cur) {
        while (*cur == ' ') cur++;
        if (*cur == '\0') break;
        std::uint8_t v = 0;
        while (*cur && *cur != ' ') {
            v = static_cast<std::uint8_t>(
                v * 16 + (*cur >= 'a' ? *cur - 'a' + 10 : *cur - '0'));
            cur++;
        }
        want.push_back(v);
    }
    if (code != want) {
        std::fprintf(stderr, "FAIL %s: code =", name);
        for (std::uint8_t x : code) std::fprintf(stderr, " %02x", x);
        std::fprintf(stderr, " want");
        for (std::uint8_t x : want) std::fprintf(stderr, " %02x", x);
        std::fprintf(stderr, "\n");
        g_failures++;
    }
}

// ---------------------------------------------------------------------------
// Part A: synthetic-buffer unit tests.
// ---------------------------------------------------------------------------

void test_peephole_goldens() {
    // P3.1: 1 + 2 -> push_3 return
    {
        Builder b;
        b.op_i32(1, 1);
        b.op_i32(1, 2);
        b.op(156);
        b.op(40);
        b.stack_size = 2;
        b.finish(0);
        expect_code("p31 add 1+2", &b, "bd 28");
    }
    // P3.1: 100 * 7 -> push_i16(700)
    {
        Builder b;
        b.op_i32(1, 100);
        b.op_i32(1, 7);
        b.op(153);
        b.op(40);
        b.stack_size = 2;
        b.finish(0);
        expect_code("p31 mul 100*7", &b, "c3 bc 02 28");
    }
    // P3.1: 3 - 10 -> push_i8(-7)
    {
        Builder b;
        b.op_i32(1, 3);
        b.op_i32(1, 10);
        b.op(157);
        b.op(40);
        b.stack_size = 2;
        b.finish(0);
        expect_code("p31 sub 3-10", &b, "c2 f9 28");
    }
    // P3.2: 5 === 5 -> push_true; 5 !== 5 -> push_false
    {
        Builder b;
        b.op_i32(1, 5);
        b.op_i32(1, 5);
        b.op(173);
        b.op(40);
        b.stack_size = 2;
        b.finish(0);
        expect_code("p32 strict_eq", &b, "0a 28");
    }
    {
        Builder b;
        b.op_i32(1, 5);
        b.op_i32(1, 5);
        b.op(174);
        b.op(40);
        b.stack_size = 2;
        b.finish(0);
        expect_code("p32 strict_neq", &b, "09 28");
    }
    // P3.6: constant conditional jumps -> the taken branch only.
    {
        Builder b;
        b.op(10);  // push_true
        b.op_i32(105, 5);
        b.op(41);
        b.op(41);
        b.stack_size = 1;
        b.finish(0);
        expect_code("p36 true branch", &b, "29");
    }
    {
        Builder b;
        b.op(9);  // push_false
        b.op_i32(104, 5);
        b.op(41);
        b.op(41);
        b.stack_size = 1;
        b.finish(0);
        expect_code("p36 false branch", &b, "29");
    }
    // P3.4: push + drop; P3.5: dup drop / swap swap.
    {
        Builder b;
        b.op_i32(1, 1);
        b.op(14);
        b.op(41);
        b.stack_size = 1;
        b.finish(0);
        expect_code("p34 push+drop", &b, "29");
    }
    {
        Builder b;
        b.op_i32(1, 1);
        b.op(17);  // dup
        b.op(14);  // drop
        b.op(41);
        b.stack_size = 2;
        b.finish(0);
        expect_code("p35 dup drop", &b, "bb 29");
    }
    {
        Builder b;
        b.op_i32(1, 1);
        b.op_i32(1, 2);
        b.op(27);  // swap
        b.op(27);
        b.op(41);
        b.stack_size = 2;
        b.finish(0);
        expect_code("p35 swap swap", &b, "bb bc 29");
    }
    // P4: goto chain -> threaded to the tail.
    {
        Builder b;
        b.op_i32(106, 9);
        b.op_i32(106, 4);
        b.op(41);
        b.stack_size = 0;
        b.finish(0);
        expect_code("p4 goto chain", &b, "29");
    }
    // P5: dead block after an unconditional jump.
    {
        Builder b;
        b.op_i32(106, 10);
        b.op_i32(1, 1);
        b.op(14);
        b.op(41);
        b.stack_size = 1;
        b.finish(0);
        expect_code("p5 dead block", &b, "29");
    }
    // P2 + P3.1 + P6: x = 1; x + 1 with one local slot.
    {
        Builder b;
        b.op_i32(1, 1);
        b.op_u16(88, 0);  // put_loc 0
        b.op_u16(87, 0);  // get_loc 0
        b.op_i32(1, 1);
        b.op(156);  // add
        b.op(40);
        b.stack_size = 2;
        b.finish(1);
        expect_code("p2 crossbb x+1", &b, "bb cf bc 28");
    }
}

void test_fail_closed_matrix() {
    // version mismatch
    {
        Builder b;
        b.op_i32(1, 1);
        b.op(41);
        b.stack_size = 1;
        b.finish(0);
        b.buf[0] = 25;
        std::vector<std::uint8_t> out;
        std::string err;
        CHECK(!capsid::bytecode::optimize(b.buf, &out, 0xffffffffu, false,
                                          &err));
    }
    // checksum mismatch (flip one byte inside the checksummed range)
    {
        Builder b;
        b.op_i32(1, 1);
        b.op(41);
        b.stack_size = 1;
        b.finish(0);
        b.buf[20] ^= 0x40;
        std::vector<std::uint8_t> out;
        std::string err;
        CHECK(!capsid::bytecode::optimize(b.buf, &out, 0xffffffffu, false,
                                          &err));
        CHECK(err.find("checksum") != std::string::npos);
    }
    // trailing garbage after the module record
    {
        Builder b;
        b.op_i32(1, 1);
        b.op(41);
        b.stack_size = 1;
        b.finish(0);
        b.buf.push_back(0xaa);
        std::uint32_t c = bc_csum(b.buf.data() + 5, b.buf.size() - 5);
        b.buf[1] = static_cast<std::uint8_t>(c);
        b.buf[2] = static_cast<std::uint8_t>(c >> 8);
        b.buf[3] = static_cast<std::uint8_t>(c >> 16);
        b.buf[4] = static_cast<std::uint8_t>(c >> 24);
        std::vector<std::uint8_t> out;
        std::string err;
        CHECK(!capsid::bytecode::optimize(b.buf, &out, 0xffffffffu, false,
                                          &err));
        CHECK(err.find("trailing") != std::string::npos);
    }
    // every truncation of a valid buffer must fail closed (the header
    // checksum never matches a truncated range)
    {
        Builder b;
        b.op_i32(1, 1);
        b.op(41);
        b.stack_size = 1;
        b.finish(0);
        for (std::size_t n = 0; n < b.buf.size(); n++) {
            std::vector<std::uint8_t> t(b.buf.begin(),
                                        b.buf.begin() + n);
            std::vector<std::uint8_t> out;
            std::string err;
            CHECK(!capsid::bytecode::optimize(t, &out, 0xffffffffu, false,
                                              &err));
        }
    }
    // invalid opcodes inside the code blob
    {
        Builder b;
        b.op(0);
        b.op(41);
        b.stack_size = 1;
        b.finish(0);
        std::vector<std::uint8_t> out;
        std::string err;
        CHECK(!capsid::bytecode::optimize(b.buf, &out, 0xffffffffu, false,
                                          &err));
    }
    {
        Builder b;
        b.op(252);
        b.op(41);
        b.stack_size = 1;
        b.finish(0);
        std::vector<std::uint8_t> out;
        std::string err;
        CHECK(!capsid::bytecode::optimize(b.buf, &out, 0xffffffffu, false,
                                          &err));
    }
    // oversized leb128 on the atom-count field (6 continuation bytes)
    {
        Builder b;
        b.op_i32(1, 1);
        b.op(41);
        b.stack_size = 1;
        b.finish(0);
        b.buf[5] = 0x80;
        b.buf.insert(b.buf.begin() + 6, 5, 0x80);
        std::uint32_t c = bc_csum(b.buf.data() + 5, b.buf.size() - 5);
        b.buf[1] = static_cast<std::uint8_t>(c);
        b.buf[2] = static_cast<std::uint8_t>(c >> 8);
        b.buf[3] = static_cast<std::uint8_t>(c >> 16);
        b.buf[4] = static_cast<std::uint8_t>(c >> 24);
        std::vector<std::uint8_t> out;
        std::string err;
        CHECK(!capsid::bytecode::optimize(b.buf, &out, 0xffffffffu, false,
                                          &err));
    }
    // unknown cpool tag -> fail closed
    {
        Builder b;
        b.op_i32(1, 1);
        b.op(41);
        b.stack_size = 1;
        b.cpool_count = 1;
        b.cpool.push_back(0xff);
        b.finish(0);
        std::vector<std::uint8_t> out;
        std::string err;
        CHECK(!capsid::bytecode::optimize(b.buf, &out, 0xffffffffu, false,
                                          &err));
    }
    // empty buffer
    {
        std::vector<std::uint8_t> out;
        std::string err;
        CHECK(!capsid::bytecode::optimize(std::vector<std::uint8_t>(), &out,
                                          0xffffffffu, false, &err));
    }
}

void test_cpool_kept() {
    // A valid INT32 cpool item must be skipped by the reader and copied
    // through verbatim.
    Builder b;
    b.op_i32(1, 1);
    b.op(41);
    b.stack_size = 1;
    b.cpool_count = 1;
    b.cpool.push_back(5);   // BC_TAG_INT32
    b.cpool.push_back(42);  // sleb128(42)
    b.finish(0);
    std::vector<std::uint8_t> out;
    std::string err;
    CHECK(capsid::bytecode::optimize(b.buf, &out, 0xffffffffu, false,
                                     &err));
    bool found = false;
    for (std::size_t i = 0; i + 1 < out.size(); i++) {
        if (out[i] == 5 && out[i + 1] == 42) found = true;
    }
    CHECK(found);
}

void test_p2_gates() {
    // The same x=1; x+1 shape, but with a with_get_var anywhere in the
    // function: the whole P2 pass must be disabled, so the get_loc
    // survives (shrunk to get_loc0, 0xcb).
    // Layout: push_i32(1) @0 (5B); put_loc 0 @5 (3B); push_i32(1) @8
    // (5B, the with object); with_get_var @13 (10B; its label base is
    // insn+5 = 18 and target = base + diff); get_loc 0 @23 (3B);
    // push_i32(1) @26 (5B); add @31; return @32; return_undef @33.
    // diff -> return_undef @33: 33-18=15. The found edge carries the
    // value (h=1) and terminates; the fallthrough (no_with) drops the
    // object and flows into get_loc.
    {
        Builder b;
        b.op_i32(1, 1);
        b.op_u16(88, 0);  // put_loc 0
        b.op_i32(1, 1);   // the with object
        b.op_with(115, 0, 15, 1);  // with_get_var atom 0, diff 15, with
        b.op_u16(87, 0);  // get_loc 0
        b.op_i32(1, 1);
        b.op(156);
        b.op(40);
        b.op(41);
        b.stack_size = 3;
        b.finish(1);
        std::vector<std::uint8_t> code;
        std::string err;
        CHECK(b.optimize_and_code(&code, &err, 0xffffffffu));
        bool has_getloc = false;
        for (std::uint8_t x : code) {
            if (x == 0xcb) has_getloc = true;  // get_loc0
        }
        CHECK(has_getloc);
    }
    // Same with OP_eval (byte 50): pass disabled, get_loc survives.
    // eval is 5 bytes: op + argc u16 + scope_idx u16 (interpreter reads
    // call_argc at +1, scope_idx at +3). It pops the callee (+ argc
    // args) and pushes the result; stack max is 3 here.
    // Layout: push_i32(1) @0; put_loc 0 @5; push_i32(1) @8 (callee);
    // eval @13 (5B); get_loc 0 @18; push_i32(1) @21; add @26; return
    // @27.
    {
        Builder b;
        b.op_i32(1, 1);
        b.op_u16(88, 0);
        b.op_i32(1, 1);  // callee
        b.op(50);        // eval
        put_u16(&b.code, 0);  // argc 0
        put_u16(&b.code, 0);  // scope_idx 0
        b.op_u16(87, 0);
        b.op_i32(1, 1);
        b.op(156);
        b.op(40);
        b.stack_size = 3;
        b.finish(1);
        std::vector<std::uint8_t> code;
        std::string err;
        CHECK(b.optimize_and_code(&code, &err, 0xffffffffu));
        bool has_getloc = false;
        for (std::uint8_t x : code) {
            if (x == 0xcb) has_getloc = true;
        }
        CHECK(has_getloc);
    }
    // And the ungated control: the same shape without with/eval must
    // propagate (get_loc replaced by a constant, no get_loc0 left).
    {
        Builder b;
        b.op_i32(1, 1);
        b.op_u16(88, 0);
        b.op_u16(87, 0);
        b.op_i32(1, 1);
        b.op(156);
        b.op(40);
        b.stack_size = 2;
        b.finish(1);
        std::vector<std::uint8_t> code;
        std::string err;
        CHECK(b.optimize_and_code(&code, &err, 0xffffffffu));
        bool has_getloc = false;
        for (std::uint8_t x : code) {
            if (x == 0xcb) has_getloc = true;
        }
        CHECK(!has_getloc);
    }
}

void test_dead_block_catch_target() {
    // goto M skips a block whose catch label points at the surviving
    // return_undef; the catch opcode itself is unreachable and must be
    // removed, but its target (a static root) must stay.
    // Layout: push_i32(1) @0 (5B); goto M @5 (operand start 6, M=16,
    //         offset 10); catch L @10 (operand start 11, L=15, offset 4);
    //         return_undef @15; M: return_undef @16.
    Builder b;
    b.op_i32(1, 1);
    b.op_i32(106, 10);  // goto M
    b.op_i32(107, 4);   // catch L
    b.op(41);           // return_undef @15
    b.op(41);           // M: return_undef @16
    b.stack_size = 1;
    b.finish(0);
    std::vector<std::uint8_t> code;
    std::string err;
    CHECK(b.optimize_and_code(&code, &err, 0xffffffffu));
    bool has_catch = false;
    for (std::uint8_t x : code) {
        if (x == 107) has_catch = true;
    }
    CHECK(!has_catch);
    bool has_ret = false;
    for (std::uint8_t x : code) {
        if (x == 41) has_ret = true;
    }
    CHECK(has_ret);
}

void test_debug_block_remap() {
    // Function with a debug block: the optimizer must rewrite the code
    // blob and keep the debug block structurally valid (the optimizer's
    // own self-check re-parses the whole output, so success is the
    // assertion). The pc2line blob shrinks when instructions are removed.
    Builder b;
    b.op_i32(1, 1);
    b.op_i32(1, 2);
    b.op(156);  // add
    b.op(40);   // return
    b.stack_size = 2;
    std::vector<std::uint8_t> dbg = Builder::debug_block();
    b.finish(0, 0x800, &dbg);  // bit 11 = allow_debug
    std::vector<std::uint8_t> out;
    std::string err;
    CHECK(capsid::bytecode::optimize(b.buf, &out, 0xffffffffu, false,
                                     &err));
    std::uint32_t stored = static_cast<std::uint32_t>(out[1]) |
                           (static_cast<std::uint32_t>(out[2]) << 8) |
                           (static_cast<std::uint32_t>(out[3]) << 16) |
                           (static_cast<std::uint32_t>(out[4]) << 24);
    CHECK(stored == bc_csum(out.data() + 5, out.size() - 5));
}

void test_determinism_and_idempotence() {
    // x = 1; x + 1 twice plus a conditional, run twice: identical
    // output; optimizing the output again is a no-op.
    Builder b;
    b.op_i32(1, 1);
    b.op_u16(88, 0);
    b.op(10);          // push_true
    b.op_i32(105, 4);  // if_true -> skip the first get_loc
    b.op_u16(87, 0);
    b.op_u16(87, 0);
    b.op_i32(1, 1);
    b.op(156);
    b.op(40);
    b.stack_size = 3;
    b.finish(1);
    std::vector<std::uint8_t> o1, o2, o3;
    std::string e1, e2, e3;
    CHECK(capsid::bytecode::optimize(b.buf, &o1, 0xffffffffu, false, &e1));
    CHECK(capsid::bytecode::optimize(b.buf, &o2, 0xffffffffu, false, &e2));
    CHECK(o1 == o2);
    CHECK(capsid::bytecode::optimize(o1, &o3, 0xffffffffu, false, &e3));
    CHECK(o1 == o3);
}

// ---------------------------------------------------------------------------
// Part B: full round-trip through the real runtime.
// ---------------------------------------------------------------------------

struct RtResult {
    bool ok;
    std::string value;
    std::string stack;
};

// Compile + serialize + optimize + deserialize + eval. When `optimize` is
// false the middle step is skipped (unoptimized control path).
RtResult run_roundtrip(JSContext* ctx, const char* source,
                       const char* name, bool optimize) {
    RtResult r;
    JSValue module = JS_Eval(ctx, source, std::strlen(source), name,
                             JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    if (JS_IsException(module)) {
        JSValue ex = JS_GetException(ctx);
        const char* s = JS_ToCString(ctx, ex);
        r.ok = false;
        r.value = s ? s : "<no message>";
        JS_FreeCString(ctx, s);
        JS_FreeValue(ctx, ex);
        return r;
    }
    std::size_t size = 0;
    std::uint8_t* data =
        JS_WriteObject(ctx, &size, module, JS_WRITE_OBJ_BYTECODE);
    JS_FreeValue(ctx, module);
    if (data == nullptr) {
        r.ok = false;
        r.value = "<serialize failed>";
        return r;
    }
    std::vector<std::uint8_t> buf(data, data + size);
    js_free(ctx, data);
    if (optimize) {
        std::vector<std::uint8_t> opt;
        std::string err;
        if (!capsid::bytecode::optimize(buf, &opt, 0xffffffffu, false,
                                        &err)) {
            r.ok = false;
            r.value = "<optimize failed: " + err + ">";
            return r;
        }
        buf.swap(opt);
    }
    JSValue back = JS_ReadObject(ctx, buf.data(), buf.size(),
                                 JS_READ_OBJ_BYTECODE);
    if (JS_IsException(back)) {
        JSValue ex = JS_GetException(ctx);
        const char* s = JS_ToCString(ctx, ex);
        r.ok = false;
        r.value = s ? s : "<read failed>";
        JS_FreeCString(ctx, s);
        JS_FreeValue(ctx, ex);
        return r;
    }
    JSValue ret = JS_EvalFunction(ctx, back);
    if (JS_IsException(ret)) {
        JSValue ex = JS_GetException(ctx);
        r.ok = false;
        const char* msg = JS_ToCString(ctx, ex);
        r.value = msg ? msg : "<exception>";
        JS_FreeCString(ctx, msg);
        JSValue st = JS_GetPropertyStr(ctx, ex, "stack");
        if (JS_IsString(st)) {
            const char* s = JS_ToCString(ctx, st);
            if (s != nullptr) r.stack = s;
            JS_FreeCString(ctx, s);
        }
        JS_FreeValue(ctx, st);
        JS_FreeValue(ctx, ex);
        JS_FreeValue(ctx, ret);
        return r;
    }
    // Modules evaluate to a promise in this quickjs-ng; a top-level
    // throw rejects it instead of propagating as an exception. The
    // promise is settled synchronously (js_evaluate_module calls the
    // resolving functions directly), so the state is inspectable right
    // away — no job draining needed.
    if (JS_PromiseState(ctx, ret) == JS_PROMISE_REJECTED) {
        JSValue reason = JS_PromiseResult(ctx, ret);
        r.ok = false;
        const char* msg = JS_ToCString(ctx, reason);
        r.value = msg ? msg : "<module rejected>";
        JS_FreeCString(ctx, msg);
        JSValue st = JS_GetPropertyStr(ctx, reason, "stack");
        if (JS_IsString(st)) {
            const char* s = JS_ToCString(ctx, st);
            if (s != nullptr) r.stack = s;
            JS_FreeCString(ctx, s);
        }
        JS_FreeValue(ctx, st);
        JS_FreeValue(ctx, reason);
        JS_FreeValue(ctx, ret);
        return r;
    }
    if (JS_PromiseState(ctx, ret) == JS_PROMISE_PENDING) {
        r.ok = false;
        r.value = "<module evaluation pending>";
        JS_FreeValue(ctx, ret);
        return r;
    }
    JS_FreeValue(ctx, ret);
    JSValue g = JS_GetGlobalObject(ctx);
    JSValue v = JS_GetPropertyStr(ctx, g, "__r");
    JS_FreeValue(ctx, g);
    r.ok = true;
    if (JS_IsNumber(v)) {
        double d;
        JS_ToFloat64(ctx, &d, v);
        char tmp[64];
        std::snprintf(tmp, sizeof(tmp), "%g", d);
        r.value = tmp;
    } else {
        const char* s = JS_ToCString(ctx, v);
        r.value = s ? s : "<non-string>";
        JS_FreeCString(ctx, s);
    }
    JS_FreeValue(ctx, v);
    return r;
}

void test_roundtrip_values() {
    JSRuntime* rt = JS_NewRuntime();
    CHECK(rt != nullptr);
    JSContext* ctx = JS_NewContext(rt);
    CHECK(ctx != nullptr);
    if (ctx == nullptr) return;

    const char* sources[] = {
        "globalThis.__r = (1 + 2) * (3 + 4);",
        "let acc = 0; for (let i = 0; i < 1000; i++) acc += i; "
        "globalThis.__r = acc;",
        "function fib(n) { return n < 2 ? n : fib(n-1) + fib(n-2); } "
        "globalThis.__r = fib(20);",
        "globalThis.__r = (x => x * x)(7) + 1;",
        "try { throw new Error('inner'); } catch (e) { "
        "globalThis.__r = 'caught'; }",
        "globalThis.__r = `${1 + 1}-${'x'.repeat(2)}`;",
        "globalThis.__r = 1n + 2n;",
        "class A { constructor() { this.v = 3; } m() { return this.v * 2; } }"
        "globalThis.__r = new A().m();",
        "globalThis.__r = [1, 2, 3].map(x => x * 10).reduce((a, b) => a + b, 0);",
        "let s = 0; for (const x of [1, 2, 3, 4]) s += x; "
        "globalThis.__r = s;",
        "function* g() { yield 1; yield 2; } let acc = 0; "
        "for (const x of g()) acc += x; globalThis.__r = acc;",
        "let acc = 0; for (let i = 0; i < 100; i++) "
        "acc += i % 2 === 0 ? i : -i; globalThis.__r = acc;",
        "globalThis.__r = eval('1 + 2') * 3;",
    };
    for (const char* src : sources) {
        RtResult base = run_roundtrip(ctx, src, "rt.js", false);
        RtResult opt = run_roundtrip(ctx, src, "rt.js", true);
        CHECK(base.ok == opt.ok);
        if (base.ok) {
            CHECK(base.value == opt.value);
        }
        if (base.value != opt.value) {
            std::fprintf(stderr, "  source: %s\n", src);
            std::fprintf(stderr, "    base=%s opt=%s\n", base.value.c_str(),
                         opt.value.c_str());
        }
    }
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
}

void test_roundtrip_exception_lines() {
    JSRuntime* rt = JS_NewRuntime();
    CHECK(rt != nullptr);
    JSContext* ctx = JS_NewContext(rt);
    CHECK(ctx != nullptr);
    if (ctx == nullptr) return;

    const char* src =
        "const a = 1;\n"
        "const b = 2;\n"
        "const c = a + b;\n"
        "throw new Error('boom');\n"
        "globalThis.__r = c;\n";
    RtResult base = run_roundtrip(ctx, src, "rt.js", false);
    RtResult opt = run_roundtrip(ctx, src, "rt.js", true);
    CHECK(!base.ok);
    CHECK(!opt.ok);
    CHECK(base.value == opt.value);
    CHECK(base.stack == opt.stack);
    if (base.stack != opt.stack) {
        std::fprintf(stderr, "  base stack: %s\n", base.stack.c_str());
        std::fprintf(stderr, "  opt  stack: %s\n", opt.stack.c_str());
    }
    // The line number of the throw must be preserved through the pc2line
    // remap (source line 4, 1-based).
    CHECK(base.stack.find("rt.js:4") != std::string::npos);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
}

}  // namespace

int main() {
    test_peephole_goldens();
    test_fail_closed_matrix();
    test_cpool_kept();
    test_p2_gates();
    test_dead_block_catch_target();
    test_debug_block_remap();
    test_determinism_and_idempotence();
    test_roundtrip_values();
    test_roundtrip_exception_lines();
    if (g_failures != 0) {
        std::fprintf(stderr, "test_bytecode_optimizer: %d failure(s)\n",
                     g_failures);
        return 1;
    }
    std::fprintf(stderr, "test_bytecode_optimizer: all checks passed\n");
    return 0;
}
