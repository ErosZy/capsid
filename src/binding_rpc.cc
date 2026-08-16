// Binding v1 §5.3 structured clone between the User and Binding Runtimes.
//
// Only plain data crosses the boundary. The clone walks own enumerable
// properties via JS_GetOwnProperty (which returns accessor functions
// without invoking them — getters are detected and rejected, never
// executed). Proxies are rejected by class identity before any property
// read can trigger a trap; Map/Set/Weak collections, Error, RegExp and
// custom class instances are rejected by instanceof/class checks.

#include "binding_rpc.h"

#include "quickjs.h"

#include <cctype>
#include <cstring>
#include <string>
#include <unordered_set>
#include <utility>

namespace capsid {
namespace {

struct CloneContext {
    JSContext *ctx;
    std::size_t depth;
    std::size_t nodes;
    std::size_t bytes;
    std::unordered_set<const void *> ancestors;  // cycle detection
    JSValue object_proto;   // Object.prototype, referenced
};

bool clone_value(JSContext *ctx,
                 JSValueConst value,
                 NeutralValue *out,
                 std::string *error,
                 CloneContext *context);

bool fail_closed(std::string *error, const char *message) {
    if (error != NULL) {
        *error = message;
    }
    return false;
}

// A fresh object literal reaches the runtime's intrinsic Object.prototype
// without consulting a mutable global constructor. Every exotic kind uses
// QuickJS's native predicate below; no attacker-controlled Symbol.hasInstance
// or replaced global constructor participates in type classification.
bool learn_class_ids(JSContext *ctx, CloneContext *context) {
    // Class instances share the plain-object class id in this quickjs-ng,
    // so instance detection uses the prototype: a plain object's prototype
    // is Object.prototype (or null). The Proxy class id stays class-based
    // because a proxy's prototype mirrors its target's.
    JSValue plain = JS_Eval(
        ctx, "({})", 4, "<clone-probe>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(plain)) {
        JS_FreeValue(ctx, plain);
        return false;
    }
    context->object_proto = JS_GetPrototype(ctx, plain);
    JS_FreeValue(ctx, plain);
    if (JS_IsException(context->object_proto)) {
        JS_FreeValue(ctx, context->object_proto);
        return false;
    }
    // JS_GetPrototype returned a new reference; the context owns exactly
    // one and frees it in neutral_from_js.
    return true;
}

bool clone_string(JSContext *ctx,
                  JSValueConst value,
                  std::string *target,
                  std::string *error,
                  CloneContext *context) {
    const char *text = JS_ToCString(ctx, value);
    if (text == NULL) {
        return fail_closed(error, "unprintable string");
    }
    const std::size_t size = std::strlen(text);
    if (size > kMaxBindingCloneBytes - context->bytes) {
        JS_FreeCString(ctx, text);
        return fail_closed(error, "clone byte limit exceeded");
    }
    target->assign(text, size);
    JS_FreeCString(ctx, text);
    context->bytes += size;
    return true;
}

bool reject_enumerable_symbol_keys(JSContext *ctx,
                                   JSValueConst value,
                                   std::string *error) {
    JSPropertyEnum *properties = NULL;
    uint32_t count = 0;
    if (JS_GetOwnPropertyNames(
            ctx,
            &properties,
            &count,
            value,
            JS_GPN_ENUM_ONLY | JS_GPN_SYMBOL_MASK) < 0) {
        return fail_closed(error, "object properties are unreadable");
    }
    JS_FreePropertyEnum(ctx, properties, count);
    return count == 0 ||
           fail_closed(error, "symbol property keys are not cloneable");
}

bool clone_value(JSContext *ctx,
                 JSValueConst value,
                 NeutralValue *out,
                 std::string *error,
                 CloneContext *context) {
    if (context->depth > kMaxBindingCloneDepth) {
        return fail_closed(error, "clone depth limit exceeded");
    }
    if (++context->nodes > kMaxBindingCloneNodes) {
        return fail_closed(error, "clone node limit exceeded");
    }

    if (JS_IsUndefined(value)) {
        out->kind = NeutralValue::Kind::kUndefined;
        return true;
    }
    if (JS_IsNull(value)) {
        out->kind = NeutralValue::Kind::kNull;
        return true;
    }
    if (JS_IsBool(value)) {
        out->kind = NeutralValue::Kind::kBool;
        out->bool_value = JS_ToBool(ctx, value) != 0;
        return true;
    }
    if (JS_IsNumber(value)) {
        out->kind = NeutralValue::Kind::kNumber;
        if (JS_ToFloat64(ctx, &out->number, value) != 0) {
            return fail_closed(error, "unrepresentable number");
        }
        return true;
    }
    if (JS_IsBigInt(value)) {
        out->kind = NeutralValue::Kind::kBigInt;
        if (!clone_string(ctx, value, &out->bigint, error, context)) {
            return false;
        }
        return true;
    }
    if (JS_IsString(value)) {
        out->kind = NeutralValue::Kind::kString;
        return clone_string(ctx, value, &out->string, error, context);
    }
    if (JS_IsSymbol(value)) {
        return fail_closed(error, "symbol values are not cloneable");
    }
    if (JS_IsDate(value)) {
        out->kind = NeutralValue::Kind::kDate;
        // Read the internal [[DateValue]] slot directly. Looking up or
        // calling value.getTime would execute an attacker-controlled own
        // property/prototype override during the cross-runtime clone.
        if (JS_GetDateValue(ctx, value, &out->date_ms) != 0) {
            return fail_closed(error, "date value is unreadable");
        }
        return true;
    }
    if (JS_IsError(value)) {
        return fail_closed(error, "Error values are not cloneable");
    }
    if (JS_IsRegExp(value)) {
        return fail_closed(error, "RegExp values are not cloneable");
    }
    if (JS_IsPromise(value)) {
        return fail_closed(error, "promise values are not cloneable");
    }
    if (JS_IsFunction(ctx, value)) {
        return fail_closed(error, "function values are not cloneable");
    }
    if (!JS_IsObject(value)) {
        return fail_closed(error, "value kind is not cloneable");
    }

    // Native objects before generic object handling.
    if (JS_IsArrayBuffer(value)) {
        out->kind = NeutralValue::Kind::kBytes;
        size_t size = 0;
        uint8_t *data = JS_GetArrayBuffer(ctx, &size, value);
        if (data == NULL) {
            return fail_closed(error, "array buffer is unreadable");
        }
        if (size > kMaxBindingCloneBytes - context->bytes) {
            return fail_closed(error, "clone byte limit exceeded");
        }
        out->bytes.assign(data, data + size);
        context->bytes += size;
        return true;
    }
    // Only Uint8Array is cloneable; JS_GetUint8Array raises on other
    // values, so the instanceof gate runs first. Every other typed array
    // falls through to the prototype check below and is rejected as a
    // class instance.
    if (JS_GetTypedArrayType(value) == JS_TYPED_ARRAY_UINT8) {
        out->kind = NeutralValue::Kind::kBytes;
        size_t typed_size = 0;
        uint8_t *typed = JS_GetUint8Array(ctx, &typed_size, value);
        if (typed == NULL) {
            return fail_closed(error, "byte array is unreadable");
        }
        if (typed_size > kMaxBindingCloneBytes - context->bytes) {
            return fail_closed(error, "clone byte limit exceeded");
        }
        out->bytes.assign(typed, typed + typed_size);
        context->bytes += typed_size;
        return true;
    }

    if (JS_IsProxy(value)) {
        return fail_closed(error, "Proxy values are not cloneable");
    }
    if (JS_IsMap(value)) {
        return fail_closed(error, "Map values are not cloneable");
    }
    if (JS_IsSet(value)) {
        return fail_closed(error, "Set values are not cloneable");
    }
    if (JS_IsWeakMap(value)) {
        return fail_closed(error, "WeakMap values are not cloneable");
    }
    if (JS_IsWeakSet(value)) {
        return fail_closed(error, "WeakSet values are not cloneable");
    }
    if (!reject_enumerable_symbol_keys(ctx, value, error)) {
        return false;
    }
    // A plain object's prototype is Object.prototype or null; anything
    // else is a class instance. Arrays are handled by their own branch
    // below and keep Array.prototype.
    if (!JS_IsArray(value)) {
        JSValue proto = JS_GetPrototype(ctx, value);
        const bool plain = !JS_IsException(proto) &&
                           (JS_IsNull(proto) ||
                            JS_VALUE_GET_PTR(proto) ==
                                JS_VALUE_GET_PTR(context->object_proto));
        JS_FreeValue(ctx, proto);
        if (!plain) {
            return fail_closed(error,
                               "class instance values are not cloneable");
        }
    }

    // Cycle detection by object identity along the current path.
    const void *identity = static_cast<const void *>(
        JS_VALUE_GET_PTR(value));
    if (!context->ancestors.insert(identity).second) {
        return fail_closed(error, "cyclic values are not cloneable");
    }

    if (JS_IsArray(value)) {
        out->kind = NeutralValue::Kind::kArray;
        uint32_t length = 0;
        JSValue length_value = JS_GetPropertyStr(ctx, value, "length");
        if (JS_IsException(length_value) ||
            JS_ToUint32(ctx, &length, length_value) != 0) {
            JS_FreeValue(ctx, length_value);
            context->ancestors.erase(identity);
            return fail_closed(error, "array length is unreadable");
        }
        JS_FreeValue(ctx, length_value);
        ++context->depth;
        out->array.reserve(length);
        for (uint32_t index = 0; index < length; ++index) {
            JSAtom atom = JS_NewAtomUInt32(ctx, index);
            JSPropertyDescriptor descriptor;
            const int present =
                JS_GetOwnProperty(ctx, &descriptor, value, atom);
            JS_FreeAtom(ctx, atom);
            if (present < 0) {
                --context->depth;
                context->ancestors.erase(identity);
                return fail_closed(error,
                                   "array element is unreadable");
            }
            JSValue element = JS_UNDEFINED;
            if (present > 0) {
                const bool is_accessor =
                    !JS_IsUndefined(descriptor.getter) ||
                    !JS_IsUndefined(descriptor.setter);
                element = descriptor.value;
                JS_FreeValue(ctx, descriptor.getter);
                JS_FreeValue(ctx, descriptor.setter);
                if (is_accessor) {
                    JS_FreeValue(ctx, element);
                    --context->depth;
                    context->ancestors.erase(identity);
                    return fail_closed(
                        error, "accessor properties are not cloneable");
                }
            }
            NeutralValue child;
            const bool ok = clone_value(
                ctx, element, &child, error, context);
            JS_FreeValue(ctx, element);
            if (!ok) {
                --context->depth;
                context->ancestors.erase(identity);
                return false;
            }
            out->array.push_back(std::move(child));
        }
        --context->depth;
        context->ancestors.erase(identity);
        return true;
    }

    out->kind = NeutralValue::Kind::kObject;
    JSPropertyEnum *tab = NULL;
    uint32_t count = 0;
    if (JS_GetOwnPropertyNames(ctx, &tab, &count, value,
                               JS_GPN_ENUM_ONLY | JS_GPN_STRING_MASK) < 0) {
        context->ancestors.erase(identity);
        return fail_closed(error, "object properties are unreadable");
    }
    ++context->depth;
    for (uint32_t index = 0; index < count; ++index) {
        const char *key = JS_AtomToCString(ctx, tab[index].atom);
        if (key == NULL) {
            continue;
        }
        const std::string key_string(key);
        JS_FreeCString(ctx, key);
        if (key_string.empty()) {
            continue;
        }
        if (key_string.size() > kMaxBindingCloneBytes - context->bytes) {
            JS_FreePropertyEnum(ctx, tab, count);
            --context->depth;
            context->ancestors.erase(identity);
            return fail_closed(error, "clone byte limit exceeded");
        }
        context->bytes += key_string.size();
        // JS_GetOwnProperty returns accessors as functions without
        // invoking them; anything function-shaped here is a getter/setter
        // and is rejected (never executed).
        JSPropertyDescriptor descriptor;
        if (JS_GetOwnProperty(ctx, &descriptor, value, tab[index].atom) <
            0) {
            JS_FreePropertyEnum(ctx, tab, count);
            --context->depth;
            context->ancestors.erase(identity);
            return fail_closed(error, "object property is unreadable");
        }
        const bool is_accessor =
            !JS_IsUndefined(descriptor.getter) ||
            !JS_IsUndefined(descriptor.setter);
        const JSValue member = descriptor.value;
        if (is_accessor) {
            JS_FreeValue(ctx, descriptor.getter);
            JS_FreeValue(ctx, descriptor.setter);
            JS_FreeValue(ctx, member);
            JS_FreePropertyEnum(ctx, tab, count);
            --context->depth;
            context->ancestors.erase(identity);
            return fail_closed(error,
                               "accessor properties are not cloneable");
        }
        JS_FreeValue(ctx, descriptor.getter);
        JS_FreeValue(ctx, descriptor.setter);
        NeutralValue child;
        const bool ok =
            clone_value(ctx, member, &child, error, context);
        JS_FreeValue(ctx, member);
        if (!ok) {
            JS_FreePropertyEnum(ctx, tab, count);
            --context->depth;
            context->ancestors.erase(identity);
            return false;
        }
        out->object.push_back(
            std::make_pair(key_string, std::move(child)));
    }
    JS_FreePropertyEnum(ctx, tab, count);
    --context->depth;
    context->ancestors.erase(identity);
    return true;
}

}  // namespace

JSValue neutral_to_js(JSContext *ctx, const NeutralValue &value) {
    switch (value.kind) {
    case NeutralValue::Kind::kUndefined:
        return JS_UNDEFINED;
    case NeutralValue::Kind::kNull:
        return JS_NULL;
    case NeutralValue::Kind::kBool:
        return value.bool_value ? JS_TRUE : JS_FALSE;
    case NeutralValue::Kind::kNumber: {
        return JS_NewFloat64(ctx, value.number);
    }
    case NeutralValue::Kind::kBigInt: {
        std::size_t offset = 0;
        if (!value.bigint.empty() &&
            (value.bigint[0] == '-' || value.bigint[0] == '+')) {
            offset = 1;
        }
        if (offset == value.bigint.size()) {
            return JS_ThrowSyntaxError(ctx,
                                       "invalid neutral bigint");
        }
        for (; offset < value.bigint.size(); ++offset) {
            if (!std::isdigit(
                    static_cast<unsigned char>(value.bigint[offset]))) {
                return JS_ThrowSyntaxError(ctx,
                                           "invalid neutral bigint");
            }
        }
        // QuickJS's JS_ToBigInt64 intentionally truncates modulo 2^64. Use a
        // validated decimal literal so arbitrary precision survives exactly.
        const std::string literal =
            "(" + value.bigint + "n)";
        return JS_Eval(
            ctx, literal.data(), literal.size(), "<binding-bigint>",
            JS_EVAL_TYPE_GLOBAL);
    }
    case NeutralValue::Kind::kString:
        return JS_NewStringLen(
            ctx, value.string.data(), value.string.size());
    case NeutralValue::Kind::kDate:
        return JS_NewDate(ctx, value.date_ms);
    case NeutralValue::Kind::kBytes: {
        JSValue array = JS_NewUint8ArrayCopy(
            ctx, value.bytes.data(), value.bytes.size());
        return array;
    }
    case NeutralValue::Kind::kArray: {
        JSValue array = JS_NewArray(ctx);
        if (JS_IsException(array)) {
            return array;
        }
        for (std::size_t index = 0; index < value.array.size(); ++index) {
            JSValue child = neutral_to_js(ctx, value.array[index]);
            if (JS_IsException(child)) {
                JS_FreeValue(ctx, array);
                return child;
            }
            if (JS_SetPropertyUint32(
                    ctx, array, static_cast<uint32_t>(index), child) < 0) {
                JS_FreeValue(ctx, array);
                return JS_EXCEPTION;
            }
        }
        return array;
    }
    case NeutralValue::Kind::kObject: {
        JSValue object = JS_NewObjectProto(ctx, JS_NULL);
        if (JS_IsException(object)) {
            return object;
        }
        for (const auto &entry : value.object) {
            JSValue child = neutral_to_js(ctx, entry.second);
            if (JS_IsException(child)) {
                JS_FreeValue(ctx, object);
                return child;
            }
            if (JS_SetPropertyStr(
                    ctx, object, entry.first.c_str(), child) < 0) {
                JS_FreeValue(ctx, object);
                return JS_EXCEPTION;
            }
        }
        return object;
    }
    }
    return JS_ThrowInternalError(ctx, "invalid neutral value kind");
}

bool neutral_from_js(JSContext *ctx,
                     JSValueConst value,
                     NeutralValue *out,
                     std::string *error) {
    if (ctx == NULL || out == NULL) {
        return fail_closed(error, "clone arguments are invalid");
    }
    if (error != NULL) {
        error->clear();
    }
    CloneContext context;
    context.ctx = ctx;
    context.depth = 0;
    context.nodes = 0;
    context.bytes = 0;
    if (!learn_class_ids(ctx, &context)) {
        return fail_closed(error, "clone class probe failed");
    }
    NeutralValue result;
    const bool ok =
        clone_value(ctx, value, &result, error, &context);
    JS_FreeValue(ctx, context.object_proto);
    if (ok) {
        *out = std::move(result);
    }
    return ok;
}

JSValue binding_call_id_to_js(JSContext *ctx, std::uint64_t call_id) {
    if (ctx == NULL || call_id == 0) {
        return ctx != NULL
                   ? JS_ThrowRangeError(ctx, "binding call id zero is invalid")
                   : JS_EXCEPTION;
    }
    return JS_NewBigUint64(ctx, call_id);
}

bool binding_call_id_from_js(JSContext *ctx,
                             JSValueConst value,
                             std::uint64_t *call_id) {
    if (ctx == NULL || call_id == NULL || !JS_IsBigInt(value)) {
        return false;
    }
    std::uint64_t decoded = 0;
    if (JS_ToBigUint64(ctx, &decoded, value) != 0) {
        JSValue exception = JS_GetException(ctx);
        JS_FreeValue(ctx, exception);
        return false;
    }
    if (decoded == 0) {
        return false;
    }
    *call_id = decoded;
    return true;
}

}  // namespace capsid
