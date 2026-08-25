// Classic-script bytecode compiler/runner for profile-guided rewriter work.
//
// Unlike capsid-bytecode-compile (which deliberately accepts ES modules only),
// this benchmark-only tool preserves the classic-script semantics used by
// Octane, Kraken, SunSpider, and V8 Suite.  It lets the exact same serialized
// global script run as raw BC26 or through a selected rewriter pass mask.
//
// Usage:
//   classic-bytecode compile --input suite.js --source-name file:///suite.js
//       --output suite.qjsb [--rewrite] [--passes MASK] [--report]
//   classic-bytecode run --input suite.qjsb [--warmup N] [--rounds N]
//       [--expect-global-true NAME] [--opcode-profile FILE]

// `run` measures JS_EvalFunction only. Runtime/context construction and
// JS_ReadObject are deliberately outside the interval. Each sample gets a
// fresh runtime because a global bytecode function is consumed by evaluation
// and suite globals are intentionally not reusable across samples.
#include "bytecode_rewriter/bytecode_rewriter.h"

#include <chrono>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

extern "C" {
#include "quickjs.h"
}

namespace {

[[noreturn]] void fail(const std::string& message) {
    std::fprintf(stderr, "classic-bytecode: %s\n", message.c_str());
    std::exit(1);
}

bool read_file(const std::string& path, std::vector<std::uint8_t>* out) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return false;
    out->assign(std::istreambuf_iterator<char>(input),
                std::istreambuf_iterator<char>());
    return input.good() || input.eof();
}

bool write_file(const std::string& path,
                const std::vector<std::uint8_t>& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) return false;
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    return output.good();
}

std::uint32_t parse_u32(const char* text, const char* option) {
    if (text == nullptr || *text == '\0' || *text == '-') {
        fail(std::string("invalid ") + option);
    }
    errno = 0;
    char* end = nullptr;
    const unsigned long value = std::strtoul(text, &end, 0);
    if (errno != 0 || end == text || *end != '\0' ||
        value > std::numeric_limits<std::uint32_t>::max()) {
        fail(std::string("invalid ") + option + ": " + text);
    }
    return static_cast<std::uint32_t>(value);
}

void dump_exception(JSContext* ctx, const char* prefix) {
    JSValue exception = JS_GetException(ctx);
    const char* message = JS_ToCString(ctx, exception);
    std::fprintf(stderr, "classic-bytecode: %s: %s\n", prefix,
                 message != nullptr ? message : "<exception>");
    JS_FreeCString(ctx, message);
    JSValue stack = JS_GetPropertyStr(ctx, exception, "stack");
    if (!JS_IsUndefined(stack)) {
        const char* text = JS_ToCString(ctx, stack);
        if (text != nullptr) std::fprintf(stderr, "%s\n", text);
        JS_FreeCString(ctx, text);
    }
    JS_FreeValue(ctx, stack);
    JS_FreeValue(ctx, exception);
}

JSValue js_noop_output(JSContext*, JSValueConst, int, JSValueConst*) {
    return JS_UNDEFINED;
}

JSValue js_empty_read(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    // Legacy shell suites probe a global read() even when their generated
    // payload is already concatenated into the self-contained source. Do not
    // grant filesystem access to the benchmark runner; an empty fallback is
    // sufficient for those environment probes.
    return JS_NewStringLen(ctx, "", 0);
}

void install_classic_harness_globals(JSContext* ctx) {
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue print = JS_NewCFunction(ctx, js_noop_output, "print", 1);
    if (JS_IsException(print) ||
        JS_SetPropertyStr(ctx, global, "print", print) < 0) {
        JS_FreeValue(ctx, global);
        fail("cannot install print()");
    }
    JSValue read = JS_NewCFunction(ctx, js_empty_read, "read", 1);
    if (JS_IsException(read) ||
        JS_SetPropertyStr(ctx, global, "read", read) < 0) {
        JS_FreeValue(ctx, global);
        fail("cannot install read()");
    }
    JSValue console = JS_NewObject(ctx);
    JSValue log = JS_NewCFunction(ctx, js_noop_output, "log", 1);
    if (JS_IsException(console) || JS_IsException(log) ||
        JS_SetPropertyStr(ctx, console, "log", log) < 0 ||
        JS_SetPropertyStr(ctx, global, "console", console) < 0) {
        JS_FreeValue(ctx, global);
        fail("cannot install console.log()");
    }
    JS_FreeValue(ctx, global);
}

struct CompileOptions {
    std::string input;
    std::string source_name;
    std::string output;
    bool rewrite = false;
    bool report = false;
    std::uint32_t passes = capsid::bytecode::kPassAll;
};

CompileOptions parse_compile(int argc, char** argv) {
    CompileOptions options;
    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        if ((arg == "--input" || arg == "--source-name" ||
             arg == "--output" || arg == "--passes") &&
            i + 1 >= argc) {
            fail("missing value for " + arg);
        }
        if (arg == "--input") {
            options.input = argv[++i];
        } else if (arg == "--source-name") {
            options.source_name = argv[++i];
        } else if (arg == "--output") {
            options.output = argv[++i];
        } else if (arg == "--passes") {
            options.passes = parse_u32(argv[++i], "--passes");
        } else if (arg == "--rewrite") {
            options.rewrite = true;
        } else if (arg == "--report") {
            options.report = true;
        } else {
            fail("unknown compile option: " + arg);
        }
    }
    if (options.input.empty() || options.source_name.empty() ||
        options.output.empty()) {
        fail("compile requires --input, --source-name, and --output");
    }
    return options;
}

int compile_script(const CompileOptions& options) {
    std::vector<std::uint8_t> source;
    if (!read_file(options.input, &source)) {
        fail("cannot read source: " + options.input);
    }
    source.push_back(0);  // QuickJS lexer lookahead sentinel.

    JSRuntime* runtime = JS_NewRuntime();
    if (runtime == nullptr) fail("JS_NewRuntime failed");
    JS_SetMaxStackSize(runtime, 64u * 1024u * 1024u);
    JSContext* ctx = JS_NewContext(runtime);
    if (ctx == nullptr) {
        JS_FreeRuntime(runtime);
        fail("JS_NewContext failed");
    }
    JSValue compiled = JS_Eval(
        ctx, reinterpret_cast<const char*>(source.data()), source.size() - 1,
        options.source_name.c_str(),
        JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY);
    if (JS_IsException(compiled)) {
        dump_exception(ctx, "compile failed");
        JS_FreeContext(ctx);
        JS_FreeRuntime(runtime);
        return 1;
    }
    std::size_t size = 0;
    std::uint8_t* serialized =
        JS_WriteObject(ctx, &size, compiled, JS_WRITE_OBJ_BYTECODE);
    JS_FreeValue(ctx, compiled);
    if (serialized == nullptr) {
        dump_exception(ctx, "serialization failed");
        JS_FreeContext(ctx);
        JS_FreeRuntime(runtime);
        return 1;
    }
    std::vector<std::uint8_t> bytes(serialized, serialized + size);
    js_free(ctx, serialized);
    JS_FreeContext(ctx);
    JS_FreeRuntime(runtime);

    if (options.rewrite) {
        std::vector<std::uint8_t> rewritten;
        std::string error;
        if (!capsid::bytecode::rewrite_classic_for_benchmark(
                bytes, &rewritten, options.passes, options.report, &error)) {
            fail(error);
        }
        bytes.swap(rewritten);
    }
    if (!write_file(options.output, bytes)) {
        fail("cannot write bytecode: " + options.output);
    }
    std::fprintf(stderr, "classic-bytecode: %s bytes=%zu version=%u\n",
                 options.rewrite ? "rewritten" : "raw", bytes.size(),
                 bytes.empty() ? 0u : static_cast<unsigned>(bytes[0]));
    return 0;
}

struct RunOptions {
    std::string input;
    std::string expect_global_true;
    std::string opcode_profile;
    std::uint32_t warmup = 1;
    std::uint32_t rounds = 5;
};

RunOptions parse_run(int argc, char** argv) {
    RunOptions options;
    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        if ((arg == "--input" || arg == "--warmup" || arg == "--rounds" ||
             arg == "--expect-global-true" || arg == "--opcode-profile") &&
            i + 1 >= argc) {
            fail("missing value for " + arg);
        }
        if (arg == "--input") {
            options.input = argv[++i];
        } else if (arg == "--warmup") {
            options.warmup = parse_u32(argv[++i], "--warmup");
        } else if (arg == "--rounds") {
            options.rounds = parse_u32(argv[++i], "--rounds");
        } else if (arg == "--expect-global-true") {
            options.expect_global_true = argv[++i];
        } else if (arg == "--opcode-profile") {
            options.opcode_profile = argv[++i];
        } else {
            fail("unknown run option: " + arg);
        }
    }
    if (options.input.empty() || options.rounds == 0) {
        fail("run requires --input and nonzero --rounds");
    }
    if (!options.opcode_profile.empty() &&
        (options.warmup != 0 || options.rounds != 1)) {
        fail("--opcode-profile requires --warmup 0 --rounds 1");
    }
#ifndef CONFIG_OPCODE_PROFILE
    if (!options.opcode_profile.empty()) {
        fail("--opcode-profile requires a CONFIG_OPCODE_PROFILE build");
    }
#endif
    return options;
}

bool run_once(const std::vector<std::uint8_t>& bytes,
              const std::string& expect_global_true,
              const std::string& opcode_profile,
              double* elapsed_ms) {
    JSRuntime* runtime = JS_NewRuntime();
    if (runtime == nullptr) fail("JS_NewRuntime failed");
    JS_SetMaxStackSize(runtime, 64u * 1024u * 1024u);
    JSContext* ctx = JS_NewContext(runtime);
    if (ctx == nullptr) {
        JS_FreeRuntime(runtime);
        fail("JS_NewContext failed");
    }
    install_classic_harness_globals(ctx);
    JSValue compiled = JS_ReadObject(ctx, bytes.data(), bytes.size(),
                                     JS_READ_OBJ_BYTECODE);
    if (JS_IsException(compiled)) {
        dump_exception(ctx, "read failed");
        JS_FreeContext(ctx);
        JS_FreeRuntime(runtime);
        return false;
    }

    const auto start = std::chrono::steady_clock::now();
    JSValue result = JS_EvalFunction(ctx, compiled);  // consumes compiled
    bool ok = !JS_IsException(result);
    if (!ok) dump_exception(ctx, "evaluation failed");
    JS_FreeValue(ctx, result);
    if (ok) {
        JSContext* job_ctx = nullptr;
        for (;;) {
            const int status = JS_ExecutePendingJob(runtime, &job_ctx);
            if (status == 0) break;
            if (status < 0) {
                dump_exception(job_ctx != nullptr ? job_ctx : ctx,
                               "pending job failed");
                ok = false;
                break;
            }
        }
    }
    if (ok && !expect_global_true.empty()) {
        JSValue global = JS_GetGlobalObject(ctx);
        JSValue marker =
            JS_GetPropertyStr(ctx, global, expect_global_true.c_str());
        const int truthy = JS_ToBool(ctx, marker);
        JS_FreeValue(ctx, marker);
        JS_FreeValue(ctx, global);
        if (truthy != 1) {
            std::fprintf(stderr,
                         "classic-bytecode: expected global %s to be true\n",
                         expect_global_true.c_str());
            ok = false;
        }
    }
    const auto end = std::chrono::steady_clock::now();
    *elapsed_ms =
        std::chrono::duration<double, std::milli>(end - start).count();
#ifdef CONFIG_OPCODE_PROFILE
    if (!opcode_profile.empty()) {
        FILE* profile = std::fopen(opcode_profile.c_str(), "w");
        if (profile == nullptr) {
            std::fprintf(stderr,
                         "classic-bytecode: cannot open opcode profile: %s\n",
                         opcode_profile.c_str());
            ok = false;
        } else {
            JS_DumpOpcodeProfile(profile, runtime);
            if (std::fclose(profile) != 0) {
                std::fprintf(stderr,
                             "classic-bytecode: cannot close opcode profile: %s\n",
                             opcode_profile.c_str());
                ok = false;
            }
        }
    }
#else
    (void)opcode_profile;
#endif
    JS_FreeContext(ctx);
    JS_FreeRuntime(runtime);
    return ok;
}

int run_script(const RunOptions& options) {
    std::vector<std::uint8_t> bytes;
    if (!read_file(options.input, &bytes) || bytes.empty()) {
        fail("cannot read bytecode: " + options.input);
    }
    const std::uint64_t total =
        static_cast<std::uint64_t>(options.warmup) + options.rounds;
    for (std::uint64_t index = 0; index < total; ++index) {
        double elapsed_ms = 0.0;
        if (!run_once(bytes, options.expect_global_true,
                      options.opcode_profile, &elapsed_ms)) {
            return 1;
        }
        if (index < options.warmup) continue;
        const std::uint64_t round = index - options.warmup + 1;
        std::printf("{\"round\":%llu,\"ms\":%.6f,\"ok\":true}\n",
                    static_cast<unsigned long long>(round), elapsed_ms);
        std::fflush(stdout);
    }
    return 0;
}

void usage(const char* argv0) {
    std::fprintf(
        stderr,
        "usage:\n"
        "  %s compile --input FILE --source-name NAME --output FILE "
        "[--rewrite] [--passes MASK] [--report]\n"
        "  %s run --input FILE [--warmup N] [--rounds N] "
        "[--expect-global-true NAME] [--opcode-profile FILE]\n",
        argv0, argv0);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        usage(argv[0]);
        return 2;
    }
    const std::string command = argv[1];
    if (command == "compile") return compile_script(parse_compile(argc, argv));
    if (command == "run") return run_script(parse_run(argc, argv));
    usage(argv[0]);
    return 2;
}
