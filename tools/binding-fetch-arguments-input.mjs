import assert from 'node:assert/strict';
import {gzipSync} from 'node:zlib';
import {nativeArgumentRoutes, nativeArgumentWidths, expectedNativeArgumentState} from './workerd-fetch-arguments-contract.mjs';

export const bindingArgumentRoutes = nativeArgumentRoutes;
export const bindingArgumentViews = Object.freeze(Object.keys(nativeArgumentWidths));
export const bindingArgumentMethods = Object.freeze(['read', 'least']);
export const bindingArgumentKinds = Object.freeze(['raw', 'gzip']);
export const bindingArgumentInputPaths = Object.freeze({
    runner: 'tests/test_binding_fetch_arguments.mjs', controls: 'tests/test_binding_fetch_arguments_contract.mjs',
    binding: 'tests/fixtures/binding-fetch-arguments-package.mjs', user: 'tests/fixtures/binding-fetch-arguments-user.mjs',
    referenceFixture: 'tests/fixtures/workerd-binding-fetch-arguments.mjs', referenceArchive: 'tests/fixtures/workerd-binding-fetch-arguments-report.json.gz',
    referenceMetadata: 'tests/fixtures/workerd-binding-fetch-arguments-report.json', sourceModel: 'tools/binding-fetch-arguments-input.mjs',
    contract: 'tools/binding-fetch-arguments-contract.mjs', priorModel: 'tools/workerd-fetch-arguments-contract.mjs',
    priorFixture: 'tests/fixtures/workerd-fetch-arguments.mjs', nativeContract: 'tools/workerd-native-fetch-contract.mjs',
    transitionContract: 'tools/workerd-fetch-transition-contract.mjs', platform: 'tools/workerd-platform.mjs',
    helper: 'tests/test_binding_fetch_body.cc', exitObserver: 'tests/graceful_worker_exit.h', rpcPolicy: 'src/binding_rpc.h', rpcBackend: 'src/binding_rpc.cc',
    windowsLock: 'tests/fixtures/workerd-ws/reference-lock-windows.json', linuxLock: 'tests/fixtures/workerd-ws/reference-lock.json',
});
const payload = Buffer.from(Array.from({length: 16}, (_, index) => index+1));
const encoded = gzipSync(payload); encoded[9] = 255;
export function bindingArgumentPacket(kind) {
    assert.ok(bindingArgumentKinds.includes(kind)); const body = kind === 'gzip' ? encoded : payload;
    return Buffer.concat([Buffer.from('HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Type: application/octet-stream\r\n'+
        (kind === 'gzip' ? 'Content-Encoding: gzip\r\n' : '')+'Content-Length: '+body.length+'\r\n\r\n'), body]);
}
export function bindingArgumentCall() {
    return {requestIdType: 'bigint', requestId: '1', deadlineType: 'bigint', nullPrototype: true, frozen: true,
        signalType: 'object', aborted: false, signalNew: true};
}
export function bindingArgumentWireTypes(value) {
    let objects = 0, arrays = 0;
    const visit = item => {if (!item || typeof item !== 'object') return;
        if (Array.isArray(item)) arrays++; else objects++; for (const child of Object.values(item)) visit(child);};
    visit(value); return {objects, arrays, objectNullPrototypes: true, arrayPrototypes: true};
}
// Fixed input models only. No observed report or validation result is cached.
const models = new Map();
const freeze = value => {if (value && typeof value === 'object') {for (const child of Object.values(value)) freeze(child); Object.freeze(value);} return value;};
function template(bodyKind, route, view, method) {
    assert.ok(bindingArgumentKinds.includes(bodyKind) && bindingArgumentRoutes.includes(route));
    assert.ok(bindingArgumentViews.includes(view) && bindingArgumentMethods.includes(method));
    const key = JSON.stringify([bodyKind, route, view, method]);
    if (!models.has(key)) {
        const state = expectedNativeArgumentState('native-arguments-'+route+'-'+view+'-'+method, 'binding-arguments');
        for (const row of state.rows) row.response.contentLength = String(bodyKind === 'gzip' ? encoded.length : payload.length);
        if (models.size === 32) models.delete(models.keys().next().value);
        models.set(key, freeze({state, calls: state.rows.map(bindingArgumentCall), wireTypes: state.rows.map(row => bindingArgumentWireTypes({
            state: {kind: state.kind, challenge: state.challenge, rows: [row], unexpected: null}, call: bindingArgumentCall()}))}));
    }
    return models.get(key);
}
export function bindingArgumentStateTemplate(bodyKind, route, view, method) {
    return template(bodyKind, route, view, method).state;
}
export function expectedBindingArgumentEnvelope(bodyKind, route, view, method) {
    return template(bodyKind, route, view, method);
}
