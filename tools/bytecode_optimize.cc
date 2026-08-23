// Bytecode AOT optimizer implementation (design: docs/bytecode-aot-optimizer.md).
//
// State: G4-trimmed v1 pipeline over the serialized quickjs-ng bytecode
// format (BC_VERSION 26):
//   parse  -> per-function: decode -> P2 cross-BB constant lattice ->
//   P3.1 const binop peephole -> P6 re-shrink (short forms by
//   value/index/argc) -> emit (jump-distance fixpoint) -> verifier
//   (compute_stack_size re-implementation) -> P7 pc2line remap
//   -> buffer rebuild (leb128 patches + bc_csum recompute) -> full
//   reparse self-check.
// G4 (2026-08-23, 20-bundle corpus) attributed P3.2/P3.4/P3.5/P3.6/
// P4/P5 below the 1% gate and trimmed them — quickjs-ng's resolve_labels
// already removes push+drop, dup/swap/rot3, and same-block constant
// conditions; see PassFlags in bytecode_optimize.h.
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
    // Per-slot captured mask (vardef position = slot index; is_captured
    // bit 0x40 in the vardef flags). A captured slot can be written by
    // its capturing closure at any runtime point (via var_ref into the
    // live frame), invisible to this function's own instruction stream,
    // so every slot-based pass must treat it as opaque.
    std::vector<uint8_t> captured;
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
        std::vector<uint8_t> captured_slots;
        captured_slots.reserve(vardef_count);
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
                captured_slots.push_back(1);
            } else {
                captured_slots.push_back(0);
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
        rec.captured = std::move(captured_slots);
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
// exactly the deployed pipeline patterns (P2's rewrites are not
// visible in the static scan — they need the dataflow — so this is
// the P3.1 + P6 part of the ceiling; see G5 in
// docs/bytecode-aot-optimizer.md); the estimate is deliberately
// conservative.
//   P3.1: push small-int + push small-int + binop (i32, no overflow)
//         -> single push; removes the two pushes and the binop, emits
//         the shortest push form.
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
        case OP_using_dispose: case OP_using_dispose_async:
            in.aux = static_cast<uint16_t>(code[pc + 1]) |
                     (static_cast<uint16_t>(code[pc + 2]) << 8);
            in.has_aux = true;
            break;
        case OP_get_loc8: case OP_put_loc8: case OP_set_loc8:
        case OP_inc_loc: case OP_dec_loc: case OP_add_loc:
            // inc_loc/dec_loc/add_loc carry their destination slot as a
            // 1-byte loc8 operand (emitted by the += / ++ peephole); the
            // in-place mutation reads and writes that slot, so the slot
            // index must be decoded into aux or every slot-tracking pass
            // would see "slot 0".
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
        // Atom-family operands: raw u32 JSAtom value at pc+1 (also the
        // atom_u8/u16 and with_* label variants). P14 compares
        // define_field vs get_field atoms by value; no name resolution
        // is needed (both operands use the same raw encoding).
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
    uint64_t folds_p31 = 0;   // const binop folds
    uint64_t folds_p2 = 0;    // P2: cross-BB constant propagation replaces
    uint64_t folds_p11 = 0;   // P11: dead stores removed via copy propagation
    uint64_t folds_p14 = 0;   // P14: literal get_field folds
    uint64_t folds_sccp = 0;  // P10: SSI SCCP folds (const reads, branches)
    uint64_t folds_p11s = 0;  // P11': SSA copy-prop renames
    uint64_t folds_dce = 0;   // P12': SSA dead-store pairs + dead blocks
    uint64_t folds_licm = 0;  // P13': loop-invariant hoists
    uint64_t folds_p14s = 0;  // P14': form-(b) literal folds
    uint64_t folds_gvn = 0;   // P15: slot-read CSE (dup) folds
    uint64_t shrinks = 0;     // short-form re-encodings
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
        i++;
    }
    return changed;
}

// ---------------------------------------------------------------------------
// Tier-2 direct-level passes (P11 copy propagation, P14 literal folds).
// Linear sweeps over the decoded stream, run before the v1 fixpoint.
// All rewrites are stack-neutral (deleted pairs or equal-stack-effect
// replacements) so compact_insns' jump-target redirect stays sound.
// ---------------------------------------------------------------------------

// Loc-slot access helpers. get_loc0_loc1 reads two slots;
// apply_copyprop counts it as a reader of both 0 and 1, and P2 reads
// both lattice cells. NOTE: the compiler emits check-form and
// short-form loc ops for captured slots too (a captured lexical var is
// still read by its defining function via get_loc_check), so passes
// must consult the FuncRecord captured mask (from the vardef
// is_captured flag) — never assume a loc op implies an uncaptured
// slot.
bool is_get_loc_op(uint8_t op) {
    return op == OP_get_loc || op == OP_get_loc8 || op == OP_get_loc_check ||
           (op >= OP_get_loc0 && op <= OP_get_loc3);
}
bool is_put_loc_op(uint8_t op) {
    return op == OP_put_loc || op == OP_put_loc8 || op == OP_put_loc_check ||
           op == OP_put_loc_check_init ||
           (op >= OP_put_loc0 && op <= OP_put_loc3);
}
bool is_set_loc_op(uint8_t op) {
    return op == OP_set_loc || op == OP_set_loc8 ||
           (op >= OP_set_loc0 && op <= OP_set_loc3);
}
// In-place slot mutations (read + write the slot).
bool is_slot_mut_op(uint8_t op) {
    return op == OP_inc_loc || op == OP_dec_loc || op == OP_add_loc ||
           op == OP_set_loc_uninitialized || op == OP_close_loc;
}

// Ops after which no slot copy/literal description may survive: they
// can run arbitrary user code that could touch a captured frame, or
// create closures (frame shapes are compiler-fixed, but a conservative
// barrier is free here).
bool is_slot_alias_barrier(uint8_t op) {
    switch (op) {
    case OP_eval:
    case OP_with_get_var: case OP_with_put_var:
    case OP_with_delete_var: case OP_with_make_ref:
    case OP_with_get_ref: case OP_with_get_ref_undef:
    case OP_fclosure: case OP_fclosure8:
    case OP_using_dispose: case OP_using_dispose_async:
        return true;
    default:
        return false;
    }
}

// Cross-frame and suspension ops (P2's opacity model). await resumes
// arbitrary microtasks; get_var/put_var touch the global object where
// accessors can run; the var_ref family moves values into outer frames
// where other closures can observe or mutate them (or reads values
// that outer code wrote there). A literal description bound to a local
// slot must not survive any of these: the described object could have
// been mutated by the code they run or reach.
static bool is_frame_opaque_op(uint8_t op) {
    switch (op) {
    case OP_await: case OP_import: case OP_for_await_of_start:
    case OP_get_var_undef: case OP_get_var: case OP_put_var:
    case OP_put_var_init: case OP_define_var: case OP_check_define_var:
    case OP_get_var_ref: case OP_put_var_ref: case OP_set_var_ref:
    case OP_get_var_ref_check: case OP_put_var_ref_check:
    case OP_put_var_ref_check_init:
    case OP_get_var_ref0: case OP_get_var_ref1: case OP_get_var_ref2:
    case OP_get_var_ref3:
    case OP_put_var_ref0: case OP_put_var_ref1: case OP_put_var_ref2:
    case OP_put_var_ref3:
    case OP_set_var_ref0: case OP_set_var_ref1: case OP_set_var_ref2:
    case OP_set_var_ref3:
    case OP_make_var_ref: case OP_make_var_ref_ref:
        return true;
    default:
        return false;
    }
}

// Build the set of instruction indices that are jump targets (any jump
// form, including catch/gosub, which land on stack effect boundaries).
std::vector<uint8_t> compute_targets(const std::vector<Insn>& insns) {
    std::vector<uint8_t> t(insns.size(), 0);
    for (size_t i = 0; i < insns.size(); i++) {
        if (insns[i].target >= 0) {
            size_t tt = static_cast<size_t>(insns[i].target);
            if (tt < t.size()) t[tt] = 1;
        }
    }
    return t;
}

// Resolve a loc-family op to its slot index (raw aux value or the
// short-form implicit slot 0..3).
bool slot_of(const Insn& in, uint32_t* slot) {
    switch (in.op) {
    case OP_get_loc: case OP_get_loc8: case OP_get_loc_check:
    case OP_put_loc: case OP_put_loc8: case OP_put_loc_check:
    case OP_put_loc_check_init:
    case OP_set_loc: case OP_set_loc8:
    case OP_set_loc_uninitialized:
    case OP_inc_loc: case OP_dec_loc: case OP_add_loc:
    case OP_close_loc:
        *slot = static_cast<uint32_t>(in.aux);
        return true;
    case OP_get_loc0: case OP_get_loc1: case OP_get_loc2:
    case OP_get_loc3:
        *slot = static_cast<uint32_t>(in.op - OP_get_loc0);
        return true;
    case OP_put_loc0: case OP_put_loc1: case OP_put_loc2:
    case OP_put_loc3:
        *slot = static_cast<uint32_t>(in.op - OP_put_loc0);
        return true;
    case OP_set_loc0: case OP_set_loc1: case OP_set_loc2:
    case OP_set_loc3:
        *slot = static_cast<uint32_t>(in.op - OP_set_loc0);
        return true;
    default:
        return false;
    }
}

// P11: copy propagation + dead store materialization.
// One linear pass tracks alias pairs (get_loc s; put_loc t => t copies
// s), renames later reads of t to s, and records candidate dead stores.
// A store may be deleted only together with its feeding get_loc (pair
// is stack-neutral), only when nothing after it in the linear stream
// reads t, only when no read of t precedes it (a loop-back edge could
// otherwise carry the pre-store value around), and only when neither
// instruction is a jump target. Captured slots never participate: a
// closure write (invisible to this stream) can change the slot between
// the copy and a later read, or observe a deleted store.
bool apply_copyprop(std::vector<Insn>* insns, std::vector<uint8_t>* dead,
                    uint32_t var_count,
                    const std::vector<uint8_t>& captured,
                    RewriteStats* stats) {
    const size_t n = insns->size();
    std::vector<uint8_t> targets = compute_targets(*insns);
    // alias[t] = s means slot t currently holds a copy of slot s.
    std::vector<int32_t> alias(var_count, -1);
    // Candidate dead stores: put_idx -> (feeding get_idx, slot, reads).
    struct Cand {
        size_t put_idx;
        size_t get_idx;
    };
    std::vector<Cand> cands;
    std::vector<uint32_t> cand_slot;
    std::vector<size_t> reads_after;
    // read_attr[put_idx] = candidate index, or n when the put is not a
    // candidate; last_put_idx[t] = index of the most recent put_loc t.
    std::vector<size_t> last_put_idx(var_count, n);
    std::vector<size_t> read_attr(n, n);
    // first_get[t] = position of the first (un-aliased) read of t, or
    // n when t is never read. Precomputed on the original stream so a
    // read before a candidate store disqualifies it (loop-back edges).
    // get_loc0_loc1 is a fused read of slots 0 and 1 and counts for
    // both (a plain get_loc read of either is folded into it by the
    // compiler's peephole, so excluding it would undercount readers).
    std::vector<size_t> first_get(var_count, n);
    for (size_t i = 0; i < n; i++) {
        if ((*dead)[i]) continue;
        const Insn& in = (*insns)[i];
        uint32_t sl;
        if (in.op == OP_get_loc0_loc1) {
            if (first_get[0] == n) first_get[0] = i;
            if (first_get[1] == n) first_get[1] = i;
        } else if (is_get_loc_op(in.op) && slot_of(in, &sl) &&
                   first_get[sl] == n) {
            first_get[sl] = i;
        }
    }
    auto clear_alias_to = [&alias](uint32_t s) {
        for (size_t j = 0; j < alias.size(); j++)
            if (alias[j] == static_cast<int32_t>(s)) alias[j] = -1;
    };
    auto clear_all_alias = [&alias]() {
        for (size_t j = 0; j < alias.size(); j++) alias[j] = -1;
    };

    for (size_t i = 0; i < n; i++) {
        if ((*dead)[i]) continue;
        const Insn& in = (*insns)[i];
        if (is_slot_alias_barrier(in.op)) {
            // Barrier ops (eval/with/fclosure) can never write loc
            // slots — the compiler emits loc ops only for slots it
            // proved are not captured — so aliases stay valid and,
            // crucially, later reads must still attribute to stores
            // before the barrier (last_put_idx must NOT be reset).
            clear_all_alias();
            continue;
        }
        uint32_t sl;
        if (is_put_loc_op(in.op) && slot_of(in, &sl)) {
            // Pair candidate: previous live insn is a get_loc reading a
            // different slot, and no read of t precedes this store.
            size_t prev = i;
            while (prev > 0) {
                prev--;
                if (!(*dead)[prev]) break;
            }
            if (prev < i && is_get_loc_op((*insns)[prev].op)) {
                uint32_t s;
                if (slot_of((*insns)[prev], &s) && s != sl &&
                    !captured[s] && !captured[sl] && alias[sl] < 0 &&
                    first_get[sl] >= i) {
                    size_t c = cands.size();
                    cands.push_back({i, prev});
                    cand_slot.push_back(sl);
                    reads_after.push_back(0);
                    read_attr[i] = c;
                    alias[sl] = static_cast<int32_t>(s);
                    last_put_idx[sl] = i;
                    continue;
                }
            }
            // Store of a different value or disqualified: break alias.
            clear_alias_to(sl);
            alias[sl] = -1;
            last_put_idx[sl] = n;
            continue;
        }
        if (is_set_loc_op(in.op) || is_slot_mut_op(in.op)) {
            if (slot_of(in, &sl)) {
                clear_alias_to(sl);
                alias[sl] = -1;
                last_put_idx[sl] = n;
            }
            continue;
        }
        if (in.op == OP_get_loc0_loc1) {
            // Fused read of slots 0 and 1: a real reader of both; it
            // keeps the most recent stores of 0 and 1 alive (never
            // renamed — the fused form cannot be rewritten to a
            // different slot pair).
            for (uint32_t k = 0; k <= 1; k++) {
                size_t lp = last_put_idx[k];
                if (lp < n && read_attr[lp] < cands.size())
                    reads_after[read_attr[lp]]++;
            }
            continue;
        }
        if (is_get_loc_op(in.op) && slot_of(in, &sl)) {
            if (alias[sl] >= 0) {
                // Rename this read of t to its alias root s. The read
                // no longer touches t, so it does not keep the store
                // alive (reads_after stays 0).
                uint32_t s = static_cast<uint32_t>(alias[sl]);
                (*insns)[i].aux = s;
                (*insns)[i].has_aux = true;
            } else {
                // Un-aliased read of t: the most recent store of t has
                // a real reader and must stay.
                size_t lp = last_put_idx[sl];
                if (lp < n && read_attr[lp] < cands.size())
                    reads_after[read_attr[lp]]++;
            }
            continue;
        }
    }
    bool changed = false;
    for (size_t c = 0; c < cands.size(); c++) {
        if (reads_after[c] != 0) continue;
        const Cand& cd = cands[c];
        // Neither the store nor its feeding load may be a jump target
        // (a jump landing on the load expects the pushed value).
        if (targets[cd.put_idx] || targets[cd.get_idx]) continue;
        (*dead)[cd.put_idx] = 1;
        (*dead)[cd.get_idx] = 1;
        stats->folds_p11++;
        changed = true;
    }
    return changed;
}

// P14: literal get_field/get_array_el fold. Tracks OP_object /
// OP_array_from construction sequences in a linear sweep: construction;
// constant pushes; define_field / array_from; put_loc t binds a field /
// element description to slot t. A later get_loc t + get_field x (or
// get_loc t + push k + get_array_el) folds to a push of the recorded
// value when x / k is a known own data property or element.
//
// Soundness:
//  - The constructed value holds only construction writes (a plain
//    object literal has no accessors); any mutation of a slot-read
//    value (put_field / put_array_el / define_array_el / append) clears
//    every description, since the construction is no longer the only
//    writer. define_field with an unknown (non-constant) value makes
//    the object under construction unfoldable.
//  - Any call may observe or mutate a described value (argument,
//    receiver, or closure), so the whole call family is a barrier
//    (OP_fclosure already is, in is_slot_alias_barrier).
//  - Unknown fields (prototype chain, e.g. toString) are never folded.
//  - The under-construction tracker (cur) is cleared by any
//    instruction that is not a construction step: the stack value may
//    be consumed (get_field on a literal, a call) or aliased, and a
//    stale description must never be bound to a later put_loc.
//  - Values recorded from push_const/push_const8 are re-emitted as the
//    identical instruction (same constant-pool index; the table is
//    never modified), so value identity and aliasing are preserved.
//  - Captured slots never hold descriptions: the capturing closure can
//    write the slot (or mutate the object it holds) at any runtime
//    point, so a bound description would be stale.
// Stack effect of each folded pair equals the replacement push.
bool apply_lit_fold(std::vector<Insn>* insns, std::vector<uint8_t>* dead,
                    uint32_t var_count,
                    const std::vector<uint8_t>& captured,
                    RewriteStats* stats) {
    const size_t n = insns->size();
    // A recorded value: a small integer, an existing constant-pool
    // entry (the push_const/push_const8 source), or an atom value
    // (the push_atom_value source, e.g. a string literal).
    struct Val {
        bool is_cpool;
        bool is_atom;
        int64_t v;        // small-int value when !is_cpool && !is_atom
        uint32_t cidx;    // cpool index when is_cpool
        uint32_t atom;    // atom when is_atom
    };
    // Description of a value under construction / bound to a slot.
    // is_array == false: object (fields); true: array (elements).
    struct Desc {
        bool valid;
        bool is_array;
        std::vector<std::pair<uint32_t, Val>> fields;
        std::vector<Val> elements;
    };
    std::vector<Desc> slot_desc(var_count);
    Desc cur;  // value currently being built on the stack
    cur.valid = false;

    auto clear_cur = [&]() {
        cur.valid = false;
        cur.fields.clear();
        cur.elements.clear();
    };
    auto clear_all = [&]() {
        for (size_t j = 0; j < slot_desc.size(); j++) slot_desc[j].valid = false;
        clear_cur();
    };
    auto find_field = [](Desc* d, uint32_t atom) -> Val* {
        for (size_t j = 0; j < d->fields.size(); j++)
            if (d->fields[j].first == atom) return &d->fields[j].second;
        return nullptr;
    };
    auto is_call = [](uint8_t op) {
        switch (op) {
        case OP_call: case OP_call1: case OP_call2: case OP_call3:
        case OP_tail_call:
        case OP_call_method: case OP_tail_call_method:
        case OP_call_constructor:
        case OP_apply: case OP_apply_eval:
        case OP_iterator_call:
            return true;
        default:
            return false;
        }
    };
    // Index of the previous live instruction at or below `at`, or -1.
    auto prev_live = [&](size_t at) -> int64_t {
        while (at > 0) {
            at--;
            if (!(*dead)[at]) return static_cast<int64_t>(at);
        }
        return -1;
    };
    // Read a constant push (small int, cpool, or atom value) into
    // `val`; false for anything else.
    auto read_const_push = [](const Insn& in, Val* val) -> bool {
        val->is_atom = false;
        if (is_small_int_push(in.op)) {
            val->is_cpool = false;
            val->v = in.imm;
            return true;
        }
        if (in.op == OP_push_const || in.op == OP_push_const8) {
            val->is_cpool = true;
            val->cidx = in.aux;
            return true;
        }
        if (in.op == OP_push_atom_value) {
            val->is_cpool = false;
            val->is_atom = true;
            val->atom = in.aux;
            return true;
        }
        return false;
    };
    // Replace the instruction at `at` with a push of `val`; `pc_off` is
    // the original offset of the fold site (used for pc2line). The
    // constant pool is never modified (red line): cpool and atom
    // values re-emit the identical original instruction.
    auto emit_val_push = [&](size_t at, const Val& val, uint32_t pc_off) {
        Insn ni;
        if (val.is_atom) {
            ni.op = OP_push_atom_value;
            ni.aux = val.atom;
            ni.has_aux = true;
            ni.imm = 0;
        } else if (val.is_cpool) {
            ni.op = OP_push_const;  // reshrunk to push_const8 when small
            ni.aux = val.cidx;
            ni.has_aux = true;
            ni.imm = 0;
        } else {
            ni.op = shortest_push_op(val.v);
            ni.imm = val.v;
            ni.has_aux = false;
        }
        ni.old_off = (*insns)[at].old_off;
        ni.old_size = static_cast<uint16_t>(short_opcode_info(ni.op).size);
        ni.pc_off = pc_off;
        ni.target = -1;
        (*insns)[at] = ni;
    };

    bool changed = false;
    for (size_t i = 0; i < n; i++) {
        if ((*dead)[i]) continue;
        const Insn& in = (*insns)[i];
        if (is_slot_alias_barrier(in.op) || is_call(in.op) ||
            is_frame_opaque_op(in.op)) {
            clear_all();
            continue;
        }
        if (in.op == OP_object) {
            cur.fields.clear();
            cur.elements.clear();
            cur.is_array = false;
            cur.valid = true;
            continue;
        }
        if (in.op == OP_array_from) {
            // Elements are the `aux` most recent live constant pushes
            // (pushed in order, so walked back in reverse).
            uint32_t count = in.aux;
            std::vector<Val> elems;
            elems.reserve(count);
            int64_t p = static_cast<int64_t>(i);
            bool ok = true;
            for (uint32_t k = 0; k < count; k++) {
                p = prev_live(static_cast<size_t>(p));
                if (p < 0) { ok = false; break; }
                Val val;
                if (!read_const_push((*insns)[static_cast<size_t>(p)], &val)) {
                    ok = false;
                    break;
                }
                elems.push_back(val);
            }
            if (!ok || elems.size() != count) {
                clear_cur();
                continue;
            }
            std::reverse(elems.begin(), elems.end());
            cur.elements = std::move(elems);
            cur.fields.clear();
            cur.is_array = true;
            cur.valid = true;
            continue;
        }
        if (in.op == OP_define_field) {
            if (!cur.valid) {
                // Mutating an object read out of a slot (or any other
                // object): no description can be trusted anymore.
                clear_all();
                continue;
            }
            int64_t prev = prev_live(i);
            if (prev >= 0) {
                Val val;
                if (read_const_push((*insns)[static_cast<size_t>(prev)], &val)) {
                    Val* f = find_field(&cur, in.aux);
                    if (f) {
                        *f = val;
                    } else {
                        cur.fields.push_back({in.aux, val});
                    }
                    continue;
                }
            }
            // Non-constant field value: the object's shape is still
            // defined by the writes, but the value is unknown; fold
            // nothing for this object.
            cur.valid = false;
            continue;
        }
        if (in.op == OP_put_field || in.op == OP_put_array_el ||
            in.op == OP_define_array_el || in.op == OP_append) {
            // Mutation of a described value (slot-read or under
            // construction): the description reflects the construction
            // writes, not this writer.
            clear_all();
            continue;
        }
        if (in.op == OP_set_home_object || in.op == OP_set_name ||
            in.op == OP_set_name_computed) {
            // Class/name machinery can change the object.
            clear_all();
            continue;
        }
        uint32_t sl;
        if (is_put_loc_op(in.op) && slot_of(in, &sl)) {
            if (cur.valid && !captured[sl]) {
                slot_desc[sl] = cur;
            }
            clear_cur();
            continue;
        }
        if (is_set_loc_op(in.op) || is_slot_mut_op(in.op)) {
            if (slot_of(in, &sl)) slot_desc[sl].valid = false;
            continue;
        }
        if (in.op == OP_get_field) {
            int64_t prev = prev_live(i);
            if (prev >= 0 && is_get_loc_op((*insns)[static_cast<size_t>(prev)].op)) {
                uint32_t s;
                if (slot_of((*insns)[static_cast<size_t>(prev)], &s) &&
                    !captured[s] && slot_desc[s].valid &&
                    !slot_desc[s].is_array) {
                    Val* f = find_field(&slot_desc[s], in.aux);
                    if (f) {
                        emit_val_push(static_cast<size_t>(prev), *f,
                                      in.old_off);
                        (*dead)[i] = 1;
                        stats->folds_p14++;
                        changed = true;
                        continue;
                    }
                }
            }
            // Unknown property (prototype chain could provide it) or
            // value not from a construction: no fold; the description
            // of the read slot stays valid (own fields remain
            // foldable). The stack object is consumed.
            clear_cur();
            continue;
        }
        if (in.op == OP_get_array_el) {
            int64_t prev = prev_live(i);
            if (prev >= 0 && is_small_int_push((*insns)[static_cast<size_t>(prev)].op)) {
                int64_t k = (*insns)[static_cast<size_t>(prev)].imm;
                int64_t prev2 = prev_live(static_cast<size_t>(prev));
                if (prev2 >= 0 && is_get_loc_op((*insns)[static_cast<size_t>(prev2)].op)) {
                    uint32_t s;
                    if (slot_of((*insns)[static_cast<size_t>(prev2)], &s) &&
                        !captured[s] && slot_desc[s].valid &&
                        slot_desc[s].is_array &&
                        k >= 0 &&
                        static_cast<uint64_t>(k) <
                            slot_desc[s].elements.size()) {
                        emit_val_push(static_cast<size_t>(prev2),
                                      slot_desc[s].elements[k], in.old_off);
                        (*dead)[static_cast<size_t>(prev)] = 1;
                        (*dead)[i] = 1;
                        stats->folds_p14++;
                        changed = true;
                        continue;
                    }
                }
            }
            clear_cur();
            continue;
        }
        if (is_small_int_push(in.op) || in.op == OP_push_const ||
            in.op == OP_push_const8 || in.op == OP_push_atom_value) {
            // Construction value.
            continue;
        }
        // Any other instruction: the stack value under construction is
        // consumed or aliased; the description dies with it. Slot
        // descriptions bound by put_loc survive reads (mutations and
        // calls were handled above).
        clear_cur();
    }
    return changed;
}

// ---------------------------------------------------------------------------
// Tier-2 SSI suite (P9 build, P10 SCCP, P11' copy propagation, P12' SSA
// DCE, P13' LICM, P14' form-(b) literal folds, P15 slot-read CSE).
//
// Model: the emitted bytecode is a pure stack machine; SSI names the
// per-block slot versions (a slot's version is the write instruction
// that defined it, or a block-entry join) and the stack values that
// cross block joins (a value is a push instruction, or a per-position
// join). Naming never changes the stack layout, so every rewrite is
// stack-neutral (deleted pairs or equal-stack-effect replacements) and
// compact_insns' jump-target redirect stays sound.
//
// The suite rebuilds its structure per stage from the current
// (possibly tombstoned) stream, so folds from earlier stages feed later
// ones. It declines (returns false with no rewrite) on anything outside
// its model: dynamic scope (with/eval, P2's barrier precedent), a
// function with no loc slots, or a join whose predecessors disagree on
// exit stack height (the catch/gosub exceptional edges) — the plan's
// "phi consistency is verified at build time" gate.
//
// Soundness of the fold rules (see docs/bytecode-aot-optimizer.md):
//  - Slot versions are exact only for slots the vardefs mark as NOT
//    captured. A captured slot is still accessed by its defining
//    function via loc ops (get_loc_check etc. — the compiler does not
//    switch the defining function to var_ref ops), while the capturing
//    closure writes the same frame slot at any runtime point via its
//    var_ref; those writes are invisible to this stream. The suite
//    therefore treats every captured slot as opaque: its versions are
//    BOTTOM in SCCP, and P11'/P12'/P13'/P14'/P15 all skip captured
//    slots (the mask comes from the vardef is_captured flag).
//    For uncaptured slots the versions are exact: no call can touch
//    them. The only unknown write is set_loc_uninitialized, which
//    records the TDZ marker; a version whose lattice cell is known can
//    therefore never be the marker, and folding a check-form read on
//    such a version cannot remove an observable TDZ throw.
//  - object literals are constructed fresh per execution (the compiler
//    emits object + define_field, never a shared cpool template), so a
//    literal description is invalidated only by path-local mutation
//    (the P14 kill list) or closure creation, tracked via the cone
//    rule for cross-block folds: every block on some path from the
//    definition to the fold site must be kill-free. A backedge puts
//    the whole loop body in the cone, so a loop-internal mutation
//    (even after the fold site) kills the fold — correct, because the
//    same object is re-read on the next iteration.
// ---------------------------------------------------------------------------

// SCCP lattice: TOP < constants < BOTTOM (overdefined).
enum SsiLatKind : uint8_t {
    SSI_L_TOP = 0,
    SSI_L_BOTTOM = 1,
    SSI_L_INT = 2,
    SSI_L_BOOL = 3,
    SSI_L_UNDEF = 4,
    SSI_L_NULL = 5,
    SSI_L_ATOM = 6,
    SSI_L_CPOOL = 7,
    SSI_L_OBJECT = 8,
};
struct SsiLat {
    uint8_t kind;
    int32_t v;
};
static SsiLat ssi_lat_meet(const SsiLat& a, const SsiLat& b) {
    if (a.kind == SSI_L_TOP) return b;
    if (b.kind == SSI_L_TOP) return a;
    if (a.kind == SSI_L_BOTTOM || b.kind == SSI_L_BOTTOM)
        return SsiLat{SSI_L_BOTTOM, 0};
    if (a.kind == b.kind && a.v == b.v) return a;
    return SsiLat{SSI_L_BOTTOM, 0};
}

// Literal value / description, mirroring P14's Val/Desc (the cpool
// reader and the fold emitter never touch the constant pool).
struct SVal {
    bool is_cpool = false;
    bool is_atom = false;
    int64_t v = 0;
    uint32_t cidx = 0;
    uint32_t atom = 0;
};
struct SDesc {
    bool valid = false;
    bool is_array = false;
    std::vector<std::pair<uint32_t, SVal>> fields;
    std::vector<SVal> elements;
};
static bool ssi_read_const_push(const Insn& in, SVal* val) {
    val->is_atom = false;
    if (is_small_int_push(in.op)) {
        val->is_cpool = false;
        val->v = in.imm;
        return true;
    }
    if (in.op == OP_push_const || in.op == OP_push_const8) {
        val->is_cpool = true;
        val->cidx = in.aux;
        return true;
    }
    if (in.op == OP_push_atom_value) {
        val->is_cpool = false;
        val->is_atom = true;
        val->atom = in.aux;
        return true;
    }
    return false;
}
static bool ssi_same_val(const SVal& a, const SVal& b) {
    return a.is_cpool == b.is_cpool && a.is_atom == b.is_atom &&
           a.v == b.v && a.cidx == b.cidx && a.atom == b.atom;
}
static bool ssi_same_desc(const SDesc& a, const SDesc& b) {
    if (a.valid != b.valid || a.is_array != b.is_array) return false;
    if (a.fields.size() != b.fields.size() ||
        a.elements.size() != b.elements.size()) return false;
    for (size_t i = 0; i < a.fields.size(); i++) {
        if (a.fields[i].first != b.fields[i].first ||
            !ssi_same_val(a.fields[i].second, b.fields[i].second))
            return false;
    }
    for (size_t i = 0; i < a.elements.size(); i++) {
        if (!ssi_same_val(a.elements[i], b.elements[i])) return false;
    }
    return true;
}

// Pure single-slot pushes used as removable pair producers.
static bool is_pure_const_push(uint8_t op) {
    return is_small_int_push(op) || op == OP_push_const ||
           op == OP_push_const8 || op == OP_push_atom_value ||
           op == OP_push_true || op == OP_push_false || op == OP_undefined ||
           op == OP_null;
}
static bool is_check_loc_op(uint8_t op) {
    return op == OP_get_loc_check || op == OP_put_loc_check ||
           op == OP_put_loc_check_init;
}
// Pure single-push instructions whose value a branch fold may delete
// together with the branch (the pair is stack-neutral). Check-form
// reads are included: the branch folds only on a known condition cell,
// which is then not the TDZ marker, so the read could not throw.
static bool is_ssi_pure_push(uint8_t op) {
    return is_pure_const_push(op) || is_get_loc_op(op);
}

// Terminators and their control-flow kind. The with_* family is gated
// out before the build; gosub's +1 target height lands on a join whose
// preds disagree and declines the suite.
enum SsiTerm : uint8_t {
    SSI_T_NONE,
    SSI_T_GOTO,
    SSI_T_COND,
    SSI_T_RETURN,
    SSI_T_CATCH,
    SSI_T_GOSUB,
};
static SsiTerm ssi_terminator(uint8_t op) {
    switch (op) {
    case OP_goto: case OP_goto8: case OP_goto16:
        return SSI_T_GOTO;
    case OP_if_true: case OP_if_false: case OP_if_true8: case OP_if_false8:
        return SSI_T_COND;
    case OP_catch:
        return SSI_T_CATCH;
    case OP_gosub:
        return SSI_T_GOSUB;
    case OP_return: case OP_return_undef: case OP_return_async:
    case OP_throw: case OP_throw_error: case OP_ret:
    case OP_tail_call: case OP_tail_call_method:
        return SSI_T_RETURN;
    default:
        return SSI_T_NONE;
    }
}

// Pop count of an instruction, mirroring verify_code's model.
static int32_t ssi_pop_count(const Insn& in) {
    const OpInfo& oi = short_opcode_info(in.op);
    int32_t n_pop = oi.n_pop;
    if (oi.fmt == OP_FMT_npop || oi.fmt == OP_FMT_npop_u16) {
        n_pop += static_cast<int32_t>(in.aux);
    } else if (oi.fmt == OP_FMT_npopx) {
        n_pop += static_cast<int32_t>(in.op) - OP_call0;
    }
    return n_pop;
}

// P9: one rebuilt view of the function — CFG, slot versions, value
// graph — from the current (possibly tombstoned) instruction stream.
struct SsiFn {
    bool ok = false;
    size_t n = 0;                    // live instruction count
    std::vector<size_t> live_idx;    // live pos -> real index
    size_t nb = 0;                   // blocks
    std::vector<size_t> bstart, bend;
    std::vector<std::vector<int32_t>> succs, preds;
    std::vector<int32_t> entry_h, exit_h;
    std::vector<int32_t> block_of;   // per live pos
    std::vector<uint8_t> reached;    // per live pos: reachable from entry
    uint32_t nslot = 0;              // loc slots (var_count)
    // Slot versions. Ver id < n: the defining write's live pos; ver id
    // >= n: the block-entry join (b, s) = n + b*nslot + s.
    std::vector<int32_t> slot_write; // per live pos: written slot or -1
    std::vector<int32_t> ver_def;    // per live pos: version read by a get-loc
    std::vector<int32_t> ver_def2;   // per live pos: second read (get_loc0_loc1)
    std::vector<int32_t> set_pre;    // per live pos: pre-write version (set_loc)
    std::vector<int32_t> ver_slot;   // per ver id
    std::vector<int32_t> ver_block;  // per ver id (joins)
    std::vector<std::vector<int32_t>> ver_uses;    // per ver id: < n = read insn, >= n = consuming join
    std::vector<std::vector<int32_t>> ver_inputs;  // per join ver: input ver ids
    std::vector<int32_t> exit_def;   // per block per slot: ver id at exit
    std::vector<int32_t> join_off;   // per block per slot: join ver id
    // Value graph. kind 0: push(live pos, ord); kind 1: stack join(block, pos).
    struct V {
        int32_t kind;
        int32_t a;
        int32_t b;
    };
    std::vector<V> vals;
    std::vector<std::vector<int32_t>> v_uses;    // per value: consuming live insns
    std::vector<std::vector<int32_t>> v_juses;   // per value: consuming stack joins
    std::vector<std::vector<int32_t>> v_inputs;  // per stack-join value: input value ids
    std::vector<std::vector<int32_t>> stk;       // per live pos: value ids before the insn
    std::vector<std::vector<int32_t>> pops;      // per live pos: popped value ids (bottom to top)
    std::vector<std::vector<int32_t>> pushes;    // per live pos: pushed value ids
    std::vector<std::vector<int32_t>> copy_src;  // per live pos: dup-family push cell sources
    std::vector<std::vector<int32_t>> exit_stk;  // per block: value ids at exit
    std::vector<std::vector<int32_t>> entry_stk; // per block: entry stack join value ids
    // Captured-slot mask (vardef is_captured): a captured slot can be
    // written by its closure at any runtime point, so its versions are
    // opaque (BOTTOM) and no pass may fold/copy/hoist through them.
    std::vector<uint8_t> captured;
    size_t ver_count = 0;
};

// Version id -> slot index (write versions: the written slot; join
// versions: the joined slot).
static int32_t ssi_ver_slot(const SsiFn& g, int32_t v) {
    if (v < static_cast<int32_t>(g.n)) return g.slot_write[v];
    return g.ver_slot[v];
}

// First live position at or after real index `r`, or n when none.
static size_t ssi_live_ceil(const std::vector<size_t>& live_idx, size_t r) {
    size_t lo = 0, hi = live_idx.size();
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (live_idx[mid] < r) lo = mid + 1; else hi = mid;
    }
    return lo;
}

static bool build_ssi(const std::vector<Insn>& insns,
                      const std::vector<uint8_t>& dead,
                      uint32_t var_count,
                      const std::vector<uint8_t>& captured, SsiFn* g) {
    const size_t ni = insns.size();
    if (var_count == 0) return false;
    g->captured = captured;
    for (size_t i = 0; i < ni; i++)
        if (!dead[i]) g->live_idx.push_back(i);
    g->n = g->live_idx.size();
    if (g->n == 0) return false;
    // Block split: leaders = live 0, live jump targets (ceiled over
    // dead runs), live fallthrough successors of terminators.
    std::vector<uint8_t> is_leader(g->n, 0);
    is_leader[0] = 1;
    for (size_t li = 0; li < g->n; li++) {
        const Insn& in = insns[g->live_idx[li]];
        if (in.target >= 0) {
            size_t c = ssi_live_ceil(g->live_idx, static_cast<size_t>(in.target));
            if (c >= g->n) return false;
            is_leader[c] = 1;
        }
        SsiTerm t = ssi_terminator(in.op);
        // A conditional/catch/gosub terminator's fallthrough starts a
        // new block; plain instructions fall through within the block.
        if ((t == SSI_T_COND || t == SSI_T_CATCH || t == SSI_T_GOSUB) &&
            li + 1 < g->n)
            is_leader[li + 1] = 1;
    }
    for (size_t li = 0; li < g->n; li++)
        if (is_leader[li]) g->bstart.push_back(li);
    g->nb = g->bstart.size();
    g->bend.resize(g->nb);
    for (size_t b = 0; b < g->nb; b++)
        g->bend[b] = (b + 1 < g->nb) ? g->bstart[b + 1] : g->n;
    g->block_of.assign(g->n, -1);
    for (size_t b = 0; b < g->nb; b++)
        for (size_t k = g->bstart[b]; k < g->bend[b]; k++)
            g->block_of[k] = static_cast<int32_t>(b);
    // Successors / predecessors.
    g->succs.resize(g->nb);
    g->preds.resize(g->nb);
    for (size_t b = 0; b < g->nb; b++) {
        size_t last = g->bend[b] - 1;
        const Insn& in = insns[g->live_idx[last]];
        SsiTerm t = ssi_terminator(in.op);
        size_t tt = (in.target >= 0)
                        ? ssi_live_ceil(g->live_idx, static_cast<size_t>(in.target))
                        : g->n;
        auto add_succ = [&](size_t pos) {
            if (pos >= g->n) return;
            size_t sb = static_cast<size_t>(g->block_of[pos]);
            g->succs[b].push_back(static_cast<int32_t>(sb));
            g->preds[sb].push_back(static_cast<int32_t>(b));
        };
        switch (t) {
        case SSI_T_GOTO:
            add_succ(tt);
            break;
        case SSI_T_COND:
        case SSI_T_CATCH:
            add_succ(tt);
            add_succ(last + 1);
            break;
        case SSI_T_GOSUB:
            add_succ(tt);
            add_succ(last + 1);
            break;
        case SSI_T_RETURN:
            break;
        case SSI_T_NONE:
            add_succ(last + 1);
            break;
        }
    }
    // Heights: verify_code's edge model, seeded from the entry.
    std::vector<int32_t> heights(g->n, -1), posts(g->n, 0);
    std::vector<uint8_t> seeded(g->n, 0);
    std::vector<size_t> wl;
    auto seed = [&](size_t pos, int32_t h) -> bool {
        if (pos >= g->n) return false;
        if (seeded[pos]) {
            if (heights[pos] != h) return false;  // height mismatch: decline
            return true;
        }
        seeded[pos] = 1;
        heights[pos] = h;
        wl.push_back(pos);
        return true;
    };
    if (!seed(0, 0)) return false;
    while (!wl.empty()) {
        size_t li = wl.back();
        wl.pop_back();
        const Insn& in = insns[g->live_idx[li]];
        int32_t n_pop = ssi_pop_count(in);
        if (heights[li] < n_pop) return false;
        int32_t post = heights[li] - n_pop + short_opcode_info(in.op).n_push;
        posts[li] = post;
        SsiTerm t = ssi_terminator(in.op);
        size_t tt = (in.target >= 0)
                        ? ssi_live_ceil(g->live_idx, static_cast<size_t>(in.target))
                        : g->n;
        switch (t) {
        case SSI_T_GOTO:
            if (!seed(tt, post)) return false;
            break;
        case SSI_T_COND:
        case SSI_T_CATCH:
            if (!seed(tt, post)) return false;
            if (!seed(li + 1, post)) return false;
            break;
        case SSI_T_GOSUB:
            // The finally entry sees the return-address slot (+1); the
            // return point is seeded via the fallthrough edge.
            if (!seed(tt, post + 1)) return false;
            if (!seed(li + 1, post)) return false;
            break;
        case SSI_T_RETURN:
            break;
        case SSI_T_NONE:
            if (!seed(li + 1, post)) return false;
            break;
        }
    }
    g->reached = seeded;
    g->entry_h.resize(g->nb);
    g->exit_h.resize(g->nb);
    for (size_t b = 0; b < g->nb; b++) {
        g->entry_h[b] = heights[g->bstart[b]];
        g->exit_h[b] = posts[g->bend[b] - 1];
    }
    // Join uniformity (phi consistency): every pred's exit height must
    // equal the join's entry height. catch edges match (the exception
    // lands at the post-catch height); gosub/with edges do not, which
    // declines the suite.
    for (size_t b = 0; b < g->nb; b++) {
        if (g->entry_h[b] < 0) continue;  // unreachable block
        for (int32_t p : g->preds[b]) {
            if (g->exit_h[p] != g->entry_h[b]) return false;
        }
    }
    // Version space.
    g->nslot = var_count;
    g->ver_count = g->n + g->nb * g->nslot;
    g->ver_slot.assign(g->ver_count, -1);
    g->ver_block.assign(g->ver_count, -1);
    g->ver_uses.resize(g->ver_count);
    g->ver_inputs.resize(g->nb * g->nslot);
    g->exit_def.assign(g->nb * g->nslot, -1);
    g->join_off.assign(g->nb * g->nslot, -1);
    for (size_t b = 0; b < g->nb; b++) {
        for (uint32_t s = 0; s < g->nslot; s++) {
            size_t j = b * g->nslot + s;
            int32_t ver = static_cast<int32_t>(g->n + j);
            g->join_off[j] = ver;
            g->ver_slot[ver] = static_cast<int32_t>(s);
            g->ver_block[ver] = static_cast<int32_t>(b);
        }
    }
    // Value graph + per-block stack walk.
    g->stk.resize(g->n);
    g->pops.resize(g->n);
    g->pushes.resize(g->n);
    g->copy_src.resize(g->n);
    g->slot_write.assign(g->n, -1);
    g->ver_def.assign(g->n, -1);
    g->ver_def2.assign(g->n, -1);
    g->set_pre.assign(g->n, -1);
    g->exit_stk.resize(g->nb);
    g->entry_stk.resize(g->nb);
    auto make_val = [&](int32_t kind, int32_t a, int32_t b) -> int32_t {
        int32_t v = static_cast<int32_t>(g->vals.size());
        g->vals.push_back({kind, a, b});
        g->v_uses.emplace_back();
        g->v_juses.emplace_back();
        g->v_inputs.emplace_back();
        return v;
    };
    for (size_t b = 0; b < g->nb; b++) {
        if (g->entry_h[b] < 0) continue;  // unreachable: no walk
        std::vector<int32_t> stack;
        for (int32_t p = 0; p < g->entry_h[b]; p++)
            stack.push_back(make_val(1, static_cast<int32_t>(b), p));
        g->entry_stk[b] = stack;
        std::vector<int32_t> cur_ver(g->nslot);
        for (uint32_t s = 0; s < g->nslot; s++)
            cur_ver[s] = g->join_off[b * g->nslot + s];
        for (size_t k = g->bstart[b]; k < g->bend[b]; k++) {
            g->stk[k] = stack;
            const Insn& in = insns[g->live_idx[k]];
            int32_t n_pop = ssi_pop_count(in);
            std::vector<int32_t> src;
            switch (in.op) {
            case OP_dup: src = {0, 0}; break;
            case OP_dup1: src = {0, 0, 1}; break;
            case OP_dup2: src = {0, 1, 0, 1}; break;
            case OP_dup3: src = {0, 1, 2, 0, 1, 2}; break;
            default: break;
            }
            std::vector<int32_t> popped;
            if (n_pop > 0) {
                popped.reserve(static_cast<size_t>(n_pop));
                for (int32_t kk = 0; kk < n_pop; kk++) {
                    int32_t v = stack[stack.size() - static_cast<size_t>(n_pop) +
                                      static_cast<size_t>(kk)];
                    popped.push_back(v);
                    g->v_uses[static_cast<size_t>(v)].push_back(
                        static_cast<int32_t>(k));
                }
                stack.resize(stack.size() - static_cast<size_t>(n_pop));
            }
            g->pops[k] = popped;
            if (!src.empty()) {
                for (size_t kk = 0; kk < src.size(); kk++)
                    g->copy_src[k].push_back(popped[src[kk]]);
            }
            uint32_t sl;
            if (is_get_loc_op(in.op) && slot_of(in, &sl)) {
                if (sl >= g->nslot) return false;  // malformed: fail closed
                g->ver_def[k] = cur_ver[sl];
                // Register the read as a use of the version it reads
                // (ver_uses — the slot-version space; v_uses is the
                // stack-value space and must not be indexed by a
                // version id). Join liveness (P12') is computed over
                // ver_uses; without read uses a join whose only readers
                // are loc ops looks dead, so the join is never marked
                // live and its preds' exit versions never get use
                // counts — the DCE would then kill a slot init whose
                // value is read through the join (TDZ marker left in
                // the slot).
                g->ver_uses[static_cast<size_t>(cur_ver[sl])].push_back(
                    static_cast<int32_t>(k));
            } else if (in.op == OP_get_loc0_loc1) {
                if (g->nslot < 2) return false;
                g->ver_def[k] = cur_ver[0];
                g->ver_def2[k] = cur_ver[1];
                g->ver_uses[static_cast<size_t>(cur_ver[0])].push_back(
                    static_cast<int32_t>(k));
                g->ver_uses[static_cast<size_t>(cur_ver[1])].push_back(
                    static_cast<int32_t>(k));
            }
            int32_t n_push = short_opcode_info(in.op).n_push;
            for (int32_t kk = 0; kk < n_push; kk++)
                g->pushes[k].push_back(make_val(0, static_cast<int32_t>(k), kk));
            for (int32_t v : g->pushes[k]) stack.push_back(v);
            // set_loc pops the new value and pushes the old one: record
            // the pre-write version before overwriting.
            if (is_put_loc_op(in.op) || is_set_loc_op(in.op) ||
                is_slot_mut_op(in.op)) {
                if (slot_of(in, &sl)) {
                    if (sl >= g->nslot) return false;  // malformed
                    if (is_set_loc_op(in.op)) g->set_pre[k] = cur_ver[sl];
                    cur_ver[sl] = static_cast<int32_t>(k);
                    g->slot_write[k] = static_cast<int32_t>(sl);
                }
            }
        }
        g->exit_stk[b] = stack;
        for (uint32_t s = 0; s < g->nslot; s++)
            g->exit_def[b * g->nslot + s] = cur_ver[s];
    }
    // Join inputs and uses.
    for (size_t b = 0; b < g->nb; b++) {
        if (g->entry_h[b] < 0) continue;
        for (uint32_t s = 0; s < g->nslot; s++) {
            int32_t jv = g->join_off[b * g->nslot + s];
            std::vector<int32_t>* inputs =
                &g->ver_inputs[jv - static_cast<int32_t>(g->n)];
            for (int32_t p : g->preds[b]) {
                int32_t ev = g->exit_def[p * g->nslot + s];
                inputs->push_back(ev);
                g->ver_uses[static_cast<size_t>(ev)].push_back(jv);
            }
        }
        for (size_t p = 0; p < g->entry_stk[b].size(); p++) {
            int32_t jv = g->entry_stk[b][p];
            for (int32_t pr : g->preds[b]) {
                int32_t ev = g->exit_stk[static_cast<size_t>(pr)][p];
                g->v_inputs[static_cast<size_t>(jv)].push_back(ev);
                g->v_juses[static_cast<size_t>(ev)].push_back(jv);
            }
        }
    }
    g->ok = true;
    return true;
}

// The set of write live positions that can define `ver` (a write or a
// join). DFS over the exit-def graph; the in-stack marker cuts cycles
// (a backedge's defs are reached through the loop's own writes).
static void collect_defs(const SsiFn& g, int32_t ver,
                         std::vector<uint8_t>* in_stack,
                         std::vector<int32_t>* defs) {
    if (in_stack->at(static_cast<size_t>(ver))) return;
    in_stack->at(static_cast<size_t>(ver)) = 1;
    if (ver < static_cast<int32_t>(g.n)) {
        defs->push_back(ver);
        return;
    }
    size_t j = static_cast<size_t>(ver - static_cast<int32_t>(g.n));
    size_t b = j / g.nslot;
    size_t s = j % g.nslot;
    for (int32_t p : g.preds[b])
        collect_defs(g, g.exit_def[p * static_cast<int32_t>(g.nslot) +
                                    static_cast<int32_t>(s)],
                     in_stack, defs);
}

// The version of `slot` at live position `k`: the last write before k
// in k's block, or the block-entry join.
static int32_t resolve_version(const SsiFn& g, size_t k, uint32_t slot) {
    size_t b = static_cast<size_t>(g.block_of[k]);
    for (size_t j = k; j-- > g.bstart[b];) {
        if (g.slot_write[j] == static_cast<int32_t>(slot))
            return static_cast<int32_t>(j);
    }
    return g.join_off[b * g.nslot + slot];
}

// P14' kill list for the cone rule (mirror of the sweep's clear_all).
static bool ssi_cone_kill(uint8_t op) {
    switch (op) {
    case OP_call: case OP_call1: case OP_call2: case OP_call3:
    case OP_tail_call: case OP_call_method: case OP_tail_call_method:
    case OP_call_constructor: case OP_apply: case OP_apply_eval:
    case OP_iterator_call:
    case OP_put_field: case OP_put_array_el:
    case OP_define_array_el: case OP_append:
    case OP_set_home_object: case OP_set_name: case OP_set_name_computed:
    case OP_fclosure: case OP_fclosure8:
    case OP_eval: case OP_using_dispose: case OP_using_dispose_async:
        return true;
    default:
        return false;
    }
}
// True when every block on some path from block `ab` to block `bb` is
// kill-free. The intersection of forward reachability from `ab` and
// backward reachability to `bb` is exactly the set of blocks that lie
// on a def->fold path.
static bool ssi_cone_clean(const SsiFn& g, const std::vector<Insn>& insns,
                           size_t ab, size_t bb) {
    std::vector<uint8_t> fwd(g.nb, 0);
    std::vector<size_t> wl;
    fwd[ab] = 1;
    wl.push_back(ab);
    while (!wl.empty()) {
        size_t x = wl.back();
        wl.pop_back();
        for (int32_t su : g.succs[x])
            if (!fwd[static_cast<size_t>(su)]) {
                fwd[static_cast<size_t>(su)] = 1;
                wl.push_back(static_cast<size_t>(su));
            }
    }
    std::vector<uint8_t> back(g.nb, 0);
    back[bb] = 1;
    wl.push_back(bb);
    while (!wl.empty()) {
        size_t x = wl.back();
        wl.pop_back();
        for (int32_t p : g.preds[x])
            if (!back[static_cast<size_t>(p)]) {
                back[static_cast<size_t>(p)] = 1;
                wl.push_back(static_cast<size_t>(p));
            }
    }
    for (size_t b = 0; b < g.nb; b++) {
        if (!fwd[b] || !back[b]) continue;
        for (size_t k = g.bstart[b]; k < g.bend[b]; k++)
            if (ssi_cone_kill(insns[g.live_idx[k]].op)) return false;
    }
    return true;
}

// Replace the instruction at live pos `li` with a push of the constant
// cell (used by P10; the constant pool is never modified).
static void ssi_emit_lat_push(std::vector<Insn>* insns, size_t real,
                              const SsiLat& c) {
    Insn ni;
    ni.old_off = (*insns)[real].old_off;
    ni.pc_off = (*insns)[real].pc_off;
    ni.target = -1;
    ni.imm = 0;
    ni.aux = 0;
    ni.has_aux = false;
    switch (c.kind) {
    case SSI_L_INT:
    case SSI_L_BOOL:
        ni.op = shortest_push_op(c.v);
        ni.imm = c.v;
        break;
    case SSI_L_UNDEF:
        ni.op = OP_undefined;
        break;
    case SSI_L_NULL:
        ni.op = OP_null;
        break;
    case SSI_L_ATOM:
        ni.op = OP_push_atom_value;
        ni.aux = c.v;
        ni.has_aux = true;
        break;
    case SSI_L_CPOOL:
        ni.op = OP_push_const;  // reshrunk to push_const8 when small
        ni.aux = c.v;
        ni.has_aux = true;
        break;
    default:
        return;
    }
    ni.old_size = static_cast<uint16_t>(short_opcode_info(ni.op).size);
    (*insns)[real] = ni;
}

// Replace the instruction at real index `real` with a push of `val`
// (P14' folds; mirrors P14's emit_val_push).
static void ssi_emit_val_push(std::vector<Insn>* insns, size_t real,
                              const SVal& val, uint32_t pc_off) {
    Insn ni;
    if (val.is_atom) {
        ni.op = OP_push_atom_value;
        ni.aux = val.atom;
        ni.has_aux = true;
        ni.imm = 0;
    } else if (val.is_cpool) {
        ni.op = OP_push_const;  // reshrunk to push_const8 when small
        ni.aux = val.cidx;
        ni.has_aux = true;
        ni.imm = 0;
    } else {
        ni.op = shortest_push_op(val.v);
        ni.imm = val.v;
        ni.has_aux = false;
    }
    ni.old_off = (*insns)[real].old_off;
    ni.old_size = static_cast<uint16_t>(short_opcode_info(ni.op).size);
    ni.pc_off = pc_off;
    ni.target = -1;
    (*insns)[real] = ni;
}

// Cell of the k-th value pushed by the instruction at live pos li.
static SsiLat ssi_push_cell(const SsiFn& g, const std::vector<Insn>& insns,
                            const std::vector<SsiLat>& val_lat,
                            const std::vector<SsiLat>& ver_lat, size_t v) {
    const SsiFn::V& val = g.vals[v];
    size_t li = static_cast<size_t>(val.a);
    int32_t k = val.b;
    const Insn& in = insns[g.live_idx[li]];
    switch (in.op) {
    case OP_push_minus1: case OP_push_0: case OP_push_1:
    case OP_push_2: case OP_push_3: case OP_push_4:
    case OP_push_5: case OP_push_6: case OP_push_7:
    case OP_push_i8: case OP_push_i16: case OP_push_i32:
        return SsiLat{SSI_L_INT, static_cast<int32_t>(in.imm)};
    case OP_push_const: case OP_push_const8:
        return SsiLat{SSI_L_CPOOL, static_cast<int32_t>(in.aux)};
    case OP_push_atom_value:
        return SsiLat{SSI_L_ATOM, static_cast<int32_t>(in.aux)};
    case OP_push_true:
        return SsiLat{SSI_L_BOOL, 1};
    case OP_push_false:
        return SsiLat{SSI_L_BOOL, 0};
    case OP_undefined:
        return SsiLat{SSI_L_UNDEF, 0};
    case OP_null:
        return SsiLat{SSI_L_NULL, 0};
    case OP_object:
    case OP_array_from:
        return SsiLat{SSI_L_OBJECT, 0};
    case OP_get_loc: case OP_get_loc8: case OP_get_loc_check:
    case OP_get_loc0: case OP_get_loc1: case OP_get_loc2: case OP_get_loc3:
        return ver_lat[g.ver_def[li]];
    case OP_get_loc0_loc1:
        return ver_lat[k == 0 ? g.ver_def[li] : g.ver_def2[li]];
    case OP_set_loc: case OP_set_loc8:
    case OP_set_loc0: case OP_set_loc1: case OP_set_loc2: case OP_set_loc3:
        // Pushes the old value.
        return ver_lat[g.set_pre[li]];
    case OP_dup: case OP_dup1: case OP_dup2: case OP_dup3: {
        int32_t src = g.copy_src[li][k];
        if (src < 0) return SsiLat{SSI_L_BOTTOM, 0};
        return val_lat[src];
    }
    case OP_add: case OP_sub: case OP_mul: case OP_and: case OP_or:
    case OP_xor: case OP_shl: case OP_sar: case OP_shr: case OP_mod: {
        if (g.pops[li].size() != 2) return SsiLat{SSI_L_BOTTOM, 0};
        const SsiLat& a = val_lat[g.pops[li][0]];
        const SsiLat& b = val_lat[g.pops[li][1]];
        if (a.kind == SSI_L_INT && b.kind == SSI_L_INT) {
            int64_t r;
            if (foldable_binop(in.op, a.v, b.v, &r))
                return SsiLat{SSI_L_INT, static_cast<int32_t>(r)};
        }
        return SsiLat{SSI_L_BOTTOM, 0};
    }
    default:
        return SsiLat{SSI_L_BOTTOM, 0};
    }
}

// P10: SCCP — lattice fixpoint, then constant folds of slot reads and
// constant-condition branch folds. All rewrites are stack-neutral.
static bool apply_ssi_sccp(std::vector<Insn>* insns,
                           std::vector<uint8_t>* dead,
                           uint32_t var_count,
                           const std::vector<uint8_t>& captured,
                           RewriteStats* stats) {
    SsiFn g;
    if (!build_ssi(*insns, *dead, var_count, captured, &g)) return false;
    // Lattice fixpoint (Gauss-Seidel sweeps; a cell moves at most
    // TOP -> constant -> BOTTOM, so the cap only ever leaves cells at
    // TOP, which folds skip).
    std::vector<SsiLat> val_lat(g.vals.size(), SsiLat{SSI_L_TOP, 0});
    std::vector<SsiLat> ver_lat(g.ver_count, SsiLat{SSI_L_TOP, 0});
    int rounds = 0;
    bool cell_changed = true;
    while (cell_changed && rounds++ < 64) {
        cell_changed = false;
        for (size_t v = 0; v < g.vals.size(); v++) {
            SsiLat nv;
            if (g.vals[v].kind == 1) {
                nv = SsiLat{SSI_L_TOP, 0};
                for (int32_t in : g.v_inputs[v])
                    nv = ssi_lat_meet(nv, val_lat[in]);
            } else {
                nv = ssi_push_cell(g, *insns, val_lat, ver_lat, v);
            }
            if (nv.kind != val_lat[v].kind || nv.v != val_lat[v].v) {
                val_lat[v] = nv;
                cell_changed = true;
            }
        }
        for (size_t vid = 0; vid < g.ver_count; vid++) {
            SsiLat nv;
            if (static_cast<int32_t>(vid) < static_cast<int32_t>(g.n)) {
                nv = g.pops[vid].empty()
                         ? SsiLat{SSI_L_BOTTOM, 0}
                         : val_lat[g.pops[vid].back()];
            } else {
                nv = SsiLat{SSI_L_TOP, 0};
                for (int32_t in : g.ver_inputs[vid - g.n])
                    nv = ssi_lat_meet(nv, ver_lat[in]);
            }
            // Captured-slot versions are opaque: the closure's runtime
            // writes (via var_ref into the live frame) are invisible
            // to this stream, so no cell may claim a constant.
            int32_t vslot = ssi_ver_slot(g, static_cast<int32_t>(vid));
            if (vslot >= 0 &&
                static_cast<size_t>(vslot) < g.captured.size() &&
                g.captured[static_cast<size_t>(vslot)]) {
                nv = SsiLat{SSI_L_BOTTOM, 0};
            }
            if (nv.kind != ver_lat[vid].kind || nv.v != ver_lat[vid].v) {
                ver_lat[vid] = nv;
                cell_changed = true;
            }
        }
    }
    bool changed = false;
    // Constant slot reads -> constant pushes. A check-form read folds
    // only on a known cell: the cell is then not the TDZ marker, so the
    // check could not throw. Captured slots never fold (defense in
    // depth: their versions are BOTTOM above, so the cell test already
    // excludes them).
    for (size_t li = 0; li < g.n; li++) {
        const Insn& in = (*insns)[g.live_idx[li]];
        if (!is_get_loc_op(in.op) || in.op == OP_get_loc0_loc1) continue;
        uint32_t sl;
        if (!slot_of(in, &sl) || sl >= g.captured.size() ||
            g.captured[sl])
            continue;
        const SsiLat& c = ver_lat[g.ver_def[li]];
        if (c.kind >= SSI_L_INT && c.kind <= SSI_L_CPOOL) {
            ssi_emit_lat_push(insns, g.live_idx[li], c);
            stats->folds_sccp++;
            changed = true;
        }
    }
    // Constant-condition branches: the taken edge dies (fold the
    // producer+branch pair away, or replace the branch with a drop), or
    // the fallthrough dies (goto with the producer removed). A landing
    // between the producer and the branch would evaluate a different
    // value, so any target in that span blocks the fold.
    std::vector<uint8_t> targets = compute_targets(*insns);
    for (size_t li = 0; li < g.n; li++) {
        const Insn& in = (*insns)[g.live_idx[li]];
        if (in.op != OP_if_true && in.op != OP_if_false &&
            in.op != OP_if_true8 && in.op != OP_if_false8)
            continue;
        if (targets[g.live_idx[li]]) continue;
        if (g.stk[li].empty()) continue;
        int32_t cv = g.stk[li].back();
        const SsiLat& c = val_lat[cv];
        bool truthy;
        if (c.kind == SSI_L_INT || c.kind == SSI_L_BOOL)
            truthy = c.v != 0;
        else if (c.kind == SSI_L_UNDEF || c.kind == SSI_L_NULL)
            truthy = false;
        else if (c.kind == SSI_L_OBJECT)
            truthy = true;
        else
            continue;
        bool cond_true = (in.op == OP_if_true || in.op == OP_if_true8);
        bool taken = cond_true == truthy;
        size_t gp = g.n;
        bool producer_ok = false;
        if (g.vals[cv].kind == 0 && g.vals[cv].b == 0) {
            size_t p = static_cast<size_t>(g.vals[cv].a);
            if (p < li) {
                const Insn& pin = (*insns)[g.live_idx[p]];
                producer_ok = is_ssi_pure_push(pin.op) &&
                              pin.op != OP_get_loc0_loc1;
                if (producer_ok) gp = p;
            }
        }
        // The span guard: no target in (r0, li], where r0 is the
        // producer (or the block start for a join condition). Landing
        // on r0 itself is fine.
        size_t r0 = gp < g.n ? g.live_idx[gp]
                             : g.live_idx[g.bstart[g.block_of[li]]];
        size_t r1 = g.live_idx[li];
        bool span_clean = true;
        for (size_t r = r0 + 1; r <= r1; r++) {
            if (targets[r]) {
                span_clean = false;
                break;
            }
        }
        if (!span_clean) continue;
        bool lone = g.v_uses[cv].size() == 1 &&
                    g.v_uses[cv][0] == static_cast<int32_t>(li) &&
                    g.v_juses[cv].empty();
        if (!taken) {
            // Fallthrough live: the pair is stack-neutral, so both
            // delete together, or the branch becomes a drop.
            if (producer_ok && lone) {
                (*dead)[g.live_idx[gp]] = 1;
                (*dead)[g.live_idx[li]] = 1;
            } else {
                Insn ni = (*insns)[g.live_idx[li]];
                ni.op = OP_drop;
                ni.target = -1;
                ni.imm = 0;
                ni.aux = 0;
                ni.has_aux = false;
                ni.old_size = static_cast<uint16_t>(
                    short_opcode_info(OP_drop).size);
                (*insns)[g.live_idx[li]] = ni;
            }
            stats->folds_sccp++;
            changed = true;
        } else {
            // Taken edge live: fold only when the producer dies with
            // it; otherwise the branch stays and the DCE stage removes
            // the now-unreachable fallthrough block.
            if (producer_ok && lone) {
                (*dead)[g.live_idx[gp]] = 1;
                Insn ni = (*insns)[g.live_idx[li]];
                ni.op = OP_goto;  // the emitter's jump fixpoint resizes
                ni.target = in.target;
                ni.imm = 0;
                ni.aux = 0;
                ni.has_aux = false;
                ni.old_size = 5;
                (*insns)[g.live_idx[li]] = ni;
                stats->folds_sccp++;
                changed = true;
            }
        }
    }
    return changed;
}

// P11': SSA copy propagation. A version whose defining writes all copy
// the same source version (get_loc s2; put_loc t) is renamed to read
// slot s2 directly, at each read site where the s2 version there is
// provably the same. The now-unused copies die in P12'.
static bool apply_ssi_copyprop(std::vector<Insn>* insns,
                               std::vector<uint8_t>* dead,
                               uint32_t var_count,
                               const std::vector<uint8_t>& captured,
                               RewriteStats* stats) {
    SsiFn g;
    if (!build_ssi(*insns, *dead, var_count, captured, &g)) return false;
    bool changed = false;
    for (size_t i = 0; i < g.n; i++) {
        const Insn& in = (*insns)[g.live_idx[i]];
        if (!is_get_loc_op(in.op) || in.op == OP_get_loc0_loc1) continue;
        uint32_t rslot;
        if (!slot_of(in, &rslot) || rslot >= captured.size() ||
            captured[rslot])
            continue;
        int32_t v = g.ver_def[i];
        std::vector<int32_t> defs;
        std::vector<uint8_t> in_stack(g.ver_count, 0);
        collect_defs(g, v, &in_stack, &defs);
        if (defs.empty()) continue;
        int32_t u2 = -1;
        uint32_t s2 = 0;
        bool ok = true;
        for (int32_t w : defs) {
            if (w >= static_cast<int32_t>(g.n) || g.pops[w].empty()) {
                ok = false;
                break;
            }
            int32_t sv = g.pops[w].back();
            if (g.vals[sv].kind != 0 || g.vals[sv].b != 0) {
                ok = false;
                break;
            }
            size_t gp = static_cast<size_t>(g.vals[sv].a);
            const Insn& gin = (*insns)[g.live_idx[gp]];
            uint32_t gs;
            if (!is_get_loc_op(gin.op) || gin.op == OP_get_loc0_loc1 ||
                !slot_of(gin, &gs) || gs >= captured.size() ||
                captured[gs]) {
                ok = false;
                break;
            }
            int32_t gu = g.ver_def[gp];
            if (u2 < 0) {
                u2 = gu;
                s2 = gs;
            } else if (u2 != gu || s2 != gs) {
                ok = false;
                break;
            }
        }
        if (!ok || u2 < 0) continue;
        // The slot s2 version at the read site must be provably the
        // same version the copies read.
        int32_t ri = resolve_version(g, i, s2);
        bool same;
        if (ri == u2) {
            same = true;
        } else if (ri >= static_cast<int32_t>(g.n)) {
            std::vector<int32_t> d2;
            std::vector<uint8_t> s2v(g.ver_count, 0);
            collect_defs(g, ri, &s2v, &d2);
            std::vector<int32_t> d3;
            std::vector<uint8_t> s3v(g.ver_count, 0);
            collect_defs(g, u2, &s3v, &d3);
            same = d2.size() == d3.size() && d2 == d3;
        } else {
            same = false;
        }
        if (!same) continue;
        Insn ni = (*insns)[g.live_idx[i]];
        ni.op = OP_get_loc;  // reshrunk to the short form later
        ni.aux = s2;
        ni.has_aux = true;
        (*insns)[g.live_idx[i]] = ni;
        stats->folds_p11s++;
        changed = true;
    }
    return changed;
}

// P12': SSA DCE. Dead stores vanish as pairs with their feeding push
// (stack-neutral, so a landing on the get_loc is safe; a landing in
// (get, store] would see the pushed value, so any target there blocks
// the fold). Blocks unreachable from the entry are removed wholesale.
static bool apply_ssi_dce(std::vector<Insn>* insns,
                          std::vector<uint8_t>* dead,
                          uint32_t var_count,
                          const std::vector<uint8_t>& captured,
                          RewriteStats* stats) {
    SsiFn g;
    if (!build_ssi(*insns, *dead, var_count, captured, &g)) return false;
    bool changed = false;
    // Join liveness (backward fixpoint): a join is live iff a read uses
    // it or a live successor join consumes its exit version.
    std::vector<uint8_t> jlive(g.nb * g.nslot, 0);
    int rounds = 0;
    bool cl = true;
    while (cl && rounds++ < static_cast<int>(g.nb) + 4) {
        cl = false;
        for (size_t b = 0; b < g.nb; b++) {
            for (uint32_t s = 0; s < g.nslot; s++) {
                size_t j = b * g.nslot + s;
                if (jlive[j]) continue;
                int32_t jv = static_cast<int32_t>(g.n + j);
                bool live = false;
                for (int32_t u : g.ver_uses[static_cast<size_t>(jv)]) {
                    if (u < static_cast<int32_t>(g.n) ||
                        jlive[static_cast<size_t>(u - static_cast<int32_t>(g.n))]) {
                        live = true;
                        break;
                    }
                }
                if (live) {
                    jlive[j] = 1;
                    cl = true;
                }
            }
        }
    }
    // Version use counts.
    std::vector<int32_t> vcount(g.ver_count, 0);
    for (size_t k = 0; k < g.n; k++) {
        if (g.ver_def[k] >= 0) vcount[g.ver_def[k]]++;
        if (g.ver_def2[k] >= 0) vcount[g.ver_def2[k]]++;
    }
    for (size_t b = 0; b < g.nb; b++) {
        if (g.entry_h[b] < 0) continue;
        for (uint32_t s = 0; s < g.nslot; s++) {
            size_t j = b * g.nslot + s;
            if (!jlive[j]) continue;
            for (int32_t p : g.preds[b]) {
                int32_t ev = g.exit_def[p * g.nslot + s];
                vcount[ev]++;
            }
        }
    }
    std::vector<uint8_t> targets = compute_targets(*insns);
    // TDZ-managed slots: set_loc_uninitialized plants the marker that
    // the init store clears. Removing the init store (value never read)
    // leaves the marker in place, and the first check-form op on that
    // path (get_loc_check/put_loc_check) then throws where the source
    // did not — the marker-clearing effect is not visible to
    // value-flow liveness. Any slot with a marker write anywhere is
    // ineligible for store removal.
    std::vector<uint8_t> tdz_slot(g.nslot, 0);
    for (size_t k = 0; k < g.n; k++) {
        if ((*insns)[g.live_idx[k]].op == OP_set_loc_uninitialized) {
            int32_t ts = g.slot_write[k];
            if (ts >= 0 && static_cast<size_t>(ts) < tdz_slot.size())
                tdz_slot[static_cast<size_t>(ts)] = 1;
        }
    }
    for (size_t w = 0; w < g.n; w++) {
        if (g.slot_write[w] < 0 || vcount[w] != 0) continue;
        // Captured slot: the closure can read the stored value at any
        // runtime point (the store is not dead).
        if (static_cast<size_t>(g.slot_write[w]) < captured.size() &&
            captured[static_cast<size_t>(g.slot_write[w])])
            continue;
        // TDZ slot: see above — the init store also clears the marker.
        if (static_cast<size_t>(g.slot_write[w]) < tdz_slot.size() &&
            tdz_slot[static_cast<size_t>(g.slot_write[w])])
            continue;
        const Insn& win = (*insns)[g.live_idx[w]];
        // Non-check put_loc family only: the check forms can throw on
        // the TDZ marker, and set_loc pushes the old value.
        if (!(win.op == OP_put_loc || win.op == OP_put_loc8 ||
              (win.op >= OP_put_loc0 && win.op <= OP_put_loc3)))
            continue;
        if (g.pops[w].empty()) continue;
        int32_t sv = g.pops[w].back();
        if (g.vals[sv].kind != 0 || g.vals[sv].b != 0) continue;
        size_t gp = static_cast<size_t>(g.vals[sv].a);
        if (gp >= w || g.block_of[gp] != g.block_of[w]) continue;
        if (g.v_uses[sv].size() != 1 ||
            g.v_uses[sv][0] != static_cast<int32_t>(w) ||
            !g.v_juses[sv].empty())
            continue;
        const Insn& gin = (*insns)[g.live_idx[gp]];
        bool prod_ok =
            (is_get_loc_op(gin.op) && !is_check_loc_op(gin.op) &&
             gin.op != OP_get_loc0_loc1) ||
            is_pure_const_push(gin.op);
        if (!prod_ok) continue;
        size_t r0 = g.live_idx[gp], r1 = g.live_idx[w];
        bool clean = true;
        for (size_t r = r0 + 1; r <= r1; r++) {
            if (targets[r]) {
                clean = false;
                break;
            }
        }
        if (!clean) continue;
        (*dead)[g.live_idx[gp]] = 1;
        (*dead)[g.live_idx[w]] = 1;
        stats->folds_dce++;
        changed = true;
    }
    // Unreachable blocks (their insns never run; removing them changes
    // nothing observable).
    for (size_t b = 0; b < g.nb; b++) {
        if (g.entry_h[b] >= 0) continue;
        for (size_t k = g.bstart[b]; k < g.bend[b]; k++)
            (*dead)[g.live_idx[k]] = 1;
        stats->folds_dce++;
        changed = true;
    }
    return changed;
}

// P13': LICM (stack-neutral subset). In a natural loop with a single
// backedge and a plain-goto pre-header, a (get_loc s; put_loc t) pair
// with s unwritten in the loop and every t-read resolving to the pair
// is hoisted in front of the pre-header's goto. The hoisted copy runs
// exactly on the loop-entry path, so the first iteration's reads are
// unchanged; the in-loop pair removal is stack-neutral.
static bool apply_ssi_licm(std::vector<Insn>* insns,
                           std::vector<uint8_t>* dead,
                           uint32_t var_count,
                           const std::vector<uint8_t>& captured,
                           RewriteStats* stats) {
    SsiFn g;
    if (!build_ssi(*insns, *dead, var_count, captured, &g)) return false;
    bool changed = false;
    // Iterative dominators over the reachable blocks.
    std::vector<std::vector<uint8_t>> dom(
        g.nb, std::vector<uint8_t>(g.nb, 0));
    for (size_t b = 0; b < g.nb; b++) {
        if (g.entry_h[b] < 0) continue;
        for (size_t i = 0; i < g.nb; i++) dom[b][i] = 1;
    }
    for (size_t i = 1; i < g.nb; i++) dom[0][i] = 0;
    bool changed_dom = true;
    while (changed_dom) {
        changed_dom = false;
        for (size_t b = 1; b < g.nb; b++) {
            if (g.entry_h[b] < 0) continue;
            std::vector<uint8_t> nd(g.nb, 0);
            bool first = true;
            for (int32_t p : g.preds[b]) {
                if (g.entry_h[p] < 0) continue;
                if (first) {
                    nd = dom[static_cast<size_t>(p)];
                    first = false;
                } else {
                    for (size_t i = 0; i < g.nb; i++) nd[i] &= dom[static_cast<size_t>(p)][i];
                }
            }
            if (!first) {
                nd[b] = 1;
                if (nd != dom[b]) {
                    dom[b] = nd;
                    changed_dom = true;
                }
            }
        }
    }
    std::vector<uint8_t> targets = compute_targets(*insns);
    for (size_t b = 1; b < g.nb; b++) {
        if (g.entry_h[b] < 0) continue;
        for (int32_t h32 : g.succs[b]) {
            size_t h = static_cast<size_t>(h32);
            if (h == b || g.entry_h[h] < 0 || !dom[b][h]) continue;
            // Backedge (b -> h): the loop is everything that reaches b
            // without going through h.
            std::vector<uint8_t> in_loop(g.nb, 0);
            std::vector<size_t> wl2;
            in_loop[b] = 1;
            wl2.push_back(b);
            while (!wl2.empty()) {
                size_t x = wl2.back();
                wl2.pop_back();
                for (int32_t p : g.preds[x]) {
                    if (static_cast<size_t>(p) == h) continue;
                    if (!in_loop[static_cast<size_t>(p)]) {
                        in_loop[static_cast<size_t>(p)] = 1;
                        wl2.push_back(static_cast<size_t>(p));
                    }
                }
            }
            in_loop[h] = 1;
            // Header preds: exactly one outside the loop (the
            // pre-header) and exactly one inside (this backedge).
            std::vector<int32_t> outside;
            int loop_preds = 0;
            for (int32_t p : g.preds[h]) {
                if (in_loop[static_cast<size_t>(p)]) loop_preds++;
                else outside.push_back(p);
            }
            if (outside.size() != 1 || loop_preds != 1) continue;
            size_t ph = static_cast<size_t>(outside[0]);
            if (g.entry_h[ph] < 0) continue;
            // The pre-header must end in a plain goto to the header, or
            // fall through into it (a conditional end whose taken edge
            // leaves the loop is fine: the fallthrough is the only path
            // into the header and it passes the insertion point).
            size_t pl = g.bend[ph] - 1;
            const Insn& pin = (*insns)[g.live_idx[pl]];
            bool pre_ok = false;
            if (pin.op == OP_goto || pin.op == OP_goto8 ||
                pin.op == OP_goto16) {
                size_t pt =
                    ssi_live_ceil(g.live_idx, static_cast<size_t>(pin.target));
                pre_ok = (pt == g.bstart[h]);
            } else if (ssi_terminator(pin.op) == SSI_T_NONE) {
                pre_ok = (g.bend[ph] == g.bstart[h]);
            }
            if (!pre_ok) continue;
            // The pair candidate: (get_loc s; put_loc t) in a loop
            // block, t != s, the value consumed only by the store.
            size_t pair_g = g.n, pair_w = g.n;
            uint32_t slot_s = 0, slot_t = 0;
            for (size_t k = 0; k < g.n && pair_w == g.n; k++) {
                if (!in_loop[g.block_of[k]]) continue;
                if (g.slot_write[k] < 0) continue;
                const Insn& win = (*insns)[g.live_idx[k]];
                if (!(win.op == OP_put_loc || win.op == OP_put_loc8 ||
                      (win.op >= OP_put_loc0 && win.op <= OP_put_loc3)))
                    continue;
                if (g.pops[k].empty()) continue;
                int32_t sv = g.pops[k].back();
                if (g.vals[sv].kind != 0 || g.vals[sv].b != 0) continue;
                size_t gp = static_cast<size_t>(g.vals[sv].a);
                if (gp >= k || g.block_of[gp] != g.block_of[k]) continue;
                const Insn& gin = (*insns)[g.live_idx[gp]];
                uint32_t gs, ts;
                if (!(is_get_loc_op(gin.op) && !is_check_loc_op(gin.op) &&
                      gin.op != OP_get_loc0_loc1) ||
                    !slot_of(gin, &gs) || !slot_of(win, &ts) || gs == ts)
                    continue;
                // Captured slots: closure writes can break the
                // invariance and the version resolution.
                if (gs >= captured.size() || ts >= captured.size() ||
                    captured[gs] || captured[ts])
                    continue;
                if (g.v_uses[sv].size() != 1 ||
                    g.v_uses[sv][0] != static_cast<int32_t>(k) ||
                    !g.v_juses[sv].empty())
                    continue;
                size_t r0 = g.live_idx[gp], r1 = g.live_idx[k];
                bool clean = true;
                for (size_t r = r0 + 1; r <= r1; r++) {
                    if (targets[r]) {
                        clean = false;
                        break;
                    }
                }
                if (!clean) continue;
                pair_g = gp;
                pair_w = k;
                slot_s = gs;
                slot_t = ts;
            }
            if (pair_w == g.n) continue;
            // s must be unwritten in the loop (the read value is
            // invariant; any write would change it on later iterations).
            bool s_clean = true;
            for (size_t k = 0; k < g.n; k++) {
                if (!in_loop[g.block_of[k]]) continue;
                if (g.slot_write[k] == static_cast<int32_t>(slot_s)) {
                    s_clean = false;
                    break;
                }
            }
            if (!s_clean) continue;
            // Every t read in the loop must resolve to the pair's
            // version (a read before the pair on any path would see a
            // different value once the pair moves to the pre-header).
            bool t_clean = true;
            for (size_t k = 0; k < g.n; k++) {
                if (!in_loop[g.block_of[k]]) continue;
                const Insn& kin = (*insns)[g.live_idx[k]];
                int32_t rv = -1;
                if (is_get_loc_op(kin.op)) {
                    uint32_t sl;
                    if (kin.op == OP_get_loc0_loc1) {
                        if (slot_t == 0) rv = g.ver_def[k];
                        else if (slot_t == 1) rv = g.ver_def2[k];
                    } else if (slot_of(kin, &sl) && sl == slot_t) {
                        rv = g.ver_def[k];
                    }
                }
                if (rv < 0) continue;
                int32_t resolved = resolve_version(g, k, slot_t);
                if (resolved == static_cast<int32_t>(pair_w)) continue;
                if (resolved >= static_cast<int32_t>(g.n)) {
                    std::vector<int32_t> defs;
                    std::vector<uint8_t> in_stack(g.ver_count, 0);
                    collect_defs(g, resolved, &in_stack, &defs);
                    if (defs.size() == 1 &&
                        defs[0] == static_cast<int32_t>(pair_w))
                        continue;
                }
                t_clean = false;
                break;
            }
            if (!t_clean) continue;
            // Hoist [get_loc s; put_loc t] in front of the pre-header's
            // goto; remove the in-loop pair.
            size_t pre_goto = g.live_idx[pl];
            Insn ng, nw;
            ng.op = OP_get_loc;
            ng.aux = slot_s;
            ng.has_aux = true;
            ng.old_off = (*insns)[pre_goto].old_off;
            ng.old_size = 3;
            ng.pc_off = (*insns)[pre_goto].pc_off;
            ng.target = -1;
            ng.imm = 0;
            nw.op = OP_put_loc;
            nw.aux = slot_t;
            nw.has_aux = true;
            nw.old_off = (*insns)[pre_goto].old_off;
            nw.old_size = 3;
            nw.pc_off = (*insns)[pre_goto].pc_off;
            nw.target = -1;
            nw.imm = 0;
            insns->insert(insns->begin() + pre_goto, {ng, nw});
            dead->insert(dead->begin() + pre_goto, {0, 0});
            (*dead)[g.live_idx[pair_g]] = 1;
            (*dead)[g.live_idx[pair_w]] = 1;
            stats->folds_licm++;
            return true;  // one hoist per stage; later rounds hoist more
        }
    }
    return changed;
}

// P14': form-(b) literal folds across blocks. A linear desc sweep
// (P14's rules) records the description bound at each put_loc; the fold
// resolves a get_loc's version, requires a uniform desc across all its
// defining writes, and requires a kill-free cone from every def to the
// fold site.
static bool apply_ssi_desc_fold(std::vector<Insn>* insns,
                                std::vector<uint8_t>* dead,
                                uint32_t var_count,
                                const std::vector<uint8_t>& captured,
                                RewriteStats* stats) {
    SsiFn g;
    if (!build_ssi(*insns, *dead, var_count, captured, &g)) return false;
    std::vector<SDesc> desc_of_write(g.n);
    std::vector<SDesc> slot_desc(var_count);
    SDesc cur;
    cur.valid = false;
    auto clear_cur = [&]() {
        cur.valid = false;
        cur.fields.clear();
        cur.elements.clear();
    };
    auto clear_all = [&]() {
        for (size_t j = 0; j < slot_desc.size(); j++)
            slot_desc[j].valid = false;
        clear_cur();
    };
    auto find_field = [](SDesc* d, uint32_t atom) -> SVal* {
        for (size_t j = 0; j < d->fields.size(); j++)
            if (d->fields[j].first == atom) return &d->fields[j].second;
        return nullptr;
    };
    auto is_call = [](uint8_t op) {
        switch (op) {
        case OP_call: case OP_call1: case OP_call2: case OP_call3:
        case OP_tail_call: case OP_call_method: case OP_tail_call_method:
        case OP_call_constructor: case OP_apply: case OP_apply_eval:
        case OP_iterator_call:
            return true;
        default:
            return false;
        }
    };
    // Linear desc sweep (P14's rules; block-boundary crossings die at
    // the terminators' clear_cur, so a description recorded at a write
    // is exactly the straight-line construction path's).
    for (size_t li = 0; li < g.n; li++) {
        const Insn& in = (*insns)[g.live_idx[li]];
        if (is_slot_alias_barrier(in.op) || is_call(in.op) ||
            is_frame_opaque_op(in.op)) {
            clear_all();
            continue;
        }
        if (in.op == OP_object) {
            cur.fields.clear();
            cur.elements.clear();
            cur.is_array = false;
            cur.valid = true;
            continue;
        }
        if (in.op == OP_array_from) {
            uint32_t count = in.aux;
            std::vector<SVal> elems;
            elems.reserve(count);
            size_t p = li;
            bool ok = true;
            for (uint32_t k = 0; k < count; k++) {
                if (p == 0) {
                    ok = false;
                    break;
                }
                p--;
                SVal val;
                if (!ssi_read_const_push((*insns)[g.live_idx[p]], &val)) {
                    ok = false;
                    break;
                }
                elems.push_back(val);
            }
            if (!ok || elems.size() != count) {
                clear_cur();
                continue;
            }
            std::reverse(elems.begin(), elems.end());
            cur.elements = std::move(elems);
            cur.fields.clear();
            cur.is_array = true;
            cur.valid = true;
            continue;
        }
        if (in.op == OP_define_field) {
            if (!cur.valid) {
                clear_all();
                continue;
            }
            if (li > 0) {
                SVal val;
                if (ssi_read_const_push((*insns)[g.live_idx[li - 1]], &val)) {
                    SVal* f = find_field(&cur, in.aux);
                    if (f) {
                        *f = val;
                    } else {
                        cur.fields.push_back({in.aux, val});
                    }
                    continue;
                }
            }
            cur.valid = false;
            continue;
        }
        if (in.op == OP_put_field || in.op == OP_put_array_el ||
            in.op == OP_define_array_el || in.op == OP_append) {
            clear_all();
            continue;
        }
        if (in.op == OP_set_home_object || in.op == OP_set_name ||
            in.op == OP_set_name_computed) {
            clear_all();
            continue;
        }
        uint32_t sl;
        if (is_put_loc_op(in.op) && slot_of(in, &sl)) {
            if (cur.valid && !(sl < captured.size() && captured[sl])) {
                desc_of_write[li] = cur;
                slot_desc[sl] = cur;
            }
            clear_cur();
            continue;
        }
        if (is_set_loc_op(in.op) || is_slot_mut_op(in.op)) {
            if (slot_of(in, &sl)) slot_desc[sl].valid = false;
            continue;
        }
        if (in.op == OP_get_field || in.op == OP_get_array_el) {
            clear_cur();
            continue;
        }
        if (is_small_int_push(in.op) || in.op == OP_push_const ||
            in.op == OP_push_const8 || in.op == OP_push_atom_value) {
            continue;
        }
        clear_cur();
    }
    bool changed = false;
    auto uniform_desc = [&](size_t gp, const SDesc** d) -> bool {
        int32_t v = g.ver_def[gp];
        std::vector<int32_t> defs;
        std::vector<uint8_t> in_stack(g.ver_count, 0);
        collect_defs(g, v, &in_stack, &defs);
        if (defs.empty()) return false;
        const SDesc* found = nullptr;
        for (int32_t dd : defs) {
            if (dd >= static_cast<int32_t>(g.n)) return false;
            const SDesc& sd = desc_of_write[static_cast<size_t>(dd)];
            if (!sd.valid) return false;
            if (found && !ssi_same_desc(*found, sd)) return false;
            if (!found) found = &sd;
        }
        *d = found;
        return true;
    };
    for (size_t li = 0; li < g.n; li++) {
        const Insn& in = (*insns)[g.live_idx[li]];
        if (in.op == OP_get_field) {
            if (g.stk[li].empty()) continue;
            int32_t rcvr = g.stk[li].back();
            if (g.vals[rcvr].kind != 0 || g.vals[rcvr].b != 0) continue;
            size_t gp = static_cast<size_t>(g.vals[rcvr].a);
            if (gp >= li) continue;
            const Insn& gin = (*insns)[g.live_idx[gp]];
            uint32_t sl;
            if (!is_get_loc_op(gin.op) || gin.op == OP_get_loc0_loc1 ||
                !slot_of(gin, &sl) || sl >= captured.size() ||
                captured[sl])
                continue;
            const SDesc* d = nullptr;
            if (!uniform_desc(gp, &d) || d->is_array) continue;
            SVal* f = find_field(const_cast<SDesc*>(d), in.aux);
            if (!f) continue;
            bool cone_ok = true;
            for (size_t k = 0; k < g.n; k++) {
                if (g.slot_write[k] < 0) continue;
                if (g.ver_def[gp] != static_cast<int32_t>(k)) continue;
                // Only the defs of the read version are in the cone:
                // collect the write live positions via the defs list.
            }
            // Cone check over the version's defining writes.
            {
                int32_t v = g.ver_def[gp];
                std::vector<int32_t> defs;
                std::vector<uint8_t> in_stack(g.ver_count, 0);
                collect_defs(g, v, &in_stack, &defs);
                for (int32_t dd : defs) {
                    if (!ssi_cone_clean(g, *insns,
                                        static_cast<size_t>(g.block_of[dd]),
                                        static_cast<size_t>(g.block_of[li]))) {
                        cone_ok = false;
                        break;
                    }
                }
            }
            if (!cone_ok) continue;
            ssi_emit_val_push(insns, g.live_idx[gp], *f, in.old_off);
            (*dead)[g.live_idx[li]] = 1;
            stats->folds_p14s++;
            changed = true;
            continue;
        }
        if (in.op == OP_get_array_el) {
            if (g.stk[li].size() < 2) continue;
            int32_t idx = g.stk[li].back();
            int32_t rcvr = g.stk[li][g.stk[li].size() - 2];
            if (g.vals[idx].kind != 0 || g.vals[idx].b != 0) continue;
            size_t pp = static_cast<size_t>(g.vals[idx].a);
            const Insn& pin = (*insns)[g.live_idx[pp]];
            if (!is_small_int_push(pin.op)) continue;
            int64_t k = pin.imm;
            if (g.vals[rcvr].kind != 0 || g.vals[rcvr].b != 0) continue;
            size_t gp = static_cast<size_t>(g.vals[rcvr].a);
            if (gp >= li || gp >= pp) continue;
            const Insn& gin = (*insns)[g.live_idx[gp]];
            uint32_t sl;
            if (!is_get_loc_op(gin.op) || gin.op == OP_get_loc0_loc1 ||
                !slot_of(gin, &sl) || sl >= captured.size() ||
                captured[sl])
                continue;
            const SDesc* d = nullptr;
            if (!uniform_desc(gp, &d) || !d->is_array || k < 0 ||
                static_cast<uint64_t>(k) >= d->elements.size())
                continue;
            bool cone_ok = true;
            {
                int32_t v = g.ver_def[gp];
                std::vector<int32_t> defs;
                std::vector<uint8_t> in_stack(g.ver_count, 0);
                collect_defs(g, v, &in_stack, &defs);
                for (int32_t dd : defs) {
                    if (!ssi_cone_clean(g, *insns,
                                        static_cast<size_t>(g.block_of[dd]),
                                        static_cast<size_t>(g.block_of[li]))) {
                        cone_ok = false;
                        break;
                    }
                }
            }
            if (!cone_ok) continue;
            ssi_emit_val_push(insns, g.live_idx[gp], d->elements[k],
                              in.old_off);
            (*dead)[g.live_idx[pp]] = 1;
            (*dead)[g.live_idx[li]] = 1;
            stats->folds_p14s++;
            changed = true;
            continue;
        }
    }
    return changed;
}

// P15: slot-read CSE. Adjacent duplicate slot reads fold to a dup of
// the first read's value (stack effect preserved; the first read's
// value is on top). A landing on the second read would supply a
// different value, so targets there block the fold.
static bool apply_ssi_cse(std::vector<Insn>* insns,
                          std::vector<uint8_t>* dead,
                          uint32_t var_count,
                          const std::vector<uint8_t>& captured,
                          RewriteStats* stats) {
    SsiFn g;
    if (!build_ssi(*insns, *dead, var_count, captured, &g)) return false;
    std::vector<uint8_t> targets = compute_targets(*insns);
    bool changed = false;
    for (size_t li = 1; li < g.n; li++) {
        const Insn& in = (*insns)[g.live_idx[li]];
        if (!is_get_loc_op(in.op)) continue;
        if (targets[g.live_idx[li]]) continue;
        const Insn& p = (*insns)[g.live_idx[li - 1]];
        if (!is_get_loc_op(p.op) || p.op == OP_get_loc0_loc1) continue;
        uint32_t s1, s2;
        if (!slot_of(in, &s1) || !slot_of(p, &s2) || s1 != s2) continue;
        // Captured slot: a closure write between the two reads would
        // make the dup observe the wrong value.
        if (s1 >= captured.size() || captured[s1]) continue;
        Insn ni = (*insns)[g.live_idx[li]];
        ni.op = OP_dup;
        ni.target = -1;
        ni.imm = 0;
        ni.aux = 0;
        ni.has_aux = false;
        ni.old_size = static_cast<uint16_t>(short_opcode_info(OP_dup).size);
        (*insns)[g.live_idx[li]] = ni;
        stats->folds_gvn++;
        changed = true;
    }
    return changed;
}

// The suite driver (P10..P15, one rebuilt structure per stage).
bool apply_ssi_suite(std::vector<Insn>* insns, std::vector<uint8_t>* dead,
                     uint32_t var_count,
                     const std::vector<uint8_t>& captured,
                     uint32_t passes, RewriteStats* stats) {
    if (!(passes & kPassP9) || var_count == 0) return false;
    // Dynamic scope (with/eval) can reach loc slots through the scope
    // chain and invalidates exact versioning (P2's barrier precedent).
    for (size_t i = 0; i < insns->size(); i++) {
        if ((*dead)[i]) continue;
        switch ((*insns)[i].op) {
        case OP_eval: case OP_apply_eval:
        case OP_with_get_var: case OP_with_put_var:
        case OP_with_delete_var: case OP_with_make_ref:
        case OP_with_get_ref: case OP_with_get_ref_undef:
            return false;
        default:
            break;
        }
    }
    bool changed = false;
    if ((passes & kPassP10) &&
        apply_ssi_sccp(insns, dead, var_count, captured, stats))
        changed = true;
    if ((passes & kPassP11S) &&
        apply_ssi_copyprop(insns, dead, var_count, captured, stats))
        changed = true;
    if ((passes & kPassP12S) &&
        apply_ssi_dce(insns, dead, var_count, captured, stats))
        changed = true;
    if ((passes & kPassP13S) &&
        apply_ssi_licm(insns, dead, var_count, captured, stats))
        changed = true;
    if ((passes & kPassP14S) &&
        apply_ssi_desc_fold(insns, dead, var_count, captured, stats))
        changed = true;
    if ((passes & kPassP15S) &&
        apply_ssi_cse(insns, dead, var_count, captured, stats))
        changed = true;
    return changed;
}

// Tier-2 direct level combined driver (P11 then P14 per round).
bool apply_tier2_direct(std::vector<Insn>* insns, std::vector<uint8_t>* dead,
                        uint32_t var_count,
                        const std::vector<uint8_t>& captured,
                        uint32_t passes, RewriteStats* stats) {
    bool changed = false;
    if ((passes & kPassP11) &&
        apply_copyprop(insns, dead, var_count, captured, stats))
        changed = true;
    if ((passes & kPassP14) &&
        apply_lit_fold(insns, dead, var_count, captured, stats))
        changed = true;
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
// Slot index for a loc/arg access. Operand-less short forms carry the
// index in the opcode itself; every other loc/arg op carries it in aux.
// Never imm: imm holds push immediates (the decoder keeps the two
// operand spaces separate), and the P2 lattice must not alias slots.
static int32_t loc_index(const Insn& in) {
    const uint8_t op = in.op;
    if (op >= OP_get_loc0 && op <= OP_get_loc3) return op - OP_get_loc0;
    if (op >= OP_put_loc0 && op <= OP_put_loc3) return op - OP_put_loc0;
    if (op >= OP_set_loc0 && op <= OP_set_loc3) return op - OP_set_loc0;
    if (op >= OP_get_arg0 && op <= OP_get_arg3) return op - OP_get_arg0;
    if (op >= OP_put_arg0 && op <= OP_put_arg3) return op - OP_put_arg0;
    if (op >= OP_set_arg0 && op <= OP_set_arg3) return op - OP_set_arg0;
    return static_cast<int32_t>(in.aux);
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
    // Every block starts in the worklist: within-block folds (constant
    // chains after a put_loc) do not depend on the entry state, and a
    // block whose only incoming edge is a conditional jump in the middle
    // of another block never receives a propagated exit state.
    std::vector<P2Val> in_val(nb * var_count, kP2Unknown);
    std::vector<uint8_t> in_seen(nb, 0);  // entry got its first contribution
    std::vector<uint8_t> in_wl(nb, 0);
    std::vector<size_t> worklist;
    std::vector<P2Val> repl(n, kP2Unknown);  // pending get_loc replacements
    for (size_t b = 0; b < nb; b++) {
        in_wl[b] = 1;
        worklist.push_back(b);
    }

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
                int32_t s = loc_index(in);
                P2Val v = (s >= 0 && static_cast<size_t>(s) < var_count)
                              ? vals[static_cast<size_t>(s)]
                              : kP2Unknown;
                repl[i] = v;
                prev = kP2Unknown;
                top = v;
            } else if (is_loc_write(op)) {
                int32_t s = loc_index(in);
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
                int32_t s = loc_index(in);
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
            } else if (p2_op_barrier(op, loc_index(in), top,
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
            P2Val* entry = &in_val[static_cast<size_t>(sb) * var_count];
            bool changed = false;
            if (!in_seen[static_cast<size_t>(sb)]) {
                // First predecessor: adopt its exit state wholesale —
                // the join of the unseen (bottom) entry with anything is
                // that anything. Marked seen even when the contribution
                // is all-unknown, so a later, more precise predecessor
                // meets with it (absorbing to unknown) instead of
                // overwriting — p2_meet(unknown, x) == unknown is the
                // correct join for a block reachable on both paths.
                for (uint32_t v = 0; v < var_count; v++) {
                    if (p2_set(vals[v], &entry[v])) changed = true;
                }
                in_seen[static_cast<size_t>(sb)] = 1;
            } else {
                for (uint32_t v = 0; v < var_count; v++) {
                    if (p2_set(p2_meet(entry[v], vals[v]), &entry[v])) {
                        changed = true;
                    }
                }
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
        case OP_push_atom_value:
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

    // Tier-2 direct level (P11/P14), the SSI suite (P10..P15), then
    // P2/P3.1/P6 fixpoint: peephole sweeps until stable (each round
    // that changes anything deletes at least one instruction, so this
    // terminates), then shrink. The tier-2 passes run first so their
    // folds feed the v1 lattice; the SSI suite rebuilds its structure
    // per round from the current stream.
    for (int round = 0; round < 16; round++) {
        std::vector<uint8_t> dead(insns.size(), 0);
        bool round_changed = false;
        if ((passes & (kPassP11 | kPassP14)) &&
            apply_tier2_direct(&insns, &dead, f.var_count, f.captured,
                               passes, stats)) {
            round_changed = true;
        }
        if ((passes & kPassSSI) &&
            apply_ssi_suite(&insns, &dead, f.var_count, f.captured,
                            passes, stats)) {
            round_changed = true;
        }
        if ((passes & kPassP2) &&
            apply_crossbb(&insns, &dead, f.var_count, stats)) {
            round_changed = true;
        }
        if ((passes & kPassP31) &&
            apply_peepholes(&insns, &dead, passes, stats)) {
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
        if (report) {
            std::fprintf(stderr,
                         "bytecode optimize: %llu -> %llu insns, %llu -> "
                         "%llu code bytes; folds P2 %llu P3.1 %llu "
                         "P11 %llu P14 %llu "
                         "P10 %llu P11' %llu P12' %llu P13' %llu "
                         "P14' %llu P15 %llu, shrinks %llu\n",
                         static_cast<unsigned long long>(stats.insns_before),
                         static_cast<unsigned long long>(stats.insns_after),
                         static_cast<unsigned long long>(stats.bytes_before),
                         static_cast<unsigned long long>(stats.bytes_after),
                         static_cast<unsigned long long>(stats.folds_p2),
                         static_cast<unsigned long long>(stats.folds_p31),
                         static_cast<unsigned long long>(stats.folds_p11),
                         static_cast<unsigned long long>(stats.folds_p14),
                         static_cast<unsigned long long>(stats.folds_sccp),
                         static_cast<unsigned long long>(stats.folds_p11s),
                         static_cast<unsigned long long>(stats.folds_dce),
                         static_cast<unsigned long long>(stats.folds_licm),
                         static_cast<unsigned long long>(stats.folds_p14s),
                         static_cast<unsigned long long>(stats.folds_gvn),
                         static_cast<unsigned long long>(stats.shrinks));
        }
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
                     "code bytes; folds P2 %llu P3.1 %llu "
                     "P11 %llu P14 %llu "
                     "P10 %llu P11' %llu P12' %llu P13' %llu "
                     "P14' %llu P15 %llu, shrinks %llu\n",
                     static_cast<unsigned long long>(stats.insns_before),
                     static_cast<unsigned long long>(stats.insns_after),
                     static_cast<unsigned long long>(stats.bytes_before),
                     static_cast<unsigned long long>(stats.bytes_after),
                     static_cast<unsigned long long>(stats.folds_p2),
                     static_cast<unsigned long long>(stats.folds_p31),
                     static_cast<unsigned long long>(stats.folds_p11),
                     static_cast<unsigned long long>(stats.folds_p14),
                     static_cast<unsigned long long>(stats.folds_sccp),
                     static_cast<unsigned long long>(stats.folds_p11s),
                     static_cast<unsigned long long>(stats.folds_dce),
                     static_cast<unsigned long long>(stats.folds_licm),
                     static_cast<unsigned long long>(stats.folds_p14s),
                     static_cast<unsigned long long>(stats.folds_gvn),
                     static_cast<unsigned long long>(stats.shrinks));
    }
    return true;
}

}  // namespace bytecode
}  // namespace capsid
