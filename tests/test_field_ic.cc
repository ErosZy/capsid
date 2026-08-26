// Sparse per-site field IC contract. BC26 remains canonical: hot exact PCs
// train lazy sidecars, while accessors, prototype hits, mutation and shape-id
// wrap must always preserve the generic get_field result.
#include "quickjs.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

int failures = 0;

void check(const char* name, bool ok) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok)
        failures++;
}

bool eval(JSContext* ctx, const char* source, const char* name) {
    JSValue value = JS_Eval(ctx, source, std::strlen(source), name,
                            JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(value)) {
        JSValue exception = JS_GetException(ctx);
        const char* text = JS_ToCString(ctx, exception);
        std::fprintf(stderr, "%s: %s\n", name,
                     text ? text : "<unprintable exception>");
        JS_FreeCString(ctx, text);
        JS_FreeValue(ctx, exception);
        return false;
    }
    JS_FreeValue(ctx, value);
    return true;
}

JSFieldICReport report(JSRuntime* rt) {
    JSFieldICReport value{};
    JS_GetFieldICReport(rt, &value);
    return value;
}

}  // namespace

int main() {
    JSRuntime* rt = JS_NewRuntime();
    JSContext* ctx = rt ? JS_NewContext(rt) : nullptr;
    if (!rt || !ctx)
        return 2;

    check("mono_and_exact_pc",
          eval(ctx,
               "function mono(o) { let s=0; for(let i=0;i<2000;i++) s+=o.x; return s; }"
               "function two(a,b) { let s=0; for(let i=0;i<2000;i++){s+=a.x;s+=b.x;} return s; }"
               "if(mono({x:7})!==14000) throw Error('mono');"
               "if(two({x:3},{pad:0,x:5})!==16000) throw Error('two-pc');",
               "field-ic-mono.js"));
    JSFieldICReport mono_report = report(rt);
    check("lazy_sidecar_allocated",
          mono_report.current_bytes != 0 &&
              mono_report.current_sidecars != 0);
    check("hot_sites_selected",
          mono_report.selected_sites >= 3 && mono_report.mono_sites >= 3);
    check("same_atom_two_pc_no_pollution", mono_report.poly2_sites == 0);

    check("poly2_then_megamorphic",
          eval(ctx,
               "function poly(o) { let s=0; for(let i=0;i<2000;i++) s+=o.x; return s; }"
               "if(poly({x:11})!==22000) throw Error('poly-a');"
               "if(poly({a:0,x:13})!==26000) throw Error('poly-b');"
               "if(poly({a:0,b:0,x:17})!==34000) throw Error('poly-c');"
               "if(poly({x:19})!==38000) throw Error('generic-after-mega');",
               "field-ic-poly.js"));
    JSFieldICReport poly_report = report(rt);
    check("poly2_observed", poly_report.poly2_sites > 0);
    check("third_shape_bypasses", poly_report.megamorphic_sites > 0);

    check("mutation_and_accessors",
          eval(ctx,
               "let gets=0;"
               "let mut={x:23}; if(mono(mut)!==46000) throw Error('before');"
               "Object.defineProperty(mut,'x',{get(){gets++;return 29;},configurable:true});"
               "if(mono(mut)!==58000||gets!==2000) throw Error('getter');"
               "Object.defineProperty(mut,'x',{value:31,writable:true,configurable:true});"
               "if(mono(mut)!==62000) throw Error('data');"
               "delete mut.x; mut.x=37; if(mono(mut)!==74000) throw Error('delete-add');"
               "let proto={x:41}; let child=Object.create(proto);"
               "if(mono(child)!==82000) throw Error('proto');"
               "child.x=43; if(mono(child)!==86000) throw Error('proto-own');",
               "field-ic-mutation.js"));

    JS_FieldICSetShapeCounterForTest(rt, UINT32_MAX);
    check("shape_id_wrap_fails_generic",
          eval(ctx,
               "let wrapped={x:47};"
               "if(mono(wrapped)!==94000) throw Error('shape-wrap');",
               "field-ic-wrap.js"));

    const JSFieldICReport final_report = report(rt);
    check("bounded_runtime_memory", final_report.current_bytes <= 256 * 1024);

    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    if (failures) {
        std::fprintf(stderr, "test_field_ic: %d failure(s)\n", failures);
        return 1;
    }
    std::fprintf(stderr, "test_field_ic: all green\n");
    return 0;
}
