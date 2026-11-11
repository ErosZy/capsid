// Check the generated CTest inventory, so a missing platform test cannot pass
// merely because its name disappeared at configure time. This is registration
// validation; successful application runs remain a separate requirement.
import assert from 'node:assert/strict';
import {execFileSync} from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import {fileURLToPath} from 'node:url';

const families = [
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
];

export function validatePlatformMatrix(inventory, system, node) {
    assert.ok(['Linux', 'Darwin', 'Windows'].includes(system), 'known platform');
    assert.equal(inventory.kind, 'ctestInfo', 'actual CTest JSON inventory');
    assert.ok(Array.isArray(inventory.tests) && inventory.tests.length > 0, 'nonempty test inventory');
    const byName = new Map();
    for (const test of inventory.tests) {
        assert.equal(typeof test.name, 'string');
        assert.ok(!byName.has(test.name), `duplicate CTest name: ${test.name}`);
        byName.set(test.name, test);
    }
    const modes = system === 'Linux' ? ['single-worker', 'static-pool', 'managed'] : ['single-worker', 'static-pool'];
    const normalize = value => {
        const result = path.resolve(value).replaceAll('\\', '/');
        return system === 'Windows' ? result.toLowerCase() : result;
    };
    const required = [];
    for (const [family, script, defaultName] of families) for (const mode of modes) {
        const name = defaultName && mode === 'single-worker' ? family : `${family}_${mode}`;
        const test = byName.get(name);
        assert.ok(test, `${system}: missing required test ${name}`);
        assert.ok(Array.isArray(test.command), `${name}: executable command`);
        assert.equal(normalize(test.command[0]), normalize(node), `${name}: pinned Node executable`);
        assert.equal(path.basename(test.command[1]), script, `${name}: intended driver`);
        const option = key => {
            const positions = test.command.flatMap((value, index) => value === key ? [index] : []);
            assert.ok(positions.length <= 1, `${name}: unique ${key}`);
            return positions.length ? test.command[positions[0] + 1] : undefined;
        };
        assert.equal(option('--mode') ?? 'single-worker', mode, `${name}: actual backend mode`);
        if (family.startsWith('websocket_async_'))
            assert.equal(option('--budgets'), family === 'websocket_async_budget' ? '1' : undefined, `${name}: exact async callback family`);
        if (['websocket_fetch_boundaries','websocket_fetch_transitions'].includes(family))
            assert.ok(option('--openssl') && path.isAbsolute(option('--openssl')) && !option('--openssl').endsWith('-NOTFOUND'), `${name}: explicit certificate tool`);
        for (const binary of ['host', 'worker'])
            assert.ok(option('--' + binary)?.length > 0, `${name}: actual ${binary} argument`);
        assert.ok(!test.command.includes('--reference-from-env'), `${name}: candidate cannot be a reference alias`);
        const properties = Object.fromEntries((test.properties ?? []).map(({name, value}) => [name, value]));
        for (const property of ['DISABLED', 'WILL_FAIL'])
            assert.ok(!properties[property], `${name}: ${property} cannot weaken the required test`);
        assert.ok(!Object.hasOwn(properties, 'SKIP_RETURN_CODE'), `${name}: required candidate cannot skip`);
        assert.ok(Number.isFinite(properties.TIMEOUT) && properties.TIMEOUT > 0 && properties.TIMEOUT <= 300,
            `${name}: bounded execution timeout`);
        required.push(name);
    }
    if (system === 'Linux' || system === 'Windows') {
        for (const [name, script, valuedFlag, optional] of [
            ['workerd_ws_reference', 'test_workerd_ws_fixtures.mjs', false, true],
            ['websocket_outbound_reference', 'test_websocket_outbound.mjs', false, true],
            ['websocket_interop_reference', 'test_websocket_interop.mjs', true, false],
            ['workerd_fanout_reference', 'test_workerd_fanout.mjs', true, false],
            ['websocket_frame_boundaries_reference', 'test_websocket_frame_boundaries.mjs', true, false],
            ['websocket_owner_lifecycle_reference', 'test_websocket_owner_lifecycle.mjs', true, false],
            ['websocket_async_callbacks_reference', 'test_websocket_async_callbacks.mjs', true, false],
            ['websocket_cross_context_reference', 'test_websocket_cross_context.mjs', true, false],
            ['websocket_fetch_resources_reference', 'test_websocket_fetch_resources.mjs', true, false],
            ['websocket_fetch_upgrade_reference', 'test_websocket_fetch_upgrade.mjs', true, false],
            ['websocket_fetch_upgrade_lifetime_reference', 'test_websocket_fetch_upgrade_lifetime.mjs', true, false],
            ['websocket_fetch_proxy_reference', 'test_websocket_fetch_proxy.mjs', true, false],
            ['websocket_fetch_boundaries_reference', 'test_websocket_fetch_boundaries.mjs', true, false],
            ['websocket_fetch_transitions_reference', 'test_websocket_fetch_transitions.mjs', true, false],
            ['websocket_crypto_reference', 'test_websocket_crypto.mjs', true, false],
        ]) {
            const test = byName.get(name);
            assert.ok(test, `${system}: missing required reference test ${name}`);
            assert.ok(Array.isArray(test.command), `${name}: executable command`);
            assert.equal(normalize(test.command[0]), normalize(node), `${name}: pinned Node executable`);
            assert.equal(path.basename(test.command[1]), script, `${name}: intended reference driver`);
            const positions = test.command.flatMap((value, index) => value === '--reference-from-env' ? [index] : []);
            assert.equal(positions.length, 1, `${name}: unique locked reference flag`);
            if (valuedFlag) assert.equal(test.command[positions[0] + 1], '1', `${name}: actual reference selector`);
            if (['websocket_fetch_boundaries_reference','websocket_fetch_transitions_reference'].includes(name)) {
                const tool = test.command.flatMap((value,index)=>value==='--openssl'?[test.command[index+1]]:[]);
                assert.equal(tool.length,1,`${name}: unique certificate executable`);
                assert.ok(tool[0] && path.isAbsolute(tool[0]) && !tool[0].endsWith('-NOTFOUND'), `${name}: explicit certificate tool`);
            }
            for (const flag of ['--host', '--worker', '--mode', '--workerd', '--budgets'])
                assert.ok(!test.command.includes(flag), `${name}: reference cannot be a backend or unlocked binary alias`);
            const properties = Object.fromEntries((test.properties ?? []).map(({name, value}) => [name, value]));
            assert.ok(!properties.DISABLED && !properties.WILL_FAIL, `${name}: cannot weaken reference test`);
            if (optional) assert.ok(properties.SKIP_RETURN_CODE === undefined || properties.SKIP_RETURN_CODE === 77,
                `${name}: only explicit unavailable-reference skip`);
            else assert.ok(!Object.hasOwn(properties, 'SKIP_RETURN_CODE'), `${name}: required reference cannot skip`);
            assert.ok(Number.isFinite(properties.TIMEOUT) && properties.TIMEOUT > 0 && properties.TIMEOUT <= 240,
                `${name}: bounded reference timeout`);
            required.push(name);
        }
    }
    {
        const name='host_generation_pool_portable_contract',entry=byName.get(name);
        assert.ok(entry,`${system}: missing required portable generation pool test`);
        assert.ok(Array.isArray(entry.command)&&entry.command.length===3,`${name}: direct portable generation pool command`);
        assert.match(path.basename(entry.command[0]),/^test-host-generation-pool(?:\.exe)?$/,`${name}: intended portable generation pool executable`);
        assert.match(path.basename(entry.command[1]),/^capsid-worker(?:\.exe)?$/,`${name}: actual portable generation pool Worker`);
        assert.equal(entry.command[2],'--portable',`${name}: actual portable generation pool selector`);
        const properties=Object.fromEntries((entry.properties??[]).map(({name,value})=>[name,value]));
        assert.ok(!properties.DISABLED&&!properties.WILL_FAIL,`${name}: cannot weaken portable generation pool test`);
        assert.ok(!Object.hasOwn(properties,'SKIP_RETURN_CODE'),`${name}: portable generation pool cannot skip`);
        assert.ok(Number.isFinite(properties.TIMEOUT)&&properties.TIMEOUT>0&&properties.TIMEOUT<=120,`${name}: bounded portable generation pool timeout`);
        required.push(name);
    }
    for(const [name,executable] of [['channel_owned_io','test-channel-io'],
        ...(system==='Windows'?[['channel_owned_io_debug','test-channel-io-debug']]:[])]){
        const entry=byName.get(name);assert.ok(entry,`${system}: missing required channel I/O profile ${name}`);
        assert.ok(Array.isArray(entry.command)&&entry.command.length===1,`${name}: direct channel I/O profile command`);
        assert.ok([executable,executable+'.exe'].includes(path.basename(entry.command[0])),`${name}: intended channel I/O profile executable`);
        const properties=Object.fromEntries((entry.properties??[]).map(({name,value})=>[name,value]));
        assert.ok(!properties.DISABLED&&!properties.WILL_FAIL,`${name}: cannot weaken channel I/O profile`);
        assert.ok(!Object.hasOwn(properties,'SKIP_RETURN_CODE'),`${name}: channel I/O profile cannot skip`);
        assert.ok(Number.isFinite(properties.TIMEOUT)&&properties.TIMEOUT>0&&properties.TIMEOUT<=15,`${name}: bounded channel I/O profile timeout`);
        required.push(name);
    }
    if (system === 'Linux') {
        const name='websocket_managed_local_owner',entry=byName.get(name);
        assert.ok(entry,`${system}: missing required managed local owner test`);
        assert.ok(Array.isArray(entry.command)&&entry.command.length===8,`${name}: direct managed local owner command`);
        assert.equal(normalize(entry.command[0]),normalize(node),`${name}: pinned Node executable`);
        assert.equal(path.basename(entry.command[1]),'test_websocket_managed_local_owner.mjs',`${name}: intended managed local owner driver`);
        const option=flag=>{const positions=entry.command.flatMap((value,index)=>value===flag?[index]:[]);assert.equal(positions.length,1,`${name}: unique ${flag}`);return entry.command[positions[0]+1];};
        assert.equal(option('--mode'),'managed',`${name}: actual managed local owner mode`);
        assert.match(path.basename(option('--host')),/^capsid-host(?:\.exe)?$/,`${name}: actual managed local owner Host`);
        assert.match(path.basename(option('--worker')),/^capsid-worker(?:\.exe)?$/,`${name}: actual managed local owner Worker`);
        const properties=Object.fromEntries((entry.properties??[]).map(({name,value})=>[name,value]));
        assert.ok(!properties.DISABLED&&!properties.WILL_FAIL,`${name}: cannot weaken managed local owner test`);
        assert.ok(!Object.hasOwn(properties,'SKIP_RETURN_CODE'),`${name}: managed local owner cannot skip`);
        assert.ok(Number.isFinite(properties.TIMEOUT)&&properties.TIMEOUT>0&&properties.TIMEOUT<=120,`${name}: bounded managed local owner timeout`);
        required.push(name);
    }
    if (system !== 'Linux') for (const name of byName.keys()) {
        assert.ok(!families.some(([family]) => name === `${family}_managed`) && !name.startsWith('websocket_managed_'),
            `${system}: Linux-only managed scenario registered: ${name}`);
    }
    for (const scenario of ['idle', 'exchange', 'tls-reset', 'reset-session', 'close-reset']) {
        const name = 'websocket_python_peer_' + scenario, test = byName.get(name);
        assert.ok(test, `${system}: missing required Python peer test ${name}`);
        assert.ok(Array.isArray(test.command), `${name}: executable command`);
        assert.equal(normalize(test.command[0]), normalize(node), `${name}: pinned Node executable`);
        assert.equal(path.basename(test.command[1]), 'test_websocket_python_peer_lifecycle.mjs', `${name}: intended peer driver`);
        const positions = test.command.flatMap((value, index) => value === '--case' ? [index] : []);
        assert.equal(positions.length, 1, `${name}: unique peer scenario`);
        assert.equal(test.command[positions[0] + 1], scenario, `${name}: actual peer scenario`);
        const properties = Object.fromEntries((test.properties ?? []).map(({name, value}) => [name, value]));
        assert.ok(!properties.DISABLED && !properties.WILL_FAIL, `${name}: cannot weaken peer lifecycle test`);
        assert.ok(!Object.hasOwn(properties, 'SKIP_RETURN_CODE'), `${name}: required peer test cannot skip`);
        assert.ok(Number.isFinite(properties.TIMEOUT) && properties.TIMEOUT > 0 && properties.TIMEOUT <= 120,
            `${name}: bounded peer timeout`);
        required.push(name);
    }
    for (const mode of ['single-worker', 'static-pool']) for (const scenario of [
        'idle', 'parent-eof', 'repeat-stop', 'active-ws', 'no-close-reply', 'pending-dial',
        'startup-fail', 'invalid-fd', 'ready-alias', 'stdio-fd', 'malformed-fd', 'file-fd',
        ...(system === 'Windows' ? [] : ['signal']),
    ]) {
        const name = `host_parent_stop_${mode}_${scenario}`, test = byName.get(name);
        assert.ok(test, `${system}: missing required parent stop test ${name}`);
        assert.ok(Array.isArray(test.command), `${name}: executable command`);
        assert.equal(normalize(test.command[0]), normalize(node), `${name}: pinned Node executable`);
        assert.equal(path.basename(test.command[1]), 'test_host_parent_stop.mjs', `${name}: intended stop driver`);
        const option = key => {
            const positions = test.command.flatMap((value, index) => value === key ? [index] : []);
            assert.ok(positions.length <= 1, `${name}: unique ${key}`);
            return positions.length ? test.command[positions[0] + 1] : undefined;
        };
        assert.equal(option('--case'), scenario, `${name}: actual stop scenario`);
        assert.equal(option('--mode'), mode, `${name}: actual stop mode`);
        for (const binary of ['host', 'worker'])
            assert.ok(option('--' + binary)?.length > 0, `${name}: actual ${binary} argument`);
        const properties = Object.fromEntries((test.properties ?? []).map(({name, value}) => [name, value]));
        assert.ok(!properties.DISABLED && !properties.WILL_FAIL, `${name}: cannot weaken stop test`);
        assert.ok(!Object.hasOwn(properties, 'SKIP_RETURN_CODE'), `${name}: required stop test cannot skip`);
        assert.ok(Number.isFinite(properties.TIMEOUT) && properties.TIMEOUT > 0 && properties.TIMEOUT <= 120,
            `${name}: bounded stop timeout`);
        required.push(name);
    }
    for (const scenario of ['before-completion', 'before-adoption', 'cancel-pending', 'queued-before-exit',
        'idle-after-adoption', 'keepalive-after-adoption', 'pre-post-worker-exit', 'pre-post-stop', 'pre-post-drain']) {
        const name = `host_external_accept_exit_${scenario}`, test = byName.get(name);
        assert.ok(test, `${system}: missing required external accept test ${name}`);
        assert.ok(Array.isArray(test.command), `${name}: executable command`);
        assert.equal(normalize(test.command[0]), normalize(node), `${name}: pinned Node executable`);
        assert.equal(path.basename(test.command[1]), 'test_host_external_accept_exit.mjs', `${name}: intended accept driver`);
        const option = key => {
            const positions = test.command.flatMap((value, index) => value === key ? [index] : []);
            assert.equal(positions.length, 1, `${name}: unique required ${key}`);
            return test.command[positions[0] + 1];
        };
        assert.equal(option('--case'), scenario, `${name}: actual accept scenario`);
        assert.match(path.basename(option('--fixture') ?? ''), /^test-host-single-worker-lifecycle(?:\.exe)?$/,
            `${name}: intended native accept fixture`);
        assert.match(path.basename(option('--worker') ?? ''), /^capsid-worker(?:\.exe)?$/,
            `${name}: actual Worker executable`);
        const properties = Object.fromEntries((test.properties ?? []).map(({name, value}) => [name, value]));
        assert.ok(!properties.DISABLED && !properties.WILL_FAIL, `${name}: cannot weaken accept test`);
        assert.ok(!Object.hasOwn(properties, 'SKIP_RETURN_CODE'), `${name}: required accept test cannot skip`);
        assert.ok(Number.isFinite(properties.TIMEOUT) && properties.TIMEOUT > 0 && properties.TIMEOUT <= 60,
            `${name}: bounded accept timeout`);
        required.push(name);
    }
    for (const scenario of ['first-worker-exit', 'last-worker-exit', 'first-worker-exit-websocket',
        'all-workers-exit', 'last-ready-worker-exit', 'all-workers-exit-websocket']) {
        const name = `host_static_pool_fault_${scenario}`, test = byName.get(name);
        assert.ok(test, `${system}: missing required pool fault test ${name}`);
        assert.ok(Array.isArray(test.command), `${name}: executable command`);
        assert.equal(normalize(test.command[0]), normalize(node), `${name}: pinned Node executable`);
        assert.equal(path.basename(test.command[1]), 'test_host_static_pool_faults.mjs', `${name}: intended pool fault driver`);
        const option = key => {
            const positions = test.command.flatMap((value, index) => value === key ? [index] : []);
            assert.equal(positions.length, 1, `${name}: unique required ${key}`);
            return test.command[positions[0] + 1];
        };
        assert.equal(option('--case'), scenario, `${name}: actual pool fault scenario`);
        assert.match(path.basename(option('--fixture') ?? ''), /^test-host-static-pool-faults(?:\.exe)?$/,
            `${name}: intended native pool fixture`);
        assert.match(path.basename(option('--worker') ?? ''), /^capsid-worker(?:\.exe)?$/,
            `${name}: actual Worker executable`);
        const properties = Object.fromEntries((test.properties ?? []).map(({name, value}) => [name, value]));
        assert.ok(!properties.DISABLED && !properties.WILL_FAIL, `${name}: cannot weaken pool fault test`);
        assert.ok(!Object.hasOwn(properties, 'SKIP_RETURN_CODE'), `${name}: required pool fault test cannot skip`);
        assert.ok(Number.isFinite(properties.TIMEOUT) && properties.TIMEOUT > 0 && properties.TIMEOUT <= 60,
            `${name}: bounded pool fault timeout`);
        required.push(name);
    }
    for (const scenario of ['reselect-worker-exit', 'reselect-last-workers-exit', 'pre-post-stop',
        'pre-post-drain', 'active-drain']) {
        const name = `host_static_pool_adoption_${scenario}`, test = byName.get(name);
        assert.ok(test, `${system}: missing required pool adoption test ${name}`);
        assert.ok(Array.isArray(test.command), `${name}: executable command`);
        assert.equal(normalize(test.command[0]), normalize(node), `${name}: pinned Node executable`);
        assert.equal(path.basename(test.command[1]), 'test_host_static_pool_adoption.mjs', `${name}: intended pool adoption driver`);
        const option = key => {
            const positions = test.command.flatMap((value, index) => value === key ? [index] : []);
            assert.equal(positions.length, 1, `${name}: unique required ${key}`);
            return test.command[positions[0] + 1];
        };
        assert.equal(option('--case'), scenario, `${name}: actual pool adoption scenario`);
        assert.match(path.basename(option('--fixture') ?? ''), /^test-host-static-pool-adoption(?:\.exe)?$/,
            `${name}: intended native pool adoption fixture`);
        assert.match(path.basename(option('--worker') ?? ''), /^capsid-worker(?:\.exe)?$/,
            `${name}: actual Worker executable`);
        const properties = Object.fromEntries((test.properties ?? []).map(({name, value}) => [name, value]));
        assert.ok(!properties.DISABLED && !properties.WILL_FAIL, `${name}: cannot weaken pool adoption test`);
        assert.ok(!Object.hasOwn(properties, 'SKIP_RETURN_CODE'), `${name}: required pool adoption test cannot skip`);
        assert.ok(Number.isFinite(properties.TIMEOUT) && properties.TIMEOUT > 0 && properties.TIMEOUT <= 60,
            `${name}: bounded pool adoption timeout`);
        required.push(name);
    }
    for (const scenario of ['drain-before-submit', 'drain-during-prepare', 'queued-before-drain',
        'owner-drain-before-submit', 'healthy-control']) {
        const name = `host_external_drain_${scenario}`, test = byName.get(name);
        assert.ok(test, `${system}: missing required external drain test ${name}`);
        assert.ok(Array.isArray(test.command), `${name}: executable command`);
        assert.equal(normalize(test.command[0]), normalize(node), `${name}: pinned Node executable`);
        assert.equal(path.basename(test.command[1]), 'test_host_external_drain.mjs', `${name}: intended external drain driver`);
        const option = key => {
            const positions = test.command.flatMap((value, index) => value === key ? [index] : []);
            assert.equal(positions.length, 1, `${name}: unique required ${key}`);
            return test.command[positions[0] + 1];
        };
        assert.equal(option('--case'), scenario, `${name}: actual external drain scenario`);
        assert.match(path.basename(option('--fixture') ?? ''), /^test-host-external-drain(?:\.exe)?$/,
            `${name}: intended native external drain fixture`);
        assert.match(path.basename(option('--worker') ?? ''), /^capsid-worker(?:\.exe)?$/,
            `${name}: actual Worker executable`);
        const properties = Object.fromEntries((test.properties ?? []).map(({name, value}) => [name, value]));
        assert.ok(!properties.DISABLED && !properties.WILL_FAIL, `${name}: cannot weaken external drain test`);
        assert.ok(!Object.hasOwn(properties, 'SKIP_RETURN_CODE'), `${name}: required external drain test cannot skip`);
        assert.ok(Number.isFinite(properties.TIMEOUT) && properties.TIMEOUT > 0 && properties.TIMEOUT <= 60,
            `${name}: bounded external drain timeout`);
        required.push(name);
    }
    for (const scenario of ['stop', 'drain']) {
        const name = `host_static_pool_local_shutdown_${scenario}`, test = byName.get(name);
        assert.ok(test, `${system}: missing required local pool shutdown test ${name}`);
        assert.ok(Array.isArray(test.command), `${name}: executable command`);
        assert.equal(normalize(test.command[0]), normalize(node), `${name}: pinned Node executable`);
        assert.equal(path.basename(test.command[1]), 'test_host_static_pool_local_shutdown.mjs', `${name}: intended local pool shutdown driver`);
        const option = key => {
            const positions = test.command.flatMap((value, index) => value === key ? [index] : []);
            assert.equal(positions.length, 1, `${name}: unique required ${key}`);
            return test.command[positions[0] + 1];
        };
        assert.equal(option('--case'), scenario, `${name}: actual local pool shutdown scenario`);
        assert.match(path.basename(option('--fixture') ?? ''), /^test-host-static-pool-local-shutdown(?:\.exe)?$/,
            `${name}: intended native local pool shutdown fixture`);
        assert.match(path.basename(option('--worker') ?? ''), /^capsid-worker(?:\.exe)?$/,
            `${name}: actual Worker executable`);
        const properties = Object.fromEntries((test.properties ?? []).map(({name, value}) => [name, value]));
        assert.ok(!properties.DISABLED && !properties.WILL_FAIL, `${name}: cannot weaken local pool shutdown test`);
        assert.ok(!Object.hasOwn(properties, 'SKIP_RETURN_CODE'), `${name}: required local pool shutdown test cannot skip`);
        assert.ok(Number.isFinite(properties.TIMEOUT) && properties.TIMEOUT > 0 && properties.TIMEOUT <= 60,
            `${name}: bounded local pool shutdown timeout`);
        required.push(name);
    }
    for (const scenario of ['get-close-before-completion', 'head-close-before-completion', 'get-normal-completion']) {
        const name = `host_unavailable_lifecycle_${scenario}`, test = byName.get(name);
        assert.ok(test, `${system}: missing required unavailable lifecycle test ${name}`);
        assert.ok(test.command?.length === 2, `${name}: direct native lifecycle command`);
        assert.match(path.basename(test.command[0]), /^test-host-unavailable-lifecycle(?:\.exe)?$/,
            `${name}: intended unavailable lifecycle executable`);
        assert.equal(test.command[1], scenario, `${name}: actual unavailable lifecycle scenario`);
        const properties = Object.fromEntries((test.properties ?? []).map(({name, value}) => [name, value]));
        assert.ok(!properties.DISABLED && !properties.WILL_FAIL, `${name}: cannot weaken unavailable lifecycle test`);
        assert.ok(!Object.hasOwn(properties, 'SKIP_RETURN_CODE'), `${name}: required unavailable lifecycle test cannot skip`);
        assert.ok(Number.isFinite(properties.TIMEOUT) && properties.TIMEOUT > 0 && properties.TIMEOUT <= 20,
            `${name}: bounded unavailable lifecycle timeout`);
        required.push(name);
    }
    for (const [name, scenario] of [
        ['host_write_timeout_cancels', 'write-timeout-cancels'],
        ['host_worker_deadline_independent', 'worker-deadline-independent'],
        ['host_fast_response_untouched', 'fast-response-untouched'],
        ['host_write_timeout_disabled', 'write-timeout-disabled'],
        ['host_pool_write_timeout_cancels', 'pool-write-timeout-cancels'],
        ['host_pool_drain_write_deadline', 'pool-drain-write-deadline'],
        ['host_pool_stop_pending_write', 'pool-stop-pending-write'],
    ]) {
        const test = byName.get(name);
        assert.ok(test, `${system}: missing required write deadline test ${name}`);
        assert.ok(test.command?.length === 3, `${name}: direct write deadline command`);
        assert.match(path.basename(test.command[0]), /^test-host-write-timeout(?:\.exe)?$/,
            `${name}: intended write deadline executable`);
        assert.equal(test.command[1], scenario, `${name}: actual write deadline scenario`);
        assert.match(path.basename(test.command[2]), /^capsid-worker(?:\.exe)?$/,
            `${name}: actual write deadline Worker`);
        const properties = Object.fromEntries((test.properties ?? []).map(({name, value}) => [name, value]));
        assert.ok(!properties.DISABLED && !properties.WILL_FAIL, `${name}: cannot weaken write deadline test`);
        assert.ok(!Object.hasOwn(properties, 'SKIP_RETURN_CODE'), `${name}: required write deadline test cannot skip`);
        assert.ok(Number.isFinite(properties.TIMEOUT) && properties.TIMEOUT > 0 && properties.TIMEOUT <= 30,
            `${name}: bounded write deadline timeout`);
        required.push(name);
    }
    for (const [name, args] of [['tjs_fetch_abort_pending_handshake', []],
        ['tjs_fetch_context_cancel_pending_handshake', ['--context-cancel']],
        ['tjs_fetch_context_cancel_dns', ['--dns-context-cancel']],
        ['tjs_fetch_policy_async_context', ['--policy-async-context']],
        ['tjs_fetch_policy_binding_context', ['--policy-binding-context']],
        ['tjs_fetch_policy_unowned_context', ['--policy-unowned-context']]]) {
        const test = byName.get(name);
        assert.ok(test, `${system}: missing required fetch abort test`);
        assert.ok(test.command?.length === 1 + args.length, `${name}: direct native regression command`);
        assert.deepEqual(test.command.slice(1), args, `${name}: exact native regression scenario`);
        assert.match(path.basename(test.command[0]), /^test-tjs-fetch-abort(?:\.exe)?$/,
            `${name}: intended native fetch abort executable`);
        const properties = Object.fromEntries((test.properties ?? []).map(({name, value}) => [name, value]));
        assert.ok(!properties.DISABLED && !properties.WILL_FAIL, `${name}: cannot weaken fetch abort test`);
        assert.ok(!Object.hasOwn(properties, 'SKIP_RETURN_CODE'), `${name}: required fetch abort test cannot skip`);
        assert.ok(Number.isFinite(properties.TIMEOUT) && properties.TIMEOUT > 0 && properties.TIMEOUT <= 30,
            `${name}: bounded fetch abort timeout`);
        required.push(name);
    }
    for (const [name, args] of [['host_dns_dialer_lifecycle', []], ['host_dns_reserved_invalid', ['--reserved-invalid']]]) {
        const entry = byName.get(name);
        assert.ok(entry, `${system}: missing required native DNS test ${name}`);
        assert.equal(entry.command?.length, 1 + args.length, `${name}: direct native DNS command`);
        assert.deepEqual(entry.command.slice(1), args, `${name}: exact native DNS scenario`);
        assert.match(path.basename(entry.command[0]), /^test-host-dns-dialer(?:\.exe)?$/, `${name}: intended DNS executable`);
        const properties = Object.fromEntries((entry.properties ?? []).map(({name, value}) => [name, value]));
        assert.ok(!properties.DISABLED && !properties.WILL_FAIL, `${name}: cannot weaken DNS test`);
        assert.ok(!Object.hasOwn(properties, 'SKIP_RETURN_CODE'), `${name}: required DNS test cannot skip`);
        assert.ok(Number.isFinite(properties.TIMEOUT) && properties.TIMEOUT > 0 && properties.TIMEOUT <= 30, `${name}: bounded DNS timeout`);
        required.push(name);
    }
    if (system === 'Windows') {
        const name = 'worker_windows_environment', test = byName.get(name);
        assert.ok(test, 'Windows: required OS environment startup regression is registered');
        assert.match(path.basename(test.command?.[0] ?? ''), /^test-windows-worker-environment(?:\.exe)?$/,
            `${name}: intended native executable`);
        assert.match(path.basename(test.command?.[1] ?? ''), /^capsid-worker(?:\.exe)?$/,
            `${name}: actual Worker executable`);
        const properties = Object.fromEntries((test.properties ?? []).map(({name, value}) => [name, value]));
        assert.ok(!properties.DISABLED && !properties.WILL_FAIL && !Object.hasOwn(properties, 'SKIP_RETURN_CODE'),
            `${name}: required native startup cannot be weakened`);
        assert.ok(Number.isFinite(properties.TIMEOUT) && properties.TIMEOUT > 0 && properties.TIMEOUT <= 30,
            `${name}: bounded startup deadline`);
        required.push(name);
    }
    return {complete: true, scope: 'CTest registration only; no application execution claimed', system, modes, required};
}

export function readCtestInventory(ctest, buildDirectory) {
    const build = path.resolve(buildDirectory);
    assert.ok(fs.existsSync(path.join(build, 'CTestTestfile.cmake')), 'generated CTest registration exists');
    // Even --show-only writes Testing/Temporary/LastTest.log. Reading the active
    // build here would replace the outer CTest runner's log and its temporary
    // file. Copy only generated registration files to an owned scratch tree;
    // subdirs and commands retain the registrations produced by this build.
    const scratch = fs.mkdtempSync(path.join(os.tmpdir(), 'capsid-ctest-registration-'));
    assert.ok(path.isAbsolute(scratch) && path.basename(scratch).startsWith('capsid-ctest-registration-'));
    try {
        assert.ok(!scratch.startsWith(build + path.sep), 'scratch registration must be outside the build');
        const registration = path.join(scratch, 'registration');
        fs.cpSync(build, registration, {recursive: true, filter: entry => {
            const stat = fs.lstatSync(entry);
            return stat.isDirectory() || (stat.isFile() && path.basename(entry) === 'CTestTestfile.cmake');
        }});
        const raw = execFileSync(ctest, ['--test-dir', registration, '--show-only=json-v1'],
            {encoding: 'utf8', timeout: 30000, maxBuffer: 8 * 1024 * 1024, windowsHide: true});
        return JSON.parse(raw);
    } finally {
        fs.rmSync(scratch, {recursive: true, force: true});
    }
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
    const args = new Map();
    for (let index = 2; index < process.argv.length; index += 2) {
        assert.ok(process.argv[index]?.startsWith('--') && process.argv[index + 1], 'complete argument pair');
        args.set(process.argv[index], process.argv[index + 1]);
    }
    for (const key of ['--ctest', '--build-dir', '--system']) assert.ok(args.get(key), `required ${key}`);
    console.log(JSON.stringify(validatePlatformMatrix(readCtestInventory(args.get('--ctest'), args.get('--build-dir')),
        args.get('--system'), process.execPath)));
}
