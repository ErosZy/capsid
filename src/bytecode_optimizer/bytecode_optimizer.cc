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
// conditions; see PassFlags in bytecode_optimizer.h.
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
#include "bytecode_optimizer/bytecode_optimizer.h"

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
// the P3.1 + P6 part of the ceiling; see the final evidence in
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
    uint64_t folds_p16 = 0;   // P16: dead stores + TDZ markers removed
    uint64_t tdz_checks_removed = 0;  // tier-3 Lane 1: proven TDZ checks
                                      // rewritten to plain loc ops
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

// ---- P16: TDZ-sound dead store elimination (tier-2b) ----
// Removes stores to slots that are never read again on any path, and
// the set_loc_uninitialized TDZ markers that precede them. Sound
// because quickjs-ng stores the JS_UNINITIALIZED marker as the slot
// VALUE (get_loc_check throws when it reads the marker), so a store is
// deletable exactly when the slot is dead after it on every path:
// plain backward slot liveness, no special TDZ reasoning needed. The
// archived tier-2 P12' (commit 4465f36) never fired because its guard
// excluded every slot that held a marker write; this replaces that
// whole-slot guard with the precise liveness.
//
// Deletions are limited to:
//  - non-check put_loc-family stores (put_loc/put_loc8/put_loc0-3),
//    together with an immediately preceding side-effect-free pure-push
//    producer (push +1, put -1: net stack effect 0, so compaction's
//    jump-target redirect stays height-equivalent); check forms
//    (put_loc_check/put_loc_check_init) can throw on a marker and are
//    never touched;
//  - set_loc_uninitialized markers (net stack effect 0);
//  - captured slots are excluded (their closure can read them at any
//    time, invisible to this function's instruction stream).
//
// Liveness reads include put_loc_check/put_loc_check_init: they READ
// the slot's current value to decide whether to throw a TDZ error, so
// deleting the marker in front of one would change observable
// exceptions. (The tier-2b plan §5.1 listed only get_loc_check; the
// grep mandated by plan §5.1 turns up the check-store read, and
// fail-closed soundness requires it.)
//
// Gate: functions containing dynamic-scope or frame-aliasing constructs
// are skipped entirely — eval/with/apply_eval (dynamic name binding),
// OP_special_object (creates the `arguments` object or the var object,
// which capture the frame; quickjs-ng has no dedicated OP_arguments,
// it is emitted as OP_special_object), and the reference ops
// (make_loc_ref/make_arg_ref/make_var_ref/make_var_ref_ref and
// get_ref_value/put_ref_value), whose escaping references could alias
// a slot invisibly to this intra-function analysis. Deviation from the
// plan's gate list: the plan also gated on the get_arg family, but the
// arg ops address a separate frame store (arg_buf, with its own index
// space) and loc_index maps them to -1, so they can never alias a var
// slot; liveness over var slots covers them and no gate is needed.
// Slot universe is
// var_count (P2's lattice size), not the plan's stack_size: every
// existing slot pass bounds loc indices against var_count, and any
// out-of-range index is unanalyzable and therefore never deleted.
// Forward declarations: the loc/arg slot helpers live in the P2
// section below (they are shared with the v1 lattice passes).
static bool is_loc_read(uint8_t op);
static int32_t loc_index(const Insn& in);
static bool is_loc_write(uint8_t op);

// Value producers that may be deleted along with the store they feed:
// side-effect-free pure pushes only (small ints, cpool consts, atoms,
// undefined/null/false/true). Explicitly excluded: push_this (reads
// the frame's this slot) and any op that reads the stack or a slot
// (dup, get_loc, ...) — deleting such a producer would change the
// stack contents the surrounding code depends on.
static bool p16_is_pure_push(uint8_t op) {
    return is_small_int_push(op) || op == OP_push_const ||
           op == OP_push_const8 || op == OP_push_atom_value ||
           op == OP_undefined || op == OP_null || op == OP_push_false ||
           op == OP_push_true;
}

bool apply_dead_store_p16(std::vector<Insn>* insns, std::vector<uint8_t>* dead,
                          uint32_t var_count,
                          const std::vector<uint8_t>& captured,
                          RewriteStats* stats) {
    const size_t n = insns->size();
    if (n == 0 || var_count == 0) return false;

    for (size_t i = 0; i < n; i++) {
        uint8_t op = (*insns)[i].op;
        // The with_* ops are already inside is_slot_alias_barrier; the
        // tier-2b plan's OP_with_jump does not exist in this VM (the
        // with statement compiles to the with_get_var/... family).
        if (is_slot_alias_barrier(op) || op == OP_apply_eval ||
            op == OP_special_object || op == OP_make_loc_ref ||
            op == OP_make_arg_ref || op == OP_make_var_ref ||
            op == OP_make_var_ref_ref || op == OP_get_ref_value ||
            op == OP_put_ref_value) {
            return false;
        }
    }

    // Leaders and blocks: same construction as apply_crossbb (entry,
    // every jump target, post-gosub return points).
    std::vector<uint8_t> is_leader(n, 0);
    is_leader[0] = 1;
    for (size_t i = 0; i < n; i++) {
        const Insn& in = (*insns)[i];
        if (in.target >= 0) is_leader[static_cast<size_t>(in.target)] = 1;
        if (in.op == OP_gosub) {
            is_leader[next_live(*insns, *dead, i + 1)] = 1;
        }
    }
    std::vector<int32_t> block_id(n, -1);
    size_t nb = 0;
    for (size_t i = 0; i < n; i++) {
        if (is_leader[i]) block_id[i] = static_cast<int32_t>(nb++);
    }
    if (nb == 0) return false;
    for (size_t i = 1; i < n; i++) {
        if (block_id[i] < 0) block_id[i] = block_id[i - 1];
    }
    std::vector<size_t> bstart(nb), bend(nb);
    for (size_t b = 0; b < nb; b++) {
        size_t i = 0;
        while (i < n && block_id[i] != static_cast<int32_t>(b)) i++;
        bstart[b] = i;
        while (i < n && block_id[i] == static_cast<int32_t>(b)) i++;
        bend[b] = i;
    }
    // Successor edges from the block's last live instruction (a block
    // whose members are all tombstones from this round's earlier passes
    // is transparent: control flows through it linearly). A terminator
    // that falls through inside its own block cannot be the last live
    // instruction — its fallthrough insn would share the block id — so
    // the fallthrough edge always lands on the next block.
    auto last_live_in = [&](size_t b) -> size_t {
        size_t i = bend[b];
        while (i > bstart[b]) {
            i--;
            if (!(*dead)[i]) return i;
        }
        return n;  // sentinel: no live insns
    };
    std::vector<std::vector<size_t>> succ(nb), pred(nb);
    for (size_t b = 0; b < nb; b++) {
        size_t t = last_live_in(b);
        if (t != n) {
            uint8_t op = (*insns)[t].op;
            switch (op) {
            case OP_return: case OP_return_undef: case OP_return_async:
            case OP_throw: case OP_throw_error: case OP_ret:
            case OP_tail_call: case OP_tail_call_method:
                break;  // terminators: no successor
            case OP_goto: case OP_goto8: case OP_goto16:
                succ[b].push_back(
                    static_cast<size_t>(block_id[(*insns)[t].target]));
                break;
            case OP_if_true: case OP_if_false: case OP_if_true8:
            case OP_if_false8:
            case OP_catch:
            case OP_gosub:  // falls through to its return point
                succ[b].push_back(
                    static_cast<size_t>(block_id[(*insns)[t].target]));
                if (b + 1 < nb) succ[b].push_back(b + 1);
                break;
            default:
                if (b + 1 < nb) succ[b].push_back(b + 1);
                break;
            }
        } else if (b + 1 < nb) {
            succ[b].push_back(b + 1);
        }
        for (size_t s : succ[b]) pred[s].push_back(b);
    }
    // Mid-block conditional edges: a conditional that is not the block's
    // last live insn (its fall-through continues the block) still has a
    // real jump-taken edge to its target. These edges are absent from
    // succ[] — the seed must stay the fall-through state at the block
    // exit — but they join the pred[] graph so a later growth of the
    // target's live_in re-queues this block; the walks themselves merge
    // live_in[target] at the conditional site.
    for (size_t b = 0; b < nb; b++) {
        size_t t = last_live_in(b);
        if (t == n) continue;
        for (size_t i = bstart[b]; i < t; i++) {
            if ((*dead)[i]) continue;
            const Insn& in = (*insns)[i];
            uint8_t op = in.op;
            if ((op == OP_if_true || op == OP_if_false ||
                 op == OP_if_true8 || op == OP_if_false8 ||
                 op == OP_catch || op == OP_gosub) &&
                in.target >= 0) {
                pred[static_cast<size_t>(block_id[in.target])]
                    .push_back(b);
            }
        }
    }

    // Backward slot liveness, block worklist fixpoint. Live sets only
    // grow (union transfer), so iteration terminates; when a block's
    // entry set grows, its predecessors must be revisited.
    // read (gen): loc reads incl. get_loc_check and the arg family,
    //   put_loc_check/put_loc_check_init (they read the slot to test
    //   the TDZ marker), in-place inc/dec/add_loc and close_loc
    //   (read+write; the close moves the value out).
    // write (kill): all loc/arg stores, set_loc_uninitialized, and the
    //   in-place mutators.
    // Ops outside these sets (var_ref family, stack ops, calls, ...)
    // never touch a loc slot: the var_ref family addresses the closure
    // environment (captured slots only, which are excluded from
    // deletion anyway), so they need no liveness effect.
    std::vector<uint8_t> live_in(nb * var_count, 0);
    std::vector<uint8_t> in_wl(nb, 0);
    std::vector<size_t> worklist;
    for (size_t b = 0; b < nb; b++) {
        in_wl[b] = 1;
        worklist.push_back(b);
    }
    while (!worklist.empty()) {
        size_t b = worklist.back();
        worklist.pop_back();
        in_wl[b] = 0;
        std::vector<uint8_t> live(var_count, 0);
        for (size_t s : succ[b]) {
            const uint8_t* p = &live_in[s * var_count];
            for (uint32_t k = 0; k < var_count; k++) live[k] |= p[k];
        }
        size_t last = last_live_in(b);
        for (size_t i = bend[b]; i > bstart[b];) {
            i--;
            if ((*dead)[i]) continue;
            const Insn& in = (*insns)[i];
            uint8_t op = in.op;
            if (i < last &&
                (op == OP_if_true || op == OP_if_false ||
                 op == OP_if_true8 || op == OP_if_false8 ||
                 op == OP_catch || op == OP_gosub) &&
                in.target >= 0) {
                // Mid-block conditional: its jump-taken edge reaches the
                // target with the live state at the target's entry, which
                // the linear walk never passes through (the fall-through
                // path can kill/overwrite the slots the target reads, as
                // in `if (c) { x = 1; }` — the true-path store kills x
                // for the false-path read of the marker). Merge the
                // target's entry liveness. Conservative: merging only
                // adds live bits, so a decision can only flip from
                // delete to keep.
                const uint8_t* p =
                    &live_in[static_cast<size_t>(block_id[in.target]) *
                             var_count];
                for (uint32_t k = 0; k < var_count; k++) live[k] |= p[k];
            }
            if (op == OP_get_loc0_loc1) {
                // Fused read of slots 0 and 1.
                if (var_count > 0) live[0] = 1;
                if (var_count > 1) live[1] = 1;
                continue;
            }
            bool reads = is_loc_read(op) || op == OP_put_loc_check ||
                         op == OP_put_loc_check_init ||
                         op == OP_inc_loc || op == OP_dec_loc ||
                         op == OP_add_loc || op == OP_close_loc;
            bool writes = is_loc_write(op) ||
                          op == OP_set_loc_uninitialized ||
                          op == OP_inc_loc || op == OP_dec_loc ||
                          op == OP_add_loc || op == OP_close_loc;
            if (reads || writes) {
                int32_t s = loc_index(in);
                // Out-of-range slot: unanalyzable. It is never deleted
                // (the deletion guards re-check the bound) and cannot
                // alias a tracked slot, so it has no liveness effect.
                if (s >= 0 && static_cast<uint32_t>(s) < var_count) {
                    // Read+write ops (put_loc_check, inc/dec/add_loc,
                    // close_loc) read the slot before overwriting it:
                    // the write kills earlier producers, but the read
                    // still makes the slot live in, so the gen must
                    // win (write first, then read).
                    if (writes) live[static_cast<size_t>(s)] = 0;
                    if (reads) live[static_cast<size_t>(s)] = 1;
                }
            }
        }
        uint8_t* out = &live_in[b * var_count];
        bool grew = false;
        for (uint32_t k = 0; k < var_count; k++) {
            if (live[k] && !out[k]) {
                out[k] = 1;
                grew = true;
            }
        }
        if (grew) {
            for (size_t p : pred[b]) {
                if (!in_wl[p]) {
                    in_wl[p] = 1;
                    worklist.push_back(p);
                }
            }
        }
    }

    // Second phase: replay the backward transfer once per block to
    // record the live-out at every decision point (store or marker),
    // then decide deletions in a forward sweep. Deletion decisions
    // never perturb the liveness they were derived from: live-out is
    // computed on the original (plus this round's earlier tombstones)
    // stream. A store is removed only together with its pure-push
    // producer; a marker is removed alone. Both rules are independent:
    // the arith-rt shape (marker runs plus separate push/put pairs) is
    // covered either way, and a read in between keeps whatever it
    // reads.
    struct Decision {
        size_t insn;         // store or marker index
        uint8_t is_marker;
        uint8_t s;           // slot index (guarded < var_count)
        uint8_t dead_slot;   // slot not live after this insn
    };
    std::vector<Decision> decisions;
    {
        std::vector<uint8_t> live(var_count, 0);
        for (size_t b = 0; b < nb; b++) {
            std::fill(live.begin(), live.end(), 0);
            for (size_t s : succ[b]) {
                const uint8_t* p = &live_in[s * var_count];
                for (uint32_t k = 0; k < var_count; k++) live[k] |= p[k];
            }
            size_t last = last_live_in(b);
            for (size_t i = bend[b]; i > bstart[b];) {
                i--;
                if ((*dead)[i]) continue;
                const Insn& in = (*insns)[i];
                uint8_t op = in.op;
                if (i < last &&
                    (op == OP_if_true || op == OP_if_false ||
                     op == OP_if_true8 || op == OP_if_false8 ||
                     op == OP_catch || op == OP_gosub) &&
                    in.target >= 0) {
                    // Same mid-block merge as the fixpoint walk, against
                    // the converged live_in (the replay makes its keep/
                    // delete decisions on this walk's live sets).
                    const uint8_t* p =
                        &live_in[static_cast<size_t>(block_id[in.target]) *
                                 var_count];
                    for (uint32_t k = 0; k < var_count; k++)
                        live[k] |= p[k];
                }
                if (op == OP_put_loc || op == OP_put_loc8 ||
                    (op >= OP_put_loc0 && op <= OP_put_loc3) ||
                    op == OP_set_loc_uninitialized) {
                    int32_t s = loc_index(in);
                    if (s >= 0 && static_cast<uint32_t>(s) < var_count) {
                        decisions.push_back(
                            {i, static_cast<uint8_t>(
                                    op == OP_set_loc_uninitialized ? 1 : 0),
                             static_cast<uint8_t>(s),
                             static_cast<uint8_t>(
                                 live[static_cast<size_t>(s)] ? 0 : 1)});
                    }
                }
                if (op == OP_get_loc0_loc1) {
                    if (var_count > 0) live[0] = 1;
                    if (var_count > 1) live[1] = 1;
                    continue;
                }
                bool reads = is_loc_read(op) || op == OP_put_loc_check ||
                             op == OP_put_loc_check_init ||
                             op == OP_inc_loc || op == OP_dec_loc ||
                             op == OP_add_loc || op == OP_close_loc;
                bool writes = is_loc_write(op) ||
                              op == OP_set_loc_uninitialized ||
                              op == OP_inc_loc || op == OP_dec_loc ||
                              op == OP_add_loc || op == OP_close_loc;
                if (reads || writes) {
                    int32_t s = loc_index(in);
                    if (s >= 0 && static_cast<uint32_t>(s) < var_count) {
                        // Same order as the fixpoint transfer: the gen
                        // of a read+write op wins over its kill.
                        if (writes) live[static_cast<size_t>(s)] = 0;
                        if (reads) live[static_cast<size_t>(s)] = 1;
                    }
                }
            }
        }
    }
    // Forward sweep: apply the recorded decisions.
    bool changed = false;
    for (const Decision& d : decisions) {
        if ((*dead)[d.insn]) continue;
        if (!d.dead_slot) continue;
        if (d.s >= captured.size() || captured[d.s]) continue;
        if (d.is_marker) {
            (*dead)[d.insn] = 1;
            stats->folds_p16 += 1;
            changed = true;
            continue;
        }
        // Store: deletable only together with a preceding pure push.
        size_t prev = d.insn;
        while (prev > 0) {
            prev--;
            if (!(*dead)[prev]) break;
        }
        if (prev < d.insn && p16_is_pure_push((*insns)[prev].op)) {
            (*dead)[prev] = 1;
            (*dead)[d.insn] = 1;
            stats->folds_p16 += 2;
            changed = true;
        }
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
// Slot index for a loc access, or -1 for the arg family. The VM frame
// keeps arguments and locals in separate stores with separate index
// spaces (arg_buf vs var_buf = local_buf + arg_allocated_size), so an
// arg index must never alias a var slot: every consumer (P2's lattice,
// P16's liveness, tier-3's slot-init states) guards s >= 0 and treats
// -1 as "no effect". Args are call-initialized and opaque to the
// intra-function analysis, so untracking them loses no folds.
// Operand-less short forms carry the index in the opcode itself; every
// other loc op carries it in aux. Never imm: imm holds push immediates
// (the decoder keeps the two operand spaces separate), and the P2
// lattice must not alias slots.
static int32_t loc_index(const Insn& in) {
    const uint8_t op = in.op;
    if (op >= OP_get_loc0 && op <= OP_get_loc3) return op - OP_get_loc0;
    if (op >= OP_put_loc0 && op <= OP_put_loc3) return op - OP_put_loc0;
    if (op >= OP_set_loc0 && op <= OP_set_loc3) return op - OP_set_loc0;
    if (op >= OP_get_arg0 && op <= OP_get_arg3) return -1;
    if (op >= OP_put_arg0 && op <= OP_put_arg3) return -1;
    if (op >= OP_set_arg0 && op <= OP_set_arg3) return -1;
    if (op == OP_get_arg || op == OP_put_arg || op == OP_set_arg) return -1;
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
    // Last live insn per block: a conditional jump whose fall-through
    // continues the same block is mid-block, yet its jump-taken edge is
    // real — the target is reached with the slot state AT the jump, not
    // with the block-exit state the propagation below attaches to the
    // last live insn. Dropping that edge lets a join block adopt the
    // fall-through exit state wholesale and fold reads on state that
    // never reached the join on the jump-taken path (`if (c) { x = 1; }
    // y = x + 1;` folded y on the false path, where x kept its old
    // value). Soundness fix: snapshot the state at every mid-block
    // conditional and propagate it to the target alongside the exit
    // edges.
    std::vector<size_t> block_last(nb, 0);
    for (size_t b = 0; b < nb; b++) {
        for (size_t i = bend[b]; i-- > bstart[b];) {
            if (!(*dead)[i]) { block_last[b] = i; break; }
        }
    }
    // One snapshot per mid-block conditional in the current block:
    // (jump target, slot state at the jump).
    struct MidJump {
        int32_t target;
        std::vector<P2Val> state;
    };

    while (!worklist.empty()) {
        size_t b = worklist.back();
        worklist.pop_back();
        in_wl[b] = 0;
        std::vector<P2Val> vals(in_val.begin() + b * var_count,
                                in_val.begin() + (b + 1) * var_count);
        std::vector<MidJump> mid_jumps;
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
            } else if (op == OP_if_true || op == OP_if_false ||
                       op == OP_if_true8 || op == OP_if_false8 ||
                       op == OP_catch || op == OP_gosub) {
                // Pure conditional: no barrier, no stack lattice. If the
                // jump is not the block's last live insn, its jump-taken
                // edge reaches the target with the state at this point;
                // the exit propagation below covers only the last live
                // insn, so snapshot the edge for the propagation phase.
                if (i < block_last[b]) {
                    mid_jumps.push_back(
                        {in.target, std::vector<P2Val>(vals)});
                }
                prev = kP2Unknown;
                top = kP2Unknown;
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
        // One edge contribution: meet `st` into the target block's entry
        // lattice and re-queue it when the entry changed.
        auto contribute = [&](int32_t s, const std::vector<P2Val>& st) {
            if (s < 0 || static_cast<size_t>(s) >= n) return;
            int32_t sb = block_id[static_cast<size_t>(s)];
            if (sb < 0) return;
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
                    if (p2_set(st[v], &entry[v])) changed = true;
                }
                in_seen[static_cast<size_t>(sb)] = 1;
            } else {
                for (uint32_t v = 0; v < var_count; v++) {
                    if (p2_set(p2_meet(entry[v], st[v]), &entry[v])) {
                        changed = true;
                    }
                }
            }
            if (changed && !in_wl[static_cast<size_t>(sb)]) {
                in_wl[static_cast<size_t>(sb)] = 1;
                worklist.push_back(static_cast<size_t>(sb));
            }
        };
        // Mid-block conditional jump-taken edges first: the target is
        // reached with the state at the jump, not with the block-exit
        // state (the unsound fold above would otherwise happen).
        for (const MidJump& mj : mid_jumps) {
            contribute(mj.target, mj.state);
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
            contribute(succs[si], vals);
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

// Defined in the tier-3 section below; run after the fixpoint converges
// so the proof reflects the exact stream being emitted. Captured slots
// need no special casing: the emitter never emits loc ops for them (it
// uses var_ref forms), so they can never be proven INIT in this stream.
static bool tier3_apply_lane1(std::vector<Insn>* insns, uint32_t var_count,
                              RewriteStats* stats);

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

    // Tier-2 direct level (P11/P14), then P2/P3.1/P6 fixpoint:
    // peephole sweeps until stable (each round that changes anything
    // deletes at least one instruction, so this terminates), then
    // shrink. The tier-2 passes run first so their folds feed the v1
    // lattice.
    for (int round = 0; round < 16; round++) {
        std::vector<uint8_t> dead(insns.size(), 0);
        bool round_changed = false;
        if ((passes & (kPassP11 | kPassP14)) &&
            apply_tier2_direct(&insns, &dead, f.var_count, f.captured,
                               passes, stats)) {
            round_changed = true;
        }
        // P16 (tier-2b): TDZ-sound dead store elimination. Runs after
        // the tier-2 direct passes so their dead-store materializations
        // are already visible; it only deletes instructions, so it
        // cannot feed the lattice and the fixpoint still terminates.
        if ((passes & kPassP16) &&
            apply_dead_store_p16(&insns, &dead, f.var_count, f.captured,
                                 stats)) {
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
    // Tier-3 P18 (Lane 1): provably-safe TDZ check elimination, run
    // after the fixpoint (the proof reflects the emitted stream) and
    // before the final reshrink (emitted plain ops shorten as usual).
    if ((passes & kPassTier3Lane1) &&
        tier3_apply_lane1(&insns, f.var_count, stats)) {
        // fall through; reshrink + emit below handle the rewritten ops
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

// ---------------------------------------------------------------------------
// A3 (tier-3 plan §4): analyze-only density proofs for the two candidates
// selected by the A2 ranking (docs/quickjs-ng-opcode-optimization.md §3.4a).
// Emits nothing — this is the go/no-go density measurement.
//
//   (a) TDZ-check elimination: get_loc_check / put_loc_check whose slot is
//       provably initialized on every reaching path degrade to
//       get_loc / put_loc. Existing opcodes, zero format change.
//   (b) get_array_el specialization: sites whose object operand is a
//       provable array (array_from construction) and whose index operand is
//       a provable int could skip the generic JS_GetPropertyValue path.
//
// Both analyses are conservative and state-free. Functions containing a
// dynamic environment or an exception/finally edge (with_*, eval,
// catch/gosub/ret/nip_catch) are skipped — the exception-edge model is
// not built — and reported separately so the density number is not
// polluted by unknown CFG shapes. Everywhere the model under-approximates
// the CFG it undercounts, never overcounts: missing edges only suppress
// reducibility claims, and the monotone meet absorbs any contribution
// computed from a weaker-than-true entry state.
// ---------------------------------------------------------------------------

struct Tier3Stats {
    uint64_t funcs = 0;
    uint64_t funcs_skipped = 0;
    uint64_t get_loc_check = 0;
    uint64_t get_loc_check_red = 0;
    uint64_t put_loc_check = 0;
    uint64_t put_loc_check_red = 0;
    uint64_t get_array_el = 0;
    uint64_t get_array_el_red = 0;  // obj provably array AND idx provably int
    uint64_t get_array_el_arr = 0;  // obj provably array, idx not proven
};

// Slot initialization states for candidate (a). Meet: equal states meet to
// themselves, UNINIT ^ INIT = MAYBE. A slot whose state is MAYBE on any
// reaching path is never claimed reducible.
enum SlotInit { SI_UNINIT = 0, SI_INIT = 1, SI_MAYBE = 2 };
static uint8_t slot_meet(uint8_t a, uint8_t b) {
    if (a == b) return a;
    return SI_MAYBE;
}

// Stack value classes for candidate (b). Meet: equal classes meet to
// themselves, anything else is UNKNOWN. The class proves "is an array" /
// "is an int32", never identity: array mutation does not change the
// class, only a write of a different-class value does.
enum StackClass { SC_UNKNOWN = 0, SC_INT = 1, SC_ARRAY = 2 };
static uint8_t stack_meet(uint8_t a, uint8_t b) {
    if (a == b) return a;
    return SC_UNKNOWN;
}

// Exception/finally edges and dynamic scope are not modeled: a function
// containing any of these ops is skipped by both analyses (undercount,
// never unsound). with_* ops can re-bind any local name; direct eval can
// read/write the whole frame; catch/gosub/ret/nip_catch create edges that
// are not part of the explicit CFG.
static bool tier3_has_unmodeled_cfg(const std::vector<Insn>& insns) {
    for (size_t i = 0; i < insns.size(); i++) {
        uint8_t op = insns[i].op;
        if (is_with_jump(op) || op == OP_eval || op == OP_apply_eval ||
            op == OP_catch || op == OP_gosub || op == OP_ret ||
            op == OP_nip_catch) {
            return true;
        }
    }
    return false;
}

// True for ops that transfer control without a fall-through: the block
// ends there, and the next block is entered only via the op's own target
// (for goto) or not at all (return/throw). Same set as the leader rule.
static bool tier3_is_terminator(uint8_t op) {
    switch (op) {
    case OP_goto: case OP_goto8: case OP_goto16:
    case OP_return: case OP_return_undef: case OP_return_async:
    case OP_throw: case OP_throw_error: case OP_ret:
    case OP_tail_call: case OP_tail_call_method:
        return true;
    default:
        return false;
    }
}

// Edges of a block's last insn (P2's model; fall-through is the next
// block). with_*/catch/gosub never appear here — their functions were
// skipped by the unmodeled-CFG gate.
static void tier3_succs(const Insn& l,
                        size_t fallthrough,
                        int32_t* succs,
                        int* nsucc) {
    switch (l.op) {
    case OP_tail_call: case OP_tail_call_method:
    case OP_return: case OP_return_undef: case OP_return_async:
    case OP_throw: case OP_throw_error: case OP_ret:
        break;
    case OP_goto: case OP_goto8: case OP_goto16:
        succs[(*nsucc)++] = l.target;
        break;
    case OP_if_true: case OP_if_false:
    case OP_if_true8: case OP_if_false8:
    case OP_catch: case OP_gosub:
        succs[(*nsucc)++] = l.target;
        succs[(*nsucc)++] = static_cast<int32_t>(fallthrough);
        break;
    default:
        succs[(*nsucc)++] = static_cast<int32_t>(fallthrough);
        break;
    }
}

// CFG construction: leaders are the entry, every jump target, and every
// position after an unconditional control transfer (goto/return/throw/
// ret/tail_call). Code between an unconditional transfer and the next
// leader is unreachable; carving it into its own block keeps its writes
// out of the live path's state — its contribution is still met into the
// next block, but the real-path contribution (via the transfer's own
// edge) is always there too, so the meet absorbs the dead claims.
static void tier3_cfg(const std::vector<Insn>& insns,
                      std::vector<int32_t>* block_id,
                      std::vector<size_t>* bstart,
                      std::vector<size_t>* bend,
                      std::vector<uint8_t>* reachable) {
    const size_t n = insns.size();
    std::vector<uint8_t> is_leader(n, 0);
    is_leader[0] = 1;
    for (size_t i = 0; i < n; i++) {
        if (insns[i].target >= 0) {
            is_leader[static_cast<size_t>(insns[i].target)] = 1;
        }
        switch (insns[i].op) {
        case OP_goto: case OP_goto8: case OP_goto16:
        case OP_return: case OP_return_undef: case OP_return_async:
        case OP_throw: case OP_throw_error: case OP_ret:
        case OP_tail_call: case OP_tail_call_method:
            if (i + 1 < n) is_leader[i + 1] = 1;
            break;
        default:
            break;
        }
    }
    std::vector<int32_t> bid(n, -1);
    size_t nb = 0;
    for (size_t i = 0; i < n; i++) {
        if (is_leader[i]) bid[i] = static_cast<int32_t>(nb++);
    }
    for (size_t i = 1; i < n; i++) {
        if (bid[i] < 0) bid[i] = bid[i - 1];
    }
    std::vector<size_t> bs(nb), be(nb);
    for (size_t b = 0; b < nb; b++) {
        size_t i = 0;
        while (i < n && bid[i] != static_cast<int32_t>(b)) i++;
        bs[b] = i;
        while (i < n && bid[i] == static_cast<int32_t>(b)) i++;
        be[b] = i;
    }
    // Reachability: entry block plus the closure over all jump targets
    // inside each block and the last-insn edges. Mid-block conditional
    // targets (short-circuit `&&`/`||` chains) are blocks too; the
    // analysis propagates per-jump edge states, so the closure must
    // include them or their sites would silently drop from the counts.
    std::vector<uint8_t> vis(nb, 0);
    std::vector<size_t> wl;
    vis[0] = 1;
    wl.push_back(0);
    while (!wl.empty()) {
        size_t b = wl.back();
        wl.pop_back();
        auto mark = [&](int32_t s) {
            if (s < 0 || static_cast<size_t>(s) >= n) return;
            int32_t sb = bid[static_cast<size_t>(s)];
            if (sb < 0 || vis[static_cast<size_t>(sb)]) return;
            vis[static_cast<size_t>(sb)] = 1;
            wl.push_back(static_cast<size_t>(sb));
        };
        for (size_t i = bs[b]; i < be[b]; i++) {
            if (insns[i].target >= 0) mark(insns[i].target);
        }
        int32_t succs[2];
        int nsucc = 0;
        tier3_succs(insns[be[b] - 1], be[b], succs, &nsucc);
        for (int si = 0; si < nsucc; si++) mark(succs[si]);
    }
    *block_id = std::move(bid);
    *bstart = std::move(bs);
    *bend = std::move(be);
    *reachable = std::move(vis);
}

// One candidate-(a) site in the original instruction stream. Phase 2 of
// the slot-init analysis records every get_loc_check/put_loc_check with
// the verdict its converged entry state gives; P18 rewrites exactly the
// reducible ones, in stream order, so indices stay valid.
struct Tier3Site {
    size_t idx;
    uint8_t op;  // OP_get_loc_check or OP_put_loc_check
    bool reducible;
};

// Candidate (a): forward slot-initialization lattice over the CFG.
// Transfer is monotone (UNINIT -> INIT via any write; only
// set_loc_uninitialized regresses), so a worklist sweep to fixpoint
// terminates; per-block entry states meet predecessor exit states.
// Unlike the P2 value lattice, user code can never reset a slot's
// initialization state: closures write through var_ref storage, not this
// function's loc slots, and a call cannot resurrect a TDZ marker.
// `st` counts sites (analyze-only reporting); `sites`, when non-null,
// additionally records every site's verdict for P18 emission. The
// rewritten plain ops have no transfer in this lattice, so one sweep of
// verdicts from the original stream is sound: no verdict depends on any
// other site's rewrite.
static void tier3_slotinit(const std::vector<Insn>& insns,
                           uint32_t var_count,
                           Tier3Stats* st,
                           std::vector<Tier3Site>* sites = nullptr) {
    const size_t n = insns.size();
    if (n == 0 || var_count == 0) return;

    std::vector<int32_t> block_id;
    std::vector<size_t> bstart, bend;
    std::vector<uint8_t> reachable;
    tier3_cfg(insns, &block_id, &bstart, &bend, &reachable);
    const size_t nb = bstart.size();
    if (nb == 0) return;

    // Per-block entry lattice; meet-only transfer, worklist fixpoint.
    // Phase 1 propagates states to the fixpoint without counting; phase 2
    // sweeps each reachable block once against the converged entries, so
    // every site is counted exactly once and with the final (most
    // conservative) entry. Only the entry block starts in the worklist —
    // every other block is processed only after a real predecessor
    // contribution (its first contribution is copied wholesale, later
    // ones meet; the lattice has no top, so the first real path state is
    // adopted in full). Placeholder-derived exits are never computed, so
    // they can never weaken a successor through the meet: the fixpoint is
    // the meet over all modeled paths, with no floor artifact.
    std::vector<uint8_t> in_state(nb * var_count, SI_MAYBE);
    std::vector<uint8_t> in_seen(nb, 0);
    std::vector<uint8_t> in_wl(nb, 0);
    std::vector<size_t> worklist;
    in_seen[0] = 1;  // the entry block's initial state is its real state
    in_wl[0] = 1;
    worklist.push_back(0);

    // The entry block's initial state is genuinely all-MAYBE (var/arg
    // slots are initialized, let/const slots are UNINIT until their
    // set_loc_uninitialized; the two are indistinguishable here, and
    // MAYBE is the sound join).
    auto sweep = [&](size_t b, std::vector<uint8_t>& state,
                     Tier3Stats* cnt,
                     std::vector<std::pair<size_t, std::vector<uint8_t>>>* edges) {
        for (size_t i = bstart[b]; i < bend[b]; i++) {
            const Insn& in = insns[i];
            uint8_t op = in.op;
            if (op == OP_set_loc_uninitialized) {
                int32_t s = loc_index(in);
                if (s >= 0 && static_cast<size_t>(s) < var_count) {
                    state[static_cast<size_t>(s)] = SI_UNINIT;
                }
            } else if (op == OP_inc_loc || op == OP_dec_loc ||
                       op == OP_add_loc) {
                // In-place slot arithmetic is only ever peepholed from
                // plain get_loc/put_loc patterns (quickjs.c emit_peephole),
                // i.e. for var slots — never for TDZ-checked let/const —
                // so the slot is initialized afterwards.
                int32_t s = loc_index(in);
                if (s >= 0 && static_cast<size_t>(s) < var_count) {
                    state[static_cast<size_t>(s)] = SI_INIT;
                }
            } else if (op == OP_put_loc_check) {
                // Read+write with TDZ check: count the site, and the write
                // initializes the slot regardless of the verdict.
                int32_t s = loc_index(in);
                bool red = s >= 0 && static_cast<size_t>(s) < var_count &&
                           state[static_cast<size_t>(s)] == SI_INIT;
                if (cnt) {
                    cnt->put_loc_check++;
                    if (red) cnt->put_loc_check_red++;
                }
                if (sites) sites->push_back({i, OP_put_loc_check, red});
                if (s >= 0 && static_cast<size_t>(s) < var_count) {
                    state[static_cast<size_t>(s)] = SI_INIT;
                }
            } else if (op == OP_put_loc_check_init || is_loc_write(op)) {
                // Any write initializes. put_loc_check_init marks the
                // let/const binding's first store (never a check site);
                // put_loc/set_loc and the arg forms write var/param slots
                // that are already initialized — both leave the slot INIT.
                int32_t s = loc_index(in);
                if (s >= 0 && static_cast<size_t>(s) < var_count) {
                    state[static_cast<size_t>(s)] = SI_INIT;
                }
            } else if (op == OP_get_loc_check) {
                int32_t s = loc_index(in);
                bool red = s >= 0 && static_cast<size_t>(s) < var_count &&
                           state[static_cast<size_t>(s)] == SI_INIT;
                if (cnt) {
                    cnt->get_loc_check++;
                    if (red) cnt->get_loc_check_red++;
                }
                if (sites) sites->push_back({i, OP_get_loc_check, red});
            }
            if (edges && in.target >= 0) {
                // Jump edge: carry the state as of the jump itself, so a
                // mid-block conditional (e.g. an assignment inside an
                // `&&` chain) cannot smuggle the block-end state into
                // its target and overclaim on the merge below.
                int32_t tb = block_id[static_cast<size_t>(in.target)];
                if (tb >= 0 && reachable[static_cast<size_t>(tb)]) {
                    edges->push_back({static_cast<size_t>(tb), state});
                }
            }
        }
        if (edges && !tier3_is_terminator(insns[bend[b] - 1].op) &&
            bend[b] < n) {
            // Fall-through edge: the state at the block's last insn.
            int32_t tb = block_id[bend[b]];
            if (tb >= 0 && reachable[static_cast<size_t>(tb)]) {
                edges->push_back({static_cast<size_t>(tb), state});
            }
        }
    };

    while (!worklist.empty()) {
        size_t b = worklist.back();
        worklist.pop_back();
        in_wl[b] = 0;
        if (!reachable[b]) continue;  // never entered: no counting, no state
        std::vector<uint8_t> state(in_state.begin() + b * var_count,
                                   in_state.begin() + (b + 1) * var_count);
        std::vector<std::pair<size_t, std::vector<uint8_t>>> edges;
        sweep(b, state, nullptr, &edges);

        // Propagate along every jump edge (snapshot at the jump) and the
        // last-insn fall-through.
        for (auto& e : edges) {
            size_t sb = e.first;
            uint8_t* dst = &in_state[sb * var_count];
            const uint8_t* src = e.second.data();
            if (!in_seen[sb]) {
                // First predecessor: adopt its exit state wholesale.
                std::memcpy(dst, src, var_count);
                in_seen[sb] = 1;
                in_wl[sb] = 1;
                worklist.push_back(sb);
            } else {
                bool ch = false;
                for (size_t j = 0; j < var_count; j++) {
                    uint8_t m = slot_meet(dst[j], src[j]);
                    if (m != dst[j]) {
                        dst[j] = m;
                        ch = true;
                    }
                }
                if (ch && !in_wl[sb]) {
                    in_wl[sb] = 1;
                    worklist.push_back(sb);
                }
            }
        }
    }

    // Phase 2: count each site exactly once against the converged entries.
    for (size_t b = 0; b < nb; b++) {
        if (!reachable[b]) continue;
        std::vector<uint8_t> state(in_state.begin() + b * var_count,
                                   in_state.begin() + (b + 1) * var_count);
        sweep(b, state, st, nullptr);
    }
}

// P18 Lane 1 emission, candidate (a): rewrite every provably-safe TDZ
// check site to the plain op. The proof runs on the exact stream the
// optimizer is about to emit (post-fixpoint), so its verdicts reflect
// the code that actually runs; the rewritten ops have no transfer in the
// slot-init lattice, so no verdict depends on another site's rewrite and
// one sweep is sound. Same-width opcode change (op byte only), so jump
// targets and pc2line PCs are untouched here; the caller's final
// reshrink converts get_loc/put_loc to their short forms as usual.
// Returns true if any site was rewritten.
// Lives in the anonymous namespace (matching its forward declaration
// next to rewrite_function): the P18 hook is called from inside the
// optimizer's internal namespace while the tier-3 section below sits at
// bytecode-namespace scope.
namespace {
static bool tier3_apply_lane1(std::vector<Insn>* insns,
                              uint32_t var_count,
                              RewriteStats* stats) {
    if (var_count == 0) return false;
    if (tier3_has_unmodeled_cfg(*insns)) return false;
    std::vector<Tier3Site> sites;
    tier3_slotinit(*insns, var_count, nullptr, &sites);
    bool changed = false;
    for (size_t i = 0; i < sites.size(); i++) {
        const Tier3Site& s = sites[i];
        if (!s.reducible) continue;
        Insn& in = (*insns)[s.idx];
        if (in.op != s.op) continue;  // safety: index/op agreement
        in.op = (s.op == OP_get_loc_check) ? OP_get_loc : OP_put_loc;
        changed = true;
        stats->tdz_checks_removed++;
    }
    return changed;
}
}  // namespace

// Candidate (b): forward stack lattice tracking provable arrays and
// provable ints. The stack abstraction is the top-2 classes (top = the
// value that would be popped first, prev = the one below), exactly P2's
// top/prev level of precision: get_array_el pops its object and index
// operands, whichever order they were pushed in, and the transfer resets
// top/prev to unknown on every unmodeled op — so only within-block
// produced values and slot-carried classes are ever claimed. Slot
// classes are tracked per var/arg slot (captured slots excluded — their
// closures can write them at any time) and are the only state propagated
// across blocks, like P2's value lattice. Class semantics are sound
// because array-ness survives element mutation; only a write of a
// different-class value changes a slot's class, and every such write is
// a modeled put_loc-family op.
static void tier3_arrayidx(const std::vector<Insn>& insns,
                           const std::vector<uint8_t>& captured,
                           uint32_t var_count,
                           Tier3Stats* st) {
    const size_t n = insns.size();
    if (n == 0 || var_count == 0) return;

    std::vector<int32_t> block_id;
    std::vector<size_t> bstart, bend;
    std::vector<uint8_t> reachable;
    tier3_cfg(insns, &block_id, &bstart, &bend, &reachable);
    const size_t nb = bstart.size();
    if (nb == 0) return;

    // Per-block entry slot-class lattice; same two-phase copy-or-meet
    // protocol as the slot analysis (phase 1: fixpoint from the entry
    // block, no counting; phase 2: one counting sweep per block against
    // the converged entries). The entry block starts all-unknown:
    // arguments can carry anything.
    std::vector<uint8_t> in_cls(nb * var_count, SC_UNKNOWN);
    std::vector<uint8_t> in_seen(nb, 0);
    std::vector<uint8_t> in_wl(nb, 0);
    std::vector<size_t> worklist;
    in_seen[0] = 1;
    in_wl[0] = 1;
    worklist.push_back(0);

    auto slot_class = [&](const uint8_t* cls, int32_t s) -> uint8_t {
        if (s < 0 || static_cast<size_t>(s) >= var_count) return SC_UNKNOWN;
        if (static_cast<size_t>(s) >= captured.size() || captured[static_cast<size_t>(s)]) {
            return SC_UNKNOWN;  // captured: closure-writable at any time
        }
        return cls[static_cast<size_t>(s)];
    };

    auto sweep = [&](size_t b, std::vector<uint8_t>& cls,
                     Tier3Stats* cnt,
                     std::vector<std::pair<size_t, std::vector<uint8_t>>>* edges) {
        uint8_t top = SC_UNKNOWN;
        uint8_t prev = SC_UNKNOWN;
        for (size_t i = bstart[b]; i < bend[b]; i++) {
            const Insn& in = insns[i];
            uint8_t op = in.op;
            if (is_small_int_push(op)) {
                prev = top;
                top = SC_INT;
            } else if (op == OP_push_true || op == OP_push_false) {
                // Booleans are int32 on the quickjs stack.
                prev = top;
                top = SC_INT;
            } else if (op == OP_array_from) {
                // Pops the element values (count in aux), pushes a fresh
                // array: provably an array; everything below is opaque.
                prev = SC_UNKNOWN;
                top = SC_ARRAY;
            } else if (op == OP_and || op == OP_or || op == OP_xor ||
                       op == OP_shl || op == OP_sar || op == OP_shr ||
                       op == OP_lnot || op == OP_lt || op == OP_lte ||
                       op == OP_gt || op == OP_gte || op == OP_eq ||
                       op == OP_neq || op == OP_strict_eq ||
                       op == OP_strict_neq || op == OP_is_undefined ||
                       op == OP_is_null || op == OP_is_undefined_or_null ||
                       op == OP_typeof_is_undefined) {
                // Bitwise and comparison results are always int32.
                prev = SC_UNKNOWN;
                top = SC_INT;
            } else if (op == OP_dup || op == OP_dup1 || op == OP_dup2 ||
                       op == OP_dup3 || op == OP_insert2 || op == OP_insert3) {
                // dup (a -> a a) and the dup1..3/insert2..3 rearrangements
                // all leave the top two classes in place.
            } else if (op == OP_get_loc0_loc1) {
                prev = slot_class(cls.data(), 0);
                top = slot_class(cls.data(), 1);
            } else if (is_loc_read(op)) {
                // A loc read pushes: the old top becomes prev, the slot's
                // class becomes top.
                int32_t s = loc_index(in);
                prev = top;
                top = slot_class(cls.data(), s);
            } else if (op == OP_set_loc || op == OP_set_loc8 ||
                       (op >= OP_set_loc0 && op <= OP_set_loc3) ||
                       op == OP_set_arg ||
                       (op >= OP_set_arg0 && op <= OP_set_arg3)) {
                // set_loc pops the value and re-pushes it: the slot takes
                // the class, the stack classes survive.
                int32_t s = loc_index(in);
                if (s >= 0 && static_cast<size_t>(s) < var_count &&
                    (static_cast<size_t>(s) >= captured.size() ||
                     !captured[static_cast<size_t>(s)])) {
                    cls[static_cast<size_t>(s)] = top;
                }
            } else if (op == OP_set_loc_uninitialized) {
                // TDZ marker: the slot holds no value yet. No stack
                // effect (the marker precedes the binding's own store),
                // so top/prev survive untouched.
                int32_t s = loc_index(in);
                if (s >= 0 && static_cast<size_t>(s) < var_count) {
                    cls[static_cast<size_t>(s)] = SC_UNKNOWN;
                }
            } else if (op == OP_put_loc_check || op == OP_put_loc_check_init ||
                       is_loc_write(op)) {
                int32_t s = loc_index(in);
                if (s >= 0 && static_cast<size_t>(s) < var_count &&
                    (static_cast<size_t>(s) >= captured.size() ||
                     !captured[static_cast<size_t>(s)])) {
                    cls[static_cast<size_t>(s)] = top;
                }
                prev = SC_UNKNOWN;
                top = SC_UNKNOWN;
            } else if (op == OP_inc_loc || op == OP_dec_loc ||
                       op == OP_add_loc) {
                // In-place slot arithmetic: int32 overflow promotes to
                // float64, so the slot's class is lost. inc/dec have no
                // stack effect; add_loc pops the RHS.
                int32_t s = loc_index(in);
                if (s >= 0 && static_cast<size_t>(s) < var_count) {
                    cls[static_cast<size_t>(s)] = SC_UNKNOWN;
                }
                prev = SC_UNKNOWN;
                top = SC_UNKNOWN;
            } else if (op == OP_get_array_el || op == OP_get_array_el2) {
                // Pops obj idx (either push order): specializable iff one
                // operand is provably an array and the other provably an
                // int.
                bool arr = (top == SC_ARRAY && prev == SC_INT) ||
                           (top == SC_INT && prev == SC_ARRAY);
                if (cnt) {
                    cnt->get_array_el++;
                    if (arr) {
                        cnt->get_array_el_red++;
                    } else if (top == SC_ARRAY || prev == SC_ARRAY) {
                        // index would need a guard (int32 range unknown)
                        cnt->get_array_el_arr++;
                    }
                }
                prev = SC_UNKNOWN;
                top = SC_UNKNOWN;
            } else {
                // Every other op makes the stack top opaque; slot classes
                // survive — no frame-local slot is written by any op not
                // enumerated above, and calls/property ops only move
                // values through the stack.
                prev = SC_UNKNOWN;
                top = SC_UNKNOWN;
            }
            if (edges && in.target >= 0) {
                // Jump edge: carry the slot classes as of the jump itself
                // (see the slot analysis for the mid-block rationale).
                int32_t tb = block_id[static_cast<size_t>(in.target)];
                if (tb >= 0 && reachable[static_cast<size_t>(tb)]) {
                    edges->push_back({static_cast<size_t>(tb), cls});
                }
            }
        }
        if (edges && !tier3_is_terminator(insns[bend[b] - 1].op) &&
            bend[b] < n) {
            int32_t tb = block_id[bend[b]];
            if (tb >= 0 && reachable[static_cast<size_t>(tb)]) {
                edges->push_back({static_cast<size_t>(tb), cls});
            }
        }
    };

    while (!worklist.empty()) {
        size_t b = worklist.back();
        worklist.pop_back();
        in_wl[b] = 0;
        if (!reachable[b]) continue;
        std::vector<uint8_t> cls(in_cls.begin() + b * var_count,
                                 in_cls.begin() + (b + 1) * var_count);
        std::vector<std::pair<size_t, std::vector<uint8_t>>> edges;
        sweep(b, cls, nullptr, &edges);

        // Propagate along every jump edge (snapshot at the jump) and the
        // last-insn fall-through.
        for (auto& e : edges) {
            size_t sb = e.first;
            uint8_t* dst = &in_cls[sb * var_count];
            const uint8_t* src = e.second.data();
            if (!in_seen[sb]) {
                std::memcpy(dst, src, var_count);
                in_seen[sb] = 1;
                in_wl[sb] = 1;
                worklist.push_back(sb);
            } else {
                bool ch = false;
                for (size_t j = 0; j < var_count; j++) {
                    uint8_t m = stack_meet(dst[j], src[j]);
                    if (m != dst[j]) {
                        dst[j] = m;
                        ch = true;
                    }
                }
                if (ch && !in_wl[sb]) {
                    in_wl[sb] = 1;
                    worklist.push_back(sb);
                }
            }
        }
    }

    // Phase 2: count each site exactly once against the converged entries.
    for (size_t b = 0; b < nb; b++) {
        if (!reachable[b]) continue;
        std::vector<uint8_t> cls(in_cls.begin() + b * var_count,
                                 in_cls.begin() + (b + 1) * var_count);
        sweep(b, cls, st, nullptr);
    }
}

// Analyze one function record and its children for the A3 density proofs.
// Mirrors scan_function's recursion shape so the tier3 numbers cover the
// same function set as the foldability line.
static bool tier3_function(const FuncRecord& f,
                           const uint8_t* data,
                           Tier3Stats* st,
                           std::string* error) {
    std::vector<Insn> insns;
    if (!decode_code(data + f.code_off, f.code_len, &insns, error)) {
        return false;
    }
    st->funcs++;
    if (tier3_has_unmodeled_cfg(insns)) {
        st->funcs_skipped++;
    } else {
        tier3_slotinit(insns, f.var_count, st);
        tier3_arrayidx(insns, f.captured, f.var_count, st);
    }
    for (size_t i = 0; i < f.children.size(); i++) {
        if (!tier3_function(f.children[i], data, st, error)) return false;
    }
    return true;
}

bool analyze_only(const std::vector<std::uint8_t>& in, std::string* error) {
    error->clear();
    std::vector<FuncRecord> functions;
    if (!parse_buffer(in.data(), in.size(), &functions, error)) return false;
    FoldStats st;
    Tier3Stats t3;
    for (size_t i = 0; i < functions.size(); i++) {
        if (!scan_function(functions[i], in.data(), &st)) {
            *error = "bytecode optimize: invalid opcode in function";
            return false;
        }
        if (!tier3_function(functions[i], in.data(), &t3, error)) {
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
    std::fprintf(stderr,
                 "bytecode tier3: get_loc_check %llu/%llu reducible (%.2f%%), "
                 "put_loc_check %llu/%llu reducible (%.2f%%), get_array_el "
                 "%llu/%llu specializable (%.2f%%, obj-only %llu), "
                 "funcs %llu (%llu skipped)\n",
                 static_cast<unsigned long long>(t3.get_loc_check_red),
                 static_cast<unsigned long long>(t3.get_loc_check),
                 t3.get_loc_check ? 100.0 * t3.get_loc_check_red /
                                        t3.get_loc_check
                                  : 0.0,
                 static_cast<unsigned long long>(t3.put_loc_check_red),
                 static_cast<unsigned long long>(t3.put_loc_check),
                 t3.put_loc_check ? 100.0 * t3.put_loc_check_red /
                                        t3.put_loc_check
                                  : 0.0,
                 static_cast<unsigned long long>(t3.get_array_el_red),
                 static_cast<unsigned long long>(t3.get_array_el),
                 t3.get_array_el ? 100.0 * t3.get_array_el_red /
                                       t3.get_array_el
                                 : 0.0,
                 static_cast<unsigned long long>(t3.get_array_el_arr),
                 static_cast<unsigned long long>(t3.funcs),
                 static_cast<unsigned long long>(t3.funcs_skipped));
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
                         "P11 %llu P14 %llu P16 %llu T3 %llu, "
                         "shrinks %llu\n",
                         static_cast<unsigned long long>(stats.insns_before),
                         static_cast<unsigned long long>(stats.insns_after),
                         static_cast<unsigned long long>(stats.bytes_before),
                         static_cast<unsigned long long>(stats.bytes_after),
                         static_cast<unsigned long long>(stats.folds_p2),
                         static_cast<unsigned long long>(stats.folds_p31),
                         static_cast<unsigned long long>(stats.folds_p11),
                         static_cast<unsigned long long>(stats.folds_p14),
                         static_cast<unsigned long long>(stats.folds_p16),
                         static_cast<unsigned long long>(
                             stats.tdz_checks_removed),
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
                     "P11 %llu P14 %llu P16 %llu T3 %llu, "
                     "shrinks %llu\n",
                     static_cast<unsigned long long>(stats.insns_before),
                     static_cast<unsigned long long>(stats.insns_after),
                     static_cast<unsigned long long>(stats.bytes_before),
                     static_cast<unsigned long long>(stats.bytes_after),
                     static_cast<unsigned long long>(stats.folds_p2),
                     static_cast<unsigned long long>(stats.folds_p31),
                     static_cast<unsigned long long>(stats.folds_p11),
                     static_cast<unsigned long long>(stats.folds_p14),
                     static_cast<unsigned long long>(stats.folds_p16),
                     static_cast<unsigned long long>(
                         stats.tdz_checks_removed),
                     static_cast<unsigned long long>(stats.shrinks));
    }
    return true;
}

}  // namespace bytecode
}  // namespace capsid
