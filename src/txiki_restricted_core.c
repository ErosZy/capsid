/*
 * Minimal txiki.js native helpers for the Capsid restricted build.
 *
 * The implementations below are derived from txiki.js's MIT-licensed
 * mod_engine.c, mod_os.c, and mod_sys.c. Keeping them in the embedding layer
 * avoids registering or linking the unrelated process, filesystem, REPL, and
 * host-introspection helpers from those broad modules.
 */

#include "private.h"
#include "quickjs.h"
#include "utils.h"

#include <errno.h>
#include <iconv.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <uv.h>

int capsid_tjs_set_ca_bundle_path(TJSRuntime *runtime, const char *path) {
    if (!runtime || !path || runtime->lws.ca_bundle_path) {
        return -1;
    }
    runtime->lws.ca_bundle_path = js_strdup(runtime->ctx, path);
    return runtime->lws.ca_bundle_path ? 0 : -1;
}

int capsid_tjs_set_cookie_jar_path(TJSRuntime *runtime, const char *path) {
    if (!runtime || !path || runtime->lws.cookie_jar_path) {
        return -1;
    }
    runtime->lws.cookie_jar_path = js_strdup(runtime->ctx, path);
    return runtime->lws.cookie_jar_path ? 0 : -1;
}

int capsid_tjs_set_egress_policy(
    TJSRuntime *runtime,
    int (*check)(void *opaque,
                 const char *host,
                 uint16_t port,
                 const struct sockaddr *address,
                 socklen_t address_len,
                 char *reason,
                 size_t reason_size),
    void *opaque) {
    if (!runtime || !check || runtime->egress.check) {
        return -1;
    }
    runtime->egress.check = check;
    runtime->egress.opaque = opaque;
    return 0;
}

int capsid_tjs_set_fs_policy(
    TJSRuntime *runtime,
    int (*check)(void *opaque, const char *path, int write_access,
                 char *reason, size_t reason_size),
    void *opaque) {
    if (!runtime || !check || runtime->capsid_fs.check) {
        return -1;
    }
    runtime->capsid_fs.check = check;
    runtime->capsid_fs.opaque = opaque;
    return 0;
}

int capsid_tjs_install_binding_fs(TJSRuntime *runtime) {
    if (!runtime) {
        return -1;
    }
    /* The restricted-core bootstrap deliberately omits the fs module; the
     * Binding Runtime registers it so bindings get the raw core.fs path,
     * with every entry point gated by the per-origin FS policy (patch
     * 0019). The User runtime never registers it.
     *
     * txiki's fs module registers its functions flat on the namespace it
     * is given, so it goes on a dedicated sub-object: bindings address it
     * as core.fs (Binding v1 §4.1), keeping the flat core namespace
     * identical between User and Binding runtimes. */
    JSContext *ctx = runtime->ctx;
    JSValue fs = JS_NewObject(ctx);
    if (JS_IsException(fs)) {
        return -1;
    }
    tjs__mod_fs_init(ctx, fs);
    JS_SetPropertyStr(ctx, TJS_GetInternalCore(runtime), "fs", fs);
    return 0;
}

static JSValue capsid_serialize(JSContext *ctx,
                              JSValueConst this_val,
                              int argc,
                              JSValueConst *argv) {
    (void) this_val;
    (void) argc;
    size_t size = 0;
    const int flags =
        JS_WRITE_OBJ_BYTECODE | JS_WRITE_OBJ_REFERENCE |
        JS_WRITE_OBJ_SAB | JS_WRITE_OBJ_STRIP_SOURCE;
    uint8_t *data = JS_WriteObject(ctx, &size, argv[0], flags);
    if (!data) {
        return JS_EXCEPTION;
    }
    JSValue result = TJS_NewUint8Array(ctx, data, size);
    if (JS_IsException(result)) {
        js_free(ctx, data);
    }
    return result;
}

static JSValue capsid_deserialize(JSContext *ctx,
                                JSValueConst this_val,
                                int argc,
                                JSValueConst *argv) {
    (void) this_val;
    (void) argc;
    size_t size = 0;
    const uint8_t *data = JS_GetUint8Array(ctx, &size, argv[0]);
    if (!data) {
        return JS_EXCEPTION;
    }
    return JS_ReadObject(
        ctx,
        data,
        size,
        JS_READ_OBJ_BYTECODE | JS_READ_OBJ_REFERENCE | JS_READ_OBJ_SAB);
}

static JSValue capsid_random(JSContext *ctx,
                           JSValueConst this_val,
                           int argc,
                           JSValueConst *argv) {
    (void) this_val;
    size_t size = 0;
    uint8_t *data = JS_GetArrayBuffer(ctx, &size, argv[0]);
    if (!data) {
        return JS_EXCEPTION;
    }

    uint64_t offset = 0;
    if (argc > 1 && !JS_IsUndefined(argv[1]) &&
        JS_ToIndex(ctx, &offset, argv[1])) {
        return JS_EXCEPTION;
    }

    uint64_t length = size;
    if (argc > 2 && !JS_IsUndefined(argv[2]) &&
        JS_ToIndex(ctx, &length, argv[2])) {
        return JS_EXCEPTION;
    }
    if (offset > size || length > size - offset) {
        return JS_ThrowRangeError(ctx, "array buffer overflow");
    }

    const int result =
        uv_random(NULL, NULL, data + offset, (size_t) length, 0, NULL);
    if (result != 0) {
        return tjs_throw_errno(ctx, result);
    }
    return JS_UNDEFINED;
}

static JSValue capsid_random_uuid(JSContext *ctx,
                                JSValueConst this_val,
                                int argc,
                                JSValueConst *argv) {
    (void) this_val;
    (void) argc;
    (void) argv;
    unsigned char value[16];
    const int result =
        uv_random(NULL, NULL, value, sizeof(value), 0, NULL);
    if (result != 0) {
        return tjs_throw_errno(ctx, result);
    }

    value[6] = (unsigned char) ((value[6] & 15u) | 64u);
    value[8] = (unsigned char) ((value[8] & 63u) | 128u);

    char text[37];
    snprintf(text,
             sizeof(text),
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-"
             "%02x%02x-%02x%02x%02x%02x%02x%02x",
             value[0],
             value[1],
             value[2],
             value[3],
             value[4],
             value[5],
             value[6],
             value[7],
             value[8],
             value[9],
             value[10],
             value[11],
             value[12],
             value[13],
             value[14],
             value[15]);
    return JS_NewString(ctx, text);
}

static JSValue capsid_is_array_buffer(JSContext *ctx,
                                    JSValueConst this_val,
                                    int argc,
                                    JSValueConst *argv) {
    (void) this_val;
    (void) argc;
    return JS_NewBool(ctx, JS_IsArrayBuffer(argv[0]));
}

static JSValue capsid_detach_array_buffer(JSContext *ctx,
                                        JSValueConst this_val,
                                        int argc,
                                        JSValueConst *argv) {
    (void) ctx;
    (void) this_val;
    (void) argc;
    JS_DetachArrayBuffer(ctx, argv[0]);
    return JS_UNDEFINED;
}

static const char *capsid_legacy_encoding(const char *name) {
    if (strcmp(name, "GBK") == 0) {
        /*
         * WHATWG GBK uses the full two-byte GB18030 index. Some Unix iconv
         * GBK tables omit those extension rows, while GB18030 retains them.
         */
        return "GB18030";
    }
    if (strcmp(name, "gb18030") == 0) {
        return "GB18030";
    }
    if (strcmp(name, "Big5") == 0) {
        return "BIG5-HKSCS";
    }
    if (strcmp(name, "EUC-JP") == 0) {
        return "EUC-JP";
    }
    if (strcmp(name, "ISO-2022-JP") == 0) {
        return "ISO-2022-JP";
    }
    if (strcmp(name, "Shift_JIS") == 0) {
        return "SHIFT_JIS";
    }
    if (strcmp(name, "EUC-KR") == 0) {
        return "CP949";
    }
    return NULL;
}

/*
 * This is deliberately a narrow bootstrap primitive, not a public codec API.
 * Label normalization and TextDecoder state remain in JavaScript, while the
 * host conversion table avoids embedding several hundred KiB of indexes.
 */
static JSValue capsid_legacy_decode(JSContext *ctx,
                                  JSValueConst this_val,
                                  int argc,
                                  JSValueConst *argv) {
    (void) this_val;
    (void) argc;

    const char *requested = JS_ToCString(ctx, argv[0]);
    if (!requested) {
        return JS_EXCEPTION;
    }
    const char *encoding = capsid_legacy_encoding(requested);
    JS_FreeCString(ctx, requested);
    if (!encoding) {
        return JS_ThrowRangeError(ctx, "unsupported internal encoding");
    }

    size_t input_size = 0;
    const uint8_t *input = JS_GetUint8Array(ctx, &input_size, argv[1]);
    if (!input) {
        return JS_EXCEPTION;
    }
    const int fatal = JS_ToBool(ctx, argv[2]);
    if (fatal < 0) {
        return JS_EXCEPTION;
    }

    if (input_size > (SIZE_MAX - 16u) / 4u) {
        return JS_ThrowRangeError(ctx, "input is too large");
    }
    const size_t output_capacity = input_size * 4u + 16u;
    char *output = js_malloc(ctx, output_capacity);
    if (!output) {
        return JS_ThrowOutOfMemory(ctx);
    }

    iconv_t converter = iconv_open("UTF-8", encoding);
    if (converter == (iconv_t) -1) {
        js_free(ctx, output);
        return JS_ThrowInternalError(ctx, "host encoding is unavailable");
    }

    char *input_cursor = (char *) (uintptr_t) input;
    size_t input_left = input_size;
    char *output_cursor = output;
    size_t output_left = output_capacity;
    while (input_left > 0) {
        if (iconv(converter,
                  &input_cursor,
                  &input_left,
                  &output_cursor,
                  &output_left) != (size_t) -1) {
            break;
        }
        if (errno != EILSEQ && errno != EINVAL) {
            iconv_close(converter);
            js_free(ctx, output);
            return JS_ThrowInternalError(ctx, "host decoding failed");
        }
        if (fatal) {
            iconv_close(converter);
            js_free(ctx, output);
            return JS_ThrowTypeError(ctx, "decoding error");
        }
        if (output_left < 3u) {
            iconv_close(converter);
            js_free(ctx, output);
            return JS_ThrowInternalError(ctx, "host decoding overflow");
        }
        *output_cursor++ = (char) 0xef;
        *output_cursor++ = (char) 0xbf;
        *output_cursor++ = (char) 0xbd;
        output_left -= 3u;

        if (errno == EINVAL) {
            input_cursor += input_left;
            input_left = 0;
        } else {
            input_cursor++;
            input_left--;
            (void) iconv(converter, NULL, NULL, NULL, NULL);
        }
    }

    if (iconv(converter,
              NULL,
              NULL,
              &output_cursor,
              &output_left) == (size_t) -1) {
        iconv_close(converter);
        js_free(ctx, output);
        return JS_ThrowInternalError(ctx, "host decoding flush failed");
    }
    iconv_close(converter);

    JSValue result =
        JS_NewStringLen(ctx, output, output_capacity - output_left);
    js_free(ctx, output);
    return result;
}

void tjs__mod_engine_init(JSContext *ctx, JSValue ns) {
    static const JSCFunctionListEntry functions[] = {
        TJS_CFUNC_DEF("serialize", 1, capsid_serialize),
        TJS_CFUNC_DEF("deserialize", 1, capsid_deserialize),
    };
    JS_SetPropertyFunctionList(ctx, ns, functions, countof(functions));
}

void tjs__mod_os_init(JSContext *ctx, JSValue ns) {
    static const JSCFunctionListEntry functions[] = {
        TJS_CFUNC_DEF("random", 3, capsid_random),
    };
    JS_SetPropertyFunctionList(ctx, ns, functions, countof(functions));
}

void tjs__mod_sys_init(JSContext *ctx, JSValue ns) {
    static const JSCFunctionListEntry functions[] = {
        TJS_CFUNC_DEF("randomUUID", 0, capsid_random_uuid),
        TJS_CFUNC_DEF("isArrayBuffer", 1, capsid_is_array_buffer),
        TJS_CFUNC_DEF("detachArrayBuffer", 1, capsid_detach_array_buffer),
        TJS_CFUNC_DEF("legacyDecode", 3, capsid_legacy_decode),
    };
    JS_SetPropertyFunctionList(ctx, ns, functions, countof(functions));
}
