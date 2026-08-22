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
// Each bit switches one optimization independently; bit 0 is the
// format-level passes (P6 re-shorten, compaction, verification) which
// always run when the optimizer runs and are not separately
// measurable. Passes are never on the frozen CLI: the compiler calls
// with kPassAll.
enum PassFlags : uint32_t {
    kPassP2 = 1u << 0,   // cross-BB constant lattice propagation
    kPassP31 = 1u << 1,  // const binop folding (add/sub/mul/and/.../mod)
    kPassP32 = 1u << 2,  // const strict_eq / strict_neq folding
    kPassP34 = 1u << 3,  // push + drop removal
    kPassP35 = 1u << 4,  // dup/dup2/swap/rot3 peepholes
    kPassP36 = 1u << 5,  // const conditional-jump folding
    kPassP4 = 1u << 6,   // goto-chain threading
    kPassP5 = 1u << 7,   // unreachable block elimination
    kPassAll = 0xFFFFFFFFu,
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
