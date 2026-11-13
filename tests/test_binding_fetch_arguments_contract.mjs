import assert from 'node:assert/strict';
import {test, after} from 'node:test';
import {createHash} from 'node:crypto';
import {bindingArgumentKinds, bindingArgumentRoutes, bindingArgumentViews, bindingArgumentMethods, bindingArgumentInputPaths,
    bindingArgumentPacket, bindingArgumentStateTemplate, expectedBindingArgumentEnvelope} from '../tools/binding-fetch-arguments-input.mjs';
import {assertBindingArgumentCase, validateBindingArgumentReport, loadBindingArgumentPrimaries,
    assertBindingArgumentReferenceReport, assertBindingArgumentReferenceArchive} from '../tools/binding-fetch-arguments-contract.mjs';

const freeze = value => {if (value && typeof value === 'object') {for (const child of Object.values(value)) freeze(child); Object.freeze(value);} return value;};
const hash = value => createHash('sha256').update(value).digest('hex');
const stdout = item => 'memoryLimit=0 actualFeatures=0 expectedFeatures=0\nBINDING_BODY '+JSON.stringify({state: item.state, calls: item.calls, wireTypes: item.wireTypes})+'\nBINDING_EXIT '+JSON.stringify(item.workerExit)+'\n';
// Cache only the lossless JSON encoding of deeply frozen construction inputs.
// Check the round trip before using that encoding; each counterexample still
// receives a fresh mutable graph and complete, independent validation.
const sampleJSON = new WeakMap();
function assertFrozenTree(value, seen = new Set()) {
    if (!value || typeof value !== 'object') return;
    assert.ok(Object.isFrozen(value), 'every construction input branch must be frozen');
    assert.ok(!seen.has(value), 'construction samples must be trees without shared object identities');
    seen.add(value);
    for (const child of Object.values(value)) assertFrozenTree(child, seen);
}
function copySample(base) {
    assert.ok(Object.isFrozen(base), 'only immutable construction samples may be serialized once');
    if (!sampleJSON.has(base)) {
        assertFrozenTree(base);
        const encoded = JSON.stringify(base);
        assert.deepEqual(JSON.parse(encoded), base, 'construction samples must round trip without losing values or types');
        sampleJSON.set(base, encoded);
    }
    return JSON.parse(sampleJSON.get(base));
}
let rejected = 0;
function sample(bodyKind, route, view, method) {
    const port = 19000, envelope = structuredClone(expectedBindingArgumentEnvelope(bodyKind, route, view, method)), request = 'GET /body HTTP/1.1\r\nHost: 127.0.0.1:'+port;
    const sources = Array.from({length: envelope.state.rows.length}, (_, index) => ({request, rawRequest: Buffer.from(request+'\r\n\r\n').toString('base64'),
        peerEnded: true, peerReset: false, fixtureEnded: true, fixtureDestroyed: false, sent: bindingArgumentPacket(bodyKind).toString('base64'),
        events: ['request', 'server-complete', 'peer-ended'].map((name, at) => ({name, sequence: index*3+at}))}));
    const item = {bodyKind, route, view, method, kind: envelope.state.kind, port, ...envelope, sourcesBeforeStop: sources, sampleSequence: 3*sources.length,
        finalSources: structuredClone(sources), child: {code: 0, signal: null}, workerExit: {reaped: true, channelClosed: true, code: 0}, stdout: '', stderr: ''}; item.stdout = stdout(item); return item;
}
function reject(base, change, syncFinal = true, syncStdout = true) {
    const item = copySample(base); change(item); if (syncFinal) item.finalSources = structuredClone(item.sourcesBeforeStop); if (syncStdout) item.stdout = stdout(item);
    assert.throws(() => assertBindingArgumentCase(item, base.bodyKind, base.route, base.view, base.method)); rejected++;
}
function leaves(value, prefix = []) {
    if (!value || typeof value !== 'object') return [prefix];
    return Object.entries(value).flatMap(([key, child]) => leaves(child, [...prefix, key]));
}
const changeLeaf = (item, path) => {let parent = item; for (const key of path.slice(0, -1)) parent = parent[key]; const key = path.at(-1), value = parent[key];
    parent[key] = value === null ? false : typeof value === 'boolean' ? Number(value) : typeof value === 'number' ? value+1 : typeof value === 'string' ? value+'!' : null;};
for (const bodyKind of bindingArgumentKinds) for (const route of bindingArgumentRoutes) test(bodyKind+'/'+route+': exact valid family and typed mutable counterexamples', () => {
    for (const view of bindingArgumentViews) for (const method of bindingArgumentMethods) assertBindingArgumentCase(sample(bodyKind, route, view, method), bodyKind, route, view, method);
    const base = freeze(sample(bodyKind, route, bodyKind === 'raw' ? 'Float64Array' : 'DataView', bodyKind === 'raw' ? 'read' : 'least'));
    for (const key of Object.keys(base)) reject(base, item => {delete item[key];}, false, false);
    for (let index = 0; index < base.state.rows.length; index++) {
        const row = base.state.rows[index];
        for (const key of Object.keys(row)) reject(base, item => {delete item.state.rows[index][key];});
        for (const path of [['events'], ['response', 'used'], ['construction', 'instanceOfReader'], ['afterCall', 'bufferBytes'], ['afterCall', 'locked'], ['eof', 'done'], ['eof', 'bufferBytes'], ['cancelReturnedUndefined'], ['lockedAfterRelease'], ['bodyUsed']]) {
            reject(base, item => {if (path[0] === 'events') item.state.rows[index].events.push('wrong-order'); else changeLeaf(item.state.rows[index], path);});
        }
        const error = row.read?.error ?? row.synchronous;
        if (error) for (const key of Object.keys(error)) reject(base, item => {changeLeaf(item.state.rows[index], [row.synchronous ? 'synchronous' : 'read', ...(row.synchronous ? [] : ['error']), key]);});
        for (const key of Object.keys(base.calls[index])) reject(base, item => {changeLeaf(item.calls[index], [key]);});
        for (const key of Object.keys(base.wireTypes[index])) reject(base, item => {changeLeaf(item.wireTypes[index], [key]);});
    }
    for (const index of [0, base.state.rows.length-1]) for (const path of leaves(base.state.rows[index])) reject(base, item => {changeLeaf(item.state.rows[index], path);});
    for (const key of Object.keys(base.sourcesBeforeStop[0])) reject(base, item => {delete item.sourcesBeforeStop[0][key];});
    for (const change of [item => {item.sourcesBeforeStop[0].peerEnded = false;}, item => {item.sourcesBeforeStop[0].peerReset = true;}, item => {item.sourcesBeforeStop[0].fixtureDestroyed = true;},
        item => {item.sourcesBeforeStop[0].sent = '';}, item => {item.sourcesBeforeStop[0].events[1].sequence = 0;}, item => {item.sourcesBeforeStop[0].events[2].sequence = 99999;},
        item => {item.sourcesBeforeStop[0].rawRequest += 'AA==';}, item => {item.sourcesBeforeStop.pop();}, item => {item.sampleSequence--;}, item => {item.workerExit.code = 1;}, item => {item.child.code = 1;}, item => {item.stderr = 'unexpected';}]) reject(base, change);
    reject(base, item => {item.finalSources[0].peerEnded = false;}, false); reject(base, item => {item.stdout += 'manufactured\n';}, true, false);
    const mutable = copySample(base);
    assertBindingArgumentCase(mutable, bodyKind, route, base.view, base.method); mutable.calls[0].signalNew = false; mutable.stdout = stdout(mutable);
    assert.throws(() => assertBindingArgumentCase(mutable, bodyKind, route, base.view, base.method)); rejected++;
});
test('both actual primaries are checked against the input model even when their rows are wrong together', () => {
    const {archive, metadata} = loadBindingArgumentPrimaries();
    const reports = archive.reports.map(record => JSON.parse(record.originalTextUTF8));
    const changes = [report => {report.cases[0].state.rows[0].response.used = true;}, report => {report.cases[0].state.rows[1].originalBacking[0] = 0;},
        report => {report.cases[0].state.rows[1].retry.backing[0] = 0;}, report => {report.cases[0].sourcesBeforeStop[0].peerEnded = false; report.cases[0].finalSources[0].peerEnded = false;},
        report => {report.referenceLock.compatibility_date = '2026-08-21';}, report => {report.binaries.reference = '0'.repeat(64);}];
    for (const change of changes) for (const [index, original] of reports.entries()) {
        // Share untouched actual groups; copy every mutable branch changed here.
        const report = {...original, cases: original.cases.slice(), referenceLock: structuredClone(original.referenceLock), binaries: {...original.binaries}};
        report.cases[0] = structuredClone(original.cases[0]); change(report);
        assert.throws(() => assertBindingArgumentReferenceReport(report, index ? 'linux' : 'native', metadata.captureRunnerSHA256)); rejected++;
    }
    const bad = {...archive, reports: archive.reports.map(item => ({...item}))}; bad.reports[0].originalTextUTF8 += ' ';
    assert.throws(() => assertBindingArgumentReferenceArchive(bad, metadata)); rejected++;
    bad.reports[0].originalReportSHA256 = hash(Buffer.from(bad.reports[0].originalTextUTF8));
    assert.throws(() => assertBindingArgumentReferenceArchive(bad, metadata)); rejected++;
});
test('current full report, identity, group order and a previously accepted object are revalidated', () => {
    const inputs = Object.fromEntries(Object.keys(bindingArgumentInputPaths).map(key => [key, 'a'.repeat(64)])), binaries = {helper: 'b'.repeat(64), worker: 'c'.repeat(64)};
    const report = {schema: 'capsid-binding-fetch-arguments-v1', complete: true, runId: 'synthetic-controls', bodyKind: 'raw', route: 'getReader', inputs, binaries,
        cases: bindingArgumentViews.flatMap(view => bindingArgumentMethods.map(method => sample('raw', 'getReader', view, method)))};
    const expected = {runId: report.runId, bodyKind: report.bodyKind, route: report.route, inputs: structuredClone(inputs), binaries: structuredClone(binaries)}; validateBindingArgumentReport(report, expected);
    for (const change of [item => {item.schema = '';}, item => {item.complete = 1;}, item => {item.runId = 'changed';}, item => {item.bodyKind = 'gzip';}, item => {item.route = 'direct';},
        item => {item.inputs.priorModel = 'd'.repeat(64);}, item => {item.binaries.worker = 'd'.repeat(64);}, item => {item.cases.pop();}, item => {item.cases[1] = structuredClone(item.cases[0]);}]) {
        const bad = structuredClone(report); change(bad); assert.throws(() => validateBindingArgumentReport(bad, expected)); rejected++;
    }
    report.cases[0].state.rows[1].retry.bytes[0] = 0; report.cases[0].stdout = stdout(report.cases[0]); assert.throws(() => validateBindingArgumentReport(report, expected)); rejected++;
});
test('input templates are deeply immutable, sample copies independent and the32 entry FIFO bound exact', () => {
    const selections = bindingArgumentKinds.flatMap(kind => bindingArgumentRoutes.flatMap(route => bindingArgumentViews.flatMap(view => bindingArgumentMethods.map(method => [kind, route, view, method]))));
    for (const selection of selections) bindingArgumentStateTemplate(...selection);
    const first = bindingArgumentStateTemplate(...selections[0]);
    assert.ok(Object.isFrozen(first) && Object.isFrozen(first.rows) && Object.isFrozen(first.rows[1].retry.backing) && Object.isFrozen(expectedBindingArgumentEnvelope(...selections[0]).calls[0]));
    assert.throws(() => {first.rows[1].retry.backing[0] = 0;}, TypeError); const copy = structuredClone(first); copy.rows[1].retry.backing[0] = 0; assert.equal(first.rows[1].retry.backing[0], 1);
    for (const selection of selections.slice(1, 32)) bindingArgumentStateTemplate(...selection); assert.equal(bindingArgumentStateTemplate(...selections[0]), first);
    bindingArgumentStateTemplate(...selections[32]); assert.notEqual(bindingArgumentStateTemplate(...selections[0]), first);
});
test('lossless immutable samples produce independent complete mutable counterexamples', () => {
    const base = freeze(sample('raw', 'getReader', 'Float64Array', 'read'));
    const left = copySample(base), right = copySample(base);
    assert.deepEqual(left, structuredClone(base)); assert.deepEqual(right, structuredClone(base));
    assert.notEqual(left, right); assert.notEqual(left.state.rows[1].retry.backing, right.state.rows[1].retry.backing);
    assert.ok(!Object.isFrozen(left) && !Object.isFrozen(left.state.rows[1].retry.backing));
    assertBindingArgumentCase(left, 'raw', 'getReader', 'Float64Array', 'read');
    left.state.rows[1].retry.backing[0] = 0; left.stdout = stdout(left);
    assert.throws(() => assertBindingArgumentCase(left, 'raw', 'getReader', 'Float64Array', 'read'));
    assert.equal(right.state.rows[1].retry.backing[0], 1); assert.equal(base.state.rows[1].retry.backing[0], 1);
    assertBindingArgumentCase(right, 'raw', 'getReader', 'Float64Array', 'read');
    assert.throws(() => copySample(sample('raw', 'getReader', 'Float64Array', 'read')));
    for (const value of [undefined, NaN, Infinity, -Infinity, -0, 1n, new Date(0), new Uint8Array(), Symbol('value')]) {
        const input = freeze({value});
        assert.throws(() => copySample(input), 'a non-lossless construction input must be rejected');
    }
    assert.throws(() => copySample(Object.freeze({branch: {value: 1}})), /every construction input branch must be frozen/);
    const shared = freeze({value: 1});
    assert.throws(() => copySample(freeze({left: shared, right: shared})), /without shared object identities/);
});

after(() => {assert.equal(rejected, 19189); console.log(JSON.stringify({complete: true, uniqueArgumentInputs: 10920, rejectedMutations: rejected}));});
