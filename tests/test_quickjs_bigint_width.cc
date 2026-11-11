#include "quickjs.h"

#include <cstdio>
#include <cstring>

int main() {
    JSRuntime *runtime = JS_NewRuntime();
    if (!runtime) return 1;
    JS_SetMemoryLimit(runtime, 64U * 1024U * 1024U);
    JSContext *context = JS_NewContext(runtime);
    if (!context) {
        JS_FreeRuntime(runtime);
        return 1;
    }
    // Arithmetic modulo is independent of the builtins' limb masking and
    // sign-extension shortcuts. Exercise both short and heap BigInts.
    const char *source = R"JS((() => {
        let checks = 0, failed = 0;
        const examples = [];
        const check = (condition, label) => {
            ++checks;
            if (condition) return;
            ++failed;
            if (examples.length < 12) examples.push(label);
        };
        const fixed = [0n, 1n, -1n, 2n, -2n, 2147483647n, -2147483648n,
            2147483648n, -2147483649n, 4294967295n, 4294967296n,
            9223372036854775807n, -9223372036854775808n,
            18446744073709551615n, -18446744073709551616n,
            18446744073709551616n, -(1n << 193n), (1n << 193n) + 3n];
        for (let bits = 0; bits <= 257; ++bits) {
            const modulus = 1n << BigInt(bits);
            const values = fixed.concat([modulus - 1n, modulus, modulus + 1n,
                -modulus - 1n, -modulus, -modulus + 1n,
                modulus / 2n - 1n, modulus / 2n, modulus / 2n + 1n]);
            for (const value of values) {
                const unsigned = ((value % modulus) + modulus) % modulus;
                const signed = bits && unsigned >= modulus / 2n ? unsigned - modulus : unsigned;
                check(BigInt.asUintN(bits, value) === unsigned, `uint ${bits} ${value}`);
                check(BigInt.asIntN(bits, value) === signed, `int ${bits} ${value}`);
            }
        }
        const throws = (type, callback, label) => {
            let error;
            try { callback(); } catch (caught) { error = caught; }
            check(error instanceof type, label);
        };
        for (const name of ['asUintN', 'asIntN']) {
            const convert = BigInt[name];
            check(convert('64', '-1') === (name === 'asUintN' ? 18446744073709551615n : -1n), `${name} strings`);
            check(convert(1.9, -1n) === (name === 'asUintN' ? 1n : -1n), `${name} fraction`);
            check(convert(NaN, -1n) === 0n && convert(undefined, -1n) === 0n, `${name} zero index`);
            check(convert(-0.9, -1n) === 0n && convert(0, true) === 0n, `${name} index truncation`);
            check(convert(64, true) === 1n && convert(64, false) === 0n, `${name} boolean`);
            check(convert(Number.MAX_SAFE_INTEGER, 0n) === 0n, `${name} wide zero`);
            throws(RangeError, () => convert(-1, 0n), `${name} negative width`);
            throws(RangeError, () => convert(Infinity, 0n), `${name} infinite width`);
            throws(RangeError, () => convert(Number.MAX_SAFE_INTEGER + 1, 0n), `${name} width overflow`);
            throws(TypeError, () => convert(1n, 0n), `${name} BigInt width`);
            throws(TypeError, () => convert(64, 1), `${name} Number value`);
            throws(TypeError, () => convert(64, Symbol()), `${name} Symbol value`);
            throws(SyntaxError, () => convert(64, 'not an integer'), `${name} invalid string`);
            const marker = {};
            let events = [], caught;
            try {
                convert({valueOf() { events.push('bits'); throw marker; }},
                    {valueOf() { events.push('value'); return -1n; }});
            } catch (error) { caught = error; }
            check(caught === marker && events.join() === 'bits', `${name} first conversion exception`);
            events = []; caught = undefined;
            try {
                convert({valueOf() { events.push('bits'); return 0; }},
                    {valueOf() { events.push('value'); throw marker; }});
            } catch (error) { caught = error; }
            check(caught === marker && events.join() === 'bits,value', `${name} zero-width value conversion`);
            events = [];
            const converted = convert({[Symbol.toPrimitive](hint) { events.push('bits:' + hint); return 64; }},
                {[Symbol.toPrimitive](hint) { events.push('value:' + hint); return -1n; }});
            check(events.join() === 'bits:number,value:number' &&
                converted === (name === 'asUintN' ? 18446744073709551615n : -1n), `${name} primitive order`);
        }
        throws(RangeError, () => BigInt.asUintN(Number.MAX_SAFE_INTEGER, -1n), 'negative unsigned allocation limit');
        check(BigInt.asIntN(Number.MAX_SAFE_INTEGER, -1n) === -1n, 'wide signed negative');
        for (let i = 0; i < 2048; ++i) {
            const bits = 32 + i % 226;
            check(BigInt.asUintN(bits, -BigInt(i + 1)) === (1n << BigInt(bits)) - BigInt(i + 1), 'repeated allocation ' + i);
        }
        if (failed) throw new Error(JSON.stringify({checks, failed, examples}));
        return checks;
    })())JS";
    JSValue result = JS_Eval(context, source, std::strlen(source),
        "bigint-width-regression.js", JS_EVAL_TYPE_GLOBAL);
    int status = 0;
    if (JS_IsException(result)) {
        JSValue error = JS_GetException(context);
        const char *message = JS_ToCString(context, error);
        std::fprintf(stderr, "FAIL: %s\n", message ? message : "BigInt evaluation failed");
        JS_FreeCString(context, message);
        JS_FreeValue(context, error);
        status = 1;
    } else {
        int checks = 0;
        if (JS_ToInt32(context, &checks, result) < 0 || checks != 16014) status = 1;
        std::printf("%s: %d BigInt width, conversion and allocation checks\n", status ? "FAIL" : "PASS", checks);
    }
    JS_FreeValue(context, result);
    JS_RunGC(runtime);
    JS_FreeContext(context);
    JS_FreeRuntime(runtime);
    return status;
}
