// I1 full-stack SSA implementation (see ssa.h). Construction is a
// single pass over the reverse-postorder blocks: values are symbolic
// ids, multi-predecessor blocks take a block parameter per live stack
// position and per used frame slot, single-predecessor blocks flow the
// predecessor's edge snapshot through, and the gosub/with_* entry
// slots the edges cannot name become phantom values that are never
// read. The analyses (value lattice, world token, exception
// successors, refcount ownership) then iterate the same blocks to a
// monotone fixpoint; an ownership violation rejects the function
// (fail-closed, counted as rejected coverage).
//
// Refcount semantics are pinned from the vendored interpreter
// (vendor/txiki.js/deps/quickjs/quickjs.c):
//  - RELEASE pops free their values (drop, stores, calls, ...)
//  - the shuffle family frees nothing (ALIAS); nip/nip1 free only the
//    deepest popped value (NIP: JS_FreeValue(sp[-2]) / sp[-3]))
//  - frame-slot stores move the stored ref into the slot and free the
//    previous slot value; get_loc copies are fresh owning references
//  - catch pushes a BORROWED JS_NewCatchOffset marker; dropping a
//    sentinel ends the exception region
//  - gosub pushes the return address (the finally's +1 entry slot
//    after it is the phantom); with_put_var's -1 target drops the top
//    snapshot value (the scope).
#include "bytecode_rewriter/ir/ssa.h"

#include <algorithm>
#include <climits>
#include <cstdio>
#include <utility>


namespace capsid {
namespace bytecode {
namespace ir {
namespace {

// Serialized opcode enum, built exactly like cfg.cc (quickjs.c:1166).
enum Opcode {
#define FMT(f)
#define DEF(id, size, n_pop, n_push, f) OP_##id,
#define def(id, size, n_pop, n_push, f) /* temporary: never serialized */
#include "quickjs-opcode.h"
#undef def
#undef DEF
#undef FMT
    OP_COUNT, /* excluding temporary opcodes */
};

// ---------------------------------------------------------------------------
// Shuffle result sources (from the vendored interpreter stack cases;
// args are top-first, results are push order). src[k] is the arg index
// whose runtime value result k carries; fresh[k] marks the js_dup
// copies (fresh owning references) vs the kept originals (same value
// id).
// ---------------------------------------------------------------------------

// Shuffle semantics (pinned from the vendored interpreter, quickjs.c
// stack cases): the runtime shuffles IN PLACE — n_pop cells are removed
// from the top and n_push cells end up above the remaining stack. Each
// result cell is either a moved cell (src indexes into `args`, which
// holds the popped cells top-first, followed by `base` more cells
// captured below the pop window) or a fresh js_dup copy of its source
// (fresh[k] == 1). Every fresh cell carries the interpreter's js_dup,
// so the ownership simulation bumps the source's refcount by one. The
// entries below are derived from the runtime sp[] manipulations, one
// op at a time; n is always n_push.
struct ShuffleDef {
    uint8_t n;
    const int8_t* src;
    const uint8_t* fresh;
    uint8_t base;
};

static const int8_t kDup1[] = {0, 0};
static const uint8_t kDupF1[] = {0, 1};
static const int8_t kDup1_3[] = {1, 0};
static const uint8_t kDupF1_3[] = {1, 0};
static const int8_t kDup2[] = {1, 0, 1, 0};
static const uint8_t kDupF2[] = {0, 0, 1, 1};
static const int8_t kDup3[] = {2, 1, 0, 2, 1, 0};
static const uint8_t kDupF3[] = {0, 0, 0, 1, 1, 1};
static const int8_t kIns2[] = {0, 1, 0};
static const uint8_t kInsF2[] = {0, 0, 1};
static const int8_t kIns3[] = {0, 2, 1, 0};
static const uint8_t kInsF3[] = {0, 0, 0, 1};
static const int8_t kIns4[] = {0, 3, 2, 1, 0};
static const uint8_t kInsF4[] = {0, 0, 0, 0, 1};
static const int8_t kPerm3[] = {1, 2, 0};
static const uint8_t kPermF3[] = {0, 0, 0};
static const int8_t kPerm4[] = {1, 3, 2, 0};
static const uint8_t kPermF4[] = {0, 0, 0, 0};
static const int8_t kPerm5[] = {1, 4, 3, 2, 0};
static const uint8_t kPermF5[] = {0, 0, 0, 0, 0};
static const int8_t kRot3l[] = {1, 0, 2};
static const uint8_t kRotF3l[] = {0, 0, 0};
static const int8_t kRot3r[] = {0, 2, 1};
static const uint8_t kRotF3r[] = {0, 0, 0};
static const int8_t kRot4l[] = {2, 1, 0, 3};
static const uint8_t kRotF4l[] = {0, 0, 0, 0};
static const int8_t kRot5l[] = {3, 2, 1, 0, 4};
static const uint8_t kRotF5l[] = {0, 0, 0, 0, 0};
static const int8_t kSwap[] = {0, 1};
static const uint8_t kSwapF[] = {0, 0};
static const int8_t kSwap2[] = {1, 0, 3, 2};
static const uint8_t kSwapF2[] = {0, 0, 0, 0};
static const int8_t kNipCatch[] = {0};
static const uint8_t kNipCatchF[] = {0};

const ShuffleDef* shuffle_def(uint8_t op) {
    switch (op) {
    case OP_dup: {
        static const ShuffleDef d = {2, kDup1, kDupF1, 0};
        return &d;
    }
    case OP_dup1: {
        static const ShuffleDef d = {2, kDup1_3, kDupF1_3, 1};
        return &d;
    }
    case OP_dup2: {
        static const ShuffleDef d = {4, kDup2, kDupF2, 0};
        return &d;
    }
    case OP_dup3: {
        static const ShuffleDef d = {6, kDup3, kDupF3, 0};
        return &d;
    }
    case OP_insert2: {
        static const ShuffleDef d = {3, kIns2, kInsF2, 0};
        return &d;
    }
    case OP_insert3: {
        static const ShuffleDef d = {4, kIns3, kInsF3, 0};
        return &d;
    }
    case OP_insert4: {
        static const ShuffleDef d = {5, kIns4, kInsF4, 0};
        return &d;
    }
    case OP_perm3: {
        static const ShuffleDef d = {3, kPerm3, kPermF3, 0};
        return &d;
    }
    case OP_perm4: {
        static const ShuffleDef d = {4, kPerm4, kPermF4, 0};
        return &d;
    }
    case OP_perm5: {
        static const ShuffleDef d = {5, kPerm5, kPermF5, 0};
        return &d;
    }
    case OP_rot3l: {
        static const ShuffleDef d = {3, kRot3l, kRotF3l, 0};
        return &d;
    }
    case OP_rot3r: {
        static const ShuffleDef d = {3, kRot3r, kRotF3r, 0};
        return &d;
    }
    case OP_rot4l: {
        static const ShuffleDef d = {4, kRot4l, kRotF4l, 0};
        return &d;
    }
    case OP_rot5l: {
        static const ShuffleDef d = {5, kRot5l, kRotF5l, 0};
        return &d;
    }
    case OP_swap: {
        static const ShuffleDef d = {2, kSwap, kSwapF, 0};
        return &d;
    }
    case OP_swap2: {
        static const ShuffleDef d = {4, kSwap2, kSwapF2, 0};
        return &d;
    }
    case OP_nip_catch: {
        static const ShuffleDef d = {1, kNipCatch, kNipCatchF, 0};
        return &d;
    }
    default:
        return NULL;
    }
}

// ---------------------------------------------------------------------------
// Lattice join (§3.3): monotone, imm/shape aware. BOTTOM is the
// identity; equal lattices keep the imm only when both sides carry the
// same known small int (or the same closure path); numeric joins lose
// the imm; OBJECT_SHAPES merges its id sets (capped at 2, else
// UNKNOWN); EXACT_CLOSURE survives only with equal paths.
// ---------------------------------------------------------------------------

Lattice join_lattice(Lattice a, int64_t ia, bool ha, Lattice b, int64_t ib,
                     bool hb, int64_t* out_imm, bool* out_has,
                     const std::vector<uint32_t>& shapes_a,
                     const std::vector<uint32_t>& shapes_b,
                     std::vector<uint32_t>* out_shapes) {
    auto merge_shapes = [&](int* overflow) -> Lattice {
        out_shapes->clear();
        for (size_t i = 0; i < shapes_a.size(); i++) {
            out_shapes->push_back(shapes_a[i]);
        }
        for (size_t i = 0; i < shapes_b.size(); i++) {
            bool dup = false;
            for (size_t j = 0; j < out_shapes->size(); j++) {
                if ((*out_shapes)[j] == shapes_b[i]) {
                    dup = true;
                    break;
                }
            }
            if (!dup) {
                if (out_shapes->size() >= 2) {
                    *overflow = 1;
                    return Lattice::UNKNOWN;
                }
                out_shapes->push_back(shapes_b[i]);
            }
        }
        *overflow = 0;
        return Lattice::OBJECT_SHAPES;
    };
    if (a == b) {
        if (a == Lattice::OBJECT_SHAPES) {
            int overflow = 0;
            Lattice r = merge_shapes(&overflow);
            *out_imm = 0;
            *out_has = false;
            if (overflow) {
                out_shapes->clear();
                return Lattice::UNKNOWN;
            }
            return r;
        }
        if (ha && hb && ia == ib &&
            (a == Lattice::INT32 || a == Lattice::EXACT_CLOSURE)) {
            *out_imm = ia;
            *out_has = true;
        } else {
            *out_imm = 0;
            *out_has = false;
        }
        return a;
    }
    if (a == Lattice::BOTTOM) {
        *out_imm = ib;
        *out_has = hb;
        *out_shapes = shapes_b;
        return b;
    }
    if (b == Lattice::BOTTOM) {
        *out_imm = ia;
        *out_has = ha;
        *out_shapes = shapes_a;
        return a;
    }
    if (a == Lattice::UNKNOWN || b == Lattice::UNKNOWN) {
        *out_imm = 0;
        *out_has = false;
        out_shapes->clear();
        return Lattice::UNKNOWN;
    }
    const bool a_num = a == Lattice::INT32 || a == Lattice::FLOAT64 ||
                       a == Lattice::NUMBER;
    const bool b_num = b == Lattice::INT32 || b == Lattice::FLOAT64 ||
                       b == Lattice::NUMBER;
    if (a_num && b_num) {
        *out_imm = 0;
        *out_has = false;
        out_shapes->clear();
        if (a == Lattice::INT32 && b == Lattice::INT32) {
            return Lattice::INT32;
        }
        if (a == Lattice::FLOAT64 && b == Lattice::FLOAT64) {
            return Lattice::FLOAT64;
        }
        return Lattice::NUMBER;
    }
    if (a == Lattice::EXACT_CLOSURE && b == Lattice::EXACT_CLOSURE) {
        if (ha && hb && ia == ib) {
            *out_imm = ia;
            *out_has = true;
            return Lattice::EXACT_CLOSURE;
        }
        *out_imm = 0;
        *out_has = false;
        return Lattice::UNKNOWN;
    }
    *out_imm = 0;
    *out_has = false;
    out_shapes->clear();
    return Lattice::UNKNOWN;
}

// ---------------------------------------------------------------------------
// Construction + analysis state.
// ---------------------------------------------------------------------------

// Per-block analysis state. The entry fields are the joined values from
// the predecessor edges (monotone fixpoint); `refs` is the dense local
// refcount map (path-local: the same value id can hold different refs
// on different paths, so each block simulates its own copy).
struct AState {
    std::vector<int32_t> handlers;  // entry handler stack
    uint32_t token;                 // entry world token
    std::vector<int> entry_refs;    // entry refcounts (dense, -1 = absent)
    std::vector<int> refs;          // local simulation refcounts
};

struct Analyzer {
    explicit Analyzer(const Cfg& c) : cfg(c) {}
    const Cfg& cfg;
    SsaFunc* f;
    std::string* error;
    std::vector<uint32_t> rpo;         // reachable blocks, reverse postorder
    std::vector<uint32_t> pred_block;  // valid when pred_count == 1
    std::vector<uint32_t> pred_edge;
    std::vector<int> pred_count;
    std::vector<std::vector<std::pair<uint32_t, uint32_t> > > preds;
    // The handler block each catch marker belongs to (parallel to the
    // value table; -1 = not a catch marker). The region-end drop pops
    // the handler stack only when the dropped marker's own region is
    // its top: the CATCH edge already exits the region, so the
    // handler-entry drop of the marker must not pop the outer handler.
    std::vector<int32_t> marker_region;

    bool fail(const char* msg) const {
        *error = std::string("ssa: ") + msg;
        return false;
    }

    uint32_t new_val(Lattice lat, bool sentinel, bool is_param, int64_t imm,
                     bool has_imm) {
        uint32_t id = f->value_count++;
        f->lattice.push_back(lat);
        f->imm.push_back(imm);
        f->has_imm.push_back(has_imm ? 1 : 0);
        f->ownership.push_back(sentinel ? Ownership::BORROWED
                                        : Ownership::OWNED);
        f->sentinel.push_back(sentinel ? 1 : 0);
        f->is_param.push_back(is_param ? 1 : 0);
        f->shapes.push_back(std::vector<uint32_t>());
        marker_region.push_back(-1);
        return id;
    }

    // Reverse postorder over all edges. The DFS back edges are exactly
    // the cycle-forming edges (an edge that is a DFS back edge lies on
    // a cycle, so its target has another predecessor — a reachable
    // block's unique edge can never be a DFS back edge). Every other
    // edge's source precedes its target in the order, which the
    // single-predecessor snapshot flow depends on. Unreachable blocks
    // are excluded.
    void compute_rpo() {
        const size_t nb = cfg.blocks.size();
        std::vector<uint8_t> color(nb, 0);  // 0 white, 1 gray, 2 black
        std::vector<uint32_t> finish;
        std::vector<uint32_t> stack;
        stack.push_back(0);
        while (!stack.empty()) {
            uint32_t b = stack.back();
            if (color[b] == 0) {
                color[b] = 1;
                const Block& blk = cfg.blocks[b];
                for (size_t e = 0; e < blk.edges.size(); e++) {
                    uint32_t t = blk.edges[e].to;
                    if (color[t] == 0) stack.push_back(t);
                }
            } else if (color[b] == 1) {
                color[b] = 2;
                finish.push_back(b);
                stack.pop_back();
            } else {
                stack.pop_back();
            }
        }
        rpo.assign(finish.rbegin(), finish.rend());
    }

    // Slot census over the whole function (used slots per space).
    void census(uint32_t* args_slots, uint32_t* loc_slots) {
        uint32_t a = 0, l = 0;
        for (size_t i = 0; i < cfg.insns.size(); i++) {
            const Insn& in = cfg.insns[i];
            if (!is_frame_slot_op(in.op)) continue;
            int idx = slot_index(in.op, in.aux);
            if (in.op == OP_get_loc0_loc1) idx = 1;  // covers slots 0 and 1
            uint32_t& m = (slot_space(in.op) == SlotSpace::ARG) ? a : l;
            if (static_cast<uint32_t>(idx) + 1 > m) {
                m = static_cast<uint32_t>(idx) + 1;
            }
        }
        *args_slots = a;
        *loc_slots = l;
    }

    // Block entry stack: values bottom-first. Multi-predecessor blocks
    // take a block parameter per position; single-predecessor blocks
    // flow the unique predecessor's edge snapshot through, with fresh
    // phantoms for the positions the snapshot cannot name (the
    // gosub/with_* +1/+2 entry slots). Block 0's stack is empty.
    bool entry_stack(uint32_t b, std::vector<uint32_t>* out) {
        const int32_t h = cfg.blocks[b].entry_height;
        if (h < 0) return fail("unverified block in SSA");
        out->clear();
        if (b == 0) return true;
        if (pred_count[b] > 1) {
            for (int32_t p = 0; p < h; p++) {
                out->push_back(new_val(Lattice::BOTTOM, false, true, 0,
                                       false));
            }
            return true;
        }
        const SsaEdgeSnap& snap =
            f->blocks[pred_block[b]].edge_snaps[pred_edge[b]];
        for (int32_t p = 0; p < h; p++) {
            if (static_cast<size_t>(p) < snap.stack.size()) {
                out->push_back(snap.stack[p]);
            } else {
                // Phantom: the gosub/with_* +1/+2 entry slot (never
                // read — only popped by the target's drops/ret).
                out->push_back(new_val(Lattice::UNKNOWN, false, false, 0,
                                       false));
            }
        }
        return true;
    }

    // Same for the arg/loc slot spaces. The edge snapshots carry the
    // full census width, so every position is covered (defensive
    // phantom for the case it is not).
    bool entry_slots(uint32_t b, SlotSpace sp, uint32_t slots,
                     std::vector<uint32_t>* out) {
        out->clear();
        if (b == 0) {
            for (uint32_t p = 0; p < slots; p++) {
                if (sp == SlotSpace::ARG) {
                    // Unknown caller-provided values, one ref each.
                    out->push_back(new_val(Lattice::UNKNOWN, false, false, 0,
                                           false));
                } else {
                    // Never-written locals: UNINITIALIZED sentinels.
                    out->push_back(new_val(Lattice::UNINITIALIZED, true,
                                           false, 0, false));
                }
            }
            return true;
        }
        if (pred_count[b] > 1) {
            for (uint32_t p = 0; p < slots; p++) {
                out->push_back(new_val(Lattice::BOTTOM, false, true, 0,
                                       false));
            }
            return true;
        }
        const SsaEdgeSnap& snap =
            f->blocks[pred_block[b]].edge_snaps[pred_edge[b]];
        const std::vector<uint32_t>& src =
            (sp == SlotSpace::ARG) ? snap.args : snap.locs;
        for (uint32_t p = 0; p < slots; p++) {
            if (p < src.size()) {
                out->push_back(src[p]);
            } else {
                out->push_back(new_val(sp == SlotSpace::ARG
                                           ? Lattice::UNKNOWN
                                           : Lattice::UNINITIALIZED,
                                       sp != SlotSpace::ARG, false, 0,
                                       false));
            }
        }
        return true;
    }
};

}  // namespace

// ---------------------------------------------------------------------------
// Lattice transfer (§3.3). Updates the node's result values from the
// current analysis state; `changed` tracks lattice/imm movement so the
// fixpoint can detect convergence.
// ---------------------------------------------------------------------------

namespace {

void set_value(SsaFunc* f, uint32_t v, Lattice lat, int64_t imm, bool has,
               bool* changed) {
    if (f->lattice[v] != lat || (f->has_imm[v] != 0) != has ||
        (has && f->imm[v] != imm)) {
        *changed = true;
    }
    f->lattice[v] = lat;
    f->has_imm[v] = has ? 1 : 0;
    f->imm[v] = imm;
}

void transfer(const Analyzer& A, const SsaNode& node, SsaFunc* f,
              const std::vector<uint32_t>& cur_args,
              const std::vector<uint32_t>& cur_locs, bool* changed) {
    const Insn& in = A.cfg.insns[node.insn];
    if (node.results.empty() && node.slot_writes.empty()) return;
    const bool is_sh = is_shuffle(in.op);
    const ShuffleDef* sd = is_sh ? shuffle_def(in.op) : NULL;
    // Fresh results default to UNKNOWN, no imm — but only the ones no
    // case below sets. The default must be applied AFTER the switch:
    // applied before, it stomps a case-set lattice back to UNKNOWN on
    // every fixpoint iteration and the analysis never converges. Kept
    // ids (shuffle kepts, the set_loc family's stack value, the RMW
    // old-slot push) are existing values with stable lattices; never
    // touch them.
    std::vector<uint8_t> touched(node.results.size(), 0);
    auto set = [&](size_t k, Lattice lat, int64_t imm, bool has) {
        if (k >= node.results.size()) return;
        touched[k] = 1;
        set_value(f, node.results[k], lat, imm, has, changed);
    };
    auto copy_shapes = [&](uint32_t dst, uint32_t src) {
        if (f->lattice[src] == Lattice::OBJECT_SHAPES) {
            f->shapes[dst] = f->shapes[src];
        }
    };
    switch (in.op) {
    case OP_push_minus1: case OP_push_0: case OP_push_1: case OP_push_2:
    case OP_push_3: case OP_push_4: case OP_push_5: case OP_push_6:
    case OP_push_7: case OP_push_i8: case OP_push_i16: case OP_push_i32:
        set(0, Lattice::INT32, in.imm, true);
        return;
    case OP_push_empty_string:
        set(0, Lattice::STRING, 0, false);
        return;
    case OP_typeof:
        set(0, Lattice::STRING, 0, false);
        return;
    case OP_lnot: case OP_is_undefined: case OP_is_null:
    case OP_is_undefined_or_null: case OP_typeof_is_undefined:
    case OP_typeof_is_function:
    case OP_lt: case OP_lte: case OP_gt: case OP_gte:
    case OP_eq: case OP_neq: case OP_strict_eq: case OP_strict_neq:
    case OP_in: case OP_instanceof:
        set(0, Lattice::INT32, 0, false);
        return;
    case OP_not: {
        // Number ~ produces int32, but BigInt ~ produces BigInt. UNKNOWN
        // therefore cannot be narrowed merely because execution returned.
        const Lattice a = node.args.empty()
                                  ? Lattice::UNKNOWN
                                  : f->lattice[node.args[0]];
        const bool numeric = a == Lattice::INT32 ||
                             a == Lattice::FLOAT64 ||
                             a == Lattice::NUMBER;
        set(0, numeric ? Lattice::INT32 : Lattice::UNKNOWN, 0, false);
        return;
    }
    case OP_and: case OP_or: case OP_xor: case OP_shl: case OP_sar: {
        // Number operands produce an int32. These operators also have a
        // normal BigInt result, so an unproven operand pair stays UNKNOWN.
        bool numeric = node.args.size() >= 2;
        for (size_t i = 0; numeric && i < 2; i++) {
            const Lattice a = f->lattice[node.args[i]];
            numeric = a == Lattice::INT32 || a == Lattice::FLOAT64 ||
                      a == Lattice::NUMBER;
        }
        set(0, numeric ? Lattice::INT32 : Lattice::UNKNOWN, 0, false);
        return;
    }
    case OP_shr:
        // Unsigned shift can exceed INT32_MAX and become a float64-tagged
        // uint32 in QuickJS.
        set(0, Lattice::NUMBER, 0, false);
        return;
    case OP_add: case OP_sub: case OP_mul: {
        // args are top-first: [right, left]; result = left op right.
        Lattice la, lb;
        int64_t ia, ib;
        bool ha, hb;
        if (node.args.size() >= 2) {
            la = f->lattice[node.args[1]];
            ha = f->has_imm[node.args[1]] != 0;
            ia = f->imm[node.args[1]];
            lb = f->lattice[node.args[0]];
            hb = f->has_imm[node.args[0]] != 0;
            ib = f->imm[node.args[0]];
        } else {
            la = lb = Lattice::UNKNOWN;
            ha = hb = false;
            ia = ib = 0;
        }
        if (la == Lattice::INT32 && lb == Lattice::INT32 && ha && hb) {
            // Fold small ints (the int64 intermediate is exact for two
            // int32 operands). QuickJS represents an int32-range overflow
            // as a float64 number, so retain that proven tag class.
            int64_t r = 0;
            if (in.op == OP_add) {
                r = ia + ib;
            } else if (in.op == OP_sub) {
                r = ia - ib;
            } else {
                r = ia * ib;
            }
            if (r < INT_MIN || r > INT_MAX) {
                set(0, Lattice::FLOAT64, 0, false);
            } else {
                set(0, Lattice::INT32, r, true);
            }
        } else {
            const bool a_num = la == Lattice::INT32 ||
                               la == Lattice::FLOAT64 ||
                               la == Lattice::NUMBER;
            const bool b_num = lb == Lattice::INT32 ||
                               lb == Lattice::FLOAT64 ||
                               lb == Lattice::NUMBER;
            if (a_num && b_num) {
                // Two unknown int32 operands can overflow. The old transfer
                // joined INT32+INT32 back to INT32 and was unsound for any
                // later tag-specialized lowering.
                set(0, la == Lattice::FLOAT64 && lb == Lattice::FLOAT64
                           ? Lattice::FLOAT64
                           : Lattice::NUMBER,
                    0, false);
            } else {
                set(0, Lattice::UNKNOWN, 0, false);
            }
        }
        return;
    }
    case OP_div: case OP_mod: case OP_pow: {
        if (node.args.size() < 2) {
            set(0, Lattice::UNKNOWN, 0, false);
            return;
        }
        const Lattice la = f->lattice[node.args[1]];
        const Lattice lb = f->lattice[node.args[0]];
        const bool a_num = la == Lattice::INT32 ||
                           la == Lattice::FLOAT64 ||
                           la == Lattice::NUMBER;
        const bool b_num = lb == Lattice::INT32 ||
                           lb == Lattice::FLOAT64 ||
                           lb == Lattice::NUMBER;
        if (!a_num || !b_num) {
            set(0, Lattice::UNKNOWN, 0, false);
        } else {
            set(0, Lattice::NUMBER, 0, false);
        }
        return;
    }
    case OP_object:
        // Object literal with a known template shape (cpool idx).
        set(0, Lattice::OBJECT_SHAPES, 0, false);
        f->shapes[node.results[0]].clear();
        f->shapes[node.results[0]].push_back(in.aux);
        return;
    case OP_fclosure: case OP_fclosure8:
        // Exact closure: the function's cpool path.
        set(0, Lattice::EXACT_CLOSURE, static_cast<int64_t>(in.aux), true);
        return;
    case OP_get_loc: case OP_get_loc8: case OP_get_loc0:
    case OP_get_loc1: case OP_get_loc2: case OP_get_loc3:
    case OP_get_loc_check: {
        const int idx = slot_index(in.op, in.aux);
        const uint32_t v = cur_locs[idx];
        set(0, f->lattice[v], f->has_imm[v] ? f->imm[v] : 0,
            f->has_imm[v] != 0);
        copy_shapes(node.results[0], v);
        return;
    }
    case OP_get_loc0_loc1: {
        const uint32_t v0 = cur_locs[0];
        set(0, f->lattice[v0], f->has_imm[v0] ? f->imm[v0] : 0,
            f->has_imm[v0] != 0);
        copy_shapes(node.results[0], v0);
        const uint32_t v1 = cur_locs[1];
        set(1, f->lattice[v1], f->has_imm[v1] ? f->imm[v1] : 0,
            f->has_imm[v1] != 0);
        copy_shapes(node.results[1], v1);
        return;
    }
    case OP_get_arg: case OP_get_arg0: case OP_get_arg1:
    case OP_get_arg2: case OP_get_arg3: {
        const int idx = slot_index(in.op, in.aux);
        const uint32_t v = cur_args[idx];
        set(0, f->lattice[v], f->has_imm[v] ? f->imm[v] : 0,
            f->has_imm[v] != 0);
        copy_shapes(node.results[0], v);
        return;
    }
    case OP_set_loc: case OP_set_loc8:
    case OP_set_loc0: case OP_set_loc1: case OP_set_loc2: case OP_set_loc3:
    case OP_set_arg: case OP_set_arg0: case OP_set_arg1: case OP_set_arg2:
    case OP_set_arg3:
        // The slot's js_dup copy carries the stored value's lattice.
        for (size_t k = 0; k < node.slot_writes.size(); k++) {
            const uint32_t src = node.args[0];
            const uint32_t dst = node.slot_writes[k];
            set_value(f, dst, f->lattice[src],
                      f->has_imm[src] ? f->imm[src] : 0,
                      f->has_imm[src] != 0, changed);
            copy_shapes(dst, src);
        }
        return;
    case OP_gosub:
        // The pushed return address is a tagged int.
        set(0, Lattice::INT32, 0, false);
        return;
    default:
        break;
    }
    if (is_sh) {
        for (uint8_t k = 0; k < sd->n && k < node.results.size(); k++) {
            if (!sd->fresh[k]) continue;  // kept: same value id
            const uint32_t src = node.args[sd->src[k]];
            set(k, f->lattice[src], f->has_imm[src] ? f->imm[src] : 0,
                f->has_imm[src] != 0);
            copy_shapes(node.results[k], src);
        }
    }
    // RMW replacement slot values are unknown.
    if (is_loc_rmw(in.op)) {
        for (size_t k = 0; k < node.slot_writes.size(); k++) {
            set_value(f, node.slot_writes[k], Lattice::UNKNOWN, 0, false,
                      changed);
        }
    }
    // Default: UNKNOWN for the fresh results no case set (calls, field
    // ops, everything unclassified). Never touches kept ids.
    for (size_t k = 0; k < node.results.size(); k++) {
        if (touched[k]) continue;
        if (is_sh && !sd->fresh[k]) continue;
        if (is_loc_rmw(in.op)) continue;
        if (is_frame_slot_store(in.op) && in.n_push == 1) continue;
        set_value(f, node.results[k], Lattice::UNKNOWN, 0, false, changed);
    }
}

// ---------------------------------------------------------------------------
// Refcount verification (§3.4). Returns false on an ownership violation
// (a release of a non-sentinel value with no owning reference) — the
// function is rejected. `final_refs` accumulates each value's refcount
// at its last simulation touch (birth, release, or block entry) in RPO
// order, which the ownership census reads after convergence.
// ---------------------------------------------------------------------------

bool ownership(const Analyzer& A, const SsaNode& node, SsaFunc* f,
               AState& st, std::vector<int>* final_refs) {
    const Insn& in = A.cfg.insns[node.insn];
    auto release = [&](uint32_t v, const char* what) -> bool {
        if (f->sentinel[v]) return true;  // borrowed: release is a no-op
        if (v >= st.refs.size() || st.refs[v] < 1) {
            *A.error = "ssa: ownership violation: " + std::string(what) +
                       " of value " + std::to_string(v) +
                       " with no owning reference (insn " +
                       std::to_string(node.insn) + ")";
            return false;
        }
        st.refs[v]--;
        (*final_refs)[v] = st.refs[v];
        return true;
    };
    if (is_frame_slot_store(in.op)) {
        // The popped value is stored; the overwritten slot value (the
        // trailing arg) is freed by the interpreter's set_value.
        // set_loc_uninitialized pops nothing: args[0] is the old slot.
        // set_loc family (n_push == 1): the stored value keeps its
        // stack ref (the slot holds a fresh js_dup — slot_writes).
        // The stored value's ref moves into the slot (never released
        // here); fresh slot values (the set_loc dup, the
        // set_loc_uninitialized sentinel) are born below.
        if (node.args.size() < 1) return A.fail("store with no old slot");
        if (!release(node.args.back(), "release of overwritten slot")) {
            return false;
        }
        for (size_t k = 0; k < node.slot_writes.size(); k++) {
            const uint32_t v = node.slot_writes[k];
            if (v >= st.refs.size() || st.refs[v] != -1) continue;
            st.refs[v] = f->sentinel[v] ? 0 : 1;
            (*final_refs)[v] = st.refs[v];
        }
        return true;
    }
    if (is_loc_rmw(in.op)) {
        // inc_loc/dec_loc/add_loc consume the old slot value (freed by
        // set_value) and add_loc's popped RHS; the fresh replacement
        // slot value is born owned below.
        for (size_t k = 0; k < node.args.size(); k++) {
            if (!release(node.args[k], "release of rmw value")) return false;
        }
        for (size_t k = 0; k < node.slot_writes.size(); k++) {
            const uint32_t v = node.slot_writes[k];
            if (v < st.refs.size() && st.refs[v] == -1) {
                st.refs[v] = 1;
                (*final_refs)[v] = 1;
            }
        }
        return true;
    }
    switch (pop_ownership(in.op)) {
    case PopOwnership::RELEASE:
        for (size_t k = 0; k < node.args.size(); k++) {
            if (!release(node.args[k], "release of popped value")) {
                return false;
            }
        }
        break;
    case PopOwnership::ALIAS:
        break;  // shuffles free nothing
    case PopOwnership::NIP:
        // nip/nip1 free only the deepest popped value
        // (JS_FreeValue(sp[-2]) / sp[-3]).
        if (!node.args.empty() &&
            !release(node.args.back(), "release of nipped value")) {
            return false;
        }
        break;
    }
    if (is_shuffle(in.op)) {
        // Every fresh shuffle cell is the interpreter's js_dup of its
        // source: the source gains one reference.
        const ShuffleDef& sd = *shuffle_def(in.op);
        for (uint8_t k = 0; k < sd.n && k < node.results.size(); k++) {
            if (!sd.fresh[k]) continue;
            const uint32_t src = node.args[sd.src[k]];
            if (src >= st.refs.size() || st.refs[src] < 0) continue;
            st.refs[src]++;
            (*final_refs)[src] = st.refs[src];
        }
    }
    // Results: fresh values are born owned (refs 1); sentinels are
    // born borrowed (refs 0); kept shuffle ids are already accounted.
    for (size_t k = 0; k < node.results.size(); k++) {
        const uint32_t v = node.results[k];
        if (v >= st.refs.size() || st.refs[v] != -1) continue;
        st.refs[v] = f->sentinel[v] ? 0 : 1;
        (*final_refs)[v] = st.refs[v];
    }
    for (size_t k = 0; k < node.slot_writes.size(); k++) {
        const uint32_t v = node.slot_writes[k];
        if (v >= st.refs.size() || st.refs[v] != -1) continue;
        st.refs[v] = f->sentinel[v] ? 0 : 1;
        (*final_refs)[v] = st.refs[v];
    }
    return true;
}

}  // namespace

bool ssa_analyze_function(const Cfg& cfg, SsaFunc* out, std::string* error) {
    error->clear();
    const size_t nb = cfg.blocks.size();
    if (nb == 0) {
        *error = "ssa: empty CFG";
        return false;
    }
    Analyzer A(cfg);
    A.f = out;
    A.error = error;
    out->param_count = 0;
    out->value_count = 0;
    out->lattice.clear();
    out->imm.clear();
    out->has_imm.clear();
    out->ownership.clear();
    out->sentinel.clear();
    out->is_param.clear();
    out->shapes.clear();
    out->blocks.clear();
    out->entry_args.clear();
    out->entry_locs.clear();
    out->args_slot_count = 0;
    out->loc_slot_count = 0;

    // Predecessor census over all edges.
    A.pred_count.assign(nb, 0);
    A.pred_block.assign(nb, 0);
    A.pred_edge.assign(nb, 0);
    A.preds.resize(nb);
    for (size_t b = 0; b < nb; b++) {
        const Block& blk = cfg.blocks[b];
        for (size_t e = 0; e < blk.edges.size(); e++) {
            const uint32_t t = blk.edges[e].to;
            A.pred_count[t]++;
            A.pred_block[t] = static_cast<uint32_t>(b);
            A.pred_edge[t] = static_cast<uint32_t>(e);
            A.preds[t].push_back(
                std::make_pair(static_cast<uint32_t>(b),
                               static_cast<uint32_t>(e)));
        }
    }
    A.compute_rpo();
    A.census(&out->args_slot_count, &out->loc_slot_count);
    const uint32_t args_slots = out->args_slot_count;
    const uint32_t loc_slots = out->loc_slot_count;

    // -------------------------------------------------------------------
    // Construction: single pass over the RPO.
    // -------------------------------------------------------------------
    out->blocks.resize(nb);
    for (size_t ri = 0; ri < A.rpo.size(); ri++) {
        const uint32_t b = A.rpo[ri];
        const Block& blk = cfg.blocks[b];
        SsaBlock& sb = out->blocks[b];
        sb.block = b;
        sb.token_in = 0;
        std::vector<uint32_t> stack;
        if (!A.entry_stack(b, &stack)) return false;
        std::vector<uint32_t> cur_args, cur_locs;
        if (!A.entry_slots(b, SlotSpace::ARG, args_slots, &cur_args)) {
            return false;
        }
        if (!A.entry_slots(b, SlotSpace::LOC, loc_slots, &cur_locs)) {
            return false;
        }
        sb.entry_stack = stack;
        sb.entry_args = cur_args;
        sb.entry_locs = cur_locs;
        if (b == 0) {
            out->entry_args = cur_args;
            out->entry_locs = cur_locs;
        }
        sb.edge_snaps.resize(blk.edges.size());
        for (size_t i = blk.start; i < blk.end; i++) {
            const Insn& in = cfg.insns[i];
            SsaNode node;
            node.insn = static_cast<uint32_t>(i);
            node.old_off = in.old_off;
            node.old_size = in.old_size;
            node.src_line = in.src_line;
            node.src_col = in.src_col;
            node.may_throw = in.may_throw;
            node.effect = in.effect;
            node.exc_succ = -1;
            node.token_in = 0;
            node.token_out = 0;
            const int n_pop = insn_pop_count(in);
            const int base =
                is_shuffle(in.op) ? shuffle_def(in.op)->base : 0;
            if (static_cast<int>(stack.size()) < n_pop + base) {
                *error = "ssa: stack underflow at instruction " +
                         std::to_string(i);
                return false;
            }
            for (int k = 0; k < n_pop; k++) {
                node.args.push_back(stack.back());
                stack.pop_back();
            }
            for (int k = 0; k < base; k++) {
                // Cells below the pop window (dup1 needs its deep
                // source). They stay on the stack — only referenced.
                node.args.push_back(stack[stack.size() - 1 - k]);
            }
            if (is_frame_slot_store(in.op)) {
                const uint32_t si = static_cast<uint32_t>(
                    slot_index(in.op, in.aux));
                std::vector<uint32_t>& slots =
                    (slot_space(in.op) == SlotSpace::ARG) ? cur_args
                                                          : cur_locs;
                if (in.op == OP_set_loc_uninitialized) {
                    // set_loc_uninitialized pops/pushes nothing: it
                    // writes the UNINITIALIZED sentinel and the
                    // overwritten slot value is freed (set_value).
                    node.args.push_back(slots[si]);
                    const uint32_t sent = A.new_val(Lattice::UNINITIALIZED,
                                                    true, false, 0, false);
                    node.slot_writes.push_back(sent);
                    slots[si] = sent;
                } else {
                    if (node.args.empty()) {
                        *error = "ssa: store without value at instruction " +
                                 std::to_string(i);
                        return false;
                    }
                    const uint32_t v = node.args[0];
                    // The overwritten slot value (trailing arg) is
                    // freed by the interpreter's set_value.
                    node.args.push_back(slots[si]);
                    if (in.n_push == 1) {
                        // set_loc family: the popped value stays on the
                        // stack (set_value(&slot, js_dup(sp[-1])) never
                        // touches sp); the slot receives a fresh js_dup
                        // copy.
                        node.results.push_back(v);
                        const uint32_t dup = A.new_val(Lattice::BOTTOM,
                                                       false, false, 0,
                                                       false);
                        node.slot_writes.push_back(dup);
                        slots[si] = dup;
                    } else {
                        // put_loc family: the popped ref moves into the
                        // slot.
                        node.slot_writes.push_back(v);
                        slots[si] = v;
                    }
                }
            } else if (is_loc_rmw(in.op)) {
                const uint32_t si = static_cast<uint32_t>(
                    slot_index(in.op, in.aux));
                // inc_loc/dec_loc/add_loc push nothing: the old slot
                // value is consumed (freed) and replaced in place.
                node.args.push_back(cur_locs[si]);
                const uint32_t nv =
                    A.new_val(Lattice::UNKNOWN, false, false, 0, false);
                node.slot_writes.push_back(nv);
                cur_locs[si] = nv;
            } else if (is_catch_marker_push(in.op)) {
                const uint32_t marker = A.new_val(Lattice::UNKNOWN, true,
                                                  false, 0, false);
                node.results.push_back(marker);
                // The marker's region is the CATCH edge target pushed
                // at this same instruction.
                for (size_t e = 0; e < blk.edges.size(); e++) {
                    if (blk.edges[e].src_insn != i) continue;
                    if (blk.edges[e].kind == EdgeKind::CATCH) {
                        A.marker_region[marker] =
                            static_cast<int32_t>(blk.edges[e].to);
                        break;
                    }
                }
            } else if (is_shuffle(in.op)) {
                const ShuffleDef& sd = *shuffle_def(in.op);
                if (sd.n != in.n_push) {
                    *error = "ssa: shuffle table mismatch at instruction " +
                             std::to_string(i);
                    return false;
                }
                for (uint8_t k = 0; k < sd.n; k++) {
                    const uint32_t src = node.args[sd.src[k]];
                    if (sd.fresh[k]) {
                        node.results.push_back(
                            A.new_val(Lattice::BOTTOM, false, false, 0,
                                      false));
                    } else {
                        node.results.push_back(src);
                    }
                }
            } else {
                for (int k = 0; k < in.n_push; k++) {
                    node.results.push_back(
                        A.new_val(Lattice::BOTTOM, false, false, 0, false));
                }
            }
            for (size_t k = 0; k < node.results.size(); k++) {
                stack.push_back(node.results[k]);
            }
            // Edge snapshots at this instruction (post state).
            for (size_t e = 0; e < blk.edges.size(); e++) {
                if (blk.edges[e].src_insn != i) continue;
                SsaEdgeSnap& snap = sb.edge_snaps[e];
                snap.stack = stack;
                snap.args = cur_args;
                snap.locs = cur_locs;
            }
            sb.nodes.push_back(node);
            if (is_terminator(in.op) || is_uncond_jump(in.op)) break;
        }
    }
    uint32_t param_count = 0;
    for (uint32_t v = 0; v < out->value_count; v++) {
        if (out->is_param[v]) param_count++;
    }
    out->param_count = param_count;

    // -------------------------------------------------------------------
    // Analyses: lattice / world token / exception successors / refcounts
    // to a monotone fixpoint. Per pass over the RPO: the block's entry
    // handlers/token/refs are joined from the predecessor edge snaps
    // (filled during the preds' simulations), the multi-predecessor
    // block parameters' lattices join the incoming values, then the
    // block's nodes re-simulate the transfers, releases, exception
    // successors, and region pushes/pops.
    // -------------------------------------------------------------------
    std::vector<AState> states(nb);
    std::vector<uint32_t> token_inject(nb, 0);
    std::vector<int> final_refs(out->value_count, -1);
    for (size_t b = 0; b < nb; b++) {
        states[b].token = 0;
        states[b].refs.assign(out->value_count, -1);
        states[b].entry_refs.assign(out->value_count, -1);
    }

    const int kMaxIter =
        2 * (static_cast<int>(nb) + static_cast<int>(out->value_count)) + 32;
    bool converged = false;
    for (int iter = 0; iter < kMaxIter; iter++) {
        bool changed = false;
        for (size_t ri = 0; ri < A.rpo.size(); ri++) {
            const uint32_t b = A.rpo[ri];
            const Block& blk = cfg.blocks[b];
            SsaBlock& sb = out->blocks[b];
            AState& st = states[b];

            // ---- entry: handler stack (all-incoming-equal join)
            std::vector<int32_t> handlers;
            bool first = true;
            bool equal = true;
            for (size_t pi = 0; pi < A.preds[b].size() && equal; pi++) {
                const SsaEdgeSnap& snap =
                    out->blocks[A.preds[b][pi].first]
                        .edge_snaps[A.preds[b][pi].second];
                if (first) {
                    handlers = snap.handler;
                    first = false;
                } else if (snap.handler != handlers) {
                    equal = false;
                }
            }
            if (!equal) handlers.clear();

            // ---- entry: world token (max over pred edges + exception
            // flows into this block, accumulated monotonically).
            // Backedges are excluded: the loop-carried edge re-injects
            // the loop body's own effect count, which would grow the
            // token unboundedly around a cycle. The entry token is the
            // max over the acyclic paths; the loop body's nodes count
            // the loop's effects themselves (their tokens start at the
            // loop head's entry token every iteration).
            uint32_t token = token_inject[b];
            for (size_t pi = 0; pi < A.preds[b].size(); pi++) {
                const uint32_t pb = A.preds[b][pi].first;
                const uint32_t pe = A.preds[b][pi].second;
                if (cfg.blocks[pb].edges[pe].backedge) continue;
                const uint32_t t = out->blocks[pb].edge_snaps[pe].token;
                if (t > token) token = t;
            }

            // ---- entry: refcounts
            std::vector<int> entry_refs(out->value_count, -1);
            if (b == 0) {
                for (size_t p = 0; p < out->entry_args.size(); p++) {
                    entry_refs[out->entry_args[p]] = 1;
                }
                for (size_t p = 0; p < out->entry_locs.size(); p++) {
                    entry_refs[out->entry_locs[p]] = 0;  // sentinel
                }
            } else if (A.pred_count[b] == 1) {
                const SsaEdgeSnap& snap =
                    out->blocks[A.pred_block[b]].edge_snaps[A.pred_edge[b]];
                for (size_t p = 0; p < snap.stack.size(); p++) {
                    entry_refs[snap.stack[p]] =
                        snap.stack_refs.empty() ? 1 : snap.stack_refs[p];
                }
                for (size_t p = 0; p < snap.args.size(); p++) {
                    entry_refs[snap.args[p]] =
                        snap.args_refs.empty() ? 1 : snap.args_refs[p];
                }
                for (size_t p = 0; p < snap.locs.size(); p++) {
                    entry_refs[snap.locs[p]] =
                        snap.locs_refs.empty() ? 1 : snap.locs_refs[p];
                }
                // Phantoms (the gosub/with_* +1/+2 entry slots) are
                // born owned.
                for (size_t p = snap.stack.size(); p < sb.entry_stack.size();
                     p++) {
                    entry_refs[sb.entry_stack[p]] = 1;
                }
            } else {
                // Multi-pred: a param's refs are the min over the
                // incoming values' refs at its position (phantom
                // incoming contributes 1); the same value id carried on
                // several paths takes the min over the paths carrying
                // it. Two sentinel-aware corrections to that min:
                //  - a position whose incoming values are ALL sentinels
                //    (a catch marker on every path — a try body that
                //    joins before its drop — or never-written storage)
                //    births a borrowed parameter: its later region-end
                //    drop / store release must be a no-op, not an
                //    ownership violation;
                //  - with any real value among the incoming (a
                //    conditionally-written slot), the parameter is
                //    real and holds exactly one owning reference, so
                //    the min cannot go below 1 — a path carrying a
                //    sentinel has no ref to min in.
                auto join_refs = [&](const std::vector<uint32_t>& entry,
                                     size_t p) {
                    int r = INT_MAX;
                    bool all_sent = true;
                    for (size_t pi = 0; pi < A.preds[b].size(); pi++) {
                        const SsaEdgeSnap& snap =
                            out->blocks[A.preds[b][pi].first]
                                .edge_snaps[A.preds[b][pi].second];
                        if (p >= snap.stack.size()) {
                            // Phantom incoming (gosub/with_* entry
                            // slot): a real owned value.
                            r = std::min(r, 1);
                            all_sent = false;
                            continue;
                        }
                        r = std::min(r, snap.stack_refs.empty()
                                            ? 1
                                            : snap.stack_refs[p]);
                        if (!out->sentinel[snap.stack[p]]) all_sent = false;
                    }
                    const uint32_t param = entry[p];
                    if (all_sent) {
                        if (!out->sentinel[param]) {
                            out->sentinel[param] = 1;
                            changed = true;
                        }
                    } else if (r < 1) {
                        r = 1;
                    }
                    entry_refs[param] = r;
                };
                for (size_t p = 0; p < sb.entry_stack.size(); p++) {
                    join_refs(sb.entry_stack, p);
                }
                for (size_t p = 0; p < sb.entry_args.size(); p++) {
                    int r = INT_MAX;
                    bool all_sent = true;
                    for (size_t pi = 0; pi < A.preds[b].size(); pi++) {
                        const SsaEdgeSnap& snap =
                            out->blocks[A.preds[b][pi].first]
                                .edge_snaps[A.preds[b][pi].second];
                        r = std::min(
                            r, snap.args_refs.empty() ? 1 : snap.args_refs[p]);
                        if (!out->sentinel[snap.args[p]]) all_sent = false;
                    }
                    const uint32_t param = sb.entry_args[p];
                    if (all_sent) {
                        if (!out->sentinel[param]) {
                            out->sentinel[param] = 1;
                            changed = true;
                        }
                    } else if (r < 1) {
                        r = 1;
                    }
                    entry_refs[param] = r;
                }
                for (size_t p = 0; p < sb.entry_locs.size(); p++) {
                    int r = INT_MAX;
                    bool all_sent = true;
                    for (size_t pi = 0; pi < A.preds[b].size(); pi++) {
                        const SsaEdgeSnap& snap =
                            out->blocks[A.preds[b][pi].first]
                                .edge_snaps[A.preds[b][pi].second];
                        r = std::min(
                            r, snap.locs_refs.empty() ? 1 : snap.locs_refs[p]);
                        if (!out->sentinel[snap.locs[p]]) all_sent = false;
                    }
                    const uint32_t param = sb.entry_locs[p];
                    if (all_sent) {
                        if (!out->sentinel[param]) {
                            out->sentinel[param] = 1;
                            changed = true;
                        }
                    } else if (r < 1) {
                        r = 1;
                    }
                    entry_refs[param] = r;
                }
            }

            // ---- entry: multi-predecessor parameter lattices (join
            // over the incoming values; short snaps contribute UNKNOWN)
            if (b != 0 && A.pred_count[b] > 1) {
                static const std::vector<uint32_t> kNoShapes;
                auto join_stack = [&](uint32_t param, size_t p) {
                    Lattice jlat = Lattice::BOTTOM;
                    int64_t jimm = 0;
                    bool jhas = false;
                    std::vector<uint32_t> jshapes;
                    for (size_t pi = 0; pi < A.preds[b].size(); pi++) {
                        const SsaEdgeSnap& snap =
                            out->blocks[A.preds[b][pi].first]
                                .edge_snaps[A.preds[b][pi].second];
                        Lattice ilat = Lattice::UNKNOWN;
                        int64_t iimm = 0;
                        bool ihas = false;
                        const std::vector<uint32_t>* ish = &kNoShapes;
                        if (p < snap.stack.size()) {
                            const uint32_t v = snap.stack[p];
                            ilat = out->lattice[v];
                            iimm = out->imm[v];
                            ihas = out->has_imm[v] != 0;
                            if (ilat == Lattice::OBJECT_SHAPES) {
                                ish = &out->shapes[v];
                            }
                        }
                        int64_t oi;
                        bool oh;
                        std::vector<uint32_t> od;
                        jlat = join_lattice(jlat, jimm, jhas, ilat, iimm,
                                            ihas, &oi, &oh, jshapes,
                                            *ish, &od);
                        jimm = oi;
                        jhas = oh;
                        jshapes = (jlat == Lattice::OBJECT_SHAPES)
                                      ? od
                                      : std::vector<uint32_t>();
                    }
                    if (jlat != out->lattice[param] ||
                        (out->has_imm[param] != 0) != jhas ||
                        (jhas && out->imm[param] != jimm) ||
                        out->shapes[param] != jshapes) {
                        changed = true;
                    }
                    out->lattice[param] = jlat;
                    out->has_imm[param] = jhas ? 1 : 0;
                    out->imm[param] = jimm;
                    out->shapes[param] = jshapes;
                };
                for (size_t p = 0; p < sb.entry_stack.size(); p++) {
                    join_stack(sb.entry_stack[p], p);
                }
                for (size_t p = 0; p < sb.entry_args.size(); p++) {
                    Lattice jlat = Lattice::BOTTOM;
                    int64_t jimm = 0;
                    bool jhas = false;
                    std::vector<uint32_t> jshapes;
                    for (size_t pi = 0; pi < A.preds[b].size(); pi++) {
                        const SsaEdgeSnap& snap =
                            out->blocks[A.preds[b][pi].first]
                                .edge_snaps[A.preds[b][pi].second];
                        const uint32_t v = snap.args[p];
                        int64_t oi;
                        bool oh;
                        std::vector<uint32_t> od;
                        jlat = join_lattice(jlat, jimm, jhas,
                                            out->lattice[v],
                                            out->imm[v],
                                            out->has_imm[v] != 0,
                                            &oi, &oh, jshapes,
                                            out->lattice[v] ==
                                                    Lattice::OBJECT_SHAPES
                                                ? out->shapes[v]
                                                : kNoShapes,
                                            &od);
                        jimm = oi;
                        jhas = oh;
                        jshapes = (jlat == Lattice::OBJECT_SHAPES)
                                      ? od
                                      : std::vector<uint32_t>();
                    }
                    const uint32_t param = sb.entry_args[p];
                    if (jlat != out->lattice[param] ||
                        (out->has_imm[param] != 0) != jhas ||
                        (jhas && out->imm[param] != jimm) ||
                        out->shapes[param] != jshapes) {
                        changed = true;
                    }
                    out->lattice[param] = jlat;
                    out->has_imm[param] = jhas ? 1 : 0;
                    out->imm[param] = jimm;
                    out->shapes[param] = jshapes;
                }
                for (size_t p = 0; p < sb.entry_locs.size(); p++) {
                    Lattice jlat = Lattice::BOTTOM;
                    int64_t jimm = 0;
                    bool jhas = false;
                    std::vector<uint32_t> jshapes;
                    for (size_t pi = 0; pi < A.preds[b].size(); pi++) {
                        const SsaEdgeSnap& snap =
                            out->blocks[A.preds[b][pi].first]
                                .edge_snaps[A.preds[b][pi].second];
                        const uint32_t v = snap.locs[p];
                        int64_t oi;
                        bool oh;
                        std::vector<uint32_t> od;
                        jlat = join_lattice(jlat, jimm, jhas,
                                            out->lattice[v],
                                            out->imm[v],
                                            out->has_imm[v] != 0,
                                            &oi, &oh, jshapes,
                                            out->lattice[v] ==
                                                    Lattice::OBJECT_SHAPES
                                                ? out->shapes[v]
                                                : kNoShapes,
                                            &od);
                        jimm = oi;
                        jhas = oh;
                        jshapes = (jlat == Lattice::OBJECT_SHAPES)
                                      ? od
                                      : std::vector<uint32_t>();
                    }
                    const uint32_t param = sb.entry_locs[p];
                    if (jlat != out->lattice[param] ||
                        (out->has_imm[param] != 0) != jhas ||
                        (jhas && out->imm[param] != jimm) ||
                        out->shapes[param] != jshapes) {
                        changed = true;
                    }
                    out->lattice[param] = jlat;
                    out->has_imm[param] = jhas ? 1 : 0;
                    out->imm[param] = jimm;
                    out->shapes[param] = jshapes;
                }
            }

            // ---- compare entry state (changed detection)
            if (handlers != st.handlers || token != st.token ||
                entry_refs != st.entry_refs) {
                changed = true;
            }
            st.handlers = handlers;
            st.token = token;
            // Publish the joined entry token on the block (the nodes
            // also carry it; the block-level field is what the census
            // and the callers read).
            sb.token_in = token;
            st.entry_refs = entry_refs;
            st.refs = entry_refs;
            for (size_t p = 0; p < entry_refs.size(); p++) {
                if (entry_refs[p] != -1) final_refs[p] = entry_refs[p];
            }

            // ---- entry slot ids (the construction's record)
            std::vector<uint32_t> cur_args = sb.entry_args;
            std::vector<uint32_t> cur_locs = sb.entry_locs;

            // ---- simulate the nodes. The simulation runs on a local
            // copy of the entry handler stack: st.handlers is the
            // changed-detection baseline (entry state), so the region
            // pushes/pops must never mutate it.
            std::vector<int32_t> sim_handlers = st.handlers;
            uint32_t t = st.token;
            for (size_t ni = 0; ni < sb.nodes.size(); ni++) {
                SsaNode& node = sb.nodes[ni];
                const Insn& in = cfg.insns[node.insn];

                // World token.
                node.token_in = t;
                node.token_out =
                    t + (effect_touches_world(node.effect) ? 1U : 0U);
                t = node.token_out;

                // Value lattice.
                transfer(A, node, out, cur_args, cur_locs, &changed);

                // Refcounts (may reject the function).
                if (!ownership(A, node, out, st, &final_refs)) {
                    return false;
                }

                // Slot state (mirror the construction).
                if (is_frame_slot_store(in.op)) {
                    const uint32_t si = static_cast<uint32_t>(
                        slot_index(in.op, in.aux));
                    std::vector<uint32_t>& slots =
                        (slot_space(in.op) == SlotSpace::ARG) ? cur_args
                                                              : cur_locs;
                    slots[si] = node.slot_writes[0];
                } else if (is_loc_rmw(in.op)) {
                    const uint32_t si = static_cast<uint32_t>(
                        slot_index(in.op, in.aux));
                    cur_locs[si] = node.slot_writes[0];
                }

                // Exception successor: the top of the handler stack at
                // this point (before any region-end pop of this node).
                // The SIMULATED stack, not the entry baseline: regions
                // pushed earlier in this block (a catch inside the
                // block) and regions closed by an earlier region-end
                // drop both take effect here.
                node.exc_succ = (node.may_throw && !sim_handlers.empty())
                                    ? sim_handlers.back()
                                    : -1;
                if (node.exc_succ >= 0) {
                    if (node.token_out > token_inject[node.exc_succ]) {
                        token_inject[node.exc_succ] = node.token_out;
                    }
                }

                // Region end: a drop/nip/nip1/nip_catch popping a
                // sentinel (the catch marker / uninitialized storage)
                // pops the handler stack.
                const bool region_end =
                    in.op == OP_drop || in.op == OP_nip || in.op == OP_nip1 ||
                    in.op == OP_nip_catch;
                if (region_end) {
                    bool sentinel = false;
                    uint32_t sentinel_arg = 0;
                    for (size_t k = 0; k < node.args.size(); k++) {
                        if (out->sentinel[node.args[k]]) {
                            sentinel = true;
                            sentinel_arg = node.args[k];
                            break;
                        }
                    }
                    if (sentinel && !sim_handlers.empty()) {
                        // The dropped marker's own region must be the
                        // current top: the handler-entry drop (whose
                        // CATCH edge already exited the region) must
                        // not pop the outer handler. Uninitialized
                        // storage sentinels pop unconditionally.
                        const int32_t region = A.marker_region[sentinel_arg];
                        if (region < 0 || sim_handlers.back() == region) {
                            sim_handlers.pop_back();
                        }
                    }
                }

                // Edge snapshots at this node (the analysis fills the
                // refs/handler/token; the values came from the
                // construction). CATCH edges record the PRE-push state.
                for (size_t e = 0; e < blk.edges.size(); e++) {
                    if (blk.edges[e].src_insn != node.insn) continue;
                    if (blk.edges[e].kind != EdgeKind::CATCH) continue;
                    SsaEdgeSnap& snap = sb.edge_snaps[e];
                    snap.handler = sim_handlers;
                    snap.token = node.token_out;
                    snap.stack_refs.assign(snap.stack.size(), 0);
                    for (size_t p = 0; p < snap.stack.size(); p++) {
                        snap.stack_refs[p] = st.refs[snap.stack[p]];
                    }
                    snap.args_refs.assign(snap.args.size(), 0);
                    for (size_t p = 0; p < snap.args.size(); p++) {
                        snap.args_refs[p] = st.refs[snap.args[p]];
                    }
                    snap.locs_refs.assign(snap.locs.size(), 0);
                    for (size_t p = 0; p < snap.locs.size(); p++) {
                        snap.locs_refs[p] = st.refs[snap.locs[p]];
                    }
                }
                if (is_catch_marker_push(in.op)) {
                    for (size_t e = 0; e < blk.edges.size(); e++) {
                        if (blk.edges[e].src_insn != node.insn) continue;
                        if (blk.edges[e].kind == EdgeKind::CATCH) {
                            sim_handlers.push_back(blk.edges[e].to);
                        }
                    }
                }
                for (size_t e = 0; e < blk.edges.size(); e++) {
                    if (blk.edges[e].src_insn != node.insn) continue;
                    if (blk.edges[e].kind == EdgeKind::CATCH) continue;
                    SsaEdgeSnap& snap = sb.edge_snaps[e];
                    snap.handler = sim_handlers;
                    snap.token = node.token_out;
                    snap.stack_refs.assign(snap.stack.size(), 0);
                    for (size_t p = 0; p < snap.stack.size(); p++) {
                        snap.stack_refs[p] = st.refs[snap.stack[p]];
                    }
                    snap.args_refs.assign(snap.args.size(), 0);
                    for (size_t p = 0; p < snap.args.size(); p++) {
                        snap.args_refs[p] = st.refs[snap.args[p]];
                    }
                    snap.locs_refs.assign(snap.locs.size(), 0);
                    for (size_t p = 0; p < snap.locs.size(); p++) {
                        snap.locs_refs[p] = st.refs[snap.locs[p]];
                    }
                }
            }
            // st.token stays the entry token (the changed-detection
            // baseline at the next iteration); the exit token lives in
            // the edge snaps only.
        }
        if (!changed) {
            converged = true;
            break;
        }
    }
    if (!converged) {
        *error = "ssa: analysis did not converge";
        return false;
    }

    // -------------------------------------------------------------------
    // Ownership census: sentinels are BORROWED; values a js_dup copy
    // was taken from are DUPLICATED; values whose owning reference was
    // released/stored are CONSUMED; the rest are OWNED.
    // -------------------------------------------------------------------
    std::vector<uint8_t> copied(out->value_count, 0);
    for (size_t b = 0; b < nb; b++) {
        const SsaBlock& sb = out->blocks[b];
        for (size_t ni = 0; ni < sb.nodes.size(); ni++) {
            const SsaNode& node = sb.nodes[ni];
            const uint8_t op = cfg.insns[node.insn].op;
            if (is_shuffle(op)) {
                const ShuffleDef& sd = *shuffle_def(op);
                for (uint8_t k = 0; k < sd.n && k < node.results.size();
                     k++) {
                    if (sd.fresh[k]) copied[node.args[sd.src[k]]] = 1;
                }
            } else if (is_frame_slot_store(op) &&
                       cfg.insns[node.insn].n_push == 1) {
                // set_loc family: the slot holds a fresh js_dup of the
                // stored stack value.
                copied[node.args[0]] = 1;
            }
        }
    }
    for (uint32_t v = 0; v < out->value_count; v++) {
        if (out->sentinel[v]) {
            out->ownership[v] = Ownership::BORROWED;
        } else if (copied[v]) {
            out->ownership[v] = Ownership::DUPLICATED;
        } else if (final_refs[v] == 0) {
            out->ownership[v] = Ownership::CONSUMED;
        } else {
            out->ownership[v] = Ownership::OWNED;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Whole-bundle analyze-only walker (§3.1).
// ---------------------------------------------------------------------------

namespace {
void count_function(const std::vector<FuncInfo>& funcs, uint64_t* count) {
    for (size_t i = 0; i < funcs.size(); i++) {
        (*count)++;
        count_function(funcs[i].children, count);
    }
}
}  // namespace

bool ssa_round_trip(const uint8_t* data,
                    size_t size,
                    SsaReport* out,
                    std::string* error) {
    std::vector<FuncInfo> functions;
    if (!read_functions(data, size, &functions, error)) return false;
    SsaReport rep;
    rep.functions = 0;
    rep.rejected_functions = 0;
    rep.rejected_insns = 0;
    rep.nodes = 0;
    rep.values = 0;
    rep.params = 0;
    rep.max_token = 0;
    for (size_t i = 0; i < 10; i++) rep.lattice_count[i] = 0;
    count_function(functions, &rep.functions);

    struct Walker {
        const uint8_t* data;
        SsaReport* rep;
        bool run(const FuncInfo& fi, std::string* error) {
            std::vector<Insn> insns;
            if (!decode_function(data + fi.code_off, fi.code_len, data, fi,
                                 &insns, error)) {
                std::fprintf(stderr, "ssa: rejected (decode): %s\n",
                             error->c_str());
                rep->rejected_functions++;
                error->clear();
                return true;
            }
            Cfg cfg;
            if (!build_cfg(insns, &cfg, error)) {
                std::fprintf(stderr, "ssa: rejected (build): %s\n",
                             error->c_str());
                rep->rejected_functions++;
                rep->rejected_insns += static_cast<uint64_t>(insns.size());
                error->clear();
                return true;
            }
            cfg.recorded_stack_size = fi.stack_size;
            if (!verify_cfg(cfg, error)) {
                std::fprintf(stderr, "ssa: rejected (verify): %s\n",
                             error->c_str());
                rep->rejected_functions++;
                rep->rejected_insns += static_cast<uint64_t>(insns.size());
                error->clear();
                return true;
            }
            SsaFunc ssa;
            if (!ssa_analyze_function(cfg, &ssa, error)) {
                std::fprintf(stderr, "ssa: rejected (ssa): %s\n",
                             error->c_str());
                rep->rejected_functions++;
                rep->rejected_insns += static_cast<uint64_t>(insns.size());
                error->clear();
                return true;
            }
            for (size_t b = 0; b < ssa.blocks.size(); b++) {
                rep->nodes += static_cast<uint64_t>(ssa.blocks[b].nodes.size());
                for (size_t n = 0; n < ssa.blocks[b].nodes.size(); n++) {
                    const uint32_t t = ssa.blocks[b].nodes[n].token_out;
                    if (t > rep->max_token) rep->max_token = t;
                }
            }
            rep->values += static_cast<uint64_t>(ssa.value_count);
            rep->params += static_cast<uint64_t>(ssa.param_count);
            for (size_t v = 0; v < ssa.lattice.size(); v++) {
                rep->lattice_count[static_cast<size_t>(ssa.lattice[v])]++;
            }
            for (size_t i = 0; i < fi.children.size(); i++) {
                if (!run(fi.children[i], error)) return false;
            }
            return true;
        }
    };
    Walker w;
    w.data = data;
    w.rep = &rep;
    for (size_t i = 0; i < functions.size(); i++) {
        if (!w.run(functions[i], error)) return false;
    }
    *out = rep;
    return true;
}

}  // namespace ir
}  // namespace bytecode
}  // namespace capsid
