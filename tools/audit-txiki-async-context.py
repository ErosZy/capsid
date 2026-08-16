#!/usr/bin/env python3
"""WP-03 §7.3 async-context inventory audit gate.

Every entry point where the txiki overlay calls back into JavaScript from a
libuv callback must be consciously classified:

  * ``tjs_call_handler`` / ``tjs_call_handler_ctx`` invocation sites,
  * ``TJS_InitPromise`` / ``TJS_SettlePromise`` / ``TJS_ClearPromise``
    invocation sites,
  * structs that store a ``JSValue callback`` / ``JSValue func`` field and
    use it across a libuv tick.

Each item must belong to one of the classes below; an unclassified site is
a CI failure (spec §7.3: "出现新调用点而未分类时 CI 失败"). The tables in
this file are the archived inventory for the txiki upgrade report.

Classes
-------
``context-wired``
    Reached from the capsid restricted profile and wired through patch
    0013 (TJSAsyncContextHooks): the async context is captured at resource
    creation, restored around the callback (``tjs_call_handler_ctx`` /
    ``tjs_enter_ctx`` / ``tjs_leave_ctx``), and released at destruction.

``profile-unreachable``
    Reachable only through the native ``tjs`` global or native module
    imports. ``tjs`` is absent from ``profileGlobalNames`` in
    ``src/js/profile-manifest.js``, so the capsid bootstrap deletes it from
    the app realm (``delete globalThis[name]`` for every non-profile
    global). A module that the application cannot name cannot invoke its
    callbacks — but the gate keeps that reasoning explicit and auditable
    instead of assuming it silently.

``synchronous-reentry``
    Reachable from the profile (WebAssembly) but the stored JSValue is
    invoked only synchronously inside the current JS call stack (the WAMR
    import trampoline calls ``JS_Call`` while the WebAssembly execution is
    already inside a JS call) — never across a libuv tick, so there is no
    async context to leak.

``value-only``
    The struct's JSValue fields are data / GC-root references (URL
    components, promise results) and are never invoked as callbacks, let
    alone across a libuv tick. No async entry point exists in the file.

Usage: audit-txiki-async-context.py <overlay-src-dir> [--report]
"""

import argparse
import os
import re
import sys

CLASS_WIRED = "context-wired"
CLASS_UNREACHABLE = "profile-unreachable"
CLASS_SYNC = "synchronous-reentry"
CLASS_VALUE = "value-only"

REASONS = {
    CLASS_WIRED: (
        "reached from the capsid restricted profile; wired via patch 0013 "
        "(TJSAsyncContextHooks capture at creation, contextual invoke, "
        "release at destruction)"),
    CLASS_UNREACHABLE: (
        "reachable only through the native `tjs` global or native module "
        "imports; `tjs` is not in profile-manifest.js profileGlobalNames so "
        "the capsid bootstrap deletes it from the app realm"),
    CLASS_SYNC: (
        "reachable (WebAssembly) but invoked only synchronously inside the "
        "current JS call stack (WAMR import trampoline) — never across a "
        "libuv tick"),
    CLASS_VALUE: (
        "JSValue struct fields are data / GC-root references, never invoked "
        "as callbacks from a libuv tick"),
}

# (file, enclosing function) -> class, for every tjs_call_handler call site.
# The two utils.c entries are the body of the tjs_call_handler_ctx wrapper:
# the fallback for a NULL ctx_data and the contextual call after enter.
HANDLER_SITES = {
    ("utils.c", "tjs_call_handler_ctx"): CLASS_WIRED,
    ("httpserver.c", "tjs_http_emit_body_chunk"): CLASS_UNREACHABLE,
    ("httpserver.c", "tjs_http_callback"): CLASS_UNREACHABLE,
    ("httpserver.c", "tjs_http_invoke_handler"): CLASS_UNREACHABLE,
    ("httpclient.c", "maybe_invoke_callback"): CLASS_WIRED,
    ("mod_fswatch.c", "uv__fs_event_cb"): CLASS_WIRED,
    ("mod_process.c", "uv__exit_cb"): CLASS_UNREACHABLE,
    ("mod_streams.c", "maybe_invoke_callback"): CLASS_WIRED,
    ("mod_tls.c", "maybe_invoke_tls_callback"): CLASS_WIRED,
    ("mod_udp.c", "maybe_invoke_callback"): CLASS_WIRED,
    ("signals.c", "uv__signal_cb"): CLASS_UNREACHABLE,
    ("timers.c", "uv__timer_cb"): CLASS_WIRED,
    ("webcrypto.c", "tjs__digest_after_work_cb"): CLASS_WIRED,
    ("webcrypto.c", "tjs__hmac_sign_after_work_cb"): CLASS_WIRED,
    ("webcrypto.c", "tjs__cipher_after_work_cb"): CLASS_WIRED,
    ("webcrypto.c", "tjs__pbkdf2_after_work_cb"): CLASS_WIRED,
    ("webcrypto.c", "tjs__hkdf_after_work_cb"): CLASS_WIRED,
    ("webcrypto.c", "tjs__ec_generate_key_after_work_cb"): CLASS_WIRED,
    ("webcrypto.c", "tjs__ecdsa_sign_after_work_cb"): CLASS_WIRED,
    ("webcrypto.c", "tjs__ecdsa_verify_after_work_cb"): CLASS_WIRED,
    ("webcrypto.c", "tjs__ecdh_derive_bits_after_work_cb"): CLASS_WIRED,
    ("webcrypto.c", "tjs__rsa_generate_key_after_work_cb"): CLASS_WIRED,
    ("webcrypto.c", "tjs__rsa_oaep_encrypt_after_work_cb"): CLASS_WIRED,
    ("webcrypto.c", "tjs__rsa_oaep_decrypt_after_work_cb"): CLASS_WIRED,
    ("webcrypto.c", "tjs__rsa_sign_after_work_cb"): CLASS_WIRED,
    ("webcrypto.c", "tjs__rsa_verify_after_work_cb"): CLASS_WIRED,
    ("webcrypto.c", "tjs__ed25519_generate_key_after_work_cb"): CLASS_WIRED,
    ("webcrypto.c", "tjs__ed25519_sign_after_work_cb"): CLASS_WIRED,
    ("webcrypto.c", "tjs__ed25519_verify_after_work_cb"): CLASS_WIRED,
    ("webcrypto.c", "tjs__x25519_generate_key_after_work_cb"): CLASS_WIRED,
    ("webcrypto.c", "tjs__x25519_derive_bits_after_work_cb"): CLASS_WIRED,
    ("worker.c", "emit_event"): CLASS_UNREACHABLE,
    ("ws.c", "maybe_call_callback"): CLASS_WIRED,
}

# (file, enclosing function) -> class, for every TJS_(Init|Settle|Clear)
# Promise call site. utils.c entries are the hook-wired definitions
# themselves: TJS_InitPromise captures, TJS_SettlePromise enters/leaves
# around resolve/reject and releases through TJS_ClearPromise.
PROMISE_SITES = {
    ("utils.c", "TJS_InitPromise"): CLASS_WIRED,
    ("utils.c", "TJS_SettlePromise"): CLASS_WIRED,
    ("utils.c", "TJS_ClearPromise"): CLASS_WIRED,
    ("mod_dns.c", "uv__getaddrinfo_cb"): CLASS_WIRED,
    ("mod_dns.c", "tjs_dns_getaddrinfo"): CLASS_WIRED,
    ("mod_fs.c", "tjs_fsreq_init"): CLASS_WIRED,
    ("mod_fs.c", "uv__fs_req_cb"): CLASS_WIRED,
    ("mod_fs.c", "tjs__readfile_after_work_cb"): CLASS_WIRED,
    ("mod_fs.c", "tjs_fs_readfile"): CLASS_WIRED,
}

# Files whose structs hold JSValue callback/func fields used across a libuv
# tick (per-file granularity). Local variables and function parameters are
# not struct fields and are excluded by the detector below.
STRUCT_FILES = {
    "httpclient.c": CLASS_WIRED,   # TJSHc callbacks[] + ctx_data
    "timers.c": CLASS_WIRED,       # TJSTimer.callback + ctx_data
    "webcrypto.c": CLASS_WIRED,    # TJSWebCryptoOp callback + ctx_data
    # utils.c is NOT listed: TJSPromise's JSValue fields (rfuncs) live in
    # utils.h, which the field detector does not scan; utils.c's inventory
    # is the PROMISE_SITES wired entries (TJS_InitPromise / Settle /
    # ClearPromise).
    "url.c": CLASS_VALUE,          # TJSURL.url_obj / search_params: data refs
    "wasm.c": CLASS_SYNC,          # TJSWasmImportCtx.func (WAMR trampoline)
    "httpserver.c": CLASS_UNREACHABLE,  # server callbacks + ws callbacks
    "mod_ffi.c": CLASS_UNREACHABLE,
    "mod_fs.c": CLASS_VALUE,            # TJSFileReq.result (promise data)
    "mod_fswatch.c": CLASS_WIRED,
    "mod_posix-socket.c": CLASS_UNREACHABLE,
    "mod_process.c": CLASS_UNREACHABLE,  # onexit
    "mod_streams.c": CLASS_WIRED,
    "mod_tls.c": CLASS_WIRED,
    "mod_udp.c": CLASS_WIRED,
    "signals.c": CLASS_UNREACHABLE,      # sh->func
    "worker.c": CLASS_UNREACHABLE,
    "ws.c": CLASS_WIRED,
}


def enclosing_function(lines, index):
    """Name of the function containing ``lines[index]``, or None."""
    for i in range(index, -1, -1):
        line = lines[i]
        if not line.strip():
            continue
        if line[0].isspace():
            continue
        # Column-0 candidate: function signature (possibly multi-line, e.g.
        # "static void" on its own line) or a bare macro/control token.
        if re.match(r'^(?:typedef|#)', line):
            continue
        m = re.match(r'^(?:[A-Za-z_][\w .*]*\s+)?([A-Za-z_]\w*)\s*\(', line)
        if m and ';' not in line and 'typedef' not in line:
            return m.group(1)
    return None


def strip_comments(src):
    src = re.sub(r'/\*.*?\*/', ' ', src, flags=re.S)
    src = re.sub(r'//[^\n]*', ' ', src)
    return src


def scan_handler_sites(lines, path):
    sites = []
    for idx, line in enumerate(lines):
        if not re.search(r'\btjs_call_handler(?:_ctx)?\s*\(', line):
            continue
        if re.match(
                r'\s*(?:void|static)\s+tjs_call_handler(?:_ctx)?\s*\(',
                line):
            continue  # definition
        fn = enclosing_function(lines, idx)
        sites.append((os.path.basename(path), idx + 1, fn,
                      line.strip()[:72]))
    return sites


def scan_promise_sites(lines, path):
    sites = []
    pat = re.compile(r'\bTJS_(?:Init|Settle|Clear)Promise\b')
    for idx, line in enumerate(lines):
        for m in pat.finditer(line):
            name = m.group(0)
            # Definitions are scanned too: the hook-wired definitions in
            # utils.c are themselves inventory entries (capture at init,
            # enter/leave at settle, release at clear).
            fn = enclosing_function(lines, idx)
            sites.append((os.path.basename(path), idx + 1, fn,
                          name + ':', line.strip()[:64]))
    return sites


def scan_struct_files(files):
    """Files whose structs hold any JSValue field.

    The field name varies by module (callback, callbacks[], func, cb,
    onexit, rfuncs...); the property that matters for §7.3 is that a
    JSValue is stored on a native struct and survives a libuv tick. The
    per-file classification then decides whether that struct's JSValue is
    invoked from the tick (wired / unreachable / synchronous-reentry).
    """
    found = {}
    field_pat = re.compile(r'^\s*JSValue\s+\w+\b')
    struct_header = re.compile(
        r'(?:typedef\s+struct|\bstruct\s+\w+\s*\{|\}\s*\w+\s*\{)')
    signature = re.compile(
        r'^(?:[A-Za-z_][\w .*]*\s+)?[A-Za-z_]\w*\s*\(')
    for path in files:
        try:
            src = open(path, encoding='utf-8', errors='replace').read()
        except OSError:
            continue
        src = strip_comments(src)
        lines = src.split('\n')
        for idx, line in enumerate(lines):
            if not field_pat.match(line):
                continue
            # A struct field sits inside a struct definition: scanning
            # backwards, the nearest column-0 boundary must be a struct
            # header — a function signature boundary first means the
            # match is a local variable or a function return type.
            is_field = False
            for i in range(idx - 1, max(0, idx - 40) - 1, -1):
                w = lines[i]
                if not w.strip():
                    continue
                if w[0].isspace():
                    continue
                if struct_header.search(w):
                    is_field = True
                    break
                if signature.match(w) or w.startswith('}'):
                    break  # function boundary or struct end without header
            if not is_field:
                continue
            found.setdefault(os.path.basename(path), []).append(idx + 1)
    return found


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('overlay_src_dir',
                        help='prepared overlay: build-m02/vendor-overlay/'
                             'txiki.js/src')
    parser.add_argument('--report', action='store_true',
                        help='print the full archived classification')
    args = parser.parse_args(argv)

    src_dir = args.overlay_src_dir
    if not os.path.isdir(src_dir):
        print(f'audit-txiki-async-context: not a directory: {src_dir}',
              file=sys.stderr)
        return 2

    c_files = []
    for root, dirs, files in os.walk(src_dir):
        dirs[:] = [d for d in dirs if d not in ('deps', '.git')]
        c_files.extend(os.path.join(root, f) for f in files
                       if f.endswith('.c'))
    c_files.sort()

    failures = []
    counts = {cls: 0 for cls in REASONS}

    def classify(kind, key):
        table = (HANDLER_SITES if kind == 'handler' else
                 PROMISE_SITES if kind == 'promise' else STRUCT_FILES)
        cls = table.get(key)
        if cls is None:
            return None
        counts[cls] += 1
        return cls

    lines_by_file = {}
    for path in c_files:
        lines_by_file[path] = open(path, encoding='utf-8',
                                   errors='replace').read().split('\n')

    for path in c_files:
        lines = lines_by_file[path]
        for fname, lineno, fn, text in scan_handler_sites(lines, path):
            cls = classify('handler', (fname, fn))
            if cls is None:
                failures.append(
                    f'{fname}:{lineno}: tjs_call_handler() call site not '
                    f'classified (enclosing function {fn!r})')
        for fname, lineno, fn, _name, _text in scan_promise_sites(lines, path):
            cls = classify('promise', (fname, fn))
            if cls is None:
                failures.append(
                    f'{fname}:{lineno}: TJS_*Promise call site not '
                    f'classified (enclosing function {fn!r})')

    struct_found = scan_struct_files(c_files)
    for fname, linenos in sorted(struct_found.items()):
        cls = classify('struct', fname)
        if cls is None:
            failures.append(
                f'{fname}: struct holds JSValue field at lines '
                f'{",".join(map(str, linenos))} but the file is not '
                f'classified')

    # Stale entries: a table row that no longer matches any scan result
    # means the inventory drifted from the source (or a patch reworked the
    # call site without updating the archive). Either way it must fail.
    handler_seen = set()
    promise_seen = set()
    for path in c_files:
        lines = lines_by_file[path]
        for fname, _ln, fn, _text in scan_handler_sites(lines, path):
            handler_seen.add((fname, fn))
        for fname, _ln, fn, _nm, _text in scan_promise_sites(lines, path):
            promise_seen.add((fname, fn))
    for key in sorted(HANDLER_SITES):
        if key not in handler_seen:
            failures.append(
                f'{key[0]}.{key[1]}: handler-site inventory entry has no '
                f'matching tjs_call_handler() call site')
    for key in sorted(PROMISE_SITES):
        if key not in promise_seen:
            failures.append(
                f'{key[0]}.{key[1]}: promise-site inventory entry has no '
                f'matching TJS_*Promise call site')
    for fname in sorted(STRUCT_FILES):
        if fname not in struct_found:
            failures.append(
                f'{fname}: struct inventory entry has no JSValue struct '
                f'field in the overlay')

    print('txiki async-context inventory (WP-03 §7.3)')
    for cls in (CLASS_WIRED, CLASS_UNREACHABLE, CLASS_SYNC, CLASS_VALUE):
        print(f'  {counts[cls]:2d} {cls}')

    if args.report:
        print()
        print('handler call sites:')
        for key in sorted(HANDLER_SITES):
            print(f'  {".".join(key):44s} {HANDLER_SITES[key]}')
        print('promise call sites:')
        for key in sorted(PROMISE_SITES):
            print(f'  {".".join(key):44s} {PROMISE_SITES[key]}')
        print('callback-holding struct files:')
        for key in sorted(STRUCT_FILES):
            print(f'  {key:24s} {STRUCT_FILES[key]}')

    if failures:
        print(f'audit FAILED: {len(failures)} unclassified item(s):',
              file=sys.stderr)
        for item in failures:
            print(f'  {item}', file=sys.stderr)
        print('  classify each item in tools/audit-txiki-async-context.py '
              'as context-wired, profile-unreachable or '
              'synchronous-reentry (spec §7.3).',
              file=sys.stderr)
        return 1

    print('audit OK: every async entry point into JS is classified')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
