// I1: full-stack SSA (docs/quickjs-optimization.md §2). Analyze-only:
// this module builds the SSA form of a
// verified CFG, runs the lattice / effect-token / exception-successor /
// ownership analyses, and emits nothing. Functions the analyses cannot
// prove are rejected (fail-closed) and counted as rejected coverage by
// the ssa_round_trip walker; their bundles stay byte-for-byte BC26.
//
// Form (§3.3): every live operand-stack position and every used
// non-captured frame slot (arguments and locals are separate index
// spaces) has a value at each block entry. Multi-predecessor blocks
// take a block parameter (phi) per position; single-predecessor blocks
// flow the predecessor's edge-snapshot value through. Join positions
// the edges cannot name (the gosub/with_* +1/+2 entry slots, i.e. the
// finally return-address and scope slots, and the with_put_var -1
// drop) become fresh "phantom" values that are never read — they only
// exist to keep the entry height consistent. Values are dense ids
// [0, param_count) for parameters, then node results in creation
// order; the lattice/imm/ownership/sentinel arrays are parallel.
//
// Analyses:
//  - value lattice (§3.3): BOTTOM at creation, transfer per opcode,
//    monotone join at parameters; UNINITIALIZED is the runtime state
//    of a never-written local (a sentinel), distinct from the metadata
//    state UNKNOWN; INT32 folds small-int immediates.
//  - one ordered world token (§3.3 "version 1 uses ONE ordered
//    world/effect token"): +1 per node whose effect touches the world
//    (>= MEMORY), joined by max at blocks and across exception flows.
//  - exception successors (§3.4): per-block handler-stack fixpoint.
//    catch pushes its CATCH-edge target; the region ends when a
//    drop/nip/nip1/nip_catch pops a sentinel value (the catch marker
//    or the uninitialized sentinel); joins require all-incoming-equal,
//    else the conservative empty stack (exc_succ = -1, function
//    boundary). may_throw nodes record the top of the stack.
//  - ownership proof (§3.4): per-block refcount simulation. Values are
//    born OWNED (refs 1) or BORROWED (sentinels, refs 0). RELEASE pops
//    require refs >= 1 (sentinels are exempt — their release is a
//    no-op); NIP releases only the last pop; ALIAS releases nothing;
//    frame-slot stores consume the stored value and release the old
//    slot value; get_loc copies are fresh owning references. A release
//    of a non-sentinel with refs 0 is a rejected function.
#ifndef CAPSID_SRC_BYTECODE_REWRITER_IR_SSA_H
#define CAPSID_SRC_BYTECODE_REWRITER_IR_SSA_H

#include <cstdint>
#include <string>
#include <vector>

#include "bytecode_rewriter/ir/cfg.h"

namespace capsid {
namespace bytecode {
namespace ir {

// §3.3 value lattice. BOTTOM is the analysis initial state; the
// runtime state UNINITIALIZED (never-written local) is distinct from
// the metadata state UNKNOWN. Joins move monotonically toward UNKNOWN.
enum class Lattice : uint8_t {
    BOTTOM = 0,
    UNINITIALIZED,
    INT32,
    FLOAT64,
    NUMBER,
    STRING,
    FAST_ARRAY,
    OBJECT_SHAPES,   // shape-id set (see shapes[]), merged up to 2 ids
    EXACT_CLOSURE,   // exact function (cpool path in imm)
    UNKNOWN,
};

// §3.4 ownership states, as reported per value after the analysis:
//   BORROWED   — sentinel (catch marker / uninitialized storage):
//                non-owning, refs 0, releases are no-ops
//   CONSUMED   — the single owning reference was released or stored
//                (refs reached 0 at the value's last live block)
//   DUPLICATED — a fresh js_dup copy of the value existed (runtime
//                refcount 2; the copy itself is its own value)
//   OWNED      — exactly one owning reference remains
enum class Ownership : uint8_t {
    BORROWED = 0,
    OWNED,
    CONSUMED,
    DUPLICATED,
};

// One SSA node. args are the popped operand-stack values, top-first;
// results are the pushed values in push order. For frame-slot stores
// the stored value is args[0] and the overwritten slot value is an
// extra trailing arg (released, per the interpreter's set_value); for
// inc_loc/dec_loc/add_loc the pushed value is the old slot value and
// slot_writes[0] is the fresh replacement slot value (never pushed).
struct SsaNode {
    uint32_t insn;         // index into Cfg::insns
    uint32_t old_off;      // original PC (kept for later lowering)
    uint8_t old_size;
    uint16_t src_line;
    uint16_t src_col;
    bool may_throw;
    EffectClass effect;
    std::vector<uint32_t> args;       // top-first popped values
    std::vector<uint32_t> results;    // pushed values, in push order
    std::vector<uint32_t> slot_writes;  // slot value ids written
                                        // (stores: the stored value;
                                        // RMW: the fresh replacement)
    int32_t exc_succ;      // exception-successor block, or -1 (function
                           // boundary / no handler)
    uint32_t token_in;     // world token on entry
    uint32_t token_out;
};

// Per-edge value snapshot at the edge's source instruction (the post
// state). The join target's parameters read their incoming values from
// these. `refs` are the source block's local refcounts of the same
// values at the edge (the path-local refcount discipline: the same
// value id can have different refs on different paths).
struct SsaEdgeSnap {
    std::vector<uint32_t> stack;  // bottom-first values
    std::vector<uint32_t> args;   // arg-space slot values (used slots)
    std::vector<uint32_t> locs;   // loc-space slot values
    std::vector<int> stack_refs;  // parallel refs
    std::vector<int> args_refs;
    std::vector<int> locs_refs;
    std::vector<int32_t> handler;  // handler-stack at the edge's post
                                   // (for CATCH edges: at the pre-push
                                   // state — the handler block's own
                                   // region is the outer one)
    uint32_t token;                // world token at the edge's post
};

struct SsaBlock {
    uint32_t block;              // back reference to Cfg::blocks
    std::vector<SsaNode> nodes;  // live instructions, in order
    std::vector<SsaEdgeSnap> edge_snaps;  // parallel to Cfg Block::edges
    // Entry value ids, as constructed: multi-predecessor blocks carry a
    // fresh block parameter per position; single-predecessor blocks the
    // predecessor's snapshot values (plus fresh phantoms for the
    // gosub/with_* +1/+2 slots the snapshot cannot name); block 0 the
    // function entry state. The analyses read these to re-derive the
    // per-block slot state and the parameter joins.
    std::vector<uint32_t> entry_stack;  // bottom-first
    std::vector<uint32_t> entry_args;   // arg-space slot values
    std::vector<uint32_t> entry_locs;   // loc-space slot values
    uint32_t token_in;
};

// The SSA function. Value table: block parameters first
// ([0, param_count)), then node results in creation order.
struct SsaFunc {
    uint32_t param_count;
    uint32_t value_count;
    std::vector<Lattice> lattice;
    std::vector<int64_t> imm;     // valid when lattice==INT32 && has_imm
                                  // (small-int fold) or EXACT_CLOSURE
                                  // (cpool path)
    std::vector<uint8_t> has_imm;
    std::vector<Ownership> ownership;
    std::vector<uint8_t> sentinel;  // catch marker / uninitialized value
    std::vector<uint8_t> is_param;
    std::vector<std::vector<uint32_t> > shapes;  // OBJECT_SHAPES id sets
    std::vector<SsaBlock> blocks;
    // Block-0 entry slot values (function entry state): args are
    // unknown caller values, locs are UNINITIALIZED sentinels.
    std::vector<uint32_t> entry_args;
    std::vector<uint32_t> entry_locs;
    uint32_t args_slot_count;  // used arg-slot count
    uint32_t loc_slot_count;   // used loc-slot count
};

// Builds the SSA and runs all four analyses (lattice, world token,
// exception successors, ownership) to a monotone fixpoint. Returns
// false (fail-closed, with `error`) on an ownership violation — a
// release of a non-sentinel value with refs < 1 — or on non-convergence;
// the caller counts the function as rejected coverage.
bool ssa_analyze_function(const Cfg& cfg, SsaFunc* out, std::string* error);

// Whole-bundle analyze-only walker (§3.1): read_functions + per-function
// decode -> build -> verify -> ssa_analyze_function, counting rejected
// coverage (functions whose CFG or SSA cannot prove them). One-line
// reports to stderr, nothing else. Always analyze-only.
struct SsaReport {
    uint64_t functions;
    uint64_t rejected_functions;  // decode/build/verify/ssa rejects
    uint64_t rejected_insns;
    uint64_t nodes;
    uint64_t values;
    uint64_t params;
    uint64_t max_token;
    uint64_t lattice_count[10];   // indexed by Lattice
};
bool ssa_round_trip(const uint8_t* data,
                    size_t size,
                    SsaReport* out,
                    std::string* error);

}  // namespace ir
}  // namespace bytecode
}  // namespace capsid

#endif  // CAPSID_SRC_BYTECODE_REWRITER_IR_SSA_H
