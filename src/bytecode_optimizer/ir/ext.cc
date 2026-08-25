// F0 ext foundation implementation (see ext.h). The ext table is
// generated from the vendored quickjs-ext-opcode.h exactly like the
// opcode tables in cfg.cc/effects.cc: one positional row per id in
// header order, with removed ids keeping zeroed rows that ext_lookup
// rejects. The decoder-facing side (ext_round_trip) rides the I0
// walker stack — read_functions -> decode_function -> build_cfg ->
// verify_cfg — with ext-table sizes and stack effects, so the BC27
// reader, the verifier, and the identity gates share one decode path.
// Analyze-only: nothing is emitted and the production pipeline
// (bytecode_optimizer.cc optimize/analyze_only) rejects BC27 input.
#include "bytecode_optimizer/ir/ext.h"

#include <cstdio>
#include <string>
#include <vector>

#include "bytecode_optimizer/ir/cfg.h"

namespace capsid {
namespace bytecode {
namespace ir {
namespace {

// Serialized ids, generated from the same vendor table. Removed ids retain
// their named, size-zero row, so their numeric value can never be reused.
enum ExtOpcode : uint8_t {
#define EXT_FMT(f)
#define EXT_DEF(id, name, size, n_pop, n_push, fmt) EXT_##name = (id),
#include "quickjs-ext-opcode.h"
#undef EXT_DEF
#undef EXT_FMT
    EXT_COUNT,
};

struct ExtRow {
    uint8_t size;
    uint8_t n_pop;
    uint8_t n_push;
    ExtFmt fmt;
    const char* name;
};

// Positional init, one row per id in header order. A header fmt the
// ExtFmt enum does not know fails to compile here (the divergence
// guard for the format column).
static const ExtRow ext_rows[EXT_COUNT] = {
#define EXT_FMT(f)
#define EXT_DEF(id, name, size, n_pop, n_push, fmt) \
    { size, n_pop, n_push, EXT_FMT_##fmt, #name },
#include "quickjs-ext-opcode.h"
#undef EXT_DEF
#undef EXT_FMT
};

void count_function(const std::vector<FuncInfo>& funcs, uint64_t* count) {
    for (size_t i = 0; i < funcs.size(); i++) {
        (*count)++;
        count_function(funcs[i].children, count);
    }
}

}  // namespace

bool ext_lookup(uint8_t id, ExtInfo* out) {
    if (id == 0 || id >= EXT_COUNT) return false;
    const ExtRow& r = ext_rows[id];
    if (r.size == 0) return false;  // reserved / removed hole
    out->size = r.size;
    out->n_pop = r.n_pop;
    out->n_push = r.n_push;
    out->fmt = r.fmt;
    out->name = r.name;
    return true;
}

bool validate_ext_operands(const ExtInfo& info,
                           const uint8_t* payload,
                           size_t payload_len,
                           uint32_t arg_count,
                           uint32_t var_count,
                           std::string* error) {
    if (info.size < 2) {
        *error = "ext: invalid instruction size";
        return false;
    }
    size_t slots = 0;
    switch (info.fmt) {
    case EXT_FMT_none:
        if (payload_len != static_cast<size_t>(info.size - 2)) {
            *error = "ext: operand format/size mismatch";
            return false;
        }
        return true;
    case EXT_FMT_slot2:
        slots = 2;
        break;
    case EXT_FMT_slot3:
        slots = 3;
        break;
    default:
        *error = "ext: unimplemented operand format";
        return false;
    }
    if (payload_len != slots || info.size != slots + 2) {
        *error = "ext: operand format/size mismatch";
        return false;
    }
    for (size_t i = 0; i < slots; i++) {
        const uint8_t slot = payload[i];
        const uint32_t index = slot & 0x7fu;
        const uint32_t limit = (slot & 0x80u) ? arg_count : var_count;
        if (index >= limit) {
            *error = std::string("ext: ") +
                     ((slot & 0x80u) ? "argument" : "local") +
                     " slot operand out of range";
            return false;
        }
    }
    return true;
}

bool ext_round_trip(const uint8_t* data,
                    size_t size,
                    ExtRoundTripReport* out,
                    std::string* error) {
    std::vector<FuncInfo> functions;
    if (!read_functions(data, size, &functions, error)) return false;
    ExtRoundTripReport rep;
    rep.functions = 0;
    rep.ext_instructions = 0;
    rep.rejected_functions = 0;
    rep.rejected_insns = 0;
    for (int i = 0; i < 256; i++) rep.per_id[i] = 0;
    count_function(functions, &rep.functions);

    // The reader already enforced the ext policy; decode_function
    // mirrors it (allow_ext = the bundle is BC27) so the walkers stay
    // fail-closed even without the reader in front of them.
    const bool allow_ext = size > 0 && data[0] == BC_VERSION_EXT;

    struct Walker {
        const uint8_t* data;
        bool allow_ext;
        ExtRoundTripReport* rep;
        bool run(const FuncInfo& fi, std::string* error) {
            std::vector<Insn> insns;
            if (!decode_function(data + fi.code_off, fi.code_len, data, fi,
                                 &insns, error, allow_ext)) {
                std::fprintf(stderr, "ext: rejected (decode): %s\n",
                             error->c_str());
                rep->rejected_functions++;
                error->clear();
                return true;
            }
            for (size_t i = 0; i < insns.size(); i++) {
                if (insns[i].op == OP_EXT) {
                    rep->ext_instructions++;
                    rep->per_id[insns[i].aux]++;
                }
            }
            Cfg cfg;
            if (!build_cfg(insns, &cfg, error)) {
                std::fprintf(stderr, "ext: rejected (build): %s\n",
                             error->c_str());
                rep->rejected_functions++;
                rep->rejected_insns += static_cast<uint64_t>(insns.size());
                error->clear();
                return true;
            }
            cfg.recorded_stack_size = fi.stack_size;
            if (!verify_cfg(cfg, error)) {
                std::fprintf(stderr, "ext: rejected (verify): %s\n",
                             error->c_str());
                rep->rejected_functions++;
                rep->rejected_insns += static_cast<uint64_t>(insns.size());
                error->clear();
                return true;
            }
            for (size_t i = 0; i < fi.children.size(); i++) {
                if (!run(fi.children[i], error)) return false;
            }
            return true;
        }
    };
    Walker w;
    w.data = data;
    w.allow_ext = allow_ext;
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
