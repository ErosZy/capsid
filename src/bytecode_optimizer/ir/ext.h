// F0 ext foundation (tier-3 plan docs/quickjs-ng-cfg-ssa-shape-ic.md
// §2/§4.4): table-generated OP_ext metadata for the BC27 wire format.
// The single source of truth is the vendored quickjs-ext-opcode.h (the
// same include mechanism as quickjs-opcode.h in cfg.cc/effects.cc);
// every optimizer consumer — the strict bundle reader, the two
// independent function decoders, the CFG stack verifier, the identity
// round trips — derives ext sizes, stack effects, and operand formats
// from the one generated table, so a new ext id cannot appear in a
// bundle without the optimizer following. id 0 is reserved and always
// invalid; a removed id keeps its row with size 0 and stays rejected.
#ifndef CAPSID_SRC_BYTECODE_OPTIMIZER_IR_EXT_H
#define CAPSID_SRC_BYTECODE_OPTIMIZER_IR_EXT_H

#include <cstddef>
#include <cstdint>
#include <string>

namespace capsid {
namespace bytecode {
namespace ir {

// Operand formats, mirroring the EXT_FMT lines in the vendored
// quickjs-ext-opcode.h (compile-time divergence: ext.cc expands
// EXT_FMT_##fmt, so a header fmt this enum does not know fails to
// build). Only `none` exists today; the decoders fail closed on any
// other format until its operand handling is implemented.
enum ExtFmt : uint8_t {
    EXT_FMT_none = 0,
    EXT_FMT_COUNT,
};

// One ext instruction row from the table. `size` is the total encoded
// size (OP_ext prefix + ext_id + payload), so a decoder advances past
// the payload without ever interpreting payload bytes as opcodes.
struct ExtInfo {
    uint8_t size;
    uint8_t n_pop;
    uint8_t n_push;
    ExtFmt fmt;
    const char* name;
};

// The BC27 wire version (vendor BC_VERSION_EXT). BC27 is canonical
// only when at least one ext instruction is present in the bundle;
// otherwise the writer emits BC26.
enum { BC_VERSION_EXT = 27 };

// Wire registry (§4.4): OP_ext prefix byte.
enum { OP_EXT = 252 };

// Table lookup. Returns false for id 0, reserved holes, and unknown
// ids; the caller fails closed.
bool ext_lookup(uint8_t id, ExtInfo* out);

// Whole-bundle ext round trip (§3.1 contract, analyze-only): reads the
// bundle (BC26 or BC27), decodes every function with ext-table sizes
// and stack effects, builds and verifies the CFG, and counts ext
// instructions per id. BC26 must contain no OP_ext; BC27 must be
// canonical (>= 1 ext, all ids known, no truncated payloads). Returns
// false only on a whole-bundle parse failure; per-function analysis
// failures are counted as rejected coverage, never skipped silently.
struct ExtRoundTripReport {
    uint64_t functions;
    uint64_t ext_instructions;
    uint64_t per_id[256];  // index = ext id (0 is never valid)
    uint64_t rejected_functions;
    uint64_t rejected_insns;
};

bool ext_round_trip(const uint8_t* data,
                    size_t size,
                    ExtRoundTripReport* out,
                    std::string* error);

}  // namespace ir
}  // namespace bytecode
}  // namespace capsid

#endif  // CAPSID_SRC_BYTECODE_OPTIMIZER_IR_EXT_H
