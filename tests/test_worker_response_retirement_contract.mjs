import assert from 'node:assert/strict';
import {test} from 'node:test';
import {retirementActions, retirementPlans, retirementTranscript, assertRetirementCase, validateRetirementReport} from '../tools/response-retirement-contract.mjs';

function model(action, processMemory = 268435456) {
    return {action, runId: 'retirement-controls', processMemory, features: processMemory ? 1 : 0,
        compatibility: 'sha256:' + 'a'.repeat(64), manifest: 'b'.repeat(64),
        inputs: {fixture: 'c'.repeat(64)}, binaries: {worker: 'd'.repeat(64)}};
}
function observation(plan, expected) {
    return {plan, collectionComplete: true, helperExit: {code: 0, signal: null},
        stdout: retirementTranscript(plan, expected), stderr: ''};
}
test('all four retirement paths validate complete credit, bytes, ownership and natural exit', () => {
    for (const processMemory of [268435456, 0]) for (const action of retirementActions) {
        const expected = model(action, processMemory);
        const report = {schema: 'capsid-response-retirement-v1', complete: true, ...expected,
            cases: retirementPlans(action).map(plan => observation(plan, expected)), failures: []};
        validateRetirementReport(report, expected);
        for (const change of [r => {r.runId += '-stale';}, r => {r.complete = false;}, r => {r.binaries.worker = 'e'.repeat(64);},
            r => {r.inputs.fixture = 'e'.repeat(64);}, r => {r.failures.push({id: 'ignored'});}, r => {r.cases.pop();},
            r => {r.cases.push(structuredClone(r.cases[0]));}, r => {r.cases.reverse();}, r => {r.processMemory++;}]) {
            const changed = structuredClone(report); change(changed); assert.throws(() => validateRetirementReport(changed, expected));
        }
        report.cases[0].stdout += 'unexpected\n';
        assert.throws(() => validateRetirementReport(report, expected), 'accepted mutable objects are revalidated');
    }
});
test('192 coherent transcript mutations per process-memory profile are rejected', () => {
    for (const processMemory of [268435456, 0]) {
        let rejected = 0;
        for (const action of retirementActions) for (const plan of retirementPlans(action)) {
            const expected = model(action, processMemory), original = observation(plan, expected);
            for (const change of [
                row => {row.collectionComplete = false;},
                row => {row.helperExit.code = 1;},
                row => {row.helperForced = true;},
                row => {row.stderr = 'TypeError: not a function\n';},
                row => {row.stdout = row.stdout.replaceAll('request=1', 'request=3');},
                row => {row.stdout = row.stdout.replaceAll('window=32', 'window=64');},
                row => {row.stdout = row.stdout.replaceAll('payload=616263', 'payload=786263');},
                row => {row.stdout = row.stdout.replace(/RETIREMENT_EVENT type=4[^\n]*\n/, '');},
                row => {const line = row.stdout.match(/RETIREMENT_EVENT type=4[^\n]*\n/)[0]; row.stdout = row.stdout.replace(line, line + line);},
                row => {row.stdout = row.stdout.replace(/milliseconds=\d+/, 'milliseconds=' + (action === 'timeout' ? '749' : '5000'));},
                row => {row.stdout = row.stdout.replace('status=0 wait=0 forced=0', 'status=1 wait=0 forced=1');},
                row => {row.stdout = row.stdout.replaceAll(expected.compatibility, 'sha256:' + 'e'.repeat(64));}
            ]) {
                const row = structuredClone(original); change(row);
                assert.throws(() => assertRetirementCase(row, plan, expected)); rejected++;
            }
        }
        assert.equal(rejected, 192);
        console.log(JSON.stringify({processMemory, rejectedRetirementMutations: rejected}));
    }
});
