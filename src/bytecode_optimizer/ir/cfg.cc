// I0 CFG implementation (see cfg.h for the contract). Bring-up IR,
// analyze-only: the identity gate must pass on the corpus before any
// of this is linked into the optimization pipeline.
//
// The decoder is intentionally independent of bytecode_optimizer.cc's
// (per plan §3.1 the IR is a parallel bring-up stack): the identity
// gate then proves two independent decode paths agree byte-for-byte on
// the whole corpus. Operand encodings, jump resolution, and pc2line
// rules mirror the vendored quickjs.c (emit_single_byte_code /
// resolve_labels / find_line_num / compute_pc2line_info) as pinned in
// bytecode_optimizer.cc; divergence surfaces as identity-gate failures.
#include "bytecode_optimizer/ir/cfg.h"

#include <algorithm>
#include <cstring>

namespace capsid {
namespace bytecode {
namespace ir {
namespace {

// ---------------------------------------------------------------------------
// Opcode tables, built exactly like quickjs.c:1158/1166/21856. Only the
// final serialized opcodes matter here: the never-serialized temporary
// opcodes cannot appear in BC26 function code.
// ---------------------------------------------------------------------------

enum OpFmt {
#define FMT(f) OP_FMT_##f,
#define DEF(id, size, n_pop, n_push, f)
#include "quickjs-opcode.h"
#undef DEF
#undef FMT
};

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

struct OpInfo {
    uint8_t size;
    uint8_t n_pop;
    uint8_t n_push;
    OpFmt fmt;
    const char* name;
};

static const OpInfo opcode_info[OP_COUNT] = {
#define FMT(f)
#define DEF(id, size, n_pop, n_push, f) \
    { size, n_pop, n_push, OP_FMT_##f, #id },
#define def(id, size, n_pop, n_push, f) /* never serialized */
#include "quickjs-opcode.h"
#undef def
#undef DEF
#undef FMT
};

// PC2LINE encoding constants (quickjs.c:767-770).
static const int PC2LINE_BASE = -1;
static const int PC2LINE_RANGE = 5;
static const int PC2LINE_OP_FIRST = 1;

// ---------------------------------------------------------------------------
// I0 metadata classification (may_throw / effect class / result kind).
//
// Conservative by design: the default case is {may_throw, CALL, ANY} —
// an opcode that is not explicitly classified here can never make the
// analysis unsound, only less precise. Effects are ordered by strength
// in the EffectClass enum; the I1 effect token joins by max.
// ---------------------------------------------------------------------------

struct OpClass {
    bool may_throw;
    EffectClass effect;
    ResultKind result;
};

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
    case OP_inc_loc: case OP_dec_loc: case OP_add_loc:
    case OP_set_loc_uninitialized:
        return {false, EffectClass::LOCAL, ResultKind::ANY};
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

// ---------------------------------------------------------------------------
// pc2line decode (mirror bytecode_optimizer.cc / quickjs.c find_line_num).
// ---------------------------------------------------------------------------

bool read_leb(const uint8_t* p, size_t len, size_t* i, uint32_t* out) {
    uint32_t v = 0;
    int shift = 0;
    for (; shift < 35 && *i < len; shift += 7) {
        uint8_t a = p[(*i)++];
        v |= static_cast<uint32_t>(a & 0x7f) << shift;
        if (!(a & 0x80)) {
            *out = v;
            return true;
        }
    }
    return false;
}

struct SrcEntry {
    uint32_t pc;
    int32_t line;
    int32_t col;
};

// Decodes the pc2line blob; returns false if the blob is malformed.
bool decode_pc2line(const uint8_t* p,
                    size_t len,
                    int32_t base_line,
                    int32_t base_col,
                    std::vector<SrcEntry>* out) {
    uint32_t pc = 0;
    int32_t line = base_line;
    int32_t col = base_col;
    size_t i = 0;
    while (i < len) {
        uint8_t b = p[i++];
        uint32_t pc_delta;
        int32_t line_delta;
        if (b == 0) {
            uint32_t v;
            if (!read_leb(p, len, &i, &v)) return false;
            pc_delta = v;
            uint32_t sv;
            if (!read_leb(p, len, &i, &sv)) return false;
            line_delta = static_cast<int32_t>(
                (sv >> 1) ^ static_cast<uint32_t>(-(sv & 1)));
        } else {
            uint32_t op = b - PC2LINE_OP_FIRST;
            pc_delta = op / PC2LINE_RANGE;
            line_delta = static_cast<int32_t>(op % PC2LINE_RANGE) +
                         PC2LINE_BASE;
        }
        uint32_t cv;
        if (!read_leb(p, len, &i, &cv)) return false;
        int32_t col_delta = static_cast<int32_t>(
            (cv >> 1) ^ static_cast<uint32_t>(-(cv & 1)));
        pc += pc_delta;
        line += line_delta;
        col += col_delta;
        SrcEntry e;
        e.pc = pc;
        e.line = line;
        e.col = col;
        out->push_back(e);
    }
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// decode_function
// ---------------------------------------------------------------------------

bool decode_function(const uint8_t* code,
                     size_t len,
                     const uint8_t* bundle,
                     const FuncInfo& fi,
                     std::vector<Insn>* out,
                     std::string* error) {
    // Resolve the pc2line entries once; src_line/src_col are filled
    // per instruction from the entry with the largest pc <= pc.
    std::vector<SrcEntry> src;
    if (fi.dbg_pc2line_len != 0) {
        if (!decode_pc2line(bundle + fi.dbg_pc2line_off, fi.dbg_pc2line_len,
                            fi.dbg_line, fi.dbg_col, &src)) {
            *error = "cfg: malformed pc2line table";
            return false;
        }
    }
    size_t si = 0;
    auto resolve_src = [&](uint32_t pc, uint16_t* line,
                           uint16_t* col) {
        while (si + 1 < src.size() && src[si + 1].pc <= pc) si++;
        if (si < src.size() && src[si].pc <= pc) {
            *line = static_cast<uint16_t>(src[si].line);
            *col = static_cast<uint16_t>(src[si].col);
        }
    };

    out->clear();
    size_t pc = 0;
    while (pc < len) {
        uint8_t op = code[pc];
        if (op == 0 || op >= OP_COUNT) {
            *error = "cfg: invalid opcode in function";
            return false;
        }
        const OpInfo& oi = opcode_info[op];
        if (pc + oi.size > len) {
            *error = "cfg: truncated instruction";
            return false;
        }
        const OpClass& oc = classify_op(op);
        Insn in;
        in.op = op;
        in.old_off = static_cast<uint32_t>(pc);
        in.old_size = oi.size;
        in.target = INT32_MIN;  // "no jump" sentinel
        in.imm = 0;
        in.aux = 0;
        in.has_aux = false;
        in.n_pop = oi.n_pop;
        in.n_push = oi.n_push;
        in.src_line = 0;
        in.src_col = 0;
        in.may_throw = oc.may_throw;
        in.effect = oc.effect;
        in.result = oc.result;
        switch (op) {
        case OP_push_minus1: in.imm = -1; break;
        case OP_push_0: case OP_push_1: case OP_push_2: case OP_push_3:
        case OP_push_4: case OP_push_5: case OP_push_6: case OP_push_7:
            in.imm = op - OP_push_0;
            break;
        case OP_push_i8:
            in.imm = static_cast<int8_t>(code[pc + 1]);
            break;
        case OP_push_i16:
            in.imm = static_cast<int16_t>(
                static_cast<uint16_t>(code[pc + 1]) |
                (static_cast<uint16_t>(code[pc + 2]) << 8));
            break;
        case OP_push_i32:
            in.imm = static_cast<int32_t>(
                static_cast<uint32_t>(code[pc + 1]) |
                (static_cast<uint32_t>(code[pc + 2]) << 8) |
                (static_cast<uint32_t>(code[pc + 3]) << 16) |
                (static_cast<uint32_t>(code[pc + 4]) << 24));
            break;
        case OP_push_const:
        case OP_fclosure:
        case OP_push_atom_value:
            in.aux = static_cast<uint32_t>(code[pc + 1]) |
                     (static_cast<uint32_t>(code[pc + 2]) << 8) |
                     (static_cast<uint32_t>(code[pc + 3]) << 16) |
                     (static_cast<uint32_t>(code[pc + 4]) << 24);
            in.has_aux = true;
            break;
        case OP_push_const8:
        case OP_fclosure8:
            in.aux = code[pc + 1];
            in.has_aux = true;
            break;
        case OP_get_loc: case OP_put_loc: case OP_set_loc:
        case OP_get_arg: case OP_put_arg: case OP_set_arg:
        case OP_get_var_ref: case OP_put_var_ref: case OP_set_var_ref:
        case OP_set_loc_uninitialized: case OP_get_loc_check:
        case OP_put_loc_check: case OP_put_loc_check_init:
        case OP_get_var_ref_check: case OP_put_var_ref_check:
        case OP_put_var_ref_check_init: case OP_close_loc:
        case OP_using_dispose: case OP_using_dispose_async:
            in.aux = static_cast<uint16_t>(code[pc + 1]) |
                     (static_cast<uint16_t>(code[pc + 2]) << 8);
            in.has_aux = true;
            break;
        case OP_get_loc8: case OP_put_loc8: case OP_set_loc8:
        case OP_inc_loc: case OP_dec_loc: case OP_add_loc:
            in.aux = code[pc + 1];
            in.has_aux = true;
            break;
        case OP_call: case OP_call_method: case OP_tail_call:
        case OP_tail_call_method: case OP_call_constructor:
        case OP_array_from: case OP_apply: case OP_apply_eval:
        case OP_eval:
            in.aux = static_cast<uint16_t>(code[pc + 1]) |
                     (static_cast<uint16_t>(code[pc + 2]) << 8);
            in.has_aux = true;
            break;
        case OP_if_false: case OP_if_true: case OP_goto:
        case OP_catch: case OP_gosub: {
            int32_t diff = static_cast<int32_t>(
                static_cast<uint32_t>(code[pc + 1]) |
                (static_cast<uint32_t>(code[pc + 2]) << 8) |
                (static_cast<uint32_t>(code[pc + 3]) << 16) |
                (static_cast<uint32_t>(code[pc + 4]) << 24));
            in.target = static_cast<int32_t>(pc + 1) + diff;
            break;
        }
        case OP_if_false8: case OP_if_true8: case OP_goto8: {
            int32_t diff = static_cast<int8_t>(code[pc + 1]);
            in.target = static_cast<int32_t>(pc + 1) + diff;
            break;
        }
        case OP_goto16: {
            int32_t diff = static_cast<int16_t>(
                static_cast<uint16_t>(code[pc + 1]) |
                (static_cast<uint16_t>(code[pc + 2]) << 8));
            in.target = static_cast<int32_t>(pc + 1) + diff;
            break;
        }
        case OP_with_get_var: case OP_with_put_var:
        case OP_with_delete_var: case OP_with_make_ref:
        case OP_with_get_ref: case OP_with_get_ref_undef: {
            int32_t diff = static_cast<int32_t>(
                static_cast<uint32_t>(code[pc + 5]) |
                (static_cast<uint32_t>(code[pc + 6]) << 8) |
                (static_cast<uint32_t>(code[pc + 7]) << 16) |
                (static_cast<uint32_t>(code[pc + 8]) << 24));
            in.target = static_cast<int32_t>(pc + 5) + diff;
            break;
        }
        default:
            break;
        }
        // Atom-family operands: raw u32 at pc+1 (also atom_u8/u16 and
        // with_* label variants). No name resolution is needed.
        if (!in.has_aux) {
            switch (oi.fmt) {
            case OP_FMT_atom:
            case OP_FMT_atom_u8:
            case OP_FMT_atom_u16:
            case OP_FMT_atom_label_u8:
            case OP_FMT_atom_label_u16:
                in.aux = static_cast<uint32_t>(code[pc + 1]) |
                         (static_cast<uint32_t>(code[pc + 2]) << 8) |
                         (static_cast<uint32_t>(code[pc + 3]) << 16) |
                         (static_cast<uint32_t>(code[pc + 4]) << 24);
                in.has_aux = true;
                break;
            default:
                break;
            }
        }
        resolve_src(static_cast<uint32_t>(pc), &in.src_line, &in.src_col);
        out->push_back(in);
        pc += oi.size;
    }
    // Resolve jump operands to instruction indexes. Offsets are
    // strictly increasing, so a binary search over instruction starts
    // suffices.
    std::vector<uint32_t> offs;
    offs.reserve(out->size());
    for (size_t i = 0; i < out->size(); i++) {
        offs.push_back((*out)[i].old_off);
    }
    for (size_t i = 0; i < out->size(); i++) {
        Insn& in = (*out)[i];
        // INT32_MIN is the "no jump" sentinel; any other negative value
        // is a jump that lands before the blob (fail closed — a computed
        // -1 is out of range, never a sentinel).
        if (in.target == INT32_MIN) continue;
        int32_t want = in.target;
        if (want < 0 || static_cast<size_t>(want) >= len) {
            *error = "cfg: jump target out of range";
            return false;
        }
        std::vector<uint32_t>::const_iterator it =
            std::lower_bound(offs.begin(), offs.end(),
                             static_cast<uint32_t>(want));
        if (it == offs.end() || *it != static_cast<uint32_t>(want)) {
            *error = "cfg: jump target not on instruction boundary";
            return false;
        }
        in.target = static_cast<int32_t>(it - offs.begin());
    }
    return true;
}

// ---------------------------------------------------------------------------
// build_cfg / verify_cfg
// ---------------------------------------------------------------------------

bool build_cfg(const std::vector<Insn>& insns, Cfg* out, std::string* error) {
    const size_t n = insns.size();
    if (n == 0) {
        *error = "cfg: empty code blob";
        return false;
    }
    // Leaders: entry, every jump target, post-gosub return points.
    std::vector<uint8_t> is_leader(n, 0);
    is_leader[0] = 1;
    for (size_t i = 0; i < n; i++) {
        const Insn& in = insns[i];
        if (in.target >= 0) {
            is_leader[static_cast<size_t>(in.target)] = 1;
        }
        if (in.op == OP_gosub && i + 1 < n) {
            is_leader[i + 1] = 1;
        }
    }
    // Block ids: each leader starts a block; run members share it.
    std::vector<int32_t> block_id(n, -1);
    size_t nb = 0;
    for (size_t i = 0; i < n; i++) {
        if (is_leader[i]) block_id[i] = static_cast<int32_t>(nb++);
    }
    if (nb == 0) {
        *error = "cfg: no blocks";
        return false;
    }
    for (size_t i = 1; i < n; i++) {
        if (block_id[i] < 0) block_id[i] = block_id[i - 1];
    }
    // Block ranges [start, end).
    std::vector<size_t> bstart(nb), bend(nb);
    for (size_t b = 0; b < nb; b++) {
        size_t i = 0;
        while (i < n && block_id[i] != static_cast<int32_t>(b)) i++;
        bstart[b] = i;
        while (i < n && block_id[i] == static_cast<int32_t>(b)) i++;
        bend[b] = i;
    }

    out->insns = insns;
    out->blocks.clear();
    out->blocks.reserve(nb);
    for (size_t b = 0; b < nb; b++) {
        Block blk;
        blk.start = static_cast<uint32_t>(bstart[b]);
        blk.end = static_cast<uint32_t>(bend[b]);
        blk.has_ret = false;
        blk.reachable = false;
        blk.entry_height = -1;
        for (size_t i = bstart[b]; i < bend[b]; i++) {
            if (insns[i].op == OP_ret) blk.has_ret = true;
        }
        out->blocks.push_back(blk);
    }

    // Edges. An edge to a block whose start precedes the source block's
    // start is conservatively a loop backedge. Only the live prefix of a
    // block emits edges: instructions after the first terminator or
    // unconditional jump are dead code (quickjs's emitter keeps
    // statements after return/throw), never seeded by the verifier —
    // mirroring the v1 verifier, which never visits them either.
    for (size_t b = 0; b < nb; b++) {
        Block& blk = out->blocks[b];
        bool live = true;
        for (size_t i = bstart[b]; i < bend[b]; i++) {
            if (!live) continue;
            const Insn& in = insns[i];
            EdgeKind taken = EdgeKind::JUMP;
            switch (in.op) {
            case OP_catch: taken = EdgeKind::CATCH; break;
            case OP_gosub: taken = EdgeKind::GOSUB; break;
            case OP_with_get_var: case OP_with_put_var:
            case OP_with_delete_var: case OP_with_make_ref:
            case OP_with_get_ref: case OP_with_get_ref_undef:
                taken = EdgeKind::BARRIER;
                break;
            default: break;
            }
            if (in.target >= 0) {
                // The target must be a leader, hence a block start.
                uint32_t to = static_cast<uint32_t>(block_id[in.target]);
                Edge e;
                e.kind = taken;
                e.to = to;
                e.src_insn = static_cast<uint32_t>(i);
                e.backedge = bstart[to] <= bstart[b];
                blk.edges.push_back(e);
            }
            // Cross-block fallthrough only: intra-block flow is the
            // verifier's linear height propagation, not an edge (a
            // self-loop would alias the block's entry-height check).
            if (!is_terminator(in.op) && !is_uncond_jump(in.op)) {
                if (i + 1 >= n) {
                    *error = "cfg: control flow falls off the code blob";
                    return false;
                }
                if (block_id[i + 1] != block_id[i]) {
                    Edge e;
                    e.kind = (in.op == OP_await || in.op == OP_yield ||
                              in.op == OP_yield_star ||
                              in.op == OP_async_yield_star)
                                 ? EdgeKind::SUSPEND
                                 : EdgeKind::FALLTHROUGH;
                    e.to = static_cast<uint32_t>(block_id[i + 1]);
                    e.src_insn = static_cast<uint32_t>(i);
                    e.backedge = bstart[block_id[i + 1]] <= bstart[b];
                    blk.edges.push_back(e);
                }
            }
            if (is_terminator(in.op) || is_uncond_jump(in.op)) {
                live = false;
            }
        }
    }
    return true;
}

bool verify_cfg(const Cfg& cfg, std::string* error) {
    const size_t nb = cfg.blocks.size();
    if (nb == 0) {
        *error = "cfg: empty CFG";
        return false;
    }
    // Per-block entry height propagation (the existing verifier's
    // contract at block granularity).
    std::vector<int32_t> heights(nb, -1);
    std::vector<size_t> worklist;
    auto seed = [&](size_t b, int32_t h) -> bool {
        if (b >= nb) {
            *error = "cfg: edge targets an invalid block";
            return false;
        }
        if (heights[b] != -1) {
            if (heights[b] != h) {
                *error = "cfg: inconsistent stack height at block " +
                         std::to_string(b);
                return false;
            }
            return true;
        }
        heights[b] = h;
        worklist.push_back(b);
        return true;
    };
    if (!seed(0, 0)) return false;
    uint32_t max_h = 0;
    while (!worklist.empty()) {
        size_t b = worklist.back();
        worklist.pop_back();
        Cfg& mut = const_cast<Cfg&>(cfg);
        mut.blocks[b].reachable = true;
        mut.blocks[b].entry_height = heights[b];
        int32_t h = heights[b];
        const Block& blk = cfg.blocks[b];
        for (size_t i = blk.start; i < blk.end; i++) {
            const Insn& in = cfg.insns[i];
            const OpInfo& oi = opcode_info[in.op];
            int32_t n_pop = in.n_pop;
            // npop/npopx/npop_u16 formats fold the operand count into
            // the pop count (the v1 verifier's exact rule; apply and
            // apply_eval are plain u16 and do NOT fold).
            if (oi.fmt == OP_FMT_npop || oi.fmt == OP_FMT_npop_u16) {
                n_pop += static_cast<int32_t>(in.aux);
            } else if (oi.fmt == OP_FMT_npopx) {
                n_pop += static_cast<int32_t>(in.op) - OP_call0;
            }
            if (h < n_pop) {
                *error = "cfg: stack underflow at instruction " +
                         std::to_string(i);
                return false;
            }
            int32_t post = h - n_pop + in.n_push;
            if (post > static_cast<int32_t>(max_h)) max_h = post;
            h = post;
            // Edge seeds carry the verify_code height adjustments for
            // the gosub/with_* handler forms. Each edge seeds exactly
            // once, with the post height at its own source instruction.
            for (size_t e = 0; e < blk.edges.size(); e++) {
                const Edge& ed = blk.edges[e];
                if (ed.src_insn != i) continue;
                if (ed.kind == EdgeKind::JUMP) {
                    if (!seed(ed.to, post)) return false;
                } else if (ed.kind == EdgeKind::CATCH ||
                           ed.kind == EdgeKind::FALLTHROUGH ||
                           ed.kind == EdgeKind::SUSPEND) {
                    if (!seed(ed.to, post)) return false;
                } else if (ed.kind == EdgeKind::GOSUB) {
                    if (!seed(ed.to, post + 1)) return false;
                } else if (ed.kind == EdgeKind::BARRIER) {
                    switch (in.op) {
                    case OP_with_get_var:
                    case OP_with_delete_var:
                        if (!seed(ed.to, post + 1)) return false;
                        break;
                    case OP_with_make_ref:
                    case OP_with_get_ref:
                    case OP_with_get_ref_undef:
                        if (!seed(ed.to, post + 2)) return false;
                        break;
                    case OP_with_put_var:
                        if (!seed(ed.to, post - 1)) return false;
                        break;
                    default:
                        break;
                    }
                }
            }
            // Linear flow ends at terminators and unconditional jumps;
            // anything after them in the block is dead code (see
            // build_cfg) and must not be height-checked or seeded.
            if (is_terminator(in.op) || is_uncond_jump(in.op)) break;
        }
    }
    const_cast<Cfg&>(cfg).max_height = max_h;
    if (max_h > cfg.recorded_stack_size) {
        *error = "cfg: max stack height exceeds the recorded stack size";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// emit_identity
// ---------------------------------------------------------------------------

bool emit_identity(const std::vector<Insn>& insns,
                   const uint8_t* old_code,
                   size_t old_len,
                   std::vector<uint8_t>* out,
                   std::string* error) {
    // Offsets are the original sizes (identity lowering never changes
    // forms), so jump distances must reproduce the original operands.
    std::vector<uint32_t> offs(insns.size());
    uint32_t off = 0;
    for (size_t i = 0; i < insns.size(); i++) {
        offs[i] = off;
        off += opcode_info[insns[i].op].size;
    }
    if (off != old_len) {
        *error = "cfg: identity size mismatch";
        return false;
    }
    auto emit_jump_dist = [&](size_t operand_start, size_t width,
                              const Insn& in) -> bool {
        int64_t dist = static_cast<int64_t>(offs[in.target]) -
                       static_cast<int64_t>(operand_start);
        for (size_t k = 0; k < width; k++) {
            uint8_t want = static_cast<uint8_t>(
                (static_cast<uint64_t>(static_cast<int64_t>(dist)) >>
                 (8 * k)) &
                0xff);
            // Identity emission means the operand's output position
            // equals its original position, wherever that is (offs[i]+1
            // for plain jumps, offs[i]+5 for the with_* label field).
            if (old_code[operand_start + k] != want) {
                *error = "cfg: jump operand diverges from original";
                return false;
            }
        }
        return true;
    };
    out->clear();
    out->reserve(off);
    for (size_t i = 0; i < insns.size(); i++) {
        const Insn& in = insns[i];
        const OpInfo& oi = opcode_info[in.op];
        out->push_back(static_cast<uint8_t>(in.op));
        switch (in.op) {
        case OP_push_minus1: case OP_push_0: case OP_push_1:
        case OP_push_2: case OP_push_3: case OP_push_4:
        case OP_push_5: case OP_push_6: case OP_push_7:
            break;
        case OP_push_i8:
            if (old_code[in.old_off + 1] !=
                static_cast<uint8_t>(static_cast<int8_t>(in.imm))) {
                *error = "cfg: push_i8 operand diverges from original";
                return false;
            }
            out->push_back(static_cast<uint8_t>(static_cast<int8_t>(in.imm)));
            break;
        case OP_push_i16: {
            uint16_t v = static_cast<uint16_t>(static_cast<int16_t>(in.imm));
            if (old_code[in.old_off + 1] != static_cast<uint8_t>(v) ||
                old_code[in.old_off + 2] !=
                    static_cast<uint8_t>(v >> 8)) {
                *error = "cfg: push_i16 operand diverges from original";
                return false;
            }
            out->push_back(static_cast<uint8_t>(v));
            out->push_back(static_cast<uint8_t>(v >> 8));
            break;
        }
        case OP_push_i32: {
            uint32_t v = static_cast<uint32_t>(static_cast<int32_t>(in.imm));
            for (size_t k = 0; k < 4; k++) {
                if (old_code[in.old_off + 1 + k] !=
                    static_cast<uint8_t>(v >> (8 * k))) {
                    *error = "cfg: push_i32 operand diverges from original";
                    return false;
                }
                out->push_back(static_cast<uint8_t>(v >> (8 * k)));
            }
            break;
        }
        case OP_push_const:
        case OP_fclosure:
        case OP_push_atom_value:
            for (size_t k = 0; k < 4; k++) {
                if (old_code[in.old_off + 1 + k] !=
                    static_cast<uint8_t>(in.aux >> (8 * k))) {
                    *error = "cfg: cpool/atom operand diverges from original";
                    return false;
                }
                out->push_back(static_cast<uint8_t>(in.aux >> (8 * k)));
            }
            break;
        case OP_push_const8:
        case OP_fclosure8:
            if (old_code[in.old_off + 1] != static_cast<uint8_t>(in.aux)) {
                *error = "cfg: cpool8 operand diverges from original";
                return false;
            }
            out->push_back(static_cast<uint8_t>(in.aux));
            break;
        case OP_get_loc8: case OP_put_loc8: case OP_set_loc8:
        case OP_inc_loc: case OP_dec_loc: case OP_add_loc:
            if (old_code[in.old_off + 1] != static_cast<uint8_t>(in.aux)) {
                *error = "cfg: loc8 operand diverges from original";
                return false;
            }
            out->push_back(static_cast<uint8_t>(in.aux));
            break;
        case OP_get_loc: case OP_put_loc: case OP_set_loc:
        case OP_get_arg: case OP_put_arg: case OP_set_arg:
        case OP_get_var_ref: case OP_put_var_ref: case OP_set_var_ref:
        case OP_set_loc_uninitialized: case OP_get_loc_check:
        case OP_put_loc_check: case OP_put_loc_check_init:
        case OP_get_var_ref_check: case OP_put_var_ref_check:
        case OP_put_var_ref_check_init: case OP_close_loc:
        case OP_using_dispose: case OP_using_dispose_async:
            for (size_t k = 0; k < 2; k++) {
                if (old_code[in.old_off + 1 + k] !=
                    static_cast<uint8_t>(in.aux >> (8 * k))) {
                    *error = "cfg: slot operand diverges from original";
                    return false;
                }
                out->push_back(static_cast<uint8_t>(in.aux >> (8 * k)));
            }
            break;
        case OP_call: case OP_call_method: case OP_tail_call:
        case OP_tail_call_method: case OP_call_constructor:
        case OP_array_from: case OP_apply: case OP_apply_eval:
            for (size_t k = 0; k < 2; k++) {
                if (old_code[in.old_off + 1 + k] !=
                    static_cast<uint8_t>(in.aux >> (8 * k))) {
                    *error = "cfg: call operand diverges from original";
                    return false;
                }
                out->push_back(static_cast<uint8_t>(in.aux >> (8 * k)));
            }
            break;
        case OP_eval:
            // Two u16 operands (argc + scope index); only argc is
            // decoded, so copy verbatim (matches the v1 emitter).
            out->insert(out->end(), old_code + in.old_off + 1,
                        old_code + in.old_off + oi.size);
            break;
        case OP_if_false: case OP_if_true: case OP_goto:
        case OP_catch: case OP_gosub:
            if (!emit_jump_dist(offs[i] + 1, 4, in)) return false;
            out->insert(out->end(), old_code + in.old_off + 1,
                        old_code + in.old_off + oi.size);
            break;
        case OP_if_false8: case OP_if_true8: case OP_goto8:
            if (!emit_jump_dist(offs[i] + 1, 1, in)) return false;
            out->insert(out->end(), old_code + in.old_off + 1,
                        old_code + in.old_off + oi.size);
            break;
        case OP_goto16:
            if (!emit_jump_dist(offs[i] + 1, 2, in)) return false;
            out->insert(out->end(), old_code + in.old_off + 1,
                        old_code + in.old_off + oi.size);
            break;
        case OP_with_get_var: case OP_with_put_var:
        case OP_with_delete_var: case OP_with_make_ref:
        case OP_with_get_ref: case OP_with_get_ref_undef:
            // atom (u32) + label offset (self-relative from +5) + u8.
            for (size_t k = 0; k < 4; k++) {
                if (old_code[in.old_off + 1 + k] !=
                    static_cast<uint8_t>(in.aux >> (8 * k))) {
                    *error = "cfg: with_* atom diverges from original";
                    return false;
                }
            }
            if (!emit_jump_dist(offs[i] + 5, 4, in)) return false;
            out->insert(out->end(), old_code + in.old_off + 1,
                        old_code + in.old_off + oi.size);
            break;
        default:
            // Everything else copies verbatim; identity lowering does
            // not re-select forms.
            out->insert(out->end(), old_code + in.old_off + 1,
                        old_code + in.old_off + oi.size);
            break;
        }
    }
    // Final assertion: the re-encoded blob is byte-for-byte the input.
    if (out->size() != old_len ||
        (old_len > 0 && std::memcmp(out->data(), old_code, old_len) != 0)) {
        *error = "cfg: identity lowering did not reproduce the code blob";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// identity_round_trip
// ---------------------------------------------------------------------------

namespace {
void count_function(const std::vector<FuncInfo>& funcs, uint64_t* count) {
    for (size_t i = 0; i < funcs.size(); i++) {
        (*count)++;
        count_function(funcs[i].children, count);
    }
}
}  // namespace

bool identity_round_trip(const uint8_t* data,
                         size_t size,
                         IdentityReport* out,
                         std::string* error) {
    std::vector<FuncInfo> functions;
    // Full bundle validation (version, checksum, atoms, module record).
    if (!read_functions(data, size, &functions, error)) return false;
    IdentityReport rep;
    rep.functions = 0;
    rep.insns = 0;
    rep.rejected_functions = 0;
    rep.rejected_insns = 0;
    rep.missing_pc2line = 0;
    count_function(functions, &rep.functions);

    // Walk the function tree; `walk` returns false only when the
    // identity contract itself breaks (a decodable, verifiable function
    // whose re-emission diverges). Unsupported functions are counted as
    // rejected coverage, never as identity failures.
    struct Walker {
        const uint8_t* data;
        IdentityReport* rep;
        bool fail(const char* msg, std::string* error) {
            *error = std::string("cfg: ") + msg;
            return false;
        }
        bool run(const FuncInfo& fi, std::string* error) {
            std::vector<Insn> insns;
            if (!decode_function(data + fi.code_off, fi.code_len, data, fi,
                                 &insns, error)) {
                std::fprintf(stderr, "cfg: rejected (decode): %s\n",
                             error->c_str());
                // Undecodable: no instruction count is knowable, so the
                // coverage ledger counts the function only. Its code
                // stays byte-for-byte BC26.
                rep->rejected_functions++;
                return true;
            }
            rep->insns += static_cast<uint64_t>(insns.size());
            Cfg cfg;
            if (!build_cfg(insns, &cfg, error)) {
                std::fprintf(stderr, "cfg: rejected (build): %s\n",
                             error->c_str());
                rep->rejected_functions++;
                rep->rejected_insns += static_cast<uint64_t>(insns.size());
                error->clear();
                return true;
            }
            cfg.recorded_stack_size = fi.stack_size;
            if (!verify_cfg(cfg, error)) {
                std::fprintf(stderr, "cfg: rejected (verify): %s\n",
                             error->c_str());
                rep->rejected_functions++;
                rep->rejected_insns += static_cast<uint64_t>(insns.size());
                error->clear();
                return true;
            }
            std::vector<uint8_t> re;
            if (!emit_identity(insns, data + fi.code_off, fi.code_len, &re,
                               error)) {
                return fail("identity lowering diverged", error);
            }
            // pc2line readability: every function with a debug block
            // must decode its pc2line table with pcs inside the code
            // blob (byte-identity of the table itself is guaranteed by
            // the bundle-level verbatim copy).
            if (fi.dbg_pc2line_len != 0) {
                std::vector<SrcEntry> src;
                if (!decode_pc2line(data + fi.dbg_pc2line_off,
                                    fi.dbg_pc2line_len, fi.dbg_line,
                                    fi.dbg_col, &src)) {
                    return fail("malformed pc2line", error);
                }
                for (size_t i = 0; i < src.size(); i++) {
                    if (src[i].pc > fi.code_len) {
                        return fail("pc2line pc beyond code length", error);
                    }
                }
            } else {
                rep->missing_pc2line++;
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
