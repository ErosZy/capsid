// I1 side-effect and exception classification (tier-3 plan §3.4).
//
// The I0 CFG metadata columns (may_throw / effect class / result kind)
// live here rather than in cfg.h so that the effect model is the shared
// contract of every consumer (the I0 verifier, the I1 SSA, and later
// I2 fusion analysis). The classification is conservative by design:
// the default is {may_throw, CALL, ANY} — an opcode that is not
// explicitly classified here can never make an analysis unsound, only
// less precise.
//
// The I1 additions (PopOwnership, frame-slot/marker/shuffle helpers)
// pin the refcount semantics verified against the vendored interpreter:
// every pop is RELEASE (the value is freed), ALIAS (a stack shuffle
// that frees nothing — results keep or duplicate the popped values), or
// NIP (nip/nip1 free only the last popped value); catch pushes a
// BORROWED marker (JS_NewCatchOffset, never freed) whose later drop
// ends the exception region; frame-slot stores free the overwritten
// slot value; get_loc copies are fresh owning references.
#ifndef CAPSID_SRC_BYTECODE_OPTIMIZER_IR_EFFECTS_H
#define CAPSID_SRC_BYTECODE_OPTIMIZER_IR_EFFECTS_H

#include <cstdint>

namespace capsid {
namespace bytecode {
namespace ir {

// Effect strength ordering (weakest -> strongest). The I1 world/effect
// token joins by max so that a candidate can never move across a
// stronger effect: PURE < LOCAL < MEMORY < CALL < ALLOC < BARRIER <
// SUSPEND. CONTROL and TERMINAL are structural classes (branches and
// terminators do not "have" effects to move across; they shape the
// graph). This ordering is the contract I1 builds on.
enum class EffectClass : uint8_t {
    PURE = 0,    // stack/scalar ops with no observable state change
    LOCAL,       // non-captured frame-local slot access (loc/arg)
    MEMORY,      // heap/global/shared-storage reads and writes
    CALL,        // may execute arbitrary code (calls, iterators, eval,
                 // getters/proxies reachable from property access)
    ALLOC,       // materializes heap objects (array/object/closure/...)
    CONTROL,     // branches, catch/gosub dispatch
    BARRIER,     // with_*/eval: invalidates all local knowledge
    SUSPEND,     // yield/await: suspension/resume point (safepoint)
    TERMINAL,    // return*/throw: no fallthrough edge
};

// What the instruction's pushed value can be at runtime — the seed of
// the §3.3 value lattice. SCALAR is provably non-heap (null/undefined/
// booleans/small ints/typeof results); HEAP is provably a heap
// reference (object/array/closure/string/regexp/class materialization);
// ANY is the conservative default.
enum class ResultKind : uint8_t {
    SCALAR = 0,
    HEAP,
    ANY,
};

struct OpClass {
    bool may_throw;
    EffectClass effect;
    ResultKind result;
};

// Per-opcode classification (I0 metadata). Everything not explicitly
// classified falls back to {true, CALL, ANY}.
OpClass classify_op(uint8_t op);

// True when an effect can be observed by other worlds (the I1 token
// rule): MEMORY and stronger. PURE/LOCAL effects are frame-local.
bool effect_touches_world(EffectClass e);

// How an instruction consumes its popped operands, from the vendored
// interpreter's refcount behavior (quickjs.c stack ops):
//   RELEASE — the popped values are freed (drop, put_loc, calls, ...)
//   ALIAS   — nothing is freed; results keep the popped ids or push
//             fresh js_dup copies (the 17-op shuffle family, nip_catch)
//   NIP     — nip/nip1 free only the LAST popped value and keep the rest
enum class PopOwnership : uint8_t {
    RELEASE = 0,
    ALIAS,
    NIP,
};

PopOwnership pop_ownership(uint8_t op);

// True for the 17 stack-shuffle ops (dup/dup1/dup2/dup3/insert2/3/4,
// perm3/4/5, rot3l/rot3r/rot4l/rot5l, swap, swap2, nip_catch): pure
// value reordering with js_dup copies, no frees (see the shuffle source
// tables in ssa.cc).
bool is_shuffle(uint8_t op);

// True for frame-slot operations (loc/arg space, non-captured). The
// slot index is resolved by slot_index().
bool is_frame_slot_op(uint8_t op);
bool is_frame_slot_read(uint8_t op);   // get_loc*/get_arg* (copies out)
bool is_frame_slot_store(uint8_t op);  // put_loc*/set_loc*/put_arg*/...:
                                       // overwrites the slot, freeing the
                                       // previous value
// inc_loc/dec_loc/add_loc: fused read-modify-write (push the old value,
// replace the slot with a fresh one).
bool is_loc_rmw(uint8_t op);

enum class SlotSpace : uint8_t {
    ARG = 0,  // function arguments (caller-owned, never released)
    LOC,      // non-captured frame locals
};

SlotSpace slot_space(uint8_t op);

// Resolved slot index for a frame-slot op (short forms fold: get_loc0
// is slot 0, get_arg3 is arg slot 3, get_loc0_loc1 covers slots 0 and
// 1). Meaningful only when is_frame_slot_op(op).
int slot_index(uint8_t op, uint32_t aux);

// True for OP_catch: pushes the BORROWED exception marker
// (JS_NewCatchOffset sentinel) whose drop/nip closes the region.
bool is_catch_marker_push(uint8_t op);

// Structural helpers shared by the verifier and the SSA.
bool is_terminator(uint8_t op);    // return*/throw/ret/tail_call*
bool is_uncond_jump(uint8_t op);   // goto*

}  // namespace ir
}  // namespace bytecode
}  // namespace capsid

#endif  // CAPSID_SRC_BYTECODE_OPTIMIZER_IR_EFFECTS_H
