// I2 region census implementation (see region.h). The matcher runs
// over the SSA form built by ssa_analyze_function: for each block it
// finds maximal runs of template-matching nodes (single-BB by
// construction, <= 8 original instructions, never crossing a call —
// non-matching nodes split runs — a handler boundary, a suspension, or
// a safepoint) and scores each run with the §4.1 cost model. The
// report aggregates per-template totals and selects the at-most-two
// first templates by predicted cost. Analyze-only, like every I0-I2
// module: nothing is emitted and the production pipeline never calls
// this.
#include "bytecode_optimizer/ir/region.h"

#include <algorithm>
#include <climits>
#include <cstdio>

#include "bytecode_optimizer/ir/cfg.h"
#include "bytecode_optimizer/ir/effects.h"
#include "bytecode_optimizer/ir/ssa.h"

namespace capsid {
namespace bytecode {
namespace ir {
namespace {

// Serialized opcode enum, built exactly like cfg.cc/effects.cc
// (quickjs.c:1166). Only the final serialized opcodes matter here: the
// never-serialized temporary opcodes cannot appear in BC26 function
// code.
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

const char* template_name(Template t) {
    switch (t) {
    case Template::FAST_ARRAY_GET_I32:
        return "fast_array_get_i32";
    case Template::FAST_ARRAY_UPDATE_NUM:
        return "fast_array_update_number";
    case Template::SHAPE_GET_OWN:
        return "shape_get_own";
    case Template::SHAPE_PUT_OWN:
        return "shape_put_own";
    case Template::I32_ARITH_CHAIN:
        return "i32_arith_chain";
    case Template::F64_ARITH_CHAIN:
        return "f64_arith_chain";
    case Template::COUNT:
        return "<none>";
    }
    return "<none>";
}

// Per-template guard requirements (§4.3: all guards execute before the
// fast path mutates stack, heap, refcounts, or exception state; the
// fast body is non-throwing after its guards). Arith chains prove
// their operands from the lattice (0 guards); the property templates
// pay runtime class/shape checks. Declared in region.h (catalog
// metadata; the report prints the aggregate).
uint32_t template_guards(Template t) {
    switch (t) {
    case Template::FAST_ARRAY_GET_I32:   // Array class + in-bounds
    case Template::FAST_ARRAY_UPDATE_NUM:
        return 2;
    case Template::SHAPE_GET_OWN:        // shape check
        return 1;
    case Template::SHAPE_PUT_OWN:        // shape + own-data
        return 2;
    case Template::I32_ARITH_CHAIN:      // provable from the lattice
    case Template::F64_ARITH_CHAIN:
        return 0;
    case Template::COUNT:
        return 0;
    }
    return 0;
}

namespace {

// The shape/array templates need an object operand that is not
// provably a non-object. UNKNOWN (could be any object) and the
// provably-heap lattices qualify; scalars and the never-written
// sentinel do not (a string index site like "abc"[i] is a different
// fast path, not in the first catalog).
bool lattice_accepts_object(Lattice l) {
    switch (l) {
    case Lattice::UNKNOWN:
    case Lattice::FAST_ARRAY:
    case Lattice::OBJECT_SHAPES:
    case Lattice::EXACT_CLOSURE:
        return true;
    default:
        return false;  // INT32 / FLOAT64 / NUMBER / STRING / UNINITIALIZED
    }
}

// Node predicate: is `node` a candidate member of template `t`?
// Region rules (§4.3): a candidate never crosses an exception handler
// boundary (exc_succ >= 0), a suspension, or a safepoint (BARRIER /
// SUSPEND effects). The template ops themselves are CALL-effect (the
// interpreter's full property/coercion path); a fused fast body
// replaces exactly those. A call BETWEEN candidates splits the run
// because it matches no template.
bool node_matches(const SsaFunc& ssa, const SsaNode& node, Template t) {
    if (node.exc_succ >= 0) return false;
    if (node.effect == EffectClass::BARRIER ||
        node.effect == EffectClass::SUSPEND) {
        return false;
    }
    if (node.args.empty()) return false;
    switch (t) {
    case Template::FAST_ARRAY_GET_I32: {
        // get_array_el obj idx -> value: args are top-first [idx, obj].
        if (node.args.size() < 2) return false;
        const Lattice obj = ssa.lattice[node.args[1]];
        const Lattice idx = ssa.lattice[node.args[0]];
        return idx == Lattice::INT32 && lattice_accepts_object(obj);
    }
    case Template::FAST_ARRAY_UPDATE_NUM: {
        // put_array_el obj idx value: args top-first [value, idx, obj].
        if (node.args.size() < 3) return false;
        const Lattice obj = ssa.lattice[node.args[2]];
        const Lattice idx = ssa.lattice[node.args[1]];
        const Lattice val = ssa.lattice[node.args[0]];
        const bool num = val == Lattice::INT32 || val == Lattice::FLOAT64 ||
                         val == Lattice::NUMBER;
        return idx == Lattice::INT32 && num && lattice_accepts_object(obj);
    }
    case Template::SHAPE_GET_OWN: {
        // get_field obj: args top-first [obj].
        return lattice_accepts_object(ssa.lattice[node.args[0]]);
    }
    case Template::SHAPE_PUT_OWN: {
        // put_field obj value: args top-first [value, obj].
        if (node.args.size() < 2) return false;
        return lattice_accepts_object(ssa.lattice[node.args[1]]);
    }
    case Template::I32_ARITH_CHAIN: {
        // add/sub/mul/and/or/xor/shl/sar/shr with both operands
        // provably int32. A site where BOTH operands are folded
        // immediates is already a constant (the v1 pipeline removes
        // it) — the fused op only pays for itself on sites that
        // survive folding.
        if (node.args.size() < 2) return false;
        const uint32_t l = node.args[1];
        const uint32_t r = node.args[0];
        if (ssa.lattice[l] != Lattice::INT32 ||
            ssa.lattice[r] != Lattice::INT32) {
            return false;
        }
        return !(ssa.has_imm[l] != 0 && ssa.has_imm[r] != 0);
    }
    case Template::F64_ARITH_CHAIN: {
        // add/sub/mul/div/mod/pow with both operands provably float64.
        if (node.args.size() < 2) return false;
        return ssa.lattice[node.args[1]] == Lattice::FLOAT64 &&
               ssa.lattice[node.args[0]] == Lattice::FLOAT64;
    }
    case Template::COUNT:
        return false;
    }
    return false;
}

// The §4.2 opcode families.
bool op_is_get_array_el(uint8_t op) {
    return op == OP_get_array_el || op == OP_get_array_el2;
}

bool op_is_i32_arith(uint8_t op) {
    switch (op) {
    case OP_add: case OP_sub: case OP_mul: case OP_and: case OP_or:
    case OP_xor: case OP_shl: case OP_sar: case OP_shr:
        return true;
    default:
        return false;
    }
}

bool op_is_f64_arith(uint8_t op) {
    switch (op) {
    case OP_add: case OP_sub: case OP_mul: case OP_div: case OP_mod:
    case OP_pow:
        return true;
    default:
        return false;
    }
}

// The template a node's opcode can belong to, or -1. The lattice check
// (node_matches) is applied when extending a run; the opcode family is
// the cheap pre-filter.
int opcode_template(uint8_t op) {
    if (op_is_get_array_el(op)) return static_cast<int>(Template::FAST_ARRAY_GET_I32);
    if (op == OP_put_array_el) return static_cast<int>(Template::FAST_ARRAY_UPDATE_NUM);
    if (op == OP_get_field) return static_cast<int>(Template::SHAPE_GET_OWN);
    if (op == OP_put_field) return static_cast<int>(Template::SHAPE_PUT_OWN);
    if (op_is_i32_arith(op)) return static_cast<int>(Template::I32_ARITH_CHAIN);
    if (op_is_f64_arith(op)) return static_cast<int>(Template::F64_ARITH_CHAIN);
    return -1;
}

// The §4.1 predicted score in dispatch-equivalent units:
//   avoided dispatches (n) + avoided tag/class checks (n)
//   - ext secondary dispatch (1) - guards
//   - duplicated slow-path I-cache cost (retained bytes / 8).
int64_t predicted_cost(uint32_t n_insns, uint32_t n_guards,
                       uint32_t slow_bytes) {
    return 2 * static_cast<int64_t>(n_insns) - n_guards - 1 -
           static_cast<int64_t>(slow_bytes / 8);
}

// One maximal run being extended. A run is a candidate when flushed.
struct Run {
    Template tmpl;
    uint32_t first_node;  // SsaBlock::nodes index
    uint32_t last_node;   // inclusive
    uint32_t n_insns;
    bool active;
};

void flush_run(const Run& run, const SsaBlock& block,
               RegionCensusReport* rep) {
    if (!run.active) return;
    const SsaNode& first = block.nodes[run.first_node];
    const SsaNode& last = block.nodes[run.last_node];
    const uint32_t slow =
        (last.old_off + last.old_size) - first.old_off;
    const uint32_t guards = template_guards(run.tmpl);
    const int64_t pred = predicted_cost(run.n_insns, guards, slow);
    const size_t k = static_cast<size_t>(run.tmpl);
    rep->candidates[k]++;
    rep->insns_covered[k] += run.n_insns;
    rep->slow_bytes[k] += slow;
    rep->predicted_total[k] += pred;
    if (pred > rep->predicted_best[k]) rep->predicted_best[k] = pred;
}

// Census one function: walk every SSA block, extend maximal runs of
// same-template matching nodes, split at 8 original instructions.
// Returns false only on an internal invariant break (fail-closed); the
// caller counts the function as rejected coverage.
bool census_function(const Cfg& cfg, const SsaFunc& ssa,
                     RegionCensusReport* rep, std::string* error) {
    for (size_t b = 0; b < ssa.blocks.size(); b++) {
        const SsaBlock& block = ssa.blocks[b];
        Run run;
        run.active = false;
        run.tmpl = Template::COUNT;
        run.first_node = 0;
        run.last_node = 0;
        run.n_insns = 0;
        for (size_t n = 0; n < block.nodes.size(); n++) {
            const SsaNode& node = block.nodes[n];
            if (node.insn >= cfg.insns.size()) {
                *error = "region: internal error: node.insn out of range";
                return false;
            }
            const uint8_t op = static_cast<uint8_t>(cfg.insns[node.insn].op);
            int t = opcode_template(op);
            if (t >= 0 && !node_matches(ssa, node, static_cast<Template>(t))) {
                t = -1;
            }
            if (t < 0) {
                flush_run(run, block, rep);
                run.active = false;
                continue;
            }
            if (!run.active || run.tmpl != static_cast<Template>(t)) {
                flush_run(run, block, rep);
                run.active = true;
                run.tmpl = static_cast<Template>(t);
                run.first_node = static_cast<uint32_t>(n);
                run.n_insns = 0;
            }
            run.last_node = static_cast<uint32_t>(n);
            run.n_insns++;
            if (run.n_insns == 8) {  // region rule: at most 8 originals
                flush_run(run, block, rep);
                run.active = false;
            }
        }
        flush_run(run, block, rep);
    }
    return true;
}

void count_function(const std::vector<FuncInfo>& funcs, uint64_t* count) {
    for (size_t i = 0; i < funcs.size(); i++) {
        (*count)++;
        count_function(funcs[i].children, count);
    }
}

}  // namespace

bool region_round_trip(const uint8_t* data,
                       size_t size,
                       RegionCensusReport* out,
                       std::string* error) {
    std::vector<FuncInfo> functions;
    if (!read_functions(data, size, &functions, error)) return false;
    RegionCensusReport rep;
    rep.functions = 0;
    rep.rejected_functions = 0;
    rep.rejected_insns = 0;
    for (size_t i = 0; i < static_cast<size_t>(Template::COUNT); i++) {
        rep.candidates[i] = 0;
        rep.insns_covered[i] = 0;
        rep.slow_bytes[i] = 0;
        rep.predicted_total[i] = 0;
        rep.predicted_best[i] = INT64_MIN;
    }
    rep.first_templates[0] = Template::COUNT;
    rep.first_templates[1] = Template::COUNT;
    rep.first_candidates[0] = 0;
    rep.first_candidates[1] = 0;
    rep.first_predicted[0] = 0;
    rep.first_predicted[1] = 0;
    count_function(functions, &rep.functions);

    struct Walker {
        const uint8_t* data;
        RegionCensusReport* rep;
        bool run(const FuncInfo& fi, std::string* error) {
            std::vector<Insn> insns;
            if (!decode_function(data + fi.code_off, fi.code_len, data, fi,
                                 &insns, error)) {
                std::fprintf(stderr, "region: rejected (decode): %s\n",
                             error->c_str());
                rep->rejected_functions++;
                error->clear();
                return true;
            }
            Cfg cfg;
            if (!build_cfg(insns, &cfg, error)) {
                std::fprintf(stderr, "region: rejected (build): %s\n",
                             error->c_str());
                rep->rejected_functions++;
                rep->rejected_insns += static_cast<uint64_t>(insns.size());
                error->clear();
                return true;
            }
            cfg.recorded_stack_size = fi.stack_size;
            if (!verify_cfg(cfg, error)) {
                std::fprintf(stderr, "region: rejected (verify): %s\n",
                             error->c_str());
                rep->rejected_functions++;
                rep->rejected_insns += static_cast<uint64_t>(insns.size());
                error->clear();
                return true;
            }
            SsaFunc ssa;
            if (!ssa_analyze_function(cfg, &ssa, error)) {
                std::fprintf(stderr, "region: rejected (ssa): %s\n",
                             error->c_str());
                rep->rejected_functions++;
                rep->rejected_insns += static_cast<uint64_t>(insns.size());
                error->clear();
                return true;
            }
            if (!census_function(cfg, ssa, rep, error)) {
                std::fprintf(stderr, "region: rejected (census): %s\n",
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
    w.rep = &rep;
    for (size_t i = 0; i < functions.size(); i++) {
        if (!w.run(functions[i], error)) return false;
    }

    // Select the at-most-two first templates by predicted cost
    // (ties: insns_covered desc, then template id asc). Only templates
    // with at least one candidate are selectable; empty slots stay
    // Template::COUNT ("none").
    {
        int order[static_cast<int>(Template::COUNT)];
        for (int i = 0; i < static_cast<int>(Template::COUNT); i++) {
            order[i] = i;
        }
        std::sort(order, order + static_cast<int>(Template::COUNT),
                  [&rep](int a, int b) {
                      const int64_t pa = rep.predicted_total[a];
                      const int64_t pb = rep.predicted_total[b];
                      if (pa != pb) return pa > pb;
                      if (rep.insns_covered[a] != rep.insns_covered[b]) {
                          return rep.insns_covered[a] > rep.insns_covered[b];
                      }
                      return a < b;
                  });
        for (int k = 0; k < 2; k++) {
            const int t = order[k];
            if (rep.candidates[t] == 0) break;
            rep.first_templates[k] = static_cast<Template>(t);
            rep.first_candidates[k] = rep.candidates[t];
            rep.first_predicted[k] = rep.predicted_total[t];
        }
    }

    *out = rep;
    return true;
}

}  // namespace ir
}  // namespace bytecode
}  // namespace capsid
