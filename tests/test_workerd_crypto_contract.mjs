import {referenceBackendModes} from '../tools/workerd-platform.mjs';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import test from 'node:test';
import {cryptoCases, assertCryptoCase, validateCryptoReport, compareCryptoReports} from '../tools/workerd-crypto-contract.mjs';

const stored = JSON.parse(fs.readFileSync(new URL('./fixtures/workerd-crypto-report.json', import.meta.url)));
function matrix() {
    const reference = structuredClone(stored);
    const expected = {lock: reference.referenceLock, runId: reference.runId, inputs: reference.inputs,
        host: 'a'.repeat(64), worker: 'b'.repeat(64)};
    const candidates = referenceBackendModes(expected.lock).map(mode => {
        const report = structuredClone(reference);
        report.mode = mode; report.binaries = {host: expected.host, worker: expected.worker};
        for (const value of report.cases) value.shutdown = {code: 0, signal: null, forced: false};
        return report;
    });
    return {reference, candidates, expected};
}
function change(value, mutate, field = 'observed') {
    const document = JSON.parse(value[field].text); mutate(document.state, document);
    value[field].text = JSON.stringify(document);
}
test('actual native workerd run satisfies the independent complete contract', () => {
    assert.deepEqual(validateCryptoReport(stored, 'reference'), {complete: true, cases: 42});
    const {reference, candidates, expected} = matrix();
    assert.deepEqual(compareCryptoReports(reference, candidates, expected), {complete: true, casesPerBackend: 42, backends: candidates.length});
});
for (const kind of cryptoCases) for (const [name, mutate] of [
    ['lost completion', value => change(value, state => state.trace.splice(2, 1))],
    ['late callback', value => change(value, state => state.trace.push('late'))],
    ['different result', value => change(value, state => {state.result = 'wrong';})],
    ['different failure', value => change(value, state => {state.failure = {name: 'WrongError'};})],
    ['different getter reads', value => change(value, state => state.reads.push('extra'))],
    ['different return mode', value => change(value, state => {state.returned = 'wrong';})],
    ['different mutation', value => change(value, state => {state.mutation = {succeeded: false};})],
    ['different realm', value => change(value, (state, document) => {document.realm = '00000000-0000-4000-8000-000000000001';})],
    ['different challenge', value => {value.challenge = '00000000-0000-4000-8000-000000000002';}],
    ['truncated raw bytes', value => {value.wire.rawTail = '';}],
    ['missing frame', value => value.wire.frames.pop()],
    ['missing Close ACK', value => {value.wire.closeReplySent = false;}],
    ['local disposal', value => {value.wire.completion = 'client-disposed';}],
    ['different Upgrade key', value => {value.headers.requestKey = 'A'.repeat(22) + '==';}],
    ['forced shutdown', value => {value.shutdown.forced = true;}],
    ['runtime diagnostic', value => {value.stderr += 'ERROR: AddressSanitizer: heap-use-after-free';}],
]) test(kind + ' rejects ' + name, () => {
    const value = structuredClone(stored.cases.find(entry => entry.kind === kind)); mutate(value);
    assert.throws(() => assertCryptoCase(value, 'reference'));
});
for (const kind of cryptoCases) test(kind + ' rejects identical wrong behavior on every backend', () => {
    const {reference, candidates, expected} = matrix();
    for (const report of [reference, ...candidates])
        change(report.cases.find(value => value.kind === kind), state => state.trace.push('shared-wrong'));
    assert.throws(() => compareCryptoReports(reference, candidates, expected), /complete same-realm state/);
});
for (const kind of ['crypto-key-map-prototype', 'crypto-subtle-set-prototype'])
    test(kind + ' rejects an internal collection observation', () => {
        const value = structuredClone(stored.cases.find(entry => entry.kind === kind));
        change(value, state => state.observations.push({tag:'[object Uint8Array]',bytes:Array(32).fill(0)}));
        assert.throws(() => assertCryptoCase(value, 'reference'), /complete same-realm state/);
    });
for (const [name, mutate] of [
    ['missing case', ({reference}) => reference.cases.pop()],
    ['duplicate case', ({reference}) => {reference.cases[1] = reference.cases[0];}],
    ['uncollected report', ({reference}) => {reference.complete = false;}],
    ['stale binary', ({candidates}) => {candidates[0].binaries.worker = 'c'.repeat(64);}],
    ['stale input', ({candidates}) => {candidates[0].inputs.fixture = 'c'.repeat(64);}],
    ['stale run', ({candidates}) => {candidates[0].runId = '00000000-0000-4000-8000-000000000001';}],
    ['missing backend', ({candidates}) => candidates.pop()],
    ['reference alias', ({candidates}) => {candidates[0].mode = 'reference';}],
    ['reference mismatch', ({candidates}) => {candidates[0].referenceLock.compatibility_date = '2020-01-01';}],
]) test('matrix rejects ' + name, () => {
    const value = matrix(); mutate(value);
    assert.throws(() => compareCryptoReports(value.reference, value.candidates, value.expected));
});

test('Linux validation rejects omission of managed from a verified healthy backend matrix', () => {
    const m=matrix();assert.ok(referenceBackendModes(m.expected.lock).includes('managed'));
    assert.doesNotThrow(()=>compareCryptoReports(m.reference,m.candidates,m.expected));
    assert.throws(()=>compareCryptoReports(m.reference,m.candidates.filter(r=>r.mode!=='managed'),m.expected));
});
