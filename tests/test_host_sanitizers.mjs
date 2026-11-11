import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import {spawnSync} from 'node:child_process';
import {fileURLToPath} from 'node:url';
import {test} from 'node:test';
import {auditHostSanitizers, compileArguments} from './audit-host-sanitizers.mjs';

const mandatory = ['src/host/config.cc', 'src/host/metrics.cc', 'src/host/worker_executor.cc', 'vendor/jansson/src/load.c'];
const hostSources = [...mandatory.slice(0, 3), 'src/host/added_later.cc'];
const fixture = (windows, expected) => {
    const api = windows ? path.win32 : path.posix;
    const sourceRoot = windows ? 'C:\\source tree' : '/source tree';
    const flags = windows ? ['/fsanitize=address'] : ['-fsanitize=' + expected.join(','),
        '-fno-omit-frame-pointer', '-fno-sanitize-recover=all'];
    const entries = [...hostSources, mandatory[3]].map(name => ({directory: sourceRoot,
        file: api.join(sourceRoot, name), arguments: [windows ? 'cl.exe' : 'c++', ...flags, '-c', api.join(sourceRoot, name)]}));
    return {entries, options: {sourceRoot, hostSources, expected}};
};

test('native command quoting keeps flag-like macro contents inside definitions', () => {
    assert.deepEqual(compileArguments('"C:\\Program Files\\cl.exe" /DNOTE="text /fsanitize=address text" /c "C:\\source tree\\a.cc"', true),
        ['C:\\Program Files\\cl.exe', '/DNOTE=text /fsanitize=address text', '/c', 'C:\\source tree\\a.cc']);
    assert.deepEqual(compileArguments('c++ -DNOTE=\'text -fsanitize=address text\' -c "/source tree/a.cc"', false),
        ['c++', '-DNOTE=text -fsanitize=address text', '-c', '/source tree/a.cc']);
    assert.deepEqual(compileArguments('cl /DNOTE=\\"quoted\\" /c a.cc', true), ['cl', '/DNOTE="quoted"', '/c', 'a.cc']);
    assert.throws(() => compileArguments('cl "unfinished', true), /closed/);
    assert.throws(() => compileArguments('c++ \'unfinished', false), /closed/);
});

for (const [windows, expected] of [[true, ['address']], [false, ['address']], [false, ['undefined']], [false, ['thread']], [false, ['address', 'undefined']]]) {
    test(`${windows ? 'MSVC' : 'GNU/Clang'} ${expected}: every copy and newly discovered Host source`, () => {
        const {entries, options} = fixture(windows, expected);
        const run = value => auditHostSanitizers(value, options);
        assert.equal(run(entries).hostCompileEntries, 5);
        const mutations = [
            value => value[0].arguments.splice(1, 1),
            value => value.push({...structuredClone(value[0]), arguments: ['c++', '-c', value[0].file]}),
            value => value[3].arguments.splice(1, 1),
            value => value[0].arguments[1] = windows ? '/DNOTE=/fsanitize=address' : '-DNOTE=-fsanitize=' + expected.join(','),
            value => value[0].arguments.push('-fno-sanitize=all'),
            value => value[0].file += '.substitute',
            value => value.splice(0, 1),
            value => value[0].arguments = [],
        ];
        for (const mutate of mutations) {
            const changed = structuredClone(entries); mutate(changed);
            assert.throws(() => run(changed));
        }
        if (!windows) for (const mutate of [
            value => value[0].arguments.push('-fno-sanitize=alignment'),
            value => value[0].arguments.push('-fsanitize=' + (expected.includes('thread') ? 'address' : 'thread')),
            value => value[0].arguments.splice(2, 1),
            value => value[0].arguments.push('-fomit-frame-pointer'),
            value => value[0].arguments.splice(3, 1),
            value => value[0].arguments.push('-fsanitize-recover=all'),
            value => value[0].arguments.push('-fsanitize-recover=' + expected[0]),
        ]) {
            const changed = structuredClone(entries); mutate(changed);
            assert.throws(() => run(changed));
        }
        if (expected.includes('address')) for (const kind of ['STRING', 'VECTOR']) {
            const changed = structuredClone(entries);
            changed[0].arguments.push('/D_DISABLE_' + kind + '_ANNOTATION');
            assert.throws(() => run(changed), /annotations retained/);
        }
        const commands = entries.map(entry => ({...entry, arguments: undefined,
            command: entry.arguments.map(arg => '"' + arg + '"').join(' ')}));
        assert.equal(run(commands).hostCompileEntries, 5);
        const deceptive = structuredClone(commands);
        deceptive[0].command = windows ? 'cl /DNOTE="text /fsanitize=address text" /c "' + entries[0].file + '"'
            : 'c++ -DNOTE="text -fsanitize=' + expected.join(',') + ' text" -c "' + entries[0].file + '"';
        assert.throws(() => run(deceptive), /does not instrument/);
    });
}

for (const [windows, expected] of [[true, ['address']], [false, ['address']], [false, ['undefined']], [false, ['thread']], [false, ['address', 'undefined']]]) {
    test(`${windows ? 'MSVC' : 'GNU/Clang'} ${expected}: complete dependency families, original tree and every copy`, () => {
        const {entries, options} = fixture(windows, expected);
        const api = windows ? path.win32 : path.posix;
        options.vendorRoot = windows ? 'C:\\build tree\\vendor-overlay\\txiki.js' : '/build tree/vendor-overlay/txiki.js';
        const vendorFiles = ['src/wasm.c', 'deps/quickjs/quickjs.c', 'deps/ada/ada.cpp',
            'deps/libuv/src/uv-common.c', 'deps/sqlite3/sqlite3.c', 'deps/miniz/miniz.c',
            'deps/mbedtls/library/sha256.c', 'deps/mbedtls/library/x509.c', 'deps/mbedtls/library/ssl_tls.c',
            'deps/libwebsockets/lib/core/context.c', 'deps/wamr/core/iwasm/interpreter/wasm_runtime.c'];
        const flags = entries[0].arguments.slice(1, -2);
        const append = (root, name) => {
            const file = api.join(root, name);
            entries.push({directory: root, file, arguments: ['compiler-must-never-run', ...flags, '-c', file]});
        };
        const first = entries.length;
        for (const name of vendorFiles) append(name.startsWith('deps/libuv/')
            ? api.join(options.sourceRoot, 'vendor/txiki.js') : options.vendorRoot, name);
        append(options.vendorRoot, 'deps/libuv/src/unix/core.c');
        append(options.vendorRoot, 'src/wasm.c');
        const consumerStart = entries.length;
        const consumerFiles = ['tests/test_tjs_shared_loop.cc', 'tests/test_binding_rpc.cc',
            'tests/test_quickjs_job_context.cc', 'tests/test_quickjs_async_gc.cc', 'src/binding_rpc.cc'];
        for (const name of consumerFiles) append(options.sourceRoot, name);
        const result = auditHostSanitizers(entries, options);
        assert.equal(result.vendorCompileEntries, 13);
        assert.equal(result.vendorSources, 12);
        assert.equal(result.vendorAsanCompileEntries, expected.includes('address') ? 13 : 0);
        assert.equal(result.runtimeConsumerCompileEntries, 5);
        for (let index = first; index < entries.length; ++index) {
            const changed = structuredClone(entries); changed[index].arguments.splice(1, 1);
            assert.throws(() => auditHostSanitizers(changed, options), /does not instrument/);
        }
        for (const name of vendorFiles) {
            const changed = entries.filter(entry => !entry.file.replaceAll('\\', '/').endsWith('/' + name));
            assert.throws(() => auditHostSanitizers(changed, options), /required vendor/);
        }
        assert.throws(() => auditHostSanitizers(entries.slice(0, consumerStart), options), /required runtime consumer/);
        // Assembly is reported separately: a C/C++ flag audit cannot prove its instrumentation.
        append(options.vendorRoot, 'deps/asm/standalone.S');
        entries.at(-1).arguments = ['assembler-must-never-run', '-c', entries.at(-1).file];
        assert.equal(auditHostSanitizers(entries, options).vendorAssemblyCompileEntries, 1);
        // Revalidate the same accepted array after mutation; no identity-based result cache.
        entries[first].arguments.splice(1, 1);
        assert.throws(() => auditHostSanitizers(entries, options), /does not instrument/);
    });
}

test('every Jansson translation unit and duplicate must be instrumented, beyond the mandatory load.c anchor', () => {
    for (const [windows, expected] of [[true, ['address']], [false, ['address']], [false, ['undefined']], [false, ['thread']], [false, ['address', 'undefined']]]) {
        const {entries, options} = fixture(windows, expected);
        const api = windows ? path.win32 : path.posix;
        const file = api.join(options.sourceRoot, 'vendor/jansson/src/value.c');
        const entry = {directory: options.sourceRoot, file,
            arguments: ['compiler-must-never-run', ...entries[0].arguments.slice(1, -2), '-c', file]};
        entries.push(entry);
        assert.equal(auditHostSanitizers(entries, options).hostCompileEntries, 6);
        const missing = structuredClone(entries); missing.at(-1).arguments.splice(1, 1);
        assert.throws(() => auditHostSanitizers(missing, options), /does not instrument/);
        entries.push({...entry, arguments: ['compiler-must-never-run', '-c', file]});
        assert.throws(() => auditHostSanitizers(entries, options), /does not instrument/);
    }
});

test('dependency scope preserves the deliberately uninstrumented peer for UBSan and TSan', () => {
    for (const expected of [['undefined'], ['thread']]) {
        const {entries, options} = fixture(false, expected);
        options.uninstrumentedSources = ['tests/fake_worker_peer.cc'];
        const file = options.sourceRoot + '/tests/fake_worker_peer.cc';
        entries.push({directory: options.sourceRoot, file, arguments: ['cc', '-c', file]});
        assert.equal(auditHostSanitizers(entries, options).uninstrumentedCompileEntries, 1);
        assert.throws(() => auditHostSanitizers(entries.slice(0, -1), options), /required uninstrumented peer/);
        entries.at(-1).arguments.splice(1, 0, '-fsanitize=' + expected[0]);
        assert.throws(() => auditHostSanitizers(entries, options), /flags leaked/);
    }
});

for (const [windows, expected] of [[true, ['address']], [false, ['thread']]]) {
    test(`writer fixture requires ${expected} on shared Worker sources and fixture main, including every copy`, () => {
        const {entries, options} = fixture(windows, expected);
        const api = windows ? path.win32 : path.posix;
        options.runtimeConsumers = ['src/worker_runtime.cc', 'src/worker_main.cc',
            'tests/worker_writer_fixture.cc', 'tests/worker_timeout_writer_fixture.cc'];
        const first = entries.length;
        for (const name of options.runtimeConsumers) for (let copy = 0; copy < (name.startsWith('src/') ? 2 : 1); ++copy) {
            const file = api.join(options.sourceRoot,name);
            entries.push({directory: options.sourceRoot, file,
                arguments: [windows ? 'cl' : 'c++', ...entries[0].arguments.slice(1, -2), '-c',file]});
        }
        assert.equal(auditHostSanitizers(entries,options).runtimeConsumerCompileEntries,6);
        for (let index = first; index < entries.length; ++index) {
            const changed = structuredClone(entries);
            changed[index].arguments.splice(1,1);
            assert.throws(() => auditHostSanitizers(changed,options), /does not instrument/);
        }
        assert.throws(() => auditHostSanitizers(entries.slice(0,-1),options), /required runtime consumer/);
    });
}

test('explicit fuzz targets require all seven drivers and retain strict instrumentation on every ordinary copy', () => {
    const {entries, options} = fixture(false, ['address', 'undefined']);
    options.fuzzers = true;
    const drivers = [
        ['fuzz-protocol-parser', 'tests/fuzz/fuzz_protocol_parser.cc'],
        ['fuzz-response-headers', 'tests/fuzz/fuzz_response_headers.cc'],
        ['fuzz-worker-protocol', 'tests/fuzz/fuzz_worker_protocol.cc'],
        ['fuzz-audit-decode', 'tests/fuzz/fuzz_audit_decode.cc'],
        ['fuzz-channel-registry', 'tests/fuzz/fuzz_channel_registry.cc'],
        ['fuzz-websocket-frames', 'tests/fuzz/fuzz_websocket_frames.cc'],
        ['fuzz-websocket-handshake', 'tests/fuzz/fuzz_websocket_handshake.cc'],
    ];
    for (const [target, relative] of [...drivers, ['fuzz-websocket-handshake', 'src/host/config.cc']]) {
        const file = path.posix.join(options.sourceRoot, relative);
        entries.push({directory: options.sourceRoot, file,
            arguments: ['compiler-must-never-run', '-fsanitize=fuzzer,address,undefined',
                '-fno-omit-frame-pointer', '-fno-sanitize-recover=all', '-o',
                'CMakeFiles/' + target + '.dir/' + relative + '.o', '-c', file]});
    }
    const run = value => auditHostSanitizers(value, options);
    assert.equal(run(entries).fuzzerCompileEntries, 8);
    assert.equal(run(entries).fuzzerTargets, 7);
    const first = 5;
    for (const mode of ['address', 'undefined', 'fuzzer']) {
        const changed = structuredClone(entries);
        changed[first].arguments[1] = '-fsanitize=' + ['fuzzer', 'address', 'undefined'].filter(name => name !== mode).join(',');
        assert.throws(() => run(changed));
    }
    for (let i = 0; i < drivers.length; ++i) {
        const changed = structuredClone(entries); changed.splice(first + i, 1);
        assert.throws(() => run(changed), /required fuzzer driver/);
    }
    for (const mutate of [
        value => value[0].arguments.push('-fsanitize=fuzzer'),
        value => value[0].arguments.splice(1, 1),
        value => value.at(-1).arguments[5] = 'CMakeFiles/unrecognized.dir/src/host/config.cc.o',
        value => value.at(-1).arguments[5] = 'CMakeFiles/fuzz-websocket-handshake.dir/src/host/metrics.cc.o',
        value => value.at(-1).arguments[1] = '-DNOTE=-fsanitize=fuzzer,address,undefined',
        value => value.at(-1).arguments.push('-o', 'second-output.o'),
        value => value.at(-1).output = 'contradictory-output.o',
        value => value.at(-1).arguments.push('-fno-sanitize=undefined'),
        value => value.at(-1).arguments.push('-fsanitize-recover=undefined'),
        value => value.at(-1).arguments.push('-fomit-frame-pointer'),
    ]) {
        const changed = structuredClone(entries); mutate(changed);
        assert.throws(() => run(changed));
    }
    assert.throws(() => auditHostSanitizers(entries, {...options, fuzzers: false}), /unexpected sanitizer mode/);
    // Fuzz targets still require fatal UBSan when the ordinary build selects
    // ASan alone, because the fuzz targets enable both sanitizers themselves.
    const addressOnly = structuredClone(entries);
    for (const entry of addressOnly.slice(0, first)) entry.arguments[1] = '-fsanitize=address';
    const addressOptions = {...options, expected: ['address']};
    assert.equal(auditHostSanitizers(addressOnly, addressOptions).fuzzerTargets, 7);
    addressOnly.at(-1).arguments.push('-fsanitize-recover=undefined');
    assert.throws(() => auditHostSanitizers(addressOnly, addressOptions), /fatal sanitizer recovery/);
});

test('actual Node CLI and CMake wrapper reject defective metadata without executing compile commands', () => {
    const root = fs.mkdtempSync(path.join(os.tmpdir(), 'capsid-sanitizer-controls-'));
    assert.ok(path.isAbsolute(root) && path.basename(root).startsWith('capsid-sanitizer-controls-'));
    const source = fileURLToPath(new URL('..', import.meta.url));
    const database = path.join(root, 'compile_commands.json');
    const flags = process.platform === 'win32' ? ['/fsanitize=address'] : ['-fsanitize=address',
        '-fno-omit-frame-pointer', '-fno-sanitize-recover=all'];
    const entries = mandatory.map(name => ({directory: source, file: path.join(source, name),
        arguments: ['compiler-must-never-run', ...flags, '-c', path.join(source, name)]}));
    const nodeArgs = [fileURLToPath(new URL('./audit-host-sanitizers.mjs', import.meta.url)), '--compile-commands', database,
        '--source-root', source, '--asan', 'ON', '--ubsan', 'OFF', '--tsan', 'OFF'];
    const cmake = process.env.CAPSID_SANITIZER_AUDIT_CMAKE;
    assert.ok(cmake, 'actual CMake supplied by test registration');
    const cmakeArgs = ['-DCAPSID_COMPILE_COMMANDS=' + database, '-DCAPSID_NODE_EXECUTABLE=' + process.execPath,
        '-DCAPSID_EXPECT_ASAN=ON', '-DCAPSID_EXPECT_UBSAN=OFF', '-DCAPSID_EXPECT_TSAN=OFF', '-P',
        path.join(source, 'cmake/TestHostSanitizerInstrumentation.cmake')];
    try {
        const cases = [
            ['valid', () => {}, true],
            ['one uninstrumented copy', value => value.push({...structuredClone(value[0]), arguments: ['never-run', '-c', value[0].file]}), false],
            ['missing required source', value => value.splice(1, 1), false],
            ['flag inside macro', value => value[0].arguments[1] = '-DNOTE=-fsanitize=address', false],
        ];
        if (process.platform !== 'win32') cases.push(
            ['frame pointer overridden', value => value[0].arguments.push('-fomit-frame-pointer'), false],
            ['recovering sanitizer', value => value[0].arguments.push('-fsanitize-recover=address'), false],
        );
        for (const [label, mutate, valid] of cases) {
            const value = structuredClone(entries); mutate(value);
            fs.writeFileSync(database, JSON.stringify(value));
            for (const [executable, args] of [[process.execPath, nodeArgs], [cmake, cmakeArgs]]) {
                const result = spawnSync(executable, args, {encoding: 'utf8', timeout: 10000, windowsHide: true});
                assert.equal(result.error, undefined, label);
                assert.equal(result.signal, null, label);
                assert.equal(result.status === 0, valid, label + ': ' + result.stdout + result.stderr);
            }
        }
        console.log(`Verifier-only controls: ${cases.length * 2} actual CLI/wrapper invocations; no compiler or product program executed`);
    } finally {
        fs.rmSync(root, {recursive: true, force: true});
    }
});
