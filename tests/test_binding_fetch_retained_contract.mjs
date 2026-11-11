import assert from 'node:assert/strict';
import {test, after} from 'node:test';
import {retainedBindingKinds, retainedBindingRoutes, retainedBindingViews, retainedBindingMethods,
    retainedBindingPacket, retainedBindingStateTemplate, retainedBindingInputPaths} from '../tools/binding-fetch-retained-input.mjs';
import {assertRetainedBindingCase, validateRetainedBindingReport} from '../tools/binding-fetch-retained-contract.mjs';

let rejected = 0;
const stdout = item => 'memoryLimit=0 actualFeatures=0 expectedFeatures=0\nBINDING_REQUEST 1\nBINDING_PHASE 1 '+JSON.stringify(item.request1)+'\nBINDING_REQUEST 2\nBINDING_PHASE 2 '+JSON.stringify(item.request2)+'\nBINDING_EXIT '+JSON.stringify(item.workerExit)+'\n';
function sample(kind, route, view, method, port = 19000) {
    const state = structuredClone(retainedBindingStateTemplate(kind, route, view, method, 'http://127.0.0.1:'+port+'/body')), request = 'GET /body HTTP/1.1\r\nHost: 127.0.0.1:'+port, packet = retainedBindingPacket(kind), split = packet.indexOf('\r\n\r\n')+4;
    const source = {request, rawRequest: Buffer.from(request+'\r\n\r\n').toString('base64'), peerEnded: false, peerReset: false, headersSent: true, bodySent: false, fixtureEnded: false, fixtureDestroyed: false,
        events: [{name: 'request', sequence: 1}, {name: 'headers-only', sequence: 2}], sentHeader: packet.subarray(0, split).toString('base64')};
    const final = {...structuredClone(source), peerEnded: true, bodySent: true, fixtureEnded: true, sent: packet.toString('base64')}; final.events.push({name: 'body-after-request2', sequence: 7}, {name: 'peer-ended', sequence: 8});
    const item = {kind, route, view, method, events: ['request1-begun', 'request1-ended', 'continue-request2', 'request2-begun', 'request2-ended', 'finish'].map((name, index) => ({name, sequence: [0, 3, 5, 6, 9, 11][index]})),
        port, ...state, afterRequest1: [source], request1SampleSequence: 4, beforeStop: [final], finalSampleSequence: 10, child: {code: 0, signal: null}, workerExit: {reaped: true, channelClosed: true, code: 0}, stdout: '', stderr: '', finalSources: [structuredClone(final)]};
    item.stdout = stdout(item); return item;
}
function leaves(value, path = []) {return value && typeof value === 'object' ? Object.entries(value).flatMap(([key, child]) => leaves(child, [...path, key])) : [path];}
function mutate(item, path) {let parent = item; for (const key of path.slice(0, -1)) parent = parent[key]; const key = path.at(-1), value = parent[key]; parent[key] = value === null ? false : typeof value === 'boolean' ? Number(value) : typeof value === 'number' ? value+1 : String(value)+'!';}
function reject(base, change, syncFinal = true, syncStdout = true) {
    const item = structuredClone(base); change(item); if (syncFinal) item.finalSources = structuredClone(item.beforeStop); if (syncStdout) item.stdout = stdout(item);
    assert.throws(() => assertRetainedBindingCase(item, base.kind, base.route, base.view, base.method)); rejected++;
}
for (const kind of retainedBindingKinds) for (const route of retainedBindingRoutes) test(kind+'/'+route+': exact full family, call/context ownership and real source history counterexamples', () => {
    for (const view of retainedBindingViews) for (const method of retainedBindingMethods) assertRetainedBindingCase(sample(kind, route, view, method), kind, route, view, method);
    const base = sample(kind, route, kind === 'raw' ? 'Float64Array' : 'DataView', kind === 'raw' ? 'read' : 'least');
    for (const key of Object.keys(base)) reject(base, item => {delete item[key];}, false, false);
    for (const section of ['request1', 'request2', 'afterRequest1', 'beforeStop', 'events', 'child', 'workerExit']) {
        for (const path of leaves(base[section])) reject(base, item => mutate(item, [section, ...path]));
    }
    for (const key of ['port', 'request1SampleSequence', 'finalSampleSequence']) reject(base, item => mutate(item, [key]));
    for (const change of [item => {item.request2.reads.pop();}, item => {item.request2.reads[0] = structuredClone(item.request2.reads[1]);}, item => {item.request2.closed.data.call.requestId = '1';},
        item => {item.request1.data.closedState = 'fulfilled';}, item => {item.request2.reads[0].data.openSignalAborted = true;}, item => {item.afterRequest1[0].bodySent = true;}, item => {item.afterRequest1[0].peerEnded = true;},
        item => {item.beforeStop[0].peerEnded = false;}, item => {item.events[3].sequence = item.events[2].sequence;}, item => {item.stderr = 'unexpected';}]) reject(base, change);
    reject(base, item => {item.finalSources[0].peerEnded = false;}, false); reject(base, item => {item.stdout += 'manufactured\n';}, true, false);
    assertRetainedBindingCase(base, kind, route, base.view, base.method); base.request2.reads[0].data.call.frozen = 1; base.stdout = stdout(base);
    assert.throws(() => assertRetainedBindingCase(base, kind, route, base.view, base.method)); rejected++;
});
test('full mutable report, source/binary identity and exact selection/order are checked again', () => {
    const inputs = Object.fromEntries(Object.keys(retainedBindingInputPaths).map(key => [key, 'a'.repeat(64)])), binaries = {helper: 'b'.repeat(64), worker: 'c'.repeat(64)};
    const report = {schema: 'capsid-binding-fetch-retained-v1', complete: true, runId: 'synthetic-controls', kind: 'raw', route: 'getReader', inputs, binaries,
        cases: retainedBindingViews.flatMap(view => retainedBindingMethods.map(method => sample('raw', 'getReader', view, method)))};
    const expected = {runId: report.runId, kind: report.kind, route: report.route, inputs: structuredClone(inputs), binaries: structuredClone(binaries)}; validateRetainedBindingReport(report, expected);
    for (const change of [item => {item.schema = '';}, item => {item.complete = 1;}, item => {item.runId = 'changed';}, item => {item.kind = 'gzip';}, item => {item.route = 'direct';},
        item => {item.inputs.policy = 'd'.repeat(64);}, item => {item.binaries.worker = 'd'.repeat(64);}, item => {item.cases.pop();}, item => {item.cases[1] = structuredClone(item.cases[0]);}]) {
        const bad = structuredClone(report); change(bad); assert.throws(() => validateRetainedBindingReport(bad, expected)); rejected++;
    }
    report.cases[0].afterRequest1[0].peerEnded = true;
    assert.throws(() => validateRetainedBindingReport(report, expected)); rejected++;
});
test('only32 deeply frozen fixed-input templates are shared; mutable samples and acceptance are independent', () => {
    const first = retainedBindingStateTemplate('raw', 'getReader', 'Uint8Array', 'read', 'http://127.0.0.1:21000/body');
    assert.ok(Object.isFrozen(first) && Object.isFrozen(first.request2.reads[0].data.read.backing) && Object.isFrozen(first.request1.data.call));
    assert.throws(() => {first.request2.reads[0].data.read.backing[0] = 0;}, TypeError); const copy = structuredClone(first); copy.request2.reads[0].data.read.backing[0] = 0; assert.equal(first.request2.reads[0].data.read.backing[0], 165);
    for (let index = 1; index <= 31; index++) retainedBindingStateTemplate('raw', 'getReader', 'Uint8Array', 'read', 'http://127.0.0.1:'+(21000+index)+'/body');
    assert.equal(retainedBindingStateTemplate('raw', 'getReader', 'Uint8Array', 'read', 'http://127.0.0.1:21000/body'), first);
    retainedBindingStateTemplate('raw', 'getReader', 'Uint8Array', 'read', 'http://127.0.0.1:21032/body'); assert.notEqual(retainedBindingStateTemplate('raw', 'getReader', 'Uint8Array', 'read', 'http://127.0.0.1:21000/body'), first);
});
after(() => {assert.equal(rejected, 6450); console.log(JSON.stringify({complete: true, uniqueRetainedReaderInputs: 260, rejectedMutations: rejected}));});
