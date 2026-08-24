// Opcode profile v3 contract: every executed instruction has source
// provenance plus an exact runtime-local function id + original PC record,
// and property sites classify the path actually taken. The profile build is
// diagnostic only.
#include "quickjs.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

int failures = 0;

void check(const char* name, bool ok) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok)
        failures++;
}

}  // namespace

int main() {
    JSRuntime* rt = JS_NewRuntime();
    JSContext* ctx = rt ? JS_NewContext(rt) : nullptr;
    if (!rt || !ctx)
        return 2;

    const char* src =
        "function field(o) { return o.x; }"
        "const own = {x: 1}; const proto = {x: 2};"
        "const inherited = Object.create(proto);"
        "const accessor = {get x() { return 3; }};"
        "let s = 0; for (let i = 0; i < 200; i++) s += field(own);"
        "s += field(inherited) + field(accessor); field(3);"
        "const a = [4, 5]; s += a[0]; void a[99]; globalThis.__r = s;";
    JSValue value = JS_Eval(ctx, src, std::strlen(src), "profile.js",
                            JS_EVAL_TYPE_GLOBAL);
    check("profile_eval", !JS_IsException(value));
    JS_FreeValue(ctx, value);

    FILE* fp = std::tmpfile();
    check("profile_tmpfile", fp != nullptr);
    std::string dump;
    if (fp) {
        JS_DumpOpcodeProfile(fp, rt);
        std::fflush(fp);
        std::fseek(fp, 0, SEEK_END);
        const long n = std::ftell(fp);
        std::rewind(fp);
        if (n > 0) {
            dump.resize(static_cast<std::size_t>(n));
            const std::size_t got =
                std::fread(&dump[0], 1, dump.size(), fp);
            dump.resize(got);
        }
        std::fclose(fp);
    }

    check("profile_schema_v3",
          dump.find("\"schema\":\"quickjs-ng-opcode-profile-v3\"") !=
              std::string::npos);
    check("profile_exact_sites",
          dump.find("\"sites\":[{") != std::string::npos &&
              dump.find("\"function\":") != std::string::npos &&
              dump.find("\"pc\":") != std::string::npos &&
              dump.find("\"source_hash\":\"a3cd291695095097\"") !=
                  std::string::npos &&
              dump.find("\"exec\":") != std::string::npos);
    check("profile_no_site_overflow",
          dump.find("\"site_overflow\":0") != std::string::npos);
    check("profile_own_direct",
          dump.find("\"direct\":") != std::string::npos);
    check("profile_proto_or_int_fallback",
          dump.find("\"prototype_or_int_fallback\":") !=
              std::string::npos);
    check("profile_accessor_generic",
          dump.find("\"accessor_or_generic\":") != std::string::npos);
    check("profile_primitive",
          dump.find("\"primitive_or_nullish\":") != std::string::npos);

    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    if (failures) {
        std::fprintf(stderr, "test_opcode_profile: %d failure(s)\n", failures);
        return 1;
    }
    std::fprintf(stderr, "test_opcode_profile: all green\n");
    return 0;
}
