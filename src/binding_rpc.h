// Binding v1 §5.3: the C++ neutral value between the User and Binding
// Runtimes. Only plain data crosses the boundary — never JSValue,
// objects, or native handles. Cloning rejects functions, promises,
// symbols, accessors (without executing them), cycles, Map/Set/Weak
// collections, Error/RegExp/class instances, exotic proxies and every
// native handle; depth, node count and byte size are bounded.

#ifndef CAPSID_BINDING_RPC_H
#define CAPSID_BINDING_RPC_H

#include "quickjs.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace capsid {

constexpr std::size_t kMaxBindingCloneDepth = 64;
constexpr std::size_t kMaxBindingCloneNodes = 10000;
constexpr std::size_t kMaxBindingCloneBytes = 1024U * 1024U;

struct NeutralValue {
    enum class Kind {
        kUndefined,
        kNull,
        kBool,
        kNumber,
        kBigInt,
        kString,
        kDate,
        kBytes,
        kArray,
        kObject,
    };

    Kind kind = Kind::kUndefined;
    bool bool_value = false;
    double number = 0;
    std::string bigint;   // decimal digits
    std::string string;   // string payload
    double date_ms = 0;   // kDate
    std::vector<std::uint8_t> bytes;  // kBytes
    std::vector<NeutralValue> array;  // kArray
    // kObject: insertion-ordered, duplicate keys rejected at clone time.
    std::vector<std::pair<std::string, NeutralValue>> object;
};

// JS -> neutral. Fails closed with a static diagnostic; `out` is not
// touched on failure.
bool neutral_from_js(JSContext *ctx,
                     JSValueConst value,
                     NeutralValue *out,
                     std::string *error);

// neutral -> JS in the caller's context. The caller owns the result.
JSValue neutral_to_js(JSContext *ctx, const NeutralValue &value);

}  // namespace capsid

#endif
