// Raw bytecode generator for the exec-throughput benchmark (Step 8).
// Compiles a module source with the SAME path as the deployed compiler
// (JS_Eval compile-only + JS_WriteObject) but skips the optimizer, so
// the benchmark can compare unoptimized vs optimized bytecode on
// identical inputs. Not part of the product toolchain.
//
// Usage: bytecode-raw <source.js> <source-name> <out.qjsb>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>

extern "C" {
#include "quickjs.h"
}

static JSModuleDef* compile_module_loader(JSContext* ctx,
                                          const char* module_name,
                                          void*) {
    JS_ThrowReferenceError(ctx, "module is unavailable: %s", module_name);
    return nullptr;
}

static bool read_file(const char* path, std::string* out) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    out->assign(static_cast<size_t>(size), '\0');
    if (size > 0 &&
        std::fread(&(*out)[0], 1, static_cast<size_t>(size), f) !=
            static_cast<size_t>(size)) {
        std::fclose(f);
        return false;
    }
    std::fclose(f);
    return true;
}

int main(int argc, char** argv) {
    if (argc != 4) {
        std::fprintf(stderr,
                     "usage: %s <source.js> <source-name> <out.qjsb>\n",
                     argv[0]);
        return 2;
    }
    std::string source;
    if (!read_file(argv[1], &source)) {
        std::fprintf(stderr, "cannot open source: %s\n", argv[1]);
        return 1;
    }
    source.push_back('\0');

    JSRuntime* runtime = JS_NewRuntime();
    if (!runtime) return 1;
    JSContext* ctx = JS_NewContext(runtime);
    if (!ctx) {
        JS_FreeRuntime(runtime);
        return 1;
    }
    JS_SetModuleLoaderFunc(runtime, nullptr, compile_module_loader, nullptr);
    JSValue module = JS_Eval(ctx, source.data(), source.size() - 1, argv[2],
                             JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    if (JS_IsException(module)) {
        JSValue e = JS_GetException(ctx);
        const char* s = JS_ToCString(ctx, e);
        std::fprintf(stderr, "compile failed: %s\n", s ? s : "?");
        JS_FreeCString(ctx, s);
        JS_FreeValue(ctx, e);
        JS_FreeContext(ctx);
        JS_FreeRuntime(runtime);
        return 1;
    }
    std::size_t out_size = 0;
    std::uint8_t* data =
        JS_WriteObject(ctx, &out_size, module, JS_WRITE_OBJ_BYTECODE);
    JS_FreeValue(ctx, module);
    if (!data) {
        std::fprintf(stderr, "serialize failed\n");
        JS_FreeContext(ctx);
        JS_FreeRuntime(runtime);
        return 1;
    }
    FILE* out = std::fopen(argv[3], "wb");
    if (!out) {
        std::fprintf(stderr, "cannot create output: %s\n", argv[3]);
        return 1;
    }
    std::fwrite(data, 1, out_size, out);
    std::fclose(out);
    js_free(ctx, data);
    JS_FreeContext(ctx);
    JS_FreeRuntime(runtime);
    std::fprintf(stderr, "raw bytes: %zu\n", out_size);
    return 0;
}
