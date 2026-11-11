#include "quickjs.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
void require(bool value, const char *message) {
    if (!value) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}

void check_exception(JSContext *ctx, JSValue value) {
    if (!JS_IsException(value)) return;
    JSValue error = JS_GetException(ctx);
    const char *message = JS_ToCString(ctx, error);
    std::fprintf(stderr, "FAIL: JS exception: %s\n", message ? message : "unknown");
    JS_FreeCString(ctx, message);
    JS_FreeValue(ctx, error);
    std::exit(1);
}

void eval(JSContext *ctx, const char *source) {
    JSValue value = JS_Eval(ctx, source, std::strlen(source), "async-gc.js", JS_EVAL_TYPE_GLOBAL);
    check_exception(ctx, value);
    JS_FreeValue(ctx, value);
}

void drain(JSRuntime *rt) {
    JSContext *ctx = nullptr;
    int count = 0, status;
    while ((status = JS_ExecutePendingJob(rt, &ctx)) > 0)
        require(++count < 1000, "job drain must be bounded");
    if (status < 0) check_exception(ctx, JS_EXCEPTION);
}

void expect(JSContext *ctx, const char *source) {
    JSValue value = JS_Eval(ctx, source, std::strlen(source), "assert.js", JS_EVAL_TYPE_GLOBAL);
    check_exception(ctx, value);
    require(JS_ToBool(ctx, value) == 1, source);
    JS_FreeValue(ctx, value);
}
} // namespace

int main() {
    JSRuntime *rt = JS_NewRuntime();
    require(rt != nullptr, "runtime creation");
    JSContext *ctx = JS_NewContext(rt);
    require(ctx != nullptr, "context creation");

    // The callback is the only external root of a suspended frame. In the
    // old engine, its attached var-ref did not retain that frame during GC.
    eval(ctx, R"JS(
        globalThis.answer = null;
        (async () => {
            let resolve;
            const waiting = new Promise(r => resolve = r);
            globalThis.resume = () => resolve('alive');
            globalThis.answer = await waiting;
        })();
    )JS");
    JS_RunGC(rt);
    eval(ctx, "resume()");
    drain(rt);
    expect(ctx, "answer === 'alive'");
    eval(ctx, "delete globalThis.resume; delete globalThis.answer");
    JS_RunGC(rt);

    // Both generator forms use the same heap-frame ownership. Exercise a
    // captured local across yield, normal return, and throw completion.
    eval(ctx, R"JS(
        globalThis.iterator = (function* () {
            let value = 1;
            globalThis.increment = () => ++value;
            yield value;
            return value;
        })();
        globalThis.first = iterator.next();
    )JS");
    JS_RunGC(rt);
    expect(ctx, "first.value === 1 && !first.done && increment() === 2");
    expect(ctx, "iterator.next().value === 2 && iterator.next().done && increment() === 3");
    eval(ctx, "delete globalThis.iterator; delete globalThis.increment; delete globalThis.first");
    JS_RunGC(rt);

    eval(ctx, R"JS(
        globalThis.iterator = (async function* () {
            let value = 4;
            globalThis.increment = () => ++value;
            yield value;
            return value;
        })();
        iterator.next().then(r => globalThis.first = r);
    )JS");
    drain(rt);
    JS_RunGC(rt);
    expect(ctx, "first.value === 4 && !first.done && increment() === 5");
    eval(ctx, "iterator.next().then(r => globalThis.last = r)");
    drain(rt);
    expect(ctx, "last.value === 5 && last.done && increment() === 6");
    eval(ctx, R"JS(
        delete globalThis.iterator; delete globalThis.increment;
        delete globalThis.first; delete globalThis.last;
        globalThis.iterator = (function* () { yield 1; throw new Error('expected'); })();
        iterator.next();
        try { iterator.next(); } catch (e) { globalThis.caught = e.message; }
    )JS");
    expect(ctx, "caught === 'expected' && iterator.next().done");
    eval(ctx, "delete globalThis.iterator; delete globalThis.caught");
    drain(rt);
    JS_RunGC(rt);

    // Adding an ownership edge must not turn abandoned async cycles into
    // permanent roots. After warm-up, repeated GC returns to the same heap.
    const char *abandoned = R"JS(
        for (let i = 0; i < 100; ++i) {
            (async () => {
                let resolve;
                const waiting = new Promise(r => resolve = r);
                const cycle = () => resolve;
                await waiting;
                cycle();
            })();
        }
    )JS";
    eval(ctx, abandoned);
    drain(rt);
    JS_RunGC(rt);
    JSMemoryUsage before{}, after{};
    JS_ComputeMemoryUsage(rt, &before);
    for (int round = 0; round < 20; ++round) {
        eval(ctx, abandoned);
        drain(rt);
        JS_RunGC(rt);
    }
    JS_ComputeMemoryUsage(rt, &after);
    require(after.memory_used_size <= before.memory_used_size + 1024,
            "abandoned async frame cycles must be collected");
    std::printf("PASS: async closure, generator GC, abandoned cycles (%lld -> %lld bytes)\n",
                static_cast<long long>(before.memory_used_size),
                static_cast<long long>(after.memory_used_size));
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
}
