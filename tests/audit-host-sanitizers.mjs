import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import {fileURLToPath} from 'node:url';

// These native command-line parsers never execute a compile command. Quoted
// definitions containing flag-like text must remain single arguments.
export function compileArguments(command, windows) {
    assert.equal(typeof command, 'string', 'compile command string');
    const result = [];
    let token = '', quote = '', started = false;
    for (let i = 0; i < command.length; ++i) {
        const c = command[i];
        if (windows && c === '\\') {
            let end = i;
            while (command[end] === '\\') ++end;
            const count = end - i;
            if (command[end] === '"') {
                token += '\\'.repeat(Math.floor(count / 2));
                if (count % 2) token += '"'; else quote = quote ? '' : '"';
                i = end;
            } else { token += '\\'.repeat(count); i = end - 1; }
            started = true;
        } else if (!windows && c === '\\' && quote !== "'") {
            assert.ok(i + 1 < command.length, 'complete escape');
            token += command[++i]; started = true;
        } else if (c === '"' || (!windows && c === "'")) {
            if (quote === c) quote = '';
            else if (!quote) quote = c;
            else token += c;
            started = true;
        } else if (/\s/.test(c) && !quote) {
            if (started) result.push(token);
            token = ''; started = false;
        } else { token += c; started = true; }
    }
    assert.equal(quote, '', 'closed compile-command quote');
    if (started) result.push(token);
    assert.ok(result.length > 0, 'nonempty compile command');
    return result;
}

export function auditHostSanitizers(entries, options) {
    assert.ok(Array.isArray(entries) && entries.length > 0, 'nonempty compile database');
    const windows = /^[a-z]:[\\/]|^\\\\/i.test(options.sourceRoot);
    const api = windows ? path.win32 : path.posix;
    const canonical = value => {
        const normalized = api.resolve(value).replaceAll('\\', '/');
        return windows ? normalized.toLowerCase() : normalized;
    };
    const root = canonical(options.sourceRoot);
    const host = new Set(options.hostSources.map(name => canonical(api.join(options.sourceRoot, name))));
    const mandatory = ['src/host/config.cc', 'src/host/metrics.cc', 'src/host/worker_executor.cc', 'vendor/jansson/src/load.c'];
    const jansson = canonical(api.join(options.sourceRoot, 'vendor/jansson/src'));
    const expected = options.expected;
    assert.ok(expected.length > 0 && expected.every(name => ['address', 'undefined', 'thread'].includes(name)), 'explicit expected sanitizers');
    const vendor = options.vendorRoot ? canonical(options.vendorRoot) : '';
    // Some dependency directories can be consumed from the pinned original
    // tree rather than the copied overlay. Audit both source locations.
    const vendorRoots = vendor ? [vendor, canonical(api.join(root, 'vendor/txiki.js'))] : [];
    const consumerNames = ['tests/test_tjs_shared_loop.cc', 'tests/test_binding_rpc.cc',
        'tests/test_quickjs_job_context.cc', 'tests/test_quickjs_async_gc.cc', 'src/binding_rpc.cc'];
    const consumers = new Set([
        ...(vendor ? consumerNames : []),
        ...(options.runtimeConsumers ?? []),
    ].map(name => canonical(api.join(root, name))));
    const uninstrumented = new Set((options.uninstrumentedSources ?? []).map(name => canonical(api.join(root, name))));
    const fuzzerDrivers = new Map([
        ['fuzz-protocol-parser', 'tests/fuzz/fuzz_protocol_parser.cc'],
        ['fuzz-response-headers', 'tests/fuzz/fuzz_response_headers.cc'],
        ['fuzz-worker-protocol', 'tests/fuzz/fuzz_worker_protocol.cc'],
        ['fuzz-audit-decode', 'tests/fuzz/fuzz_audit_decode.cc'],
        ['fuzz-channel-registry', 'tests/fuzz/fuzz_channel_registry.cc'],
        ['fuzz-websocket-frames', 'tests/fuzz/fuzz_websocket_frames.cc'],
        ['fuzz-websocket-handshake', 'tests/fuzz/fuzz_websocket_handshake.cc'],
    ]);
    const fuzzerSeen = new Set();
    let fuzzerChecked = 0;
    const seen = new Set(), vendorSeen = new Set(), vendorRelativeSeen = new Set(), consumerSeen = new Set(), uninstrumentedSeen = new Set();
    let checked = 0, vendorChecked = 0, consumerChecked = 0, uninstrumentedChecked = 0, assemblyChecked = 0;
    for (const entry of entries) {
        assert.ok(entry && typeof entry.file === 'string' && typeof entry.directory === 'string', 'structured compile source/directory');
        const file = canonical(api.resolve(entry.directory, entry.file));
        const vendorPrefix = vendorRoots.find(prefix => file.startsWith(prefix + '/'));
        const isVendor = Boolean(vendorPrefix) && /\.(c|cc|cpp|cxx)$/i.test(file);
        if (vendorPrefix && /\.(s|asm)$/i.test(file)) ++assemblyChecked;
        const isHost = host.has(file) || (file.startsWith(jansson + '/') && /\.(c|cc|cpp|cxx)$/i.test(file));
        const isConsumer = consumers.has(file);
        const isUninstrumented = uninstrumented.has(file);
        const args = entry.arguments ?? compileArguments(entry.command, windows);
        assert.ok(Array.isArray(args) && args.length && args.every(arg => typeof arg === 'string'), 'compile arguments array');
        // Only the explicitly enabled CMake fuzz targets may add libFuzzer.
        // Use the compiler's output argument, not flag-like text in a macro.
        const outputIndex = args.indexOf('-o');
        const output = outputIndex >= 0 && args[outputIndex + 1]
            ? canonical(api.resolve(entry.directory, args[outputIndex + 1])) : '';
        const fuzzerTarget = options.fuzzers && !windows && file.startsWith(root + '/') && [...fuzzerDrivers.keys()].find(name =>
            output === canonical(api.join(entry.directory, 'CMakeFiles', name + '.dir', api.relative(root, file) + '.o')));
        if (!isHost && !isVendor && !isConsumer && !isUninstrumented && !fuzzerTarget) continue;
        if (fuzzerTarget) {
            assert.equal(args.filter(arg => arg === '-o').length, 1, `one compiler output: ${file}`);
            if (entry.output !== undefined) assert.equal(canonical(api.resolve(entry.directory, entry.output)), output, `compile output agrees: ${file}`);
        }
        assert.ok(args.some(arg => canonical(api.resolve(entry.directory, arg)) === file), `source argument agrees with file: ${file}`);
        const required = fuzzerTarget ? ['address', 'undefined', 'fuzzer'] : [...new Set(expected)];
        const enabled = new Set();
        let framePointers = false, fatalRecovery = false;
        for (const arg of args) {
            if (/^\/fsanitize=address$/i.test(arg)) enabled.add('address');
            const positive = /^-fsanitize=([a-z,]+)$/.exec(arg);
            if (positive) for (const name of positive[1].split(',')) enabled.add(name);
            assert.ok(!/^-fno-sanitize=/.test(arg), `no per-file sanitizer exclusions: ${file}`);
            if (arg === '-fno-omit-frame-pointer') framePointers = true;
            if (arg === '-fomit-frame-pointer') framePointers = false;
            if (arg === '-fno-sanitize-recover=all') fatalRecovery = true;
            const recover = /^-fsanitize-recover=([a-z,]+)$/.exec(arg);
            if (recover && recover[1].split(',').some(name => name === 'all' || required.includes(name))) fatalRecovery = false;
            if (expected.includes('address')) assert.ok(!/^[-/]D_DISABLE_(STRING|VECTOR)_ANNOTATION(?:=|$)/i.test(arg), `STL annotations retained: ${file}`);
        }
        if (isUninstrumented) {
            assert.equal(enabled.size, 0, `dependency flags leaked into uninstrumented peer: ${file}`);
            ++uninstrumentedChecked; uninstrumentedSeen.add(file); continue;
        }
        for (const name of expected) assert.ok(enabled.has(name), `${name} does not instrument ${file}`);
        assert.deepEqual([...enabled].sort(), required.sort(), `unexpected sanitizer mode: ${file}`);
        if (!windows) {
            assert.ok(framePointers, `frame pointers retained: ${file}`);
            assert.ok(fatalRecovery, `fatal sanitizer recovery policy retained: ${file}`);
        }
        if (isHost) { ++checked; seen.add(file); }
        if (isVendor) { ++vendorChecked; vendorSeen.add(file); vendorRelativeSeen.add(file.slice(vendorPrefix.length + 1)); }
        if (isConsumer) { ++consumerChecked; consumerSeen.add(file); }
        if (fuzzerTarget) {
            ++fuzzerChecked;
            if (file === canonical(api.join(root, fuzzerDrivers.get(fuzzerTarget)))) fuzzerSeen.add(fuzzerTarget);
        }
    }
    for (const name of mandatory) assert.ok(seen.has(canonical(api.join(root, name))), `required compile input: ${name}`);
    if (vendor) for (const name of ['src/wasm.c', 'deps/quickjs/quickjs.c', 'deps/ada/ada.cpp',
        'deps/libuv/src/uv-common.c', 'deps/sqlite3/sqlite3.c', 'deps/miniz/miniz.c',
        'deps/mbedtls/library/sha256.c', 'deps/mbedtls/library/x509.c', 'deps/mbedtls/library/ssl_tls.c',
        'deps/libwebsockets/lib/core/context.c', 'deps/wamr/core/iwasm/interpreter/wasm_runtime.c']) {
        assert.ok(vendorRelativeSeen.has(name), `required vendor input: ${name}`);
    }
    for (const file of consumers) assert.ok(consumerSeen.has(file), `required runtime consumer: ${file}`);
    for (const file of uninstrumented) assert.ok(uninstrumentedSeen.has(file), `required uninstrumented peer: ${file}`);
    if (options.fuzzers) for (const name of fuzzerDrivers.keys()) assert.ok(fuzzerSeen.has(name), `required fuzzer driver: ${name}`);
    return {complete: true, scope: 'Every matching C/C++ compile-database entry; assembly and external prebuilt libraries are outside this metadata audit. No runtime-execution claim', expected, hostCompileEntries: checked, hostSources: seen.size,
        vendorCompileEntries: vendorChecked, vendorSources: vendorSeen.size, vendorAssemblyCompileEntries: assemblyChecked,
        vendorAsanCompileEntries: expected.includes('address') ? vendorChecked : 0, vendorAsanSources: expected.includes('address') ? vendorSeen.size : 0,
        runtimeConsumerCompileEntries: consumerChecked, uninstrumentedCompileEntries: uninstrumentedChecked,
        fuzzerCompileEntries: fuzzerChecked, fuzzerTargets: fuzzerSeen.size};
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
    const args = new Map();
    for (let i = 2; i < process.argv.length; i += 2) {
        assert.ok(process.argv[i].startsWith('--') && process.argv[i + 1] !== undefined && !args.has(process.argv[i]), 'unique complete argument pair');
        args.set(process.argv[i], process.argv[i + 1]);
    }
    const sourceRoot = args.get('--source-root');
    assert.ok(sourceRoot && args.get('--compile-commands'), 'source root and compile database');
    const enabled = value => /^(1|on|true|yes)$/i.test(value ?? '');
    const hostSources = fs.readdirSync(path.join(sourceRoot, 'src/host')).filter(name => name.endsWith('.cc')).map(name => 'src/host/' + name);
    console.log(JSON.stringify(auditHostSanitizers(JSON.parse(fs.readFileSync(args.get('--compile-commands'), 'utf8')), {
        sourceRoot, hostSources, vendorRoot: args.get('--vendor-root'), fuzzers: enabled(args.get('--fuzzers')),
        runtimeConsumers: [
            ...(enabled(args.get('--worker-writer-fixture'))
                ? ['src/worker_runtime.cc', 'src/worker_main.cc', 'tests/worker_writer_fixture.cc'] : []),
            ...(enabled(args.get('--worker-timeout-writer-fixture'))
                ? ['src/worker_runtime.cc', 'src/worker_main.cc', 'tests/worker_timeout_writer_fixture.cc'] : []),
        ],
        uninstrumentedSources: args.get('--vendor-root') && !/^[a-z]:[\\/]|^\\\\/i.test(sourceRoot)
            ? ['tests/fake_worker_peer.cc'] : [],
        expected: [['--asan', 'address'], ['--ubsan', 'undefined'], ['--tsan', 'thread']].filter(([key]) => enabled(args.get(key))).map(([, name]) => name),
    })));
}
