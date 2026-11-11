import assert from 'node:assert/strict';
import {bindingFetchPacket, expectedBindingFetchRow, bindingFetchViewGroups, bindingFetchRoutes, bindingFetchMethods} from './binding-fetch-body-input.mjs';
import {bindingArgumentCall, bindingArgumentWireTypes} from './binding-fetch-arguments-input.mjs';

export const retainedBindingKinds = Object.freeze(['raw', 'gzip']);
export const retainedBindingRoutes = bindingFetchRoutes;
export const retainedBindingMethods = bindingFetchMethods;
export const retainedBindingViews = Object.freeze(bindingFetchViewGroups.flat());
export const retainedBindingPacket = bindingFetchPacket;
export const retainedBindingInputPaths = Object.freeze({
    runner: 'tests/test_binding_fetch_retained.mjs', controls: 'tests/test_binding_fetch_retained_contract.mjs', helper: 'tests/test_binding_fetch_retained.cc',
    binding: 'tests/fixtures/binding-fetch-retained-package.mjs', user: 'tests/fixtures/binding-fetch-retained-user.mjs',
    sourceModel: 'tools/binding-fetch-retained-input.mjs', contract: 'tools/binding-fetch-retained-contract.mjs', policy: 'docs/binding-technical-design.md',
    priorBodyModel: 'tools/binding-fetch-body-input.mjs', priorBodyContract: 'tools/binding-fetch-body-contract.mjs',
    finiteIoReferenceFixture: 'tests/fixtures/workerd-binding-fetch-body.mjs', finiteIoReferenceArchive: 'tests/fixtures/workerd-binding-fetch-body-report.json',
    finiteIoBindingFixture: 'tests/fixtures/binding-fetch-body-package.mjs',
    priorArgumentModel: 'tools/binding-fetch-arguments-input.mjs', priorNativeArgumentModel: 'tools/workerd-fetch-arguments-contract.mjs',
    priorNativeContract: 'tools/workerd-native-fetch-contract.mjs', priorTransitionContract: 'tools/workerd-fetch-transition-contract.mjs',
    platform: 'tools/workerd-platform.mjs', exitObserver: 'tests/graceful_worker_exit.h', rpcPolicy: 'src/binding_rpc.h', rpcBackend: 'src/binding_rpc.cc',
});
const models = new Map();
const freeze = value => {if (value && typeof value === 'object') {for (const child of Object.values(value)) freeze(child); Object.freeze(value);} return value;};
// Immutable expected values are derived solely from fixed byte/API inputs.
// No observed report or validation acceptance is cached.
export function retainedBindingStateTemplate(kind, route, view, method, target) {
    assert.ok(retainedBindingKinds.includes(kind) && retainedBindingRoutes.includes(route));
    assert.ok(retainedBindingViews.includes(view) && retainedBindingMethods.includes(method)); assert.equal(typeof target, 'string');
    const key = JSON.stringify([kind, route, view, method, target]);
    if (!models.has(key)) {
        const row = expectedBindingFetchRow(route, method, view, kind, target), call1 = bindingArgumentCall(), call2 = {...bindingArgumentCall(), requestId: '2'};
        const opened = {metadata: {...row.metadata, lockedBefore: true}, brand: true, closedStable: true, closedState: 'pending', call: call1};
        const reads = row.reads.map(read => {const data = {read, closedStable: true, openSignalAborted: false, call: call2}; return {data, types: bindingArgumentWireTypes(data)};});
        const closed = {closed: 'fulfilled', bodyUsed: true, locked: false, openSignalAborted: false, call: call2};
        const model = freeze({request1: {data: opened, types: bindingArgumentWireTypes(opened)}, request2: {reads, closed: {data: closed, types: bindingArgumentWireTypes(closed)}}});
        if (models.size === 32) models.delete(models.keys().next().value); models.set(key, model);
    }
    return models.get(key);
}
