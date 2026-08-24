// Bytecode AOT optimizer for the trusted-bytecode path (design:
// docs/bytecode-aot-optimizer.md). Post-serialization, capsid-side:
// rewrites standard quickjs-ng bytecode (BC_VERSION 26) produced by
// capsid-bytecode-compile before attestation. Never changes the
// vendored VM or the compatibility identity; unmodified runtimes load
// the output with JS_ReadObject as-is. The deployed optimizer emits
// canonical BC26 only. BC27 is owned by the separate ext/fusion
// foundation and is not emitted until a measured multi-instruction
// template passes its independent keep gate.
//
// Fail-closed contract: any input the optimizer cannot fully parse
// (unknown BCTag, unknown opcode, malformed LEB128, checksum mismatch,
// bounds violation) returns false with a message; the caller aborts
// the compile without producing output files. No silent passthrough.
#ifndef CAPSID_SRC_BYTECODE_OPTIMIZER_BYTECODE_OPTIMIZER_H
#define CAPSID_SRC_BYTECODE_OPTIMIZER_BYTECODE_OPTIMIZER_H

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
// fixture). Full adjudication is in docs/bytecode-aot-optimizer.md.
// Tier-2b returned P12'' as P16, a TDZ-sound dead-store elimination
// that replaces
// the archived P12' whole-slot TDZ guard with precise backward slot
// liveness (set_loc_uninitialized writes the JS_UNINITIALIZED marker
// as the slot value, so plain value liveness is sound). Adjudicated in
// docs/bytecode-aot-optimizer.md.
// The format passes (P6 re-shorten, compaction, verification) always
// run when the optimizer runs and are not separately measurable.
enum PassFlags : uint32_t {
    kPassP2 = 1u << 0,  // cross-BB constant lattice propagation
    kPassP31 = 1u << 1, // const binop folding (add/sub/mul/and/.../mod)
    // Tier-2 direct passes (docs/bytecode-aot-optimizer.md):
    // linear sweeps over the decoded instruction stream, run before the
    // v1 fixpoint so their folded results feed the lattice.
    kPassP11 = 1u << 2, // copy propagation + dead store materialization
                        // (get_loc s; put_loc t chains; dead stores
                        // vanish as pairs with their feeding get_loc)
    kPassP14 = 1u << 3, // literal get_field fold: OP_object construction
                        // sequences + slot binding, fold get_loc t +
                        // get_field x to the field's known value
    kPassP16 = 1u << 4, // TDZ-sound dead store elimination (tier-2b):
                        // removes stores and set_loc_uninitialized
                        // markers to slots dead after the store (plain
                        // backward slot liveness; captured slots and
                        // check-form stores excluded)
    // Tier-3 (tier-3 plan docs/quickjs-ng-opcode-optimization.md):
    // P17/P18 Lane 1 guard-free emission, candidate (a): forward
    // slot-initialization lattice proves every reaching path to a
    // get_loc_check/put_loc_check site has initialized the slot; the
    // check (uninitialized-marker test + ReferenceError branch) can then
    // never fire, so the site is rewritten to the plain op. Existing
    // opcodes only — zero format change; the emitted plain ops are
    // re-shortened by P6 as usual. Functions with unmodeled CFG gates
    // (with_*/eval/catch/gosub/ret) are skipped whole.
    kPassTier3Lane1 = 1u << 5,
    // P18 Lane 2, candidate (c): to_propkey elimination. The interpreter
    // no-ops INT/STRING/SYMBOL keys, so a provably-int key (form A) or a
    // provable key-of-key (form A2) makes the instruction pure dispatch
    // overhead — deletable unconditionally. With a provably non-null
    // base (form C: base class OBJECT/ARRAY from call_constructor) the
    // conversion is redundant with the consumer's internal conversion
    // whenever only pure stack shuffles / constant pushes / provable
    // no-op conversions sit between site and consumer — also deletable.
    // to_propkey has zero stack effect (converts in place) and no aux,
    // so deletion is stack-neutral with no target fixups beyond the
    // standard compaction remap.
    kPassTier3Lane2 = 1u << 6,
    kPassAll = kPassP2 | kPassP31 | kPassP11 | kPassP14 | kPassP16 |
               kPassTier3Lane1 | kPassTier3Lane2,
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

// Benchmark-only classic-script entry. Capsid's product compiler and worker
// remain module-only and call optimize(); this function exists solely so the
// upstream classic-script suites can exercise the identical rewrite pipeline
// without changing their strictness/global semantics. It requires a serialized
// top-level global function and rejects a module record.
bool optimize_classic_for_benchmark(const std::vector<std::uint8_t>& in,
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

// I0 identity gate (tier-3 plan docs/quickjs-ng-cfg-ssa-shape-ic.md
// §3.2): decodes every function of `in` into the bring-up CFG IR and
// re-emits it byte-for-byte identical — code blobs, pc2line tables,
// checksum, bundle. Analyze-only: the production pipeline never calls
// this; functions the CFG cannot model are counted as rejected coverage
// and reported, never skipped silently. Returns false on any identity
// divergence or parse failure, same contract as optimize().
bool cfg_identity_round_trip(const std::vector<std::uint8_t>& in,
                             std::string* error);

// I1 full-stack SSA (tier-3 plan §3.3/§3.4, analyze-only): builds the
// SSA form of every decodable function (block parameters, value
// lattice, one ordered world token, exception successors, refcount
// ownership) and reports coverage to stderr. Nothing is emitted; the
// production pipeline never calls this. Functions the analyses cannot
// prove are counted as rejected coverage and reported, never skipped
// silently. Returns false only on a whole-bundle parse failure, same
// contract as optimize().
bool ssa_analyze(const std::vector<std::uint8_t>& in, std::string* error);

// I2 region census (tier-3 plan §4, analyze-only): matches candidate
// fusion regions (the §4.2 template catalog) on the SSA form of every
// decodable function and reports static coverage, guard
// requirements, slow-path duplication, and the §4.1 predicted cost to
// stderr, selecting the at-most-two first templates by predicted
// total. The lower-level region_round_trip_profiled API performs dynamic
// exact-site weighting for decisions. Nothing is emitted; the production
// pipeline never calls this.
// Functions the analyses cannot prove are counted as rejected coverage
// and reported, never skipped silently. Returns false only on a
// whole-bundle parse failure, same contract as optimize().
bool region_census(const std::vector<std::uint8_t>& in, std::string* error);

// F0 ext foundation (tier-3 plan §2/§4.4, analyze-only): BC27 dual
// reader + ext round trip. Reads BC26 and BC27 bundles, decodes every
// function with ext-table sizes and stack effects (table-generated
// OP_ext from quickjs-ext-opcode.h, single source of truth), builds
// and verifies the CFG, and reports ext coverage to stderr. BC26
// containing OP_ext, BC27 without ext (noncanonical), unknown ext ids,
// truncated payloads, and unimplemented operand formats fail closed at
// the reader or the decoder. Nothing is emitted; the production
// pipeline (optimize / analyze_only) rejects BC27 input outright.
bool ext_round_trip(const std::vector<std::uint8_t>& in, std::string* error);

}  // namespace bytecode
}  // namespace capsid

#endif  // CAPSID_SRC_BYTECODE_OPTIMIZER_BYTECODE_OPTIMIZER_H
