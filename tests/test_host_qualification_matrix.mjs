// Required registrations for the recent lifecycle/encoding/metrics repairs.
// Reading and checking CTest registration does not assert a product test pass.
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import {readCtestInventory} from './test_websocket_platform_matrix.mjs';

const p = value => ({path: value});
export function qualificationRequirements(options, manifest) {
    const {system, build, source, worker, host, node, buildNode, ctest, wptRoot = '', tsan = '0'} = options;
    assert.ok(['Linux', 'Darwin', 'Windows'].includes(system), 'known qualification platform');
    assert.ok(['0', '1'].includes(tsan), 'normalized TSan registration flag');
    const api = system === 'Windows' ? path.win32 : path.posix;
    const binaryRoot = api.dirname(worker);
    const binary = name => p(api.join(binaryRoot, name + (system === 'Windows' ? '.exe' : '')));
    const sourceFile = name => p(api.join(source, name));
    const generated = name => p(api.join(build, 'generated', name));
    const specs = [];
    const add = (name, command, timeout, fixture, controlShard) => specs.push({name, command, timeout, fixture, ...(controlShard?{controlShard}:{})});
    add('host_parent_stop_monitor', [binary('test-host-parent-stop-monitor')], 10);
    add('spawn_failure_recovery', [binary('test-spawn-failure-recovery'), p(worker)], 60);
    add('worker_watchdog_active_deadline', [binary('test-worker-watchdog-observation'), binary('test-stubborn-worker'), 'active'], 10);
    add('worker_watchdog_late_first_observation', [binary('test-worker-watchdog-observation'), binary('test-stubborn-worker'), 'late'], 10);
    add('host_cli_unknown_arguments', [p(node), sourceFile('tests/test_host_cli_unknown_arguments.mjs'),
        '--host', p(host), '--worker', p(worker)], 60);
    if (['Linux', 'Windows'].includes(system)) {
        add('host_generation_pool_contract', [binary('test-host-generation-pool'), p(worker)], 90);
    }
    if (tsan === '0') {
        add('abi_guard_oom_countdown', [binary('test-abi-guard'), p(worker)], 60);
        add('abi_guard_without_worker', [binary('test-abi-guard')], 60);
        for (const mode of ['egress-empty', 'egress-rules', 'capability-empty', 'permission-rule',
            'capability-snapshot', 'v4-carriage', 'v4-channels', 'snapshot-v4-channels']) {
            add('abi_guard_oom_' + mode.replaceAll('-', '_'),
                [binary('test-abi-guard'), p(worker), mode], 60);
        }
    }
    for (const [name, flag] of [['host_generation_quarantine_callback_lock', '--quarantine-lock'],
        ['host_generation_retired_exit_barrier', '--retired-exit']]) {
        add(name, [binary('test-host-generation-pool'), p(worker), flag], 40);
    }
    for (const mode of ['local', 'local-no-external', 'external', 'destructor']) {
        add('host_worker_supervisor_' + mode.replaceAll('-', '_'),
            [binary('test-host-worker-supervisor'), mode], 10);
    }
    for (const mode of ['pool-lease', 'executor-lease', 'stale-pool', 'stop-local', 'stop-external', 'active-probe', 'exit-notifier']) {
        add('host_worker_supervisor_' + mode.replaceAll('-', '_'),
            [binary('test-host-worker-supervisor-ownership'), p(worker), mode], 40);
    }
    add('worker_outbound_websocket', [binary('test-worker-outbound'), p(worker), binary('test-worker-writer-fixture')], 30);
    add('worker_outbound_result_ack_pressure', [binary('test-worker-outbound'), binary('test-worker-writer-fixture'), 'ack-pressure'], 30);
    add('worker_response_timeout_queue_pressure', [binary('test-response-queue-saturation'),
        binary('test-worker-timeout-writer-fixture'), generated('test-queue-saturation.js'), '18'], 30);
    for (const mode of ['aborted-warm', 'commit-reap-race', 'lifecycle', 'reaped-before-activation']) {
        add('host_worker_capacity_lease_' + mode.replaceAll('-', '_'),
            [binary('test-host-worker-capacity-lease'), mode], 20);
    }
    for (const mode of ['ledger_overflow_limits', 'ledger_retired_headroom']) {
        add('host_worker_capacity_ledger_' + mode, [binary('test-host-worker-capacity-ledger'), mode], 10);
    }
    if (system === 'Linux') {
        for (const suffix of ['deploy_overlaps_quarantine', 'quarantine_releases_capacity']) {
            const name = 'host_managed_executable_' + suffix;
            add(name, [binary('test-host-managed-executable'), name, p(host), p(worker)], 40);
        }
        for (const suffix of ['misconfigured_activation_capacity', 'prepare_failure_capacity',
            'quarantine_aborted_generation', 'quarantine_expired_notifications', 'quarantine_pending_shutdown',
            'quarantine_persist_failure', 'quarantine_prepared_generation_commits', 'quarantine_retire_stale',
            'quarantine_same_generation_reactivation', 'quarantine_stale_generation', 'quarantine_stale_handles']) {
            const name = 'host_managed_' + suffix;
            add(name, [binary('test-host-managed'), name, p(worker), binary('capsid-bytecode-compile'),
                sourceFile('tests/fixtures/ipc-sync-response.js')], 120);
        }
    }
    for (const mode of system === 'Linux' ? ['single-worker', 'static-pool', 'managed'] : ['single-worker', 'static-pool']) {
        add('gb18030_compatibility_' + mode,
            [p(node), sourceFile('tests/test_gb18030_compatibility.mjs'), p(host), p(worker), mode], 60);
    }
    for (const [mode, timeout] of [['retirement', 40], ['concurrency', 40], ['bank', 15]]) {
        add('host_ipc_metrics_' + mode, [binary('test-host-worker-executor'), p(worker), '--metrics-' + mode], timeout);
    }
    add('host_ipc_metrics_format', [binary('test-host-ipc-metrics-format')], 15);
    add('host_websocket_session_lifecycle', [binary('test-host-websocket-session')], 15);
    add('worker_binding_ready_zero', [binary('test-worker-binding-ready'), p(worker), 'zero'], 40);
    if (system === 'Windows') {
        add('worker_binding_ready_finite', [binary('test-worker-binding-ready'), p(worker), 'finite'], 40);
    }
    for (const mode of ['default', '262144']) {
        add('worker_js_stack_limits_' + mode, [p(buildNode), sourceFile('tests/test_worker_stack_limits.mjs'),
            '--driver', binary('test-hono-worker-driver'), '--worker', p(worker),
            '--bundle', sourceFile('tests/fixtures/js-stack-limits.js'), '--stack-bytes', mode], 30);
    }
    add('worker_framework_concurrent_diagnostics', [p(buildNode),
        sourceFile('tests/test_framework_concurrent_diagnostics.mjs'),
        '--driver', binary('test-hono-worker-driver'), '--worker', p(worker),
        '--bundle', sourceFile('tests/fixtures/framework-concurrent-failure.js')], 30);
    add('worker_quickjs_call_regexp', [p(buildNode), sourceFile('tests/test_quickjs_call_regexp.mjs'),
        '--driver', binary('test-hono-worker-driver'), '--worker', p(worker),
        '--bundle', sourceFile('tests/fixtures/quickjs-call-regexp.mjs')], 45);
    add('quickjs_call_dispatch', [binary('test-quickjs-call-dispatch')], 10);
    add('quickjs_iterator_storage', [binary('test-quickjs-iterator-storage')], 20);
    for (const mode of system === 'Linux' ? ['single-worker', 'static-pool', 'managed'] : ['single-worker', 'static-pool']) {
        add('websocket_throw_wire_' + mode, [p(node), sourceFile('tests/test_websocket_throw_wire.mjs'),
            '--host', p(host), '--worker', p(worker), '--mode', mode], 45);
    }
    add('workerd_throw_contract_controls', [p(node), '--test', sourceFile('tests/test_workerd_throw_contract.mjs')], 10);
    add('workerd_handler_error_contract_controls', [p(node), '--test', sourceFile('tests/test_workerd_handler_error_contract.mjs')], 10);
    if (system === 'Linux' || system === 'Windows') {
        add('workerd_ws_gate_controls', [p(node), '--test', sourceFile('tests/test_workerd_reference_gate.mjs')], 120, undefined, 'primary');
        add('workerd_ws_gate_archive_controls', [p(node), '--test', sourceFile('tests/test_workerd_reference_gate.mjs')], 120, undefined, 'archives');
        add('workerd_ws_gate_extension_controls', [p(node), '--test', sourceFile('tests/test_workerd_reference_gate.mjs')], 120, undefined, 'extensions');
        add('workerd_ws_gate_support_controls', [p(node), '--test',
            ...['test_workerd_outbound_contract.mjs',
                'test_workerd_response_contract.mjs', 'test_websocket_interop_contract.mjs',
                'test_workerd_fanout_contract.mjs', 'test_workerd_frame_contract.mjs']
                .map(name => sourceFile('tests/' + name))], 120);
    }
    add('host_sanitizer_audit_controls', [p(node), '--test', sourceFile('tests/test_host_sanitizers.mjs')], 30);
    for (const transport of ['ordinary', 'upgrade']) {
        add('native_fetch_default_reader_gates_' + transport,
            [p(node), sourceFile('tests/test_native_fetch_default_reader_gates.mjs'),
                '--host', p(host), '--worker', p(worker), '--transport', transport], 120);
    }
    add('native_fetch_default_reader_gates_contract_controls', [p(node), '--test', '--test-reporter=tap',
        sourceFile('tests/test_native_fetch_default_reader_gates_contract.mjs')], 30);
    for (const encoded of [false, true]) {
        add(encoded ? 'fetch_response_limits_encoded_upgrade_abi' : 'fetch_response_limits_native_abi',
            [p(node), sourceFile('tests/test_fetch_response_limits.mjs'), binary('test-fetch-response-limits'),
                p(worker), ...(encoded ? ['--encoded-headers'] : [])], 60);
    }
    add('fetch_response_limit_contract_controls', [p(node), '--test',
        sourceFile('tests/test_fetch_response_limit_contract.mjs')], 30);
    for (const group of ['held_raw', 'held_gzip', 'checkpoints', 'cancel',
        'controls_raw', 'controls_gzip', 'priority_raw', 'priority_gzip']) {
        add('native_fetch_branch_graph_' + group, [p(node), sourceFile('tests/test_native_branch_graph.mjs'),
            '--helper', binary('test-native-reader-context'), '--worker', p(worker), '--group', group], 60);
    }
    add('native_branch_graph_contract_controls', [p(node), '--test', '--test-reporter=tap',
        sourceFile('tests/test_native_branch_graph_contract.mjs')], 30);
    for (const group of ['promise_direct', 'promise_await', 'promise_await-then',
        'promise_await-prelude', 'promise_await-nested', 'promise_new-promise-adopt',
        'promise_resolve-adopt', 'promise_all-one', 'promise_all-resolved', 'promise_race-one',
        'promise_any-one', 'promise_all-settled', 'promise_finally', 'promise_catch',
        'promise_then-adopt', 'promise_async-return', 'branches', 'iterator_next',
        'iterator_prevent', 'iterator_loop', 'iterator_prelude', 'controls']) {
        add('native_retired_ownership_' + group, [p(node), sourceFile('tests/test_native_retired_ownership.mjs'),
            '--helper', binary('test-native-retired-ownership'), '--worker', p(worker), '--group', group], 60);
    }
    add('native_retired_ownership_contract_controls', [p(node), '--test', '--test-reporter=tap',
        sourceFile('tests/test_native_retired_ownership_contract.mjs')], 30);
    for (const shard of ['released', 'retained']) {
        add('native_reader_context_' + shard, [p(node), sourceFile('tests/test_native_reader_context.mjs'),
            '--helper', binary('test-native-reader-context'), '--worker', p(worker), '--shard', shard], 60);
    }
    add('native_reader_context_contract_controls', [p(node), '--test',
        sourceFile('tests/test_native_reader_context_contract.mjs')], 30);
    for (const body of ['raw', 'gzip']) for (const owner of ['none', 'pending']) {
        for (const route of ['getReader', 'direct', 'prototype', 'subclass', 'reflect']) {
            add('native_reader_context_matrix_' + [body, owner, route].join('_'),
                [p(node), sourceFile('tests/test_native_reader_context_matrix.mjs'),
                    '--helper', binary('test-native-reader-context'), '--worker', p(worker),
                    '--body-kind', body, '--owner-mode', owner, '--route', route], 60);
        }
    }
    add('native_reader_context_matrix_contract_controls', [p(node), '--test', '--test-reporter=tap',
        sourceFile('tests/test_native_reader_context_matrix_contract.mjs')], 30);
    if (wptRoot) {
        assert.equal(manifest.commit, '1985b47aa8972a970f005957f2bfa036da1787c6', 'reviewed WPT revision');
        assert.equal(manifest.executedProfile.length, 84, 'reviewed WPT corpus length');
        assert.equal(new Set(manifest.executedProfile).size, 84, 'unique WPT source paths');
        add('wpt_source_integrity', [p(buildNode), sourceFile('tests/wpt/audit-source.mjs'), p(wptRoot),
            manifest.commit, generated('wpt-input-paths.txt')], 60, 'setup');
        for (const [name, script, output] of [
            ['gb18030_ranges_generated', 'generate-gb18030-ranges.mjs', 'gb18030-ranges.js'],
            ['multibyte_indexes_generated', 'generate-multibyte-indexes.mjs', 'multibyte-indexes.js'],
        ]) {
            add(name, [p(buildNode), sourceFile('tools/' + script), p(wptRoot),
                sourceFile('js/encoding-data/' + output), '--check'], 30, 'required');
        }
        for (const file of manifest.executedProfile) {
            const id = file.replace(/[^A-Za-z0-9]/g, '_');
            add('worker_wpt_' + id, [binary('test-worker-integration'), p(worker),
                generated('test-wpt-' + id + '.js'), 'wpt', 'https://wpt.local/' + file], 20, 'required');
        }
    }
    const selfArguments = [p(node), sourceFile('tests/test_host_qualification_matrix.mjs'), '--ctest', p(ctest),
        '--build', p(build), '--source', p(source), '--system', system, '--host', p(host), '--worker', p(worker),
        '--node', p(node), '--build-node', p(buildNode), '--tsan', tsan];
    if (wptRoot) selfArguments.push('--wpt-root', p(wptRoot));
    add('host_qualification_matrix', selfArguments, 60);
    add('host_qualification_matrix_controls', [p(node), '--test', sourceFile('tests/test_host_qualification_contract.mjs')], 30);
    return specs;
}

export function auditQualificationMatrix(inventory, options, manifest) {
    assert.equal(inventory.kind, 'ctestInfo', 'actual CTest inventory kind');
    assert.ok(Array.isArray(inventory.tests) && inventory.tests.length, 'nonempty actual inventory');
    const normalize = value => {
        assert.equal(typeof value, 'string', 'path argument type');
        return options.system === 'Windows' ? path.win32.normalize(value).toLowerCase() : path.posix.normalize(value);
    };
    const byName = new Map();
    for (const test of inventory.tests) {
        assert.equal(typeof test.name, 'string', 'CTest name');
        assert.ok(!byName.has(test.name), 'unique CTest name: ' + test.name);
        byName.set(test.name, test);
    }
    const required = qualificationRequirements(options, manifest);
    for (const spec of required) {
        const test = byName.get(spec.name), label = `${options.system}: ${spec.name}`;
        assert.ok(test, label + ' registered');
        assert.ok(Array.isArray(test.command), label + ' has an executable command');
        assert.equal(test.command.length, spec.command.length, label + ' argument count');
        spec.command.forEach((expected, index) => {
            assert.equal(typeof expected === 'string' ? test.command[index] : normalize(test.command[index]),
                typeof expected === 'string' ? expected : normalize(expected.path), label + ' actual argument ' + index);
        });
        const props = new Map();
        for (const {name, value} of test.properties ?? []) {
            assert.ok(!props.has(name), label + ' unique property ' + name);
            props.set(name, value);
        }
        assert.ok(!props.get('DISABLED') && !props.get('WILL_FAIL'), label + ' enabled with real exit status');
        for (const key of ['SKIP_RETURN_CODE', 'SKIP_REGULAR_EXPRESSION', 'PASS_REGULAR_EXPRESSION']) {
            assert.ok(!props.has(key), label + ' no status override ' + key);
        }
        assert.ok(Number.isFinite(props.get('TIMEOUT')) && props.get('TIMEOUT') > 0 && props.get('TIMEOUT') <= spec.timeout,
            label + ' bounded reviewed timeout');
        if(spec.controlShard){
            const environment=props.get('ENVIRONMENT');assert.ok(Array.isArray(environment),label+' control shard environment');
            assert.deepEqual(environment.filter(value=>value.startsWith('CAPSID_WS_GATE_CONTROL_SHARD=')),
                ['CAPSID_WS_GATE_CONTROL_SHARD='+spec.controlShard],label+' exact control group');
        }
        if (spec.fixture) assert.deepEqual(props.get(spec.fixture === 'setup' ? 'FIXTURES_SETUP' : 'FIXTURES_REQUIRED'),
            ['capsid_wpt_source'], label + ' source integrity fixture');
    }
    const workerWpt = inventory.tests.filter(test => test.name.startsWith('worker_wpt_'));
    assert.equal(workerWpt.length, options.wptRoot ? 84 : 0, 'exact configured WPT inventory');
    return {complete: true, scope: 'Required CTest registration/commands/properties only, no product execution claim',
        system: options.system, wptConfigured: Boolean(options.wptRoot), required: required.map(spec => spec.name)};
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
    const args = new Map();
    for (let i = 2; i < process.argv.length; i += 2) {
        assert.ok(process.argv[i].startsWith('--') && process.argv[i + 1] && !args.has(process.argv[i]), 'unique complete argument pair');
        args.set(process.argv[i], process.argv[i + 1]);
    }
    const options = {};
    for (const [key, name] of [['ctest', 'ctest'], ['build', 'build'], ['source', 'source'], ['system', 'system'],
        ['host', 'host'], ['worker', 'worker'], ['node', 'node'], ['build-node', 'buildNode']]) {
        assert.ok(args.has('--' + key), 'required --' + key);
        options[name] = args.get('--' + key);
    }
    options.wptRoot = args.get('--wpt-root') ?? '';
    options.tsan = args.get('--tsan') ?? '0';
    const manifest = JSON.parse(fs.readFileSync(path.join(options.source, 'tests/wpt/manifest.json'), 'utf8'));
    console.log(JSON.stringify(auditQualificationMatrix(readCtestInventory(options.ctest, options.build), options, manifest)));
}
