// Unit gate for the bytecode AOT optimizer
// (docs/bytecode-aot-optimizer.md). Part A drives optimize() on
// synthetic .qjsb buffers: per-peephole golden bytes (P2 + P3.1 — the
// G4-trimmed pipeline; see the PassFlags comment), the P2 lattice with
// its dynamic-scope gates, cpool skipping, pc2line remapping, the
// fail-closed matrix, determinism, and checksum validity — no runtime
// needed. Part B runs the real pipeline (JS_Eval COMPILE_ONLY →
// JS_WriteObject → optimize → JS_ReadObject → JS_EvalFunction) through
// tjs and compares return values, exceptions, and stack-trace line
// numbers against the unoptimized path.
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

#include "bytecode_optimizer/bytecode_optimizer.h"
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

// Serialized nested function record (BC_TAG_FUNCTION_BYTECODE + header +
// vardefs + closure vars + cpool + code; no debug block — the allow_debug
// flag is clear). Field order mirrors JS_WriteFunctionBytecode
// (quickjs.c:37971-38041): flags u16, strict u8, name atom, 8 lebs
// (arg/var/defined_arg/stack/var_ref/closure_var/cpool/code_len),
// vardef count + entries (arg-first; captured bit 0x40 adds a
// var_ref_idx leb), closure entries (name atom + var_idx + flags),
// cpool, code. closure_entries are (var_idx, flags) pairs with the
// closure type in the low 3 bits of flags.
std::vector<std::uint8_t> make_child_record(
    std::uint32_t arg_count, std::uint32_t var_count,
    std::uint32_t stack_size, const std::vector<std::uint8_t>& code,
    const std::vector<std::pair<std::uint32_t, std::uint8_t>>&
        closure_entries = {},
    const std::vector<std::uint8_t>* captured = nullptr,
    std::uint32_t var_ref_count = 0,
    const std::vector<std::uint8_t>* cpool = nullptr,
    std::uint32_t cpool_count = 0) {
    std::vector<std::uint8_t> b;
    b.push_back(12);  // BC_TAG_FUNCTION_BYTECODE
    put_u16(&b, 0);   // flags
    b.push_back(1);   // strict
    put_leb(&b, 0);   // function name atom
    put_leb(&b, arg_count);
    put_leb(&b, var_count);
    put_leb(&b, 0);  // defined_arg_count
    put_leb(&b, stack_size);
    put_leb(&b, var_ref_count);
    put_leb(&b, static_cast<std::uint32_t>(closure_entries.size()));
    put_leb(&b, cpool_count);
    put_leb(&b, static_cast<std::uint32_t>(code.size()));
    put_leb(&b, arg_count + var_count);
    for (std::uint32_t i = 0; i < arg_count + var_count; i++) {
        put_leb(&b, 0);  // atom, scope_level, scope_next, flags
        put_leb(&b, 0);
        put_leb(&b, 0);
        const bool cap =
            captured != nullptr &&
            static_cast<size_t>(i) < captured->size() &&
            (*captured)[static_cast<size_t>(i)] != 0;
        b.push_back(cap ? 0x40 : 0);
        if (cap) put_leb(&b, 0);  // var_ref_idx
    }
    for (const auto& e : closure_entries) {
        put_leb(&b, 0);         // var_name atom
        put_leb(&b, e.first);   // var_idx
        put_leb(&b, e.second);  // flags (closure type in low 3 bits)
    }
    if (cpool != nullptr) {
        b.insert(b.end(), cpool->begin(), cpool->end());
    }
    b.insert(b.end(), code.begin(), code.end());
    return b;
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
                const std::vector<std::uint8_t>* debug = nullptr,
                const std::vector<std::uint8_t>* captured = nullptr,
                std::uint32_t var_ref_count = 0,
                std::uint32_t arg_count = 0) {
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
        put_leb(&buf, arg_count);
        put_leb(&buf, static_cast<std::uint32_t>(var_count));
        put_leb(&buf, 0);   // defined_arg_count
        put_leb(&buf, stack_size);
        put_leb(&buf, var_ref_count);
        put_leb(&buf, 0);   // closure_var_count
        put_leb(&buf, cpool_count);
        put_leb(&buf, static_cast<std::uint32_t>(code.size()));
        // vardefs are arg-first: arg_count entries, then var_count.
        put_leb(&buf, static_cast<std::uint32_t>(arg_count + var_count));
        for (std::uint32_t i = 0; i < arg_count + var_count; i++) {
            put_leb(&buf, 0);  // atom, scope_level, scope_next, flags
            put_leb(&buf, 0);
            put_leb(&buf, 0);
            const bool cap =
                captured != nullptr &&
                static_cast<size_t>(i) < captured->size() &&
                (*captured)[static_cast<size_t>(i)] != 0;
            buf.push_back(cap ? 0x40 : 0);  // is_captured (bit 6)
            if (cap) put_leb(&buf, 0);      // var_ref_idx
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

    bool run_optimize(std::vector<std::uint8_t>* out, std::string* err,
                      std::uint32_t passes) {
        if (!capsid::bytecode::optimize(buf, out, passes, false, err)) {
            return false;
        }
        CHECK(out->size() > 5);
        // Header checksum must match an independent recompute.
        std::uint32_t stored = static_cast<std::uint32_t>((*out)[1]) |
                               (static_cast<std::uint32_t>((*out)[2]) << 8) |
                               (static_cast<std::uint32_t>((*out)[3]) << 16) |
                               (static_cast<std::uint32_t>((*out)[4]) << 24);
        CHECK(stored == bc_csum(out->data() + 5, out->size() - 5));
        return true;
    }

    // Parse the root function record (the module's own function) to its
    // code blob (layout fixed for this builder: atom 0, no closure vars,
    // cpool allowed but all root-code tests here use none).
    bool optimize_and_code(std::vector<std::uint8_t>* out_code,
                           std::string* err, std::uint32_t passes) {
        std::vector<std::uint8_t> out;
        if (!run_optimize(&out, err, passes)) return false;
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
        std::uint32_t vc = vals[0] + vals[1];
        while (out[p++] & 0x80) { /* vardefs count */ }
        for (std::uint32_t i = 0; i < vc; i++) {
            for (int k = 0; k < 3; k++) {
                while (out[p++] & 0x80) { /* vardef fields */ }
            }
            if (out[p++] & 0x40) {  // is_captured: extra var_ref_idx leb
                while (out[p++] & 0x80) { }
            }
        }
        out_code->assign(out.begin() + static_cast<std::ptrdiff_t>(p),
                         out.end());
        return true;
    }

    // Navigate to the nested function under test: Root (the module's
    // function) has cpool [F], F has cpool [child]. Skips Root's header,
    // vardefs and closure vars, then F's header, vardefs and closure
    // vars, then the child record in full; returns F's code blob (its
    // code_len from F's header, since no debug block follows).
    bool optimize_and_fcode(std::vector<std::uint8_t>* out_code,
                            std::string* err, std::uint32_t passes) {
        std::vector<std::uint8_t> out;
        if (!run_optimize(&out, err, passes)) return false;
        std::size_t p = 5;
        auto leb32 = [&out, &p]() -> std::uint32_t {
            std::uint32_t x = 0;
            int shift = 0;
            for (;;) {
                x |= static_cast<std::uint32_t>(out[p] & 0x7f) << shift;
                if (!(out[p++] & 0x80)) break;
                shift += 7;
            }
            return x;
        };
        while (out[p++] & 0x80) { /* atom count */ }
        p += 1 + 4;  // atom 0 const
        p += 1 + 1;  // module tag + name atom
        for (int k = 0; k < 4; k++) leb32();
        p += 1;  // has_tla
        // Root record: fn tag + flags + strict + name + 8 header lebs.
        p += 1 + 2 + 1;
        leb32();
        std::uint32_t rvals[8] = {0};
        for (int f = 0; f < 8; f++) rvals[f] = leb32();
        leb32();  // vardefs count
        for (std::uint32_t i = 0; i < rvals[0] + rvals[1]; i++) {
            leb32();
            leb32();
            leb32();
            if (out[p++] & 0x40) leb32();
        }
        for (std::uint32_t i = 0; i < rvals[5]; i++) {
            leb32();
            leb32();
            leb32();
        }
        // Root cpool holds exactly F.
        CHECK(rvals[6] == 1);
        p += 1 + 2 + 1;  // F: fn tag + flags + strict
        leb32();         // F name atom
        std::uint32_t fvals[8] = {0};
        for (int f = 0; f < 8; f++) fvals[f] = leb32();
        leb32();  // F vardefs count
        for (std::uint32_t i = 0; i < fvals[0] + fvals[1]; i++) {
            leb32();
            leb32();
            leb32();
            if (out[p++] & 0x40) leb32();
        }
        for (std::uint32_t i = 0; i < fvals[5]; i++) {
            leb32();
            leb32();
            leb32();
        }
        // F cpool holds exactly the capturing child; skip it whole.
        CHECK(fvals[6] == 1);
        p += 1 + 2 + 1;  // child: fn tag + flags + strict
        leb32();         // child name atom
        std::uint32_t cvals[8] = {0};
        for (int f = 0; f < 8; f++) cvals[f] = leb32();
        leb32();  // child vardefs count
        for (std::uint32_t i = 0; i < cvals[0] + cvals[1]; i++) {
            leb32();
            leb32();
            leb32();
            if (out[p++] & 0x40) leb32();
        }
        for (std::uint32_t i = 0; i < cvals[5]; i++) {
            leb32();
            leb32();
            leb32();
        }
        CHECK(cvals[6] == 0);  // child has no cpool of its own
        p += cvals[7];         // child code (no debug block)
        // F's code follows immediately.
        out_code->assign(out.begin() + static_cast<std::ptrdiff_t>(p),
                         out.begin() + static_cast<std::ptrdiff_t>(p) +
                             fvals[7]);
        return true;
    }
};

void check_code(const char* name, const std::vector<std::uint8_t>& code,
                const char* hex) {
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

void expect_code(const char* name, Builder* b, const char* hex,
                 std::uint32_t passes = 0xffffffffu) {
    std::vector<std::uint8_t> code;
    std::string err;
    if (!b->optimize_and_code(&code, &err, passes)) {
        std::fprintf(stderr, "FAIL %s: optimize error: %s\n", name,
                     err.c_str());
        g_failures++;
        return;
    }
    check_code(name, code, hex);
}

// expect_code for the nested-function layout: Root's cpool holds F (the
// function under test), F's cpool holds the capturing child.
void expect_fcode(const char* name, Builder* b, const char* hex,
                  std::uint32_t passes = 0xffffffffu) {
    std::vector<std::uint8_t> code;
    std::string err;
    if (!b->optimize_and_fcode(&code, &err, passes)) {
        std::fprintf(stderr, "FAIL %s: optimize error: %s\n", name,
                     err.c_str());
        g_failures++;
        return;
    }
    check_code(name, code, hex);
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
        // P2 folds the read (cell 1), P3.1 folds 1+1 -> 2. P16
        // (TDZ-sound dead-store elimination, tier-2b) then removes the
        // dead store x=1: slot 0 is never read after the store on any
        // path, and the producer is the pure push_1. Output is
        // push_2; return.
        expect_code("p2 crossbb x+1", &b, "bc 28");
    }
}

void test_p16_dead_store_goldens() {
    // P16 (tier-2b, TDZ-sound dead-store elimination). Slot 4 is used
    // so P6's re-shorten (put_loc0-3 are 1-byte forms) cannot change
    // the expected bytes. Stack heights stay within stack_size (the
    // output verifier rejects underflow and over-height).
    //
    // Opcode bytes (BC_VERSION 26, quickjs-ng enum): set_loc_uninitialized
    // 0x60+u16 loc, push_i32 0x01+u32, put_loc8 0xc8+u8, get_loc_check /
    // put_loc_check 0x61/0x62+u16 loc, push_0..7 0xba..0xc1, put_loc0 0xcf,
    // push_this 0x08, eval 0x32+u16, if_false8 0xf0+i8, return 0x28,
    // return_undef 0x29.
    //
    // dead-triple-marker: marker + store + pure-push producer are all
    // dead (slot 4 is never read) -> only return_undef survives.
    {
        Builder b;
        b.op_u16(96, 4);  // set_loc_uninitialized 4
        b.op_i32(1, 5);   // push_i32 5
        b.op(200);        // put_loc8 4
        b.op(4);
        b.op(41);         // return_undef
        b.stack_size = 1;
        b.finish(5);
        expect_code("p16 dead-triple-marker", &b, "29");
    }
    // dead-store-no-marker: the dead store x=1 (push_1; put_loc0) is
    // removed with its producer -> push_2; return.
    {
        Builder b;
        b.op(187);        // push_1
        b.op(207);        // put_loc0
        b.op(188);        // push_2
        b.op(40);         // return
        b.stack_size = 1;
        b.finish(1);
        expect_code("p16 dead-store-no-marker", &b, "bc 28");
    }
    // tdz-keep-read-after: the store feeds the get_loc_check read and
    // stays; the marker is overwritten by the store before any read
    // (dead) and is removed. push_atom_value (not push_i32: P2 tracks
    // push_i32 as an exact K_INT and would fold the read away before
    // P16 runs) is a K_ATOM the lattice never folds. With tier-3
    // Lane 1 (kPassTier3Lane1) the slot is provably INIT at the read, so
    // the check is additionally rewritten to get_loc and shortened to
    // get_loc8 (61 04 00 -> c7 04).
    {
        Builder b;
        b.op_u16(96, 4);  // set_loc_uninitialized 4
        b.op_i32(4, 0);   // push_atom_value 0
        b.op(200);        // put_loc8 4
        b.op(4);
        b.op_u16(97, 4);  // get_loc_check 4
        b.op(40);         // return
        b.stack_size = 1;
        b.finish(5);
        expect_code("p16 tdz-keep-read-after", &b,
                    "04 00 00 00 00 c8 04 c7 04 28");
    }
    // tdz-keep-marker-live: the get_loc_check between marker and store
    // reads the marker value, so the marker stays; the trailing store
    // is dead and is removed with its producer.
    {
        Builder b;
        b.op_u16(96, 4);  // set_loc_uninitialized 4
        b.op_u16(97, 4);  // get_loc_check 4
        b.op_i32(1, 5);   // push_i32 5
        b.op(200);        // put_loc8 4
        b.op(4);
        b.op(40);         // return
        b.stack_size = 2;
        b.finish(5);
        expect_code("p16 tdz-keep-marker-live", &b, "60 04 00 61 04 00 28");
    }
    // loop-carried-keep: slot 4 is read by put_loc_check (a check-form
    // write that reads the slot for the TDZ test) on every loop
    // iteration, so the entry store is live across the backedge and
    // stays; the marker is overwritten by the store before any read and
    // is removed. if_false8 at byte 11 targets byte 6:
    // operand_start = 12, diff = -6 (0xfa). With tier-3 Lane 1 the slot
    // is INIT on every path into the check (entry store + backedge
    // re-store), so the check is rewritten to put_loc and shortened to
    // put_loc8; the loop body shrinks by one byte and the backedge
    // becomes -5 (0xfb).
    {
        Builder b;
        b.op_u16(96, 4);  // set_loc_uninitialized 4
        b.op(186);        // push_0
        b.op(200);        // put_loc8 4
        b.op(4);
        b.op(186);        // push_0
        b.op_u16(98, 4);  // put_loc_check 4
        b.op(187);        // push_1
        b.op(240);        // if_false8 -> loop head (byte 6)
        b.op(0xfa);
        b.op(188);        // push_2
        b.op(40);         // return
        b.stack_size = 1;
        b.finish(5);
        expect_code("p16 loop-carried-keep", &b,
                    "ba c8 04 ba c8 04 bb f0 fb bc 28");
    }
    // captured-keep: slot 4 is captured (vardef flag 0x40), so the
    // marker and store are never deleted even though slot 4 is dead.
    // return_undef (no stack value needed: the store consumed the
    // producer), push_i32 5 re-shortens to push_5 under P6.
    {
        Builder b;
        b.op_u16(96, 4);  // set_loc_uninitialized 4
        b.op_i32(1, 5);   // push_i32 5
        b.op(200);        // put_loc8 4
        b.op(4);
        b.op(41);         // return_undef
        b.stack_size = 1;
        const std::vector<std::uint8_t> captured = {0, 0, 0, 0, 1};
        b.finish(5, 0, nullptr, &captured);
        expect_code("p16 captured-keep", &b,
                    "60 04 00 bf c8 04 29");
    }
    // Captured vardefs are arg-first. With three arguments, captured local
    // slot 0 is vardef index 3, not index 0; P16 must retain its marker and
    // store. This is the serialized layout used by real nested functions.
    {
        Builder b;
        b.op_u16(96, 0);  // set_loc_uninitialized 0
        b.op_i32(1, 5);   // push_i32 5
        b.op_u16(88, 0);  // put_loc 0
        b.op(41);         // return_undef
        b.stack_size = 1;
        const std::vector<std::uint8_t> captured = {0, 0, 0, 1};
        b.finish(1, 0, nullptr, &captured, 1, 3);
        expect_code("p16 captured-after-args-keep", &b,
                    "60 00 00 bf cf 29");
    }
    // producer-side-effect: the store's producer is push_this, not a
    // pure push, so the store stays; the marker is dead (never read)
    // and is removed.
    {
        Builder b;
        b.op_u16(96, 4);  // set_loc_uninitialized 4
        b.op(8);          // push_this
        b.op(200);        // put_loc8 4
        b.op(4);
        b.op(188);        // push_2
        b.op(40);         // return
        b.stack_size = 1;
        b.finish(5);
        expect_code("p16 producer-side-effect", &b, "08 c8 04 bc 28");
    }
    // barrier-keep: OP_eval is a slot-alias barrier; the whole function
    // is left untouched except P6's re-shorten (push_i32 5 -> push_5,
    // push_i32 1 -> push_1). eval is 5 bytes: opcode + u16 argc + u16
    // scope index.
    {
        Builder b;
        b.op_u16(96, 4);  // set_loc_uninitialized 4
        b.op_i32(1, 5);   // push_i32 5
        b.op(200);        // put_loc8 4
        b.op(4);
        b.op_i32(1, 1);   // the "function" for eval
        b.op(50);         // eval argc=0, scope=0
        b.op(0);
        b.op(0);
        b.op(0);
        b.op(0);
        b.op(40);         // return
        b.stack_size = 1;
        b.finish(5);
        expect_code("p16 barrier-keep", &b,
                    "60 04 00 bf c8 04 bb 32 00 00 00 00 28");
    }
    // tdz-check-store: put_loc_check reads the slot for the TDZ test,
    // so the marker stays (the read makes the slot live), and
    // check-form stores are never candidates (their observable TDZ
    // error must survive). push_i32 5 re-shortens to push_5 under P6.
    {
        Builder b;
        b.op_u16(96, 4);  // set_loc_uninitialized 4
        b.op_i32(1, 5);   // push_i32 5
        b.op_u16(98, 4);  // put_loc_check 4
        b.op(188);        // push_2
        b.op(40);         // return
        b.stack_size = 1;
        b.finish(5);
        expect_code("p16 tdz-check-store", &b,
                    "60 04 00 bf 62 04 00 bc 28");
    }
}

// ---------------------------------------------------------------------------
// Tier-3 Lane 1 (kPassTier3Lane1): P18 TDZ-check elimination. The pass is
// a pure opcode rewrite of provably-safe get_loc_check/put_loc_check
// sites to the plain loc ops (which P6 then shortens). Soundness relies
// on the A3 slot-init lattice: a site is rewritten only when every
// reaching path into it has initialized the slot.
// ---------------------------------------------------------------------------
void test_tier3_lane1_goldens() {
    // t3 merge-maybe-stays: the entry marker sets UNINIT; the branch
    // true-path stores (INIT) but the false-path jumps straight to the
    // merge, so the get_loc_check sees MAYBE and must keep its check.
    // push_atom_value 0 is the condition (a K_ATOM: P2's constant
    // lattice never folds it, unlike push_0/push_i32). The store value
    // is pushed after the branch (both paths reach the merge at height
    // 0; get_loc_check then pushes to 1). if_false8 at byte 8 targets
    // byte 13 (target = pc+1+diff = 9+4).
    {
        Builder b;
        b.op_u16(96, 4);  // set_loc_uninitialized 4
        b.op_i32(4, 0);   // push_atom_value 0 (condition)
        b.op(240);        // if_false8 -> merge (byte 13)
        b.op(0x04);
        b.op(186);        // push_0 (store value, true path)
        b.op(200);        // put_loc8 4
        b.op(4);
        b.op_u16(97, 4);  // get_loc_check 4
        b.op(40);         // return
        b.stack_size = 1;
        b.finish(5);
        expect_code("t3 merge-maybe-stays", &b,
                    "60 04 00 04 00 00 00 00 f0 04 ba c8 04 61 04 00 28");
    }
    // t3 uninit-read-stays: the get_loc_check reads the live marker
    // (nothing wrote the slot since set_loc_uninitialized), so the check
    // must survive. Same stream as p16 tdz-keep-marker-live.
    {
        Builder b;
        b.op_u16(96, 4);  // set_loc_uninitialized 4
        b.op_u16(97, 4);  // get_loc_check 4
        b.op_i32(1, 5);   // push_i32 5
        b.op(200);        // put_loc8 4
        b.op(4);
        b.op(40);         // return
        b.stack_size = 2;
        b.finish(5);
        expect_code("t3 uninit-read-stays", &b, "60 04 00 61 04 00 28");
    }
    // t3 eval-gate-skips-whole: the slot is provably INIT (store before
    // the read) but the function contains eval, an unmodeled CFG gate —
    // the whole function is skipped, so the check stays. eval is 5
    // bytes (opcode + u16 argc + u16 scope) and consumes argc+1 stack
    // values; a second push_0 provides the dummy callable.
    {
        Builder b;
        b.op_u16(96, 4);  // set_loc_uninitialized 4
        b.op(186);        // push_0
        b.op(200);        // put_loc8 4
        b.op(4);
        b.op(186);        // push_0 (dummy callable for eval)
        b.op(50);         // eval argc=0, scope=0
        b.op(0);
        b.op(0);
        b.op(0);
        b.op(0);
        b.op_u16(97, 4);  // get_loc_check 4
        b.op(40);         // return
        b.stack_size = 2;  // get_loc_check pushes to height 2
        b.finish(5);
        expect_code("t3 eval-gate-skips-whole", &b,
                    "60 04 00 ba c8 04 ba 32 00 00 00 00 61 04 00 28");
    }
    // t3 pass-off: the identical stream that tdz-keep-read-after rewrites
    // keeps its check when the tier-3 bit is off — the pass is
    // independently switchable (G4 attribution discipline).
    {
        Builder b;
        b.op_u16(96, 4);  // set_loc_uninitialized 4
        b.op_i32(4, 0);   // push_atom_value 0
        b.op(200);        // put_loc8 4
        b.op(4);
        b.op_u16(97, 4);  // get_loc_check 4
        b.op(40);         // return
        b.stack_size = 1;
        b.finish(5);
        expect_code("t3 pass-off", &b,
                    "04 00 00 00 00 c8 04 61 04 00 28",
                    0xffffffffu & ~capsid::bytecode::kPassTier3Lane1);
    }
}

// P18 (Lane 2) read-only-capture gate: the base's slot class survives
// form-C analysis only when the capturing subtree provably never writes
// the captured slot. Layout: Root (module function) -> cpool F -> cpool
// child. F has arg 0 (get_arg: class-unknown by design — the guard
// site's key can only be deleted through form C) and captured var 0
// (base). The child's closure entry is (var_idx 0, CLOSURE_LOCAL), so a
// put_var_ref0 in the child marks F.cwritable[arg_count+0]. F's code is
// the sieve-rt guarded-array-store shape: entering [x, base, key],
// `swap dup is_undefined_or_null if_true8` tests the base, both paths
// converge at the merge swap with [x, key, base], and the merge site's
// to_propkey runs in a block whose entry stack classes are all-unknown
// (conservative) — so only the guard site can ever fold.
void test_p18_readonly_capture_goldens() {
    // 0: array_from 0; 3: put_loc 0 (slot0=base); 6: push_1 (x);
    // 7: get_loc 0; 10: get_arg 0 (key; re-shortens to get_arg0); 13: swap dup
    // is_undefined_or_null if_true8 -> merge; 18: swap to_propkey swap
    // (guard site); 21: swap (merge); 22: push_1 swap to_propkey swap
    // (merge site); 26: put_array_el; 27: return_undef.
    const std::vector<std::uint8_t> f_code = {
        38, 0, 0,    // array_from 0
        88, 0, 0,    // put_loc 0
        187,         // push_1
        87, 0, 0,    // get_loc 0
        90, 0, 0,    // get_arg 0
        27,          // swap
        17,          // dup
        175,         // is_undefined_or_null
        241, 4,      // if_true8 -> byte 17 (merge swap)
        27,          // swap
        113,         // to_propkey   (guard site)
        27,          // swap
        27,          // swap         (merge target)
        187,         // push_1
        27,          // swap
        113,         // to_propkey   (merge site)
        27,          // swap
        72,          // put_array_el
        41,          // return_undef
    };
    const std::vector<std::uint8_t> f_captured = {0, 1, 0, 0};
    // readonly-keep: the capturing child only reads the captured slot
    // (get_var_ref0; pop), so F.cwritable stays clear, the base's ARRAY
    // class survives the get_loc, and form C deletes the guard site.
    // P6 re-shortens put_loc 0 / get_loc 0 to put_loc0 / get_loc0; the
    // merge swap moves up one byte so the if_true8 diff becomes 3.
    {
        const std::vector<std::uint8_t> child_code = {227, 14, 41};
        std::vector<std::uint8_t> child =
            make_child_record(0, 0, 1, child_code, {{0, 0}});
        std::vector<std::uint8_t> f = make_child_record(
            1, 3, 4, f_code, {}, &f_captured, 1, &child, 1);
        Builder b;
        b.cpool = std::move(f);
        b.cpool_count = 1;
        b.code = {41};  // root: return_undef
        b.finish(0);
        expect_fcode("p18 readonly-capture keep", &b,
                     "26 00 00 cf bb cb d7 1b 11 af f1 03 1b 1b 1b"
                     " bb 1b 71 1b 48 29");
    }
    // closure-write-keep: the child writes the captured slot
    // (put_var_ref0), so F.cwritable[arg_count+0] is set, the base's
    // class is lost at the get_loc, and neither site folds. Output is
    // the input after P6's re-shorten only.
    {
        const std::vector<std::uint8_t> child_code = {194, 1, 231, 41};
        std::vector<std::uint8_t> child =
            make_child_record(0, 0, 1, child_code, {{0, 0}});
        std::vector<std::uint8_t> f = make_child_record(
            1, 3, 4, f_code, {}, &f_captured, 1, &child, 1);
        Builder b;
        b.cpool = std::move(f);
        b.cpool_count = 1;
        b.code = {41};
        b.finish(0);
        expect_fcode("p18 closure-write keep", &b,
                     "26 00 00 cf bb cb d7 1b 11 af f1 04 1b 71 1b"
                     " 1b bb 1b 71 1b 48 29");
    }
    // opaque-eval-keep: the child contains OP_eval, which can write
    // anything it reaches — the subtree is opaque, F.cwritable fills,
    // and neither site folds (same bytes as closure-write-keep).
    {
        const std::vector<std::uint8_t> child_code = {194, 1, 50, 0, 0, 0,
                                                      0,   14, 41};
        std::vector<std::uint8_t> child =
            make_child_record(0, 0, 1, child_code, {{0, 0}});
        std::vector<std::uint8_t> f = make_child_record(
            1, 3, 4, f_code, {}, &f_captured, 1, &child, 1);
        Builder b;
        b.cpool = std::move(f);
        b.cpool_count = 1;
        b.code = {41};
        b.finish(0);
        expect_fcode("p18 opaque-eval keep", &b,
                     "26 00 00 cf bb cb d7 1b 11 af f1 04 1b 71 1b"
                     " 1b bb 1b 71 1b 48 29");
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
    {
        Builder b;
        b.op(253);  // runtime-only get_field_ic is never legal in BC26
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


void test_p11_folds() {
    // P11 copy propagation + dead store materialization. The final
    // reshrink collapses surviving get_loc 0 to get_loc0 (0xcb).
    //
    // Chain: get_loc a; put_loc b; get_loc b; put_loc c; get_loc c;
    // return. b and c are dead (all reads renamed to a), so both
    // (get,put) pairs vanish -> get_loc a; return.
    {
        Builder b;
        b.op_u16(0x57, 0);  // get_loc 0
        b.op_u16(0x58, 1);  // put_loc 1
        b.op_u16(0x57, 1);  // get_loc 1
        b.op_u16(0x58, 2);  // put_loc 2
        b.op_u16(0x57, 2);  // get_loc 2
        b.op(0x28);         // return
        b.stack_size = 1;
        b.finish(3);
        std::vector<std::uint8_t> code;
        std::string err;
        CHECK(b.optimize_and_code(&code, &err, capsid::bytecode::kPassP11));
        const std::uint8_t expect[] = {0xcb, 0x28};  // get_loc0; return
        CHECK(code.size() == sizeof(expect));
        if (code.size() == sizeof(expect)) {
            CHECK(std::memcmp(code.data(), expect, sizeof(expect)) == 0);
        }
    }
    // Rename + surviving self-store: get_loc a; put_loc b; get_loc b
    // (renamed to a); put_loc a (same slot, not a candidate); get_loc a;
    // return. Only the first pair dies; the renamed read and the
    // same-slot store survive.
    {
        Builder b;
        b.op_u16(0x57, 0);  // get_loc a
        b.op_u16(0x58, 1);  // put_loc b
        b.op_u16(0x57, 1);  // get_loc b -> renamed to get_loc 0
        b.op_u16(0x58, 0);  // put_loc a (s == sl, not a candidate)
        b.op_u16(0x57, 0);  // get_loc a
        b.op(0x28);
        b.stack_size = 1;
        b.finish(2);
        std::vector<std::uint8_t> code;
        std::string err;
        CHECK(b.optimize_and_code(&code, &err, capsid::bytecode::kPassP11));
        const std::uint8_t expect[] = {0xcb, 0xcf, 0xcb, 0x28};
        CHECK(code.size() == sizeof(expect));
        if (code.size() == sizeof(expect)) {
            CHECK(std::memcmp(code.data(), expect, sizeof(expect)) == 0);
        }
    }
}

void test_p11_gates() {
    // A conditional copy cannot establish an alias at its join: the path
    // that skips get_loc1/put_loc0 must still read the old slot 0 value.
    {
        Builder b;
        b.op(0xcb);  // get_loc0 (condition)
        b.op(0xf0);  // if_false8 -> join at byte 5
        b.op(3);
        b.op(0xcc);  // get_loc1
        b.op(0xcf);  // put_loc0
        b.op(0xcb);  // join: get_loc0
        b.op(0x28);  // return
        b.stack_size = 1;
        b.finish(2);
        std::vector<std::uint8_t> code;
        std::string err;
        CHECK(b.optimize_and_code(&code, &err,
                                  capsid::bytecode::kPassP11));
        const std::uint8_t expect[] =
            {0xcb, 0xf0, 3, 0xcc, 0xcf, 0xcb, 0x28};
        CHECK(code.size() == sizeof(expect));
        if (code.size() == sizeof(expect)) {
            CHECK(std::memcmp(code.data(), expect, sizeof(expect)) == 0);
        }
    }
    // Loc slot 0 follows three argument vardefs in the serialized capture
    // mask. P11 must not delete a copy into that captured local.
    {
        Builder b;
        b.op_u16(0x57, 1);  // get_loc 1
        b.op_u16(0x58, 0);  // put_loc 0 (captured)
        b.op(0x29);         // return_undef
        b.stack_size = 1;
        const std::vector<std::uint8_t> captured = {0, 0, 0, 1, 0};
        b.finish(2, 0, nullptr, &captured, 1, 3);
        std::vector<std::uint8_t> code;
        std::string err;
        CHECK(b.optimize_and_code(&code, &err,
                                  capsid::bytecode::kPassP11));
        const std::uint8_t expect[] = {0xcc, 0xcf, 0x29};
        CHECK(code.size() == sizeof(expect));
        if (code.size() == sizeof(expect)) {
            CHECK(std::memcmp(code.data(), expect, sizeof(expect)) == 0);
        }
    }
    // Barrier (eval) between the store and the read: the alias dies, the
    // read touches b, and the (get,put) pair must survive (0x58 stays).
    // eval is op 50 + argc u16 + scope_idx u16; the get_loc before it
    // is the eval callee.
    {
        Builder b;
        b.op_u16(0x57, 0);  // get_loc 0
        b.op_u16(0x58, 1);  // put_loc 1
        b.op_u16(0x57, 0);  // get_loc 0 (callee)
        b.op(50);           // eval
        put_u16(&b.code, 0);
        put_u16(&b.code, 0);
        b.op_u16(0x57, 1);  // get_loc 1 (after barrier)
        b.op(0x28);
        b.stack_size = 2;
        b.finish(2);
        std::vector<std::uint8_t> code;
        std::string err;
        CHECK(b.optimize_and_code(&code, &err, capsid::bytecode::kPassP11));
        bool has_put = false;
        for (std::uint8_t x : code) {
            // 0x58 = put_loc; 0xcf..0xd2 = put_loc0..put_loc3 after
            // the final reshrink.
            if (x == 0x58 || (x >= 0xcf && x <= 0xd2)) has_put = true;
        }
        CHECK(has_put);
    }
    // Loop-back shape: a read of b before the store disqualifies the
    // pair (the value could be carried around a back edge).
    {
        Builder b;
        b.op_u16(0x57, 1);  // get_loc 1 (read before store)
        b.op_u16(0x57, 0);  // get_loc 0
        b.op_u16(0x58, 1);  // put_loc 1
        b.op(0x28);
        b.stack_size = 2;
        b.finish(2);
        std::vector<std::uint8_t> code;
        std::string err;
        CHECK(b.optimize_and_code(&code, &err, capsid::bytecode::kPassP11));
        bool has_put = false;
        for (std::uint8_t x : code) {
            // 0x58 = put_loc; 0xcf..0xd2 = put_loc0..put_loc3 after
            // the final reshrink.
            if (x == 0x58 || (x >= 0xcf && x <= 0xd2)) has_put = true;
        }
        CHECK(has_put);
    }
    // Fused read: get_loc0_loc1 (0xca) reads slots 0 and 1. A store of
    // slot 1 before it has a real reader and must survive. The fused
    // read pushes both; one value is returned, one dropped.
    {
        Builder b;
        b.op_u16(0x57, 0);  // get_loc 0
        b.op_u16(0x58, 1);  // put_loc 1
        b.op(0xca);         // get_loc0_loc1 (pushes slots 0, 1)
        b.op(0x0e);         // drop
        b.op(0x28);         // return (slot 0's value)
        b.stack_size = 2;
        b.finish(2);
        std::vector<std::uint8_t> code;
        std::string err;
        CHECK(b.optimize_and_code(&code, &err, capsid::bytecode::kPassP11));
        bool has_put = false;
        for (std::uint8_t x : code) {
            if (x == 0x58 || (x >= 0xcf && x <= 0xd2)) has_put = true;
        }
        CHECK(has_put);
    }
}

void test_p14_folds() {
    // Object literal: object; push_1; define_field a; put_loc0; then
    // get_loc0; get_field a folds to push_1 (get_loc0 replaced, get_field
    // removed). Atom operands are raw u32 values; 0 works on both sides.
    {
        Builder b;
        b.op(0x0b);  // object
        b.op(0xbb);  // push_1
        b.code.push_back(0x4b);  // define_field atom 0
        put_u32(&b.code, 0);
        b.op(0xcf);  // put_loc0
        b.op(0xcb);  // get_loc0
        b.code.push_back(0x40);  // get_field atom 0
        put_u32(&b.code, 0);
        b.op(0x28);  // return
        b.stack_size = 2;
        b.finish(1);
        std::vector<std::uint8_t> code;
        std::string err;
        CHECK(b.optimize_and_code(&code, &err, capsid::bytecode::kPassP14));
        const std::uint8_t want[] = {0x0b, 0xbb, 0x4b, 0, 0, 0, 0,
                                     0xcf, 0xbb, 0x28};
        CHECK(code.size() == sizeof(want));
        CHECK(std::memcmp(code.data(), want, sizeof(want)) == 0);
    }
    // Array literal: [10, 20, 30]; a[1] folds to push_i8 20.
    {
        Builder b;
        b.op(0xc2); b.code.push_back(10);  // push_i8 10
        b.op(0xc2); b.code.push_back(20);  // push_i8 20
        b.op(0xc2); b.code.push_back(30);  // push_i8 30
        b.op_u16(0x26, 3);  // array_from 3
        b.op(0xcf);  // put_loc0
        b.op(0xcb);  // get_loc0
        b.op(0xbb);  // push_1
        b.op(0x46);  // get_array_el
        b.op(0x28);  // return
        b.stack_size = 3;
        b.finish(1);
        std::vector<std::uint8_t> code;
        std::string err;
        CHECK(b.optimize_and_code(&code, &err, capsid::bytecode::kPassP14));
        const std::uint8_t want[] = {0xc2, 10, 0xc2, 20, 0xc2, 30, 0x26,
                                     0x03, 0x00, 0xcf, 0xc2, 20, 0x28};
        CHECK(code.size() == sizeof(want));
        CHECK(std::memcmp(code.data(), want, sizeof(want)) == 0);
    }
    // String-literal field via push_atom_value folds to the identical
    // instruction (atom operand preserved verbatim).
    {
        Builder b;
        b.op(0x0b);  // object
        b.code.push_back(0x04);  // push_atom_value atom 0
        put_u32(&b.code, 0);
        b.code.push_back(0x4b);  // define_field atom 0
        put_u32(&b.code, 0);
        b.op(0xcf);
        b.op(0xcb);
        b.code.push_back(0x40);  // get_field atom 0
        put_u32(&b.code, 0);
        b.op(0x28);
        b.stack_size = 2;
        b.finish(1);
        std::vector<std::uint8_t> code;
        std::string err;
        CHECK(b.optimize_and_code(&code, &err, capsid::bytecode::kPassP14));
        const std::uint8_t want[] = {0x0b, 0x04, 0, 0, 0, 0, 0x4b,
                                     0, 0, 0, 0, 0xcf, 0x04, 0, 0, 0, 0, 0x28};
        CHECK(code.size() == sizeof(want));
        CHECK(std::memcmp(code.data(), want, sizeof(want)) == 0);
    }
    // Constant-pool field via push_const8 folds to push_const8 (re-shortened).
    {
        Builder b;
        b.op(0x0b);
        b.op(0xc4); b.code.push_back(0);  // push_const8 idx 0
        b.code.push_back(0x4b);  // define_field atom 0
        put_u32(&b.code, 0);
        b.op(0xcf);
        b.op(0xcb);
        b.code.push_back(0x40);
        put_u32(&b.code, 0);
        b.op(0x28);
        b.stack_size = 2;
        b.finish(1);
        std::vector<std::uint8_t> code;
        std::string err;
        CHECK(b.optimize_and_code(&code, &err, capsid::bytecode::kPassP14));
        const std::uint8_t want[] = {0x0b, 0xc4, 0, 0x4b, 0, 0, 0, 0,
                                     0xcf, 0xc4, 0, 0x28};
        CHECK(code.size() == sizeof(want));
        CHECK(std::memcmp(code.data(), want, sizeof(want)) == 0);
    }
}

void test_p14_gates() {
    // As with P11/P16, loc slot 0 is vardef index arg_count+0. A captured
    // literal local cannot be described/folded because its closure may
    // observe it independently of this instruction stream.
    {
        Builder b;
        b.op(0x0b);  // object
        b.op(0xbb);  // push_1
        b.code.push_back(0x4b);  // define_field atom 0
        put_u32(&b.code, 0);
        b.op(0xcf);  // put_loc0
        b.op(0xcb);  // get_loc0
        b.code.push_back(0x40);  // get_field atom 0
        put_u32(&b.code, 0);
        b.op(0x28);
        b.stack_size = 2;
        const std::vector<std::uint8_t> captured = {0, 0, 1};
        b.finish(1, 0, nullptr, &captured, 1, 2);
        std::vector<std::uint8_t> code;
        std::string err;
        CHECK(b.optimize_and_code(&code, &err,
                                  capsid::bytecode::kPassP14));
        bool has_getfield = false;
        for (std::uint8_t x : code) {
            if (x == 0x40) has_getfield = true;
        }
        CHECK(has_getfield);
    }
    // put_field between construction and read: the mutation defeats the
    // fold; the get_field must survive.
    {
        Builder b;
        b.op(0x0b);
        b.op(0xbb);
        b.code.push_back(0x4b);  // define_field atom 0
        put_u32(&b.code, 0);
        b.op(0xcf);  // put_loc0
        b.op(0xcb);  // get_loc0 (receiver)
        b.op(0xbb);  // push_1 (value)
        b.code.push_back(0x42);  // put_field atom 0
        put_u32(&b.code, 0);
        b.op(0xcb);  // get_loc0
        b.code.push_back(0x40);  // get_field atom 0
        put_u32(&b.code, 0);
        b.op(0x28);
        b.stack_size = 2;
        b.finish(1);
        std::vector<std::uint8_t> code;
        std::string err;
        CHECK(b.optimize_and_code(&code, &err, capsid::bytecode::kPassP14));
        bool has_getfield = false;
        for (std::uint8_t x : code) {
            if (x == 0x40) has_getfield = true;
        }
        CHECK(has_getfield);
    }
    // Any call is a barrier: a call between the binding and the read
    // (the described value may be observed or mutated) defeats the fold.
    {
        Builder b;
        b.op(0x0b);
        b.op(0xbb);
        b.code.push_back(0x4b);
        put_u32(&b.code, 0);
        b.op(0xcf);
        b.op(0xbb);  // callee
        b.op(0xcb);  // arg
        b.op(0xf5);  // call1
        b.op(0x0e);  // drop
        b.op(0xcb);
        b.code.push_back(0x40);
        put_u32(&b.code, 0);
        b.op(0x28);
        b.stack_size = 2;
        b.finish(1);
        std::vector<std::uint8_t> code;
        std::string err;
        CHECK(b.optimize_and_code(&code, &err, capsid::bytecode::kPassP14));
        bool has_getfield = false;
        for (std::uint8_t x : code) {
            if (x == 0x40) has_getfield = true;
        }
        CHECK(has_getfield);
    }
    // put_array_el on a slot-read array defeats the element fold.
    {
        Builder b;
        b.op(0xc2); b.code.push_back(1);
        b.op(0xc2); b.code.push_back(2);
        b.op_u16(0x26, 2);
        b.op(0xcf);
        b.op(0xcb);  // arr
        b.op(0xbb);  // idx 1
        b.op(0xc2); b.code.push_back(9);  // value
        b.op(0x48);  // put_array_el
        b.op(0xcb);
        b.op(0xbb);  // idx 1
        b.op(0x46);  // get_array_el
        b.op(0x28);
        b.stack_size = 3;
        b.finish(1);
        std::vector<std::uint8_t> code;
        std::string err;
        CHECK(b.optimize_and_code(&code, &err, capsid::bytecode::kPassP14));
        bool has_gael = false;
        for (std::uint8_t x : code) {
            if (x == 0x46) has_gael = true;
        }
        CHECK(has_gael);
    }
    // append on a slot-read array defeats the element fold.
    {
        Builder b;
        b.op(0xc2); b.code.push_back(1);
        b.op(0xc2); b.code.push_back(2);
        b.op_u16(0x26, 2);
        b.op(0xcf);
        b.op(0xcb);
        b.op(0xbb);  // pushed values
        b.op(0xc2); b.code.push_back(8);
        b.op(0x51);  // append
        b.op(0x0e);  // drop x2 (append pushes 2)
        b.op(0x0e);
        b.op(0xcb);
        b.op(0xbb);
        b.op(0x46);
        b.op(0x28);
        b.stack_size = 3;
        b.finish(1);
        std::vector<std::uint8_t> code;
        std::string err;
        CHECK(b.optimize_and_code(&code, &err, capsid::bytecode::kPassP14));
        bool has_gael = false;
        for (std::uint8_t x : code) {
            if (x == 0x46) has_gael = true;
        }
        CHECK(has_gael);
    }
    // Out-of-range element index: no fold.
    {
        Builder b;
        b.op(0xc2); b.code.push_back(1);
        b.op(0xc2); b.code.push_back(2);
        b.op(0xc2); b.code.push_back(3);
        b.op_u16(0x26, 3);
        b.op(0xcf);
        b.op(0xcb);
        b.op(0xbd);  // push_3
        b.op(0x46);
        b.op(0x28);
        b.stack_size = 3;  // three element pushes peak here
        b.finish(1);
        std::vector<std::uint8_t> code;
        std::string err;
        CHECK(b.optimize_and_code(&code, &err, capsid::bytecode::kPassP14));
        bool has_gael = false;
        for (std::uint8_t x : code) {
            if (x == 0x46) has_gael = true;
        }
        CHECK(has_gael);
    }
    // Negative element index: no fold.
    {
        Builder b;
        b.op(0xc2); b.code.push_back(1);
        b.op(0xc2); b.code.push_back(2);
        b.op_u16(0x26, 2);
        b.op(0xcf);
        b.op(0xcb);
        b.op(0xc2); b.code.push_back(0xff);  // push_i8 -1
        b.op(0x46);
        b.op(0x28);
        b.stack_size = 2;
        b.finish(1);
        std::vector<std::uint8_t> code;
        std::string err;
        CHECK(b.optimize_and_code(&code, &err, capsid::bytecode::kPassP14));
        bool has_gael = false;
        for (std::uint8_t x : code) {
            if (x == 0x46) has_gael = true;
        }
        CHECK(has_gael);
    }
    // get_field directly on the under-construction object consumes it:
    // the later put_loc must not bind a stale description (no fold on
    // the second read).
    {
        Builder b;
        b.op(0x0b);
        b.op(0xbb);
        b.code.push_back(0x4b);
        put_u32(&b.code, 0);
        b.code.push_back(0x40);  // get_field on the stack object
        put_u32(&b.code, 0);
        b.op(0xcf);  // put_loc0 holds the FIELD VALUE, not the object
        b.op(0xcb);
        b.code.push_back(0x40);
        put_u32(&b.code, 0);
        b.op(0x28);
        b.stack_size = 2;
        b.finish(1);
        std::vector<std::uint8_t> code;
        std::string err;
        CHECK(b.optimize_and_code(&code, &err, capsid::bytecode::kPassP14));
        int getfields = 0;
        for (std::uint8_t x : code) {
            if (x == 0x40) getfields++;
        }
        CHECK(getfields == 2);
    }
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

void test_classic_benchmark_boundary() {
    JSRuntime* rt = JS_NewRuntime();
    CHECK(rt != nullptr);
    JSContext* ctx = JS_NewContext(rt);
    CHECK(ctx != nullptr);
    if (ctx == nullptr) {
        JS_FreeRuntime(rt);
        return;
    }
    const char source[] =
        "var sum = 0; for (var i = 0; i < 100; i++) sum += i; "
        "globalThis.__r = sum;";
    JSValue compiled = JS_Eval(ctx, source, sizeof(source) - 1,
                               "classic.js",
                               JS_EVAL_TYPE_GLOBAL |
                                   JS_EVAL_FLAG_COMPILE_ONLY);
    CHECK(!JS_IsException(compiled));
    std::size_t size = 0;
    std::uint8_t* data =
        JS_WriteObject(ctx, &size, compiled, JS_WRITE_OBJ_BYTECODE);
    JS_FreeValue(ctx, compiled);
    CHECK(data != nullptr);
    if (data != nullptr) {
        std::vector<std::uint8_t> raw(data, data + size);
        js_free(ctx, data);
        std::vector<std::uint8_t> rejected;
        std::string product_error;
        CHECK(!capsid::bytecode::optimize(raw, &rejected, 0xffffffffu, false,
                                          &product_error));
        CHECK(product_error.find("not a module") != std::string::npos);

        std::vector<std::uint8_t> optimized;
        std::string error;
        CHECK(capsid::bytecode::optimize_classic_for_benchmark(
            raw, &optimized, 0xffffffffu, false, &error));
        JSValue loaded = JS_ReadObject(ctx, optimized.data(), optimized.size(),
                                       JS_READ_OBJ_BYTECODE);
        CHECK(!JS_IsException(loaded));
        JSValue result = JS_EvalFunction(ctx, loaded);
        CHECK(!JS_IsException(result));
        JS_FreeValue(ctx, result);
        JSValue global = JS_GetGlobalObject(ctx);
        JSValue value = JS_GetPropertyStr(ctx, global, "__r");
        std::int32_t actual = 0;
        CHECK(JS_ToInt32(ctx, &actual, value) == 0);
        CHECK(actual == 4950);
        JS_FreeValue(ctx, value);
        JS_FreeValue(ctx, global);
    }
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
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
// R1: BC27 ext loc-array fusion (ext ids 2/3).
//
// The fused ext reads its locals directly (n_pop=0), so its stack effect
// (+1 / +2) is exactly the window's; the verifier, the runtime's own BC27
// stack check, and the handlers all share the ext table. Targets strictly
// inside a window block fusion; a target at the window start is allowed
// (it lands on the fused ext, identical behavior).
// ---------------------------------------------------------------------------

#ifdef CAPSID_ENABLE_EXT_FUSION34
void test_ext34_fusion_goldens() {
    // id 2 (kPassExtFuse34): [get_loc8 4][get_loc8 5][get_array_el] ->
    // [OP_ext, 2, 4, 5]; BC27 version byte; ext round trip accepts the
    // emitted bundle; a second optimize() must fail closed.
    {
        Builder b;
        b.op(199); b.op(4);  // get_loc8 4
        b.op(199); b.op(5);  // get_loc8 5
        b.op(70);            // get_array_el
        b.op(41);            // return_undef
        b.stack_size = 3;
        b.finish(6);
        std::vector<std::uint8_t> out;
        std::string err;
        CHECK(capsid::bytecode::optimize(
            b.buf, &out, capsid::bytecode::kPassExtFuse34, false, &err));
        CHECK(out[0] == 27);  // BC_VERSION_EXT
        std::vector<std::uint8_t> code;
        CHECK(b.optimize_and_code(&code, &err,
                                  capsid::bytecode::kPassExtFuse34));
        const std::uint8_t expected[] = {252, 2, 4, 5, 41};
        CHECK(code.size() == sizeof(expected));
        CHECK(code.size() == sizeof(expected) &&
              std::memcmp(code.data(), expected, sizeof(expected)) == 0);
        // BC27 is not re-optimizable (no foldability consumer for ext
        // sites), and the I0 ext round trip still reads it.
        std::vector<std::uint8_t> again;
        std::string re_err;
        CHECK(!capsid::bytecode::optimize(out, &again, 0xffffffffu, false,
                                          &re_err));
        CHECK(re_err.find("not re-optimizable") != std::string::npos);
        std::string rt_err;
        CHECK(capsid::bytecode::ext_round_trip(out, &rt_err));
    }
    // id 3 (kPassExtFuse4): 3 get_loc8s + get_array_el -> id 3, payload
    // [leftover, object, index]; net +2.
    {
        Builder b;
        b.op(199); b.op(4);  // get_loc8 4 (leftover)
        b.op(199); b.op(5);  // get_loc8 5 (object)
        b.op(199); b.op(6);  // get_loc8 6 (index)
        b.op(70);            // get_array_el
        b.op(41);            // return_undef
        b.stack_size = 4;
        b.finish(7);
        std::vector<std::uint8_t> code;
        std::string err;
        CHECK(b.optimize_and_code(&code, &err, capsid::bytecode::kPassExtFuse4));
        const std::uint8_t expected[] = {252, 3, 4, 5, 6, 41};
        CHECK(code.size() == sizeof(expected));
        CHECK(code.size() == sizeof(expected) &&
              std::memcmp(code.data(), expected, sizeof(expected)) == 0);
    }
    // get_loc0_loc1 forms: the quickjs emitter coalesces get_loc(0)
    // get_loc(1) into one 2-slot read, so `a[i]` with a=loc0, i=loc1
    // is a 2-insn window. id 2 payload {0, 1}.
    {
        Builder b;
        b.op(202);   // get_loc0_loc1
        b.op(70);    // get_array_el
        b.op(41);    // return_undef
        b.stack_size = 2;
        b.finish(6);
        std::vector<std::uint8_t> code;
        std::string err;
        CHECK(b.optimize_and_code(&code, &err,
                                  capsid::bytecode::kPassExtFuse34));
        const std::uint8_t expected[] = {252, 2, 0, 1, 41};
        CHECK(code.size() == sizeof(expected));
        CHECK(code.size() == sizeof(expected) &&
              std::memcmp(code.data(), expected, sizeof(expected)) == 0);
    }
    // id 3 with a fused pair: [leftover loc][get_loc0_loc1][array] ->
    // payload {leftover, 0, 1}.
    {
        Builder b;
        b.op(199); b.op(4);  // get_loc8 4 (leftover)
        b.op(202);           // get_loc0_loc1 (object, index)
        b.op(70);            // get_array_el
        b.op(41);            // return_undef
        b.stack_size = 3;
        b.finish(6);
        std::vector<std::uint8_t> code;
        std::string err;
        CHECK(b.optimize_and_code(&code, &err, capsid::bytecode::kPassExtFuse4));
        const std::uint8_t expected[] = {252, 3, 4, 0, 1, 41};
        CHECK(code.size() == sizeof(expected));
        CHECK(code.size() == sizeof(expected) &&
              std::memcmp(code.data(), expected, sizeof(expected)) == 0);
    }
    // Mixed run: [get_loc0_loc1][get_loc8 5][array] is a 3-slot run ->
    // id 3 payload {0, 1, 5}.
    {
        Builder b;
        b.op(202);           // get_loc0_loc1 (leftover pair)
        b.op(199); b.op(5);  // get_loc8 5 (index)
        b.op(70);            // get_array_el
        b.op(41);            // return_undef
        b.stack_size = 3;
        b.finish(6);
        std::vector<std::uint8_t> code;
        std::string err;
        CHECK(b.optimize_and_code(&code, &err, capsid::bytecode::kPassExtFuse4));
        const std::uint8_t expected[] = {252, 3, 0, 1, 5, 41};
        CHECK(code.size() == sizeof(expected));
        CHECK(code.size() == sizeof(expected) &&
              std::memcmp(code.data(), expected, sizeof(expected)) == 0);
    }
    // Short-form local slots: the decoder leaves aux at 0 for
    // get_loc0..3, so the payload must encode op - get_loc0 (regression:
    // these used to be encoded as slot 0, corrupting the fused reads).
    // id 2: [get_loc0][get_loc1][array] -> {0, 1}.
    {
        Builder b;
        b.op(203);           // get_loc0 (object)
        b.op(204);           // get_loc1 (index)
        b.op(70);            // get_array_el
        b.op(41);            // return_undef
        b.stack_size = 2;
        b.finish(4);
        std::vector<std::uint8_t> code;
        std::string err;
        CHECK(b.optimize_and_code(&code, &err,
                                  capsid::bytecode::kPassExtFuse34));
        const std::uint8_t expected[] = {252, 2, 0, 1, 41};
        CHECK(code.size() == sizeof(expected));
        CHECK(code.size() == sizeof(expected) &&
              std::memcmp(code.data(), expected, sizeof(expected)) == 0);
    }
    // Regression: a pair followed by another pair used to write the second
    // pair past slots[3] while considering the four-slot prefix. The legal
    // pair suffix must still fuse, and the first pair must remain intact.
    {
        Builder b;
        b.op(202);           // get_loc0_loc1 (must remain)
        b.op(202);           // get_loc0_loc1 (object, index)
        b.op(70);            // get_array_el
        b.op(41);            // return_undef
        b.stack_size = 4;
        b.finish(4);
        std::vector<std::uint8_t> code;
        std::string err;
        CHECK(b.optimize_and_code(&code, &err,
                                  capsid::bytecode::kPassExtFuse34));
        const std::uint8_t expected[] = {202, 252, 2, 0, 1, 41};
        CHECK(code.size() == sizeof(expected));
        CHECK(code.size() == sizeof(expected) &&
              std::memcmp(code.data(), expected, sizeof(expected)) == 0);
    }
    // The same overflow was reachable after two singleton reads. Reject the
    // four-slot prefix without consuming it, then fuse the legal three-slot
    // suffix beginning at get_loc3.
    {
        Builder b;
        b.op(205);           // get_loc2 (must remain)
        b.op(206);           // get_loc3 (leftover)
        b.op(202);           // get_loc0_loc1 (object, index)
        b.op(70);            // get_array_el
        b.op(41);            // return_undef
        b.stack_size = 4;
        b.finish(4);
        std::vector<std::uint8_t> code;
        std::string err;
        const bool ok = b.optimize_and_code(
            &code, &err, capsid::bytecode::kPassExtFuse4);
        if (!ok) std::fprintf(stderr, "  overflow-suffix error: %s\n",
                              err.c_str());
        CHECK(ok);
        const std::uint8_t expected[] = {205, 252, 3, 3, 0, 1, 41};
        CHECK(code.size() == sizeof(expected));
        CHECK(code.size() == sizeof(expected) &&
              std::memcmp(code.data(), expected, sizeof(expected)) == 0);
    }
    // id 3: [get_loc0][get_loc1][get_loc2][array] -> {0, 1, 2}.
    {
        Builder b;
        b.op(203);           // get_loc0 (leftover)
        b.op(204);           // get_loc1 (object)
        b.op(205);           // get_loc2 (index)
        b.op(70);            // get_array_el
        b.op(41);            // return_undef
        b.stack_size = 3;
        b.finish(4);
        std::vector<std::uint8_t> code;
        std::string err;
        CHECK(b.optimize_and_code(&code, &err, capsid::bytecode::kPassExtFuse4));
        const std::uint8_t expected[] = {252, 3, 0, 1, 2, 41};
        CHECK(code.size() == sizeof(expected));
        CHECK(code.size() == sizeof(expected) &&
              std::memcmp(code.data(), expected, sizeof(expected)) == 0);
    }
    // Precedence: with both bits, the 4-insn window wins over the
    // 3-insn prefix.
    {
        Builder b;
        b.op(199); b.op(4);
        b.op(199); b.op(5);
        b.op(199); b.op(6);
        b.op(70);
        b.op(41);
        b.stack_size = 4;
        b.finish(7);
        std::vector<std::uint8_t> code;
        std::string err;
        CHECK(b.optimize_and_code(
            &code, &err, capsid::bytecode::kPassExtFuse34 |
                             capsid::bytecode::kPassExtFuse4));
        const std::uint8_t expected[] = {252, 3, 4, 5, 6, 41};
        CHECK(code.size() == sizeof(expected));
        CHECK(code.size() == sizeof(expected) &&
              std::memcmp(code.data(), expected, sizeof(expected)) == 0);
    }
    // OFF (deployed kPassAll = 0x7f): byte-identical BC26 output.
    {
        Builder b;
        b.op(199); b.op(4);
        b.op(199); b.op(5);
        b.op(70);
        b.op(41);
        b.stack_size = 3;
        b.finish(6);
        std::vector<std::uint8_t> out;
        std::string err;
        CHECK(capsid::bytecode::optimize(
            b.buf, &out, capsid::bytecode::kPassAll, false, &err));
        CHECK(out == b.buf);
        CHECK(out[0] == 26);
    }
    // A jump strictly inside a window blocks fusion. Loop back-edge
    // landing on the second get_loc8 of an id2 window (heights: entry
    // h0 -> loc(h1) -> loc(h2) -> array(h1); the back-edge pops one,
    // landing at h1, consistent with the fallthrough).
    {
        Builder b;
        b.op(199); b.op(4);       // get_loc8 4  (window start)
        b.op(199); b.op(5);       // get_loc8 5  (loop target, mid-window)
        b.op(70);                 // get_array_el
        b.op(14);                 // drop
        b.op(10);                 // push_true
        b.op(10);                 // push_true
        b.op(241); b.op(0xF9);    // if_true8 -7 -> index 1
        b.op(41);                 // return_undef
        b.stack_size = 2;
        b.finish(6);
        std::vector<std::uint8_t> out;
        std::string err;
        CHECK(capsid::bytecode::optimize(
            b.buf, &out, capsid::bytecode::kPassExtFuse34 |
                             capsid::bytecode::kPassExtFuse4, false, &err));
        if (out != b.buf) {
            std::fprintf(stderr, "  in:  ");
            for (std::size_t k = 0; k < b.buf.size(); k++)
                std::fprintf(stderr, "%02x ", b.buf[k]);
            std::fprintf(stderr, "\n  out: ");
            for (std::size_t k = 0; k < out.size(); k++)
                std::fprintf(stderr, "%02x ", out[k]);
            std::fprintf(stderr, "\n  err: %s\n", err.c_str());
        }
        CHECK(out == b.buf);
        CHECK(out[0] == 26);
    }
    // A target at the window start is allowed and fuses: the loop
    // back-edge lands on the fused ext, whose behavior is identical to
    // the window's at any height.
    {
        Builder b;
        b.op(199); b.op(4);       // get_loc8 4  (window start, loop target)
        b.op(199); b.op(5);       // get_loc8 5
        b.op(70);                 // get_array_el
        b.op(14);                 // drop
        b.op(10);                 // push_true
        b.op(241); b.op(0xF8);    // if_true8 -8 -> index 0
        b.op(41);                 // return_undef
        b.stack_size = 2;
        b.finish(6);
        std::vector<std::uint8_t> code;
        std::string err;
        CHECK(b.optimize_and_code(
            &code, &err, capsid::bytecode::kPassExtFuse34 |
                             capsid::bytecode::kPassExtFuse4));
        const std::uint8_t expected[] = {252, 2, 4, 5, 14, 10, 241, 0xF9, 41};
        CHECK(code.size() == sizeof(expected));
        CHECK(code.size() == sizeof(expected) &&
              std::memcmp(code.data(), expected, sizeof(expected)) == 0);
    }
    // Determinism: the same input optimizes byte-identically twice.
    {
        Builder b;
        b.op(199); b.op(4);
        b.op(199); b.op(5);
        b.op(70);
        b.op(41);
        b.stack_size = 3;
        b.finish(6);
        std::vector<std::uint8_t> o1, o2;
        std::string e1, e2;
        CHECK(capsid::bytecode::optimize(
            b.buf, &o1, capsid::bytecode::kPassExtFuse34, false, &e1));
        CHECK(capsid::bytecode::optimize(
            b.buf, &o2, capsid::bytecode::kPassExtFuse34, false, &e2));
        CHECK(o1 == o2);
    }
}

// ---------------------------------------------------------------------------
// Part B: full round-trip through the real runtime.
// ---------------------------------------------------------------------------

#endif

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

void test_p11_semantics() {
    // P11 differential: copy chains, dead-store elimination, and the
    // barriers must keep raw and optimized execution identical.
    JSRuntime* rt = JS_NewRuntime();
    CHECK(rt != nullptr);
    JSContext* ctx = JS_NewContext(rt);
    CHECK(ctx != nullptr);
    if (ctx == nullptr) return;

    const char* sources[] = {
        // Copy chain with dead intermediates.
        "let a = 5, b = a, c = b, d = c; globalThis.__r = d;",
        // Dead store: b = a is overwritten before any read.
        "let a = 5, b = a; b = 3; globalThis.__r = a + b;",
        // Loop-carried copy with a real read keeps the store alive.
        "let x = 1; for (let i = 0; i < 100; i++) { const t = x; x = t; }"
        "globalThis.__r = x;",
        // Barrier (closure creation) between store and read.
        "let a = 1, b = a, c = 0; const g = () => 0; c = b;"
        "globalThis.__r = a + c;",
        // Alias read renamed across a loop pre-header.
        "let base = 10, alias = base; let acc = 0;"
        "for (let i = 0; i < 100; i++) acc += alias;"
        "globalThis.__r = acc;",
        // Reverse-alias invalidation: y copies x, then x is overwritten
        // from s; the read of y must see the old x (1), not s (5).
        "let x = 1, s = 5, y = x; x = s; globalThis.__r = y;",
    };
    for (const char* src : sources) {
        RtResult base = run_roundtrip(ctx, src, "p11.js", false);
        RtResult opt = run_roundtrip(ctx, src, "p11.js", true);
        CHECK(base.ok == opt.ok);
        if (base.ok) {
            CHECK(base.value == opt.value);
        }
        if (base.value != opt.value || base.ok != opt.ok) {
            std::fprintf(stderr, "  source: %s\n", src);
            std::fprintf(stderr, "    base=%s/%s opt=%s/%s\n",
                         base.ok ? "ok" : "fail", base.value.c_str(),
                         opt.ok ? "ok" : "fail", opt.value.c_str());
        }
    }
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
}

void test_p14_semantics() {
    // P14-specific differential: the literal folds (and their gates)
    // must keep raw and optimized execution identical. Covers the
    // object/array element folds, atom/cpool-valued fields, and the
    // mutation barriers (put_field, calls, put_array_el, read-modify-
    // write, out-of-range indexes, nested values).
    JSRuntime* rt = JS_NewRuntime();
    CHECK(rt != nullptr);
    JSContext* ctx = JS_NewContext(rt);
    CHECK(ctx != nullptr);
    if (ctx == nullptr) return;

    const char* sources[] = {
        // Object literal folds.
        "const o = { a: 1, b: 2 }; globalThis.__r = o.a + o.b * 10;",
        // Array literal folds.
        "const a = [10, 20, 30]; globalThis.__r = a[0] + a[1] + a[2];",
        // Atom/cpool-valued fields.
        "const o = { s: 'ab', f: 1.5, n: 3 };"
        "globalThis.__r = o.s + o.f + o.n + o.zzz;",
        // Mutation via put_field must defeat the fold.
        "const o = { a: 1 }; o.a = 5; globalThis.__r = o.a;",
        // Mutation via a call must defeat the fold.
        "const o = { b: 2 }; const mut = (x) => { x.b = 99; }; mut(o);"
        "globalThis.__r = o.b;",
        // Array element write must defeat the element fold.
        "const a = [1, 2, 3]; a[0] = 7; globalThis.__r = a[0];",
        // Array mutation through a call must defeat the fold.
        "const a = [1, 2, 3]; const mut = (x) => { x[2] = 42; }; mut(a);"
        "globalThis.__r = a[2];",
        // Read-modify-write must keep the folded read consistent.
        "const o = { a: 1 }; o.a += 1; globalThis.__r = o.a;",
        "const a = [1, 2, 3]; a[0] += 1; a[1]++; globalThis.__r = a[0] + a[1];",
        // Out-of-range and negative indexes read undefined, never fold.
        "const a = [1, 2]; globalThis.__r = a[5] === undefined && a[-1] === undefined;",
        // Nested (object-valued) fields and elements never fold.
        "const n = { inner: { q: 7 } }; const m = [{ k: 1 }, 2];"
        "globalThis.__r = n.inner.q + m[0].k + m[1];",
        // Field on a function-scope literal used across a loop.
        "let acc = 0; const o = { x: 1, y: 2, z: 3 };"
        "for (let i = 0; i < 100; i++) acc += o.x + o.y + o.z;"
        "globalThis.__r = acc;",
        // Same with an array literal.
        "let acc = 0; const a = [10, 20, 30];"
        "for (let i = 0; i < 100; i++) acc += a[0] + a[1] + a[2];"
        "globalThis.__r = acc;",
    };
    for (const char* src : sources) {
        RtResult base = run_roundtrip(ctx, src, "p14.js", false);
        RtResult opt = run_roundtrip(ctx, src, "p14.js", true);
        CHECK(base.ok == opt.ok);
        if (base.ok) {
            CHECK(base.value == opt.value);
        }
        if (base.value != opt.value || base.ok != opt.ok) {
            std::fprintf(stderr, "  source: %s\n", src);
            std::fprintf(stderr, "    base=%s/%s opt=%s/%s\n",
                         base.ok ? "ok" : "fail", base.value.c_str(),
                         opt.ok ? "ok" : "fail", opt.value.c_str());
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

// R1 ext semantics: f's body is an id2 window (get_loc a; get_loc i;
// get_array_el), g's an id3 window (get_loc k; get_loc a; get_loc i;
// get_array_el). With 0xffffffff both bits are on, so the optimized run
// executes the ext handlers; every branch — fast path, non-int index
// slow path, fast miss, leftover form, and the null exception — must
// agree with the unoptimized control run.
#ifdef CAPSID_ENABLE_EXT_FUSION34
void test_ext34_semantics() {
    JSRuntime* rt = JS_NewRuntime();
    CHECK(rt != nullptr);
    JSContext* ctx = JS_NewContext(rt);
    CHECK(ctx != nullptr);
    if (ctx == nullptr) return;

    const char* src =
        "function f(a, i) { return a[i]; }\n"
        "function g(a, i) { const k = 7; return k + a[i]; }\n"
        // Short-form local slots (regression: get_loc1/3 used to be
        // encoded as slot 0). Two emitter facts shape these tests:
        // (a) `let` reads emit get_loc_check (TDZ), which the matcher
        // excludes by design, so `let` never fuses; (b) a `var`
        // initialized to a constant is constant-eliminated by the
        // emitter (uses become push_<const>, the store disappears),
        // so a literal-initialized `var` never forms a window either.
        // Non-constant initializers (from args) keep the locals as
        // real slot reads. With `var` the reads are plain get_loc0..3,
        // giving windows [0x00, 0x01] (h), [0x80|0, 0x03] (h3 — the
        // exact get_loc3 regression), and the id3 [0x00, 0x80|0,
        // 0x01] (k) — the short-form shapes that exercise read_slots'
        // opcode-encoded slot convention end to end.
        "function h(o, n) { var q = o; var i = n; return q[i]; }\n"
        "function h3(o, n) { var a0=o; var a1=n; var a2=n+1; var a3=n+2;"
        " return o[a3]; }\n"
        "function k(o, n) { var t = n; var i = n + 1;"
        " return t + o[i]; }\n"
        "let s = '';\n"
        "s += f([1, 2, 3], 1) + ',';\n"   // id2 fast path
        "s += f([1, 2, 3], 5) + ',';\n"   // id2 fast miss -> undefined
        "s += f({ x: 9 }, 'x') + ',';\n"  // id2 non-int index slow path
        "s += f([1, 2, 3], '1') + ',';\n" // id2 coerced slow path
        "let hits = 0; const accessor = { get x() { hits++; return 13; } };\n"
        "s += f(accessor, 'x') + ':' + hits + ',';\n" // getter slow path
        "s += g([10, 20], 1) + ',';\n"    // id3 fast path
        "s += g({ x: 5 }, 'x') + ',';\n"  // id3 slow path
        "s += g([10, 20], 9) + ',';\n"    // id3 miss -> 7 + undefined
        "s += h([10, 11, 12, 13, 14], 4) + ',';\n"  // get_loc1 idx: o[4]
        "s += h3([1, 2, 3, 4, 5], 2) + ',';\n"      // get_loc3 idx: o[4]
        "s += h3({0:'a',1:'b',2:'c',3:'d',4:'e'}, 2) + ',';\n" // slow path
        "s += k([8, 9, 10], 1) + ',';\n"  // id3 short-form keep+idx: 11
        "try { f(null, 0); s += 'no-throw'; }\n"
        "catch (e) { s += e.name + ':' + e.message; }\n"
        "globalThis.__r = s;\n";
    RtResult base = run_roundtrip(ctx, src, "ext34.js", false);
    RtResult opt = run_roundtrip(ctx, src, "ext34.js", true);
    CHECK(base.ok);
    CHECK(opt.ok);
    // Prove the optimized run really executed the ext handlers: the
    // real compiler output must have fused and therefore emit BC27
    // (the round trip above would have failed otherwise — the BC27
    // reader is the only path that accepts it).
    {
        JSValue module = JS_Eval(ctx, src, std::strlen(src), "ext34.js",
                                 JS_EVAL_TYPE_MODULE |
                                     JS_EVAL_FLAG_COMPILE_ONLY);
        CHECK(!JS_IsException(module));
        if (!JS_IsException(module)) {
            std::size_t size = 0;
            std::uint8_t* data = JS_WriteObject(
                ctx, &size, module, JS_WRITE_OBJ_BYTECODE);
            JS_FreeValue(ctx, module);
            CHECK(data != nullptr);
            if (data != nullptr) {
                std::vector<std::uint8_t> buf(data, data + size);
                js_free(ctx, data);
                std::vector<std::uint8_t> opt2;
                std::string err;
                bool ok2 = capsid::bytecode::optimize(buf, &opt2, 0xffffffffu,
                                                      false, &err);
                if (!ok2 || opt2[0] != 27) {
                    std::fprintf(stderr, "  optimize ok=%d err=%s\n", ok2,
                                 err.c_str());
                    std::fprintf(stderr, "  raw: ");
                    for (std::size_t k = 0; k < size && k < 260; k++)
                        std::fprintf(stderr, "%02x ", buf[k]);
                    std::fprintf(stderr, "\n");
                }
                CHECK(ok2);
                CHECK(opt2[0] == 27);  // BC_VERSION_EXT
            }
        }
    }
    CHECK(base.value == opt.value);
    if (base.value != opt.value) {
        std::fprintf(stderr, "  base: %s\n  opt:  %s\n",
                     base.value.c_str(), opt.value.c_str());
    }
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
}

// The slow path can call user code and throw. Besides value/exception
// equivalence, lock the exact backtrace so the ext handler keeps the same
// sf->cur_pc convention as the original get_array_el instruction.
void test_ext34_exception_stack() {
    JSRuntime* rt = JS_NewRuntime();
    CHECK(rt != nullptr);
    JSContext* ctx = JS_NewContext(rt);
    CHECK(ctx != nullptr);
    if (ctx == nullptr) return;

    const char* src =
        "function read(o, k) {\n"
        "  return o[k];\n"
        "}\n"
        "const o = { get x() {\n"
        "  throw new Error('getter boom');\n"
        "} };\n"
        "read(o, 'x');\n";
    RtResult base = run_roundtrip(ctx, src, "ext34-stack.js", false);
    RtResult opt = run_roundtrip(ctx, src, "ext34-stack.js", true);
    CHECK(!base.ok);
    CHECK(!opt.ok);
    CHECK(base.value == opt.value);
    CHECK(base.stack == opt.stack);
    if (base.stack != opt.stack) {
        std::fprintf(stderr, "  base stack: %s\n", base.stack.c_str());
        std::fprintf(stderr, "  opt  stack: %s\n", opt.stack.c_str());
    }
    CHECK(base.stack.find("ext34-stack.js:5") != std::string::npos);
    CHECK(base.stack.find("ext34-stack.js:1") != std::string::npos);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
}
#endif

}  // namespace

int main() {
    test_peephole_goldens();
    test_p16_dead_store_goldens();
    test_tier3_lane1_goldens();
    test_p18_readonly_capture_goldens();
    test_fail_closed_matrix();
    test_cpool_kept();
    test_p2_gates();
    test_p11_folds();
    test_p11_gates();
    test_p14_folds();
    test_p14_gates();
    test_debug_block_remap();
    test_classic_benchmark_boundary();
    test_determinism_and_idempotence();
    test_roundtrip_values();
    test_p14_semantics();
    test_p11_semantics();
    test_roundtrip_exception_lines();
#ifdef CAPSID_ENABLE_EXT_FUSION34
    test_ext34_fusion_goldens();
    test_ext34_semantics();
    test_ext34_exception_stack();
#endif
    if (g_failures != 0) {
        std::fprintf(stderr, "test_bytecode_optimizer: %d failure(s)\n",
                     g_failures);
        return 1;
    }
    std::fprintf(stderr, "test_bytecode_optimizer: all checks passed\n");
    return 0;
}
