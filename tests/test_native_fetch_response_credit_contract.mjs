import assert from 'node:assert/strict';
import {test} from 'node:test';
import {responseCreditPlans, expectedCreditBody, assertResponseCreditCase,
    validateResponseCreditReport} from '../tools/native-fetch-response-credit-contract.mjs';

const exit = {clean: true, processObserved: true, exitStatus: 0, forced: false, errorEvents: 0};
function makeCase(bytes) {
    return {bytes, collectionComplete: true, stdout: 'LIMIT_PROBE ' + expectedCreditBody(bytes) + '\nLIMIT_EXIT ' + JSON.stringify(exit) + '\n',
        stderr: '', helperExit: {code: 0, signal: null}, workerExit: structuredClone(exit)};
}
function makeReport() {
    const expected = {runId: 'synthetic-credit-controls', inputs: {fixture: 'a'.repeat(64)}, binaries: {helper: 'b'.repeat(64), worker: 'c'.repeat(64)}};
    return {expected, report: {schema: 'capsid-native-fetch-response-credit-v1', complete: true, ...expected, cases: responseCreditPlans.map(makeCase)}};
}
let rejected = 0;
const rejects = action => {assert.throws(action); rejected++;};
test('independent full-byte model spans the default256KiB response window', () => {
    assert.deepEqual(responseCreditPlans, [262143, 262144, 262145, 786469]);
    assert.equal(expectedCreditBody(40), 'abcdefghijklmnopqrstuvwxyz0123456789abcd');
    for (const bytes of responseCreditPlans) assertResponseCreditCase(makeCase(bytes), bytes);
});
test('complete responses require all bytes, healthy reuse, and natural exit', () => {
    for (const bytes of responseCreditPlans) for (const mutate of [
        r => {r.stdout = r.stdout.replace('LIMIT_PROBE a', 'LIMIT_PROBE b');},
        r => {r.stdout = r.stdout.replace('LIMIT_PROBE abc', 'LIMIT_PROBE ac');},
        r => {r.stdout = r.stdout.replace('\nLIMIT_EXIT ', 'a\nLIMIT_EXIT ');},
        r => {r.stdout += 'LIMIT_PROBE ' + expectedCreditBody(bytes) + '\n';},
        r => {r.stdout = r.stdout.split('\n').filter(line => !line.startsWith('LIMIT_EXIT ')).join('\n');},
        r => {r.workerExit.forced = true; r.stdout = r.stdout.replace('"forced":false', '"forced":true');},
        r => {r.helperExit.code = 1;}, r => {r.helperExit.signal = 'SIGTERM';},
        r => {r.stderr = 'FAIL: event deadline';}, r => {r.collectionComplete = false;},
        r => {r.bytes++;}, r => {r.helperForced = true;},
    ]) {const row = makeCase(bytes); mutate(row); rejects(() => assertResponseCreditCase(row, bytes));}
});
test('reports bind exact plans, source, binaries and the current run', () => {
    const {expected, report} = makeReport(); validateResponseCreditReport(report, expected);
    for (const mutate of [r => r.cases.pop(), r => r.cases.reverse(), r => r.cases.push(structuredClone(r.cases[0])),
        r => {r.complete = false;}, r => {r.runId += '-old';}, r => {r.binaries = {...r.binaries, worker: 'd'.repeat(64)};},
        r => {r.inputs = {...r.inputs, fixture: 'd'.repeat(64)};}, r => {r.schema += '-other';}]) {
        const value = structuredClone(report); mutate(value); rejects(() => validateResponseCreditReport(value, expected));
    }
    report.cases[3].stdout = report.cases[3].stdout.replace('LIMIT_PROBE a', 'LIMIT_PROBE z');
    rejects(() => validateResponseCreditReport(report, expected));
    assert.equal(rejected, 57);
    console.log(JSON.stringify({rejectedCoherentMutations: rejected}));
});
