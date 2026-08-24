// Fuzzer gate for the bytecode AOT optimizer (docs/bytecode-aot-optimizer.md).
// Invariants under ASan/UBSan:
//   1. optimize() never crashes on any input (fail-closed parser).
//   2. A success produces output the optimizer itself can re-parse:
//      optimize(out) must return true (valid BC_VERSION, checksum,
//      tags, opcodes, jump targets, stack heights).
//   3. The pipeline is a fixed point: the second run must be
//      byte-identical (determinism + convergence).
//   4. mask-0 (parse + verify + copy) must also accept a success output.
// R0: emission builds (CAPSID_AOT_EMIT_EXT) may produce BC27 output;
// re-optimizing BC27 fails closed by design (ext sites have no
// foldability consumer), so invariants 2-4 apply to BC26 outputs, and
// BC27 outputs must fail closed with an empty buffer + message on
// every re-entry.
// Corpus: deterministic .qjsb bundles compiled from tests/fixtures.
#include "bytecode_optimizer/bytecode_optimizer.h"

#include <cstdlib>
#include <string>
#include <vector>

namespace {

void exercise(const std::uint8_t* data, size_t size) {
    const std::vector<std::uint8_t> input(data, data + size);
    std::string error;
    std::vector<std::uint8_t> out;
    if (!capsid::bytecode::optimize(input, &out,
                                    capsid::bytecode::kPassAll,
                                    false, &error)) {
        // Fail-closed: no output on failure, message must be present.
        if (!out.empty() || error.empty()) {
            std::abort();
        }
        return;
    }
    if (out.empty()) {
        std::abort();
    }
    if (out[0] == 26) {
        // BC26 output: fixed point — optimizing the output must succeed
        // and change nothing (also re-validates version, checksum and
        // structure); the parse-only pass must accept it too.
        std::string error2;
        std::vector<std::uint8_t> out2;
        if (!capsid::bytecode::optimize(out, &out2,
                                        capsid::bytecode::kPassAll,
                                        false, &error2)) {
            std::abort();
        }
        if (out2 != out) {
            std::abort();
        }
        std::string error3;
        std::vector<std::uint8_t> out3;
        if (!capsid::bytecode::optimize(out, &out3, 0, false, &error3)) {
            std::abort();
        }
        if (out3 != out) {
            std::abort();
        }
    } else {
#if defined(CAPSID_AOT_EMIT_EXT)
        // R0: BC27 output — every re-entry must fail closed (ext sites
        // have no foldability consumer) with the buffer untouched.
        std::string error2;
        std::vector<std::uint8_t> out2;
        if (capsid::bytecode::optimize(out, &out2,
                                       capsid::bytecode::kPassAll,
                                       false, &error2)) {
            std::abort();
        }
        if (!out2.empty() || error2.empty()) {
            std::abort();
        }
        std::string error3;
        std::vector<std::uint8_t> out3;
        if (capsid::bytecode::optimize(out, &out3, 0, false, &error3)) {
            std::abort();
        }
        if (!out3.empty() || error3.empty()) {
            std::abort();
        }
#else
        // Emission-OFF builds never emit BC27.
        std::abort();
#endif
    }
    // Determinism: a fresh run on the same input is byte-identical.
    std::string error4;
    std::vector<std::uint8_t> out4;
    if (!capsid::bytecode::optimize(input, &out4,
                                    capsid::bytecode::kPassAll,
                                    false, &error4)) {
        std::abort();
    }
    if (out4 != out) {
        std::abort();
    }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      size_t size) {
    exercise(data, size);
    return 0;
}
