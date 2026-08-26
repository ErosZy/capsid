// I1 SSA gate (docs/quickjs-optimization.md §2/§6):
// the analyze-only full-stack SSA. Part A drives decode -> CFG -> verify
// -> ssa_analyze_function on hand-built canonical BC26 function blobs and
// asserts the exact analysis results: block parameters (phi), the value
// lattice with small-int folding (overflow -> FLOAT64, unknown int32
// arithmetic -> NUMBER), the ordered
// world token (backedges excluded from the entry join), exception
// successors (catch markers, handler entry, region-end drops), and the
// refcount ownership census. Part B runs the bundle-level ssa_round_trip
// walker on synthetic .qjsb bundles (rejected coverage is counted, never
// skipped). Part C compiles representative module sources through the
// real quickjs-ng compiler and asserts ZERO rejected functions / insns —
// the mandatory 0-rejection gate over the corpus.
//
// Byte values are the serialized opcode space (quickjs-opcode.h physical
// order, temps excluded): push_i32=1, drop=14, dup=17, return_undef=41,
// put_field=66, catch=107, gosub=108, ret=109, nip_catch=110, add=156,
// inc_loc=146, not=148, and=161, push_0=186, push_1=187, push_i8=194,
// get_arg=90, get_loc0=203,
// put_loc0=207, put_loc1=208, set_loc0=211, get_loc0_loc1=202,
// if_false8=240, if_true8=241, goto8=242, OP_COUNT=252. Jump targets:
// pc + size + signed aux (catch/gosub aux is the 4-byte diff at pc+1;
// if_true8/goto8 aux is the u8 at pc+1).

#include "bytecode_rewriter/bytecode_rewriter.h"
#include "bytecode_rewriter/ir/cfg.h"
#include "bytecode_rewriter/ir/ssa.h"
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

// Decode + CFG + verify + SSA on a synthetic function blob. Returns
// false (fail-closed, with `error`) when the function is rejected.
bool analyze_blob(const std::vector<std::uint8_t>& code,
                  std::uint32_t stack_size, ir::SsaFunc* out,
                  std::string* err) {
    ir::FuncInfo fi = zero_fi(static_cast<std::uint32_t>(code.size()));
    std::vector<ir::Insn> insns;
    if (!ir::decode_function(code.data(), code.size(), code.data(), fi,
                             &insns, err)) {
        return false;
    }
    if (insns.empty()) return false;
    ir::Cfg cfg;
    if (!ir::build_cfg(insns, &cfg, err)) return false;
    cfg.recorded_stack_size = stack_size;
    if (!ir::verify_cfg(cfg, err)) return false;
    return ir::ssa_analyze_function(cfg, out, err);
}

// ---------------------------------------------------------------------------
// Part A: the synthetic matrix.
// ---------------------------------------------------------------------------

// a1: straight-line value flow — dup/drop, small-int fold, world token,
// ownership census (dup marks its source DUPLICATED).
void test_a1_fold_dup_drop() {
    // push_i8 50; dup; drop; push_i8 60; push_i8 40; add; return_undef.
    // (push_i8 immediates are signed int8 — the constants must fit.)
    const std::vector<std::uint8_t> code = {194, 50,  17,  14,  194, 60,
                                            194, 40,  156, 41};
    ir::SsaFunc ssa;
    std::string err;
    CHECK(analyze_blob(code, 3, &ssa, &err));
    if (err.empty() && ssa.blocks.empty()) return;
    CHECK(ssa.param_count == 0);
    CHECK(ssa.value_count == 5);
    // v0 push 50, v1 dup copy, v2 60, v3 40, v4 add = 100.
    CHECK(ssa.lattice[4] == ir::Lattice::INT32);
    CHECK(ssa.has_imm[4] != 0 && ssa.imm[4] == 100);
    // The final return_undef carries the token: two world touches
    // (add is CALL, return_undef is TERMINAL).
    const ir::SsaBlock& b = ssa.blocks[0];
    CHECK(b.nodes.size() == 7);
    CHECK(b.nodes[6].token_out == 2);
    CHECK(ssa.ownership[0] == ir::Ownership::DUPLICATED);  // dup source
    // dup (n_pop 1, n_push 2) re-pushes the popped value as the kept
    // result BELOW a fresh copy ([kept, fresh], matching the shuffle
    // table's src {0,0} / fresh {0,1}); the drop pops the COPY (v1) —
    // the original v0 rides out of the block holding its extra ref.
    CHECK(ssa.ownership[1] == ir::Ownership::CONSUMED);
}

// a2: INT32 fold overflow (INT32_MAX + 1) is a proven FLOAT64.
void test_a2_overflow() {
    // push_i32 2147483647; dup; push_i8 1; add; return_undef.
    const std::vector<std::uint8_t> code = {1, 0xff, 0xff, 0xff, 0x7f,
                                            17, 194,  1,    156,  41};
    ir::SsaFunc ssa;
    std::string err;
    CHECK(analyze_blob(code, 3, &ssa, &err));
    if (err.empty() && ssa.blocks.empty()) return;
    CHECK(ssa.value_count == 4);
    CHECK(ssa.lattice[3] == ir::Lattice::FLOAT64);
    CHECK(ssa.has_imm[3] == 0);
}

// a2b: bitwise operations cannot claim INT32 for unknown values: Number
// inputs return int32, but the same operators have normal BigInt results.
void test_a2b_unknown_bitwise() {
    {
        // get_arg 0; not; return_undef.
        const std::vector<std::uint8_t> code = {90, 0, 0, 148, 41};
        ir::SsaFunc ssa;
        std::string err;
        CHECK(analyze_blob(code, 1, &ssa, &err));
        if (!ssa.blocks.empty()) {
            const std::vector<ir::SsaNode>& nodes = ssa.blocks[0].nodes;
            CHECK(nodes.size() >= 2 && !nodes[nodes.size() - 2].results.empty());
            if (nodes.size() >= 2 && !nodes[nodes.size() - 2].results.empty()) {
                const std::uint32_t result =
                    nodes[nodes.size() - 2].results[0];
                CHECK(ssa.lattice[result] == ir::Lattice::UNKNOWN);
            }
        }
    }
    {
        // get_arg 0; get_arg 0; and; return_undef.
        const std::vector<std::uint8_t> code = {90, 0, 0, 90, 0, 0,
                                                161, 41};
        ir::SsaFunc ssa;
        std::string err;
        CHECK(analyze_blob(code, 2, &ssa, &err));
        if (!ssa.blocks.empty()) {
            const std::vector<ir::SsaNode>& nodes = ssa.blocks[0].nodes;
            CHECK(nodes.size() >= 2 && !nodes[nodes.size() - 2].results.empty());
            if (nodes.size() >= 2 && !nodes[nodes.size() - 2].results.empty()) {
                const std::uint32_t result =
                    nodes[nodes.size() - 2].results[0];
                CHECK(ssa.lattice[result] == ir::Lattice::UNKNOWN);
            }
        }
    }
}

// a3: two-way join — the merge block takes one parameter per live
// stack position: p0 joins the same value on both paths (imm kept),
// p1 joins two different immediates (INT32, imm dropped), the add
// folds nothing (no imm on p1). Entry token is the max over the pred
// edges.
void test_a3_join_params() {
    // push_0; push_1; if_true8 +5; push_i8 5; goto8 +3; [B1] push_i8 3;
    // [B2] add; return_undef.
    // Branch targets are pc + 1 + diff (cfg.cc:272): if_true8 at byte 2
    // aux 5 -> byte 8 (B1), goto8 at byte 6 aux 3 -> byte 10 (B2).
    // The conditional's fallthrough is NOT a leader (cfg.cc: leaders are
    // entry, jump targets, post-gosub), so the push_i8 5 / goto8 tail
    // runs inside block 0: 3 blocks total, the merge is block 2.
    const std::vector<std::uint8_t> code = {186, 187, 241, 5,  194, 5,
                                            242, 3,   194, 3,  156, 41};
    ir::SsaFunc ssa;
    std::string err;
    CHECK(analyze_blob(code, 2, &ssa, &err));
    if (err.empty() && ssa.blocks.empty()) return;
    CHECK(ssa.blocks.size() == 3);
    CHECK(ssa.param_count == 2);
    CHECK(ssa.value_count == 7);
    const ir::SsaBlock& merge = ssa.blocks[2];
    CHECK(merge.entry_stack.size() == 2);
    // p0 joins push_0 (both paths) with push_0: INT32, imm 0 kept.
    // p1 joins imm 5 (goto8 snap) and imm 3 (B1's push): INT32, imm
    // dropped.
    const uint32_t p0 = merge.entry_stack[0];
    const uint32_t p1 = merge.entry_stack[1];
    CHECK(ssa.lattice[p0] == ir::Lattice::INT32);
    CHECK(ssa.has_imm[p0] != 0 && ssa.imm[p0] == 0);
    CHECK(ssa.lattice[p1] == ir::Lattice::INT32);
    CHECK(ssa.has_imm[p1] == 0);
    // The runtime add may overflow even though both input tags are INT32;
    // without two known immediates the only sound result is NUMBER.
    const uint32_t add_r = merge.nodes[0].results[0];
    CHECK(ssa.lattice[add_r] == ir::Lattice::NUMBER);
    CHECK(ssa.has_imm[add_r] == 0);
    // Merge entry token: max(goto8 edge (blk0: if_true8 CONTROL +1,
    // goto8 CONTROL +1 = 2), B1 fallthrough edge (entry 1, no effect)
    // = 1).
    CHECK(merge.token_in == 2);
    CHECK(merge.nodes[0].token_out == 3);  // add: CALL +1
    // The branch condition (push_1) was consumed; the join parameters
    // were released by the add.
    CHECK(ssa.ownership[1] == ir::Ownership::CONSUMED);
    CHECK(ssa.ownership[p0] == ir::Ownership::CONSUMED);
    CHECK(ssa.ownership[p1] == ir::Ownership::CONSUMED);
}

// a4: loop backedge — the entry token excludes the backedge edge (the
// loop body's own effects would otherwise grow it unboundedly); the
// loop-carried parameters join the header values and the backedge
// values; put_field (CALL) consumes the object and the stored value.
void test_a4_loop_token() {
    // push_1; push_1; goto8 +1; [B1] dup; put_field atom 0;
    // push_i8 1; goto8 -9 (backedge to B1).
    // Branch targets are pc + 1 + diff: the first goto8 at byte 2 aux 1
    // -> byte 4 (dup), the backedge at byte 12 aux -9 -> byte 4.
    const std::vector<std::uint8_t> code = {187, 187, 242, 1, 17, 66,
                                            0,   0,   0,   0, 194, 1,
                                            242, 0xf7};
    ir::SsaFunc ssa;
    std::string err;
    // dup (n_pop 1, n_push 2) brings the loop block to height 3.
    CHECK(analyze_blob(code, 3, &ssa, &err));
    if (err.empty() && ssa.blocks.empty()) return;
    CHECK(ssa.blocks.size() == 2);
    CHECK(ssa.param_count == 2);
    CHECK(ssa.value_count == 6);
    // v0/v1 = B0's pushes; v2 = p0, v3 = p1; v4 = dup copy; v5 = the
    // loop-carried push_i8 1.
    // Entry token of the loop block: only the acyclic pred (B0's two
    // PURE pushes + goto8 CONTROL = 1) — the backedge is excluded.
    CHECK(ssa.blocks[1].token_in == 1);
    // put_field (CALL) leaves token 2; the trailing goto8 (CONTROL)
    // leaves token 3.
    CHECK(ssa.blocks[1].nodes[1].token_out == 2);
    CHECK(ssa.blocks[1].nodes[3].token_out == 3);
    // p0 joins push_1 (imm 1) with the backedge's p0 (itself):
    // imm kept. p1 joins push_1 (imm 1) with the backedge's p1
    // (itself): imm kept too — the backedge snap is TALLER than the
    // entry (the dup re-push), so both entry positions are covered.
    CHECK(ssa.lattice[2] == ir::Lattice::INT32);
    CHECK(ssa.has_imm[2] != 0 && ssa.imm[2] == 1);
    CHECK(ssa.lattice[3] == ir::Lattice::INT32);
    CHECK(ssa.has_imm[3] != 0 && ssa.imm[3] == 1);
    // put_field pops the dup copy and the kept p1 (the interpreter's
    // sp[-1]/sp[-2]) and releases both; p0 sits at the bottom of the
    // loop body and is never popped — it is born at the join and
    // still owned at the end. The dup marks its source (p1)
    // DUPLICATED.
    CHECK(ssa.ownership[2] == ir::Ownership::OWNED);
    CHECK(ssa.ownership[4] == ir::Ownership::CONSUMED);
    CHECK(ssa.ownership[3] == ir::Ownership::DUPLICATED);
}

// a5: canonical try/catch/finally — the catch marker's CATCH edge
// carries the pre-push handler stack; the finally block enters via the
// gosub snapshot; the marker drop closes the region (the following
// return no longer delivers into the handler); the handler's entry
// token joins the CATCH edge snapshot with the token injected at the
// throwing instruction.
void test_a5_try_catch() {
    // push_0; gosub +17; [B1] catch +13; push_i8 1; push_i8 2; add;
    // drop; drop; return_undef; [B2 finally] ret; [B3 handler]
    // nip_catch; return_undef.
    const std::vector<std::uint8_t> code = {186, 108, 17, 0, 0, 0,
                                            107, 13,  0,  0, 0, 194,
                                            1,   194, 2,  156, 14, 14,
                                            41,  109, 110, 41};
    ir::SsaFunc ssa;
    std::string err;
    CHECK(analyze_blob(code, 5, &ssa, &err));
    if (err.empty() && ssa.blocks.empty()) return;
    CHECK(ssa.blocks.size() == 4);
    // v0 push_0, v1 gosub ret-addr phantom, v2 catch marker, v3/v4 the
    // pushes, v5 the add result.
    CHECK(ssa.value_count == 6);
    // B0: push_0, gosub (CONTROL +1): the fallthrough snap carries
    // [v0]; the ret-addr phantom rides the gosub edge into B2.
    CHECK(ssa.blocks[0].nodes.size() == 2);
    // B1: catch pushes the marker; the add is the throwing node: its
    // successor is the handler block (3); token 1 (entry) + catch
    // (CONTROL) = 2, + add (CALL) = 3.
    CHECK(ssa.blocks[1].token_in == 1);
    const ir::SsaNode& add = ssa.blocks[1].nodes[3];
    CHECK(add.exc_succ == 3);
    CHECK(add.token_out == 3);
    CHECK(ssa.lattice[add.results[0]] == ir::Lattice::INT32);
    CHECK(ssa.has_imm[add.results[0]] != 0 &&
          ssa.imm[add.results[0]] == 3);
    // nodes[4] drops the add result; nodes[5] drops the catch marker —
    // the region-end pop, after which return_undef (nodes[6]) no
    // longer delivers into the handler.
    CHECK(ssa.blocks[1].nodes.size() == 7);
    // The marker is BORROWED; the add result was consumed by the drop.
    CHECK(ssa.ownership[2] == ir::Ownership::BORROWED);
    CHECK(ssa.ownership[5] == ir::Ownership::CONSUMED);
    // B2 (finally): entered via the gosub snapshot; ret is TERMINAL.
    CHECK(ssa.blocks[2].nodes[0].token_out == 2);
    // B3 (handler): entry token = max(CATCH snap 2, injected 3) = 3.
    CHECK(ssa.blocks[3].token_in == 3);
}

// a6: try body with an inner branch — the marker rides both paths into
// the merge, whose parameter for that position is born a BORROWED
// sentinel (all incoming values are sentinels: the all-sentinel join
// flip); the merge drop closes the region without an ownership
// violation. The handler enters via the CATCH edge with the pre-push
// stack.
void test_a6_try_join_marker() {
    // push_0; catch +17; push_1; if_false8 +6; push_i8 1; drop; goto8
    // +4; [B1] push_i8 2; drop; [B2 merge] drop; return_undef;
    // [B3 handler] drop; return_undef.
    // Branch targets are pc + 1 + diff: if_false8 at byte 7 aux 6 ->
    // byte 14 (B1), goto8 at byte 12 aux 4 -> byte 17 (B2). The
    // conditional's fallthrough is NOT a leader (cfg.cc), so the
    // push_i8 1 / drop / goto8 tail runs inside block 0: 4 blocks
    // total, merge is block 2, handler is block 3.
    const std::vector<std::uint8_t> code = {186, 107, 17, 0, 0, 0,
                                            187, 240, 6,  194, 1, 14,
                                            242, 4,   194, 2,  14, 14,
                                            41,  14,  41};
    ir::SsaFunc ssa;
    std::string err;
    CHECK(analyze_blob(code, 3, &ssa, &err));
    if (err.empty() && ssa.blocks.empty()) return;
    CHECK(ssa.blocks.size() == 4);
    CHECK(ssa.param_count == 2);
    CHECK(ssa.value_count == 7);
    // Blocks by code order: 2 = the merge (B2), 3 = the handler (B3).
    const ir::SsaBlock& merge = ssa.blocks[2];
    const ir::SsaBlock& handler = ssa.blocks[3];
    CHECK(merge.entry_stack.size() == 2);
    CHECK(handler.entry_stack.size() == 2);
    // p0 (the pushed value): joins push_0 with push_0 — the imm
    // survives. p1 (the marker position): all incoming values are
    // sentinels -> born BORROWED (Fix D: without the all-sentinel
    // flip the merge's region-end drop would release a non-sentinel
    // at refs 0 and reject the function).
    const uint32_t p0 = merge.entry_stack[0];
    const uint32_t p1 = merge.entry_stack[1];
    CHECK(ssa.lattice[p0] == ir::Lattice::INT32);
    CHECK(ssa.has_imm[p0] != 0 && ssa.imm[p0] == 0);
    CHECK(ssa.lattice[p1] == ir::Lattice::UNKNOWN);
    CHECK(ssa.sentinel[p1] != 0);
    CHECK(ssa.ownership[p1] == ir::Ownership::BORROWED);
    CHECK(ssa.is_param[p1] != 0);
    // The merge token: max(blk0 goto8 edge (blk0 exit 3), blk1 fall
    // edge (entry 2) = 2). The region-end drop is PURE; the return is
    // TERMINAL.
    CHECK(merge.token_in == 3);
    CHECK(merge.nodes[0].token_out == 3);
    CHECK(merge.nodes[1].token_out == 4);
    // The branch condition was consumed by if_false8.
    CHECK(ssa.ownership[2] == ir::Ownership::CONSUMED);
    // The handler: CATCH snap token 1 (post-catch), no injections.
    CHECK(handler.token_in == 1);
    // The handler's entry stack carries the pre-push values and the
    // marker (id 1) — a BORROWED sentinel.
    CHECK(handler.entry_stack[1] == 1);
    CHECK(ssa.ownership[1] == ir::Ownership::BORROWED);
}

// a7: set_loc keeps the value on the stack and births a fresh slot
// dup (the stored value is copied, not moved) — no false ownership
// violation when a later get_loc reads the slot (Fix A: the trailing
// release of the overwritten slot is a no-op on the sentinel).
void test_a7_set_loc() {
    // push_i8 5; set_loc0; get_loc0; add; return_undef.
    const std::vector<std::uint8_t> code = {194, 5, 211, 203, 156, 41};
    ir::SsaFunc ssa;
    std::string err;
    CHECK(analyze_blob(code, 2, &ssa, &err));
    if (err.empty() && ssa.blocks.empty()) return;
    // v0 = loc0 sentinel; v1 = push 5; v2 = the slot dup; v3 = get_loc
    // copy; v4 = add (5 + 5).
    CHECK(ssa.sentinel[0] != 0);
    CHECK(ssa.lattice[4] == ir::Lattice::INT32);
    CHECK(ssa.has_imm[4] != 0 && ssa.imm[4] == 10);
    CHECK(ssa.ownership[0] == ir::Ownership::BORROWED);
    // The stored stack value was copied into the slot (census:
    // DUPLICATED); the get_loc copy was consumed by the add; the slot
    // dup is still alive in the slot (OWNED).
    CHECK(ssa.ownership[1] == ir::Ownership::DUPLICATED);
    CHECK(ssa.ownership[3] == ir::Ownership::CONSUMED);
    CHECK(ssa.ownership[2] == ir::Ownership::OWNED);
}

// a8: put_loc store-release — the second store frees the first stored
// value (the trailing old-slot arg), never a violation.
void test_a8_put_loc() {
    // push_i8 5; put_loc0; push_i8 6; put_loc0; get_loc0; return_undef.
    const std::vector<std::uint8_t> code = {194, 5,  207, 194, 6,
                                            207, 203, 41};
    ir::SsaFunc ssa;
    std::string err;
    CHECK(analyze_blob(code, 1, &ssa, &err));
    if (err.empty() && ssa.blocks.empty()) return;
    // v0 = loc0 sentinel; v1 = push 5; v2 = push 6; v3 = get_loc copy.
    CHECK(ssa.lattice[3] == ir::Lattice::INT32);
    CHECK(ssa.has_imm[3] != 0 && ssa.imm[3] == 6);
    // The first stored value is freed by the second store.
    CHECK(ssa.ownership[1] == ir::Ownership::CONSUMED);
    CHECK(ssa.ownership[0] == ir::Ownership::BORROWED);
}

// a9: inc_loc — the RMW consumes the old slot value (CONSUMED) and
// births a fresh UNKNOWN replacement.
void test_a9_rmw() {
    // push_i8 5; put_loc0; inc_loc 0; get_loc0; return_undef.
    const std::vector<std::uint8_t> code = {194, 5, 207, 146, 0, 203, 41};
    ir::SsaFunc ssa;
    std::string err;
    CHECK(analyze_blob(code, 1, &ssa, &err));
    if (err.empty() && ssa.blocks.empty()) return;
    // v0 = loc0 sentinel; v1 = push 5; v2 = RMW replacement; v3 =
    // get_loc copy.
    CHECK(ssa.ownership[1] == ir::Ownership::CONSUMED);
    CHECK(ssa.lattice[2] == ir::Lattice::UNKNOWN);
    CHECK(ssa.lattice[3] == ir::Lattice::UNKNOWN);
}

// a10: get_loc0_loc1 — two fresh copies, two loc slots in the census.
void test_a10_get_loc0_loc1() {
    // push_i8 1; put_loc0; push_i8 2; put_loc1; get_loc0_loc1; add;
    // return_undef.
    const std::vector<std::uint8_t> code = {194, 1,  207, 194, 2,
                                            208, 202, 156, 41};
    ir::SsaFunc ssa;
    std::string err;
    CHECK(analyze_blob(code, 2, &ssa, &err));
    if (err.empty() && ssa.blocks.empty()) return;
    // v0/v1 = loc sentinels; v2 = push 1; v3 = push 2; v4/v5 = the two
    // copies; v6 = add of the two slot copies: 1 + 2.
    CHECK(ssa.loc_slot_count == 2);
    CHECK(ssa.lattice[6] == ir::Lattice::INT32);
    CHECK(ssa.has_imm[6] != 0 && ssa.imm[6] == 3);
}

// a11: a conditionally-written local — the merge's loc parameter
// joins a real value (refs 1) with the never-written sentinel (refs
// 0); the min cannot drop below 1, so the later store of the old slot
// releases an owned parameter (never a false violation, Fix D).
void test_a11_mixed_loc_join() {
    // push_0; if_false8 +6; [B1] push_i8 1; put_loc0; goto8 +4;
    // [B2] push_i8 0; drop; [B3] push_i8 6; put_loc0; return_undef.
    // Branch targets are pc + 1 + diff: if_false8 at byte 1 aux 6 ->
    // byte 8 (B2), goto8 at byte 6 aux 4 -> byte 11 (B3).
    const std::vector<std::uint8_t> code = {186, 240, 6, 194, 1,  207,
                                            242, 4,   194, 0, 14,  194,
                                            6,   207, 41};
    ir::SsaFunc ssa;
    std::string err;
    CHECK(analyze_blob(code, 1, &ssa, &err));
    if (err.empty() && ssa.blocks.empty()) return;
    CHECK(ssa.blocks.size() == 3);
    // The merge (blocks[2]) takes one loc parameter; the never-written
    // path contributes the UNINITIALIZED sentinel, so the join is
    // UNKNOWN (metadata) and the parameter is real (refs clamped to
    // 1): the final put_loc0 releases it as the overwritten slot
    // value.
    CHECK(ssa.param_count == 1);
    CHECK(ssa.blocks[2].entry_locs.size() == 1);
    const uint32_t loc_param = ssa.blocks[2].entry_locs[0];
    CHECK(ssa.lattice[loc_param] == ir::Lattice::UNKNOWN);
    CHECK(ssa.sentinel[loc_param] == 0);
    CHECK(ssa.ownership[loc_param] == ir::Ownership::CONSUMED);
    CHECK(ssa.loc_slot_count == 1);
    // The merge's push_i8 6 folds (v5: v0 sentinel, v1 push_0, v2
    // push_i8 1, v3 push_i8 0, v4 the loc parameter).
    CHECK(ssa.lattice[5] == ir::Lattice::INT32);
    CHECK(ssa.has_imm[5] != 0 && ssa.imm[5] == 6);
}

void test_ssa_blobs() {
    test_a1_fold_dup_drop();
    test_a2_overflow();
    test_a2b_unknown_bitwise();
    test_a3_join_params();
    test_a4_loop_token();
    test_a5_try_catch();
    test_a6_try_join_marker();
    test_a7_set_loc();
    test_a8_put_loc();
    test_a9_rmw();
    test_a10_get_loc0_loc1();
    test_a11_mixed_loc_join();
}

// ---------------------------------------------------------------------------
// Part B: bundle-level round trip (synthetic .qjsb, mirrors the
// test_cfg Builder layout, including the debug block).
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

std::vector<std::uint8_t> make_bundle_with_rejected_parent() {
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

    // Parent function with one child in its constant pool.
    b.push_back(12);  // BC_TAG_FUNCTION_BYTECODE
    put_u16(&b, 0);
    b.push_back(1);   // strict
    put_leb(&b, 0);   // function name atom
    put_leb(&b, 0);   // arg_count
    put_leb(&b, 0);   // var_count
    put_leb(&b, 0);   // defined_arg_count
    put_leb(&b, 1);   // stack_size
    put_leb(&b, 0);   // var_ref_count
    put_leb(&b, 0);   // closure_var_count
    put_leb(&b, 1);   // cpool_count
    put_leb(&b, 1);   // parent byte_code_len
    put_leb(&b, 0);   // parent vardef count

    // Independently valid child: push_0; return_undef.
    b.push_back(12);  // BC_TAG_FUNCTION_BYTECODE
    put_u16(&b, 0);
    b.push_back(1);
    put_leb(&b, 0);
    put_leb(&b, 0);
    put_leb(&b, 0);
    put_leb(&b, 0);
    put_leb(&b, 1);
    put_leb(&b, 0);
    put_leb(&b, 0);
    put_leb(&b, 0);
    put_leb(&b, 2);
    put_leb(&b, 0);
    b.push_back(186);  // push_0
    b.push_back(41);   // return_undef

    b.push_back(252);  // parent code: invalid serialized opcode
    std::uint32_t c = bc_csum(b.data() + 5, b.size() - 5);
    b[1] = static_cast<std::uint8_t>(c);
    b[2] = static_cast<std::uint8_t>(c >> 8);
    b[3] = static_cast<std::uint8_t>(c >> 16);
    b[4] = static_cast<std::uint8_t>(c >> 24);
    return b;
}

void test_ssa_bundles() {
    std::string err;
    ir::SsaReport rep;

    // Canonical bundle with a valid debug block: the walker covers the
    // function with zero rejections; the counts are exact.
    std::vector<std::uint8_t> dbg = make_debug({2, 0});
    std::vector<std::uint8_t> good = make_bundle({186, 41}, 1, &dbg);
    CHECK(ir::ssa_round_trip(good.data(), good.size(), &rep, &err));
    CHECK(rep.functions == 1);
    CHECK(rep.rejected_functions == 0);
    CHECK(rep.rejected_insns == 0);
    CHECK(rep.nodes == 2);   // push_0, return_undef
    CHECK(rep.values == 1);  // push_0's result
    CHECK(rep.params == 0);
    CHECK(rep.max_token == 1);  // return_undef is TERMINAL
    CHECK(rep.lattice_count[static_cast<unsigned>(ir::Lattice::INT32)] ==
          1);
    // The public entry agrees.
    CHECK(capsid::bytecode::ssa_analyze(good, &err));
    CHECK(err.empty());

    // Malformed pc2line (truncated sleb): the function cannot decode,
    // so it is counted as rejected coverage — never a silent skip.
    std::vector<std::uint8_t> bad_dbg = make_debug({2, 0x80});
    std::vector<std::uint8_t> bad = make_bundle({186, 41}, 1, &bad_dbg);
    CHECK(ir::ssa_round_trip(bad.data(), bad.size(), &rep, &err));
    CHECK(rep.rejected_functions == 1);

    // A rejected parent must not hide valid nested functions from coverage.
    std::vector<std::uint8_t> nested = make_bundle_with_rejected_parent();
    CHECK(ir::ssa_round_trip(nested.data(), nested.size(), &rep, &err));
    CHECK(rep.functions == 2);
    CHECK(rep.rejected_functions == 1);
    CHECK(rep.nodes == 2);
    CHECK(rep.values == 1);
}

// ---------------------------------------------------------------------------
// Part C: the mandatory 0-rejection gate over the real corpus.
// ---------------------------------------------------------------------------

void test_corpus_ssa() {
    JSRuntime* rt = JS_NewRuntime();
    CHECK(rt != nullptr);
    JSContext* ctx = JS_NewContext(rt);
    CHECK(ctx != nullptr);
    if (ctx == nullptr) return;

    // The test_cfg identity corpus, plus targeted additions for the
    // four analysis-correctness fixes:
    //  - set_loc keeps the stored value on the stack (Fix A);
    //  - loops with heap writes keep the world token bounded (Fix B);
    //  - nested optional-binding catches with rethrow keep the outer
    //    handler (Fix C);
    //  - try bodies that join before their drop, and conditionally-
    //    written locals, birth borrowed / owned join params (Fix D).
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
        // Fix A: assignment expression then overwrite (set_loc dup +
        // store of the overwritten slot).
        "let y; let x; y = (x = 5); x = 6; globalThis.__r = y;",
        // Fix B: loop with a heap write keeps the world token bounded.
        "const o = { a: 1 }; let s = 0; "
        "for (let i = 0; i < 10; i++) { o.a = o.a + 1; s += o.a; }"
        "globalThis.__r = s;",
        // Fix C: nested optional-binding catch + rethrow keeps the
        // outer handler on the stack.
        "let r = 0; try { try { throw 1; } catch { throw 2; } } "
        "catch { r = 1; } globalThis.__r = r;",
        // Fix D (stack): a try body that joins before its drop.
        "let r = 0; try { if (globalThis.__c) { r = 1; } "
        "else { r = 2; } } catch { r = 3; } globalThis.__r = r;",
        // Fix D (locs): a conditionally-written local.
        "let x; if (globalThis.__c) { x = 1; } x = 5; "
        "globalThis.__r = x;",
    };
    for (const char* src : sources) {
        JSValue module = JS_Eval(ctx, src, std::strlen(src), "ssa.js",
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
        ir::SsaReport rep;
        if (!ir::ssa_round_trip(buf.data(), buf.size(), &rep, &err)) {
            std::fprintf(stderr, "FAIL: ssa gate failed for: %s\n  %s\n",
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
        CHECK(capsid::bytecode::ssa_analyze(buf, &err));
        if (g_failures > 5) break;  // don't flood past the first few
    }
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
}

}  // namespace

int main() {
    test_ssa_blobs();
    test_ssa_bundles();
    test_corpus_ssa();
    if (g_failures != 0) {
        std::fprintf(stderr, "test_bytecode_ssa: %d failure(s)\n",
                     g_failures);
        return 1;
    }
    std::fprintf(stderr, "test_bytecode_ssa: all checks passed\n");
    return 0;
}
