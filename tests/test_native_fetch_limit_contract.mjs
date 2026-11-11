import assert from 'node:assert/strict';
import {test, after} from 'node:test';
import {nativeFetchLimitPlans, nativeFetchLimitCasesForShard, nativeFetchLimitReply} from '../tools/native-fetch-limit-input.mjs';
import {expectedNativeFetchLimitState, assertNativeFetchLimitCase, validateNativeFetchLimitReport} from '../tools/native-fetch-limit-contract.mjs';

// Synthetic validator controls only. Actual product qualification always uses
// the real configured C ABI helper, TCP producer and observed worker exit.
function sample(plan) {
    const port = 12345;
    const request = 'GET /hold HTTP/1.1\r\nHost: localhost:'+port;
    const source = {request, rawRequest: Buffer.from(request+'\r\n\r\n').toString('base64'),
        peerEnded: true, peerReset: false, fixtureEnded: true, fixtureDestroyed: false,
        events: ['request', 'sent-complete', 'peer-ended'], sent: nativeFetchLimitReply(plan).packet.toString('base64')};
    const state = expectedNativeFetchLimitState(plan);
    const workerExit = {clean: true, processObserved: true, exitStatus: 0, forced: false, errorEvents: 0};
    return {plan: structuredClone(plan), source, port, encodedBytes: nativeFetchLimitReply(plan).payload.length, state,
        sourceEndedBeforeFinish: true, sourceBeforeFinish: structuredClone(source), shutdown: {code: 0, signal: null},
        stdout: 'LIMIT_PROBE '+JSON.stringify(state)+'\nLIMIT_EXIT '+JSON.stringify(workerExit)+'\n', stderr: '', workerExit};
}
function report(shard) {
    return {schema: 'capsid-native-fetch-limits-v1', complete: true, shard, runId: 'synthetic-native-limit-control',
        inputs: {synthetic: 'a'.repeat(64)}, binaries: {helper: 'b'.repeat(64), worker: 'c'.repeat(64)},
        cases: nativeFetchLimitCasesForShard(shard).map(sample)};
}
let rejected = 0;
function rejects(row, plan, mutate) {
    const changed = structuredClone(row); mutate(changed);
    assert.throws(() => assertNativeFetchLimitCase(changed, plan)); rejected++;
}

test('fixed native limit inventory retains all19 independently measured inputs', () => {
    assert.deepEqual(nativeFetchLimitPlans.map(row => row.id), [
        'raw97-limit0', 'raw97-limit96', 'raw97-limit97', 'raw97-limit100',
        'gzip97-limit0', 'gzip97-limit96', 'gzip97-limit97', 'gzip97-limit100', 'gzip97-limit120',
        'gzip97-byob64-limit100', 'gzip8197-byob8192-limit0', 'gzip8197-byob8192-limit8197',
        'gzip8197-byob8192-limit9000', 'gzip8197-limit4096-view0', 'gzip8197-limit4096-view8192',
        'raw8197-byob8192-limit0', 'raw8197-byob8192-limit9000', 'gzip97-crc-limit100', 'gzip97-truncated-limit100',
    ]);
    assert.equal(nativeFetchLimitCasesForShard('boundaries').length, 10);
    assert.equal(nativeFetchLimitCasesForShard('geometry').length, 9);
    assert.throws(() => nativeFetchLimitCasesForShard('subset'));
});
for (const plan of nativeFetchLimitPlans) {
    test('complete typed native limit state '+plan.id, () => assertNativeFetchLimitCase(sample(plan), plan));
    test('reject state, source and exit mutations '+plan.id, () => {
        const row = sample(plan);
        for (const key of Object.keys(row)) rejects(row, plan, value => {delete value[key];});
        for (const key of Object.keys(row.state)) rejects(row, plan, value => {delete value.state[key];});
        for (const mutate of [
            value => {value.state.closedStable = 1;},
            value => {value.state.after.bodyUsed = false;},
            value => {value.state.after.locked = true;},
            value => {value.state.failure = {name: 'Error', message: 'discarded'};},
            value => {value.state.before.headers.pop();},
            value => {value.sourceBeforeFinish.peerEnded = false;},
            value => {value.sourceEndedBeforeFinish = false;},
            value => {value.source.fixtureDestroyed = true;},
            value => {value.source.events.reverse(); value.sourceBeforeFinish = structuredClone(value.source);},
            value => {value.source.sent = ''; value.sourceBeforeFinish = structuredClone(value.source);},
            value => {value.source.rawRequest += 'AA=='; value.sourceBeforeFinish = structuredClone(value.source);},
            value => {value.port = 1;},
            value => {value.workerExit.forced = true;},
            value => {value.workerExit.processObserved = false;},
            value => {value.workerExit.exitStatus = 1;},
            value => {value.workerExit.clean = 1;},
            value => {value.workerExit.errorEvents = 1;},
            value => {value.shutdown.code = 1;},
            value => {value.stderr = 'worker warning';},
            value => {value.stdout += value.stdout;},
            value => {value.extra = true;},
        ]) rejects(row, plan, mutate);
        for (let index = 0; index < row.state.reads.length; index++) {
            for (const key of Object.keys(row.state.reads[index]))
                rejects(row, plan, value => {delete value.state.reads[index][key];});
            for (const key of ['bytes', 'backing']) {
                const bytes = row.state.reads[index][key];
                if (!bytes?.length) continue;
                for (const offset of new Set([0, Math.floor(bytes.length/2), bytes.length-1]))
                    rejects(row, plan, value => {value.state.reads[index][key][offset] = false;});
            }
            rejects(row, plan, value => {value.state.reads[index].inputAfter = 17;});
        }
    });
}
for (const shard of ['boundaries', 'geometry']) test('native limit report requires every current case and input '+shard, () => {
    const value = report(shard);
    const expected = {shard, runId: value.runId, inputs: structuredClone(value.inputs), binaries: structuredClone(value.binaries)};
    validateNativeFetchLimitReport(value, expected);
    for (const mutate of [
        changed => {changed.cases.pop();},
        changed => {changed.cases.push(structuredClone(changed.cases[0]));},
        changed => {changed.cases.reverse();},
        changed => {changed.complete = false;},
        changed => {changed.inputs.synthetic = '0'.repeat(64);},
        changed => {changed.binaries.worker = '0'.repeat(64);},
        changed => {changed.runId = 'stale';},
        changed => {changed.extra = true;},
    ]) {
        const changed = structuredClone(value); mutate(changed);
        assert.throws(() => validateNativeFetchLimitReport(changed, expected)); rejected++;
    }
    value.cases[0].state.closedStable = 1;
    assert.throws(() => validateNativeFetchLimitReport(value, expected)); rejected++;
    validateNativeFetchLimitReport(report(shard), expected);
});
after(() => {
    assert.ok(rejected >= 1000, 'complete active mutation inventory');
    console.log(JSON.stringify({complete: true, uniqueCases: 19, rejectedMutations: rejected, syntheticControlsOnly: true}));
});
