import assert from 'node:assert/strict';
import {isDeepStrictEqual} from 'node:util';
import {retainedBindingKinds, retainedBindingRoutes, retainedBindingViews, retainedBindingMethods,
    retainedBindingPacket, retainedBindingStateTemplate} from './binding-fetch-retained-input.mjs';
// Revalidate the existing actual pinned finite97-byte User I/O captures. Their
// data/API model is reused; BindingID pool lifetime has its own declared model.
import './binding-fetch-body-contract.mjs';

const equal = (actual, expected, message) => assert.ok(isDeepStrictEqual(actual, expected), message ?? 'complete current typed value');
const fields = (value, names) => {assert.ok(value && typeof value === 'object' && !Array.isArray(value)); equal(Object.keys(value).sort(), names.slice().sort());};
export function assertRetainedBindingCase(item, kind, route, view, method) {
    fields(item, ['kind', 'route', 'view', 'method', 'events', 'port', 'request1', 'afterRequest1', 'request1SampleSequence', 'request2', 'beforeStop', 'finalSampleSequence', 'child', 'workerExit', 'stdout', 'stderr', 'finalSources']);
    equal([item.kind, item.route, item.view, item.method], [kind, route, view, method]); assert.ok(Number.isSafeInteger(item.port) && item.port > 0 && item.port < 65536);
    const expected = retainedBindingStateTemplate(kind, route, view, method, 'http://127.0.0.1:'+item.port+'/body');
    equal({request1: item.request1, request2: item.request2}, expected, 'every byte/backing, metadata, original pending closed, request ID, context and prototype across separate calls/requests');
    assert.equal(item.afterRequest1.length, 1); assert.equal(item.beforeStop.length, 1); equal(item.finalSources, item.beforeStop);
    const first = item.afterRequest1[0], last = item.beforeStop[0], packet = retainedBindingPacket(kind), split = packet.indexOf('\r\n\r\n')+4, request = 'GET /body HTTP/1.1\r\nHost: 127.0.0.1:'+item.port;
    fields(first, ['request', 'rawRequest', 'peerEnded', 'peerReset', 'headersSent', 'bodySent', 'fixtureEnded', 'fixtureDestroyed', 'events', 'sentHeader']);
    fields(last, [...Object.keys(first), 'sent']);
    const source = {request, rawRequest: Buffer.from(request+'\r\n\r\n').toString('base64'), peerEnded: false, peerReset: false, headersSent: true, bodySent: false, fixtureEnded: false, fixtureDestroyed: false, events: first.events, sentHeader: packet.subarray(0, split).toString('base64')};
    equal(first, source, 'the actual socket is still live with no body after request1 ends');
    equal(last, {...source, peerEnded: true, bodySent: true, fixtureEnded: true, events: last.events, sent: packet.toString('base64')}, 'actual complete bytes and FIN before worker shutdown');
    equal(first.events.map(event => event.name), ['request', 'headers-only']); equal(last.events.map(event => event.name), ['request', 'headers-only', 'body-after-request2', 'peer-ended']); equal(last.events.slice(0, 2), first.events);
    equal(item.events.map(event => event.name), ['request1-begun', 'request1-ended', 'continue-request2', 'request2-begun', 'request2-ended', 'finish']);
    const all = [];
    for (const events of [item.events, last.events]) {let previous = -1; for (const event of events) {fields(event, ['name', 'sequence']); assert.ok(Number.isSafeInteger(event.sequence) && event.sequence > previous); previous = event.sequence; all.push(event.sequence);}}
    assert.ok(Number.isSafeInteger(item.request1SampleSequence) && Number.isSafeInteger(item.finalSampleSequence)); all.push(item.request1SampleSequence, item.finalSampleSequence); equal(all.sort((a, b) => a-b), Array.from({length: 12}, (_, index) => index));
    const mark = Object.fromEntries(item.events.map(event => [event.name, event.sequence]));
    assert.ok(last.events[1].sequence < mark['request1-ended'] && mark['request1-ended'] < item.request1SampleSequence && item.request1SampleSequence < mark['continue-request2']);
    assert.ok(mark['continue-request2'] < mark['request2-begun'] && mark['request2-begun'] < last.events[2].sequence && last.events[2].sequence < item.finalSampleSequence);
    assert.ok(last.events[3].sequence < item.finalSampleSequence && mark['request2-ended'] < item.finalSampleSequence && item.finalSampleSequence < mark.finish);
    equal(item.child, {code: 0, signal: null}); equal(item.workerExit, {reaped: true, channelClosed: true, code: 0}); assert.equal(item.stderr, '');
    const lines = item.stdout.trim().split(/\r?\n/); assert.equal(lines.length, 6); assert.match(lines[0], /^memoryLimit=0 actualFeatures=(\d+) expectedFeatures=\1$/);
    equal(lines.slice(1), ['BINDING_REQUEST 1', 'BINDING_PHASE 1 '+JSON.stringify(item.request1), 'BINDING_REQUEST 2', 'BINDING_PHASE 2 '+JSON.stringify(item.request2), 'BINDING_EXIT '+JSON.stringify(item.workerExit)]);
    return expected;
}
export function validateRetainedBindingReport(report, expected) {
    fields(report, ['schema', 'complete', 'runId', 'kind', 'route', 'inputs', 'binaries', 'cases']); assert.equal(report.schema, 'capsid-binding-fetch-retained-v1'); assert.equal(report.complete, true);
    assert.equal(report.runId, expected.runId); assert.match(report.runId, /^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$/); assert.equal(report.kind, expected.kind); assert.equal(report.route, expected.route);
    assert.ok(retainedBindingKinds.includes(report.kind) && retainedBindingRoutes.includes(report.route)); equal(report.inputs, expected.inputs); equal(report.binaries, expected.binaries); assert.equal(report.cases.length, 26);
    return report.cases.map((item, index) => assertRetainedBindingCase(item, report.kind, report.route, retainedBindingViews[Math.floor(index/2)], retainedBindingMethods[index%2]));
}
