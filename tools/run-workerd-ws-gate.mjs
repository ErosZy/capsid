import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { execFileSync } from 'node:child_process';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { randomUUID } from 'node:crypto';
import { root, readLock, verifyBinary, verifyClient, fileDigest, digest, compareReports, fixtureIdentities, compareOutboundReports } from './workerd-reference.mjs';
import { prepareMutationRunner, validateMutationReport, requiredMutationCases } from './workerd-mutation-contract.mjs';
import { compareInteropReports, interopInputPaths } from './workerd-interop-contract.mjs';
import { compareFanoutReports, fanoutInputPaths } from './workerd-fanout-contract.mjs';
import { compareFrameReports, frameInputPaths } from './workerd-frame-contract.mjs';
import { compareOwnerReports, ownerInputPaths } from './workerd-owner-contract.mjs';
import { compareAsyncReports, asyncInputPaths, asyncSourceNames } from './workerd-async-callback-contract.mjs';
import { compareReaderIntrinsicReports, readerIntrinsicInputPaths, readerIntrinsicSourceNames } from './workerd-reader-intrinsic-contract.mjs';
import { compareNativeReaderLifecycleReports, nativeReaderLifecycleInputPaths } from './workerd-native-reader-lifecycle-contract.mjs';
import { compareReaderScopeReports, readerScopeInputPaths } from './workerd-reader-scope-contract.mjs';
import { compareStandardReaderShardReports, standardReaderShardNames, standardReaderInputPaths } from './workerd-standard-reader-contract.mjs';
import { compareCrossContextReports, crossContextInputPaths } from './workerd-cross-context-contract.mjs';
import { compareFetchResourceReports, fetchResourceInputPaths } from './workerd-fetch-resource-contract.mjs';
import {compareOrdinaryFetchShardReports, ordinaryFetchShards, ordinaryFetchInputPaths} from './workerd-ordinary-fetch-contract.mjs';
import {compareHttpCodecReports, httpCodecShards, httpCodecInputPaths} from './workerd-http-codec-contract.mjs';
import { compareFetchUpgradeReports, fetchUpgradeInputPaths } from './workerd-fetch-upgrade-contract.mjs';
import { compareFetchUpgradeLifetimeReports, fetchUpgradeLifetimeInputPaths } from './workerd-fetch-upgrade-lifetime-contract.mjs';
import { compareFetchProxyReports, fetchProxyInputPaths } from './workerd-fetch-proxy-contract.mjs';
import { compareFetchBoundaryReports, fetchBoundaryInputPaths } from './workerd-fetch-boundary-contract.mjs';
import { compareFetchTransitionReports, fetchTransitionInputPaths } from './workerd-fetch-transition-contract.mjs';
import { compareFetchBYOBShardReports, fetchBYOBShardNames, fetchBYOBInputPaths } from './workerd-fetch-byob-contract.mjs';
import { compareCryptoReports, cryptoInputPaths } from './workerd-crypto-contract.mjs';
import { referencePlatform, referenceBackendModes } from './workerd-platform.mjs';
import {messagePortInputPaths, assertMessagePortReport} from './message-port-order-contract.mjs';

export const requiredTests = Object.freeze(['workerd_ws_fixtures', 'workerd_ws_fixtures_static-pool',
    'native_message_port_order_single-worker', 'native_message_port_order_static-pool', 'native_message_port_order_managed',
    'workerd_ws_fixtures_managed', 'workerd_ws_reference', 'websocket_outbound_reference',
    'websocket_outbound_single-worker', 'websocket_outbound_static-pool', 'websocket_outbound_managed',
    'workerd_ws_gate_controls', 'workerd_ws_gate_archive_controls', 'workerd_ws_gate_extension_controls', 'workerd_ws_gate_support_controls', 'workerd_ws_mutation_controls', 'workerd_throw_contract_controls',
    'workerd_handler_error_contract_controls',
    'workerd_owner_contract_controls', 'websocket_owner_lifecycle_reference',
    'websocket_owner_lifecycle_single-worker', 'websocket_owner_lifecycle_static-pool', 'websocket_owner_lifecycle_managed',
    'workerd_async_callback_contract_controls', 'websocket_async_callbacks_reference',
    'websocket_async_callbacks_single-worker', 'websocket_async_callbacks_static-pool', 'websocket_async_callbacks_managed',
    'websocket_async_budget_single-worker', 'websocket_async_budget_static-pool', 'websocket_async_budget_managed',
    'workerd_reader_intrinsic_contract_controls', 'websocket_reader_intrinsics_reference',
    'websocket_reader_intrinsics_single-worker', 'websocket_reader_intrinsics_static-pool', 'websocket_reader_intrinsics_managed',
    'workerd_native_reader_lifecycle_contract_controls', 'websocket_native_reader_lifecycle_reference',
    'workerd_reader_scope_contract_controls', 'websocket_reader_scope_reference',
    'websocket_reader_scope_single-worker', 'websocket_reader_scope_static-pool', 'websocket_reader_scope_managed', 'workerd_standard_reader_contract_controls', 'websocket_standard_readers_shapes_reference', 'websocket_standard_readers_ownership_reference', 'websocket_standard_readers_transfers_reference',
    'websocket_native_reader_lifecycle_single-worker', 'websocket_native_reader_lifecycle_static-pool', 'websocket_native_reader_lifecycle_managed',
    'websocket_standard_readers_shapes_single-worker', 'websocket_standard_readers_ownership_single-worker', 'websocket_standard_readers_transfers_single-worker', 'websocket_standard_readers_shapes_static-pool', 'websocket_standard_readers_ownership_static-pool', 'websocket_standard_readers_transfers_static-pool', 'websocket_standard_readers_shapes_managed', 'websocket_standard_readers_ownership_managed', 'websocket_standard_readers_transfers_managed',
    'workerd_fetch_resource_contract_controls', 'websocket_fetch_resources_reference',
    'websocket_fetch_resources_single-worker', 'websocket_fetch_resources_static-pool', 'websocket_fetch_resources_managed',
    'workerd_ordinary_fetch_contract_controls',
    ...ordinaryFetchShards.flatMap(shard => ['reference', 'single-worker', 'static-pool', 'managed']
        .map(mode => 'websocket_ordinary_fetch_' + shard + '_' + mode)),
    'http_gzip_parser', 'workerd_http_codec_contract_controls',
    ...httpCodecShards.flatMap(shard => ['reference', 'single-worker', 'static-pool', 'managed']
        .map(mode => 'websocket_http_codec_' + shard + '_' + mode)),
    'workerd_fetch_upgrade_contract_controls', 'websocket_fetch_upgrade_reference',
    'websocket_fetch_upgrade_single-worker', 'websocket_fetch_upgrade_static-pool', 'websocket_fetch_upgrade_managed',
    'workerd_fetch_upgrade_lifetime_contract_controls', 'websocket_fetch_upgrade_lifetime_reference',
    'websocket_fetch_upgrade_lifetime_single-worker', 'websocket_fetch_upgrade_lifetime_static-pool', 'websocket_fetch_upgrade_lifetime_managed',
    'workerd_fetch_proxy_contract_controls', 'websocket_fetch_proxy_reference',
    'websocket_fetch_proxy_single-worker', 'websocket_fetch_proxy_static-pool', 'websocket_fetch_proxy_managed',
    'workerd_fetch_boundary_contract_controls', 'websocket_fetch_boundaries_reference',
    'websocket_fetch_boundaries_single-worker', 'websocket_fetch_boundaries_static-pool', 'websocket_fetch_boundaries_managed',
    'workerd_fetch_transition_contract_controls', 'websocket_fetch_transitions_reference',
    'websocket_fetch_transitions_single-worker', 'websocket_fetch_transitions_static-pool', 'websocket_fetch_transitions_managed',
    'workerd_fetch_byob_contract_controls',
    'websocket_fetch_byob_readers_reference', 'websocket_fetch_byob_readers_single-worker', 'websocket_fetch_byob_readers_static-pool', 'websocket_fetch_byob_readers_managed',
    'websocket_fetch_byob_constructors_reference', 'websocket_fetch_byob_constructors_single-worker', 'websocket_fetch_byob_constructors_static-pool', 'websocket_fetch_byob_constructors_managed',
    'websocket_fetch_byob_reflect-standard_reference', 'websocket_fetch_byob_reflect-standard_single-worker', 'websocket_fetch_byob_reflect-standard_static-pool', 'websocket_fetch_byob_reflect-standard_managed',
    'websocket_fetch_byob_views-brands_reference', 'websocket_fetch_byob_views-brands_single-worker', 'websocket_fetch_byob_views-brands_static-pool', 'websocket_fetch_byob_views-brands_managed',
    'websocket_fetch_byob_bodies_reference', 'websocket_fetch_byob_bodies_single-worker', 'websocket_fetch_byob_bodies_static-pool', 'websocket_fetch_byob_bodies_managed',
    'websocket_fetch_byob_standard-edges_reference', 'websocket_fetch_byob_standard-edges_single-worker', 'websocket_fetch_byob_standard-edges_static-pool', 'websocket_fetch_byob_standard-edges_managed',
    'websocket_fetch_byob_standard-close_reference', 'websocket_fetch_byob_standard-close_single-worker', 'websocket_fetch_byob_standard-close_static-pool', 'websocket_fetch_byob_standard-close_managed',
    'workerd_crypto_contract_controls', 'websocket_crypto_reference',
    'websocket_crypto_single-worker', 'websocket_crypto_static-pool', 'websocket_crypto_managed',
    'workerd_cross_context_contract_controls', 'websocket_cross_context_reference',
    'websocket_cross_context_single-worker', 'websocket_cross_context_static-pool', 'websocket_cross_context_managed',
    'websocket_interop_reference','websocket_interop_single-worker','websocket_interop_static-pool','websocket_interop_managed',
    'workerd_fanout_reference','workerd_fanout_single-worker','workerd_fanout_static-pool','workerd_fanout_managed',
    'websocket_frame_boundaries_reference','websocket_frame_boundaries_single-worker',
    'websocket_frame_boundaries_static-pool','websocket_frame_boundaries_managed']);

export function requiredTestsFor(lock) {
    return referencePlatform(lock) === 'linux'
        ? [...requiredTests] : requiredTests.filter(name => !name.endsWith('_managed'));
}

export function validateGateRun(run, expected) {
    const junit = fs.readFileSync(path.join(run, 'junit.xml'), 'utf8');
    assert.ok(!/<(?:failure|skipped|error)\b/.test(junit), 'required matrix has no failures or skips');
    const actual = [...junit.matchAll(/<testcase\s+name="([^"]+)"/g)].map(x => x[1]).sort();
    assert.deepEqual(actual, requiredTestsFor(expected.lock).sort(), 'required platform CTests actually executed');
    const read = name => JSON.parse(fs.readFileSync(path.join(run, name + '.json'), 'utf8'));
    const modes = referenceBackendModes(expected.lock);
    const candidates = modes.map(read);
    const report = compareReports(read('reference'), candidates, expected);
    report.messagePorts = modes.map(mode => assertMessagePortReport(read('message-port-order-' + mode), {
        mode, runId: expected.runId, inputs: expected.messagePortInputs,
        binaries: {host: expected.host, worker: expected.worker,
            reference: expected.lock.binary_sha256, node: expected.messagePortNode}, lock: expected.lock,
    }));
    report.outbound = compareOutboundReports(read('outbound-reference'),
        modes.map(mode=>read('outbound-'+mode)),
        {...expected, runner:expected.outboundRunner});
    report.mutations = validateMutationReport(read('mutation-controls'), expected,
        candidates[0].results.find(x=>x.fixture === 'api-compat.mjs').observation);
    report.interop = compareInteropReports(read('interop-reference'),
        modes.map(mode=>read('interop-'+mode)),
        {...expected,inputs:expected.interopInputs,clientLock:expected.interopClientLock});
    report.fanout = compareFanoutReports(read('fanout-reference'),
        modes.map(mode=>read('fanout-'+mode)),
        {...expected,inputs:expected.fanoutInputs,clientLock:expected.interopClientLock});
    report.frames = compareFrameReports(read('frame-boundaries-reference'),
        modes.map(mode=>read('frame-boundaries-'+mode)),
        {...expected,inputs:expected.frameInputs});
    report.owners = compareOwnerReports(read('owner-lifecycle-reference'),
        modes.map(mode=>read('owner-lifecycle-'+mode)),
        {...expected,inputs:expected.ownerInputs});
    report.asyncCallbacks = compareAsyncReports(read('async-shared-reference'),
        modes.map(mode=>read('async-shared-'+mode)), modes.map(mode=>read('async-budget-'+mode)),
        {...expected,inputs:expected.asyncInputs,sourceNames:expected.asyncSourceNames});
    report.crossContext = compareCrossContextReports(read('cross-context-reference'),
        modes.map(mode => read('cross-context-' + mode)), {...expected, inputs: expected.crossContextInputs});
    report.readerIntrinsics = compareReaderIntrinsicReports(read('reader-intrinsics-shared-reference'),
        modes.map(mode => read('reader-intrinsics-shared-' + mode)),
        {...expected, inputs: expected.readerIntrinsicInputs, sourceNames: expected.readerIntrinsicSourceNames});
    report.nativeReaderLifecycle = compareNativeReaderLifecycleReports(read('native-reader-lifecycle-reference'),
        modes.map(mode => read('native-reader-lifecycle-' + mode)), {...expected, inputs: expected.nativeReaderLifecycleInputs});
    report.readerScope = compareReaderScopeReports(read('reader-scope-reference'),
        modes.map(mode=>read('reader-scope-'+mode)), {...expected, inputs:expected.readerScopeInputs});
    report.standardReaders = compareStandardReaderShardReports(standardReaderShardNames.map(shard=>read('standard-readers-'+shard+'-reference')),
        modes.flatMap(mode=>standardReaderShardNames.map(shard=>read('standard-readers-'+shard+'-'+mode))), {...expected, inputs: expected.standardReaderInputs});
    report.fetchResources = compareFetchResourceReports(read('fetch-resources-reference'),
        modes.map(mode => read('fetch-resources-' + mode)), {...expected, inputs: expected.fetchResourceInputs});
    report.ordinaryFetch = compareOrdinaryFetchShardReports(ordinaryFetchShards.map(shard => read('ordinary-fetch-' + shard + '-reference')),
        modes.map(mode => ({mode, reports: ordinaryFetchShards.map(shard => read('ordinary-fetch-' + shard + '-' + mode))})),
        {...expected, inputs: expected.ordinaryFetchInputs});
    report.fetchUpgrade = compareFetchUpgradeReports(read('fetch-upgrade-reference'),
        modes.map(mode => read('fetch-upgrade-' + mode)), {...expected, inputs: expected.fetchUpgradeInputs});
    report.fetchUpgradeLifetime = compareFetchUpgradeLifetimeReports(read('fetch-upgrade-lifetime-reference'),
        modes.map(mode => read('fetch-upgrade-lifetime-' + mode)), {...expected, inputs: expected.fetchUpgradeLifetimeInputs});
    report.fetchProxy = compareFetchProxyReports(read('fetch-proxy-reference'),
        modes.map(mode => read('fetch-proxy-' + mode)), {...expected, inputs: expected.fetchProxyInputs});
    report.fetchBoundaries = compareFetchBoundaryReports(read('fetch-boundary-reference'),
        modes.map(mode => read('fetch-boundary-' + mode)), {...expected, inputs: expected.fetchBoundaryInputs});
    report.fetchTransitions = compareFetchTransitionReports(read('fetch-transition-reference'),
        modes.map(mode => read('fetch-transition-' + mode)), {...expected, inputs: expected.fetchTransitionInputs});
    report.fetchBYOB = compareFetchBYOBShardReports(fetchBYOBShardNames.map(shard=>read('fetch-byob-'+shard+'-reference')),
        modes.map(mode=>fetchBYOBShardNames.map(shard=>read('fetch-byob-'+shard+'-'+mode))), {...expected, inputs: expected.fetchBYOBInputs});
    report.crypto = compareCryptoReports(read('crypto-reference'),
        modes.map(mode => read('crypto-' + mode)), {...expected, inputs: expected.cryptoInputs});
    // The large native backing-buffer corpus remains mandatory on every
    // successful gate. Reject earlier malformed families before parsing it;
    // no actual validation result is cached or omitted.
    report.httpCodec = compareHttpCodecReports(httpCodecShards.map(shard => read('http-codec-' + shard + '-reference')),
        modes.map(mode => ({mode, reports: httpCodecShards.map(shard => read('http-codec-' + shard + '-' + mode))})),
        {...expected, inputs: expected.httpCodecInputs});
    report.tests = actual;
    report.platform = referencePlatform(expected.lock);
    report.reference_asset = expected.lock.release_asset;
    report.reference_binary_sha256 = expected.lock.binary_sha256;
    report.run = run;
    return report;
}

export function assertControlRegistrationGroups(registration,{source=root,node=process.execPath,system=process.platform}={}){
    const windows=system==='Windows'||system==='win32',api=windows?path.win32:path.posix;
    const normalize=value=>windows?api.normalize(value).toLowerCase():api.normalize(value);
    for(const [name,shard,files] of [
        ['workerd_ws_gate_controls','primary',['test_workerd_reference_gate.mjs']],
        ['workerd_ws_gate_archive_controls','archives',['test_workerd_reference_gate.mjs']],
        ['workerd_ws_gate_extension_controls','extensions',['test_workerd_reference_gate.mjs']],
        ['workerd_ws_gate_support_controls',null,['test_workerd_outbound_contract.mjs','test_workerd_response_contract.mjs',
            'test_websocket_interop_contract.mjs','test_workerd_fanout_contract.mjs','test_workerd_frame_contract.mjs']]]){
        const matches=registration.tests.filter(test=>test.name===name);assert.equal(matches.length,1,'one required control group: '+name);
        const test=matches[0],command=[node,'--test',...files.map(file=>api.join(source,'tests',file))];
        assert.equal(test.command.length,command.length,'exact control command: '+name);
        command.forEach((value,index)=>assert.equal(index===1?test.command[index]:normalize(test.command[index]),
            index===1?value:normalize(value),'bound control argument: '+name));
        const props=new Map((test.properties??[]).map(prop=>[prop.name,prop.value]));
        assert.ok(Number.isFinite(props.get('TIMEOUT'))&&props.get('TIMEOUT')>0&&props.get('TIMEOUT')<=120,'unchanged control budget: '+name);
        if(shard){
            const env=props.get('ENVIRONMENT');assert.ok(Array.isArray(env),'control environment: '+name);
            assert.deepEqual(env.filter(value=>value.startsWith('CAPSID_WS_GATE_CONTROL_SHARD=')),
                ['CAPSID_WS_GATE_CONTROL_SHARD='+shard],'distinct complete control group: '+name);
        }
    }
}

function executeGate(build, destination, runId) {
    const lock = readLock();
    verifyClient(lock);
    const reference = process.env.CAPSID_WORKERD_REF;
    assert.ok(reference, 'CAPSID_WORKERD_REF is required for the compatibility gate');
    verifyBinary(reference, lock);
    const ext = referencePlatform(lock) === 'win32' ? '.exe' : '';
    const host = path.join(build, 'capsid-host' + ext), worker = path.join(build, 'capsid-worker' + ext);
    const expected = { lock, host: fileDigest(host), worker: fileDigest(worker),
        runner: fileDigest(path.join(root, 'tests/test_workerd_ws_fixtures.mjs')),
        messagePortInputs: Object.fromEntries(Object.entries(messagePortInputPaths).map(([name, file]) => [name, fileDigest(path.join(root, file))])),
        messagePortNode: fileDigest(process.execPath),
        outboundRunner: fileDigest(path.join(root, 'tests/test_websocket_outbound.mjs')),
        mutationController: fileDigest(path.join(root, 'tests/test_workerd_ws_mutations.mjs')),
        mutationContract: fileDigest(path.join(root, 'tools/workerd-mutation-contract.mjs')),
        mutationRunner: digest(prepareMutationRunner(fs.readFileSync(path.join(root, 'tests/test_workerd_ws_fixtures.mjs'), 'utf8'))),
        mutationRunners: Object.fromEntries(requiredMutationCases.map(({name}) => [name,
            digest(prepareMutationRunner(fs.readFileSync(path.join(root, 'tests/test_workerd_ws_fixtures.mjs'), 'utf8'), name))])),
        handlerErrorObserver:fileDigest(path.join(root,'tests/helpers/ws_handler_errors.mjs')),
        runId,
        fixtures: fixtureIdentities(),
        interopInputs:Object.fromEntries(Object.entries(interopInputPaths).map(([name,file])=>[name,fileDigest(path.join(root,file))])),
        interopClientLock:JSON.parse(fs.readFileSync(path.join(root,interopInputPaths.clientLock),'utf8')),
        fanoutInputs:Object.fromEntries(Object.entries(fanoutInputPaths).map(([name,file])=>[name,fileDigest(path.join(root,file))])),
        frameInputs:Object.fromEntries(Object.entries(frameInputPaths).map(([name,file])=>[name,fileDigest(path.join(root,file))])),
        ownerInputs:Object.fromEntries(Object.entries(ownerInputPaths).map(([name,file])=>[name,fileDigest(path.join(root,file))])),
        fetchResourceInputs:Object.fromEntries(Object.entries(fetchResourceInputPaths).map(([name,file])=>[name,fileDigest(path.join(root,file))])),
        ordinaryFetchInputs:Object.fromEntries(Object.entries(ordinaryFetchInputPaths).map(([name,file])=>[name,fileDigest(path.join(root,file))])),
        httpCodecInputs:Object.fromEntries(Object.entries(httpCodecInputPaths).map(([name,file])=>[name,fileDigest(path.join(root,file))])),
        fetchUpgradeInputs:Object.fromEntries(Object.entries(fetchUpgradeInputPaths).map(([name,file])=>[name,fileDigest(path.join(root,file))])),
        fetchUpgradeLifetimeInputs:Object.fromEntries(Object.entries(fetchUpgradeLifetimeInputPaths).map(([name,file])=>[name,fileDigest(path.join(root,file))])),
        fetchProxyInputs:Object.fromEntries(Object.entries(fetchProxyInputPaths).map(([name,file])=>[name,fileDigest(path.join(root,file))])),
        fetchBoundaryInputs:Object.fromEntries(Object.entries(fetchBoundaryInputPaths).map(([name,file])=>[name,fileDigest(path.join(root,file))])),
        fetchTransitionInputs:Object.fromEntries(Object.entries(fetchTransitionInputPaths).map(([name,file])=>[name,fileDigest(path.join(root,file))])),
        fetchBYOBInputs:Object.fromEntries(Object.entries(fetchBYOBInputPaths).map(([name,file])=>[name,fileDigest(path.join(root,file))])),
        cryptoInputs:Object.fromEntries(Object.entries(cryptoInputPaths).map(([name,file])=>[name,fileDigest(path.join(root,file))])),
        crossContextInputs:Object.fromEntries(Object.entries(crossContextInputPaths).map(([name,file])=>[name,fileDigest(path.join(root,file))])),
        asyncInputs:Object.fromEntries(Object.entries(asyncInputPaths).map(([name,file])=>[name,fileDigest(path.join(root,file))])),
        readerIntrinsicInputs:Object.fromEntries(Object.entries(readerIntrinsicInputPaths).map(([name,file])=>[name,fileDigest(path.join(root,file))])),
        readerIntrinsicSourceNames:readerIntrinsicSourceNames(pathToFileURL(path.join(root,readerIntrinsicInputPaths.fixture)).href),
        nativeReaderLifecycleInputs:Object.fromEntries(Object.entries(nativeReaderLifecycleInputPaths).map(([name,file])=>[name,fileDigest(path.join(root,file))])),
        readerScopeInputs:Object.fromEntries(Object.entries(readerScopeInputPaths).map(([name,file])=>[name,fileDigest(path.join(root,file))])),
        standardReaderInputs:Object.fromEntries(Object.entries(standardReaderInputPaths).map(([name,file])=>[name,fileDigest(path.join(root,file))])),
        asyncSourceNames:asyncSourceNames(pathToFileURL(path.join(root,asyncInputPaths.fixture)).href) };
    const tests = requiredTestsFor(lock);
    const registration = JSON.parse(execFileSync('ctest', ['--test-dir', build, '--show-only=json-v1'],
        { encoding: 'utf8', maxBuffer: 16 * 1024 * 1024 }));
    for (const name of tests) assert.equal(registration.tests.filter(t => t.name === name).length, 1,
        `required CTest registered: ${name}`);
    assertControlRegistrationGroups(registration);
    const interpreters=tests.filter(name=>name.startsWith('websocket_interop_')).map(name=>{
        const command=registration.tests.find(t=>t.name===name).command,index=command.indexOf('--python');
        return index<0?(process.env.CAPSID_WS_INTEROP_PYTHON??'python3'):command[index+1];
    });
    assert.equal(new Set(interpreters).size,1,'all independent-client tests use the same Python interpreter');
    const pythonIdentity=JSON.parse(execFileSync(interpreters[0],['-I',path.join(root,interopInputPaths.pythonProgram),'identity'],{encoding:'utf8',timeout:10000}));
    const clientLock=expected.interopClientLock;
    assert.deepEqual(pythonIdentity,{implementation:clientLock.python.implementation,python:clientLock.python.version,
        websockets:clientLock.websockets.version,sourceFiles:clientLock.websockets.sourceFiles,sourceSHA256:clientLock.websockets.sourceSHA256},
        'locked independent Python client is installed before the matrix starts');
    expected.interopInterpreter={path:interpreters[0],identity:pythonIdentity};
    const nodePackages=tests.filter(name=>name.startsWith('websocket_interop_')||name.startsWith('workerd_fanout_')).map(name=>{
        const command=registration.tests.find(t=>t.name===name).command,index=command.indexOf('--node-package');
        return index<0?process.env.CAPSID_WS_INTEROP_NODE_PACKAGE:command[index+1];
    });
    assert.equal(new Set(nodePackages).size,1,'all independent-client tests use the same Node ws package');
    assert.ok(nodePackages[0],'CAPSID_WS_INTEROP_NODE_PACKAGE is required');
    const nodeIdentity=JSON.parse(execFileSync(process.execPath,[path.join(root,interopInputPaths.nodeProgram),'identity'],
        {encoding:'utf8',timeout:10000,env:{...process.env,CAPSID_WS_INTEROP_NODE_PACKAGE:nodePackages[0]}}));
    assert.deepEqual(nodeIdentity,{node:lock.client.node_version,undici:lock.client.undici_version,
        ws:clientLock.ws.version,sourceFiles:clientLock.ws.sourceFiles,sourceSHA256:clientLock.ws.sourceSHA256},'locked Node ws package source');
    expected.interopNodePackage={path:nodePackages[0],identity:nodeIdentity};
    fs.mkdirSync(destination, { recursive: true });
    const run = fs.mkdtempSync(path.join(destination, 'run-'));
    const args = ['--test-dir', build, '-R', `^(${tests.join('|')})$`, '--parallel', '1',
        '--no-tests=error', '--output-on-failure', '--output-junit', path.join(run, 'junit.xml')];
    fs.writeFileSync(path.join(run, 'command.json'), JSON.stringify(['ctest', ...args], null, 2) + '\n');
    fs.writeFileSync(path.join(run, 'registration.json'), JSON.stringify(registration, null, 2) + '\n');
    fs.writeFileSync(path.join(run, 'expected-identities.json'), JSON.stringify(expected, null, 2) + '\n');
    try {
        const stdout = execFileSync('ctest', args, { encoding: 'utf8', maxBuffer: 32 * 1024 * 1024,
            env: { ...process.env, CAPSID_WS_REPORT_DIR: run, CAPSID_WORKERD_REQUIRED: '1',
                CAPSID_WS_RUN_ID:expected.runId } });
        fs.writeFileSync(path.join(run, 'ctest.log'), stdout);
    } catch (error) {
        fs.writeFileSync(path.join(run, 'ctest.log'), String(error.stdout ?? '') + String(error.stderr ?? ''));
        throw error;
    } finally {
        const last = path.join(build, 'Testing/Temporary/LastTest.log');
        if (fs.existsSync(last)) fs.copyFileSync(last, path.join(run, 'LastTest.log'));
    }
    const report = validateGateRun(run, expected);
    return report;
}

export function runGate(build, destination) {
    fs.mkdirSync(destination, {recursive:true});
    const summary = path.join(destination, 'summary.json');
    const attempt = {complete:false, status:'running', runId:randomUUID()};
    const write = report => fs.writeFileSync(summary, JSON.stringify(report, null, 2) + '\n');
    // Invalidate the previous result before preflight, so even missing input
    // cannot leave an old passing summary masquerading as this attempt.
    write(attempt);
    try {
        const report = {...executeGate(build, destination, attempt.runId),
            status:'passed', runId:attempt.runId};
        write(report);
        console.log(JSON.stringify(report, null, 2));
        return report;
    } catch (error) {
        write({...attempt, status:'failed', error:error.message});
        throw error;
    }
}

if (process.argv[1] && fs.realpathSync(process.argv[1]) === fs.realpathSync(fileURLToPath(import.meta.url))) {
    try {
        assert.ok(process.argv[2] && process.argv[3], 'usage: run-workerd-ws-gate.mjs BUILD EVIDENCE');
        runGate(path.resolve(process.argv[2]), path.resolve(process.argv[3]));
    } catch (error) { console.error(error.message); process.exitCode = 1; }
}
