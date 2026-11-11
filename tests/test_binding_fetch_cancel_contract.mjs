import assert from 'node:assert/strict';
import {test, after} from 'node:test';
import {cancelBindingKinds, cancelBindingRoutes, cancelBindingViews, cancelBindingMethods,
    cancelBindingPacket, cancelBindingStateTemplate, cancelBindingInputPaths} from '../tools/binding-fetch-cancel-input.mjs';
import {assertCancelBindingCase, validateCancelBindingReport} from '../tools/binding-fetch-cancel-contract.mjs';

let rejected = 0;
const stdout = item => 'memoryLimit=0 actualFeatures=0 expectedFeatures=0\nBINDING_REQUEST 1\nBINDING_PHASE 1 '+JSON.stringify(item.request1)+'\nBINDING_REQUEST 2\nBINDING_PHASE 2 '+JSON.stringify(item.request2)+'\nBINDING_EXIT '+JSON.stringify(item.workerExit)+'\n';
function sample(kind,route,view,method,port=19000,later=false){
    const state=structuredClone(cancelBindingStateTemplate(kind,route,view,method,'http://127.0.0.1:'+port+'/body')),packet=cancelBindingPacket(kind),split=packet.indexOf('\r\n\r\n')+4;
    const source=(path,start)=>{const request='GET '+path+' HTTP/1.1\r\nHost: 127.0.0.1:'+port;return {path,request,rawRequest:Buffer.from(request+'\r\n\r\n').toString('base64'),peerEnded:false,peerReset:false,headersSent:true,bodySent:false,fixtureEnded:false,fixtureDestroyed:false,events:[{name:'request',sequence:start},{name:'headers-only',sequence:start+1}],sentHeader:packet.subarray(0,split).toString('base64')};};
    const pool=source('/body',1),pending=source('/pending',3),aborted={...structuredClone(pending),peerEnded:true};aborted.events.push({name:'peer-ended',sequence:later?7:5});
    const final={...structuredClone(pool),peerEnded:true,bodySent:true,fixtureEnded:true,sent:packet.toString('base64')};final.events.push({name:'body-after-request2',sequence:11},{name:'peer-ended',sequence:12});
    const item={kind,route,view,method,events:['request1-begun','request1-ended','continue-request2','request2-begun','request2-ended','finish'].map((name,index)=>({name,sequence:[0,later?5:6,9,10,13,15][index]})),port,...state,
        afterRequest1:[pool,structuredClone(later?pending:aborted)],request1SampleSequence:later?6:7,beforeRequest2:[structuredClone(pool),structuredClone(aborted)],cancelSampleSequence:8,beforeStop:[final,aborted],finalSampleSequence:14,child:{code:0,signal:null},workerExit:{reaped:true,channelClosed:true,code:0},stdout:'',stderr:'',finalSources:[structuredClone(final),structuredClone(aborted)]};
    item.stdout=stdout(item);return item;
}
function leaves(value, path = []) {return value && typeof value === 'object' ? Object.entries(value).flatMap(([key, child]) => leaves(child, [...path, key])) : [path];}
function mutate(item, path) {let parent = item; for (const key of path.slice(0, -1)) parent = parent[key]; const key = path.at(-1), value = parent[key]; parent[key] = value === null ? false : typeof value === 'boolean' ? Number(value) : typeof value === 'number' ? value+1 : String(value)+'!';}
function reject(base, change, syncFinal = true, syncStdout = true) {
    const item = structuredClone(base); change(item); if (syncFinal) item.finalSources = structuredClone(item.beforeStop); if (syncStdout) item.stdout = stdout(item);
    assert.throws(() => assertCancelBindingCase(item, base.kind, base.route, base.view, base.method)); rejected++;
}
for (const kind of cancelBindingKinds) for (const route of cancelBindingRoutes) test(kind+'/'+route+': exact full family, call/context ownership and real source history counterexamples', () => {
    for (const view of cancelBindingViews) for (const method of cancelBindingMethods) assertCancelBindingCase(sample(kind, route, view, method,19000,method==='least'), kind, route, view, method);
    const base = sample(kind, route, kind === 'raw' ? 'Float64Array' : 'DataView', kind === 'raw' ? 'read' : 'least');
    for (const key of Object.keys(base)) reject(base, item => {delete item[key];}, false, false);
    for (const section of ['request1', 'request2', 'afterRequest1', 'beforeRequest2', 'beforeStop', 'events', 'child', 'workerExit']) {
        for (const path of leaves(base[section])) reject(base, item => mutate(item, [section, ...path]));
    }
    for (const key of ['port', 'request1SampleSequence', 'cancelSampleSequence', 'finalSampleSequence']) reject(base, item => mutate(item, [key]));
    for (const change of [item => {item.request2.reads.pop();}, item => {item.request2.reads[0] = structuredClone(item.request2.reads[1]);}, item => {item.request2.closed.data.call.requestId = '1';},
        item => {item.request1.ready.data.readState = 'fulfilled';}, item => {item.request2.reads[0].data.openSignalAborted = true;}, item => {item.afterRequest1[0].bodySent = true;}, item => {item.afterRequest1[0].peerEnded = true;},
        item => {item.beforeStop[0].peerEnded = false;}, item => {item.events[3].sequence = item.events[2].sequence;}, item => {item.request2.inspected.data.signalAborted=false;}, item => {item.request2.inspected.data.abortEvents.push({aborted:true});}, item => {item.request2.late[0].status='fulfilled';}, item => {item.beforeRequest2[1].peerEnded=false;}, item => {item.stderr = 'unexpected';}]) reject(base, change);
    reject(base, item => {item.finalSources[0].peerEnded = false;}, false); reject(base, item => {item.stdout += 'manufactured\n';}, true, false);
    assertCancelBindingCase(base, kind, route, base.view, base.method); base.request2.reads[0].data.call.frozen = 1; base.stdout = stdout(base);
    assert.throws(() => assertCancelBindingCase(base, kind, route, base.view, base.method)); rejected++;
});
test('full mutable report, source/binary identity and exact selection/order are checked again', () => {
    const inputs = Object.fromEntries(Object.keys(cancelBindingInputPaths).map(key => [key, 'a'.repeat(64)])), binaries = {helper: 'b'.repeat(64), worker: 'c'.repeat(64)};
    const report = {schema: 'capsid-binding-fetch-cancel-v1', complete: true, runId: 'synthetic-controls', kind: 'raw', route: 'getReader', inputs, binaries,
        cases: cancelBindingViews.flatMap(view => cancelBindingMethods.map(method => sample('raw', 'getReader', view, method)))};
    const expected = {runId: report.runId, kind: report.kind, route: report.route, inputs: structuredClone(inputs), binaries: structuredClone(binaries)}; validateCancelBindingReport(report, expected);
    for (const change of [item => {item.schema = '';}, item => {item.complete = 1;}, item => {item.runId = 'changed';}, item => {item.kind = 'gzip';}, item => {item.route = 'direct';},
        item => {item.inputs.policy = 'd'.repeat(64);}, item => {item.binaries.worker = 'd'.repeat(64);}, item => {item.cases.pop();}, item => {item.cases[1] = structuredClone(item.cases[0]);}]) {
        const bad = structuredClone(report); change(bad); assert.throws(() => validateCancelBindingReport(bad, expected)); rejected++;
    }
    report.cases[0].afterRequest1[0].peerEnded = true;
    assert.throws(() => validateCancelBindingReport(report, expected)); rejected++;
});
test('only32 deeply frozen fixed-input templates are shared; mutable samples and acceptance are independent', () => {
    const first = cancelBindingStateTemplate('raw', 'getReader', 'Uint8Array', 'read', 'http://127.0.0.1:21000/body');
    assert.ok(Object.isFrozen(first) && Object.isFrozen(first.request2.reads[0].data.read.backing) && Object.isFrozen(first.request1.opened.data.call));
    assert.throws(() => {first.request2.reads[0].data.read.backing[0] = 0;}, TypeError); const copy = structuredClone(first); copy.request2.reads[0].data.read.backing[0] = 0; assert.equal(first.request2.reads[0].data.read.backing[0], 165);
    for (let index = 1; index <= 31; index++) cancelBindingStateTemplate('raw', 'getReader', 'Uint8Array', 'read', 'http://127.0.0.1:'+(21000+index)+'/body');
    assert.equal(cancelBindingStateTemplate('raw', 'getReader', 'Uint8Array', 'read', 'http://127.0.0.1:21000/body'), first);
    cancelBindingStateTemplate('raw', 'getReader', 'Uint8Array', 'read', 'http://127.0.0.1:21032/body'); assert.notEqual(cancelBindingStateTemplate('raw', 'getReader', 'Uint8Array', 'read', 'http://127.0.0.1:21000/body'), first);
});
after(() => {assert.equal(rejected, 7950); console.log(JSON.stringify({complete: true, uniqueCanceledReaderInputs: 260, rejectedMutations: rejected}));});
