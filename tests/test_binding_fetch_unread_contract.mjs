import assert from 'node:assert/strict';
import {test} from 'node:test';
import {unreadBindingPlans, unreadBindingKinds, unreadBindingCases,
    expectedUnreadBindingState, expectedUnreadBindingSources} from '../tools/binding-fetch-unread-input.mjs';
import {unreadBindingExit, assertUnreadBindingCase, validateUnreadBindingReport} from '../tools/binding-fetch-unread-contract.mjs';

const clone = structuredClone;
let rejected = 0;
const rejects = (label, action) => { assert.throws(action, undefined, label); rejected++; };
function output(row) {
    const features = process.platform === 'win32' ? 0 : 1;
    row.stdout = [`memoryLimit=0 actualFeatures=${features} expectedFeatures=${features}`, 'BINDING_REQUEST 1',
        'BINDING_PHASE 1 ' + JSON.stringify(row.request1), 'BINDING_REQUEST 2',
        'BINDING_PHASE 2 ' + JSON.stringify(row.request2), 'BINDING_EXIT ' + JSON.stringify(row.workerExit)].join('\n') + '\n';
    return row;
}
function makeCase(plan) {
    const live = expectedUnreadBindingSources(plan, 12345), closed = expectedUnreadBindingSources(plan, 12345, true);
    return output({plan, collectionComplete: true, port: 12345, request1: expectedUnreadBindingState(plan, 1),
        sourceAfterRequest1: live, request2: expectedUnreadBindingState(plan, 2), sourceBeforeStop: clone(live),
        workerExit: clone(unreadBindingExit), helperExit: {code: 0, signal: null}, sourceTerminalAfterWorkerExit: true,
        sourceAfterWorkerExit: closed, stderr: '', sourceFinalBeforeFixtureCleanup: clone(closed)});
}
function makeReport(kind) {
    const expected = {kind, runId: 'synthetic-unread-controls', inputs: {binding: 'a'.repeat(64)},
        binaries: {helper: 'b'.repeat(64), worker: 'c'.repeat(64)}};
    return {expected, report: {schema: 'capsid-binding-fetch-unread-v1', complete: true, ...expected,
        cases: unreadBindingCases(kind).map(makeCase)}};
}

test('all24 independent single/four-resource unread, locked and pending models are valid', () => {
    assert.equal(unreadBindingPlans.length, 24);
    assert.equal(new Set(unreadBindingPlans.map(plan => plan.id)).size, 24);
    for (const plan of unreadBindingPlans) assertUnreadBindingCase(makeCase(plan), plan);
});

test('all24 cases reject coherent changes to request ownership, state, pending reads and input detachment', () => {
    for (const plan of unreadBindingPlans) for (const mutate of [
        row => row.request1.requestId = '3', row => row.request2.requestId = '1',
        row => row.request1.openSignalAborted = true, row => row.request2.openSignalAborted = true,
        row => row.request1.resources[0].bodyUsed = !row.request1.resources[0].bodyUsed,
        row => row.request2.resources[0].locked = !row.request2.resources[0].locked,
        row => row.request1.resources[0].hasReader = !row.request1.resources[0].hasReader,
        row => row.request2.resources[0].closedPending = false,
        row => row.request2.resources[0].firstPending = false,
        row => row.request2.resources[0].inputAfterIssue = 99,
        row => row.request2.resources[0].headers.pop(), row => row.request2.resources.push(clone(row.request2.resources[0])),
    ]) {
        const row = makeCase(plan); mutate(row); output(row);
        rejects(plan.id, () => assertUnreadBindingCase(row, plan));
    }
});

test('all24 cases require every real source to stay live across both User requests', () => {
    for (const plan of unreadBindingPlans) for (const mutate of [
        sources => { sources[0].peerEnded = true; sources[0].events.push('peer-ended'); },
        sources => sources[0].peerReset = true, sources => sources[0].fixtureEnded = true,
        sources => sources[0].sent[0] = 'AA==', sources => sources[0].rawRequest = 'AA==',
        sources => sources.pop(), sources => sources.unshift(clone(sources[0])), sources => sources[0].slot = 999,
    ]) {
        const row = makeCase(plan); mutate(row.sourceAfterRequest1); row.sourceBeforeStop = clone(row.sourceAfterRequest1);
        rejects(plan.id, () => assertUnreadBindingCase(row, plan));
    }
});

test('all24 cases reject fixture-created closure, body release, incomplete peers or rewritten wire', () => {
    for (const plan of unreadBindingPlans) for (const mutate of [
        sources => { sources[0].peerEnded = sources[0].peerReset = false; sources[0].events.pop(); },
        sources => sources[0].fixtureDestroyed = true, sources => sources[0].fixtureEnded = true,
        sources => sources[0].sent.push('AA=='), sources => sources[0].rawRequest = 'AA==',
        sources => sources[0].events.reverse(), sources => sources.pop(), sources => sources[0].slot = 999,
    ]) {
        const row = makeCase(plan); mutate(row.sourceAfterWorkerExit);
        row.sourceFinalBeforeFixtureCleanup = clone(row.sourceAfterWorkerExit);
        rejects(plan.id, () => assertUnreadBindingCase(row, plan));
    }
});

test('all24 cases require actual C ABI output and natural process exits without missing fields', () => {
    for (const plan of unreadBindingPlans) for (const mutate of [
        row => row.sourceTerminalAfterWorkerExit = false,
        row => row.sourceFinalBeforeFixtureCleanup[0].fixtureDestroyed = true,
        row => row.helperExit.code = 1,
        row => { row.workerExit.code = 1; output(row); },
        row => { row.workerExit.reaped = false; output(row); },
        row => { row.workerExit.channelClosed = false; output(row); },
        row => row.stderr = 'runtime error:',
        row => row.stdout = row.stdout.replace('BINDING_REQUEST 2\n', ''),
        row => row.stdout += 'unknown output\n',
        row => row.stdout = row.stdout.replace('BINDING_PHASE 1 {', 'BINDING_PHASE 1 {"wrong":true,'),
        row => row.collectionComplete = false, row => row.extra = true,
    ]) {
        const row = makeCase(plan); mutate(row);
        rejects(plan.id, () => assertUnreadBindingCase(row, plan));
    }
});

test('both complete shards require ordered cases and exact caller-supplied input/binary/run identities', () => {
    for (const kind of unreadBindingKinds) {
        const {report, expected} = makeReport(kind); validateUnreadBindingReport(report, expected);
        for (const mutate of [value => value.cases.pop(), value => value.cases.push(clone(value.cases[0])),
            value => value.cases.reverse(), value => value.runId += '-other',
            value => value.binaries.helper = 'f'.repeat(64), value => value.binaries.worker = 'f'.repeat(64),
            value => value.inputs.binding = 'f'.repeat(64), value => value.schema += '-other',
            value => value.kind = 'other', value => value.complete = false, value => delete value.inputs,
            value => value.cases[0].extra = true]) {
            const changed = clone(report); mutate(changed);
            rejects(kind, () => validateUnreadBindingReport(changed, expected));
        }
    }
});

test('accepted mutable reports and cases are revalidated after coherent mutation', () => {
    const {report, expected} = makeReport('raw'); validateUnreadBindingReport(report, expected);
    report.cases[0].request2.openSignalAborted = true; output(report.cases[0]);
    rejects('accepted report mutated', () => validateUnreadBindingReport(report, expected));
    const plan = unreadBindingPlans.find(plan => plan.id === 'gzip-pending-byob-4'), row = makeCase(plan);
    assertUnreadBindingCase(row, plan); row.request2.resources[3].firstPending = false; output(row);
    rejects('accepted independent resource mutated', () => assertUnreadBindingCase(row, plan));
});

test('model controls count mutations separately from actual product execution', () => {
    assert.equal(rejected, 986);
    console.log('BINDING_UNREAD_CONTROLS ' + JSON.stringify({modelCases: 24, rejectedMutations: rejected,
        actualProductProgramsExecuted: 0}));
});
