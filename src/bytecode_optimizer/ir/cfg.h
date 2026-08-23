// I0: formal CFG over decoded quickjs-ng BC26 function code.
//
// This is the bring-up IR of the tier-3 plan (docs/quickjs-ng-cfg-ssa-
// shape-ic.md §3.1/§3.2): blocks with explicit edge classes, per-
// instruction metadata (PC, operands, stack effect, source location,
// may-throw, ownership/result kind, effect class), and the mandatory
// identity gate — decode -> CFG -> emit with optimization disabled must
// reproduce every canonical BC26 code section, pc2line table, checksum,
// and bundle byte-for-byte. Until that gate passes on the corpus, the
// module is analyze-only: nothing here is linked into the optimization
// pipeline, and functions the CFG cannot model are counted as rejected
// coverage (their bundles stay byte-for-byte BC26).
//
// Fail-closed contract: unknown opcodes, unclassifiable semantics,
// malformed control flow, and inconsistent stack heights abort analysis
// with an error message. The classification default is conservative
// (may throw, CALL effect, ANY result), so an unclassified opcode can
// never make the analysis unsound — only less precise.
#ifndef CAPSID_SRC_BYTECODE_OPTIMIZER_IR_CFG_H
#define CAPSID_SRC_BYTECODE_OPTIMIZER_IR_CFG_H

#include <cstdint>
#include <string>
#include <vector>

namespace capsid {
namespace bytecode {
namespace ir {

// ---------------------------------------------------------------------------
// Instruction classification (I0 metadata columns).
// ---------------------------------------------------------------------------

// Effect strength ordering (weakest -> strongest). The I1 world/effect
// token joins by max so that a candidate can never move across a
// stronger effect: PURE < LOCAL < MEMORY < CALL < ALLOC < BARRIER <
// SUSPEND. CONTROL and TERMINAL are structural classes (branches and
// terminators do not "have" effects to move across; they shape the
// graph). This ordering is the contract I1 builds on.
enum class EffectClass : uint8_t {
    PURE = 0,    // stack/scalar ops with no observable state change
    LOCAL,       // non-captured frame-local slot access (loc/arg)
    MEMORY,      // heap/global/shared-storage reads and writes
    CALL,        // may execute arbitrary code (calls, iterators, eval,
                 // getters/proxies reachable from property access)
    ALLOC,       // materializes heap objects (array/object/closure/...)
    CONTROL,     // branches, catch/gosub dispatch
    BARRIER,     // with_*/eval: invalidates all local knowledge
    SUSPEND,     // yield/await: suspension/resume point (safepoint)
    TERMINAL,    // return*/throw: no fallthrough edge
};

// What the instruction's pushed value can be at runtime — the seed of
// the §3.3 value lattice. SCALAR is provably non-heap (null/undefined/
// booleans/small ints/typeof results); HEAP is provably a heap
// reference (object/array/closure/string/regexp/class materialization);
// ANY is the conservative default.
enum class ResultKind : uint8_t {
    SCALAR = 0,
    HEAP,
    ANY,
};

// Explicit edge classes (§3.2). A FALLTHROUGH edge is the not-taken
// continuation of a conditional jump or the plain next-instruction flow
// of a non-terminator. Every edge carries a `backedge` flag (target
// precedes source in linear order = loop), set conservatively.
enum class EdgeKind : uint8_t {
    FALLTHROUGH,  // next instruction (incl. conditional not-taken)
    JUMP,         // unconditional / conditional-taken branch
    CATCH,        // exception-handler edge (OP_catch target)
    GOSUB,        // finally entry edge (OP_gosub target)
    RET,          // dynamic return from finally (OP_ret; no static
                  // target — paired with gosub sites by I1)
    SUSPEND,      // yield/await suspension edge (resume re-enters at
                  // the target with the suspended frame)
    BARRIER,      // with_* dynamic-scope edge (re-binds names)
};

struct Edge {
    EdgeKind kind;
    uint32_t to;       // target block id
    uint32_t src_insn; // source instruction index within the block
                       // (the verifier seeds the target with the post
                       // height at exactly this instruction)
    bool backedge;     // to precedes `from` in linear order (loop)
};

// One decoded instruction. old_off/old_size are the original byte
// position/extent in the code blob (the "original PC"); target is the
// resolved jump destination as an instruction index, or INT32_MIN for
// non-jump instructions (a real jump can never resolve to that value,
// so a computed -1 target is a fail-closed range error, not a
// sentinel); imm/aux/has_aux carry the decoded operands. The I0
// metadata columns (n_pop/n_push, src_line/src_col, may_throw, effect,
// result) are derived at decode time from the vendor opcode table and
// the classification.
struct Insn {
    uint16_t op;
    uint32_t old_off;
    uint8_t old_size;
    int32_t target;    // resolved insn index for jumps, else INT32_MIN
    int64_t imm;       // small-int value (push_minus1..push_i32)
    uint32_t aux;      // cpool idx / slot idx / argc / atom / label
    bool has_aux;
    uint8_t n_pop;     // static pop count (npop/npopx fold aux in)
    uint8_t n_push;
    uint16_t src_line; // pc2line-resolved line (0 = unknown)
    uint16_t src_col;
    bool may_throw;
    EffectClass effect;
    ResultKind result;
};

struct Block {
    uint32_t start;      // first insn index
    uint32_t end;        // one past the last insn
    std::vector<Edge> edges;  // outgoing, in emission order
    bool has_ret;        // contains OP_ret (finally block, dynamic target)
    bool reachable;      // from entry via the edge graph
    int32_t entry_height;  // verified stack height at block entry
};

struct Cfg {
    std::vector<Insn> insns;
    std::vector<Block> blocks;
    uint32_t recorded_stack_size;  // function record value
    uint32_t max_height;           // verified max over all paths
};

// ---------------------------------------------------------------------------
// Bundle-level interface.
// ---------------------------------------------------------------------------

// A parsed function record: the extents needed to locate and re-emit
// every section of the canonical BC26 record (code blob, pc2line debug
// block, children). Filled by read_functions (bridge implemented in
// bytecode_optimizer.cc, which owns the strict bundle reader).
struct FuncInfo {
    uint32_t code_off;   // start of the code blob (offset into bundle)
    uint32_t code_len;
    uint32_t dbg_pc2line_off;  // pc2line blob offset (0 if absent)
    uint32_t dbg_pc2line_len;
    int32_t dbg_line;    // pc2line base line/col
    int32_t dbg_col;
    uint32_t stack_size; // recorded max stack height
    std::vector<FuncInfo> children;
};

// Parses and validates the whole bundle (version, checksum, atom table,
// module record) and flattens the function tree. Same fail-closed
// contract as the optimizer's parse: any violation returns false with a
// message and `out` is untouched.
bool read_functions(const uint8_t* data,
                    size_t size,
                    std::vector<FuncInfo>* out,
                    std::string* error);

// ---------------------------------------------------------------------------
// I0 core.
// ---------------------------------------------------------------------------

// Decodes one function's code blob into Insn records with full I0
// metadata. Resolves every jump operand to an instruction index
// (self-relative target = operand start + signed offset; labels were
// removed by the compiler, so targets may point at any instruction
// boundary). Fails closed on unknown opcodes, truncation, and jump
// targets that are out of range or not on an instruction boundary.
bool decode_function(const uint8_t* code,
                     size_t len,
                     const uint8_t* bundle,
                     const FuncInfo& fi,
                     std::vector<Insn>* out,
                     std::string* error);

// Builds the CFG from decoded instructions: block leaders (entry, jump
// targets, post-gosub return points), explicit edge classes, and
// conservative backedge flags. Fails closed on control that falls off
// the end of the blob.
bool build_cfg(const std::vector<Insn>& insns, Cfg* out, std::string* error);

// Stack verifier over the CFG: breadth-first height propagation with
// per-block entry-height consistency (the existing verifier's contract,
// now block-granular), underflow checks, max height vs the recorded
// stack size, and edge-target validation.
bool verify_cfg(const Cfg& cfg, std::string* error);

// Identity lowering: re-encodes the decoded stream with its original
// opcodes and forms. Jump distances are recomputed from the resolved
// targets and must equal the original encoded distances (fail-closed
// otherwise); operand bytes are re-encoded for the classes the
// pipeline understands and copied verbatim otherwise. The output must
// equal the original blob byte-for-byte — the identity gate asserts
// that in tests and in the corpus round trip.
bool emit_identity(const std::vector<Insn>& insns,
                   const uint8_t* old_code,
                   size_t old_len,
                   std::vector<uint8_t>* out,
                   std::string* error);

// Whole-bundle identity gate (§3.2): read_functions + per-function
// decode -> CFG -> verify -> emit_identity, byte-comparing every code
// blob, validating every pc2line blob decodes and stays in range, then
// re-parsing the bundle (version/checksum/atoms) as the final check.
// Unsupported functions are counted as rejected coverage, not skipped
// silently. Always analyze-only: never emits a production bundle.
struct IdentityReport {
    uint64_t functions;           // total functions seen
    uint64_t insns;               // total instructions decoded
    uint64_t rejected_functions;  // whole-function rejects
    uint64_t rejected_insns;      // instructions inside rejected functions
    uint64_t missing_pc2line;     // functions with no pc2line debug block
};
bool identity_round_trip(const uint8_t* data,
                         size_t size,
                         IdentityReport* out,
                         std::string* error);

}  // namespace ir
}  // namespace bytecode
}  // namespace capsid

#endif  // CAPSID_SRC_BYTECODE_OPTIMIZER_IR_CFG_H
