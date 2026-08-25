// I2 region census (docs/quickjs-optimization.md §5): matches candidate
// fusion regions on the SSA form and reports
// static and dynamic-profile-weighted coverage, guard requirements,
// slow-path duplication, and predicted cost. Analyze-only — nothing is emitted and the
// production pipeline never calls this. The census's job is to answer
// "which templates, on this corpus, justify the cost of a guarded
// region" before any lowering work; it selects at most two first
// templates (§4.2 catalog) by the predicted cost model of §4.1.
//
// Region rules (§4.3): single-basic-block, maximum eight original
// instructions, never crossing a call, an unknown heap effect, an
// exception handler boundary, a suspension, a backedge, or a
// safepoint. A candidate contains at least two original operations; a
// single heavy opcode belongs to quickening/IC, not OP_ext fusion. The
// matched chain is a maximal run of template-matching
// SSA nodes; the fast body is the chain, the slow path is the
// original sequence retained verbatim (its byte size is the
// duplication cost).
#ifndef CAPSID_SRC_BYTECODE_OPTIMIZER_IR_REGION_H
#define CAPSID_SRC_BYTECODE_OPTIMIZER_IR_REGION_H

#include <cstdint>
#include <string>
#include <vector>

namespace capsid {
namespace bytecode {
namespace ir {

// The §4.2 template catalog. Ids are fixed (never renumbered); an id
// is only ever added with a handler and verifier metadata, which this
// census does not provide — it only measures demand.
enum class Template : uint8_t {
    FAST_ARRAY_GET_I32 = 0,    // get_array_el, provably-int index
    FAST_ARRAY_UPDATE_NUM = 1, // put_array_el, provably-int index
    SHAPE_GET_OWN = 2,         // get_field, own-data shape guard
    SHAPE_PUT_OWN = 3,         // put_field, own-data shape guard
    I32_ARITH_CHAIN = 4,       // provably-int32 binop runs
    F64_ARITH_CHAIN = 5,       // provably-f64 binop runs
    COUNT,
};

const char* template_name(Template t);

// Runtime guard count the fast body of template `t` must execute
// before mutating anything (§4.3: all guards run first; the fast body
// is non-throwing after its guards). Arith chains prove their operands
// from the lattice (0); the property templates pay class/shape checks.
uint32_t template_guards(Template t);

// One matched candidate. first_insn/last_insn index Cfg::insns; the
// slow-path duplication cost is the byte extent of that original
// range. `predicted` is the §4.1 score: avoided dispatches + avoided
// tag/class checks - guards - ext secondary dispatch - I-cache
// penalty, in dispatch-equivalent units (positive = worth a second
// look).
struct RegionCandidate {
    Template tmpl;
    uint32_t func;       // function index in the bundle
    uint32_t block;      // SSA block id
    uint32_t first_insn; // Cfg::insns index
    uint32_t last_insn;  // inclusive
    uint32_t n_insns;    // original instructions covered (<= 8)
    uint32_t n_guards;   // runtime guards the fast path needs
    uint32_t slow_bytes; // retained slow-path byte extent
    int64_t predicted;   // §4.1 score, dispatch-equivalent units
};

// Exact dynamic execution evidence for a serialized function/site. Function
// ids are the preorder indices used by read_functions; pc is the original
// bytecode offset. A region's execution weight is the minimum count of its
// member sites (the conservative path bottleneck).
struct RegionSiteExecution {
    uint32_t function;
    uint32_t pc;
    uint64_t executions;
};

struct RegionExecutionProfile {
    std::vector<RegionSiteExecution> sites;
};

// Aggregate census over one bundle. Per-template totals plus the
// selection. The reject counters follow the ssa_round_trip contract:
// functions the pipeline cannot analyze are reported, never skipped.
struct RegionCensusReport {
    uint64_t functions;
    uint64_t rejected_functions;
    uint64_t rejected_insns;
    uint64_t candidates[static_cast<size_t>(Template::COUNT)];
    uint64_t insns_covered[static_cast<size_t>(Template::COUNT)];
    uint64_t slow_bytes[static_cast<size_t>(Template::COUNT)];
    int64_t predicted_total[static_cast<size_t>(Template::COUNT)];
    int64_t predicted_best[static_cast<size_t>(Template::COUNT)];
    // Dynamic totals are zero for the legacy static entry point. They count
    // actual region executions, covered instruction executions, and weighted
    // predicted benefit from an exact-site RegionExecutionProfile.
    uint64_t dynamic_candidates[static_cast<size_t>(Template::COUNT)];
    uint64_t dynamic_insns_covered[static_cast<size_t>(Template::COUNT)];
    int64_t dynamic_predicted_total[static_cast<size_t>(Template::COUNT)];
    uint64_t missing_profile_sites;
    bool has_dynamic_profile;
    // The at-most-two selection, in order; Template::COUNT = none.
    Template first_templates[2];
    uint64_t first_candidates[2];
    int64_t first_predicted[2];
};

// Whole-bundle analyze-only walker (§3.1): read_functions +
// per-function decode/build/verify/SSA, then the census matcher over
// every SSA block. Returns false only on a whole-bundle parse
// failure; per-function failures are counted as rejected coverage.
bool region_round_trip(const uint8_t* data,
                       size_t size,
                       RegionCensusReport* out,
                       std::string* error);

// Dynamic decision entry point. Static candidates are still reported, but
// template selection uses dynamic_predicted_total. A member missing from the
// supplied exact-site profile gives that region weight zero and increments
// missing_profile_sites; evidence never silently falls back to static counts.
bool region_round_trip_profiled(const uint8_t* data,
                                size_t size,
                                const RegionExecutionProfile& profile,
                                RegionCensusReport* out,
                                std::string* error);

}  // namespace ir
}  // namespace bytecode
}  // namespace capsid

#endif  // CAPSID_SRC_BYTECODE_OPTIMIZER_IR_REGION_H
