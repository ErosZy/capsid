// Directed tests for the retained BC27 / OP_ext foundation. R0's array
// specialization regressed the paired benchmark, so ext id 1 is now a
// permanent reserved hole and no BC27 image is canonical. The matrix locks
// BC26 compatibility and fail-closed handling for BC26+ext, reserved/unknown
// ids, truncated instructions, noncanonical BC27, and corrupt checksums.
//
// Splicing mechanism: each fixture is compiled by the runtime's own
// compiler as a module, serialized (JS_WriteObject), and then patched in
// memory:
//   - the bytecode body is located by a full structural walk of the
//     serialized records (atoms section -> module record -> module function
//     record -> nested function records in the cpool), scanning each body
//     for the deterministic [push_0 (0xBA)][get_array_el (0x46)] tail of
//     the fixture's `a[0]` access (opcode bytes are pinned by the BC26
//     wire format — see the opcode-table derivation below);
//   - the get_array_el byte is replaced by the 2-byte [OP_ext][ext_id];
//   - the function header's byte_code_len leb128 is incremented (the
//     splice grows the body by exactly one byte);
//   - the checksum over buf[5..end] is recomputed (bc_csum algorithm).
// Any layout drift (compiler output change, opcode renumbering) fails the
// fixture-setup asserts loudly instead of silently mis-splicing.
//
// A future live ext id must restore directed size/stack/pc2line coverage
// before it can be emitted. With every current id reserved, accepting such
// a fixture would be the bug this test is intended to catch.

#include "quickjs.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace {

void fail(const std::string &message) {
    std::cerr << "FAIL: " << message << std::endl;
    std::exit(1);
}

void require(bool condition, const char *message) {
    if (!condition) {
        fail(message);
    }
}

// ---------------------------------------------------------------------------
// Bytecode wire constants (BC26 format, pinned by quickjs-opcode.h DEF
// ordinals — renumbering any of these is a wire-format break):
//
//   OP_ext          = 252  (wire prefix)
//   OP_get_field_ic = 253  (runtime-only; reader rejects it on the wire)
//   OP_push_0       = 186  DEF ordinal (the fixture's `a[0]` index)
//   OP_get_array_el = 70   DEF ordinal
//   OP_return       = 40   DEF ordinal
//   ext id 1          = permanently reserved retired-R0 hole
// BC_TAG_FUNCTION_BYTECODE = 12, BC_TAG_MODULE = 13 (BCTagEnum:
// BC_TAG_NULL = 1, ...)
// ---------------------------------------------------------------------------

constexpr uint8_t kOpExt = 0xFC;          // 252
constexpr uint8_t kExtRetiredR0 = 0x01;  // permanently reserved ext id 1
constexpr uint8_t kOpPush0 = 0xBA;        // 186
constexpr uint8_t kOpGetArrayEl = 0x46;   // 70
constexpr uint8_t kOpReturn = 0x28;       // 40
constexpr uint8_t kOpGetField = 0x40;     // 64
constexpr uint8_t kOpGetFieldIC = 0xFD;   // 253, runtime-only
constexpr uint8_t kTagFunctionBytecode = 12;
constexpr uint8_t kTagModule = 13;

constexpr uint8_t kVersion26 = 26;
constexpr uint8_t kVersion27 = 27;

uint32_t read_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

void write_le32(std::vector<uint8_t> &buf, size_t pos, uint32_t v) {
    require(pos + 4 <= buf.size(), "write_le32 out of range");
    buf[pos + 0] = (uint8_t)(v & 0xff);
    buf[pos + 1] = (uint8_t)((v >> 8) & 0xff);
    buf[pos + 2] = (uint8_t)((v >> 16) & 0xff);
    buf[pos + 3] = (uint8_t)((v >> 24) & 0xff);
}

// bc_csum() from quickjs.c — the checksum covers buf[5..end].
uint32_t bc_csum(const uint8_t *p, size_t n) {
    uint32_t h = 0;
    size_t i = 0;
    for (; i + 4 < n; i += 4) {
        h += read_le32(p + i);
        h *= 0x9e370001u;
    }
    uint32_t a = 0, b = 0, c = 0;
    switch (n - i) {
    case 3:
        c = (uint32_t)p[i + 2];
        /* fallthrough */
    case 2:
        b = (uint32_t)p[i + 1];
        /* fallthrough */
    case 1:
        a = (uint32_t)p[i + 0];
    case 0:
        break;
    }
    h += a | b << 8 | c << 16;
    h *= 0x9e370001u;
    return h;
}

size_t read_leb128(const std::vector<uint8_t> &buf, size_t pos, uint32_t *out) {
    uint32_t v = 0;
    unsigned shift = 0;
    while (pos < buf.size() && shift < 35) {
        uint8_t b = buf[pos++];
        v |= (uint32_t)(b & 0x7f) << shift;
        if ((b & 0x80) == 0) {
            *out = v;
            return pos;
        }
        shift += 7;
    }
    fail("read_leb128: unterminated");
    return 0;
}

void write_leb128(std::vector<uint8_t> &buf, size_t pos, uint32_t v) {
    size_t p = pos;
    while (v >= 0x80) {
        require(p < buf.size(), "write_leb128 overflow");
        buf[p++] = (uint8_t)(v | 0x80);
        v >>= 7;
    }
    require(p < buf.size(), "write_leb128 overflow");
    buf[p] = (uint8_t)v;
}

// ---------------------------------------------------------------------------
// Parsed view of a serialized module blob.
// ---------------------------------------------------------------------------

struct Blob {
    std::vector<uint8_t> buf;
    size_t body_pos = 0;       // offset of the ext site (the get_array_el
                               // byte) within buf
    size_t bc_len_pos = 0;     // offset of the byte_code_len leb128
    uint32_t bc_len = 0;       // bytecode body length
    uint32_t atoms_end = 0;    // offset of the first record (module tag)
    std::vector<std::string> atoms;  // atom section strings, by section idx
    uint8_t version = 0;
    // true when the fixture's body ends with [get_array_el][return]
    // (base and branch fixtures). The atom fixture ends with
    // [gea][get_loc][get_field][add][return] and the pc fixture with
    // [gea][drop][push_1][return] — for those the tail assertion is
    // skipped.
    bool tail_is_gea_return = true;

    void recompute_checksum() {
        write_le32(buf, 1, bc_csum(buf.data() + 5, buf.size() - 5));
    }

    void set_version(uint8_t v) {
        version = v;
        buf[0] = v;
    }

    // --- structural walker (mirror of JS_ReadObjectAtoms / JS_ReadModule /
    // JS_ReadFunctionTag / JS_ReadFunctionBytecode) -----------------------
    //
    // Walks the atoms section, the module record, and every function
    // record (module top-level plus nested records reached through cpool
    // BC_TAG_FUNCTION_BYTECODE values), scanning each bytecode body for the
    // deterministic [push_0][get_array_el] pattern of the fixture's `a[0]`
    // access. Exactly one record must match; its gea byte becomes the
    // splice target. Any layout drift fails loudly instead of silently
    // mis-splicing.

    struct Record {
        size_t body_start = 0;
        size_t bc_len_pos = 0;
        uint32_t bc_len = 0;
        int gea_off = -1;  // offset of the [push_0][gea] gea byte in body
    };

    std::vector<Record> records;
    size_t pos_ = 0;

    uint8_t rd_u8() {
        require(pos_ < buf.size(), "walker: eof in u8");
        return buf[pos_++];
    }

    uint16_t rd_u16() {
        require(pos_ + 2 <= buf.size(), "walker: eof in u16");
        uint16_t v = (uint16_t)(buf[pos_] | (buf[pos_ + 1] << 8));
        pos_ += 2;
        return v;
    }

    uint32_t rd_leb() {
        uint32_t v = 0;
        pos_ = read_leb128(buf, pos_, &v);
        return v;
    }

    // Consume one serialized atom (leb128, odd = tagged int, even = idx).
    void rd_atom() { (void)rd_leb(); }

    // Skip one serialized value (tag + payload) — the cpool value extents.
    // Mirrors the reader's value walk; unknown tags fail closed.
    void skip_value() {
        uint8_t t = rd_u8();
        switch (t) {
        case 1:  // BC_TAG_NULL
        case 2:  // BC_TAG_UNDEFINED
        case 3:  // BC_TAG_BOOL_FALSE
        case 4:  // BC_TAG_BOOL_TRUE
            break;
        case 5:  // BC_TAG_INT32: sleb128 (writer bc_put_sleb128 /
                 // reader bc_get_sleb128 — same continuation encoding as
                 // leb128, so skipping with rd_leb is exact)
            (void)rd_leb();
            break;
        case 6:  // BC_TAG_FLOAT64
            pos_ += 8;
            break;
        case 7: {  // BC_TAG_STRING: leb (len<<1 | wide), then bytes
            uint32_t len = rd_leb();
            bool wide = (len & 1) != 0;
            size_t n = len >> 1;
            require(pos_ + n * (wide ? 2 : 1) <= buf.size(),
                    "walker: string out of range");
            pos_ += n * (wide ? 2 : 1);
            break;
        }
        case 9: {  // BC_TAG_ARRAY: count, then values
            uint32_t n = rd_leb();
            for (uint32_t i = 0; i < n; i++) skip_value();
            break;
        }
        case 11: {  // BC_TAG_TEMPLATE_OBJECT: count, values, then raw
            uint32_t n = rd_leb();
            for (uint32_t i = 0; i < n; i++) skip_value();
            skip_value();  // raw object data
            break;
        }
        case 12:  // BC_TAG_FUNCTION_BYTECODE
            parse_function_record();
            break;
        default:
            fail("walker: unsupported cpool value tag in fixture");
        }
    }

    // Structural walk of the whole blob. Call after buf is populated.
    void parse() {
        require(buf.size() > 5, "blob too small");
        version = buf[0];
        require(version == kVersion26 || version == kVersion27,
                "unexpected blob version");
        require(read_le32(buf.data() + 1) == bc_csum(buf.data() + 5,
                                                     buf.size() - 5),
                "blob checksum mismatch in fixture");

        pos_ = 5;
        uint32_t atom_count = rd_leb();
        for (uint32_t i = 0; i < atom_count; i++) {
            uint8_t type = rd_u8();
            if (type == 0) {
                pos_ += 4;  // const atom: u32 value
                atoms.push_back("");
            } else {
                uint32_t len = rd_leb();
                bool wide = (len & 1) != 0;
                size_t n = len >> 1;
                require(pos_ + n * (wide ? 2 : 1) <= buf.size(),
                        "walker: atom string out of range");
                if (wide) {
                    atoms.push_back("<wide>");
                    pos_ += n * 2;
                } else {
                    atoms.push_back(std::string(
                        reinterpret_cast<const char *>(buf.data() + pos_),
                        n));
                    pos_ += n;
                }
            }
        }
        atoms_end = (uint32_t)pos_;

        require(rd_u8() == kTagModule, "expected BC_TAG_MODULE record");
        rd_atom();                    // module name
        for (int i = 0; i < 4; i++) {  // req/export/star/import counts
            rd_leb();
        }
        (void)rd_u8();  // has_tla

        require(rd_u8() == kTagFunctionBytecode,
                "expected module function record");
        records.clear();
        parse_function_record();

        require(pos_ == buf.size(),
                "walker: trailing bytes after last record");

        // Exactly one record body contains the fixture's [push_0][gea].
        size_t matches = 0;
        for (const Record &rec : records) {
            if (rec.gea_off >= 0) {
                matches++;
                body_pos = rec.body_start + (size_t)rec.gea_off;
                bc_len_pos = rec.bc_len_pos;
                bc_len = rec.bc_len;
            }
        }
        require(matches == 1,
                "fixture layout mismatch: [push_0][get_array_el] must occur "
                "in exactly one record body (compiler output changed?)");
        require(buf[body_pos] == kOpGetArrayEl, "splice target is not "
                "get_array_el");
        require(body_pos + 1 < buf.size(), "body tail out of range");
        if (tail_is_gea_return) {
            require(buf[body_pos + 1] == kOpReturn,
                    "fixture layout mismatch: body does not end in "
                    "get_array_el, return");
        }
        require(body_pos >= atoms_end, "body scan landed before the record");
    }

    // Function record walk — shared prefix reader plus body scan. The
    // fixed prefix is arg/var/defined_arg/stack/var_ref/closure/cpool/
    // byte_code_len/local (9 leb128s) after flags/strict/name.
    void parse_function_record() {
        (void)rd_u16();    // flags
        (void)rd_u8();     // is_strict_mode
        rd_atom();         // func_name
        rd_leb();          // arg_count
        rd_leb();          // var_count
        rd_leb();          // defined_arg_count
        rd_leb();          // stack_size
        rd_leb();          // var_ref_count
        uint32_t closure_var_count = rd_leb();
        uint32_t cpool_count = rd_leb();
        Record rec;
        rec.bc_len_pos = pos_;
        rec.bc_len = rd_leb();  // byte_code_len
        uint32_t local_count = rd_leb();

        for (uint32_t i = 0; i < local_count; i++) {  // vardefs
            rd_atom();          // var_name
            rd_leb();           // scope_level
            rd_leb();           // scope_next
            uint8_t flags = rd_u8();
            if ((flags & 0x40) != 0) {  // is_captured -> var_ref_idx
                rd_leb();
            }
        }
        for (uint32_t i = 0; i < closure_var_count; i++) {  // closure vars
            rd_atom();  // var_name
            rd_leb();   // var_idx
            rd_leb();   // flags
        }
        for (uint32_t i = 0; i < cpool_count; i++) {
            skip_value();  // nested function records recurse here
        }

        rec.body_start = pos_;
        require(pos_ + rec.bc_len <= buf.size(), "walker: body out of range");
        for (uint32_t i = 0; i + 1 < rec.bc_len; i++) {
            if (buf[rec.body_start + i] == kOpPush0 &&
                buf[rec.body_start + i + 1] == kOpGetArrayEl) {
                require(rec.gea_off < 0,
                        "fixture layout mismatch: two [push_0][gea] in one "
                        "record body");
                rec.gea_off = (int)i + 1;
            }
        }
        pos_ += rec.bc_len;

        rd_atom();                    // debug: filename
        rd_leb();                     // line
        rd_leb();                     // col
        uint32_t pc2line_len = rd_leb();
        require(pos_ + pc2line_len <= buf.size(), "walker: pc2line overflow");
        pos_ += pc2line_len;
        uint32_t source_len = rd_leb();
        require(pos_ + source_len <= buf.size(), "walker: source overflow");
        pos_ += source_len;

        records.push_back(rec);
    }

    // Replace the get_array_el at body_pos with [OP_ext][ext_id] and fix
    // the bytecode length (body grows by one byte). Keeps ext_id in place
    // for callers that need to corrupt it afterwards.
    void splice_ext(uint8_t ext_id) {
        require(buf[body_pos] == kOpGetArrayEl, "splice target lost");
        buf.insert(buf.begin() + body_pos, kOpExt);
        buf[body_pos + 1] = ext_id;
        require(buf[body_pos] == kOpExt && buf[body_pos + 1] == ext_id,
                "splice failed");
        bc_len++;
        write_leb128(buf, bc_len_pos, bc_len);
        recompute_checksum();
    }
};

struct Runtime {
    JSRuntime *rt;
    JSContext *ctx;

    Runtime() {
        rt = JS_NewRuntime();
        require(rt != nullptr, "JS_NewRuntime failed");
        ctx = JS_NewContext(rt);
        require(ctx != nullptr, "JS_NewContext failed");
    }

    ~Runtime() {
        JS_FreeContext(ctx);
        JS_FreeRuntime(rt);
    }

    // "ex:<message>" for an exception (with the "stack" backtrace appended
    // when the error object carries one — the pc2line evidence for
    // scenario 10). Consumes the exc value.
    std::string exception_detail(JSValue exc) {
        const char *msg = JS_ToCString(ctx, exc);
        std::string out = std::string("ex:") +
                          (msg ? msg : "<no message>");
        JS_FreeCString(ctx, msg);
        JSValue stack = JS_GetPropertyStr(ctx, exc, "stack");
        if (JS_IsString(stack)) {
            const char *st = JS_ToCString(ctx, stack);
            out += " | stack: " + std::string(st ? st : "?");
            JS_FreeCString(ctx, st);
        }
        JS_FreeValue(ctx, stack);
        JS_FreeValue(ctx, exc);
        return out;
    }

    // Compile a module source (fixtures assign `globalThis.__r = <expr>;`)
    // and serialize it (BC26, with debug info) — the same route as
    // capsid-bytecode-compile.cc / worker_runtime.cc: compile with
    // JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY, then write the
    // module value. The bytecode writer only serializes modules and
    // bytecode objects (not function closures), so the module form is the
    // only public path that yields a blob.
    Blob compile_blob(const std::string &source,
                      bool tail_is_gea_return = true) {
        Blob b;
        b.tail_is_gea_return = tail_is_gea_return;
        JSValue module = JS_Eval(ctx, source.c_str(), source.size(),
                                 "ext-test.js",
                                 JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
        require(!JS_IsException(module), "fixture module compile failed");
        size_t len = 0;
        uint8_t *buf = JS_WriteObject(ctx, &len, module,
                                      JS_WRITE_OBJ_BYTECODE);
        require(buf != nullptr, "JS_WriteObject failed");
        JS_FreeValue(ctx, module);
        b.buf.assign(buf, buf + len);
        js_free(ctx, buf);
        b.parse();
        return b;
    }

    // JS_ReadObject + (optionally) resolve and execute the module. Returns
    // a plain "result" string for assertions: "ex:<message>" for an
    // exception, "val:<n>" for an int result, "undef" for undefined.
    std::string load_and_run(const Blob &b, bool run = true) {
        JSValue module = JS_ReadObject(ctx, b.buf.data(), b.buf.size(),
                                       JS_READ_OBJ_BYTECODE);
        if (JS_IsException(module)) {
            return exception_detail(JS_GetException(ctx));
        }
        require(JS_VALUE_GET_TAG(module) == JS_TAG_MODULE,
                "read-back value is not a module");
        if (!run) {
            JS_FreeValue(ctx, module);
            return "ok";
        }
        require(JS_ResolveModule(ctx, module) == 0, "JS_ResolveModule failed");
        JSValue ret = JS_EvalFunction(ctx, module);  // consumes module
        if (JS_IsException(ret)) {
            return exception_detail(JS_GetException(ctx));
        }
        // The module top-level compiles as an async function (return_async —
        // top-level-await support), so a body exception surfaces as a
        // rejected promise, not a JS exception. Mirror worker_runtime.cc's
        // module completion contract: drain the job queue, then classify
        // the evaluation result.
        while (JS_IsJobPending(JS_GetRuntime(ctx))) {
            JSContext *job_ctx = NULL;
            const int job_r = JS_ExecutePendingJob(JS_GetRuntime(ctx),
                                                   &job_ctx);
            if (job_r <= 0) break;
        }
        const JSPromiseStateEnum pstate = JS_PromiseState(ctx, ret);
        if (pstate == JS_PROMISE_REJECTED) {
            JSValue reason = JS_PromiseResult(ctx, ret);
            std::string out = exception_detail(reason);
            JS_FreeValue(ctx, ret);
            return out;
        }
        if (pstate == JS_PROMISE_PENDING) {
            JS_FreeValue(ctx, ret);
            fail("module top-level await must settle without external I/O");
        }
        JS_FreeValue(ctx, ret);
        JSValue g = JS_GetGlobalObject(ctx);
        JSValue rv = JS_GetPropertyStr(ctx, g, "__r");
        JS_FreeValue(ctx, g);
        if (JS_IsException(rv)) {
            return exception_detail(JS_GetException(ctx));
        }
        if (JS_IsUndefined(rv)) {
            JS_FreeValue(ctx, rv);
            return "undef";
        }
        int32_t v = 0;
        require(JS_ToInt32(ctx, &v, rv) == 0, "result is not an int");
        JS_FreeValue(ctx, rv);
        char tmp[32];
        snprintf(tmp, sizeof(tmp), "val:%d", v);
        return std::string(tmp);
    }

    std::string read_only(const Blob &b) { return load_and_run(b, false); }

    void run() { run_scenarios(); }

    // --- scenarios -----------------------------------------------------

    void run_scenarios() {
        // 1. baseline sanity
        Blob base = compile_blob(
            "globalThis.__r = (function(){ var a=[42]; return a[0]; })();\n");
        require(base.version == kVersion26, "baseline is not BC26");
        require(load_and_run(base) == "val:42", "baseline run failed");

        // 2. BC26 must reject OP_ext — twice: a blob spliced while still
        //    BC26, and a BC27 blob with the version byte flipped back to
        //    26 (proves the gate keys on the version byte, not on how the
        //    blob was produced).
        Blob b26 = base;
        b26.splice_ext(kExtRetiredR0);
        std::string r2 = read_only(b26);
        require(r2.rfind("ex:", 0) == 0, "BC26+ext must fail closed");
        require(r2.find("ext") != std::string::npos,
                "BC26+ext error must mention ext");
        Blob b27_flipped = base;
        b27_flipped.set_version(kVersion27);
        b27_flipped.splice_ext(kExtRetiredR0);
        b27_flipped.set_version(kVersion26);
        std::string r2b = read_only(b27_flipped);
        require(r2b.rfind("ex:", 0) == 0,
                "BC27 blob flipped to BC26 must reject ext");
        require(r2b.find("ext") != std::string::npos,
                "flipped-version rejection must mention ext");

        // 3. unknown bundle version
        Blob b28 = base;
        b28.set_version(28);
        b28.recompute_checksum();
        require(read_only(b28).rfind("ex:", 0) == 0,
                "version 28 must be rejected");

        // Runtime quickening opcode 253 is never legal on the wire, even in
        // an otherwise canonical BC26 image.
        Blob runtime_ic = compile_blob(
            "globalThis.__r = (function(){ var a=[42]; var o={x:3}; return "
            "a[0] + o.x; })();\n",
            /*tail_is_gea_return=*/false);
        size_t field_pos = runtime_ic.buf.size();
        const size_t body_end = runtime_ic.body_pos + runtime_ic.bc_len;
        for (size_t i = runtime_ic.body_pos + 1;
             i + 5 <= runtime_ic.buf.size() && i < body_end; i++) {
            if (runtime_ic.buf[i] == kOpGetField) {
                field_pos = i;
                break;
            }
        }
        require(field_pos < runtime_ic.buf.size(),
                "runtime-op fixture lacks get_field");
        runtime_ic.buf[field_pos] = kOpGetFieldIC;
        runtime_ic.recompute_checksum();
        require(read_only(runtime_ic).rfind("ex:", 0) == 0,
                "runtime-only field IC opcode must fail closed on wire");

        // 4. BC27 retired R0 id 1 is a permanent reserved hole.
        Blob bc27 = base;
        bc27.set_version(kVersion27);
        bc27.splice_ext(kExtRetiredR0);
        std::string r4 = read_only(bc27);
        require(r4.rfind("ex:", 0) == 0,
                "retired R0 ext id must fail closed");

        // 5. reserved id 0 and unknown ids fail closed.
        Blob id0 = bc27;
        id0.buf[id0.body_pos + 1] = 0x00;
        id0.recompute_checksum();
        require(read_only(id0).rfind("ex:", 0) == 0, "ext id 0 must be "
                "rejected");
        Blob id7f = bc27;
        id7f.buf[id7f.body_pos + 1] = 0x7f;
        id7f.recompute_checksum();
        require(read_only(id7f).rfind("ex:", 0) == 0, "unknown ext id must "
                "be rejected");

        // 6. truncated payload: ext prefix as the last body byte (no
        //    ext_id byte follows). Note: for the current size-2 ext an
        //    "ext_id present but payload short" case cannot exist (2 bytes
        //    consume the whole instruction); when a size-3+ ext lands this
        //    test must grow a matching case.
        Blob trunc = base;
        trunc.set_version(kVersion27);
        require(trunc.buf[trunc.body_pos + 1] == kOpReturn,
                "truncation target is not the trailing return");
        trunc.buf[trunc.body_pos + 1] = kOpExt;  // [.. 46 FC] ends the body
        trunc.recompute_checksum();
        require(read_only(trunc).rfind("ex:", 0) == 0, "truncated ext must "
                "be rejected");

        // 7. BC27 without OP_ext is noncanonical while R0 is retired.
        Blob no_ext = base;
        no_ext.set_version(kVersion27);
        no_ext.recompute_checksum();
        require(read_only(no_ext).rfind("ex:", 0) == 0,
                "BC27 without ext must fail closed");

        // 8. The checksum still covers all BC26 payload bytes. After
        // recomputing it, the deliberately changed index executes.
        Blob ck_atoms = base;
        ck_atoms.buf[5] ^= 0x01;  // corrupt the atom count
        require(read_only(ck_atoms).rfind("ex:", 0) == 0, "corrupt atoms "
                "must fail the checksum");
        Blob ck_body = base;
        ck_body.buf[ck_body.body_pos - 1] ^= 0x01;  // push_0 -> push_1
        require(read_only(ck_body).rfind("ex:", 0) == 0, "corrupt body must "
                "fail the checksum");
        ck_body.recompute_checksum();
        std::string r7 = load_and_run(ck_body);
        // The flipped push_0 makes the index 1 instead of 0, so the fast
        // path misses and the fallback returns undefined — which proves
        // the accepted blob is really the corrupted one.
        require(r7 == "undef", "re-checksummed blob must run (corrupt "
                "index, undefined result)");

        // 9. Canonical reserialization remains BC26 while there is no
        // deployed ext instruction.
        JSValue obj26 = JS_ReadObject(ctx, base.buf.data(), base.buf.size(),
                                      JS_READ_OBJ_BYTECODE);
        require(!JS_IsException(obj26), "re-read of BC26 blob failed");
        size_t len26 = 0;
        uint8_t *re26 = JS_WriteObject(ctx, &len26, obj26,
                                       JS_WRITE_OBJ_BYTECODE);
        require(re26 != nullptr, "reserialize of BC26 failed");
        require(len26 == base.buf.size() && memcmp(re26, base.buf.data(),
                len26) == 0, "BC26 reserialization is not canonical");
        require(re26[0] == kVersion26,
                "no-ext function must reserialize as BC26, not BC27");
        JS_FreeValue(ctx, obj26);
        js_free(ctx, re26);
    }
};

}  // namespace

int main() {
    Runtime rt;
    rt.run();
    std::cout << "PASS: ext_bytecode_directed (E0 §2 matrix)" << std::endl;
    return 0;
}
