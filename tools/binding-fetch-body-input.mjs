import assert from 'node:assert/strict';
import {gzipSync} from 'node:zlib';

export const bindingFetchViewGroups = Object.freeze([
    ['Uint8Array', 'Uint8ClampedArray', 'Int8Array', 'Int16Array'],
    ['Uint16Array', 'Int32Array', 'Uint32Array'],
    ['Float16Array', 'Float32Array', 'Float64Array'],
    ['BigInt64Array', 'BigUint64Array', 'DataView'],
].map(Object.freeze));
export const bindingFetchKinds = Object.freeze(['raw', 'gzip']);
export const bindingFetchRoutes = Object.freeze(['getReader', 'direct', 'prototype', 'subclass', 'reflect']);
export const bindingFetchMethods = Object.freeze(['read', 'least']);
export const bindingFetchInputPaths = Object.freeze({
    runner: 'tests/test_binding_fetch_body.mjs', helper: 'tests/test_binding_fetch_body.cc',
    binding: 'tests/fixtures/binding-fetch-body-package.mjs', user: 'tests/fixtures/binding-fetch-body-user.mjs',
    referenceFixture: 'tests/fixtures/workerd-binding-fetch-body.mjs', referenceArchive: 'tests/fixtures/workerd-binding-fetch-body-report.json',
    sourceModel: 'tools/binding-fetch-body-input.mjs', contract: 'tools/binding-fetch-body-contract.mjs',
    exitObserver: 'tests/graceful_worker_exit.h', rpcPolicy: 'src/binding_rpc.h', rpcBackend: 'src/binding_rpc.cc',
});
const payload = Buffer.from(Array.from({length: 97}, (_, index) => (index*29+17)%251));
const encoded = gzipSync(payload); encoded[9] = 255;
const packets = Object.fromEntries(bindingFetchKinds.map(kind => {
    const body = kind === 'gzip' ? encoded : payload;
    const header = Buffer.from('HTTP/1.1 201 Created\r\nConnection: close\r\nContent-Type: application/octet-stream\r\n'+
        (kind === 'gzip' ? 'Content-Encoding: gzip\r\n' : '')+'Content-Length: '+body.length+'\r\n\r\n');
    return [kind, Buffer.concat([header, body])];
}));
export function bindingFetchPacket(kind) {
    assert.ok(bindingFetchKinds.includes(kind)); return Buffer.from(packets[kind]);
}
export function bindingCloneNodes(value) {
    return 1+(Array.isArray(value) ? value.reduce((sum, item) => sum+bindingCloneNodes(item), 0)
        : value && typeof value === 'object' ? Object.values(value).reduce((sum, item) => sum+bindingCloneNodes(item), 0) : 0);
}
// Only fixed byte construction is shared. No actual validation result is cached.
const readModels = [];
for (let offset = 0, index = 0; index < 5; index++) {
    const length = [32, 32, 32, 1, 0][index], done = length === 0, at = done ? 0 : 8;
    const bytes = Array.from(payload.subarray(offset, offset+length)); offset += length;
    readModels.push(Object.freeze({inputAfter: 0, done, type: 'Uint8Array', offset: at, length, capacity: 64,
        bytes: Object.freeze(bytes), backing: Object.freeze([...Array(at).fill(165), ...bytes, ...Array(64-at-length).fill(165)])}));
}
Object.freeze(readModels);
export function expectedBindingFetchRow(route, method, view, kind, target) {
    assert.ok(bindingFetchRoutes.includes(route) && bindingFetchMethods.includes(method));
    assert.ok(bindingFetchViewGroups.flat().includes(view) && bindingFetchKinds.includes(kind));
    return {route, method, view, failure: null, reads: structuredClone(readModels), metadata: {
        status: 201, statusText: 'Created', ok: true, redirected: false, type: 'default', url: target,
        headers: [['connection', 'close'], ...(kind === 'gzip' ? [['content-encoding', 'gzip']] : []),
            ['content-length', String(kind === 'gzip' ? encoded.length : 97)], ['content-type', 'application/octet-stream']],
        bodyUsedBefore: false, lockedBefore: false,
    }, brand: true, closedStable: true, closed: 'fulfilled', lockedAfter: false, bodyUsedAfter: true};
}
export function expectedBindingFetchRows(kind, group, target) {
    assert.ok(Number.isInteger(group) && group >= 0 && group < bindingFetchViewGroups.length);
    return bindingFetchRoutes.flatMap(route => bindingFetchMethods.flatMap(method =>
        bindingFetchViewGroups[group].map(view => expectedBindingFetchRow(route, method, view, kind, target))));
}
export function expectedBindingCall() {
    return {requestIdType: 'bigint', requestId: '1', deadlineType: 'bigint', nullPrototype: true, frozen: true,
        signalType: 'object', aborted: false, signalNew: true};
}
export function expectedBindingWireTypes() {
    return {nullPrototype: true, rowNullPrototype: true, callNullPrototype: true, metadataNullPrototype: true,
        readNullPrototypes: true, readsArrayPrototype: true, byteArrayPrototypes: true, headersArrayPrototypes: true};
}
function buildExpectedBindingFetchState(kind, group, target) {
    const rows = expectedBindingFetchRows(kind, group, target), count = rows.length;
    const result = {batchWireType: 'string', batch: rows, structured: structuredClone(rows),
        calls: rows.map(expectedBindingCall), rowWireTypes: rows.map(expectedBindingWireTypes),
        events: ['jsonBatch', ...rows.map((_, index) => 'row'+index), 'oversizedRejected', 'recovery', ...Array.from({length: 70}, (_, index) => 'echo'+index)],
        completedCalls: Array.from({length: 70}, (_, index) => ({value: {index}, inputWasNullPrototype: true, outputNullPrototype: true, call: expectedBindingCall()})),
        matrixCloneNodes: bindingCloneNodes(rows), maxRowCloneNodes: Math.max(...rows.map(bindingCloneNodes)),
        oversized: {fulfilled: false, name: 'Error', message: 'binding result is not cloneable: clone node limit exceeded', errorPrototype: true},
        recovery: expectedBindingFetchRow('getReader', 'read', bindingFetchViewGroups[group][0], kind, target), recoveryCall: expectedBindingCall()};
    assert.ok(result.matrixCloneNodes > 10000 && result.maxRowCloneNodes < 10000);
    return result;
}

// Bound immutable input-derived templates, never observed reports or results.
// Every validator call still compares the complete current mutable value.
const stateTemplates = new Map();
const freeze = value => {
    if (value && typeof value === 'object') {for (const item of Object.values(value)) freeze(item); Object.freeze(value);}
    return value;
};
export function bindingFetchStateTemplate(kind, group, target) {
    assert.equal(typeof target, 'string');
    const key = JSON.stringify([kind, group, target]);
    if (!stateTemplates.has(key)) {
        const model = freeze(buildExpectedBindingFetchState(kind, group, target));
        if (stateTemplates.size === 32) stateTemplates.delete(stateTemplates.keys().next().value);
        stateTemplates.set(key, model);
    }
    return stateTemplates.get(key);
}
export function expectedBindingFetchState(kind, group, target) {
    return structuredClone(bindingFetchStateTemplate(kind, group, target));
}
