#include "quickjs.h"

#include <cstdio>
#include <cstring>
#include <vector>

namespace {

int failures = 0;

void check(const char* name, bool ok) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok)
        failures++;
}

void print_exception(JSContext* ctx) {
    JSValue ex = JS_GetException(ctx);
    const char* text = JS_ToCString(ctx, ex);
    std::fprintf(stderr, "%s\n", text ? text : "<exception>");
    JS_FreeCString(ctx, text);
    JS_FreeValue(ctx, ex);
}

std::vector<uint8_t> write_bytecode(JSContext* ctx, JSValueConst value) {
    size_t size = 0;
    uint8_t* data = JS_WriteObject(ctx, &size, value, JS_WRITE_OBJ_BYTECODE);
    if (!data)
        return {};
    std::vector<uint8_t> result(data, data + size);
    js_free(ctx, data);
    return result;
}

}  // namespace

int main() {
    static const char source[] =
        "function readA(o,n){let s=0;for(let i=0;i<n;i++)s+=o.x;return s;}\n"
        "function readB(o,n){let s=0;for(let i=0;i<n;i++)s+=o.x;return s;}\n"
        "function callF(o,n){let s=0;for(let i=0;i<n;i++)s+=o.f();return s;}\n"
        "const a={x:2};if(readA(a,20000)!==40000)throw Error('mono');\n"
        "a.x=3;if(readA(a,100)!==300)throw Error('overwrite');\n"
        "delete a.x;a.x=4;if(readA(a,100)!==400)throw Error('reshape');\n"
        "Object.defineProperty(a,'x',{get(){return 5}});"
        "if(readA(a,20)!==100)throw Error('accessor');\n"
        "const b={pad:0,x:6};if(readB(b,10000)!==60000)throw Error('siteB');\n"
        "const proxy=new Proxy({x:7},{});"
        "if(readA(proxy,20)!==140)throw Error('proxy');\n"
        "const proto={x:8};const inherited=Object.create(proto);"
        "if(readA(inherited,20)!==160)throw Error('proto');\n"
        "const method={f(){return 9}};"
        "if(callF(method,10000)!==90000)throw Error('field2');\n"
        "for(let i=0;i<1000;i++)({x:i});globalThis.gc?.();"
        "if(readB(b,100)!==600)throw Error('gc');\n";

    JSRuntime* rt = JS_NewRuntime();
    JSContext* ctx = rt ? JS_NewContext(rt) : nullptr;
    JSPreICStats initial_stats{};
    if (!rt || !ctx)
        return 2;
    JS_GetPreICStats(rt, &initial_stats);

    JSValue compiled = JS_Eval(
        ctx, source, sizeof(source) - 1, "pre-ic.js",
        JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY);
    check("compile", !JS_IsException(compiled));
    if (JS_IsException(compiled)) {
        print_exception(ctx);
        JS_FreeValue(ctx, compiled);
        JS_FreeContext(ctx);
        JS_FreeRuntime(rt);
        return 1;
    }

    const std::vector<uint8_t> first = write_bytecode(ctx, compiled);
    check("serialize", !first.empty());
    JS_FreeValue(ctx, compiled);

    JSValue loaded = JS_ReadObject(ctx, first.data(), first.size(),
                                   JS_READ_OBJ_BYTECODE);
    check("deserialize", !JS_IsException(loaded));
    if (JS_IsException(loaded)) {
        print_exception(ctx);
    } else {
        const std::vector<uint8_t> second = write_bytecode(ctx, loaded);
        check("canonical_bc26_roundtrip", first == second);
        JSValue result = JS_EvalFunction(ctx, loaded);
        check("semantics", !JS_IsException(result));
        if (JS_IsException(result))
            print_exception(ctx);
        JS_FreeValue(ctx, result);
    }

    JSPreICStats stats{};
    JS_GetPreICStats(rt, &stats);
    check("per_site_selection", stats.selected_sites >= 3);
    check("one_time_training", stats.installs >= 3);
#ifdef CONFIG_PRE_IC_STATS
    check("direct_hits", stats.hits >= 39000);
    check("generic_fallbacks", stats.misses >= 40);
#endif
    check("bounded_sidecar", stats.bytes <= 256 * 1024);

    JS_FreeContext(ctx);
    JS_RunGC(rt);
    JS_GetPreICStats(rt, &stats);
    std::printf("pre_ic_bytes initial=%zu final=%zu\n",
                initial_stats.bytes, stats.bytes);
    check("sidecar_released", stats.bytes == initial_stats.bytes);
    JS_FreeRuntime(rt);

    if (failures) {
        std::fprintf(stderr, "test_pre_ic: %d failure(s)\n", failures);
        return 1;
    }
    std::fprintf(stderr, "test_pre_ic: all green\n");
    return 0;
}
