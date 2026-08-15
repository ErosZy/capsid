// Binding v1 §7.5 (docs/binding-technical-design.md §5.1): two TJS/QuickJS
// runtimes on one Capsid-owned loop. The second runtime attaches via
// TJSRunOptions.shared_loop: it never initializes or closes the loop, its
// job queue is pumped by the owning runtime's loop, heaps and globals stay
// separate, and freeing the attached runtime leaves the loop usable.

extern "C" {
#include "tjs.h"
void tjs__execute_jobs(JSContext *ctx);
JSModuleDef *tjs_module_loader(
    JSContext *ctx,
    const char *module_name,
    void *opaque,
    JSValueConst attributes);
char *js_strdup(JSContext *ctx, const char *str);
}
#include <uv.h>

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

namespace {

// Test-only module gate mirroring the worker's Binding Runtime loader: the
// normalizer grants the name (bypassing the overlay's user-code exposure
// guard) and the upstream loader serves it.
char *grant_all_normalize(JSContext *ctx,
                          const char *base_name,
                          const char *name,
                          void *opaque) {
    (void)ctx;
    (void)base_name;
    (void)opaque;
    return js_strdup(ctx, name);
}

void fail(const std::string &message) {
    std::cerr << "test-tjs-shared-loop: " << message << std::endl;
    std::exit(1);
}

void require(bool condition, const char *message) {
    if (!condition) {
        fail(message);
    }
}

JSValue eval_js(JSContext *ctx, const char *source) {
    JSValue result = JS_Eval(
        ctx, source, std::strlen(source), "<test>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(result)) {
        JSValue exception = JS_GetException(ctx);
        const char *text = JS_ToCString(ctx, exception);
        fail(std::string("JS exception: ") +
             (text ? text : "<unprintable>"));
    }
    return result;
}

double global_number(JSContext *ctx, const char *name) {
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue value = JS_GetPropertyStr(ctx, global, name);
    JS_FreeValue(ctx, global);
    double result = -1;
    if (JS_ToFloat64(ctx, &result, value) != 0) {
        result = -1;
    }
    JS_FreeValue(ctx, value);
    return result;
}

JSValue noop_fn(JSContext *ctx, JSValueConst this_val, int argc,
                JSValueConst *argv) {
    (void)ctx;
    (void)this_val;
    (void)argc;
    (void)argv;
    return JS_UNDEFINED;
}

int noop_bootstrap(TJSRuntime *runtime, JSContext *ctx, void *opaque) {
    (void)runtime;
    (void)opaque;
    // The restricted core asserts a minimal profile surface after
    // bootstrap; install just enough for the two runtimes to exist.
    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(
        ctx, global, "dispatchEvent",
        JS_NewCFunction(ctx, noop_fn, "dispatchEvent", 1));
    // A real constructor so the unhandled-rejection dispatcher can build
    // the event object without asserting.
    JS_SetPropertyStr(
        ctx, global, "PromiseRejectionEvent",
        JS_NewCFunction2(ctx, noop_fn, "PromiseRejectionEvent", 0,
                         JS_CFUNC_constructor, 0));
    JS_FreeValue(ctx, global);
    return 0;
}

void test_module_default_export() {
    char arg0[] = "test-tjs-shared-loop";
    char *argv[] = { arg0 };
    TJS_Initialize(1, argv);
    TJSRunOptions options;
    TJS_DefaultOptions(&options);
    options.skip_run_main = true;
    options.bootstrap = noop_bootstrap;
    TJSRuntime *rt = TJS_NewRuntimeOptions(&options);
    JSContext *ctx = TJS_GetJSContext(rt);
    const char *source =
        "const probe = ({ config, secrets, log }) => {"
        "  return { find(input) { return 'find:' + input; } };"
        "};"
        "export { probe };"
        "export default probe;";
    JSValue module = JS_Eval(
        ctx, source, std::strlen(source), "capsid:binding/probe",
        JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    require(!JS_IsException(module), "binding module compile threw");
    require(JS_ResolveModule(ctx, module) == 0,
            "binding module resolution failed");
    JSModuleDef *definition =
        static_cast<JSModuleDef *>(JS_VALUE_GET_PTR(module));
    JSValue evaluation = JS_EvalFunction(ctx, JS_DupValue(ctx, module));
    require(!JS_IsException(evaluation), "binding module evaluation threw");
    tjs__execute_jobs(ctx);
    require(JS_PromiseState(ctx, evaluation) == JS_PROMISE_FULFILLED,
            "binding module evaluation did not settle");
    JS_FreeValue(ctx, evaluation);
    JSValue module_namespace = JS_GetModuleNamespace(ctx, definition);
    JS_FreeValue(ctx, module);
    JSValue factory =
        JS_GetPropertyStr(ctx, module_namespace, "default");
    JS_FreeValue(ctx, module_namespace);
    require(JS_IsFunction(ctx, factory),
            "module default export is not a function");
    // The factory receives one object argument { config, secrets, log }.
    JSValue init = JS_NewObjectProto(ctx, JS_NULL);
    JS_SetPropertyStr(ctx, init, "config", JS_NewObjectProto(ctx, JS_NULL));
    JS_SetPropertyStr(ctx, init, "secrets", JS_NewObjectProto(ctx, JS_NULL));
    JS_SetPropertyStr(ctx, init, "log", JS_NewObjectProto(ctx, JS_NULL));
    JSValue args[1] = { init };
    JSValue result =
        JS_Call(ctx, factory, JS_UNDEFINED, 1, args);
    JS_FreeValue(ctx, init);
    require(!JS_IsException(result) &&
                JS_IsObject(result),
            "factory call failed");
    JSValue probe = JS_GetPropertyStr(ctx, result, "find");
    require(JS_IsFunction(ctx, probe),
            "factory result method is not a function");
    JS_FreeValue(ctx, probe);
    JS_FreeValue(ctx, result);
    JS_FreeValue(ctx, factory);
    TJS_FreeRuntime(rt);
}

void test_shared_loop_two_runtimes() {
    char arg0[] = "test-tjs-shared-loop";
    char *argv[] = { arg0 };
    TJS_Initialize(1, argv);

    TJSRunOptions user_options;
    TJS_DefaultOptions(&user_options);
    user_options.skip_run_main = true;
    user_options.bootstrap = noop_bootstrap;
    TJSRuntime *user = TJS_NewRuntimeOptions(&user_options);
    require(user != NULL, "user runtime creation failed");
    uv_loop_t *loop = TJS_GetLoop(user);
    require(loop != NULL, "user runtime has no loop");

    TJSRunOptions binding_options;
    TJS_DefaultOptions(&binding_options);
    binding_options.skip_run_main = true;
    binding_options.mem_limit = 64 * 1024 * 1024;
    binding_options.bootstrap = noop_bootstrap;
    binding_options.shared_loop = loop;
    TJSRuntime *binding = TJS_NewRuntimeOptions(&binding_options);
    require(binding != NULL, "binding runtime creation failed");
    require(TJS_GetLoop(binding) == loop,
            "binding runtime did not attach to the shared loop");

    JSContext *user_ctx = TJS_GetJSContext(user);
    JSContext *binding_ctx = TJS_GetJSContext(binding);

    // Heaps and globals are separate.
    JSValue binding_flag =
        eval_js(binding_ctx, "globalThis.bindingFlag = 123;");
    JS_FreeValue(binding_ctx, binding_flag);
    JSValue user_lookup =
        eval_js(user_ctx, "typeof globalThis.bindingFlag");
    const char *lookup_text = JS_ToCString(user_ctx, user_lookup);
    require(lookup_text != NULL && std::strcmp(lookup_text, "undefined") == 0,
            "binding global leaked into the user runtime");
    JS_FreeCString(user_ctx, lookup_text);
    JS_FreeValue(user_ctx, user_lookup);

    // The owning runtime's loop pumps the attached runtime's job queue.
    TJS_StartRuntimeJobs(user);
    TJS_StartRuntimeJobs(binding);
    JSValue binding_job_eval = eval_js(
        binding_ctx,
        "globalThis.bindingJob = 0;"
        "Promise.resolve().then(() => { globalThis.bindingJob = 1; });");
    JS_FreeValue(binding_ctx, binding_job_eval);
    JSValue user_job_eval = eval_js(
        user_ctx,
        "globalThis.userJob = 0;"
        "Promise.resolve().then(() => { globalThis.userJob = 1; });");
    JS_FreeValue(user_ctx, user_job_eval);
    for (int index = 0; index < 10000; ++index) {
        uv_run(loop, UV_RUN_NOWAIT);
        if (global_number(binding_ctx, "bindingJob") == 1 &&
            global_number(user_ctx, "userJob") == 1) {
            break;
        }
    }
    require(global_number(binding_ctx, "bindingJob") == 1,
            "binding runtime jobs were not pumped by the shared loop");
    require(global_number(user_ctx, "userJob") == 1,
            "user runtime jobs were not pumped by the shared loop");

    // The attached runtime can import tjs:internal/core through its own
    // module system (the same compile/resolve/evaluate/drain sequence the
    // worker uses for binding packages). Mirror the worker's Binding
    // loader: a granting normalizer plus the upstream loader.
    JS_SetModuleLoaderFunc2(
        JS_GetRuntime(binding_ctx), grant_all_normalize,
        tjs_module_loader, NULL, NULL);
    const char *import_source =
        "import core from 'tjs:internal/core';"
        "globalThis.coreSeen = core && typeof core === 'object' ? 1 : 0;";
    JSValue import_module = JS_Eval(
        binding_ctx, import_source, std::strlen(import_source),
        "capsid:binding/import-probe",
        JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    if (JS_IsException(import_module)) {
        JSValue exception = JS_GetException(binding_ctx);
        const char *text = JS_ToCString(binding_ctx, exception);
        fail(std::string("binding import module compile threw: ") +
             (text ? text : "?"));
    }
    require(JS_ResolveModule(binding_ctx, import_module) == 0,
            "binding import module resolution failed");
    JSValue import_evaluation = JS_EvalFunction(
        binding_ctx, JS_DupValue(binding_ctx, import_module));
    require(!JS_IsException(import_evaluation),
            "binding import module evaluation threw");
    JS_FreeValue(binding_ctx, import_module);
    tjs__execute_jobs(binding_ctx);
    require(JS_PromiseState(binding_ctx, import_evaluation) ==
                JS_PROMISE_FULFILLED,
            "binding import module evaluation did not settle");
    JS_FreeValue(binding_ctx, import_evaluation);
    require(global_number(binding_ctx, "coreSeen") == 1,
            "binding runtime could not import tjs:internal/core");

    // Freeing the attached runtime closes only its own handles: the user
    // runtime keeps running on the same loop.
    TJS_FreeRuntime(binding);
    JSValue alive = eval_js(user_ctx, "globalThis.alive = 2;");
    JS_FreeValue(user_ctx, alive);
    require(global_number(user_ctx, "alive") == 2,
            "user runtime broke after the binding runtime was freed");

    TJS_FreeRuntime(user);
}

}  // namespace

int main() {
    test_module_default_export();
    test_shared_loop_two_runtimes();
    return 0;
}
