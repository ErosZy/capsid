// Bytecode AOT optimizer for the trusted-bytecode path (design:
// docs/bytecode-aot-optimizer.md). Post-serialization, capsid-side:
// rewrites standard quickjs-ng bytecode (BC_VERSION 26) produced by
// capsid-bytecode-compile before attestation. Never changes the
// vendored VM, the serialized format, or the compatibility identity;
// unmodified runtimes load the output with JS_ReadObject as-is.
//
// Fail-closed contract: any input the optimizer cannot fully parse
// (unknown BCTag, unknown opcode, malformed LEB128, checksum mismatch,
// bounds violation) returns false with a message; the caller aborts
// the compile without producing output files. No silent passthrough.
#ifndef CAPSID_TOOLS_BYTECODE_OPTIMIZE_H
#define CAPSID_TOOLS_BYTECODE_OPTIMIZE_H

#include <cstdint>
#include <string>
#include <vector>

namespace capsid {
namespace bytecode {

// Per-pass enable mask, for attribution and A/B measurements (G4).
// Each bit switches one optimization independently. Passes are never
// on the frozen CLI: the compiler calls with kPassAll.
//
// G4 (26-bundle corpus, 2026-08-23) attributed every pass on the
// committed corpus and trimmed the below-gate ones. v1: P3.2 (const
// strict_eq), P3.4 (push+drop), P3.5 (dup/swap/rot3), P3.6 (const
// condition), P4 (threading), P5 (dead blocks) each contributed
// < 1% — quickjs-ng's own resolve_labels already removes push+drop,
// dup/swap/rot3, and same-block constant conditions — while P2 and
// P3.1 carry the whole measured win. Tier-2: the SSI suite (P9
// construction, P10 SCCP, P11' copy propagation, P12' SSA DCE,
// P13' LICM, P14' form-(b) folds, P15 slot-read CSE) was built,
// measured, and deleted at G4 — its entire net effect on the corpus
// was 2 insns (0.016%): P10's 22 folds on arith-rt duplicate P2's,
// P14' is a byte-identical substitute for P14, and P11'/P12'/P13'/P15
// fired nowhere (P13' hoisted nothing even on the LICM anchor
// fixture). Full adjudication in docs/bytecode-aot-optimizer.md §11.
// The format passes (P6 re-shorten, compaction, verification) always
// run when the optimizer runs and are not separately measurable.
enum PassFlags : uint32_t {
    kPassP2 = 1u << 0,  // cross-BB constant lattice propagation
    kPassP31 = 1u << 1, // const binop folding (add/sub/mul/and/.../mod)
    // Tier-2 direct level (tier-2 plan, docs/bytecode-aot-optimizer.md):
    // linear sweeps over the decoded instruction stream, run before the
    // v1 fixpoint so their folded results feed the lattice.
    kPassP11 = 1u << 2, // copy propagation + dead store materialization
                        // (get_loc s; put_loc t chains; dead stores
                        // vanish as pairs with their feeding get_loc)
    kPassP14 = 1u << 3, // literal get_field fold: OP_object construction
                        // sequences + slot binding, fold get_loc t +
                        // get_field x to the field's known value
    kPassAll = kPassP2 | kPassP31 | kPassP11 | kPassP14,
};

// Optimizes `in` (a serialized quickjs-ng bytecode buffer) into `out`.
// Deterministic: identical input produces identical output. `passes`
// selects which passes run (kPassAll for the deployed pipeline);
// `report` controls the per-pass statistics line on stderr (statistics
// never touch stdout or `out`). `error` receives a human-readable
// reason on failure; `out` is untouched on failure.
bool optimize(const std::vector<std::uint8_t>& in,
              std::vector<std::uint8_t>* out,
              uint32_t passes,
              bool report,
              std::string* error);

// Step 0 analysis: parse and validate, then report foldability
// statistics per function to stderr (never to stdout or `out`).
// Returns false on any parse failure, same contract as optimize().
// The pass pipeline is not run; only reader + decode + pattern scan.
bool analyze_only(const std::vector<std::uint8_t>& in,
                  std::string* error);

}  // namespace bytecode
}  // namespace capsid

#endif  // CAPSID_TOOLS_BYTECODE_OPTIMIZE_H
