// Bytecode AOT optimizer implementation (design: docs/bytecode-aot-optimizer.md).
//
// State: v1 pass pipeline over the serialized quickjs-ng bytecode
// format (BC_VERSION 26):
//   parse  -> per-function: decode -> P3 peepholes (const binop folds,
//   const-vs-const comparisons, push/drop elimination, dup/swap/rot
//   cleanup, literal-condition folds) -> P6 re-shrink (short forms by
//   value/index/argc) -> emit (jump-distance fixpoint) -> verifier
//   (compute_stack_size re-implementation) -> P7 pc2line remap
//   -> buffer rebuild (leb128 patches + bc_csum recompute) -> full
//   reparse self-check.
// Fail-closed: any input the optimizer cannot fully parse, or any
// invariant the verifier rejects, returns false; the caller aborts the
// compile without producing output files. No silent passthrough.
//
// Format contract (all offsets into deps/quickjs/quickjs.c at the
// pinned commit bf8988fc):
//   header: u8 version, u32 checksum(bc_csum over buf+5..), leb128
//           atom count, atom table
//   module: tag, atom module_name, req/export/star/import tables, u8
//           has_tla, function record
//   function: tag, u16 flags (bit 11 = allow_debug), u8 strict, atom
//             func_name, 8x leb128 (arg,var,defined_arg,stack_size,
//             var_ref,closure_var,cpool,byte_code_len), vardefs (leb128
//             count; per entry atom, leb128, leb128, u8 flags, optional
//             leb128 var_ref_idx when bit 6 (0x40) set), closure vars
//             (per entry atom, leb128 var_idx, leb128 flags — flags are
//             LEB128, not u8), cpool first (recursive), code blob,
//             debug block (atom filename, leb128 line, leb128 col,
//             leb128 pc2line_len + blob, leb128 source_len + blob)
//   jumps: target = operand_start + signed_offset (self-relative,
//          final at write time; the interpreter consumes them directly).
//          Label instructions are removed in resolve_labels, so jump
//          targets point at arbitrary instruction boundaries.
//   pc2line: cumulative (pc_delta, line_delta, col_delta) triples;
//            short form byte = line_delta + pc_delta*5 + 2 (line_delta
//            in [-1,3], pc_delta <= 50), else 0 + uleb128 + sleb128;
//            every entry ends with sleb128(col_delta); the base
//            line/col are the function record's debug line/col fields.
#include "bytecode_optimize.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace capsid {
namespace bytecode {

namespace {

// ---------------------------------------------------------------------------
// Opcode tables, built exactly like quickjs.c:1158/1166/21856.
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
#define def(id, size, n_pop, n_push, f)
#include "quickjs-opcode.h"
#undef def
#undef DEF
#undef FMT
    OP_COUNT, /* excluding temporary opcodes */
    /* temporary opcodes: overlap with the short opcodes */
    OP_TEMP_START = OP_nop + 1,
    OP___dummy = OP_TEMP_START - 1,
#define FMT(f)
#define DEF(id, size, n_pop, n_push, f)
#define def(id, size, n_pop, n_push, f) OP_##id,
#include "quickjs-opcode.h"
#undef def
#undef DEF
#undef FMT
    OP_TEMP_END,
};

struct OpInfo {
    uint8_t size;
    uint8_t n_pop;
    uint8_t n_push;
    OpFmt fmt;
    const char* name;
};

static const OpInfo opcode_info[OP_COUNT + (OP_TEMP_END - OP_TEMP_START)] = {
#define FMT(f)
#define DEF(id, size, n_pop, n_push, f) \
    { size, n_pop, n_push, OP_FMT_##f, #id },
#include "quickjs-opcode.h"
#undef DEF
#undef FMT
};

// Short opcodes (used after the final compile pass) overlap numerically
// with the never-serialized temporary opcodes; their descriptions live
// after the temporaries in opcode_info[].
static inline const OpInfo& short_opcode_info(int op) {
    return opcode_info[op >= OP_TEMP_START
                           ? op + (OP_TEMP_END - OP_TEMP_START)
                           : op];
}

// Bytecode serialization constants (quickjs.c:37660-37694).
enum BCTag {
    BC_TAG_NULL = 1,
    BC_TAG_UNDEFINED,
    BC_TAG_BOOL_FALSE,
    BC_TAG_BOOL_TRUE,
    BC_TAG_INT32,
    BC_TAG_FLOAT64,
    BC_TAG_STRING,
    BC_TAG_OBJECT,
    BC_TAG_ARRAY,
    BC_TAG_BIG_INT,
    BC_TAG_TEMPLATE_OBJECT,
    BC_TAG_FUNCTION_BYTECODE,
    BC_TAG_MODULE,
    BC_TAG_TYPED_ARRAY,
    BC_TAG_ARRAY_BUFFER,
    BC_TAG_SHARED_ARRAY_BUFFER,
    BC_TAG_REGEXP,
    BC_TAG_DATE,
    BC_TAG_OBJECT_VALUE,
    BC_TAG_OBJECT_REFERENCE,
    BC_TAG_MAP,
    BC_TAG_SET,
    BC_TAG_SYMBOL,
};
enum { BC_VERSION = 26 };

// pc2line encoding constants (quickjs.c:769-772).
static const int PC2LINE_BASE = -1;
static const int PC2LINE_RANGE = 5;
static const int PC2LINE_OP_FIRST = 1;
static const int PC2LINE_DIFF_PC_MAX = (255 - PC2LINE_OP_FIRST) / PC2LINE_RANGE;

// bc_csum (quickjs.c:37836-37860): h += u32le; h *= 0x9e370001; tail fold.
uint32_t bc_csum(const uint8_t* p, size_t n) {
    uint32_t h = 0;
    size_t i = 0;
    for (; i + 4 < n; i += 4) {
        h += static_cast<uint32_t>(p[i]) |
             (static_cast<uint32_t>(p[i + 1]) << 8) |
             (static_cast<uint32_t>(p[i + 2]) << 16) |
             (static_cast<uint32_t>(p[i + 3]) << 24);
        h *= 0x9e370001u;
    }
    uint32_t a = 0, b = 0, c = 0;
    switch (n - i) {
    case 3: c = p[i + 2];  // fall through
    case 2: b = p[i + 1];  // fall through
    case 1: a = p[i + 0];  // fall through
    case 0: break;
    }
    h += a | (b << 8) | (c << 16);
    h *= 0x9e370001u;
    return h;
}

// Canonical LEB128 writers (mirror dbuf_put_leb128 / dbuf_put_sleb128).
void put_leb128(std::vector<uint8_t>* out, uint32_t v) {
    while (v >= 0x80) {
        out->push_back(static_cast<uint8_t>(v | 0x80));
        v >>= 7;
    }
    out->push_back(static_cast<uint8_t>(v));
}
void put_sleb128(std::vector<uint8_t>* out, int32_t v) {
    uint32_t u = (static_cast<uint32_t>(v) << 1) ^
                 static_cast<uint32_t>(v >> 31);  // zigzag
    put_leb128(out, u);
}

// One function record: spans into the source buffer that the pass
// pipeline rewrites, plus the recursive child-function tree (child
// function bytecodes live in the cpool of their parent, in order).
struct FuncRecord {
    uint32_t fn_start;             // BC_TAG_FUNCTION_BYTECODE tag byte
    uint32_t fn_end;               // end of record (debug block end, or
                                   // code end when no debug block)
    uint32_t byte_code_len_off;    // offset of the byte_code_len leb128
    uint32_t byte_code_len_end;    // offset just past that leb128
    uint32_t code_off;             // start of the code blob
    uint32_t code_len;
    uint32_t stack_size;
    uint32_t var_count;
    uint32_t dbg_off;              // start of debug block (0 if absent)
    uint32_t dbg_pc2line_len_off;  // offset of the pc2line_len leb128
    uint32_t dbg_pc2line_off;      // start of the pc2line blob
    uint32_t dbg_pc2line_len;
    uint32_t dbg_end;              // end of debug block (== fn_end)
    int32_t dbg_line;              // function's declared line (pc2line
                                   // base, quickjs.c find_line_num)
    int32_t dbg_col;
    // Extents of child function records inside [fn_start, code_off),
    // as [start, end) pairs in serialization order. Everything outside
    // them (and outside the code blob / debug block) is copied
    // verbatim at rebuild time.
    std::vector<uint32_t> child_spans;
    std::vector<FuncRecord> children;
};

// ---------------------------------------------------------------------------
// Reader: strict bounds-checked cursor over the serialized buffer.
// Every access is checked; any violation fails closed.
// ---------------------------------------------------------------------------

class Reader {
public:
    Reader(const uint8_t* data, size_t size, std::string* error)
        : p_(data), end_(data + size), base_(data), error_(error) {}

    bool u8(uint8_t* out) {
        if (p_ + 1 > end_) return fail("truncated u8");
        *out = *p_++;
        return true;
    }
    bool u16le(uint16_t* out) {
        if (p_ + 2 > end_) return fail("truncated u16");
        *out = static_cast<uint16_t>(p_[0]) |
               (static_cast<uint16_t>(p_[1]) << 8);
        p_ += 2;
        return true;
    }
    bool u32le(uint32_t* out) {
        if (p_ + 4 > end_) return fail("truncated u32");
        *out = static_cast<uint32_t>(p_[0]) |
               (static_cast<uint32_t>(p_[1]) << 8) |
               (static_cast<uint32_t>(p_[2]) << 16) |
               (static_cast<uint32_t>(p_[3]) << 24);
        p_ += 4;
        return true;
    }
    bool u64le(uint64_t* out) {
        uint32_t lo, hi;
        if (!u32le(&lo) || !u32le(&hi)) return false;
        *out = static_cast<uint64_t>(lo) | (static_cast<uint64_t>(hi) << 32);
        return true;
    }
    // LEB128, max 5 bytes (quickjs.c get_leb128).
    bool leb128(uint32_t* out) {
        uint32_t v = 0;
        for (int i = 0; i < 5; i++) {
            if (p_ >= end_) return fail("truncated leb128");
            uint8_t a = *p_++;
            v |= static_cast<uint32_t>(a & 0x7f) << (i * 7);
            if (!(a & 0x80)) {
                *out = v;
                return true;
            }
        }
        return fail("leb128 too long");
    }
    // Zigzag sleb128 (quickjs.c dbuf_put_sleb128/get_sleb128).
    bool sleb128(int32_t* out) {
        uint32_t v;
        if (!leb128(&v)) return false;
        *out = static_cast<int32_t>((v >> 1) ^ static_cast<uint32_t>(-(v & 1)));
        return true;
    }
    bool skip(size_t n) {
        if (n > static_cast<size_t>(end_ - p_)) return fail("truncated blob");
        p_ += n;
        return true;
    }
    size_t offset() const { return static_cast<size_t>(p_ - base_); }
    const uint8_t* pos() const { return p_; }
    bool at_end() const { return p_ == end_; }
    bool fail_public(const char* msg) { return fail(msg); }

    // bc_put_atom: leb128(v) where table atoms are v = idx<<1 and
    // tagged-int atoms are v = (u32<<1)|1 (quickjs.c:37821-37834).
    // The optimizer never interprets atoms; it skips them.
    bool skip_atom() {
        uint32_t v;
        return leb128(&v);
    }

    // bc_put_string / JS_WriteString (quickjs.c:37906-37916):
    // leb128((len<<1)|is_wide) + len bytes or len x u16le.
    bool skip_string() {
        uint32_t len2 = 0;
        if (!leb128(&len2)) return false;
        uint32_t len = len2 >> 1;
        uint64_t nbytes = len * (len2 & 1 ? 2ull : 1ull);
        if (nbytes > static_cast<uint64_t>(end_ - p_)) {
            return fail("truncated string");
        }
        p_ += static_cast<size_t>(nbytes);
        return true;
    }

    // BC_TAG_* value record: one serialized JS value, recursively.
    // FUNCTION_BYTECODE entries recurse into function records and are
    // collected via the on_function callback.
    bool skip_object_rec(
        std::vector<FuncRecord>* children,
        std::string* error,
        int depth) {
        if (depth > 4096) return fail("bytecode nesting too deep");
        uint8_t tag = 0;
        uint32_t fn_start = static_cast<uint32_t>(offset());
        if (!u8(&tag)) return false;
        switch (tag) {
        case BC_TAG_NULL:
        case BC_TAG_UNDEFINED:
        case BC_TAG_BOOL_FALSE:
        case BC_TAG_BOOL_TRUE:
            return true;
        case BC_TAG_INT32: {
            int32_t v = 0;
            return sleb128(&v);
        }
        case BC_TAG_FLOAT64:
            return skip(8);
        case BC_TAG_STRING:
            return skip_string();
        case BC_TAG_SYMBOL:
            return skip_atom();
        case BC_TAG_OBJECT_REFERENCE: {
            uint32_t v = 0;
            return leb128(&v);
        }
        case BC_TAG_TYPED_ARRAY: {
            uint8_t class_id = 0;
            uint32_t count = 0, off = 0;
            if (!u8(&class_id) || !leb128(&count) || !leb128(&off)) {
                return false;
            }
            return skip_object_rec(children, error, depth + 1);
        }
        case BC_TAG_ARRAY_BUFFER:
        case BC_TAG_SHARED_ARRAY_BUFFER: {
            uint32_t len = 0, max_len = 0;
            if (!leb128(&len) || !leb128(&max_len)) return false;
            if (tag == BC_TAG_ARRAY_BUFFER) return skip(len);
            return skip(8);  // u64 SAB pointer
        }
        case BC_TAG_ARRAY:
        case BC_TAG_TEMPLATE_OBJECT: {
            uint32_t len = 0;
            if (!leb128(&len)) return false;
            for (uint32_t i = 0; i < len; i++) {
                if (!skip_object_rec(children, error, depth + 1)) return false;
            }
            if (tag == BC_TAG_TEMPLATE_OBJECT) {
                // trailing raw-array record
                return skip_object_rec(children, error, depth + 1);
            }
            return true;
        }
        case BC_TAG_OBJECT: {
            uint32_t prop_count = 0;
            if (!leb128(&prop_count)) return false;
            for (uint32_t i = 0; i < prop_count; i++) {
                if (!skip_atom() ||
                    !skip_object_rec(children, error, depth + 1)) {
                    return false;
                }
            }
            return true;
        }
        case BC_TAG_REGEXP:
            // two strings, no tags (JS_WriteRegExp)
            return skip_string() && skip_string();
        case BC_TAG_DATE:
        case BC_TAG_OBJECT_VALUE:
            return skip_object_rec(children, error, depth + 1);
        case BC_TAG_MAP:
        case BC_TAG_SET: {
            uint32_t count = 0;
            if (!leb128(&count)) return false;
            for (uint32_t i = 0; i < count; i++) {
                if (!skip_object_rec(children, error, depth + 1)) return false;
                if (tag == BC_TAG_MAP) {
                    if (!skip_object_rec(children, error, depth + 1)) {
                        return false;
                    }
                }
            }
            return true;
        }
        case BC_TAG_BIG_INT: {
            uint32_t len = 0;
            if (!leb128(&len)) return false;
            return skip(len);
        }
        case BC_TAG_FUNCTION_BYTECODE:
            return skip_function(children, error, depth, fn_start);
        default:
            return fail("unknown BC tag");
        }
    }

    // Full function record (JS_WriteFunctionTag quickjs.c:37965-38058).
    bool skip_function(std::vector<FuncRecord>* children,
                       std::string* error,
                       int depth,
                       uint32_t fn_start) {
        if (depth > 4096) return fail("bytecode nesting too deep");
        uint16_t flags = 0;
        uint8_t strict = 0;
        if (!u16le(&flags) || !u8(&strict) || !skip_atom()) return false;
        uint32_t arg_count = 0, var_count = 0, defined_arg_count = 0;
        uint32_t stack_size = 0;
        uint32_t var_ref_count = 0, closure_var_count = 0, cpool_count = 0;
        uint32_t byte_code_len = 0;
        if (!leb128(&arg_count) || !leb128(&var_count) ||
            !leb128(&defined_arg_count) || !leb128(&stack_size) ||
            !leb128(&var_ref_count) || !leb128(&closure_var_count) ||
            !leb128(&cpool_count)) {
            return false;
        }
        uint32_t byte_code_len_off = static_cast<uint32_t>(offset());
        if (!leb128(&byte_code_len)) return false;
        uint32_t byte_code_len_end = static_cast<uint32_t>(offset());
        // vardefs: leb128 count + per entry atom, leb128, leb128, u8,
        // optional leb128 var_ref_idx when captured.
        uint32_t vardef_count = 0;
        if (!leb128(&vardef_count)) return false;
        for (uint32_t i = 0; i < vardef_count; i++) {
            if (!skip_atom()) return false;
            uint32_t scope_level = 0, scope_next = 0;
            uint8_t vflags = 0;
            if (!leb128(&scope_level) || !leb128(&scope_next) ||
                !u8(&vflags)) {
                return false;
            }
            // var_kind(4 bits) + is_const(1) + is_lexical(1) +
            // is_captured(1) → bit 6 (0x40) per bc_set_flags order.
            if (vflags & 0x40) {  // is_captured
                uint32_t ref_idx;
                if (!leb128(&ref_idx)) return false;
            }
        }
        // closure vars: per entry atom, leb128 var_idx, leb128 flags
        // (count is closure_var_count from the header; the flags field
        // is LEB128 — bc_put_leb128(s, flags), not a u8).
        for (uint32_t i = 0; i < closure_var_count; i++) {
            if (!skip_atom()) return false;
            uint32_t var_idx, cflags;
            if (!leb128(&var_idx) || !leb128(&cflags)) return false;
        }
        // cpool first: cpool_count values, recursing into children.
        // Child function bytecodes appear here in order.
        size_t child_base = children->size();
        for (uint32_t i = 0; i < cpool_count; i++) {
            if (!skip_object_rec(children, error, depth + 1)) return false;
        }
        // code blob
        uint32_t code_off = static_cast<uint32_t>(offset());
        if (!skip(byte_code_len)) return false;
        // debug block (bit 11 of flags = allow_debug: bc_set_flags
        // accumulates idx through prototype/simple/derived/home/func_kind
        // (4)/new_target/super_call/super/arguments/backtrace, so the
        // allow_debug bit lands at 11, 0x800)
        FuncRecord rec;
        rec.fn_start = fn_start;
        rec.byte_code_len_off = byte_code_len_off;
        rec.byte_code_len_end = byte_code_len_end;
        rec.code_off = code_off;
        rec.code_len = byte_code_len;
        rec.stack_size = stack_size;
        rec.var_count = var_count;
        rec.dbg_off = 0;
        rec.dbg_pc2line_len_off = 0;
        rec.dbg_pc2line_off = 0;
        rec.dbg_pc2line_len = 0;
        rec.dbg_end = code_off + byte_code_len;
        rec.dbg_line = 0;
        rec.dbg_col = 0;
        if (flags & (1u << 11)) {
            rec.dbg_off = static_cast<uint32_t>(offset());
            if (!skip_atom()) return false;  // filename
            uint32_t line, col;
            if (!leb128(&line) || !leb128(&col)) return false;
            rec.dbg_line = static_cast<int32_t>(line);
            rec.dbg_col = static_cast<int32_t>(col);
            rec.dbg_pc2line_len_off = static_cast<uint32_t>(offset());
            uint32_t pc2line_len;
            if (!leb128(&pc2line_len)) return false;
            rec.dbg_pc2line_off = static_cast<uint32_t>(offset());
            rec.dbg_pc2line_len = pc2line_len;
            if (!skip(pc2line_len)) return false;
            uint32_t source_len;
            if (!leb128(&source_len)) return false;
            if (!skip(source_len)) return false;
            rec.dbg_end = static_cast<uint32_t>(offset());
        }
        rec.fn_end = rec.dbg_end;
        rec.child_spans.clear();
        rec.children.assign(children->begin() + child_base, children->end());
        children->resize(child_base);
        for (size_t i = 0; i < rec.children.size(); i++) {
            rec.child_spans.push_back(rec.children[i].fn_start);
            rec.child_spans.push_back(rec.children[i].fn_end);
        }
        children->push_back(rec);
        return true;
    }

    // Module record (JS_WriteModule quickjs.c:38060-38108).
    bool skip_module(std::vector<FuncRecord>* children, std::string* error) {
        if (!skip_atom()) return false;  // module_name
        uint32_t req_count = 0;
        if (!leb128(&req_count)) return false;
        for (uint32_t i = 0; i < req_count; i++) {
            if (!skip_atom()) return false;
        }
        uint32_t export_count = 0;
        if (!leb128(&export_count)) return false;
        for (uint32_t i = 0; i < export_count; i++) {
            uint8_t export_type = 0;
            if (!u8(&export_type)) return false;
            if (export_type == 0) {  // JS_EXPORT_TYPE_LOCAL
                uint32_t v = 0;
                if (!leb128(&v)) return false;
            } else {
                uint32_t v = 0;
                if (!leb128(&v) || !skip_atom()) return false;
            }
            if (!skip_atom()) return false;  // export_name
        }
        uint32_t star_count = 0;
        if (!leb128(&star_count)) return false;
        for (uint32_t i = 0; i < star_count; i++) {
            uint32_t v = 0;
            if (!leb128(&v)) return false;
        }
        uint32_t import_count = 0;
        if (!leb128(&import_count)) return false;
        for (uint32_t i = 0; i < import_count; i++) {
            uint32_t v = 0, v2 = 0;
            if (!leb128(&v) || !skip_atom() || !leb128(&v2)) return false;
        }
        uint8_t has_tla = 0;
        if (!u8(&has_tla)) return false;
        // JS_WriteModule ends with JS_WriteObjectRec(s, m->func_obj),
        // which writes the BC_TAG_FUNCTION_BYTECODE tag byte; consume it
        // before the function record.
        uint32_t fn_start = static_cast<uint32_t>(offset());
        uint8_t tag = 0;
        if (!u8(&tag)) return false;
        if (tag != BC_TAG_FUNCTION_BYTECODE) {
            return fail_public("module function is not bytecode");
        }
        if (!skip_function(children, error, 0, fn_start)) return false;
        return true;
    }

private:
    bool fail(const char* msg) {
        if (error_ && error_->empty()) {
            *error_ = std::string("bytecode optimize: ") + msg +
                       " at offset " + std::to_string(offset());
        }
        return false;
    }
    const uint8_t* p_;
    const uint8_t* end_;
    const uint8_t* base_;
    std::string* error_;
};

// ---------------------------------------------------------------------------
// Header parse + top-level dispatch.
// ---------------------------------------------------------------------------

bool parse_buffer(const uint8_t* data,
                  size_t size,
                  std::vector<FuncRecord>* functions,
                  std::string* error) {
    Reader r(data, size, error);
    uint8_t version = 0;
    if (!r.u8(&version)) return false;
    if (version != BC_VERSION) {
        *error = "bytecode optimize: unsupported bytecode version " +
                 std::to_string(version);
        return false;
    }
    uint32_t stored_csum = 0;
    if (!r.u32le(&stored_csum)) return false;
    uint32_t actual_csum = bc_csum(data + 5, size - 5);
    if (stored_csum != actual_csum) {
        *error = "bytecode optimize: checksum mismatch";
        return false;
    }
    uint32_t atom_count = 0;
    if (!r.leb128(&atom_count)) return false;
    if (atom_count > 1000000) {
        *error = "bytecode optimize: atom count too large";
        return false;
    }
    // atom table: per entry u8 type (JS_ATOM_TYPE_*, quickjs.c:592-596);
    // type 0 = const atom followed by a u32, types 1..3 (STRING,
    // GLOBAL_SYMBOL, SYMBOL) followed by a string. PRIVATE (4) is
    // asserted never serialized; anything else fails closed.
    for (uint32_t i = 0; i < atom_count; i++) {
        uint8_t type = 0;
        if (!r.u8(&type)) return false;
        if (type == 0) {
            // Const atom: full u32 value, not LEB128 (quickjs.c
            // JS_WriteObjectAtoms, "bc_put_u32(s, atom)").
            uint32_t v = 0;
            if (!r.u32le(&v)) return false;
        } else if (type >= 1 && type <= 3) {
            if (!r.skip_string()) return false;
        } else {
            return r.fail_public("unexpected atom type");
        }
    }
    uint8_t tag = 0;
    if (!r.u8(&tag)) return false;
    if (tag != BC_TAG_MODULE) {
        *error = "bytecode optimize: top-level record is not a module";
        return false;
    }
    if (!r.skip_module(functions, error)) return false;
    if (!r.at_end()) {
        *error = "bytecode optimize: trailing bytes after module record";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Foldability scan (Step 0): decode each function's code stream and
// count what the v1 pass pipeline would remove or shrink. This is the
// theoretical-ceiling measurement; it never rewrites.
// ---------------------------------------------------------------------------

struct FoldStats {
    uint64_t insns = 0;
    uint64_t bytes = 0;
    uint64_t foldable_insns = 0;
    uint64_t foldable_bytes = 0;    // bytes that disappear (v1 folds)
    uint64_t shrinkable_bytes = 0;  // bytes saved by re-shrink only
};

// Read the immediate of a small-int push opcode. Caller guarantees the
// opcode is one of push_minus1..push_7 / push_i8 / push_i16 / push_i32.
int64_t push_value(const uint8_t* code, uint8_t op) {
    switch (op) {
    case OP_push_minus1: return -1;
    case OP_push_0: return 0;
    case OP_push_1: return 1;
    case OP_push_2: return 2;
    case OP_push_3: return 3;
    case OP_push_4: return 4;
    case OP_push_5: return 5;
    case OP_push_6: return 6;
    case OP_push_7: return 7;
    case OP_push_i8: return static_cast<int8_t>(code[1]);
    case OP_push_i16:
        return static_cast<int16_t>(
            static_cast<uint16_t>(code[1]) |
            (static_cast<uint16_t>(code[2]) << 8));
    case OP_push_i32:
        return static_cast<int32_t>(
            static_cast<uint32_t>(code[1]) |
            (static_cast<uint32_t>(code[2]) << 8) |
            (static_cast<uint32_t>(code[3]) << 16) |
            (static_cast<uint32_t>(code[4]) << 24));
    default:
        return 0;  // unreachable
    }
}

// JS i32 binop folding rules: the result of and/or/xor/shl/sar/shr is
// always an int32 (shift counts mask by 31, JS ToInt32 on inputs), and
// mod of int32s is an int32 when the divisor is non-zero. Only add,
// sub, mul need a range check — overflow produces a float64, which the
// fold cannot represent without a cpool entry.
bool foldable_binop(uint8_t op, int64_t a, int64_t b, int64_t* out) {
    switch (op) {
    case OP_add:
    case OP_sub:
    case OP_mul: {
        int64_t r = op == OP_add ? a + b : op == OP_sub ? a - b : a * b;
        if (r < INT32_MIN || r > INT32_MAX) return false;
        *out = r;
        return true;
    }
    case OP_and: *out = a & b; return true;
    case OP_or: *out = a | b; return true;
    case OP_xor: *out = a ^ b; return true;
    case OP_shl: *out = (int64_t)(int32_t)((uint32_t)a << (b & 31)); return true;
    case OP_sar: *out = (int64_t)((int32_t)a >> (b & 31)); return true;
    case OP_shr: *out = (int64_t)(int32_t)((uint32_t)a >> (b & 31)); return true;
    case OP_mod:
        if (b == 0) return false;
        *out = a % b;
        return true;
    default:
        return false;
    }
}

// Full foldability scan over a function tree. Patterns counted are
// exactly the v1 pipeline patterns (docs/bytecode-aot-optimizer.md §5
// P3/P4/P6); the estimate is deliberately conservative.
//   P3.1: push small-int + push small-int + binop (i32, no overflow)
//         -> single push; removes the two pushes and the binop, emits
//         the shortest push form.
//   P3.4: push_* + drop -> both removed.
//   P3.5: dup drop / dup2 drop / swap swap / rot3l rot3r.
//   P6:   long -> short immediate re-encoding (shrinkable bytes).
bool scan_function(const FuncRecord& f,
                   const uint8_t* data,
                   FoldStats* st) {
    const uint8_t* code = data + f.code_off;
    const size_t len = f.code_len;
    size_t pc = 0;
    while (pc < len) {
        uint8_t op = code[pc];
        if (op == 0 || op >= OP_COUNT) return false;
        const OpInfo& oi = short_opcode_info(op);
        if (pc + oi.size > len) return false;
        st->insns++;
        st->bytes += oi.size;

        // --- lookahead patterns (checked at the push site) ---
        bool push_noexcept = (op >= OP_push_minus1 && op <= OP_push_7) ||
                             op == OP_push_i8 || op == OP_push_i16 ||
                             op == OP_push_i32 || op == OP_push_const ||
                             op == OP_push_const8 || op == OP_push_atom_value ||
                             op == OP_push_true || op == OP_push_false ||
                             op == OP_undefined || op == OP_null ||
                             op == OP_push_this || op == OP_push_empty_string;
        if (push_noexcept && pc + oi.size + 1 <= len) {
            size_t q = pc + oi.size;
            uint8_t op2 = code[q];
            if (op2 == OP_drop) {
                // P3.4: push + drop -> both removed
                st->foldable_insns += 2;
                st->foldable_bytes += oi.size + 1;
            }
        }
        // P3.1: push + push + binop with small-int immediates
        if (op == OP_push_i32 || op == OP_push_i8 || op == OP_push_i16 ||
            (op >= OP_push_minus1 && op <= OP_push_7)) {
            size_t q = pc + oi.size;
            if (q + 1 <= len) {
                uint8_t op2 = code[q];
                bool push2 = (op2 >= OP_push_minus1 && op2 <= OP_push_7) ||
                             op2 == OP_push_i8 || op2 == OP_push_i16 ||
                             op2 == OP_push_i32;
                if (push2) {
                    size_t s2 = short_opcode_info(op2).size;
                    if (q + s2 + 1 <= len) {
                        uint8_t op3 = code[q + s2];
                        if (op3 == OP_add || op3 == OP_sub || op3 == OP_mul ||
                            op3 == OP_and || op3 == OP_or || op3 == OP_xor ||
                            op3 == OP_shl || op3 == OP_sar || op3 == OP_shr ||
                            op3 == OP_mod) {
                            int64_t a = push_value(code + pc, op);
                            int64_t b = push_value(code + q, op2);
                            int64_t r;
                            if (foldable_binop(op3, a, b, &r)) {
                                st->foldable_insns += 3;
                                st->foldable_bytes += oi.size + s2 +
                                    short_opcode_info(op3).size;
                            }
                        }
                    }
                }
            }
        }
        // P3.5: dup drop / dup2 drop -> dup1; swap swap; rot3l rot3r
        if (op == OP_dup && pc + 1 + 1 <= len && code[pc + 1] == OP_drop) {
            st->foldable_insns += 2;
            st->foldable_bytes += 2;
        }
        if (op == OP_dup2 && pc + 1 + 1 <= len && code[pc + 1] == OP_drop) {
            st->foldable_insns += 1;  // dup2 drop -> dup1
            st->foldable_bytes += 1;
        }
        if (op == OP_swap && pc + 1 + 1 <= len && code[pc + 1] == OP_swap) {
            st->foldable_insns += 2;
            st->foldable_bytes += 2;
        }
        if ((op == OP_rot3l && pc + 1 + 1 <= len && code[pc + 1] == OP_rot3r) ||
            (op == OP_rot3r && pc + 1 + 1 <= len && code[pc + 1] == OP_rot3l)) {
            st->foldable_insns += 2;
            st->foldable_bytes += 2;
        }
        // P6 shrinkable: push_i32 -> push_i8 / push_0..7 / push_minus1
        if (op == OP_push_i32) {
            int32_t v = static_cast<int32_t>(
                static_cast<uint32_t>(code[pc + 1]) |
                (static_cast<uint32_t>(code[pc + 2]) << 8) |
                (static_cast<uint32_t>(code[pc + 3]) << 16) |
                (static_cast<uint32_t>(code[pc + 4]) << 24));
            if (v >= -128 && v <= 127) {
                st->shrinkable_bytes += (v >= -1 && v <= 7) ? 4 : 3;
            } else if (v >= -32768 && v <= 32767) {
                st->shrinkable_bytes += 2;
            }
        }
        pc += oi.size;
    }
    for (size_t i = 0; i < f.children.size(); i++) {
        if (!scan_function(f.children[i], data, st)) return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Pass pipeline: instruction decode.
// ---------------------------------------------------------------------------

struct Insn {
    uint16_t op;
    uint32_t old_off;   // byte offset in the original code blob
    uint8_t old_size;   // original encoded size
    uint32_t pc_off;    // pc2line source offset (own old_off unless a
                        // fold replaced this instruction)
    int32_t target;     // insn index for jumps, else -1
    int64_t imm;        // small-int value for push_minus1..push_i32
    uint32_t aux;       // cpool idx (push_const/fclosure), var idx
                        // (get/put/set loc/arg/var_ref), argc (call)
    bool has_aux;
};

bool is_small_int_push(uint8_t op) {
    return (op >= OP_push_minus1 && op <= OP_push_7) ||
           op == OP_push_i8 || op == OP_push_i16 || op == OP_push_i32;
}
bool is_push_noexcept(uint8_t op) {
    return is_small_int_push(op) || op == OP_push_const ||
           op == OP_push_const8 || op == OP_push_atom_value ||
           op == OP_push_true || op == OP_push_false || op == OP_undefined ||
           op == OP_null || op == OP_push_this || op == OP_push_empty_string;
}
// Literal pushes with known identity (P3.2 const-vs-const folding).
int literal_value(uint8_t op) {
    switch (op) {
    case OP_undefined: return 0;
    case OP_push_false: return 1;
    case OP_push_true: return 2;
    case OP_null: return 3;
    default: return -1;
    }
}
// P3.2 lattice value of a push: literals in {kNull,kUndef,kBool} plus any
// small-int push (its immediate). Returns false for non-constant pushes.
// Small ints are offset by +4 so their value space never collides with
// the literal tags above; equality is what matters, not the encoding.
bool push_const_value(const Insn& in, int64_t* v) {
    if (is_small_int_push(in.op)) {
        *v = static_cast<int64_t>(in.imm) + 4;
        return true;
    }
    int lv = literal_value(in.op);
    if (lv < 0) return false;
    *v = lv;
    return true;
}
bool is_cond_jump(uint8_t op) {
    return op == OP_if_true || op == OP_if_false || op == OP_if_true8 ||
           op == OP_if_false8;
}
bool is_jump_op(uint8_t op) {
    return is_cond_jump(op) || op == OP_goto || op == OP_goto8 ||
           op == OP_goto16 || op == OP_catch || op == OP_gosub;
}
bool is_with_jump(uint8_t op) {
    return op == OP_with_get_var || op == OP_with_put_var ||
           op == OP_with_delete_var || op == OP_with_make_ref ||
           op == OP_with_get_ref || op == OP_with_get_ref_undef;
}
bool is_foldable_binop_op(uint8_t op) {
    return op == OP_add || op == OP_sub || op == OP_mul || op == OP_and ||
           op == OP_or || op == OP_xor || op == OP_shl || op == OP_sar ||
           op == OP_shr || op == OP_mod;
}
// Shortest push opcode for a value known to fit in int32.
uint8_t shortest_push_op(int64_t v) {
    if (v == -1) return OP_push_minus1;
    if (v >= 0 && v <= 7) return static_cast<uint8_t>(OP_push_0 + v);
    if (v >= -128 && v <= 127) return OP_push_i8;
    if (v >= -32768 && v <= 32767) return OP_push_i16;
    return OP_push_i32;
}

// Decode a code blob into an instruction list, resolving every jump
// operand to an instruction index (self-relative target = operand
// start + signed offset; labels were removed in resolve_labels, so
// targets may point at any instruction boundary).
bool decode_code(const uint8_t* code,
                 size_t len,
                 std::vector<Insn>* insns,
                 std::string* error) {
    size_t pc = 0;
    while (pc < len) {
        uint8_t op = code[pc];
        if (op == 0 || op >= OP_COUNT) {
            *error = "bytecode optimize: invalid opcode in function";
            return false;
        }
        const OpInfo& oi = short_opcode_info(op);
        if (pc + oi.size > len) {
            *error = "bytecode optimize: truncated instruction";
            return false;
        }
        Insn in;
        in.op = op;
        in.old_off = static_cast<uint32_t>(pc);
        in.old_size = oi.size;
        in.pc_off = in.old_off;
        in.target = -1;
        in.imm = 0;
        in.aux = 0;
        in.has_aux = false;
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
        case OP_using_dispose:
            in.aux = static_cast<uint16_t>(code[pc + 1]) |
                     (static_cast<uint16_t>(code[pc + 2]) << 8);
            in.has_aux = true;
            break;
        case OP_get_loc8: case OP_put_loc8: case OP_set_loc8:
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
        insns->push_back(in);
        pc += oi.size;
    }
    // Resolve jump operands to instruction indexes. Offsets are
    // strictly increasing, so a binary search over instruction starts
    // suffices.
    std::vector<uint32_t> offs;
    offs.reserve(insns->size());
    for (size_t i = 0; i < insns->size(); i++) {
        offs.push_back((*insns)[i].old_off);
    }
    for (size_t i = 0; i < insns->size(); i++) {
        Insn& in = (*insns)[i];
        if (in.target < 0) continue;
        int32_t want = in.target;
        if (want < 0 || static_cast<size_t>(want) >= len) {
            *error = "bytecode optimize: jump target out of range";
            return false;
        }
        std::vector<uint32_t>::const_iterator it =
            std::lower_bound(offs.begin(), offs.end(),
                             static_cast<uint32_t>(want));
        if (it == offs.end() || *it != static_cast<uint32_t>(want)) {
            *error = "bytecode optimize: jump target not on instruction "
                     "boundary";
            return false;
        }
        in.target = static_cast<int32_t>(it - offs.begin());
    }
    return true;
}

// ---------------------------------------------------------------------------
// Pass pipeline: P3 peepholes + P6 re-shrink, iterated to a fixpoint.
// ---------------------------------------------------------------------------

struct RewriteStats {
    uint64_t folds_p31 = 0;  // const binop folds
    uint64_t folds_p32 = 0;  // const strict_eq/neq folds
    uint64_t folds_p34 = 0;  // push+drop eliminations
    uint64_t folds_p35 = 0;  // dup/swap/rot cleanups
    uint64_t folds_p36 = 0;  // literal-condition folds
    uint64_t folds_p2 = 0;   // P2: cross-BB constant propagation replaces
    uint64_t threads = 0;    // P4: goto-chain threading + next-jump folds
    uint64_t dead_blocks = 0;  // P5: unreachable block eliminations
    uint64_t shrinks = 0;    // short-form re-encodings
    uint64_t insns_before = 0;
    uint64_t insns_after = 0;
    uint64_t bytes_before = 0;
    uint64_t bytes_after = 0;
};

// One sweep of P3 peepholes over the (tombstone-marked) instruction
// list. Returns true if anything was deleted or replaced. No pattern
// touches an instruction that is a jump target, and every rewrite is
// stack-neutral and control-flow-preserving, so the result is valid
// whenever the input was.
// Index of the next live (non-tombstoned) instruction at or after
// `start`. Peepholes match against the logical instruction stream —
// tombstones are gone as far as the emitted code is concerned, so a
// fold must be able to see through them within the same round.
size_t next_live(const std::vector<Insn>& insns,
                 const std::vector<uint8_t>& dead,
                 size_t start) {
    while (start < insns.size() && dead[start]) start++;
    return start;
}

bool apply_peepholes(std::vector<Insn>* insns,
                     std::vector<uint8_t>* dead,
                     uint32_t passes,
                     RewriteStats* stats) {
    bool changed = false;
    size_t n = insns->size();
    size_t i = 0;
    while (i < n) {
        if ((*dead)[i]) {
            i++;
            continue;
        }
        const Insn& a = (*insns)[i];
        // P3.1: push a, push b, binop -> push result (i32, no overflow).
        // Jumps may target instructions inside a folded run: the
        // compact pass lands such targets on the first live
        // instruction after the run (stack-effect equivalent).
        if ((passes & kPassP31) && is_small_int_push(a.op) &&
            i + 2 < n && !(*dead)[i + 1] && !(*dead)[i + 2] &&
            is_small_int_push((*insns)[i + 1].op) &&
            is_foldable_binop_op((*insns)[i + 2].op)) {
            int64_t r;
            if (foldable_binop((*insns)[i + 2].op, a.imm,
                               (*insns)[i + 1].imm, &r)) {
                Insn ni;
                ni.op = shortest_push_op(r);
                ni.old_off = a.old_off;
                ni.old_size = short_opcode_info(ni.op).size;
                // The result reports the binop's line (quickjs
                // convention: a folded sequence takes the last replaced
                // instruction's location).
                ni.pc_off = (*insns)[i + 2].old_off;
                ni.target = -1;
                ni.imm = r;
                ni.has_aux = false;
                (*insns)[i] = ni;
                (*dead)[i + 1] = (*dead)[i + 2] = 1;
                stats->folds_p31++;
                changed = true;
                continue;  // re-examine the replacement at i
            }
        }
        // P3.2: const, const, strict_eq/strict_neq -> push_bool. The
        // lattice covers {kNull,kUndef,kBool,kSmallInt}; small-int
        // immediates compare by value (0 === -0 holds for ints, and
        // float64 -0 is outside the int32 lattice, so this is exact).
        // b/c skip tombstones: a P3.1 fold earlier in this round leaves
        // the old pushes dead, and the comparison must still be foldable
        // in the same round (the emitted stream never contains them).
        if (passes & kPassP32) {
            size_t b = next_live(*insns, *dead, i + 1);
            size_t c = next_live(*insns, *dead, b + 1);
            if (b < n && c < n &&
                ((*insns)[c].op == OP_strict_eq ||
                 (*insns)[c].op == OP_strict_neq)) {
                int64_t lv, rv;
                if (push_const_value(a, &lv) &&
                    push_const_value((*insns)[b], &rv)) {
                    bool same = lv == rv;
                    bool res = (*insns)[c].op == OP_strict_eq ? same : !same;
                    Insn ni;
                    ni.op = res ? OP_push_true : OP_push_false;
                    ni.old_off = a.old_off;
                    ni.old_size = 1;
                    ni.pc_off = (*insns)[c].old_off;
                    ni.target = -1;
                    ni.imm = 0;
                    ni.has_aux = false;
                    (*insns)[i] = ni;
                    (*dead)[b] = (*dead)[c] = 1;
                    stats->folds_p32++;
                    changed = true;
                    continue;
                }
            }
        }
        // P3.4: push_noexcept, drop -> both removed.
        if ((passes & kPassP34) && i + 1 < n && !(*dead)[i + 1] &&
            is_push_noexcept(a.op) && (*insns)[i + 1].op == OP_drop) {
            (*dead)[i] = (*dead)[i + 1] = 1;
            stats->folds_p34++;
            changed = true;
            i += 2;
            continue;
        }
        // P3.5: dup drop -> removed; dup2 drop -> dup1; swap swap and
        // rot3l rot3r -> removed.
        if ((passes & kPassP35) && i + 1 < n && !(*dead)[i + 1]) {
            const Insn& b = (*insns)[i + 1];
            if (a.op == OP_dup && b.op == OP_drop) {
                (*dead)[i] = (*dead)[i + 1] = 1;
                stats->folds_p35++;
                changed = true;
                i += 2;
                continue;
            }
            if (a.op == OP_dup2 && b.op == OP_drop) {
                (*insns)[i].op = OP_dup1;
                (*insns)[i].old_size = 1;
                (*dead)[i + 1] = 1;
                stats->folds_p35++;
                changed = true;
                i += 2;
                continue;
            }
            if ((a.op == OP_swap && b.op == OP_swap) ||
                (a.op == OP_rot3l && b.op == OP_rot3r) ||
                (a.op == OP_rot3r && b.op == OP_rot3l)) {
                (*dead)[i] = (*dead)[i + 1] = 1;
                stats->folds_p35++;
                changed = true;
                i += 2;
                continue;
            }
        }
        // P3.6: literal true/false followed by a conditional jump. The
        // jump is found through tombstones (a P3.2 fold this round may
        // have deadened the instructions between the push and the jump).
        if (passes & kPassP36) {
            size_t jj = next_live(*insns, *dead, i + 1);
            if (jj < n && (a.op == OP_push_true || a.op == OP_push_false) &&
                is_cond_jump((*insns)[jj].op)) {
                const Insn& j = (*insns)[jj];
                bool cond = a.op == OP_push_true;
                bool takes = (j.op == OP_if_true || j.op == OP_if_true8)
                                 ? cond
                                 : !cond;
                if (!takes) {
                    // Never taken: both removed; execution continues at
                    // the next surviving instruction.
                    (*dead)[i] = (*dead)[jj] = 1;
                    stats->folds_p36++;
                    changed = true;
                    i = jj + 1;
                    continue;
                }
                // Always taken: drop the push, replace the jump with a
                // goto to the same target. For short-form jumps, only
                // fold when the target is close enough that the
                // replacement cannot outgrow the original (removals
                // before a forward target lengthen its distance; keep
                // 32 bytes of slack).
                if (j.op != OP_if_true && j.op != OP_if_false) {
                    uint32_t target_off =
                        (*insns)[static_cast<size_t>(j.target)].old_off;
                    int64_t dist = static_cast<int64_t>(target_off) -
                                   (static_cast<int64_t>(j.old_off) + 1);
                    if (dist > 95 || dist < -128) {
                        i++;  // leave the pair alone
                        continue;
                    }
                }
                (*insns)[jj].op = OP_goto;
                (*insns)[jj].old_size = 5;
                (*dead)[i] = 1;
                stats->folds_p36++;
                changed = true;
                i = jj + 1;
                continue;
            }
        }
        i++;
    }
    return changed;
}

// P6: re-encode immediates to the shortest form (value for small ints,
// index width for cpool/variable references, argc for calls). Jump
// distances are handled at emission time.
void apply_reshrink(std::vector<Insn>* insns, RewriteStats* stats) {
    for (size_t i = 0; i < insns->size(); i++) {
        Insn& in = (*insns)[i];
        uint16_t op = in.op;
        switch (op) {
        case OP_push_i32:
        case OP_push_i16:
        case OP_push_i8:
            in.op = shortest_push_op(in.imm);
            break;
        case OP_push_const:
            if (in.aux <= 255) in.op = OP_push_const8;
            break;
        case OP_fclosure:
            if (in.aux <= 255) in.op = OP_fclosure8;
            break;
        case OP_get_loc:
            if (in.aux <= 3) in.op = static_cast<uint16_t>(OP_get_loc0 + in.aux);
            else if (in.aux <= 255) in.op = OP_get_loc8;
            break;
        case OP_get_loc8:
            if (in.aux <= 3) in.op = static_cast<uint16_t>(OP_get_loc0 + in.aux);
            break;
        case OP_put_loc:
            if (in.aux <= 3) in.op = static_cast<uint16_t>(OP_put_loc0 + in.aux);
            else if (in.aux <= 255) in.op = OP_put_loc8;
            break;
        case OP_put_loc8:
            if (in.aux <= 3) in.op = static_cast<uint16_t>(OP_put_loc0 + in.aux);
            break;
        case OP_set_loc:
            if (in.aux <= 3) in.op = static_cast<uint16_t>(OP_set_loc0 + in.aux);
            else if (in.aux <= 255) in.op = OP_set_loc8;
            break;
        case OP_set_loc8:
            if (in.aux <= 3) in.op = static_cast<uint16_t>(OP_set_loc0 + in.aux);
            break;
        case OP_get_arg:
            if (in.aux <= 3) in.op = static_cast<uint16_t>(OP_get_arg0 + in.aux);
            break;
        case OP_put_arg:
            if (in.aux <= 3) in.op = static_cast<uint16_t>(OP_put_arg0 + in.aux);
            break;
        case OP_set_arg:
            if (in.aux <= 3) in.op = static_cast<uint16_t>(OP_set_arg0 + in.aux);
            break;
        case OP_get_var_ref:
            if (in.aux <= 3) {
                in.op = static_cast<uint16_t>(OP_get_var_ref0 + in.aux);
            }
            break;
        case OP_put_var_ref:
            if (in.aux <= 3) {
                in.op = static_cast<uint16_t>(OP_put_var_ref0 + in.aux);
            }
            break;
        case OP_set_var_ref:
            if (in.aux <= 3) {
                in.op = static_cast<uint16_t>(OP_set_var_ref0 + in.aux);
            }
            break;
        case OP_call:
            if (in.aux <= 3) in.op = static_cast<uint16_t>(OP_call0 + in.aux);
            break;
        default:
            break;
        }
        if (in.op != op) {
            // The emitter's default case copies (old_size - 1) operand
            // bytes verbatim from the old code blob, which yields the
            // low bytes of the original immediate — exactly the value
            // of a shrunk form (range checks above guarantee the value
            // fits), so a size that matches the new opcode is required.
            in.old_size = static_cast<uint16_t>(short_opcode_info(in.op).size);
            stats->shrinks++;
        }
    }
}

// ---------------------------------------------------------------------------
// Pass pipeline: P4 jump threading + P5 dead-block elimination.
// ---------------------------------------------------------------------------

bool is_goto_form(uint8_t op) {
    return op == OP_goto || op == OP_goto8 || op == OP_goto16;
}

// Resolve a jump target through chains of unconditional jumps (goto's
// are stack-neutral, so a jump to a goto behaves identically when
// retargeted to the chain's end). goto-next is included: its jump
// operand IS the following instruction, so following the chain covers
// it. Cycle-safe: on a revisit the chain has no exit, so the original
// target is kept (it stays a valid, live instruction).
int32_t resolve_goto_chain(const std::vector<Insn>& insns, int32_t idx) {
    std::vector<uint8_t> seen(insns.size(), 0);
    while (idx >= 0 && static_cast<size_t>(idx) < insns.size() &&
           is_goto_form(insns[static_cast<size_t>(idx)].op)) {
        if (seen[static_cast<size_t>(idx)]) return idx;  // cycle: no exit
        seen[static_cast<size_t>(idx)] = 1;
        idx = insns[static_cast<size_t>(idx)].target;
    }
    return idx;
}

// P4: retarget every jump (goto, conditional, catch, gosub, with_*)
// through goto chains, then delete unconditional jumps whose target is
// the following instruction (untargeted ones only — retargeted jumpers
// already point at the chain end, and a targeted goto-next survives as
// a harmless no-op... it cannot survive targeted: nothing points at it
// after retargeting, so the exact post-threading target set decides).
// Also folds if_*/goto with target == next into a drop (pop semantics
// preserved). Returns true when anything changed.
bool apply_threading(std::vector<Insn>* insns,
                     std::vector<uint8_t>* dead,
                     RewriteStats* stats) {
    bool changed = false;
    const size_t n = insns->size();
    // Retarget jumps through goto chains.
    for (size_t i = 0; i < n; i++) {
        Insn& in = (*insns)[i];
        if (in.target < 0 || !is_jump_op(in.op)) continue;
        int32_t t = resolve_goto_chain(*insns, in.target);
        if (t != in.target) {
            in.target = t;
            changed = true;
        }
    }
    // Exact target set after retargeting.
    std::vector<uint8_t> targeted(n, 0);
    for (size_t i = 0; i < n; i++) {
        if ((*insns)[i].target >= 0) {
            targeted[static_cast<size_t>((*insns)[i].target)] = 1;
        }
    }
    // if_*/goto with target == next: conditional jumps become a drop
    // (both arms continue at the next instruction), unconditional
    // jumps vanish (fallthrough arrives at the same place). Both are
    // stack-preserving even when targeted.
    for (size_t i = 0; i < n; i++) {
        const Insn& in = (*insns)[i];
        if (in.target == static_cast<int32_t>(i + 1)) {
            if (in.op == OP_if_true || in.op == OP_if_false ||
                in.op == OP_if_true8 || in.op == OP_if_false8) {
                (*insns)[i].op = OP_drop;
                (*insns)[i].old_size = 1;
                (*insns)[i].target = -1;
                stats->threads++;
                changed = true;
            } else if (is_goto_form(in.op)) {
                if (!targeted[i]) {
                    (*dead)[i] = 1;
                    stats->threads++;
                    changed = true;
                }
            }
        }
    }
    return changed;
}

// ---------------------------------------------------------------------------
// P2: cross-BB constant lattice over variable slots (Step 5).
//
// Forward dataflow over basic blocks: each slot (local variable) holds
// one lattice value {kUnknown, kNull, kUndef, kBool, kSmallInt}; put/set
// writes the current stack-top lattice value, get reads it, block
// joins meet the per-slot values. A get_loc whose slot is known is
// replaced by the equivalent push (same stack effect, one dispatch
// fewer).
//
// Soundness: JS call semantics never mutate the caller's local slots
// (arguments are by value; closures can only touch var_ref slots, which
// are not in the lattice), so calls do not reset the lattice. Only
// dynamic-scope operations (with_*, eval, apply_eval) can alias a local
// name, so their presence disables the pass for the whole function.
// ---------------------------------------------------------------------------
enum P2Kind {
    K_UNKNOWN = 0,  // may be any value, incl. objects whose conversions
                    // (ToPrimitive/ToNumber) run arbitrary user code
    K_NULL,
    K_UNDEF,
    K_BOOL,
    K_INT,   // small-int push with a known value (foldable)
    K_NUM,   // push_const/push_i32: provably a number, value not tracked
    K_ATOM,  // push_atom_value: provably a string or symbol, not tracked
};
struct P2Val {
    int kind;
    int64_t imm;
};
static const P2Val kP2Unknown = {K_UNKNOWN, 0};

static P2Val p2_meet(const P2Val& a, const P2Val& b) {
    if (a.kind == K_UNKNOWN || b.kind == K_UNKNOWN) return kP2Unknown;
    if (a.kind == b.kind && a.imm == b.imm) return a;
    return kP2Unknown;
}
static bool p2_set(const P2Val& v, P2Val* out) {
    if (v.kind == out->kind && v.imm == out->imm) return false;
    *out = v;
    return true;
}

static bool is_loc_read(uint8_t op) {
    return op == OP_get_loc || op == OP_get_loc_check ||
           op == OP_get_loc8 ||
           (op >= OP_get_loc0 && op <= OP_get_loc3) ||
           op == OP_get_arg ||
           (op >= OP_get_arg0 && op <= OP_get_arg3);
}
static bool is_loc_write(uint8_t op) {
    return op == OP_put_loc || op == OP_put_loc_check ||
           op == OP_put_loc_check_init || op == OP_put_loc8 ||
           op == OP_set_loc || op == OP_set_loc8 ||
           (op >= OP_put_loc0 && op <= OP_put_loc3) ||
           (op >= OP_set_loc0 && op <= OP_set_loc3) ||
           op == OP_put_arg ||
           (op >= OP_put_arg0 && op <= OP_put_arg3) ||
           op == OP_set_arg ||
           (op >= OP_set_arg0 && op <= OP_set_arg3);
}

// ---- P2 variable barrier model ----
// The slot lattice is sound only if no user code can run between a slot
// write and a later read of that slot: a nested function invoked behind an
// opaque op (e.g. a setInterval callback) can mutate a captured local
// invisibly to this intra-function analysis, and the lattice would fold a
// later read on stale state. Every op that can run arbitrary user code —
// calls, getters/setters/proxies, the iterator protocol, dynamic-scope
// resolution (globals can carry accessors), async suspension, class
// definition (static blocks) and finally blocks entered via gosub — is a
// barrier that drops every slot to unknown. Value-converting ops
// (arithmetic, comparisons, inc/dec) only escape on non-primitive
// operands: unknown values may be objects whose ToPrimitive/ToNumber
// runs user code; provably primitive operands (numbers, bools, null,
// undefined, strings, symbols) convert without it.
static bool p2_op_barrier(uint8_t op, int32_t imm, const P2Val& top,
                          const P2Val& prev, const std::vector<P2Val>& vals,
                          uint32_t var_count) {
    switch (op) {
    // Provably pure: no user code runs, no suspension. (Reads of TDZ
    // checks and brand checks can throw, but exception edges are not
    // modeled at all — every catch block joins with unknown slots — so a
    // throwing pure op never leaks values into a handler.)
    case OP_push_i32: case OP_push_const: case OP_push_const8:
    case OP_push_atom_value: case OP_private_symbol:
    case OP_push_bigint_i32: case OP_fclosure: case OP_fclosure8:
    case OP_push_this: case OP_push_empty_string:
    case OP_object: case OP_special_object: case OP_regexp: case OP_rest:
    case OP_nop: case OP_dup1: case OP_dup2: case OP_dup3:
    case OP_drop: case OP_nip: case OP_nip1:
    case OP_insert2: case OP_insert3: case OP_insert4:
    case OP_perm3: case OP_perm4: case OP_perm5:
    case OP_swap: case OP_swap2: case OP_rot3l: case OP_rot3r:
    case OP_rot4l: case OP_rot5l:
    case OP_get_var_ref: case OP_get_var_ref_check:
    case OP_put_var_ref: case OP_put_var_ref_check:
    case OP_put_var_ref_check_init:
    case OP_set_var_ref:
    case OP_get_var_ref0: case OP_get_var_ref1:
    case OP_get_var_ref2: case OP_get_var_ref3:
    case OP_put_var_ref0: case OP_put_var_ref1:
    case OP_put_var_ref2: case OP_put_var_ref3:
    case OP_set_var_ref0: case OP_set_var_ref1:
    case OP_set_var_ref2: case OP_set_var_ref3:
    case OP_close_loc:
    case OP_make_loc_ref: case OP_make_arg_ref:
    case OP_make_var_ref_ref: case OP_make_var_ref:
    case OP_define_var: case OP_check_define_var: case OP_define_func:
    case OP_get_private_field: case OP_put_private_field:
    case OP_define_private_field: case OP_private_in:
    case OP_check_brand: case OP_add_brand:
    case OP_get_super: case OP_set_home_object:
    case OP_set_name: case OP_set_name_computed:
    case OP_lnot: case OP_typeof: case OP_typeof_is_undefined:
    case OP_typeof_is_function:
    case OP_is_undefined: case OP_is_null: case OP_is_undefined_or_null:
    case OP_strict_eq: case OP_strict_neq:
    case OP_delete_var:
    case OP_nip_catch:
    case OP_goto: case OP_goto8: case OP_goto16:
    case OP_if_true: case OP_if_false: case OP_if_true8:
    case OP_if_false8: case OP_catch: case OP_ret:
    case OP_return: case OP_return_undef: case OP_return_async:
    case OP_throw: case OP_throw_error:
        return false;
    // Value ops: escape only when an operand is not provably primitive.
    // (inc_loc/dec_loc/add_loc operate in place on a local slot, so the
    // slot value is an operand too.)
    case OP_neg: case OP_plus: case OP_inc: case OP_dec:
    case OP_post_inc: case OP_post_dec: case OP_not:
    case OP_mul: case OP_div: case OP_mod: case OP_add: case OP_sub:
    case OP_shl: case OP_sar: case OP_shr: case OP_and: case OP_or:
    case OP_xor: case OP_pow:
    case OP_eq: case OP_neq: case OP_lt: case OP_lte: case OP_gt:
    case OP_gte:
        return top.kind == K_UNKNOWN || prev.kind == K_UNKNOWN;
    case OP_inc_loc: case OP_dec_loc: case OP_add_loc:
        return top.kind == K_UNKNOWN ||
               (imm >= 0 && static_cast<size_t>(imm) < var_count &&
                vals[static_cast<size_t>(imm)].kind == K_UNKNOWN);
    // Everything else can run user code: calls, field access through
    // getters/setters/proxies, dynamic-scope var access, the iterator
    // protocol, async suspension, class static blocks, with/eval, import.
    default:
        return true;
    }
}

bool apply_crossbb(std::vector<Insn>* insns,
                   std::vector<uint8_t>* dead,
                   uint32_t var_count,
                   RewriteStats* stats) {
    const size_t n = insns->size();
    if (n == 0 || var_count == 0) return false;

    // Gate: dynamic scope can re-bind any local name.
    for (size_t i = 0; i < n; i++) {
        uint8_t op = (*insns)[i].op;
        if (is_with_jump(op) || op == OP_eval || op == OP_apply_eval) {
            return false;
        }
    }

    // Leaders: entry, every jump target, post-gosub return points.
    std::vector<uint8_t> is_leader(n, 0);
    is_leader[0] = 1;
    for (size_t i = 0; i < n; i++) {
        const Insn& in = (*insns)[i];
        if (in.target >= 0) {
            is_leader[static_cast<size_t>(in.target)] = 1;
        }
        if (in.op == OP_gosub) {
            is_leader[static_cast<size_t>(
                next_live(*insns, *dead, i + 1))] = 1;
        }
    }
    // Block ids: each leader starts a block; run members share it.
    std::vector<int32_t> block_id(n, -1);
    size_t nb = 0;
    for (size_t i = 0; i < n; i++) {
        if (is_leader[i]) block_id[i] = static_cast<int32_t>(nb++);
    }
    if (nb == 0) return false;
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

    // Per-block entry lattice; monotone meet, so iteration terminates.
    std::vector<P2Val> in_val(nb * var_count, kP2Unknown);
    std::vector<uint8_t> in_wl(nb, 0);
    std::vector<size_t> worklist;
    std::vector<P2Val> repl(n, kP2Unknown);  // pending get_loc replacements
    in_wl[block_id[0]] = 1;
    worklist.push_back(static_cast<size_t>(block_id[0]));

    while (!worklist.empty()) {
        size_t b = worklist.back();
        worklist.pop_back();
        in_wl[b] = 0;
        std::vector<P2Val> vals(in_val.begin() + b * var_count,
                                in_val.begin() + (b + 1) * var_count);
        P2Val top = kP2Unknown;
        P2Val prev = kP2Unknown;  // slot below top: enables [K_INT, K_INT,
                                  // binop] folding inside the dataflow so
                                  // folded constants feed variable slots in
                                  // the same sweep (instead of one chain
                                  // step per fixpoint round).
        size_t last = bstart[b];
        for (size_t i = bstart[b]; i < bend[b]; i++) {
            if ((*dead)[i]) continue;
            last = i;
            const Insn& in = (*insns)[i];
            uint8_t op = in.op;
            if (is_loc_read(op)) {
                int32_t s = static_cast<int32_t>(in.imm);
                P2Val v = (s >= 0 && static_cast<size_t>(s) < var_count)
                              ? vals[static_cast<size_t>(s)]
                              : kP2Unknown;
                repl[i] = v;
                prev = kP2Unknown;
                top = v;
            } else if (is_loc_write(op)) {
                int32_t s = static_cast<int32_t>(in.imm);
                if (s >= 0 && static_cast<size_t>(s) < var_count) {
                    vals[static_cast<size_t>(s)] = top;
                }
                prev = kP2Unknown;
                top = kP2Unknown;
            } else if (op == OP_get_loc0_loc1) {
                // get_loc(0) get_loc(1) fused: pushes loc0 then loc1.
                prev = vals[0];
                top = vals[1];
            } else if (op == OP_inc_loc || op == OP_dec_loc ||
                       op == OP_add_loc) {
                // In-place slot arithmetic: result is never a tracked
                // constant, but a provably-primitive operand keeps the
                // other slots sound.
                int32_t s = static_cast<int32_t>(in.imm);
                if (s >= 0 && static_cast<size_t>(s) < var_count) {
                    vals[static_cast<size_t>(s)] = kP2Unknown;
                }
                prev = kP2Unknown;
                top = kP2Unknown;
            } else if (is_small_int_push(op)) {
                prev = top;
                top = {K_INT, in.imm};
            } else if (op == OP_push_i32 || op == OP_push_const ||
                       op == OP_push_const8) {
                // Provably a number; the exact value is not tracked
                // (floats and out-of-i32-range ints), so no folding —
                // but arithmetic on it stays pure.
                prev = top;
                top = {K_NUM, 0};
            } else if (op == OP_push_atom_value || op == OP_private_symbol) {
                // Provably a string or symbol: no ToPrimitive user code
                // in later conversions, so not a barrier.
                prev = top;
                top = {K_ATOM, 0};
            } else if (op == OP_push_true || op == OP_push_false) {
                prev = top;
                top = {K_BOOL, op == OP_push_true ? 1 : 0};
            } else if (op == OP_undefined) {
                prev = top;
                top = {K_UNDEF, 0};
            } else if (op == OP_null) {
                prev = top;
                top = {K_NULL, 0};
            } else if (op == OP_dup) {
                // top unchanged; the duplicate now sits below top.
                prev = top;
            } else if (is_foldable_binop_op(op) && top.kind == K_INT &&
                       prev.kind == K_INT) {
                // Dataflow-side constant folding: mirrors P3.1 exactly
                // (same foldable_binop), so the lattice never claims a
                // value the peephole will not actually emit.
                int64_t r;
                if (foldable_binop(op, prev.imm, top.imm, &r)) {
                    top = {K_INT, r};
                } else {
                    top = kP2Unknown;
                }
                prev = kP2Unknown;
            } else if (p2_op_barrier(op, static_cast<int32_t>(in.imm), top,
                                     prev, vals, var_count)) {
                // Opaque op: user code may run between the last slot
                // write and a later read (a captured local mutated by a
                // nested function is invisible here), so every slot
                // falls back to unknown — folding a get_loc on stale
                // state would change behavior.
                std::fill(vals.begin(), vals.end(), kP2Unknown);
                prev = kP2Unknown;
                top = kP2Unknown;
            } else {
                // Provably pure op with an unmodeled stack effect: the
                // slot lattice survives, the stack lattice does not.
                prev = kP2Unknown;
                top = kP2Unknown;
            }
        }
        // Propagate to the block's successors (last live insn's edges).
        const Insn& l = (*insns)[last];
        int32_t succs[2];
        int nsucc = 0;
        switch (l.op) {
        case OP_tail_call: case OP_tail_call_method:
        case OP_return: case OP_return_undef: case OP_return_async:
        case OP_throw: case OP_throw_error: case OP_ret:
            break;
        case OP_goto: case OP_goto8: case OP_goto16:
            succs[nsucc++] = l.target;
            break;
        case OP_if_true: case OP_if_false:
        case OP_if_true8: case OP_if_false8:
        case OP_catch: case OP_gosub:
        case OP_with_get_var: case OP_with_put_var:
        case OP_with_delete_var: case OP_with_make_ref:
        case OP_with_get_ref: case OP_with_get_ref_undef:
            succs[nsucc++] = l.target;
            succs[nsucc++] = static_cast<int32_t>(
                next_live(*insns, *dead, last + 1));
            break;
        default:
            succs[nsucc++] = static_cast<int32_t>(
                next_live(*insns, *dead, last + 1));
            break;
        }
        for (int si = 0; si < nsucc; si++) {
            int32_t s = succs[si];
            if (s < 0 || static_cast<size_t>(s) >= n) continue;
            int32_t sb = block_id[static_cast<size_t>(s)];
            if (sb < 0) continue;
            bool changed = false;
            for (uint32_t v = 0; v < var_count; v++) {
                P2Val& dst = in_val[static_cast<size_t>(sb) * var_count + v];
                if (p2_set(p2_meet(dst, vals[v]), &dst)) changed = true;
            }
            if (changed && !in_wl[static_cast<size_t>(sb)]) {
                in_wl[static_cast<size_t>(sb)] = 1;
                worklist.push_back(static_cast<size_t>(sb));
            }
        }
    }

    // Apply replacements: get_loc with a known slot -> the push.
    bool changed = false;
    for (size_t i = 0; i < n; i++) {
        const P2Val& v = repl[i];
        if (v.kind == K_UNKNOWN) continue;
        Insn& in = (*insns)[i];
        switch (v.kind) {
        case K_NULL: in.op = OP_null; break;
        case K_UNDEF: in.op = OP_undefined; break;
        case K_BOOL: in.op = v.imm ? OP_push_true : OP_push_false; break;
        case K_INT: in.op = shortest_push_op(v.imm); in.imm = v.imm; break;
        default: continue;
        }
        in.old_size = short_opcode_info(in.op).size;
        in.target = -1;
        stats->folds_p2++;
        changed = true;
    }
    return changed;
}

// P5: delete unreachable instructions. Reachability runs over the same
// control-flow edges the verifier enforces, seeded from the entry and
// conservatively from every catch target and post-gosub instruction
// (exception handlers and OP_ret return points are static roots even
// when their try/gosub is unreachable). Because a reachable fallthrough
// seeds its successor, unreachable runs always sit between terminators
// and jump targets — whole blocks — and no surviving jump references
// them (a reachable jump seeds its target). Cycles of pure gotos are
// handled by resolve_goto_chain keeping their targets live.
bool eliminate_dead(std::vector<Insn>* insns,
                    std::vector<uint8_t>* dead,
                    RewriteStats* stats) {
    const size_t n = insns->size();
    std::vector<uint8_t> reach(n, 0);
    std::vector<size_t> worklist;
    auto seed = [&](int32_t idx) {
        if (idx >= 0 && static_cast<size_t>(idx) < n && !reach[idx]) {
            reach[idx] = 1;
            worklist.push_back(static_cast<size_t>(idx));
        }
    };
    seed(0);
    for (size_t i = 0; i < n; i++) {
        if ((*dead)[i]) continue;
        const Insn& in = (*insns)[i];
        if (in.op == OP_catch) seed(in.target);
        if (in.op == OP_gosub) {
            seed(in.target);
            seed(static_cast<int32_t>(
                next_live(*insns, *dead, i + 1)));
        }
    }
    while (!worklist.empty()) {
        size_t idx = worklist.back();
        worklist.pop_back();
        // Tombstoned instructions are pure-stack ops with no control
        // flow (only P3.4/P3.5 mark instructions dead; a folded
        // conditional jump is replaced by goto, never tombstoned). A
        // dead instruction's logical fallthrough still executes, so
        // reachability must not die at a tombstone — otherwise a fold
        // at the very start of a function (e.g. P3.1 on the entry
        // sequence) would make every following instruction look
        // unreachable and eliminate the whole function body.
        if ((*dead)[idx]) {
            seed(static_cast<int32_t>(next_live(*insns, *dead, idx + 1)));
            continue;
        }
        const Insn& in = (*insns)[idx];
        // Fallthrough edges land on the next live instruction: a folded
        //-away successor must not stop reachability from propagating.
        int32_t nxt = static_cast<int32_t>(next_live(*insns, *dead, idx + 1));
        switch (in.op) {
        case OP_tail_call: case OP_tail_call_method:
        case OP_return: case OP_return_undef: case OP_return_async:
        case OP_throw: case OP_throw_error: case OP_ret:
            break;  // terminators
        case OP_goto: case OP_goto8: case OP_goto16:
            seed(static_cast<int32_t>(next_live(*insns, *dead, in.target)));
            break;
        case OP_if_true: case OP_if_false:
        case OP_if_true8: case OP_if_false8:
        case OP_catch: case OP_gosub:
        case OP_with_get_var: case OP_with_put_var:
        case OP_with_delete_var: case OP_with_make_ref:
        case OP_with_get_ref: case OP_with_get_ref_undef:
            seed(static_cast<int32_t>(next_live(*insns, *dead, in.target)));
            seed(nxt);
            break;
        default:
            seed(nxt);
            break;
        }
    }
    bool changed = false;
    for (size_t i = 0; i < n; i++) {
        if (!reach[i]) {
            (*dead)[i] = 1;
            changed = true;
        }
    }
    if (changed) stats->dead_blocks++;
    return changed;
}

// Drop tombstoned instructions and remap jump targets (jump targets are
// never deleted, so every target index stays valid after remapping).
void compact_insns(std::vector<Insn>* insns, const std::vector<uint8_t>& dead) {
    std::vector<uint32_t> remap(insns->size(), 0);
    std::vector<Insn> out;
    out.reserve(insns->size());
    for (size_t i = 0; i < insns->size(); i++) {
        if (!dead[i]) {
            remap[i] = static_cast<uint32_t>(out.size());
            out.push_back((*insns)[i]);
        }
    }
    for (size_t i = 0; i < out.size(); i++) {
        if (out[i].target >= 0) {
            int32_t t = out[i].target;
            // A fold may have removed the target instruction itself:
            // landing inside the folded-away run is equivalent to
            // landing at the first live instruction after it (the
            // run's whole stack effect was folded away).
            while (t >= 0 && static_cast<size_t>(t) < dead.size() &&
                   dead[static_cast<size_t>(t)]) {
                t++;
            }
            if (t < 0 || static_cast<size_t>(t) >= insns->size()) {
                // Folded to the very end of the function: unreachable
                // jump; point it at the last live instruction (the
                // terminators can never be folded, so this is a
                // defensive fallback).
                t = static_cast<int32_t>(out.size()) - 1;
            }
            out[i].target = static_cast<int32_t>(remap[t]);
        }
    }
    insns->swap(out);
}

// ---------------------------------------------------------------------------
// Pass pipeline: emission with a jump-distance fixpoint.
// ---------------------------------------------------------------------------

// Minimal encoded size of a jump opcode; distances are evaluated at
// emission time. Catch/gosub have no short forms; the with_* family
// is a fixed 10-byte form (atom u32 + label u32 + u8 flag).
static int jump_form(uint8_t op) {
    switch (op) {
    case OP_if_false8:
    case OP_if_true8:
    case OP_goto8:
        return 2;
    case OP_goto16:
        return 3;
    case OP_with_get_var:
    case OP_with_put_var:
    case OP_with_delete_var:
    case OP_with_make_ref:
    case OP_with_get_ref:
    case OP_with_get_ref_undef:
        return 10;
    default:
        return 5;  // if_false, if_true, goto, catch, gosub
    }
}

// Choose the shortest jump form whose distance fits, mirroring
// resolve_labels' shrink rules (quickjs.c:35259-35303): conditional
// jumps shrink to 8-bit only; goto shrinks to 8-bit, then 16-bit.
// Returns true when the opcode changed. (Jump opcodes are < 256; the
// 16-bit width is only because Insn::op is uint16_t.)
bool fit_jump_form(uint16_t* op, int64_t dist) {
    if (*op == OP_if_false || *op == OP_if_true) {
        if (dist >= -128 && dist <= 127) {
            *op = (*op == OP_if_false) ? OP_if_false8 : OP_if_true8;
            return true;
        }
        return false;
    }
    if (*op == OP_goto) {
        if (dist >= -128 && dist <= 127) {
            *op = OP_goto8;
            return true;
        }
        if (dist >= -32768 && dist <= 32767) {
            *op = OP_goto16;
            return true;
        }
        return false;
    }
    if (*op == OP_goto8) {
        if (dist >= -128 && dist <= 127) return false;
        if (dist >= -32768 && dist <= 32767) {
            *op = OP_goto16;
            return true;
        }
        *op = OP_goto;
        return true;
    }
    if (*op == OP_goto16) {
        if (dist >= -128 && dist <= 127) {
            *op = OP_goto8;
            return true;
        }
        if (dist >= -32768 && dist <= 32767) return false;
        *op = OP_goto;
        return true;
    }
    if (*op == OP_if_false8 || *op == OP_if_true8) {
        if (dist >= -128 && dist <= 127) return false;
        *op = (*op == OP_if_false8) ? OP_if_false : OP_if_true;
        return true;
    }
    return false;  // catch/gosub: fixed size
}

// Emit the optimized code blob: opcodes plus re-encoded operands
// (jump offsets are self-relative from the operand start). Fills
// new_offs[i] = final byte offset of each instruction (used by the
// pc2line remap).
bool emit_code(const std::vector<Insn>& insns,
               const uint8_t* old_code,
               std::vector<uint8_t>* out,
               std::vector<uint32_t>* new_offs,
               std::string* error) {
    std::vector<Insn> work = insns;
    // Jump-size fixpoint: distances depend on offsets, which depend on
    // sizes. Iterate to a stable assignment; a pass that changes
    // nothing is a fixed point (converges in practice in 1-2 rounds).
    bool size_changed = true;
    for (int round = 0; size_changed && round < 32; round++) {
        size_changed = false;
        new_offs->resize(work.size());
        uint32_t off = 0;
        for (size_t i = 0; i < work.size(); i++) {
            (*new_offs)[i] = off;
            off += is_jump_op(work[i].op) || is_with_jump(work[i].op)
                       ? static_cast<uint32_t>(jump_form(work[i].op))
                       : short_opcode_info(work[i].op).size;
        }
        for (size_t i = 0; i < work.size(); i++) {
            Insn& in = work[i];
            if (in.target < 0) continue;
            int64_t operand_start =
                static_cast<int64_t>((*new_offs)[i]) + 1;
            if (is_with_jump(in.op)) operand_start += 4;
            int64_t dist = static_cast<int64_t>((*new_offs)[in.target]) -
                           operand_start;
            if (fit_jump_form(&in.op, dist)) size_changed = true;
        }
    }
    if (size_changed) {
        *error = "bytecode optimize: jump sizing did not converge";
        return false;
    }
    // Final offsets.
    new_offs->resize(work.size());
    uint32_t off = 0;
    for (size_t i = 0; i < work.size(); i++) {
        (*new_offs)[i] = off;
        off += is_jump_op(work[i].op) || is_with_jump(work[i].op)
                   ? static_cast<uint32_t>(jump_form(work[i].op))
                   : short_opcode_info(work[i].op).size;
    }
    out->reserve(out->size() + off);
    for (size_t i = 0; i < work.size(); i++) {
        const Insn& in = work[i];
        out->push_back(static_cast<uint8_t>(in.op));
        switch (in.op) {
        case OP_push_minus1: case OP_push_0: case OP_push_1:
        case OP_push_2: case OP_push_3: case OP_push_4:
        case OP_push_5: case OP_push_6: case OP_push_7:
            break;
        case OP_push_i8:
            out->push_back(static_cast<uint8_t>(static_cast<int8_t>(in.imm)));
            break;
        case OP_push_i16:
            out->push_back(static_cast<uint8_t>(static_cast<uint16_t>(in.imm)));
            out->push_back(static_cast<uint8_t>(
                static_cast<uint16_t>(in.imm) >> 8));
            break;
        case OP_push_i32: {
            uint32_t v = static_cast<uint32_t>(static_cast<int32_t>(in.imm));
            out->push_back(static_cast<uint8_t>(v));
            out->push_back(static_cast<uint8_t>(v >> 8));
            out->push_back(static_cast<uint8_t>(v >> 16));
            out->push_back(static_cast<uint8_t>(v >> 24));
            break;
        }
        case OP_push_const:
        case OP_fclosure:
            out->push_back(static_cast<uint8_t>(in.aux));
            out->push_back(static_cast<uint8_t>(in.aux >> 8));
            out->push_back(static_cast<uint8_t>(in.aux >> 16));
            out->push_back(static_cast<uint8_t>(in.aux >> 24));
            break;
        case OP_push_const8:
        case OP_fclosure8:
            out->push_back(static_cast<uint8_t>(in.aux));
            break;
        case OP_get_loc8: case OP_put_loc8: case OP_set_loc8:
            out->push_back(static_cast<uint8_t>(in.aux));
            break;
        case OP_get_loc: case OP_put_loc: case OP_set_loc:
        case OP_get_arg: case OP_put_arg: case OP_set_arg:
        case OP_get_var_ref: case OP_put_var_ref: case OP_set_var_ref:
        case OP_set_loc_uninitialized: case OP_get_loc_check:
        case OP_put_loc_check: case OP_put_loc_check_init:
        case OP_get_var_ref_check: case OP_put_var_ref_check:
        case OP_put_var_ref_check_init: case OP_close_loc:
        case OP_using_dispose:
            out->push_back(static_cast<uint8_t>(in.aux));
            out->push_back(static_cast<uint8_t>(in.aux >> 8));
            break;
        case OP_call: case OP_call_method: case OP_tail_call:
        case OP_tail_call_method: case OP_call_constructor:
        case OP_array_from: case OP_apply: case OP_apply_eval:
            out->push_back(static_cast<uint8_t>(in.aux));
            out->push_back(static_cast<uint8_t>(in.aux >> 8));
            break;
        case OP_eval:
            // eval carries two u16 operands: call_argc at +1 and a
            // scope index at +3 (interpreter CASE(OP_eval)). aux only
            // holds argc; neither operand is ever rewritten (eval gates
            // P2), so copy both verbatim.
            out->insert(out->end(), old_code + in.old_off + 1,
                        old_code + in.old_off + 5);
            break;
        case OP_if_false: case OP_if_true: case OP_goto:
        case OP_catch: case OP_gosub: {
            int64_t dist = static_cast<int64_t>((*new_offs)[in.target]) -
                           (static_cast<int64_t>((*new_offs)[i]) + 1);
            uint32_t v = static_cast<uint32_t>(static_cast<int32_t>(dist));
            out->push_back(static_cast<uint8_t>(v));
            out->push_back(static_cast<uint8_t>(v >> 8));
            out->push_back(static_cast<uint8_t>(v >> 16));
            out->push_back(static_cast<uint8_t>(v >> 24));
            break;
        }
        case OP_if_false8: case OP_if_true8: case OP_goto8: {
            int64_t dist = static_cast<int64_t>((*new_offs)[in.target]) -
                           (static_cast<int64_t>((*new_offs)[i]) + 1);
            out->push_back(static_cast<uint8_t>(static_cast<int8_t>(dist)));
            break;
        }
        case OP_goto16: {
            int64_t dist = static_cast<int64_t>((*new_offs)[in.target]) -
                           (static_cast<int64_t>((*new_offs)[i]) + 1);
            uint16_t v = static_cast<uint16_t>(static_cast<int16_t>(dist));
            out->push_back(static_cast<uint8_t>(v));
            out->push_back(static_cast<uint8_t>(v >> 8));
            break;
        }
        case OP_with_get_var: case OP_with_put_var:
        case OP_with_delete_var: case OP_with_make_ref:
        case OP_with_get_ref: case OP_with_get_ref_undef: {
            // atom (u32) + label offset (self-relative from +5) + u8.
            const uint8_t* src = old_code + in.old_off + 1;
            out->insert(out->end(), src, src + 4);
            int64_t dist = static_cast<int64_t>((*new_offs)[in.target]) -
                           (static_cast<int64_t>((*new_offs)[i]) + 5);
            uint32_t v = static_cast<uint32_t>(static_cast<int32_t>(dist));
            out->push_back(static_cast<uint8_t>(v));
            out->push_back(static_cast<uint8_t>(v >> 8));
            out->push_back(static_cast<uint8_t>(v >> 16));
            out->push_back(static_cast<uint8_t>(v >> 24));
            out->push_back(old_code[in.old_off + 9]);
            break;
        }
        default: {
            // Operands are copied verbatim; any opcode whose size
            // changed must have been handled above.
            if (short_opcode_info(in.op).size != in.old_size) {
                *error = "bytecode optimize: internal error, operand width "
                         "changed without a re-encoder";
                return false;
            }
            const uint8_t* src = old_code + in.old_off + 1;
            out->insert(out->end(), src, src + (in.old_size - 1));
            break;
        }
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Verifier: re-implementation of compute_stack_size (quickjs.c:35926-
// 36174) over the instruction list: breadth-first height propagation,
// underflow/consistency checks, and max-height <= recorded stack_size.
// ---------------------------------------------------------------------------

bool verify_code(const uint8_t* code,
                 size_t len,
                 uint32_t recorded_stack_size,
                 std::string* error) {
    std::vector<Insn> insns;
    if (!decode_code(code, len, &insns, error)) return false;
    if (insns.empty()) {
        *error = "bytecode optimize: empty code blob";
        return false;
    }
    std::vector<int32_t> heights(insns.size(), -1);
    std::vector<size_t> worklist;
    auto seed = [&](size_t idx, int32_t h) -> bool {
        if (idx >= insns.size()) {
            *error = "bytecode optimize: control flow falls off the code "
                     "blob";
            return false;
        }
        if (heights[idx] != -1) {
            if (heights[idx] != h) {
                *error = "bytecode optimize: inconsistent stack height at "
                         "instruction " + std::to_string(idx);
                return false;
            }
            return true;
        }
        heights[idx] = h;
        worklist.push_back(idx);
        return true;
    };
    if (!seed(0, 0)) return false;
    int32_t max_h = 0;
    while (!worklist.empty()) {
        size_t idx = worklist.back();
        worklist.pop_back();
        const Insn& in = insns[idx];
        int32_t h = heights[idx];
        const OpInfo& oi = short_opcode_info(in.op);
        int32_t n_pop = oi.n_pop;
        if (oi.fmt == OP_FMT_npop || oi.fmt == OP_FMT_npop_u16) {
            n_pop += static_cast<int32_t>(in.aux);
        } else if (oi.fmt == OP_FMT_npopx) {
            n_pop += static_cast<int32_t>(in.op) - OP_call0;
        }
        if (h < n_pop) {
            *error = "bytecode optimize: stack underflow at instruction " +
                     std::to_string(idx);
            return false;
        }
        int32_t post = h - n_pop + oi.n_push;
        if (post > max_h) max_h = post;
        switch (in.op) {
        case OP_tail_call: case OP_tail_call_method:
        case OP_return: case OP_return_undef: case OP_return_async:
        case OP_throw: case OP_throw_error: case OP_ret:
            break;  // terminators: no fallthrough
        case OP_goto: case OP_goto8: case OP_goto16:
            if (!seed(static_cast<size_t>(in.target), post)) return false;
            break;
        case OP_if_true: case OP_if_false:
        case OP_if_true8: case OP_if_false8:
            if (!seed(static_cast<size_t>(in.target), post)) return false;
            if (!seed(idx + 1, post)) return false;
            break;
        case OP_catch:
            // Both the handler edge (exception pushed) and the normal
            // path carry the post-catch height (catch pushes the
            // handler offset value).
            if (!seed(static_cast<size_t>(in.target), post)) return false;
            if (!seed(idx + 1, post)) return false;
            break;
        case OP_gosub:
            // The finally entry sees the return-address slot (+1); the
            // return point is seeded via the fallthrough edge (ret
            // pops the slot, so the heights line up).
            if (!seed(static_cast<size_t>(in.target), post + 1)) return false;
            if (!seed(idx + 1, post)) return false;
            break;
        case OP_with_get_var:
        case OP_with_delete_var:
            if (!seed(static_cast<size_t>(in.target), post + 1)) return false;
            if (!seed(idx + 1, post)) return false;
            break;
        case OP_with_make_ref:
        case OP_with_get_ref:
        case OP_with_get_ref_undef:
            if (!seed(static_cast<size_t>(in.target), post + 2)) return false;
            if (!seed(idx + 1, post)) return false;
            break;
        case OP_with_put_var:
            if (!seed(static_cast<size_t>(in.target), post - 1)) return false;
            if (!seed(idx + 1, post)) return false;
            break;
        default:
            if (!seed(idx + 1, post)) return false;
            break;
        }
    }
    if (static_cast<uint32_t>(max_h) > recorded_stack_size) {
        *error = "bytecode optimize: max stack height exceeds the recorded "
                 "stack size";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// P7: pc2line remap. Decode the old table to absolute (pc, line, col)
// triples (base = the function's declared line/col), drop entries in
// deleted regions, re-encode with compute_pc2line_info's exact rules.
// ---------------------------------------------------------------------------

struct Pc2LineEntry {
    uint32_t pc;
    int32_t line;
    int32_t col;
};

// One uleb128/sleb128 inside the pc2line table (bounds-checked, max 5
// bytes).
bool pc2line_read_leb(const uint8_t* p, size_t len, size_t* i, uint32_t* out) {
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

bool decode_pc2line(const uint8_t* p,
                    size_t len,
                    int32_t base_line,
                    int32_t base_col,
                    std::vector<Pc2LineEntry>* out) {
    uint32_t pc = 0;
    int32_t line = base_line;
    int32_t col = base_col;
    size_t i = 0;
    while (i < len) {
        uint8_t b = p[i++];
        uint32_t pc_delta;
        int32_t line_delta;
        if (b == 0) {
            // Long form: uleb128(pc_delta) + sleb128(line_delta).
            uint32_t v;
            if (!pc2line_read_leb(p, len, &i, &v)) return false;
            pc_delta = v;
            uint32_t sv;
            if (!pc2line_read_leb(p, len, &i, &sv)) return false;
            line_delta = static_cast<int32_t>(
                (sv >> 1) ^ static_cast<uint32_t>(-(sv & 1)));
        } else {
            uint32_t op = b - PC2LINE_OP_FIRST;
            pc_delta = op / PC2LINE_RANGE;
            line_delta = static_cast<int32_t>(op % PC2LINE_RANGE) +
                         PC2LINE_BASE;
        }
        uint32_t cv;
        if (!pc2line_read_leb(p, len, &i, &cv)) return false;
        int32_t col_delta = static_cast<int32_t>(
            (cv >> 1) ^ static_cast<uint32_t>(-(cv & 1)));
        pc += pc_delta;
        line += line_delta;
        col += col_delta;
        Pc2LineEntry e;
        e.pc = pc;
        e.line = line;
        e.col = col;
        out->push_back(e);
    }
    return true;
}

void encode_pc2line(const std::vector<Pc2LineEntry>& entries,
                    int32_t base_line,
                    int32_t base_col,
                    std::vector<uint8_t>* out) {
    int32_t last_line = base_line;
    int32_t last_col = base_col;
    uint32_t last_pc = 0;
    for (size_t i = 0; i < entries.size(); i++) {
        const Pc2LineEntry& e = entries[i];
        if (e.line < 0) continue;
        int64_t diff_pc = static_cast<int64_t>(e.pc) - last_pc;
        int64_t diff_line = static_cast<int64_t>(e.line) - last_line;
        int64_t diff_col = static_cast<int64_t>(e.col) - last_col;
        if (diff_pc < 0) continue;
        if (diff_line == 0 && diff_col == 0) continue;
        if (diff_line >= PC2LINE_BASE &&
            diff_line < PC2LINE_BASE + PC2LINE_RANGE &&
            diff_pc <= PC2LINE_DIFF_PC_MAX) {
            out->push_back(static_cast<uint8_t>(
                (diff_line - PC2LINE_BASE) + diff_pc * PC2LINE_RANGE +
                PC2LINE_OP_FIRST));
        } else {
            out->push_back(0);
            put_leb128(out, static_cast<uint32_t>(diff_pc));
            put_sleb128(out, static_cast<int32_t>(diff_line));
        }
        put_sleb128(out, static_cast<int32_t>(diff_col));
        last_pc = e.pc;
        last_line = e.line;
        last_col = e.col;
    }
}

// ---------------------------------------------------------------------------
// Per-function rewrite: decode -> peepholes -> shrink -> emit -> verify
// -> pc2line remap.
// ---------------------------------------------------------------------------

bool rewrite_function(const FuncRecord& f,
                      const uint8_t* data,
                      std::vector<uint8_t>* new_code,
                      std::vector<uint8_t>* new_pc2line,
                      bool* changed,
                      uint32_t passes,
                      RewriteStats* stats,
                      std::string* error) {
    const uint8_t* code = data + f.code_off;
    *changed = false;
    // The original must verify: fail closed on anything quickjs's own
    // stack checker would reject (format drift or a parse bug).
    if (!verify_code(code, f.code_len, f.stack_size, error)) return false;

    std::vector<Insn> insns;
    if (!decode_code(code, f.code_len, &insns, error)) return false;

    // pc2line entry pcs must be instruction boundaries in the original
    // code; anything else means format drift. Decode once up front and
    // reuse after emission.
    std::vector<Pc2LineEntry> pc2line_entries;
    if (f.dbg_pc2line_len) {
        if (!decode_pc2line(data + f.dbg_pc2line_off, f.dbg_pc2line_len,
                            f.dbg_line, f.dbg_col, &pc2line_entries)) {
            *error = "bytecode optimize: malformed pc2line table";
            return false;
        }
        std::vector<uint32_t> offs;
        offs.reserve(insns.size());
        for (size_t i = 0; i < insns.size(); i++) {
            offs.push_back(insns[i].old_off);
        }
        for (size_t i = 0; i < pc2line_entries.size(); i++) {
            uint32_t want = pc2line_entries[i].pc;
            std::vector<uint32_t>::const_iterator it =
                std::lower_bound(offs.begin(), offs.end(), want);
            if (it == offs.end() || *it != want) {
                *error = "bytecode optimize: pc2line entry not on an "
                         "instruction boundary";
                return false;
            }
        }
    }

    stats->insns_before += insns.size();
    stats->bytes_before += f.code_len;

    // P3/P4/P5/P6 fixpoint: peephole sweeps until stable (each round
    // that changes anything deletes at least one instruction, so this
    // terminates), then shrink. The round-start target set is a
    // superset of every target after P4 threading (threaded targets are
    // chain ends that were already jump operands), so it stays a sound
    // fold guard for the whole round.
    for (int round = 0; round < 16; round++) {
        std::vector<uint8_t> dead(insns.size(), 0);
        bool round_changed = false;
        if ((passes & kPassP4) &&
            apply_threading(&insns, &dead, stats)) {
            round_changed = true;
        }
        if ((passes & kPassP2) &&
            apply_crossbb(&insns, &dead, f.var_count, stats)) {
            round_changed = true;
        }
        if ((passes & (kPassP31 | kPassP32 | kPassP34 | kPassP35 |
                       kPassP36)) &&
            apply_peepholes(&insns, &dead, passes, stats)) {
            round_changed = true;
        }
        if ((passes & kPassP5) && eliminate_dead(&insns, &dead, stats)) {
            round_changed = true;
        }
        if (!round_changed) break;
        compact_insns(&insns, dead);
        apply_reshrink(&insns, stats);
        if (round == 15) {
            *error = "bytecode optimize: optimization did not converge";
            return false;
        }
    }
    apply_reshrink(&insns, stats);

    // Emit.
    std::vector<uint32_t> new_offs;
    if (!emit_code(insns, code, new_code, &new_offs, error)) return false;

    stats->insns_after += insns.size();
    stats->bytes_after += new_code->size();

    // Verify the rewritten code against the same rules.
    if (!verify_code(new_code->data(), new_code->size(), f.stack_size,
                     error)) {
        return false;
    }

    if (new_code->size() == f.code_len &&
        std::memcmp(new_code->data(), code, f.code_len) == 0) {
        *changed = false;  // byte-identical: no rewrite happened
    } else {
        *changed = true;
    }

    // P7: pc2line remap. Entries whose pc belongs to a deleted
    // instruction are dropped; fold replacements inherit the last
    // replaced instruction's pc via pc_off. Both pc_offs (across
    // surviving instructions) and entry pcs are strictly increasing,
    // so a single merge walk suffices.
    new_pc2line->clear();
    if (f.dbg_pc2line_len) {
        std::vector<Pc2LineEntry> remapped;
        size_t k = 0;
        for (size_t i = 0; i < pc2line_entries.size(); i++) {
            uint32_t want = pc2line_entries[i].pc;
            while (k < insns.size() && insns[k].pc_off < want) k++;
            if (k < insns.size() && insns[k].pc_off == want) {
                Pc2LineEntry e;
                e.pc = new_offs[k];
                e.line = pc2line_entries[i].line;
                e.col = pc2line_entries[i].col;
                remapped.push_back(e);
            }
        }
        encode_pc2line(remapped, f.dbg_line, f.dbg_col, new_pc2line);
        if (new_pc2line->size() != f.dbg_pc2line_len ||
            std::memcmp(new_pc2line->data(), data + f.dbg_pc2line_off,
                        f.dbg_pc2line_len) != 0) {
            *changed = true;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Buffer rebuild: verbatim segment copy with three patches per function
// (byte_code_len leb128, code blob, pc2line_len + blob) plus the header
// checksum. Only the module's top-level function record is rewritten
// here; the buffer is exactly [header..atoms..module tables] + one
// function record.
// ---------------------------------------------------------------------------

struct FuncRewrite {
    std::vector<uint8_t> code;
    std::vector<uint8_t> pc2line;
    std::vector<FuncRewrite> children;  // same shape as FuncRecord.children
};

bool rewrite_tree(const FuncRecord& f,
                  const uint8_t* data,
                  FuncRewrite* rw,
                  bool* any_changed,
                  uint32_t passes,
                  RewriteStats* stats,
                  std::string* error) {
    rw->children.resize(f.children.size());
    for (size_t i = 0; i < f.children.size(); i++) {
        if (!rewrite_tree(f.children[i], data, &rw->children[i],
                          any_changed, passes, stats, error)) {
            return false;
        }
    }
    bool changed = false;
    if (!rewrite_function(f, data, &rw->code, &rw->pc2line, &changed, passes,
                          stats, error)) {
        return false;
    }
    if (changed) *any_changed = true;
    return true;
}

void emit_record(const FuncRecord& f,
                 const FuncRewrite& rw,
                 const uint8_t* data,
                 std::vector<uint8_t>* out) {
    uint32_t first_child = f.children.empty() ? f.code_off
                                              : f.child_spans[0];
    out->insert(out->end(), data + f.fn_start, data + f.byte_code_len_off);
    put_leb128(out, static_cast<uint32_t>(rw.code.size()));
    out->insert(out->end(), data + f.byte_code_len_end, data + first_child);
    for (size_t c = 0; c < f.children.size(); c++) {
        emit_record(f.children[c], rw.children[c], data, out);
        uint32_t next = (c + 1 < f.children.size())
                            ? f.child_spans[2 * c + 2]
                            : f.code_off;
        out->insert(out->end(), data + f.child_spans[2 * c + 1], data + next);
    }
    out->insert(out->end(), rw.code.begin(), rw.code.end());
    if (f.dbg_off) {
        out->insert(out->end(), data + f.dbg_off,
                    data + f.dbg_pc2line_len_off);
        put_leb128(out, static_cast<uint32_t>(rw.pc2line.size()));
        out->insert(out->end(), rw.pc2line.begin(), rw.pc2line.end());
        out->insert(out->end(), data + f.dbg_pc2line_off + f.dbg_pc2line_len,
                    data + f.dbg_end);
    }
}

bool verify_tree(const FuncRecord& f, const uint8_t* data, std::string* error) {
    if (!verify_code(data + f.code_off, f.code_len, f.stack_size, error)) {
        return false;
    }
    for (size_t i = 0; i < f.children.size(); i++) {
        if (!verify_tree(f.children[i], data, error)) return false;
    }
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// Public API.
// ---------------------------------------------------------------------------

bool analyze_only(const std::vector<std::uint8_t>& in, std::string* error) {
    error->clear();
    std::vector<FuncRecord> functions;
    if (!parse_buffer(in.data(), in.size(), &functions, error)) return false;
    FoldStats st;
    for (size_t i = 0; i < functions.size(); i++) {
        if (!scan_function(functions[i], in.data(), &st)) {
            *error = "bytecode optimize: invalid opcode in function";
            return false;
        }
    }
    double fold_pct = st.insns ? 100.0 * st.foldable_insns / st.insns : 0.0;
    double bytes_pct =
        st.bytes ? 100.0 * (st.foldable_bytes + st.shrinkable_bytes) / st.bytes
                 : 0.0;
    std::fprintf(stderr,
                 "bytecode analyze: %llu functions, %llu insns (%llu bytes), "
                 "foldable %llu insns (%.2f%%), removable+shrinkable "
                 "%llu bytes (%.2f%%)\n",
                 static_cast<unsigned long long>(functions.size()),
                 static_cast<unsigned long long>(st.insns),
                 static_cast<unsigned long long>(st.bytes),
                 static_cast<unsigned long long>(st.foldable_insns), fold_pct,
                 static_cast<unsigned long long>(st.foldable_bytes +
                                                st.shrinkable_bytes),
                 bytes_pct);
    return true;
}

bool optimize(const std::vector<std::uint8_t>& in,
              std::vector<std::uint8_t>* out,
              uint32_t passes,
              bool report,
              std::string* error) {
    error->clear();
    std::vector<FuncRecord> functions;
    if (!parse_buffer(in.data(), in.size(), &functions, error)) return false;
    if (functions.size() != 1) {
        *error = "bytecode optimize: module does not contain exactly one "
                 "function record";
        return false;
    }
    const FuncRecord& root = functions[0];
    FuncRewrite rw;
    bool any_changed = false;
    RewriteStats stats;
    if (!rewrite_tree(root, in.data(), &rw, &any_changed, passes, &stats,
                      error)) {
        return false;
    }
    if (!any_changed) {
        // No rewrite applied: the output is byte-identical to the input
        // (determinism contract for the frozen CLI).
        out->assign(in.begin(), in.end());
        return true;
    }
    out->clear();
    out->reserve(in.size());
    out->insert(out->end(), in.begin(), in.begin() + root.fn_start);
    emit_record(root, rw, in.data(), out);
    // Patch the header checksum (bytes 1..4, little-endian).
    uint32_t csum = bc_csum(out->data() + 5, out->size() - 5);
    (*out)[1] = static_cast<uint8_t>(csum);
    (*out)[2] = static_cast<uint8_t>(csum >> 8);
    (*out)[3] = static_cast<uint8_t>(csum >> 16);
    (*out)[4] = static_cast<uint8_t>(csum >> 24);
    // Final self-check: full reparse (validates version, checksum, atom
    // table, all records) plus per-function stack verification.
    std::vector<FuncRecord> check;
    if (!parse_buffer(out->data(), out->size(), &check, error)) return false;
    if (check.size() != 1 || !verify_tree(check[0], out->data(), error)) {
        if (error->empty()) {
            *error = "bytecode optimize: internal verification failed";
        }
        return false;
    }
    if (report) {
        std::fprintf(stderr,
                     "bytecode optimize: %llu -> %llu insns, %llu -> %llu "
                     "code bytes; folds P2 %llu P3.1 %llu P3.2 %llu P3.4 "
                     "%llu P3.5 %llu P3.6 %llu, threads %llu, dead %llu, "
                     "shrinks %llu\n",
                     static_cast<unsigned long long>(stats.insns_before),
                     static_cast<unsigned long long>(stats.insns_after),
                     static_cast<unsigned long long>(stats.bytes_before),
                     static_cast<unsigned long long>(stats.bytes_after),
                     static_cast<unsigned long long>(stats.folds_p2),
                     static_cast<unsigned long long>(stats.folds_p31),
                     static_cast<unsigned long long>(stats.folds_p32),
                     static_cast<unsigned long long>(stats.folds_p34),
                     static_cast<unsigned long long>(stats.folds_p35),
                     static_cast<unsigned long long>(stats.folds_p36),
                     static_cast<unsigned long long>(stats.threads),
                     static_cast<unsigned long long>(stats.dead_blocks),
                     static_cast<unsigned long long>(stats.shrinks));
    }
    return true;
}

}  // namespace bytecode
}  // namespace capsid
