import assert from 'node:assert/strict';
import {test} from 'node:test';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import {spawnSync} from 'node:child_process';
import {fileURLToPath} from 'node:url';
import {auditQualificationMatrix, qualificationRequirements} from './test_host_qualification_matrix.mjs';
import {assertControlRegistrationGroups} from '../tools/run-workerd-ws-gate.mjs';

const manifest = JSON.parse(fs.readFileSync(new URL('./wpt/manifest.json', import.meta.url)));
// Reviewed independently of the builder: removing a requirement from the
// auditor must fail this contract as well as the actual inventory gate.
const portable = [
    'abi_guard_oom_countdown', 'abi_guard_without_worker',
    'abi_guard_oom_egress_empty', 'abi_guard_oom_egress_rules',
    'abi_guard_oom_capability_empty', 'abi_guard_oom_permission_rule',
    'abi_guard_oom_capability_snapshot', 'abi_guard_oom_v4_carriage',
    'abi_guard_oom_v4_channels', 'abi_guard_oom_snapshot_v4_channels',
    'host_parent_stop_monitor',
    'spawn_failure_recovery', 'host_cli_unknown_arguments',
    'worker_watchdog_active_deadline', 'worker_watchdog_late_first_observation',
    'host_generation_quarantine_callback_lock', 'host_generation_retired_exit_barrier',
    'host_worker_supervisor_local', 'host_worker_supervisor_local_no_external',
    'host_worker_supervisor_external', 'host_worker_supervisor_destructor',
    'host_worker_supervisor_pool_lease', 'host_worker_supervisor_executor_lease',
    'host_worker_supervisor_stale_pool', 'host_worker_supervisor_stop_local',
    'host_worker_supervisor_stop_external', 'host_worker_supervisor_active_probe',
    'host_worker_supervisor_exit_notifier',
    'worker_outbound_websocket', 'worker_outbound_result_ack_pressure',
    'worker_response_timeout_queue_pressure',
    'host_worker_capacity_lease_aborted_warm', 'host_worker_capacity_lease_commit_reap_race',
    'host_worker_capacity_lease_lifecycle', 'host_worker_capacity_lease_reaped_before_activation',
    'host_worker_capacity_ledger_ledger_overflow_limits', 'host_worker_capacity_ledger_ledger_retired_headroom',
    'gb18030_compatibility_single-worker', 'gb18030_compatibility_static-pool',
    'host_ipc_metrics_retirement', 'host_ipc_metrics_concurrency', 'host_ipc_metrics_bank', 'host_ipc_metrics_format',
    'worker_binding_ready_zero',
    'host_websocket_session_lifecycle',
    'worker_js_stack_limits_default', 'worker_js_stack_limits_262144',
    'worker_framework_concurrent_diagnostics',
    'worker_quickjs_call_regexp', 'quickjs_call_dispatch',
    'quickjs_iterator_storage',
    'websocket_throw_wire_single-worker', 'websocket_throw_wire_static-pool', 'workerd_throw_contract_controls',
    'workerd_handler_error_contract_controls',
    'host_sanitizer_audit_controls',
    'native_fetch_default_reader_gates_ordinary', 'native_fetch_default_reader_gates_upgrade',
    'native_fetch_default_reader_gates_contract_controls',
    'fetch_response_limits_native_abi', 'fetch_response_limits_encoded_upgrade_abi',
    'fetch_response_limit_contract_controls',
    'native_branch_graph_contract_controls',
    'native_retired_ownership_promise_direct',
    'native_retired_ownership_promise_await',
    'native_retired_ownership_promise_await-then',
    'native_retired_ownership_promise_await-prelude',
    'native_retired_ownership_promise_await-nested',
    'native_retired_ownership_promise_new-promise-adopt',
    'native_retired_ownership_promise_resolve-adopt',
    'native_retired_ownership_promise_all-one',
    'native_retired_ownership_promise_all-resolved',
    'native_retired_ownership_promise_race-one',
    'native_retired_ownership_promise_any-one',
    'native_retired_ownership_promise_all-settled',
    'native_retired_ownership_promise_finally',
    'native_retired_ownership_promise_catch',
    'native_retired_ownership_promise_then-adopt',
    'native_retired_ownership_promise_async-return',
    'native_retired_ownership_branches',
    'native_retired_ownership_iterator_next',
    'native_retired_ownership_iterator_prevent',
    'native_retired_ownership_iterator_loop',
    'native_retired_ownership_iterator_prelude',
    'native_retired_ownership_controls',
    'native_retired_ownership_contract_controls',
    'native_fetch_branch_graph_held_raw',
    'native_fetch_branch_graph_held_gzip',
    'native_fetch_branch_graph_checkpoints',
    'native_fetch_branch_graph_cancel',
    'native_fetch_branch_graph_controls_raw',
    'native_fetch_branch_graph_controls_gzip',
    'native_fetch_branch_graph_priority_raw',
    'native_fetch_branch_graph_priority_gzip',
    'native_reader_context_released',
    'native_reader_context_retained',
    'native_reader_context_contract_controls',
    'native_reader_context_matrix_contract_controls',
    'native_reader_context_matrix_raw_none_getReader',
    'native_reader_context_matrix_raw_none_direct',
    'native_reader_context_matrix_raw_none_prototype',
    'native_reader_context_matrix_raw_none_subclass',
    'native_reader_context_matrix_raw_none_reflect',
    'native_reader_context_matrix_raw_pending_getReader',
    'native_reader_context_matrix_raw_pending_direct',
    'native_reader_context_matrix_raw_pending_prototype',
    'native_reader_context_matrix_raw_pending_subclass',
    'native_reader_context_matrix_raw_pending_reflect',
    'native_reader_context_matrix_gzip_none_getReader',
    'native_reader_context_matrix_gzip_none_direct',
    'native_reader_context_matrix_gzip_none_prototype',
    'native_reader_context_matrix_gzip_none_subclass',
    'native_reader_context_matrix_gzip_none_reflect',
    'native_reader_context_matrix_gzip_pending_getReader',
    'native_reader_context_matrix_gzip_pending_direct',
    'native_reader_context_matrix_gzip_pending_prototype',
    'native_reader_context_matrix_gzip_pending_subclass',
    'native_reader_context_matrix_gzip_pending_reflect',
    'host_qualification_matrix', 'host_qualification_matrix_controls',
];
const linuxOnly = [
    'host_managed_executable_deploy_overlaps_quarantine', 'host_managed_executable_quarantine_releases_capacity',
    'host_managed_misconfigured_activation_capacity', 'host_managed_prepare_failure_capacity',
    'host_managed_quarantine_aborted_generation', 'host_managed_quarantine_expired_notifications',
    'host_managed_quarantine_pending_shutdown', 'host_managed_quarantine_persist_failure',
    'host_managed_quarantine_prepared_generation_commits', 'host_managed_quarantine_retire_stale',
    'host_managed_quarantine_same_generation_reactivation', 'host_managed_quarantine_stale_generation',
    'host_managed_quarantine_stale_handles', 'gb18030_compatibility_managed', 'websocket_throw_wire_managed',
];
const wptNames = ['wpt_source_integrity', 'gb18030_ranges_generated', 'multibyte_indexes_generated',
    ...manifest.executedProfile.map(file => 'worker_wpt_' + file.replace(/[^A-Za-z0-9]/g, '_'))];

for (const system of ['Linux', 'Windows', 'Darwin']) for (const wptConfigured of [false, true]) {
    test(`${system} qualification registrations, WPT=${wptConfigured}`, () => {
        const prefix = system === 'Windows' ? 'C:/qualification' : '/qualification';
        const suffix = system === 'Windows' ? '.exe' : '';
        const options = {system, build: prefix + '/build', source: prefix + '/source',
            worker: prefix + '/build/capsid-worker' + suffix, host: prefix + '/build/capsid-host' + suffix,
            node: prefix + '/node' + suffix, buildNode: prefix + '/build-node' + suffix,
            ctest: prefix + '/ctest' + suffix, wptRoot: wptConfigured ? prefix + '/wpt' : ''};
        const specs = qualificationRequirements(options, manifest);
        const expected = [...portable, ...(system === 'Linux' ? linuxOnly : []),
            ...(['Linux', 'Windows'].includes(system) ? ['host_generation_pool_contract','workerd_ws_gate_controls','workerd_ws_gate_archive_controls','workerd_ws_gate_extension_controls','workerd_ws_gate_support_controls'] : []),
            ...(system === 'Windows' ? ['worker_binding_ready_finite'] : []),
            ...(wptConfigured ? wptNames : [])].sort();
        assert.equal(wptNames.length, 87);
        for (const group of ['held_raw', 'held_gzip', 'checkpoints', 'cancel',
            'controls_raw', 'controls_gzip', 'priority_raw', 'priority_gzip']) {
            const spec = specs.find(value => value.name === 'native_fetch_branch_graph_' + group);
            assert.deepEqual(spec.command.slice(-2), ['--group', group], group + ' exact branch graph group');
            assert.equal(spec.timeout, 60, group + ' bounded branch graph group');
        }
        // This independent table checks all three actual selectors. Merely
        // registering the right names while running one default group fails.
        for (const [body, owner, route] of [["raw","none","getReader"],["raw","none","direct"],["raw","none","prototype"],["raw","none","subclass"],["raw","none","reflect"],["raw","pending","getReader"],["raw","pending","direct"],["raw","pending","prototype"],["raw","pending","subclass"],["raw","pending","reflect"],["gzip","none","getReader"],["gzip","none","direct"],["gzip","none","prototype"],["gzip","none","subclass"],["gzip","none","reflect"],["gzip","pending","getReader"],["gzip","pending","direct"],["gzip","pending","prototype"],["gzip","pending","subclass"],["gzip","pending","reflect"]]) {
            const name = 'native_reader_context_matrix_' + [body, owner, route].join('_');
            const spec = specs.find(value => value.name === name);
            assert.deepEqual(spec.command.slice(-6),
                ['--body-kind', body, '--owner-mode', owner, '--route', route], name + ' exact matrix group');
            assert.equal(spec.timeout, 60, name + ' bounded matrix group');
        }
        // Independently enumerate the semantic groups and bounded commands.
        for (const group of ['promise_direct', 'promise_await', 'promise_await-then',
            'promise_await-prelude', 'promise_await-nested', 'promise_new-promise-adopt',
            'promise_resolve-adopt', 'promise_all-one', 'promise_all-resolved', 'promise_race-one',
            'promise_any-one', 'promise_all-settled', 'promise_finally', 'promise_catch',
            'promise_then-adopt', 'promise_async-return', 'branches', 'iterator_next',
            'iterator_prevent', 'iterator_loop', 'iterator_prelude', 'controls']) {
            const spec = specs.find(value => value.name === 'native_retired_ownership_' + group);
            assert.deepEqual(spec.command.slice(-2), ['--group', group], group + ' exact retired group');
            assert.equal(spec.timeout, 60, group + ' unchanged retired budget');
        }
        assert.equal(specs.find(value => value.name === 'quickjs_iterator_storage').timeout, 20);
        assert.equal(specs.find(value => value.name === 'native_retired_ownership_contract_controls').timeout, 30);
        for (const transport of ['ordinary', 'upgrade']) {
            const spec = specs.find(value => value.name === 'native_fetch_default_reader_gates_' + transport);
            assert.deepEqual(spec.command.slice(-2), ['--transport', transport]);
            assert.equal(spec.timeout, 120);
        }
        assert.equal(specs.find(value => value.name === 'native_fetch_default_reader_gates_contract_controls').timeout, 30);
        for (const name of ['fetch_response_limits_native_abi', 'fetch_response_limits_encoded_upgrade_abi']) {
            assert.equal(specs.find(value => value.name === name).timeout, 60);
        }
        assert.equal(specs.find(value => value.name === 'fetch_response_limit_contract_controls').timeout, 30);
        assert.deepEqual(specs.map(spec => spec.name).sort(), expected, 'reviewed required case list');
        // Independent name-to-mode mapping catches a builder that registers
        // every case under the right name but executes the default scenario.
        for (const [name, mode] of [
            ['abi_guard_oom_egress_empty', 'egress-empty'],
            ['abi_guard_oom_egress_rules', 'egress-rules'],
            ['abi_guard_oom_capability_empty', 'capability-empty'],
            ['abi_guard_oom_permission_rule', 'permission-rule'],
            ['abi_guard_oom_capability_snapshot', 'capability-snapshot'],
            ['abi_guard_oom_v4_carriage', 'v4-carriage'],
            ['abi_guard_oom_v4_channels', 'v4-channels'],
            ['abi_guard_oom_snapshot_v4_channels', 'snapshot-v4-channels']]) {
            const spec = specs.find(value => value.name === name);
            assert.deepEqual(spec.command.slice(1), [{path: options.worker}, mode], name + ' actual scenario');
            assert.equal(spec.timeout, 60, name + ' bounded allocation sweep');
        }
        const inventory = {kind: 'ctestInfo', tests: specs.map(spec => ({name: spec.name,
            command: spec.command.map(value => typeof value === 'string' ? value : value.path),
            properties: [{name: 'TIMEOUT', value: spec.timeout}, {name: 'WORKING_DIRECTORY', value: options.build},
                ...(spec.controlShard ? [{name:'ENVIRONMENT',value:['CAPSID_WS_GATE_CONTROL_SHARD='+spec.controlShard]}] : []),
                ...(spec.fixture ? [{name: spec.fixture === 'setup' ? 'FIXTURES_SETUP' : 'FIXTURES_REQUIRED', value: ['capsid_wpt_source']}] : [])],
        }))};
        const audit = value => auditQualificationMatrix(value, options, manifest);
        if(system==='Linux'||system==='Windows')for(const [name,shard] of [['workerd_ws_gate_controls','primary'],['workerd_ws_gate_archive_controls','archives'],['workerd_ws_gate_extension_controls','extensions']]){
            assert.equal(specs.find(spec=>spec.name===name).controlShard,shard,'independent name-to-shard mapping');
            for(const changed of [undefined,['CAPSID_WS_GATE_CONTROL_SHARD=all'],['CAPSID_WS_GATE_CONTROL_SHARD='+(shard==='primary'?'archives':'primary')],
                ['CAPSID_WS_GATE_CONTROL_SHARD='+shard,'CAPSID_WS_GATE_CONTROL_SHARD='+shard]]){
                const mutant=structuredClone(inventory),test=mutant.tests.find(test=>test.name===name);
                test.properties=test.properties.filter(prop=>prop.name!=='ENVIRONMENT');
                if(changed)test.properties.push({name:'ENVIRONMENT',value:changed});
                assert.throws(()=>auditQualificationMatrix(mutant,options,manifest),'missing, duplicate or wrong control group cannot pass');
                assert.throws(()=>assertControlRegistrationGroups(mutant,{source:options.source,node:options.node,system}),
                    'actual gate also rejects a substituted group');
            }
        }
        if(system==='Linux'||system==='Windows'){
            assertControlRegistrationGroups(inventory,{source:options.source,node:options.node,system});
            const missing=structuredClone(inventory);missing.tests.find(test=>test.name==='workerd_ws_gate_support_controls').command.pop();
            assert.throws(()=>assertControlRegistrationGroups(missing,{source:options.source,node:options.node,system}),'all support files remain mandatory');
        }
        const before = structuredClone(inventory);
        assert.equal(audit(inventory).complete, true);
        assert.deepEqual(inventory, before, 'audit never rewrites its evidence');
        let rejected = 0;
        for (let index = 0; index < inventory.tests.length; ++index) {
            const name = inventory.tests[index].name;
            const reject = (label, mutate) => {
                const changed = structuredClone(inventory);
                mutate(changed, changed.tests[index]);
                assert.throws(() => audit(changed), {name: 'AssertionError'}, `${name}: ${label}`);
                ++rejected;
            };
            reject('deleted registration', value => value.tests.splice(index, 1));
            reject('equal count substitution', (_value, entry) => entry.name = 'substituted_case');
            reject('wrong executable', (_value, entry) => entry.command[0] += '.substitute');
            for (let argument = 1; argument < inventory.tests[index].command.length; ++argument) {
                reject('substituted argument ' + argument, (_value, entry) => entry.command[argument] += '.substitute');
            }
            reject('shell wrapper instead of driver', (_value, entry) => entry.command.unshift('sh', '-c'));
            for (const [property, value] of [['DISABLED', true], ['WILL_FAIL', true], ['SKIP_RETURN_CODE', 77],
                ['SKIP_REGULAR_EXPRESSION', '.'], ['PASS_REGULAR_EXPRESSION', '.']]) {
                reject(property, (_value, entry) => entry.properties.push({name: property, value}));
            }
            for (const timeout of [0, inventory.tests[index].properties[0].value + 1]) {
                reject('weakened timeout', (_value, entry) => entry.properties[0].value = timeout);
            }
            if (specs[index].fixture) reject('missing source fixture', (_value, entry) => entry.properties.pop());
        }
        const duplicate = structuredClone(inventory);
        duplicate.tests.push(structuredClone(duplicate.tests[0]));
        assert.throws(() => audit(duplicate), /unique CTest name/);
        const unexpectedWpt = structuredClone(inventory);
        unexpectedWpt.tests.push({name: 'worker_wpt_unreviewed_extra', command: []});
        assert.throws(() => audit(unexpectedWpt), /exact configured WPT inventory/);
        assert.deepEqual(inventory, before);
        console.log(JSON.stringify({system, wptConfigured, required: expected.length, rejectedMutations: rejected + 2,
            scope: 'Verifier-only registration/property controls; no simulated product execution claim'}));
    });
}

for (const system of ['Linux', 'Windows', 'Darwin']) {
    test(system + ' TSan registration preserves the allocator-test exclusion', () => {
        const prefix = system === 'Windows' ? 'C:/qualification' : '/qualification';
        const suffix = system === 'Windows' ? '.exe' : '';
        const options = {system, build: prefix + '/build', source: prefix + '/source',
            worker: prefix + '/build/capsid-worker' + suffix, host: prefix + '/build/capsid-host' + suffix,
            node: prefix + '/node' + suffix, buildNode: prefix + '/build-node' + suffix,
            ctest: prefix + '/ctest' + suffix, tsan:'1'};
        const specs = qualificationRequirements(options, manifest);
        const expected = [...portable.filter(name => !['abi_guard_oom_countdown', 'abi_guard_without_worker',
            'abi_guard_oom_egress_empty', 'abi_guard_oom_egress_rules',
            'abi_guard_oom_capability_empty', 'abi_guard_oom_permission_rule',
            'abi_guard_oom_capability_snapshot', 'abi_guard_oom_v4_carriage',
            'abi_guard_oom_v4_channels', 'abi_guard_oom_snapshot_v4_channels'].includes(name)),
            ...(system === 'Linux' ? linuxOnly : []),
            ...(['Linux', 'Windows'].includes(system) ? ['host_generation_pool_contract','workerd_ws_gate_controls','workerd_ws_gate_archive_controls','workerd_ws_gate_extension_controls','workerd_ws_gate_support_controls'] : []),
            ...(system === 'Windows' ? ['worker_binding_ready_finite'] : [])].sort();
        assert.deepEqual(specs.map(spec => spec.name).sort(), expected, 'reviewed TSan case list');
        const self = specs.find(spec => spec.name === 'host_qualification_matrix');
        assert.equal(self.command[self.command.indexOf('--tsan') + 1], '1');
        assert.throws(() => qualificationRequirements({...options, tsan:'ON'}, manifest), /normalized TSan/);
    });
}

test('actual CLI reads private CTest registrations and rejects deletion/substitution without executing their programs', () => {
    const ctest = process.env.CAPSID_HOST_QUALIFICATION_CTEST ?? 'ctest';
    const source = path.resolve(fileURLToPath(new URL('..', import.meta.url)));
    const root = fs.mkdtempSync(path.join(os.tmpdir(), 'capsid-qualification-control-'));
    assert.ok(path.isAbsolute(root) && path.basename(root).startsWith('capsid-qualification-control-'));
    const system = process.platform === 'win32' ? 'Windows' : process.platform === 'darwin' ? 'Darwin' : 'Linux';
    const suffix = system === 'Windows' ? '.exe' : '';
    const build = path.join(root, 'build');
    const options = {system, build, source, ctest, host: path.join(build, 'capsid-host' + suffix),
        worker: path.join(build, 'capsid-worker' + suffix), node: process.execPath, buildNode: process.execPath,
        wptRoot: path.join(root, 'wpt')};
    fs.mkdirSync(build);
    const specs = qualificationRequirements(options, manifest);
    const quote = value => {
        assert.ok(!value.includes(']==]'));
        return '[==[' + value + ']==]';
    };
    const emit = entries => entries.map(spec => {
        const command = spec.command.map(value => typeof value === 'string' ? value : value.path);
        return `add_test(${quote(spec.name)} ${command.map(quote).join(' ')})\n` +
            `set_tests_properties(${quote(spec.name)} PROPERTIES TIMEOUT ${spec.timeout}` +
            (spec.controlShard ? ` ENVIRONMENT ${quote('CAPSID_WS_GATE_CONTROL_SHARD='+spec.controlShard)}` : '') +
            (spec.fixture ? ` ${spec.fixture === 'setup' ? 'FIXTURES_SETUP' : 'FIXTURES_REQUIRED'} capsid_wpt_source` : '') + ')\n';
    }).join('');
    const registration = path.join(build, 'CTestTestfile.cmake');
    const script = fileURLToPath(new URL('./test_host_qualification_matrix.mjs', import.meta.url));
    const args = [script, '--ctest', ctest, '--build', build, '--source', source, '--system', system,
        '--host', options.host, '--worker', options.worker, '--node', options.node,
        '--build-node', options.buildNode, '--wpt-root', options.wptRoot];
    const run = expected => {
        const result = spawnSync(process.execPath, args, {encoding: 'utf8', timeout: 15000, windowsHide: true});
        assert.equal(result.error, undefined, String(result.error));
        assert.equal(result.status, expected, result.stdout + result.stderr);
        if (expected === 0) assert.equal(JSON.parse(result.stdout).complete, true);
    };
    try {
        for (const spec of specs) {
            const executable = spec.command[0].path;
            if (path.dirname(executable) === build && !fs.existsSync(executable)) {
                // Only CTest --show-only reads these metadata placeholders.
                fs.writeFileSync(executable, 'registration metadata placeholder, never executed\n', {mode: 0o700});
            }
        }
        const valid = emit(specs);
        fs.writeFileSync(registration, valid);
        run(0);
        assert.equal(fs.readFileSync(registration, 'utf8'), valid, 'CLI never rewrites source registration');
        fs.writeFileSync(registration, emit(specs.filter(spec => spec.name !== 'host_ipc_metrics_retirement')));
        run(1);
        const substituted = structuredClone(specs);
        const target = substituted.find(spec => spec.name === 'gb18030_compatibility_static-pool');
        target.command[target.command.length - 1] = 'single-worker';
        fs.writeFileSync(registration, emit(substituted));
        run(1);
        const wrongWorker = structuredClone(specs);
        wrongWorker.find(spec => spec.name === 'host_generation_retired_exit_barrier').command[1].path += '.substitute';
        fs.writeFileSync(registration, emit(wrongWorker));
        run(1);
        fs.writeFileSync(registration, valid);
        run(0);
    } finally {
        fs.rmSync(root, {recursive: true, force: true});
    }
});
