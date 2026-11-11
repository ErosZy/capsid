#include "quickjs.h"

#include <cstdio>
#include <stdexcept>

namespace {
void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
struct Runtime {
    JSRuntime* runtime = JS_NewRuntime();
    JSContext* context = runtime ? JS_NewContext(runtime) : nullptr;
    ~Runtime() {
        if (context) JS_FreeContext(context);
        if (runtime) JS_FreeRuntime(runtime);
    }
};
struct Value {
    JSContext* context;
    JSValue value;
    ~Value() { JS_FreeValue(context, value); }
};
int native_calls = 0;
int interrupt_calls = 0;
JSValue native_value(JSContext* context, JSValueConst, int, JSValueConst*) {
    ++native_calls;
    return JS_NewInt32(context, 123);
}
int interrupt(JSRuntime*, void*) {
    ++interrupt_calls;
    return 1;
}
}  // namespace

int main() {
    try {
        Runtime state;
        check(state.context != nullptr, "QuickJS context creation");
        Value function{state.context, JS_NewCFunction(state.context, native_value, "nativeValue", 0)};
        check(!JS_IsException(function.value), "native function creation");
        JS_SetInterruptHandler(state.runtime, interrupt, nullptr);
        int successful_calls = 0;
        bool interrupted = false;
        // Direct C API calls have no JS loop/back-edge to perform polling.
        // The shared call entry must also service a native-only workload.
        for (int index = 0; index < 30000; ++index) {
            Value result{state.context, JS_Call(state.context, function.value, JS_UNDEFINED, 0, nullptr)};
            if (JS_IsException(result.value)) {
                Value error{state.context, JS_GetException(state.context)};
                check(JS_IsError(error.value), "interrupt produces an Error");
                interrupted = true;
                break;
            }
            check(JS_VALUE_GET_TAG(result.value) == JS_TAG_INT && JS_VALUE_GET_INT(result.value) == 123,
                  "native call result");
            ++successful_calls;
        }
        check(interrupted && interrupt_calls == 1, "native-only calls must poll interrupts");
        check(native_calls == successful_calls, "interrupted entry must not invoke the native body");
        JS_SetInterruptHandler(state.runtime, nullptr, nullptr);
        Value recovered{state.context, JS_Call(state.context, function.value, JS_UNDEFINED, 0, nullptr)};
        check(!JS_IsException(recovered.value) && JS_VALUE_GET_TAG(recovered.value) == JS_TAG_INT &&
                  JS_VALUE_GET_INT(recovered.value) == 123,
              "native call recovers after interrupt removal");
        std::printf("native calls=%d interrupt callbacks=%d recovery=passed\n", successful_calls, interrupt_calls);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
