import assert from 'node:assert/strict';
import {test} from 'node:test';
import {readReaderIntrinsicSample} from './helpers/ws_reader_intrinsic_sample.mjs';
import {readerIntrinsicCases, expectedReaderIntrinsicState, assertReaderIntrinsicCase,
    validateReaderIntrinsicReport, compareReaderIntrinsicReports,
    readerIntrinsicSourceNames} from '../tools/workerd-reader-intrinsic-contract.mjs';
import {referenceBackendModes} from '../tools/workerd-platform.mjs';

const sample = readReaderIntrinsicSample();
const expected = {lock: sample.referenceLock, runId: sample.runId, inputs: sample.inputs,
    sourceNames: readerIntrinsicSourceNames('file:///intrinsic-fixture.mjs'),
    host: '1'.repeat(64), worker: '2'.repeat(64)};
const inventory = ['getReader', 'direct', 'prototype', 'subclass', 'reflect'].flatMap(route =>
    ['WeakMap.get', 'Promise.then', 'WeakMap.get+Promise.then', 'all'].flatMap(selection =>
        ['sync', 'deferred'].map(phase => `${route}|${selection}|${phase}`)));
test('forty explicit reader construction, intrinsic combination and scheduling cases', () =>
    assert.deepEqual(readerIntrinsicCases, inventory));
test('complete actual pinned reference passes the independent model', () =>
    assert.equal(validateReaderIntrinsicReport(sample, expected).length, 40));
for (const value of sample.cases) {
    test('actual reference ' + value.id, () => assertReaderIntrinsicCase(value, 'reference'));
    for (const [name, mutate] of [
        ['internal prototype call', state => state.traps.push('WeakMap.get')],
        ['second listener lost', state => state.trace.splice(state.trace.indexOf('second-listener'), 1)],
        ['incorrect synchronous deferral', state => {const i = state.trace.indexOf('mutated');
            const j = state.trace.indexOf('second-listener'); [state.trace[i], state.trace[j]] = [state.trace[j], state.trace[i]];}],
        ['input not transferred', state => {state.inputAfter = 32;}],
        ['payload changed', state => {state.read.backing[8] = 7;}],
        ['outside view overwritten', state => {state.read.backing[0] = 7;}],
        ['backing shortened', state => state.read.backing.pop()],
        ['wrong view offset', state => {state.read.offset = 0;}],
        ['read failed', state => {state.readError = {name: 'Error', message: 'application trap ArrayBuffer.isView'};}],
        ['closed rejected', state => {state.closed = {name: 'Error', message: 'application trap Promise.then'};}],
        ['closed identity changed', state => {state.closedDuringStable = false;}],
        ['lock retained', state => {state.locked = true;}],
        ['cancel failed', state => {state.cancelReturnedUndefined = false;}],
        ['unexpected field', state => {state.extra = true;}],
    ]) test('reject ' + name + ' ' + value.id, () => {
        const mutant = structuredClone(value); mutate(mutant.state.state);
        mutant.stateText = JSON.stringify(mutant.state);
        assert.throws(() => assertReaderIntrinsicCase(mutant, 'reference'));
    });
    for (const [name, mutate] of [
        ['worker exit', row => {row.stateStatus = 503; row.stateText = 'worker exited';}],
        ['stale worker', row => {row.state.realm = '1'.repeat(36); row.stateText = JSON.stringify(row.state);}],
        ['missing wire frame', row => row.wire.frames.pop()],
        ['changed raw wire', row => {row.wire.rawTail = '';}],
        ['wrong Upgrade challenge', row => {row.headers.requestKey = 'A'.repeat(22) + '==';}],
        ['forced cleanup', row => {row.shutdown.forced = true;}],
        ['runtime poisoned', row => {row.stderr += '\nWORKER POISONED: channel control sequence';}],
    ]) test('reject actual transport failure ' + name + ' ' + value.id, () => {
        const mutant = structuredClone(value); mutate(mutant);
        assert.throws(() => assertReaderIntrinsicCase(mutant, 'reference'));
    });
}
test('report completeness, actual client, inputs, binaries and full case inventory are mandatory', () => {
    for (const mutate of [report => {report.complete = false;}, report => report.cases.pop(),
        report => report.cases.push(report.cases[0]), report => {report.inputs.fixture = '0'.repeat(64);},
        report => {report.binaries.host = '0'.repeat(64);}, report => {report.client.node = 'v0.0.0';},
        report => {report.cases[1].challenge = report.cases[0].challenge;}]) {
        const mutant = structuredClone(sample); mutate(mutant);
        assert.throws(() => validateReaderIntrinsicReport(mutant, expected));
    }
});
test('a shared wrong result on every backend and the reference cannot pass', () => {
    const candidates = referenceBackendModes(expected.lock).map(mode => ({...structuredClone(sample), mode,
        sourceName: expected.sourceNames[mode], binaries: {host: expected.host, worker: expected.worker},
        cases: sample.cases.map(row => ({...structuredClone(row), shutdown: {code: 0, signal: null, forced: false}}))}));
    assert.deepEqual(compareReaderIntrinsicReports(sample, candidates, expected),
        {complete: true, casesPerBackend: 40, backends: candidates.length});
    const reference = structuredClone(sample), changed = structuredClone(candidates);
    for (const report of [reference, ...changed]) {
        const row = report.cases[0]; row.state.state.traps.push('Promise.then'); row.stateText = JSON.stringify(row.state);
    }
    assert.throws(() => compareReaderIntrinsicReports(reference, changed, expected));
    assert.deepEqual(expectedReaderIntrinsicState(inventory[0], '<challenge>').traps, []);
});
