// I1 effect classification implementation (see effects.h). The opcode
// table and per-opcode classification were moved verbatim from cfg.cc
// (I0 metadata), where they were already the shared contract of the
// verifier; the I1 helpers add the refcount semantics pinned from the
// vendored interpreter (quickjs.c stack cases: drop/nip/nip1 frees,
// the shuffle family's js_dup copies, catch's JS_NewCatchOffset
// sentinel, frame-slot set_value overwrites).
#include "bytecode_optimizer/ir/effects.h"

#include <cstring>

namespace capsid {
namespace bytecode {
namespace ir {
namespace {

// Serialized opcode enum, built exactly like cfg.cc (quickjs.c:1166).
// Only the final serialized opcodes matter here: the never-serialized
// temporary opcodes cannot appear in BC26 function code.
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

}  // namespace

// ---------------------------------------------------------------------------
// I0 metadata classification (may_throw / effect class / result kind).
//
// Conservative by design: the default case is {may_throw, CALL, ANY} —
// an opcode that is not explicitly classified here can never make the
// analysis unsound, only less precise. Effects are ordered by strength
// in the EffectClass enum; the I1 effect token joins by max.
// ---------------------------------------------------------------------------

// Explicitly classified opcode families. Everything not listed falls
// back to the conservative {true, CALL, ANY}.
OpClass classify_op(uint8_t op) {
    switch (op) {
    // Scalar producers: pure, cannot throw.
    case OP_push_minus1: case OP_push_0: case OP_push_1: case OP_push_2:
    case OP_push_3: case OP_push_4: case OP_push_5: case OP_push_6:
    case OP_push_7: case OP_push_i8: case OP_push_i16: case OP_push_i32:
    case OP_undefined: case OP_null: case OP_push_false: case OP_push_true:
        return {false, EffectClass::PURE, ResultKind::SCALAR};
    // Pure predicates / unary checks: ToBoolean/typeof never invoke
    // user code and never throw.
    case OP_not: case OP_lnot: case OP_typeof: case OP_is_undefined:
    case OP_is_null: case OP_is_undefined_or_null:
    case OP_typeof_is_undefined: case OP_typeof_is_function:
        return {false, EffectClass::PURE, ResultKind::SCALAR};
    // Stack shuffles: pure, no throw, copy whatever is on the stack.
    case OP_drop: case OP_nip: case OP_nip1: case OP_dup: case OP_dup1:
    case OP_dup2: case OP_dup3: case OP_insert2: case OP_insert3:
    case OP_insert4: case OP_perm3: case OP_perm4: case OP_perm5:
    case OP_swap: case OP_swap2: case OP_rot3l: case OP_rot3r:
    case OP_rot4l: case OP_rot5l: case OP_nip_catch: case OP_nop:
        return {false, EffectClass::PURE, ResultKind::ANY};
    case OP_push_this:
        // `this` is any value at runtime.
        return {false, EffectClass::PURE, ResultKind::ANY};
    case OP_push_atom_value:
        // An atom can hold any value.
        return {false, EffectClass::PURE, ResultKind::ANY};
    case OP_push_empty_string:
        // The shared empty string is a heap reference.
        return {false, EffectClass::PURE, ResultKind::HEAP};
    // Frame-local slots: no user code, no exception (plain forms).
    case OP_get_loc: case OP_put_loc: case OP_set_loc:
    case OP_get_loc8: case OP_put_loc8: case OP_set_loc8:
    case OP_get_loc0_loc1: case OP_get_loc0: case OP_get_loc1:
    case OP_get_loc2: case OP_get_loc3:
    case OP_put_loc0: case OP_put_loc1: case OP_put_loc2: case OP_put_loc3:
    case OP_set_loc0: case OP_set_loc1: case OP_set_loc2: case OP_set_loc3:
    case OP_get_arg: case OP_put_arg: case OP_set_arg:
    case OP_get_arg0: case OP_get_arg1: case OP_get_arg2: case OP_get_arg3:
    case OP_put_arg0: case OP_put_arg1: case OP_put_arg2: case OP_put_arg3:
    case OP_set_arg0: case OP_set_arg1: case OP_set_arg2: case OP_set_arg3:
    case OP_set_loc_uninitialized:
        return {false, EffectClass::LOCAL, ResultKind::ANY};
    // inc/dec/add_loc: the slow paths coerce (ToNumeric/ToPrimitive
    // can run user code) and can throw — CALL, not LOCAL.
    case OP_inc_loc: case OP_dec_loc: case OP_add_loc:
        return {true, EffectClass::CALL, ResultKind::ANY};
    // TDZ check forms: throw ReferenceError on uninitialized access.
    case OP_get_loc_check: case OP_put_loc_check: case OP_put_loc_check_init:
        return {true, EffectClass::LOCAL, ResultKind::ANY};
    // Closure storage: plain heap cells, no user code, but shared with
    // capturing closures (MEMORY, not LOCAL).
    case OP_get_var_ref: case OP_put_var_ref: case OP_set_var_ref:
    case OP_get_var_ref0: case OP_get_var_ref1: case OP_get_var_ref2:
    case OP_get_var_ref3:
    case OP_put_var_ref0: case OP_put_var_ref1: case OP_put_var_ref2:
    case OP_put_var_ref3:
    case OP_set_var_ref0: case OP_set_var_ref1: case OP_set_var_ref2:
    case OP_set_var_ref3:
    case OP_get_var_ref_check: case OP_put_var_ref_check:
    case OP_put_var_ref_check_init:
        return {true, EffectClass::MEMORY, ResultKind::ANY};
    // Heap materializations: allocation can fail (OOM exception).
    case OP_object: case OP_special_object: case OP_rest:
    case OP_array_from: case OP_fclosure: case OP_fclosure8:
    case OP_regexp: case OP_push_bigint_i32: case OP_private_symbol:
    case OP_close_loc:
        return {true, EffectClass::ALLOC, ResultKind::HEAP};
    case OP_make_loc_ref: case OP_make_arg_ref: case OP_make_var_ref_ref:
    case OP_make_var_ref:
        return {true, EffectClass::ALLOC, ResultKind::HEAP};
    // Property/global access: getters, proxies, and the global object
    // can run arbitrary code — CALL, not MEMORY (shape knowledge from
    // S0/S1 may narrow this later).
    case OP_get_var: case OP_get_var_undef: case OP_put_var:
    case OP_put_var_init: case OP_define_var: case OP_check_define_var:
    case OP_delete_var: case OP_get_field: case OP_get_field2:
    case OP_put_field: case OP_get_private_field: case OP_put_private_field:
    case OP_private_in: case OP_get_array_el: case OP_get_array_el2:
    case OP_put_array_el: case OP_get_super: case OP_get_super_value:
    case OP_put_super_value: case OP_get_length:
    case OP_get_ref_value: case OP_put_ref_value:
        return {true, EffectClass::CALL, ResultKind::ANY};
    // Coercions: can invoke user conversion hooks.
    case OP_check_object: case OP_to_object: case OP_to_propkey:
    case OP_to_propkey2:
        return {true, EffectClass::CALL, ResultKind::ANY};
    // Calls / invocation of arbitrary code.
    case OP_call: case OP_call0: case OP_call1: case OP_call2:
    case OP_call3: case OP_call_method: case OP_call_constructor:
    case OP_tail_call: case OP_tail_call_method: case OP_apply:
    case OP_import:
        return {true, EffectClass::CALL, ResultKind::ANY};
    // Iterator protocol: invokes user code at every step.
    case OP_for_in_start: case OP_for_of_start: case OP_for_await_of_start:
    case OP_for_in_next: case OP_for_of_next:
    case OP_iterator_get_value_done: case OP_iterator_close:
    case OP_iterator_next: case OP_iterator_call:
        return {true, EffectClass::CALL, ResultKind::ANY};
    // Binary / unary value operations: coercion can run user code.
    case OP_neg: case OP_plus: case OP_dec: case OP_inc:
    case OP_post_dec: case OP_post_inc:
    case OP_mul: case OP_div: case OP_mod: case OP_add: case OP_sub:
    case OP_shl: case OP_sar: case OP_shr: case OP_and: case OP_xor:
    case OP_or: case OP_pow: case OP_lt: case OP_lte: case OP_gt:
    case OP_gte: case OP_instanceof: case OP_in: case OP_eq: case OP_neq:
    case OP_strict_eq: case OP_strict_neq:
        return {true, EffectClass::CALL, ResultKind::ANY};
    case OP_delete: case OP_using_dispose_init: case OP_using_dispose:
    case OP_using_dispose_async: case OP_using_dispose_merge:
    case OP_using_dispose_end: case OP_using_check:
    case OP_check_ctor: case OP_check_ctor_return: case OP_init_ctor:
    case OP_check_brand: case OP_add_brand:
        return {true, EffectClass::CALL, ResultKind::ANY};
    // Object-shaping ops (define machinery): can reach proxy traps.
    case OP_define_field: case OP_define_array_el: case OP_append:
    case OP_copy_data_properties: case OP_define_method:
    case OP_define_method_computed: case OP_define_class:
    case OP_define_class_computed: case OP_define_private_field:
    case OP_set_name: case OP_set_name_computed: case OP_set_proto:
    case OP_set_home_object:
        return {true, EffectClass::CALL, ResultKind::ANY};
    // Dynamic scope: with_* re-binds names through an arbitrary object
    // (BARRIER — invalidates all local knowledge).
    case OP_with_get_var: case OP_with_put_var: case OP_with_delete_var:
    case OP_with_make_ref: case OP_with_get_ref: case OP_with_get_ref_undef:
        return {true, EffectClass::BARRIER, ResultKind::ANY};
    // eval: arbitrary code in an arbitrary scope — BARRIER.
    case OP_eval: case OP_apply_eval:
        return {true, EffectClass::BARRIER, ResultKind::ANY};
    // Suspension points: yield/await (resume may deliver an exception).
    case OP_initial_yield: case OP_yield: case OP_yield_star:
    case OP_async_yield_star: case OP_await:
        return {true, EffectClass::SUSPEND, ResultKind::ANY};
    // Control flow.
    case OP_if_false: case OP_if_true: case OP_if_false8: case OP_if_true8:
    case OP_goto: case OP_goto8: case OP_goto16:
        return {false, EffectClass::CONTROL, ResultKind::SCALAR};
    case OP_catch: case OP_gosub:
        return {false, EffectClass::CONTROL, ResultKind::SCALAR};
    case OP_ret:
        return {false, EffectClass::TERMINAL, ResultKind::SCALAR};
    // Terminators.
    case OP_return: case OP_return_undef: case OP_return_async:
    case OP_throw: case OP_throw_error:
        return {true, EffectClass::TERMINAL, ResultKind::SCALAR};
    default:
        // Fail-closed conservative default: any unclassified opcode
        // behaves as a throwing call.
        return {true, EffectClass::CALL, ResultKind::ANY};
    }
}

bool effect_touches_world(EffectClass e) {
    return static_cast<uint8_t>(e) >= static_cast<uint8_t>(EffectClass::MEMORY);
}

bool is_terminator(uint8_t op) {
    // tail_call/tail_call_method are terminators too (the v1 verifier
    // seeds no fallthrough past them).
    return op == OP_tail_call || op == OP_tail_call_method ||
           op == OP_return || op == OP_return_undef ||
           op == OP_return_async || op == OP_throw ||
           op == OP_throw_error || op == OP_ret;
}

bool is_uncond_jump(uint8_t op) {
    return op == OP_goto || op == OP_goto8 || op == OP_goto16;
}

PopOwnership pop_ownership(uint8_t op) {
    if (is_shuffle(op)) return PopOwnership::ALIAS;
    if (op == OP_nip || op == OP_nip1) return PopOwnership::NIP;
    return PopOwnership::RELEASE;
}

bool is_shuffle(uint8_t op) {
    switch (op) {
    case OP_dup: case OP_dup1: case OP_dup2: case OP_dup3:
    case OP_insert2: case OP_insert3: case OP_insert4:
    case OP_perm3: case OP_perm4: case OP_perm5:
    case OP_rot3l: case OP_rot3r: case OP_rot4l: case OP_rot5l:
    case OP_swap: case OP_swap2: case OP_nip_catch:
        return true;
    default:
        return false;
    }
}

bool is_frame_slot_op(uint8_t op) {
    return is_frame_slot_read(op) || is_frame_slot_store(op) ||
           is_loc_rmw(op);
}

bool is_frame_slot_read(uint8_t op) {
    switch (op) {
    case OP_get_loc: case OP_get_loc8:
    case OP_get_loc0_loc1: case OP_get_loc0: case OP_get_loc1:
    case OP_get_loc2: case OP_get_loc3:
    case OP_get_loc_check:
    case OP_get_arg: case OP_get_arg0: case OP_get_arg1: case OP_get_arg2:
    case OP_get_arg3:
        return true;
    default:
        return false;
    }
}

bool is_frame_slot_store(uint8_t op) {
    switch (op) {
    case OP_put_loc: case OP_put_loc8:
    case OP_put_loc0: case OP_put_loc1: case OP_put_loc2: case OP_put_loc3:
    case OP_set_loc: case OP_set_loc8:
    case OP_set_loc0: case OP_set_loc1: case OP_set_loc2: case OP_set_loc3:
    case OP_put_loc_check: case OP_put_loc_check_init:
    case OP_set_loc_uninitialized:
    case OP_put_arg: case OP_put_arg0: case OP_put_arg1: case OP_put_arg2:
    case OP_put_arg3:
    case OP_set_arg: case OP_set_arg0: case OP_set_arg1: case OP_set_arg2:
    case OP_set_arg3:
        return true;
    default:
        return false;
    }
}

bool is_loc_rmw(uint8_t op) {
    return op == OP_inc_loc || op == OP_dec_loc || op == OP_add_loc;
}

SlotSpace slot_space(uint8_t op) {
    switch (op) {
    case OP_get_arg: case OP_get_arg0: case OP_get_arg1: case OP_get_arg2:
    case OP_get_arg3:
    case OP_put_arg: case OP_put_arg0: case OP_put_arg1: case OP_put_arg2:
    case OP_put_arg3:
    case OP_set_arg: case OP_set_arg0: case OP_set_arg1: case OP_set_arg2:
    case OP_set_arg3:
        return SlotSpace::ARG;
    default:
        return SlotSpace::LOC;
    }
}

int slot_index(uint8_t op, uint32_t aux) {
    // Short forms carry the slot number in the opcode; the decoded aux
    // is 0/unset for them. get_loc0_loc1 reads both slot 0 and slot 1
    // (index 0; the SSA treats it as covering two slots).
    switch (op) {
    case OP_get_loc0: case OP_put_loc0: case OP_set_loc0:
        return 0;
    case OP_get_loc1: case OP_put_loc1: case OP_set_loc1:
        return 1;
    case OP_get_loc2: case OP_put_loc2: case OP_set_loc2:
        return 2;
    case OP_get_loc3: case OP_put_loc3: case OP_set_loc3:
        return 3;
    case OP_get_arg0: case OP_put_arg0: case OP_set_arg0:
        return 0;
    case OP_get_arg1: case OP_put_arg1: case OP_set_arg1:
        return 1;
    case OP_get_arg2: case OP_put_arg2: case OP_set_arg2:
        return 2;
    case OP_get_arg3: case OP_put_arg3: case OP_set_arg3:
        return 3;
    default:
        return static_cast<int>(aux);
    }
}

bool is_catch_marker_push(uint8_t op) {
    return op == OP_catch;
}

}  // namespace ir
}  // namespace bytecode
}  // namespace capsid
