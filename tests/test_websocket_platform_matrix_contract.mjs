import assert from 'node:assert/strict';
import test from 'node:test';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import {spawnSync} from 'node:child_process';
import {fileURLToPath} from 'node:url';
import {validatePlatformMatrix} from './test_websocket_platform_matrix.mjs';

// Synthetic inventories exercise the validator only. They are not build or
// runtime evidence for any of these platforms.
const writeDeadlines = [
    ['host_write_timeout_cancels', 'write-timeout-cancels'],
    ['host_worker_deadline_independent', 'worker-deadline-independent'],
    ['host_fast_response_untouched', 'fast-response-untouched'],
    ['host_write_timeout_disabled', 'write-timeout-disabled'],
    ['host_pool_write_timeout_cancels', 'pool-write-timeout-cancels'],
    ['host_pool_drain_write_deadline', 'pool-drain-write-deadline'],
    ['host_pool_stop_pending_write', 'pool-stop-pending-write'],
];

for(const family of ['websocket_fetch_boundaries','websocket_fetch_transitions'])for(const system of ['Linux','Windows'])for(const mode of ['reference','single-worker','static-pool',...(system==='Linux'?['managed']:[])]){
    const name=family+'_'+mode;
    for(const [label,edit] of [
        ['missing required family',inventory=>{inventory.tests=inventory.tests.filter(t=>t.name!==name);}],
        ['missing certificate tool',inventory=>{const t=inventory.tests.find(t=>t.name===name);t.command.splice(t.command.indexOf('--openssl'),2);}],
        ['duplicate certificate tool',inventory=>{inventory.tests.find(t=>t.name===name).command.push('--openssl','/test/other-openssl');}],
        ['unresolved certificate tool',inventory=>{const t=inventory.tests.find(t=>t.name===name);t.command[t.command.indexOf('--openssl')+1]='CAPSID_TEST_OPENSSL_EXECUTABLE-NOTFOUND';}],
    ])test(system+' '+name+' rejects '+label,()=>{const inventory=sample(system);edit(inventory);assert.throws(()=>validatePlatformMatrix(inventory,system,process.execPath));});
}
function sample(system) {
    const tests = [];
    for(const [name,executable] of [['channel_owned_io','test-channel-io'],...(system==='Windows'?[['channel_owned_io_debug','test-channel-io-debug']]:[])])
        tests.push({name,command:['/test/'+executable],properties:[{name:'TIMEOUT',value:15}]});
    tests.push({name:'host_generation_pool_portable_contract',command:['/test/test-host-generation-pool','/test/capsid-worker','--portable'],properties:[{name:'TIMEOUT',value:40}]});
    if(system==='Linux')tests.push({name:'websocket_managed_local_owner',command:[process.execPath,'test_websocket_managed_local_owner.mjs','--host','/test/capsid-host','--worker','/test/capsid-worker','--mode','managed'],properties:[{name:'TIMEOUT',value:45}]});
    for (const mode of system === 'Linux' ? ['single-worker', 'static-pool', 'managed'] : ['single-worker', 'static-pool']) {
        for (const [family, script, defaults] of [
            ['host_ws_upgrade_gates', 'test_host_ws_upgrade.mjs', true],
            ['workerd_ws_fixtures', 'test_workerd_ws_fixtures.mjs', true],
            ['websocket_outbound', 'test_websocket_outbound.mjs', false],
            ['websocket_frame_boundaries', 'test_websocket_frame_boundaries.mjs', false],
            ['websocket_throw_wire', 'test_websocket_throw_wire.mjs', false],
            ['websocket_owner_lifecycle', 'test_websocket_owner_lifecycle.mjs', false],
            ['websocket_async_callbacks', 'test_websocket_async_callbacks.mjs', false],
            ['websocket_async_budget', 'test_websocket_async_callbacks.mjs', false],
            ['websocket_cross_context', 'test_websocket_cross_context.mjs', false],
            ['websocket_fetch_resources', 'test_websocket_fetch_resources.mjs', false],
            ['websocket_fetch_upgrade', 'test_websocket_fetch_upgrade.mjs', false],
            ['websocket_fetch_upgrade_lifetime', 'test_websocket_fetch_upgrade_lifetime.mjs', false],
            ['websocket_fetch_proxy', 'test_websocket_fetch_proxy.mjs', false],
            ['websocket_fetch_boundaries', 'test_websocket_fetch_boundaries.mjs', false],
            ['websocket_fetch_transitions', 'test_websocket_fetch_transitions.mjs', false],
            ['websocket_crypto', 'test_websocket_crypto.mjs', false],
            ['websocket_budget', 'test_websocket_budget.mjs', false],
            ['host_session_tracking', 'test_host_session_tracking.mjs', false],
            ['websocket_interop', 'test_websocket_interop.mjs', false],
            ['workerd_fanout', 'test_workerd_fanout.mjs', false],
        ]) tests.push({name: defaults && mode === 'single-worker' ? family : `${family}_${mode}`,
            command: [process.execPath, script, '--host', '/test/capsid-host', '--worker', '/test/capsid-worker', '--mode', mode,
                ...(family === 'websocket_async_budget' ? ['--budgets', '1'] : []),
                ...(['websocket_fetch_boundaries','websocket_fetch_transitions'].includes(family) ? ['--openssl', '/test/openssl'] : [])],
            properties: [{name: 'TIMEOUT', value: 120}]});
    }
    if (system === 'Linux' || system === 'Windows') for (const [name, script, valuedFlag] of [
        ['workerd_ws_reference', 'test_workerd_ws_fixtures.mjs', false],
        ['websocket_outbound_reference', 'test_websocket_outbound.mjs', false],
        ['websocket_interop_reference', 'test_websocket_interop.mjs', true],
        ['workerd_fanout_reference', 'test_workerd_fanout.mjs', true],
        ['websocket_frame_boundaries_reference', 'test_websocket_frame_boundaries.mjs', true],
        ['websocket_owner_lifecycle_reference', 'test_websocket_owner_lifecycle.mjs', true],
        ['websocket_async_callbacks_reference', 'test_websocket_async_callbacks.mjs', true],
        ['websocket_cross_context_reference', 'test_websocket_cross_context.mjs', true],
            ['websocket_fetch_resources_reference', 'test_websocket_fetch_resources.mjs', true],
        ['websocket_fetch_upgrade_reference', 'test_websocket_fetch_upgrade.mjs', true],
        ['websocket_fetch_upgrade_lifetime_reference', 'test_websocket_fetch_upgrade_lifetime.mjs', true],
        ['websocket_fetch_proxy_reference', 'test_websocket_fetch_proxy.mjs', true],
        ['websocket_fetch_boundaries_reference', 'test_websocket_fetch_boundaries.mjs', true],
        ['websocket_fetch_transitions_reference', 'test_websocket_fetch_transitions.mjs', true],
        ['websocket_crypto_reference', 'test_websocket_crypto.mjs', true],
    ]) tests.push({name, command: [process.execPath, script, '--reference-from-env', ...(valuedFlag ? ['1'] : []),
        ...(['websocket_fetch_boundaries_reference','websocket_fetch_transitions_reference'].includes(name)?['--openssl','/test/openssl']:[])],
        properties: [{name: 'TIMEOUT', value: 120}]});
    if (system === 'Windows') tests.push({name: 'worker_windows_environment',
        command: ['/test/test-windows-worker-environment.exe', '/test/capsid-worker.exe'],
        properties: [{name: 'TIMEOUT', value: 30}]});
    for (const [name, args] of [['tjs_fetch_abort_pending_handshake', []],
        ['tjs_fetch_context_cancel_pending_handshake', ['--context-cancel']],
        ['tjs_fetch_context_cancel_dns', ['--dns-context-cancel']],
        ['tjs_fetch_policy_async_context', ['--policy-async-context']],
        ['tjs_fetch_policy_binding_context', ['--policy-binding-context']],
        ['tjs_fetch_policy_unowned_context', ['--policy-unowned-context']]])
        tests.push({name, command: ['/test/test-tjs-fetch-abort', ...args], properties: [{name: 'TIMEOUT', value: 15}]});
    for (const [name, args] of [['host_dns_dialer_lifecycle', []], ['host_dns_reserved_invalid', ['--reserved-invalid']]])
        tests.push({name, command: ['/test/test-host-dns-dialer', ...args], properties: [{name: 'TIMEOUT', value: 30}]});
    for (const [name, scenario] of writeDeadlines) tests.push({name,
        command: ['/test/test-host-write-timeout', scenario, '/test/capsid-worker'],
        properties: [{name: 'TIMEOUT', value: 30}]});
    for (const scenario of ['idle', 'exchange', 'tls-reset', 'reset-session', 'close-reset']) {
        tests.push({name: 'websocket_python_peer_' + scenario,
            command: [process.execPath, 'test_websocket_python_peer_lifecycle.mjs', '--case', scenario],
            properties: [{name: 'TIMEOUT', value: 45}]});
    }
    for (const mode of ['single-worker', 'static-pool']) for (const scenario of [
        'idle', 'parent-eof', 'repeat-stop', 'active-ws', 'no-close-reply', 'pending-dial',
        'startup-fail', 'invalid-fd', 'ready-alias', 'stdio-fd', 'malformed-fd', 'file-fd',
        ...(system === 'Windows' ? [] : ['signal']),
    ]) tests.push({name: `host_parent_stop_${mode}_${scenario}`,
        command: [process.execPath, 'test_host_parent_stop.mjs', '--host', '/test/capsid-host',
            '--worker', '/test/capsid-worker', '--mode', mode, '--case', scenario],
        properties: [{name: 'TIMEOUT', value: 40}]});
    for (const scenario of ['before-completion', 'before-adoption', 'cancel-pending', 'queued-before-exit',
        'idle-after-adoption', 'keepalive-after-adoption', 'pre-post-worker-exit', 'pre-post-stop', 'pre-post-drain']) {
        tests.push({name: `host_external_accept_exit_${scenario}`,
            command: [process.execPath, 'test_host_external_accept_exit.mjs',
                '--fixture', '/test/test-host-single-worker-lifecycle', '--worker', '/test/capsid-worker', '--case', scenario],
            properties: [{name: 'TIMEOUT', value: 45}]});
    }
    for (const scenario of ['first-worker-exit', 'last-worker-exit', 'first-worker-exit-websocket',
        'all-workers-exit', 'last-ready-worker-exit', 'all-workers-exit-websocket']) {
        tests.push({name: `host_static_pool_fault_${scenario}`,
            command: [process.execPath, 'test_host_static_pool_faults.mjs',
                '--fixture', '/test/test-host-static-pool-faults', '--worker', '/test/capsid-worker', '--case', scenario],
            properties: [{name: 'TIMEOUT', value: 45}]});
    }
    for (const scenario of ['reselect-worker-exit', 'reselect-last-workers-exit', 'pre-post-stop',
        'pre-post-drain', 'active-drain']) {
        tests.push({name: `host_static_pool_adoption_${scenario}`,
            command: [process.execPath, 'test_host_static_pool_adoption.mjs',
                '--fixture', '/test/test-host-static-pool-adoption', '--worker', '/test/capsid-worker', '--case', scenario],
            properties: [{name: 'TIMEOUT', value: 60}]});
    }
    for (const scenario of ['drain-before-submit', 'drain-during-prepare', 'queued-before-drain',
        'owner-drain-before-submit', 'healthy-control']) {
        tests.push({name: `host_external_drain_${scenario}`,
            command: [process.execPath, 'test_host_external_drain.mjs',
                '--fixture', '/test/test-host-external-drain', '--worker', '/test/capsid-worker', '--case', scenario],
            properties: [{name: 'TIMEOUT', value: 60}]});
    }
    for (const scenario of ['stop', 'drain']) {
        tests.push({name: `host_static_pool_local_shutdown_${scenario}`,
            command: [process.execPath, 'test_host_static_pool_local_shutdown.mjs',
                '--fixture', '/test/test-host-static-pool-local-shutdown', '--worker', '/test/capsid-worker', '--case', scenario],
            properties: [{name: 'TIMEOUT', value: 60}]});
    }
    for (const scenario of ['get-close-before-completion', 'head-close-before-completion', 'get-normal-completion']) {
        tests.push({name: `host_unavailable_lifecycle_${scenario}`,
            command: ['/test/test-host-unavailable-lifecycle', scenario],
            properties: [{name: 'TIMEOUT', value: 20}]});
    }
    return {kind: 'ctestInfo', tests};
}
for (const system of ['Linux', 'Windows', 'Darwin']) for (const [name] of writeDeadlines) {
    test(`${system}: write deadline ${name} cannot disappear`, () => {
        const inventory = sample(system);
        inventory.tests = inventory.tests.filter(entry => entry.name !== name);
        assert.throws(() => validatePlatformMatrix(inventory, system, process.execPath), /missing required write deadline test/);
    });
}
for (const [name] of writeDeadlines) for (const [label, mutate, pattern] of [
    ['executable alias', entry => { entry.command[0] = '/test/unrelated'; }, /intended write deadline executable/],
    ['case alias', entry => { entry.command[1] = 'unrelated'; }, /actual write deadline scenario/],
    ['Worker alias', entry => { entry.command[2] = '/test/unrelated'; }, /actual write deadline Worker/],
    ['extra arguments', entry => { entry.command.push('extra'); }, /direct write deadline command/],
    ['disabled', entry => { entry.properties.push({name: 'DISABLED', value: true}); }, /cannot weaken write deadline test/],
    ['inverted pass', entry => { entry.properties.push({name: 'WILL_FAIL', value: true}); }, /cannot weaken write deadline test/],
    ['skip allowance', entry => { entry.properties.push({name: 'SKIP_RETURN_CODE', value: 77}); }, /cannot skip/],
    ['no timeout', entry => { entry.properties = []; }, /bounded write deadline timeout/],
]) test(`write deadline ${name} rejects ${label}`, () => {
    const inventory = sample('Windows');
    mutate(inventory.tests.find(entry => entry.name === name));
    assert.throws(() => validatePlatformMatrix(inventory, 'Windows', process.execPath), pattern);
});
for (const system of ['Linux', 'Windows', 'Darwin']) {
    test(`${system}: complete synthetic inventory is accepted without mutation`, () => {
        const inventory = sample(system), original = structuredClone(inventory);
        const result = validatePlatformMatrix(inventory, system, process.execPath);
        assert.equal(result.required.length, system === 'Linux' ? 154 : system === 'Windows' ? 133 : 118);
        assert.deepEqual(inventory, original);
    });
    test(`${system}: removing a required candidate fails`, () => {
        const inventory = sample(system);
        inventory.tests = inventory.tests.filter(test => test.name !== 'websocket_interop_static-pool');
        assert.throws(() => validatePlatformMatrix(inventory, system, process.execPath), /missing required test websocket_interop_static-pool/);
    });
    for (const mode of system === 'Linux' ? ['single-worker', 'static-pool', 'managed'] : ['single-worker', 'static-pool']) {
        test(`${system}: throw wire regression ${mode} cannot disappear`, () => {
            const inventory = sample(system), name = `websocket_throw_wire_${mode}`;
            inventory.tests = inventory.tests.filter(entry => entry.name !== name);
            assert.throws(() => validatePlatformMatrix(inventory, system, process.execPath),
                new RegExp('missing required test ' + name));
        });
    }
}
for (const system of ['Linux', 'Windows']) for (const mode of system === 'Linux'
    ? ['single-worker', 'static-pool', 'managed'] : ['single-worker', 'static-pool']) {
    test(`${system}: connection owner lifecycle ${mode} cannot disappear`, () => {
        const inventory = sample(system), name = `websocket_owner_lifecycle_${mode}`;
        inventory.tests = inventory.tests.filter(entry => entry.name !== name);
        assert.throws(() => validatePlatformMatrix(inventory, system, process.execPath),
            new RegExp('missing required test ' + name));
    });
}
for (const system of ['Linux', 'Windows']) for (const name of ['workerd_ws_reference', 'websocket_outbound_reference',
    'websocket_interop_reference', 'workerd_fanout_reference', 'websocket_frame_boundaries_reference',
    'websocket_owner_lifecycle_reference', 'websocket_cross_context_reference', 'websocket_fetch_resources_reference',
    'websocket_fetch_upgrade_reference',
    'websocket_fetch_upgrade_lifetime_reference', 'websocket_fetch_proxy_reference',
    'websocket_crypto_reference']) {
    test(`${system}: required reference ${name} cannot disappear`, () => {
        const inventory = sample(system);inventory.tests = inventory.tests.filter(entry => entry.name !== name);
        assert.throws(() => validatePlatformMatrix(inventory, system, process.execPath),
            new RegExp('missing required reference test ' + name));
    });
}
for (const system of ['Linux', 'Windows', 'Darwin']) {
    for (const family of ['websocket_async_callbacks', 'websocket_async_budget', 'websocket_cross_context', 'websocket_fetch_resources', 'websocket_fetch_upgrade', 'websocket_fetch_upgrade_lifetime', 'websocket_fetch_proxy', 'websocket_crypto']) {
        test(`${system}: ${family} cannot disappear`, () => {
            const inventory=sample(system);inventory.tests=inventory.tests.filter(value=>value.name!==family+'_static-pool');
            assert.throws(()=>validatePlatformMatrix(inventory,system,process.execPath),/missing required test/);
        });
    }
}
for (const system of ['Linux', 'Windows', 'Darwin']) for (const name of ['tjs_fetch_context_cancel_pending_handshake', 'tjs_fetch_context_cancel_dns',
    'tjs_fetch_policy_async_context', 'tjs_fetch_policy_binding_context', 'tjs_fetch_policy_unowned_context'])
    test(`${system}: ${name} cannot disappear`, () => {
        const inventory = sample(system); inventory.tests = inventory.tests.filter(entry => entry.name !== name);
        assert.throws(() => validatePlatformMatrix(inventory, system, process.execPath), /missing required fetch abort test/);
    });
for (const [name, argument] of [['tjs_fetch_context_cancel_pending_handshake', '--context-cancel'], ['tjs_fetch_context_cancel_dns', '--dns-context-cancel'],
    ['tjs_fetch_policy_async_context', '--policy-async-context'], ['tjs_fetch_policy_binding_context', '--policy-binding-context'],
    ['tjs_fetch_policy_unowned_context', '--policy-unowned-context']])
    test(name + ' must retain its exact native scenario', () => {
        const inventory = sample('Windows'), entry = inventory.tests.find(value => value.name === name);
        assert.equal(entry.command[1], argument); entry.command[1] = '--unknown';
        assert.throws(() => validatePlatformMatrix(inventory, 'Windows', process.execPath), /exact native regression scenario/);
    });
for (const system of ['Linux', 'Windows', 'Darwin']) for (const name of ['host_dns_dialer_lifecycle', 'host_dns_reserved_invalid'])
    test(`${system}: ${name} cannot disappear`, () => {
        const inventory = sample(system); inventory.tests = inventory.tests.filter(value => value.name !== name);
        assert.throws(() => validatePlatformMatrix(inventory, system, process.execPath), /missing required native DNS test/);
    });
for (const name of ['host_dns_dialer_lifecycle', 'host_dns_reserved_invalid']) for (const [kind, mutate, pattern] of [
    ['wrong scenario', entry => {entry.command.push('--unknown');}, /direct native DNS command/],
    ['disabled', entry => {entry.properties.push({name: 'DISABLED', value: true});}, /cannot weaken DNS test/],
    ['inverted result', entry => {entry.properties.push({name: 'WILL_FAIL', value: true});}, /cannot weaken DNS test/],
    ['skip allowance', entry => {entry.properties.push({name: 'SKIP_RETURN_CODE', value: 77});}, /cannot skip/],
    ['unbounded timeout', entry => {entry.properties = [];}, /bounded DNS timeout/],
    ['wrong executable', entry => {entry.command[0] = '/test/other';}, /intended DNS executable/],
]) test(name + ' rejects ' + kind, () => {
    const inventory = sample('Windows'); mutate(inventory.tests.find(value => value.name === name));
    assert.throws(() => validatePlatformMatrix(inventory, 'Windows', process.execPath), pattern);
});
for (const [name, mutate, pattern] of [
    ['shared switches to budget', entry=>entry.command.push('--budgets','1'), /exact async callback family/],
    ['budget selector removed', entry=>{entry.command.splice(entry.command.indexOf('--budgets'),2);}, /exact async callback family/],
    ['budget selector duplicated', entry=>entry.command.push('--budgets','1'), /unique --budgets/],
    ['budget selector weakened', entry=>{entry.command[entry.command.indexOf('--budgets')+1]='0';}, /exact async callback family/],
]) test('async registration rejects '+name,()=>{
    const inventory=sample('Windows');
    mutate(inventory.tests.find(value=>value.name===(name.startsWith('shared')?'websocket_async_callbacks_single-worker':'websocket_async_budget_single-worker')));
    assert.throws(()=>validatePlatformMatrix(inventory,'Windows',process.execPath),pattern);
});
for (const system of ['Linux','Windows']) test(`${system}: native async reference cannot disappear`,()=>{
    const inventory=sample(system);inventory.tests=inventory.tests.filter(value=>value.name!=='websocket_async_callbacks_reference');
    assert.throws(()=>validatePlatformMatrix(inventory,system,process.execPath),/missing required reference test websocket_async_callbacks_reference/);
});
for (const [name, corrupt, diagnostic] of [
    ['selector removed', entry => {entry.command = entry.command.filter(value => value !== '--reference-from-env');}, /unique locked reference flag/],
    ['duplicate selector', entry => entry.command.push('--reference-from-env', '1'), /unique locked reference flag/],
    ['wrong selector', entry => {entry.command[entry.command.indexOf('--reference-from-env') + 1] = '0';}, /actual reference selector/],
    ['candidate alias', entry => entry.command.push('--host', '/fake/host'), /reference cannot be a backend/],
    ['binary alias', entry => entry.command.push('--workerd', '/fake/workerd'), /reference cannot be a backend or unlocked binary alias/],
    ['driver alias', entry => {entry.command[1] = 'unrelated.mjs';}, /intended reference driver/],
    ['disabled', entry => entry.properties.push({name: 'DISABLED', value: true}), /cannot weaken reference test/],
    ['inverted pass', entry => entry.properties.push({name: 'WILL_FAIL', value: true}), /cannot weaken reference test/],
    ['skip allowance', entry => entry.properties.push({name: 'SKIP_RETURN_CODE', value: 77}), /required reference cannot skip/],
    ['missing timeout', entry => {entry.properties = [];}, /bounded reference timeout/],
]) test(`Windows reference registration rejects ${name}`, () => {
    const inventory = sample('Windows');corrupt(inventory.tests.find(entry => entry.name === 'websocket_interop_reference'));
    assert.throws(() => validatePlatformMatrix(inventory, 'Windows', process.execPath), diagnostic);
});
for (const system of ['Linux', 'Windows', 'Darwin']) {
    for (const scenario of ['first-worker-exit', 'last-worker-exit', 'first-worker-exit-websocket',
        'all-workers-exit', 'last-ready-worker-exit', 'all-workers-exit-websocket']) {
        test(`${system}: pool fault ${scenario} cannot disappear`, () => {
            const inventory = sample(system);
            inventory.tests = inventory.tests.filter(entry => entry.name !== `host_static_pool_fault_${scenario}`);
            assert.throws(() => validatePlatformMatrix(inventory, system, process.execPath), /missing required pool fault test/);
        });
    }
    test(`${system}: pending-handshake abort regression cannot disappear`, () => {
        const inventory = sample(system);
        inventory.tests = inventory.tests.filter(entry => entry.name !== 'tjs_fetch_abort_pending_handshake');
        assert.throws(() => validatePlatformMatrix(inventory, system, process.execPath), /missing required fetch abort test/);
    });
}
for (const [name, mutate, pattern] of [
    ['case alias', entry => { entry.command[entry.command.indexOf('--case') + 1] = 'last-worker-exit'; }, /actual pool fault scenario/],
    ['duplicate case', entry => { entry.command.push('--case', 'first-worker-exit'); }, /unique required --case/],
    ['fixture alias', entry => { entry.command[entry.command.indexOf('--fixture') + 1] = '/test/unrelated'; }, /intended native pool fixture/],
    ['Worker alias', entry => { entry.command[entry.command.indexOf('--worker') + 1] = '/test/unrelated'; }, /actual Worker executable/],
    ['driver alias', entry => { entry.command[1] = 'unrelated.mjs'; }, /intended pool fault driver/],
    ['Node alias', entry => { entry.command[0] = '/different/node'; }, /pinned Node executable/],
    ['disabled', entry => { entry.properties.push({name: 'DISABLED', value: true}); }, /cannot weaken pool fault test/],
    ['inverted pass', entry => { entry.properties.push({name: 'WILL_FAIL', value: true}); }, /cannot weaken pool fault test/],
    ['skip allowance', entry => { entry.properties.push({name: 'SKIP_RETURN_CODE', value: 77}); }, /cannot skip/],
    ['no timeout', entry => { entry.properties = []; }, /bounded pool fault timeout/],
]) test(`pool fault registration rejects ${name}`, () => {
    const inventory = sample('Windows');
    mutate(inventory.tests.find(entry => entry.name === 'host_static_pool_fault_first-worker-exit'));
    assert.throws(() => validatePlatformMatrix(inventory, 'Windows', process.execPath), pattern);
});
for (const [name, mutate, pattern] of [
    ['command alias', entry => { entry.command[0] = '/test/unrelated'; }, /intended native fetch abort executable/],
    ['extra arguments', entry => { entry.command.push('--unrelated'); }, /direct native regression command/],
    ['disabled', entry => { entry.properties.push({name: 'DISABLED', value: true}); }, /cannot weaken fetch abort test/],
    ['inverted pass', entry => { entry.properties.push({name: 'WILL_FAIL', value: true}); }, /cannot weaken fetch abort test/],
    ['skip allowance', entry => { entry.properties.push({name: 'SKIP_RETURN_CODE', value: 77}); }, /cannot skip/],
    ['no timeout', entry => { entry.properties = []; }, /bounded fetch abort timeout/],
]) test(`fetch abort registration rejects ${name}`, () => {
    const inventory = sample('Windows');
    mutate(inventory.tests.find(entry => entry.name === 'tjs_fetch_abort_pending_handshake'));
    assert.throws(() => validatePlatformMatrix(inventory, 'Windows', process.execPath), pattern);
});
for (const system of ['Linux', 'Windows', 'Darwin']) {
    for (const scenario of ['reselect-worker-exit', 'reselect-last-workers-exit', 'pre-post-stop',
        'pre-post-drain', 'active-drain']) {
        test(`${system}: pool adoption ${scenario} cannot disappear`, () => {
            const inventory = sample(system);
            inventory.tests = inventory.tests.filter(entry => entry.name !== `host_static_pool_adoption_${scenario}`);
            assert.throws(() => validatePlatformMatrix(inventory, system, process.execPath), /missing required pool adoption test/);
        });
    }
}
for (const [name, mutate, pattern] of [
    ['case alias', entry => { entry.command[entry.command.indexOf('--case') + 1] = 'active-drain'; }, /actual pool adoption scenario/],
    ['duplicate case', entry => { entry.command.push('--case', 'reselect-worker-exit'); }, /unique required --case/],
    ['fixture alias', entry => { entry.command[entry.command.indexOf('--fixture') + 1] = '/test/unrelated'; }, /intended native pool adoption fixture/],
    ['Worker alias', entry => { entry.command[entry.command.indexOf('--worker') + 1] = '/test/unrelated'; }, /actual Worker executable/],
    ['driver alias', entry => { entry.command[1] = 'unrelated.mjs'; }, /intended pool adoption driver/],
    ['Node alias', entry => { entry.command[0] = '/different/node'; }, /pinned Node executable/],
    ['disabled', entry => { entry.properties.push({name: 'DISABLED', value: true}); }, /cannot weaken pool adoption test/],
    ['inverted pass', entry => { entry.properties.push({name: 'WILL_FAIL', value: true}); }, /cannot weaken pool adoption test/],
    ['skip allowance', entry => { entry.properties.push({name: 'SKIP_RETURN_CODE', value: 77}); }, /cannot skip/],
    ['no timeout', entry => { entry.properties = []; }, /bounded pool adoption timeout/],
]) test(`pool adoption registration rejects ${name}`, () => {
    const inventory = sample('Windows');
    mutate(inventory.tests.find(entry => entry.name === 'host_static_pool_adoption_reselect-worker-exit'));
    assert.throws(() => validatePlatformMatrix(inventory, 'Windows', process.execPath), pattern);
});
for (const system of ['Linux', 'Windows', 'Darwin']) {
    for (const scenario of ['drain-before-submit', 'drain-during-prepare', 'queued-before-drain',
        'owner-drain-before-submit', 'healthy-control']) {
        test(`${system}: external drain ${scenario} cannot disappear`, () => {
            const inventory = sample(system);
            inventory.tests = inventory.tests.filter(entry => entry.name !== `host_external_drain_${scenario}`);
            assert.throws(() => validatePlatformMatrix(inventory, system, process.execPath), /missing required external drain test/);
        });
    }
}
for (const [name, mutate, pattern] of [
    ['case alias', entry => { entry.command[entry.command.indexOf('--case') + 1] = 'healthy-control'; }, /actual external drain scenario/],
    ['duplicate case', entry => { entry.command.push('--case', 'drain-before-submit'); }, /unique required --case/],
    ['fixture alias', entry => { entry.command[entry.command.indexOf('--fixture') + 1] = '/test/unrelated'; }, /intended native external drain fixture/],
    ['Worker alias', entry => { entry.command[entry.command.indexOf('--worker') + 1] = '/test/unrelated'; }, /actual Worker executable/],
    ['driver alias', entry => { entry.command[1] = 'unrelated.mjs'; }, /intended external drain driver/],
    ['Node alias', entry => { entry.command[0] = '/different/node'; }, /pinned Node executable/],
    ['disabled', entry => { entry.properties.push({name: 'DISABLED', value: true}); }, /cannot weaken external drain test/],
    ['inverted pass', entry => { entry.properties.push({name: 'WILL_FAIL', value: true}); }, /cannot weaken external drain test/],
    ['skip allowance', entry => { entry.properties.push({name: 'SKIP_RETURN_CODE', value: 77}); }, /cannot skip/],
    ['no timeout', entry => { entry.properties = []; }, /bounded external drain timeout/],
]) test(`external drain registration rejects ${name}`, () => {
    const inventory = sample('Windows');
    mutate(inventory.tests.find(entry => entry.name === 'host_external_drain_drain-before-submit'));
    assert.throws(() => validatePlatformMatrix(inventory, 'Windows', process.execPath), pattern);
});
for (const system of ['Linux', 'Windows', 'Darwin']) {
    for (const scenario of ['stop', 'drain']) {
        test(`${system}: local pool shutdown ${scenario} cannot disappear`, () => {
            const inventory = sample(system);
            inventory.tests = inventory.tests.filter(entry => entry.name !== `host_static_pool_local_shutdown_${scenario}`);
            assert.throws(() => validatePlatformMatrix(inventory, system, process.execPath), /missing required local pool shutdown test/);
        });
    }
}
for (const [name, mutate, pattern] of [
    ['case alias', entry => { entry.command[entry.command.indexOf('--case') + 1] = 'drain'; }, /actual local pool shutdown scenario/],
    ['duplicate case', entry => { entry.command.push('--case', 'stop'); }, /unique required --case/],
    ['fixture alias', entry => { entry.command[entry.command.indexOf('--fixture') + 1] = '/test/unrelated'; }, /intended native local pool shutdown fixture/],
    ['Worker alias', entry => { entry.command[entry.command.indexOf('--worker') + 1] = '/test/unrelated'; }, /actual Worker executable/],
    ['driver alias', entry => { entry.command[1] = 'unrelated.mjs'; }, /intended local pool shutdown driver/],
    ['Node alias', entry => { entry.command[0] = '/different/node'; }, /pinned Node executable/],
    ['disabled', entry => { entry.properties.push({name: 'DISABLED', value: true}); }, /cannot weaken local pool shutdown test/],
    ['inverted pass', entry => { entry.properties.push({name: 'WILL_FAIL', value: true}); }, /cannot weaken local pool shutdown test/],
    ['skip allowance', entry => { entry.properties.push({name: 'SKIP_RETURN_CODE', value: 77}); }, /cannot skip/],
    ['no timeout', entry => { entry.properties = []; }, /bounded local pool shutdown timeout/],
]) test(`local pool shutdown registration rejects ${name}`, () => {
    const inventory = sample('Windows');
    mutate(inventory.tests.find(entry => entry.name === 'host_static_pool_local_shutdown_stop'));
    assert.throws(() => validatePlatformMatrix(inventory, 'Windows', process.execPath), pattern);
});
for (const system of ['Linux', 'Windows', 'Darwin']) {
    for (const scenario of ['get-close-before-completion', 'head-close-before-completion', 'get-normal-completion']) {
        test(`${system}: unavailable lifecycle ${scenario} cannot disappear`, () => {
            const inventory = sample(system);
            inventory.tests = inventory.tests.filter(entry => entry.name !== `host_unavailable_lifecycle_${scenario}`);
            assert.throws(() => validatePlatformMatrix(inventory, system, process.execPath), /missing required unavailable lifecycle test/);
        });
    }
}
for (const [name, mutate, pattern] of [
    ['executable alias', entry => { entry.command[0] = '/test/unrelated'; }, /intended unavailable lifecycle executable/],
    ['case alias', entry => { entry.command[1] = 'get-normal-completion'; }, /actual unavailable lifecycle scenario/],
    ['extra arguments', entry => { entry.command.push('get-close-before-completion'); }, /direct native lifecycle command/],
    ['disabled', entry => { entry.properties.push({name: 'DISABLED', value: true}); }, /cannot weaken unavailable lifecycle test/],
    ['inverted pass', entry => { entry.properties.push({name: 'WILL_FAIL', value: true}); }, /cannot weaken unavailable lifecycle test/],
    ['skip allowance', entry => { entry.properties.push({name: 'SKIP_RETURN_CODE', value: 77}); }, /cannot skip/],
    ['no timeout', entry => { entry.properties = []; }, /bounded unavailable lifecycle timeout/],
]) test(`unavailable lifecycle registration rejects ${name}`, () => {
    const inventory = sample('Windows');
    mutate(inventory.tests.find(entry => entry.name === 'host_unavailable_lifecycle_get-close-before-completion'));
    assert.throws(() => validatePlatformMatrix(inventory, 'Windows', process.execPath), pattern);
});
for (const system of ['Linux', 'Windows', 'Darwin']) {
    for (const scenario of ['before-completion', 'before-adoption', 'cancel-pending', 'queued-before-exit',
        'idle-after-adoption', 'keepalive-after-adoption', 'pre-post-worker-exit', 'pre-post-stop', 'pre-post-drain']) {
        test(`${system}: external accept ${scenario} cannot disappear`, () => {
            const inventory = sample(system);
            inventory.tests = inventory.tests.filter(entry => entry.name !== `host_external_accept_exit_${scenario}`);
            assert.throws(() => validatePlatformMatrix(inventory, system, process.execPath), /missing required external accept test/);
        });
    }
}
for (const [name, mutate, pattern] of [
    ['case alias', entry => { entry.command[entry.command.indexOf('--case') + 1] = 'before-completion'; }, /actual accept scenario/],
    ['duplicate case', entry => { entry.command.push('--case', 'before-adoption'); }, /unique required --case/],
    ['fixture alias', entry => { entry.command[entry.command.indexOf('--fixture') + 1] = '/test/unrelated'; }, /intended native accept fixture/],
    ['Worker alias', entry => { entry.command[entry.command.indexOf('--worker') + 1] = '/test/unrelated'; }, /actual Worker executable/],
    ['driver alias', entry => { entry.command[1] = 'unrelated.mjs'; }, /intended accept driver/],
    ['Node alias', entry => { entry.command[0] = '/different/node'; }, /pinned Node executable/],
    ['disabled', entry => { entry.properties.push({name: 'DISABLED', value: true}); }, /cannot weaken accept test/],
    ['inverted pass', entry => { entry.properties.push({name: 'WILL_FAIL', value: true}); }, /cannot weaken accept test/],
    ['skip allowance', entry => { entry.properties.push({name: 'SKIP_RETURN_CODE', value: 77}); }, /cannot skip/],
    ['no timeout', entry => { entry.properties = []; }, /bounded accept timeout/],
]) test(`external accept registration rejects ${name}`, () => {
    const inventory = sample('Windows');
    mutate(inventory.tests.find(entry => entry.name === 'host_external_accept_exit_before-adoption'));
    assert.throws(() => validatePlatformMatrix(inventory, 'Windows', process.execPath), pattern);
});
for (const [name, mutate, pattern] of [
    ['mode alias', test => { test.command[test.command.indexOf('--mode') + 1] = 'single-worker'; }, /actual backend mode/],
    ['driver alias', test => { test.command[1] = 'unrelated.mjs'; }, /intended driver/],
    ['unpinned Node', test => { test.command[0] = '/different/node'; }, /pinned Node executable/],
    ['missing Host', test => { test.command.splice(test.command.indexOf('--host'), 2); }, /actual host argument/],
    ['disabled', test => { test.properties.push({name: 'DISABLED', value: true}); }, /DISABLED/],
    ['inverted pass', test => { test.properties.push({name: 'WILL_FAIL', value: true}); }, /WILL_FAIL/],
    ['skip allowance', test => { test.properties.push({name: 'SKIP_RETURN_CODE', value: 77}); }, /cannot skip/],
    ['unbounded', test => { test.properties = []; }, /bounded execution timeout/],
    ['reference alias', test => { test.command.push('--reference-from-env'); }, /reference alias/],
]) test(`synthetic negative: ${name}`, () => {
    const inventory = sample('Windows');
    mutate(inventory.tests.find(test => test.name === 'websocket_interop_static-pool'));
    assert.throws(() => validatePlatformMatrix(inventory, 'Windows', process.execPath), pattern);
});
for (const system of ['Linux', 'Windows', 'Darwin']) test(`${system}: frame-boundary driver cannot disappear`, () => {
    const inventory = sample(system);
    inventory.tests = inventory.tests.filter(entry => entry.name !== 'websocket_frame_boundaries_static-pool');
    assert.throws(() => validatePlatformMatrix(inventory, system, process.execPath), /missing required test websocket_frame_boundaries_static-pool/);
});
for (const system of ['Windows', 'Darwin']) test(`${system}: Linux-only managed entry fails`, () => {
    const inventory = sample(system);
    inventory.tests.push({name: 'websocket_budget_managed'});
    assert.throws(() => validatePlatformMatrix(inventory, system, process.execPath), /Linux-only managed scenario/);
});
test('synthetic negative: duplicate CTest names fail', () => {
    const inventory = sample('Linux');
    inventory.tests.push(structuredClone(inventory.tests[0]));
    assert.throws(() => validatePlatformMatrix(inventory, 'Linux', process.execPath), /duplicate CTest name/);
});
test('Windows: the native startup regression cannot disappear', () => {
    const inventory = sample('Windows');
    inventory.tests = inventory.tests.filter(test => test.name !== 'worker_windows_environment');
    assert.throws(() => validatePlatformMatrix(inventory, 'Windows', process.execPath), /required OS environment startup regression/);
});

for (const system of ['Linux', 'Windows', 'Darwin']) {
    for (const scenario of ['idle', 'exchange', 'tls-reset', 'reset-session', 'close-reset']) {
        test(`${system}: Python peer ${scenario} cannot disappear`, () => {
            const inventory = sample(system);
            inventory.tests = inventory.tests.filter(entry => entry.name !== 'websocket_python_peer_' + scenario);
            assert.throws(() => validatePlatformMatrix(inventory, system, process.execPath), /missing required Python peer test/);
        });
    }
}
for (const [name, mutate, pattern] of [
    ['case alias', entry => { entry.command[3] = 'exchange'; }, /actual peer scenario/],
    ['duplicate case', entry => { entry.command.push('--case', 'close-reset'); }, /unique peer scenario/],
    ['driver alias', entry => { entry.command[1] = 'unrelated.mjs'; }, /intended peer driver/],
    ['Node alias', entry => { entry.command[0] = '/different/node'; }, /pinned Node executable/],
    ['disabled', entry => { entry.properties.push({name: 'DISABLED', value: true}); }, /cannot weaken/],
    ['inverted pass', entry => { entry.properties.push({name: 'WILL_FAIL', value: true}); }, /cannot weaken/],
    ['skip allowance', entry => { entry.properties.push({name: 'SKIP_RETURN_CODE', value: 77}); }, /cannot skip/],
    ['no timeout', entry => { entry.properties = []; }, /bounded peer timeout/],
]) test(`Python peer registration rejects ${name}`, () => {
    const inventory = sample('Windows');
    mutate(inventory.tests.find(entry => entry.name === 'websocket_python_peer_close-reset'));
    assert.throws(() => validatePlatformMatrix(inventory, 'Windows', process.execPath), pattern);
});

for (const system of ['Linux', 'Windows', 'Darwin']) {
    for (const mode of ['single-worker', 'static-pool']) for (const scenario of [
        'idle', 'parent-eof', 'repeat-stop', 'active-ws', 'no-close-reply', 'pending-dial',
        'startup-fail', 'invalid-fd', 'ready-alias', 'stdio-fd', 'malformed-fd', 'file-fd',
        ...(system === 'Windows' ? [] : ['signal']),
    ]) test(`${system}: parent stop ${mode}/${scenario} cannot disappear`, () => {
        const inventory = sample(system);
        inventory.tests = inventory.tests.filter(entry => entry.name !== `host_parent_stop_${mode}_${scenario}`);
        assert.throws(() => validatePlatformMatrix(inventory, system, process.execPath), /missing required parent stop test/);
    });
}
for (const [name, mutate, pattern] of [
    ['case alias', entry => { entry.command[entry.command.indexOf('--case') + 1] = 'idle'; }, /actual stop scenario/],
    ['mode alias', entry => { entry.command[entry.command.indexOf('--mode') + 1] = 'single-worker'; }, /actual stop mode/],
    ['duplicate case', entry => { entry.command.push('--case', 'active-ws'); }, /unique --case/],
    ['missing Worker', entry => { entry.command.splice(entry.command.indexOf('--worker'), 2); }, /actual worker argument/],
    ['driver alias', entry => { entry.command[1] = 'unrelated.mjs'; }, /intended stop driver/],
    ['Node alias', entry => { entry.command[0] = '/different/node'; }, /pinned Node executable/],
    ['disabled', entry => { entry.properties.push({name: 'DISABLED', value: true}); }, /cannot weaken/],
    ['inverted pass', entry => { entry.properties.push({name: 'WILL_FAIL', value: true}); }, /cannot weaken/],
    ['skip allowance', entry => { entry.properties.push({name: 'SKIP_RETURN_CODE', value: 77}); }, /cannot skip/],
    ['no timeout', entry => { entry.properties = []; }, /bounded stop timeout/],
]) test(`parent stop registration rejects ${name}`, () => {
    const inventory = sample('Windows');
    mutate(inventory.tests.find(entry => entry.name === 'host_parent_stop_static-pool_active-ws'));
    assert.throws(() => validatePlatformMatrix(inventory, 'Windows', process.execPath), pattern);
});

test('real CTest inventory preserves active runner files and nested registrations', () => {
    // These are synthetic registrations, read by a real CTest process. No
    // application execution or platform compatibility is claimed here.
    const root = fs.mkdtempSync(path.join(os.tmpdir(), 'capsid-ctest-inventory-'));
    assert.ok(path.isAbsolute(root) && path.basename(root).startsWith('capsid-ctest-inventory-'));
    try {
        const registrations = sample('Linux').tests;
        // CTest resolves single-executable commands even in --show-only.
        // This inert file supplies that inventory path; it is never run.
        const native = path.join(root, 'test-tjs-fetch-abort' + (process.platform === 'win32' ? '.exe' : ''));
        fs.writeFileSync(native, 'synthetic inventory executable path; not an application test\n');
        const channelIo=path.join(root,'test-channel-io'+(process.platform==='win32'?'.exe':''));
        fs.writeFileSync(channelIo,'synthetic inventory executable path; not an application test\n');
        registrations.find(entry=>entry.name==='channel_owned_io').command[0]=channelIo;
        const portablePool=path.join(root,'test-host-generation-pool'+(process.platform==='win32'?'.exe':''));
        fs.writeFileSync(portablePool,'synthetic inventory executable path; not an application test\n');
        registrations.find(entry=>entry.name==='host_generation_pool_portable_contract').command[0]=portablePool;
        for (const entry of registrations.filter(value => /^tjs_fetch_(?:abort|context_cancel|policy)_/.test(value.name))) entry.command[0] = native;
        const dns = path.join(root, 'test-host-dns-dialer' + (process.platform === 'win32' ? '.exe' : ''));
        fs.writeFileSync(dns, 'synthetic inventory executable path; not an application test\n');
        for (const entry of registrations.filter(value => /^host_dns_/.test(value.name))) entry.command[0] = dns;
        const lifecycle = path.join(root, 'test-host-unavailable-lifecycle' + (process.platform === 'win32' ? '.exe' : ''));
        fs.writeFileSync(lifecycle, 'synthetic inventory executable path; not an application test\n');
        for (const entry of registrations.filter(entry => entry.name.startsWith('host_unavailable_lifecycle_')))
            entry.command[0] = lifecycle;
        const writeDeadline = path.join(root, 'test-host-write-timeout' + (process.platform === 'win32' ? '.exe' : ''));
        fs.writeFileSync(writeDeadline, 'synthetic inventory executable path; not an application test\n');
        for (const entry of registrations.filter(entry => writeDeadlines.some(([name]) => entry.name === name)))
            entry.command[0] = writeDeadline;
        const quote = value => `"${value.replaceAll('\\', '/').replaceAll('"', '\\"')}"`;
        const render = entries => entries.map(entry =>
            `add_test(${[entry.name, ...entry.command].map(quote).join(' ')})\n` +
            `set_tests_properties(${quote(entry.name)} PROPERTIES TIMEOUT ${entry.properties.find(property => property.name === 'TIMEOUT').value})\n`).join('');
        fs.mkdirSync(path.join(root, 'nested'));
        fs.writeFileSync(path.join(root, 'CTestTestfile.cmake'),
            render(registrations.slice(0, 10)) + 'subdirs("nested")\n');
        fs.writeFileSync(path.join(root, 'nested', 'CTestTestfile.cmake'), render(registrations.slice(10)));
        const directory = path.join(root, 'Testing', 'Temporary');
        fs.mkdirSync(directory, {recursive: true});
        const markers = new Map(['LastTest.log', 'LastTest.log.tmp', 'LastTestsFailed.log', 'CTestCostData.txt']
            .map(name => [name, Buffer.from(`outer runner evidence: ${name}\n`)]));
        for (const [name, content] of markers) fs.writeFileSync(path.join(directory, name), content);
        const result = spawnSync(process.execPath, [
            fileURLToPath(new URL('./test_websocket_platform_matrix.mjs', import.meta.url)),
            '--ctest', process.env.CAPSID_PLATFORM_TEST_CTEST ?? 'ctest',
            '--build-dir', root, '--system', 'Linux',
        ], {encoding: 'utf8', timeout: 40000, windowsHide: true});
        assert.ifError(result.error);
        assert.equal(result.status, 0, `${result.stdout}\n${result.stderr}`);
        assert.equal(JSON.parse(result.stdout).required.length, 154, 'nested registrations remain visible');
        for (const [name, content] of markers) {
            assert.ok(fs.existsSync(path.join(directory, name)), `${name}: active runner file must remain present`);
            assert.deepEqual(fs.readFileSync(path.join(directory, name)), content,
                `${name}: inventory must preserve the outer runner's evidence`);
        }
        assert.deepEqual(fs.readdirSync(directory).sort(), [...markers.keys()].sort(),
            'inventory cannot add runner state to the active build');
    } finally {
        fs.rmSync(root, {recursive: true, force: true});
    }
});

for(const [label,mutate,pattern] of [
    ['driver alias',entry=>{entry.command[1]='unrelated.mjs';},/intended managed local owner driver/],
    ['client alias',entry=>{entry.command[0]='/test/unrelated';},/pinned Node executable/],
    ['Host alias',entry=>{entry.command[3]='/test/unrelated';},/actual managed local owner Host/],
    ['Worker alias',entry=>{entry.command[5]='/test/unrelated';},/actual managed local owner Worker/],
    ['backend alias',entry=>{entry.command[7]='single-worker';},/actual managed local owner mode/],
    ['partial case selector',entry=>{entry.command.push('--case','steady');},/direct managed local owner command/],
    ['disabled',entry=>{entry.properties.push({name:'DISABLED',value:true});},/cannot weaken managed local owner/],
    ['inverted pass',entry=>{entry.properties.push({name:'WILL_FAIL',value:true});},/cannot weaken managed local owner/],
    ['skip allowance',entry=>{entry.properties.push({name:'SKIP_RETURN_CODE',value:77});},/cannot skip/],
    ['no timeout',entry=>{entry.properties=[];},/bounded managed local owner timeout/],
])test('managed local owner rejects '+label,()=>{
    const inventory=sample('Linux');mutate(inventory.tests.find(entry=>entry.name==='websocket_managed_local_owner'));
    assert.throws(()=>validatePlatformMatrix(inventory,'Linux',process.execPath),pattern);
});
test('managed local owner cannot disappear from the Linux required inventory',()=>{
    const inventory=sample('Linux');inventory.tests=inventory.tests.filter(entry=>entry.name!=='websocket_managed_local_owner');
    assert.throws(()=>validatePlatformMatrix(inventory,'Linux',process.execPath),/missing required managed local owner test/);
});
for(const system of ['Windows','Darwin'])test(system+' rejects Linux managed local owner registration',()=>{
    const inventory=sample(system);inventory.tests.push(sample('Linux').tests.find(entry=>entry.name==='websocket_managed_local_owner'));
    assert.throws(()=>validatePlatformMatrix(inventory,system,process.execPath),/Linux-only managed scenario registered/);
});
for(const [label,mutate,pattern] of [
    ['executable alias',entry=>{entry.command[0]='/test/unrelated';},/intended portable generation pool executable/],
    ['Worker alias',entry=>{entry.command[1]='/test/unrelated';},/actual portable generation pool Worker/],
    ['Linux-only selector',entry=>{entry.command[2]='--reclamation';},/actual portable generation pool selector/],
    ['extra selector',entry=>{entry.command.push('--retired-exit');},/direct portable generation pool command/],
    ['disabled',entry=>{entry.properties.push({name:'DISABLED',value:true});},/cannot weaken portable generation pool/],
    ['inverted pass',entry=>{entry.properties.push({name:'WILL_FAIL',value:true});},/cannot weaken portable generation pool/],
    ['skip allowance',entry=>{entry.properties.push({name:'SKIP_RETURN_CODE',value:77});},/cannot skip/],
    ['no timeout',entry=>{entry.properties=[];},/bounded portable generation pool timeout/],
])test('portable generation pool rejects '+label,()=>{
    const inventory=sample('Windows');mutate(inventory.tests.find(entry=>entry.name==='host_generation_pool_portable_contract'));
    assert.throws(()=>validatePlatformMatrix(inventory,'Windows',process.execPath),pattern);
});
for(const system of ['Linux','Windows','Darwin'])test(system+' portable generation pool cannot disappear',()=>{
    const inventory=sample(system);inventory.tests=inventory.tests.filter(entry=>entry.name!=='host_generation_pool_portable_contract');
    assert.throws(()=>validatePlatformMatrix(inventory,system,process.execPath),/missing required portable generation pool test/);
});
for(const system of ['Linux','Windows','Darwin'])for(const name of ['channel_owned_io',...(system==='Windows'?['channel_owned_io_debug']:[])]){
    test(system+' '+name+' cannot disappear',()=>{
        const inventory=sample(system);inventory.tests=inventory.tests.filter(entry=>entry.name!==name);
        assert.throws(()=>validatePlatformMatrix(inventory,system,process.execPath),/missing required channel I\/O profile/);
    });
    for(const [label,mutate,pattern] of [
        ['wrong executable',entry=>{entry.command[0]='/test/test-channel-io-alias';},/intended channel I\/O profile executable/],
        ['extra selector',entry=>{entry.command.push('--skip-faults');},/direct channel I\/O profile command/],
        ['disabled',entry=>{entry.properties.push({name:'DISABLED',value:true});},/cannot weaken/],
        ['inverted pass',entry=>{entry.properties.push({name:'WILL_FAIL',value:true});},/cannot weaken/],
        ['skip allowance',entry=>{entry.properties.push({name:'SKIP_RETURN_CODE',value:77});},/cannot skip/],
        ['no timeout',entry=>{entry.properties=[];},/bounded channel I\/O profile timeout/],
    ])test(system+' '+name+' rejects '+label,()=>{
        const inventory=sample(system);mutate(inventory.tests.find(entry=>entry.name===name));
        assert.throws(()=>validatePlatformMatrix(inventory,system,process.execPath),pattern);
    });
}
