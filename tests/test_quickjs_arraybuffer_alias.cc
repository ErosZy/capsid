#include "quickjs.h"
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <initializer_list>

namespace {
struct Owner { unsigned frees = 0; bool invalid = false; void* data = nullptr; };
unsigned checks = 0, failures = 0;
void check(bool value, const char* message) {
    ++checks;
    if (!value) {++failures; std::fprintf(stderr, "FAIL: %s\n", message);}
}
void free_owned(JSRuntime* rt, void* opaque, void* ptr) {
    if (!ptr) return;
    auto* owner = static_cast<Owner*>(opaque);
    if (owner->frees != 0 || ptr != owner->data) owner->invalid = true;
    ++owner->frees;
    js_free_rt(rt, ptr);
}
JSValue owned_buffer(JSContext* ctx, Owner& owner) {
    auto* data = static_cast<uint8_t*>(js_malloc(ctx, 16));
    if (!data) return JS_EXCEPTION;
    std::memset(data, 165, 16); owner.data = data;
    JSValue result = JS_NewArrayBuffer(ctx, data, 16, free_owned, &owner, false);
    if (JS_IsException(result)) js_free(ctx, data);
    return result;
}
uint32_t length(JSContext* ctx, JSValueConst value) {
    JSValue size = JS_GetPropertyStr(ctx, value, "byteLength");
    uint32_t count = UINT32_MAX;
    if (!JS_IsException(size)) JS_ToUint32(ctx, &count, size);
    JS_FreeValue(ctx, size); return count;
}
void clear_exception(JSContext* ctx) {JS_FreeValue(ctx, JS_GetException(ctx));}
JSValue alias_buffer(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    return JS_NewArrayBufferAlias(ctx, argc ? argv[0] : JS_UNDEFINED);
}
JSValue collect(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    JS_RunGC(JS_GetRuntime(ctx)); return JS_UNDEFINED;
}
}

int main() {
    JSRuntime* rt = JS_NewRuntime(); if (!rt) return 1;
    JSContext* ctx = JS_NewContext(rt); if (!ctx) {JS_FreeRuntime(rt); return 1;}
    constexpr size_t unlimited = static_cast<size_t>(-1);
    unsigned alias_oom = 0, transfer_oom = 0, alias_success = 0, transfer_success = 0;
    for (bool existing : {false, true}) for (size_t budget = 0; budget <= 1024; budget += 8) {
        Owner owner;
        JSValue source = owned_buffer(ctx, owner), first = JS_UNDEFINED;
        check(!JS_IsException(source), "source allocation");
        if (existing) first = JS_NewArrayBufferAlias(ctx, source);
        JSMemoryUsage usage; JS_ComputeMemoryUsage(rt, &usage);
        JS_SetMemoryLimit(rt, static_cast<size_t>(usage.malloc_size) + budget);
        JSValue second = JS_NewArrayBufferAlias(ctx, source);
        JS_SetMemoryLimit(rt, unlimited);
        if (JS_IsException(second)) {++alias_oom; clear_exception(ctx);}
        else {
            ++alias_success; size_t a = 0, b = 0;
            check(JS_GetArrayBuffer(ctx, &a, source) == JS_GetArrayBuffer(ctx, &b, second) && a == 16 && b == 16,
                "alias shares the actual backing");
        }
        check(length(ctx, source) == 16 && owner.frees == 0, "alias OOM preserves source ownership");
        JS_FreeValue(ctx, source); JS_FreeValue(ctx, first); JS_FreeValue(ctx, second);
        check(owner.frees == 1 && !owner.invalid, "aliases free original owner exactly once");
    }
    for (const char* method : {"transfer", "transferToFixedLength", "transferToImmutable"})
    for (int requested : {0, 8, 16, 24}) for (bool aliased : {false, true})
    for (size_t budget = 0; budget <= 1024; budget += 16) {
        Owner owner;
        JSValue source = owned_buffer(ctx, owner), alias = aliased ? JS_NewArrayBufferAlias(ctx, source) : JS_UNDEFINED;
        JSValue function = JS_GetPropertyStr(ctx, source, method), argument = JS_NewInt32(ctx, requested);
        JSMemoryUsage usage; JS_ComputeMemoryUsage(rt, &usage);
        JS_SetMemoryLimit(rt, static_cast<size_t>(usage.malloc_size) + budget);
        JSValue moved = JS_Call(ctx, function, source, 1, &argument);
        JS_SetMemoryLimit(rt, unlimited);
        if (JS_IsException(moved)) {
            ++transfer_oom; clear_exception(ctx);
            check(length(ctx, source) == 16 && owner.frees == 0, "transfer OOM preserves original buffer");
        } else {
            ++transfer_success;
            check(length(ctx, source) == 0 && length(ctx, moved) == static_cast<uint32_t>(requested), "transfer lengths");
            if (requested) {
                size_t size = 0; auto* data = JS_GetArrayBuffer(ctx, &size, moved);
                bool expected = size == static_cast<size_t>(requested);
                for (size_t i = 0; i < size; ++i) expected = expected && data[i] == (i < 16 ? 165 : 0);
                check(expected, "transfer preserves prefix and zero-fills extension");
                if (aliased) {
                    size_t alias_size = 0; auto* alias_data = JS_GetArrayBuffer(ctx, &alias_size, alias);
                    const bool shared = requested == 16 && std::strcmp(method, "transferToImmutable") != 0;
                    check(alias_size == 16 && (data == alias_data) == shared, "equal transfer shares; resized and immutable copies");
                    alias_data[0] = 90;
                    check(data[0] == (shared ? 90 : 165), "remaining alias remains valid and has correct independence");
                }
            }
        }
        if (aliased) check(length(ctx, alias) == 16 && owner.frees == 0, "alias survives source transfer or OOM");
        JS_FreeValue(ctx, function); JS_FreeValue(ctx, source); JS_FreeValue(ctx, moved); JS_FreeValue(ctx, alias);
        check(owner.frees == 1 && !owner.invalid, "external transfer frees original owner exactly once");
    }
    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "aliasBuffer", JS_NewCFunction(ctx, alias_buffer, "aliasBuffer", 1));
    JS_SetPropertyStr(ctx, global, "collect", JS_NewCFunction(ctx, collect, "collect", 0));
    JS_FreeValue(ctx, global);
    const char* script = R"JS((() => {
        let checks = 0;
        const check = (value, name) => {if (!value) throw new Error(name); ++checks;};
        const typeError = callback => {let caught; try {callback();} catch (error) {caught = error;} check(caught instanceof TypeError, 'API rejection');};
        for (const value of [undefined, null, {}, new SharedArrayBuffer(8), new ArrayBuffer(8, {maxByteLength:16}),
            new ArrayBuffer(8).transferToImmutable()]) typeError(() => aliasBuffer(value));
        const detached = new ArrayBuffer(8); detached.transfer(); typeError(() => aliasBuffer(detached));
        for (let i = 0; i < 1000; ++i) {
            let source = new ArrayBuffer(i % 33), alias = aliasBuffer(source), third = aliasBuffer(alias);
            check(source !== alias && alias !== third && source.byteLength === alias.byteLength, 'distinct ordinary wrappers');
            const input = new Uint8Array(source), output = new Uint8Array(third);
            if (input.length) {input[0] = 71; check(output[0] === 71, 'shared bytes');}
            source.transfer(0); check(alias.byteLength === i % 33, 'independent detach');
            const cycle = {alias, third}; cycle.self = cycle; source = alias = third = undefined;
            if (i % 20 === 0) collect();
        }
        collect(); return checks;
    })())JS";
    JSValue result = JS_Eval(ctx, script, std::strlen(script), "arraybuffer-alias-regression.js", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(result)) {
        JSValue error = JS_GetException(ctx); const char* text = JS_ToCString(ctx, error);
        std::fprintf(stderr, "FAIL: %s\n", text ? text : "script exception");
        JS_FreeCString(ctx, text); JS_FreeValue(ctx, error); ++failures;
    } else {uint32_t count = 0; JS_ToUint32(ctx, &count, result); checks += count;}
    JS_FreeValue(ctx, result);
    check(alias_oom && transfer_oom && alias_success && transfer_success, "both OOM and success paths exercised");
    JS_FreeContext(ctx); JS_RunGC(rt); JS_FreeRuntime(rt);
    std::printf("{\"checks\":%u,\"failures\":%u,\"aliasOOM\":%u,\"aliasSuccess\":%u,\"transferOOM\":%u,\"transferSuccess\":%u}\n",
        checks, failures, alias_oom, alias_success, transfer_oom, transfer_success);
    return failures ? 1 : 0;
}
