import assert from 'node:assert/strict';
import {test, after} from 'node:test';
import fs from 'node:fs';
import {createHash} from 'node:crypto';
import {bindingFetchKinds, bindingFetchViewGroups, bindingFetchPacket, bindingFetchInputPaths, expectedBindingFetchState, bindingFetchStateTemplate} from '../tools/binding-fetch-body-input.mjs';
import {assertBindingFetchCase, assertBindingFetchReferenceArchive, validateBindingFetchReport} from '../tools/binding-fetch-body-contract.mjs';

const freeze = value => {if (value && typeof value === 'object') {for (const item of Object.values(value)) freeze(item); Object.freeze(value);} return value;};
const hash = value => createHash('sha256').update(value).digest('hex');
let rejected = 0;
const stdout = row => 'memoryLimit=0 actualFeatures=0 expectedFeatures=0\nBINDING_BODY '+JSON.stringify(row.state)+'\nBINDING_EXIT '+JSON.stringify(row.workerExit)+'\n';
function sample(kind, group) {
    const port = 19000, state = expectedBindingFetchState(kind, group, 'http://127.0.0.1:'+port+'/body');
    const request = 'GET /body HTTP/1.1\r\nHost: 127.0.0.1:'+port, packet = bindingFetchPacket(kind).toString('base64');
    const count = 2*state.batch.length+31;
    const sources = Array.from({length: count}, (_, index) => ({request, rawRequest: Buffer.from(request+'\r\n\r\n').toString('base64'),
        peerEnded: true, peerReset: false, fixtureEnded: true, fixtureDestroyed: false, sent: packet,
        events: ['request', 'server-complete', 'peer-ended'].map((name, at) => ({name, sequence: 3*index+at}))}));
    freeze(sources);
    const row = {group, views: bindingFetchViewGroups[group], port, state, sourcesBeforeFinish: sources, sampleSequence: 3*count,
        finalSources: sources, child: {code: 0, signal: null}, workerExit: {reaped: true, channelClosed: true, code: 0}, stdout: '', stderr: ''};
    row.stdout = stdout(row); return row;
}
function reject(row, kind, group, change) {
    // The synthetic base is frozen and separate from the expected model.
    // Copy only paths changed by this counterexample. Validators receive
    // ordinary values and still inspect the complete current mutable report.
    const bad = changeFrozenSample(row, change); bad.stdout = stdout(bad);
    assert.throws(() => assertBindingFetchCase(bad, kind, group)); rejected++;
}
// Used only to construct negative controls from deeply frozen synthetic data.
// Actual runtime reports never pass through this helper.
function changeFrozenSample(base, change) {
    assert.ok(Object.isFrozen(base));
    const draft = value => {
        assert.ok(Object.isFrozen(value));
        let copy;
        const children = new Map();
        const writable = () => copy ??= Array.isArray(value) ? value.slice() : {...value};
        const current = () => copy ?? value;
        const proxy = new Proxy(Array.isArray(value) ? [] : {}, {
            get(_, key) {
                const item = current()[key];
                if (!item || typeof item !== 'object') return item;
                if (!children.has(key)) children.set(key, draft(item));
                return children.get(key).proxy;
            },
            set(_, key, item) { writable()[key] = item; children.delete(key); return true; },
            deleteProperty(_, key) { delete writable()[key]; children.delete(key); return true; },
        });
        const finish = () => {
            for (const [key, child] of children) {
                const item = child.finish();
                if (item !== current()[key]) writable()[key] = item;
            }
            return current();
        };
        return {proxy, finish};
    };
    const state = draft(base); change(state.proxy);
    // stdout is refreshed by the caller even when the changed path is external
    // to state, so always return an independently mutable outer object.
    return {...state.finish()};
}

for (const kind of bindingFetchKinds) for (let group = 0; group < bindingFetchViewGroups.length; group++) test('Binding complete current model '+kind+' group'+group, () => {
    const row = freeze(sample(kind, group)); assertBindingFetchCase(row, kind, group);
    for (const key of Object.keys(row.state)) reject(row, kind, group, bad => {delete bad.state[key];});
    for (const phase of ['batch', 'structured']) for (const index of [0, Math.floor(row.state[phase].length/2), row.state[phase].length-1]) {
        const value = row.state[phase][index];
        for (const key of Object.keys(value)) reject(row, kind, group, bad => {delete bad.state[phase][index][key];});
        for (let read = 0; read < value.reads.length; read++) {
            for (const key of Object.keys(value.reads[read])) reject(row, kind, group, bad => {delete bad.state[phase][index].reads[read][key];});
            for (const at of [0, 63]) reject(row, kind, group, bad => {bad.state[phase][index].reads[read].backing[at] = 0;});
            if (value.reads[read].bytes.length) reject(row, kind, group, bad => {bad.state[phase][index].reads[read].bytes[0] = 0;});
            reject(row, kind, group, bad => {bad.state[phase][index].reads[read].inputAfter = false;});
            reject(row, kind, group, bad => {bad.state[phase][index].reads[read].done = 1;});
        }
        for (const key of Object.keys(value.metadata)) reject(row, kind, group, bad => {delete bad.state[phase][index].metadata[key];});
        for (const change of [item => {item.brand = 1;}, item => {item.closedStable = 1;}, item => {item.lockedAfter = true;}, item => {item.bodyUsedAfter = false;}, item => {item.closed = 'pending';}]) reject(row, kind, group, bad => change(bad.state[phase][index]));
    }
    for (const index of [0, Math.floor(row.state.calls.length/2), row.state.calls.length-1]) {
        for (const key of Object.keys(row.state.calls[index])) reject(row, kind, group, bad => {delete bad.state.calls[index][key];});
        for (const change of [item => {item.frozen = 1;}, item => {item.signalNew = false;}, item => {item.requestId = '2';}, item => {item.deadlineType = 'number';}]) reject(row, kind, group, bad => change(bad.state.calls[index]));
        for (const key of Object.keys(row.state.rowWireTypes[index])) {
            reject(row, kind, group, bad => {delete bad.state.rowWireTypes[index][key];});
            reject(row, kind, group, bad => {bad.state.rowWireTypes[index][key] = false;});
        }
    }
    for (const key of Object.keys(row.state.recovery)) reject(row, kind, group, bad => {delete bad.state.recovery[key];});
    for (const change of [bad => {bad.state.recovery.reads[0].inputAfter = false;}, bad => {bad.state.recovery.reads[0].backing[63] = 0;}, bad => {bad.state.recovery.closed = 'pending';}]) reject(row, kind, group, change);
    for (const key of Object.keys(row.state.recoveryCall)) reject(row, kind, group, bad => {delete bad.state.recoveryCall[key];});
    reject(row, kind, group, bad => {bad.state.recoveryCall.signalNew = false;});
    for (const key of Object.keys(row.state.oversized)) reject(row, kind, group, bad => {delete bad.state.oversized[key];});
    for (const change of [bad => {bad.state.oversized.fulfilled = true;}, bad => {bad.state.oversized.errorPrototype = false;}, bad => {bad.state.oversized.message = '';},
        bad => {bad.state.events.pop();}, bad => {[bad.state.events[0], bad.state.events[1]] = [bad.state.events[1], bad.state.events[0]];}, bad => {bad.state.matrixCloneNodes--;}, bad => {bad.state.maxRowCloneNodes++;},
        bad => {bad.sourcesBeforeFinish[0].peerEnded = false;}, bad => {bad.sourcesBeforeFinish[0].sent = '';}, bad => {bad.sourcesBeforeFinish[0].events[0].sequence = 3;}, bad => {bad.sampleSequence--;}, bad => {bad.finalSources.pop();},
        bad => {bad.child.code = 1;}, bad => {bad.workerExit.code = 1;}, bad => {bad.stderr = 'unexpected';}]) reject(row, kind, group, change);
    for (const index of [0, 35, 69]) {
        for (const key of Object.keys(row.state.completedCalls[index])) reject(row, kind, group, bad => {delete bad.state.completedCalls[index][key];});
        for (const change of [value => {value.value.index++;}, value => {value.inputWasNullPrototype = false;}, value => {value.outputNullPrototype = false;}, value => {value.call.signalNew = false;}, value => {value.call.requestId = '2';}]) reject(row, kind, group, bad => change(bad.state.completedCalls[index]));
    }
    const mutable = sample(kind, group); assertBindingFetchCase(mutable, kind, group); mutable.state.calls[0].signalNew = false; mutable.stdout = stdout(mutable);
    assert.throws(() => assertBindingFetchCase(mutable, kind, group)); rejected++;
});

test('both actual primaries cannot bless the same incorrect rows or wire', () => {
    const archive = JSON.parse(fs.readFileSync(new URL('./fixtures/workerd-binding-fetch-body-report.json', import.meta.url), 'utf8'));
    const fixture = hash(fs.readFileSync(new URL('./fixtures/workerd-binding-fetch-body.mjs', import.meta.url))); assertBindingFetchReferenceArchive(archive, fixture);
    for (const kind of bindingFetchKinds) for (const change of [item => {item.rows[0].reads[0].backing[0] = 0;}, item => {item.rows[0].metadata.status = 200;}, item => {item.sourcesBeforeReferenceStop[0].peerEnded = false;},
        (_, report) => {report.referenceLock.compatibility_date = '2026-08-21';}, (_, report) => {report.binarySHA256 = '0'.repeat(64);}, item => {item.views[0] = 'invalid-view';}]) {
        const bad = structuredClone(archive);
        for (const record of bad.reports) {
            const report = JSON.parse(record.originalTextUTF8); change(report.cases.find(item => item.kind === kind), report);
            record.originalTextUTF8 = JSON.stringify(report); record.originalReportSHA256 = hash(Buffer.from(record.originalTextUTF8));
        }
        assert.throws(() => assertBindingFetchReferenceArchive(bad, fixture)); rejected++;
    }
    archive.reports[0].originalTextUTF8 += ' '; assert.throws(() => assertBindingFetchReferenceArchive(archive, fixture)); rejected++;
});
test('full mutable report is checked again on every invocation', () => {
    const inputs = Object.fromEntries(Object.keys(bindingFetchInputPaths).map(key => [key, 'a'.repeat(64)])), binaries = {helper: 'b'.repeat(64), worker: 'c'.repeat(64)};
    const report = {schema: 'capsid-binding-fetch-body-v1', complete: true, runId: 'synthetic-controls', kind: 'raw', inputs, binaries, cases: bindingFetchViewGroups.map((_, group) => sample('raw', group))};
    const expected = {runId: report.runId, kind: report.kind, inputs: structuredClone(inputs), binaries: structuredClone(binaries)}; validateBindingFetchReport(report, expected);
    for (const change of [bad => {bad.schema = '';}, bad => {bad.runId = '';}, bad => {bad.kind = 'gzip';}, bad => {bad.inputs.sourceModel = 'd'.repeat(64);}, bad => {bad.binaries.worker = 'd'.repeat(64);},
        bad => {bad.cases.pop();}, bad => {bad.cases[1] = structuredClone(bad.cases[0]);}, bad => {bad.complete = false;}]) {
        const bad = structuredClone(report); change(bad); assert.throws(() => validateBindingFetchReport(bad, expected)); rejected++;
    }
    report.cases[0].state.calls[0].signalNew = false; report.cases[0].stdout = stdout(report.cases[0]);
    assert.throws(() => validateBindingFetchReport(report, expected)); rejected++;
});
after(() => {assert.equal(rejected, 5622); console.log(JSON.stringify({complete: true, uniqueBodyViewInputs: 260, rejectedMutations: rejected}));});

test('construction templates are deeply frozen, copied for samples and bounded', () => {
    const first = bindingFetchStateTemplate('raw', 3, 'http://127.0.0.1:20000/body');
    assert.ok(Object.isFrozen(first) && Object.isFrozen(first.batch) && Object.isFrozen(first.batch[0].reads[0].backing) && Object.isFrozen(first.completedCalls[0].call));
    assert.throws(() => {first.batch[0].reads[0].backing[0] = 0;}, TypeError);
    assert.throws(() => {first.completedCalls[0].call.signalNew = false;}, TypeError);
    const copy = expectedBindingFetchState('raw', 3, 'http://127.0.0.1:20000/body'); copy.batch[0].reads[0].backing[0] = 0;
    assert.equal(first.batch[0].reads[0].backing[0], 165);
    for (let index = 1; index <= 31; index++) bindingFetchStateTemplate('raw', 3, 'http://127.0.0.1:'+(20000+index)+'/body');
    assert.equal(bindingFetchStateTemplate('raw', 3, 'http://127.0.0.1:20000/body'), first);
    bindingFetchStateTemplate('raw', 3, 'http://127.0.0.1:20032/body');
    const rebuilt = bindingFetchStateTemplate('raw', 3, 'http://127.0.0.1:20000/body'); assert.notEqual(rebuilt, first); assert.deepEqual(rebuilt, first);
});

test('negative construction copies changed paths without changing its frozen base', () => {
    const base = freeze({nested: {values: [{key: 1}, {key: 2}], gone: true}, untouched: {key: 3}, words: ['a', 'b']});
    const bad = changeFrozenSample(base, row => {
        row.nested.values[0].key++;
        delete row.nested.gone;
        row.nested.values.pop();
        [row.words[0], row.words[1]] = [row.words[1], row.words[0]];
    });
    assert.deepEqual(bad, {nested: {values: [{key: 2}]}, untouched: {key: 3}, words: ['b', 'a']});
    assert.deepEqual(base, {nested: {values: [{key: 1}, {key: 2}], gone: true}, untouched: {key: 3}, words: ['a', 'b']});
    assert.notEqual(bad, base); assert.notEqual(bad.nested, base.nested);
    assert.notEqual(bad.nested.values, base.nested.values); assert.notEqual(bad.nested.values[0], base.nested.values[0]);
    assert.equal(bad.untouched, base.untouched); assert.ok(Object.isFrozen(bad.untouched));
    assert.deepEqual(changeFrozenSample(base, row => {row.words.pop();}).words, ['a']);
});
