import assert from 'node:assert/strict';
import fs from 'node:fs';
import {createHash} from 'node:crypto';
import {test, after} from 'node:test';
import {nativeFetchOwnerPlans, nativeFetchOwnerCasesForShard, nativeFetchOwnerReply} from '../tools/native-fetch-owner-input.mjs';
import {expectedNativeFetchOwnerState, assertNativeFetchOwnerCase, validateNativeFetchOwnerReport,
    assertNativeFetchOwnerReferenceArchive} from '../tools/native-fetch-owner-contract.mjs';

// These are synthetic validator controls. Product observations always come
// from the real C ABI helper, controlled TCP source and observed OS exit.
const config = {heapBytes: 67108864, responseLimit: 0, wireVersion: 4, capabilities: 123, maxChannels: 1};
const stdout = row => 'OWNER_CONFIG '+JSON.stringify(config)+'\nOWNER_PROBE '+JSON.stringify(row.state)+'\nOWNER_EXIT '+JSON.stringify(row.workerExit)+'\n';
function sample(plan) {
    const port = 12345, request = 'GET /hold HTTP/1.1\r\nHost: localhost:'+port;
    const source = {request, rawRequest: Buffer.from(request+'\r\n\r\n').toString('base64'), peerEnded: true,
        peerReset: false, fixtureEnded: true, fixtureDestroyed: false, events: ['request', 'sent-complete', 'peer-ended'],
        sent: nativeFetchOwnerReply(plan).packet.toString('base64')};
    const row = {plan: structuredClone(plan), source, port, encodedBytes: nativeFetchOwnerReply(plan).payload.length,
        state: expectedNativeFetchOwnerState(plan), sourceTerminalBeforeFinish: true, sourceBeforeFinish: structuredClone(source),
        shutdown: {code: 0, signal: null}, stderr: '', workerExit: {clean: true, processObserved: true, exitStatus: 0, forced: false, errorEvents: 0}};
    row.stdout = stdout(row); return row;
}
function report(shard) {
    return {schema: 'capsid-native-fetch-owner-v1', complete: true, shard, runId: 'synthetic-owner-control', inputs: {synthetic: 'a'.repeat(64)},
        binaries: {helper: 'b'.repeat(64), worker: 'c'.repeat(64)}, cases: nativeFetchOwnerCasesForShard(shard).map(sample)};
}
let rejected = 0;
function rejects(row, plan, mutate) {
    const changed = structuredClone(row); mutate(changed);
    assert.throws(() => assertNativeFetchOwnerCase(changed, plan)); rejected++;
}
test('fixed10 native owner inputs and mandatory6+4 shards', () => {
    assert.deepEqual(nativeFetchOwnerPlans.map(p => p.id), [
        'gzip65536-stream-cancel', 'gzip65536-reader-cancel', 'gzip65536-read-cancel', 'gzip65536-abort', 'gzip65536-retain',
        'gzip8388608-stream-cancel', 'gzip8388608-reader-cancel', 'gzip8388608-read-cancel', 'gzip8388608-abort', 'gzip8388608-retain']);
    assert.equal(nativeFetchOwnerCasesForShard('cancel').length, 6); assert.equal(nativeFetchOwnerCasesForShard('retirement').length, 4);
    assert.throws(() => nativeFetchOwnerCasesForShard('subset'));
});
for (const plan of nativeFetchOwnerPlans) {
    test('complete native owner state '+plan.id, () => assertNativeFetchOwnerCase(sample(plan), plan));
    test('current state, every byte, source and real-exit mutations '+plan.id, () => {
        const row = sample(plan);
        for (const key of Object.keys(row)) rejects(row, plan, value => {delete value[key];});
        for (const key of Object.keys(row.state)) rejects(row, plan, value => {delete value.state[key]; value.stdout = stdout(value);});
        for (const key of Object.keys(row.source)) rejects(row, plan, value => {delete value.source[key]; value.sourceBeforeFinish = structuredClone(value.source);});
        for (const key of Object.keys(row.workerExit)) rejects(row, plan, value => {delete value.workerExit[key]; value.stdout = stdout(value);});
        for (const mutate of [
            value => {value.state.after.bodyUsed = !value.state.after.bodyUsed; value.stdout = stdout(value);},
            value => {value.state.before.status = '201'; value.stdout = stdout(value);},
            value => {value.state.failure = {}; value.stdout = stdout(value);},
            value => {value.state.before.headers.pop(); value.stdout = stdout(value);},
            value => {value.source.peerEnded = false; value.sourceBeforeFinish = structuredClone(value.source);},
            value => {value.sourceTerminalBeforeFinish = 1;},
            value => {value.sourceBeforeFinish.fixtureDestroyed = true;},
            value => {value.source.fixtureDestroyed = true; value.sourceBeforeFinish = structuredClone(value.source);},
            value => {value.source.sent = ''; value.sourceBeforeFinish = structuredClone(value.source);},
            value => {value.source.rawRequest += 'AA=='; value.sourceBeforeFinish = structuredClone(value.source);},
            value => {value.source.events.reverse(); value.sourceBeforeFinish = structuredClone(value.source);},
            value => {value.workerExit.forced = true; value.stdout = stdout(value);},
            value => {value.workerExit.processObserved = false; value.stdout = stdout(value);},
            value => {value.workerExit.clean = 1; value.stdout = stdout(value);},
            value => {value.workerExit.errorEvents = 1; value.stdout = stdout(value);},
            value => {value.shutdown.code = 1;}, value => {value.stderr = 'diagnostic';},
            value => {value.stdout += value.stdout;}, value => {value.stdout = value.stdout.replace('67108864', '134217728');},
            value => {value.extra = true;},
        ]) rejects(row, plan, mutate);
        for (let index = 0; index < row.state.reads.length; index++) {
            for (const key of Object.keys(row.state.reads[index]))
                rejects(row, plan, value => {delete value.state.reads[index][key]; value.stdout = stdout(value);});
            for (const key of ['bytes', 'backing']) {
                const bytes = row.state.reads[index][key]; if (!bytes) continue;
                for (let offset = 0; offset < bytes.length; offset++)
                    rejects(row, plan, value => {value.state.reads[index][key][offset] = false; value.stdout = stdout(value);});
            }
            rejects(row, plan, value => {value.state.reads[index].inputAfterIssue = 80; value.stdout = stdout(value);});
        }
        const same = sample(plan); assertNativeFetchOwnerCase(same, plan);
        same.state.before.bodyUsed = 0; same.stdout = stdout(same);
        assert.throws(() => assertNativeFetchOwnerCase(same, plan)); rejected++;
        assertNativeFetchOwnerCase(sample(plan), plan);
    });
}
test('both current primaries cannot bless the same wrong byte, type or exit snapshot', () => {
    const raw = JSON.parse(fs.readFileSync(new URL('./fixtures/workerd-native-fetch-owner-report.json', import.meta.url), 'utf8'));
    const fixtureHash = createHash('sha256').update(fs.readFileSync(new URL('./fixtures/native-fetch-owner.mjs', import.meta.url))).digest('hex');
    assertNativeFetchOwnerReferenceArchive(raw, fixtureHash);
    for (const mutate of [
        report => {report.cases[0].state.before.bodyUsed = 0;},
        report => {report.cases[2].state.reads[0].backing[79] = false;},
        report => {report.cases[2].state.reads[0].bytes[63] = 165;},
        report => {report.cases[3].state.reads[0].inputAfterIssue = 80;},
        report => {report.cases[3].state.closed.error.sameReason = true;},
        report => {report.cases[0].sourceBeforeReferenceStop.fixtureDestroyed = true;},
        report => {report.cases[0].sourceBeforeReferenceStop.sent = '';},
        report => {report.compatibilityDate = 'wrong'; report.referenceLock.compatibility_date = '2026-08-21';},
    ]) {
        const changed = structuredClone(raw);
        for (const item of changed.reports) {
            const report = JSON.parse(item.originalTextUTF8); mutate(report);
            item.originalTextUTF8 = JSON.stringify(report); item.originalReportSHA256 = createHash('sha256').update(item.originalTextUTF8).digest('hex');
        }
        assert.throws(() => assertNativeFetchOwnerReferenceArchive(changed, fixtureHash)); rejected++;
    }
    raw.reports[0].originalTextUTF8 += ' ';
    assert.throws(() => assertNativeFetchOwnerReferenceArchive(raw, fixtureHash)); rejected++;
});
for (const shard of ['cancel', 'retirement']) test('complete current owner report '+shard, () => {
    const value = report(shard), expected = {shard, runId: value.runId, inputs: structuredClone(value.inputs), binaries: structuredClone(value.binaries)};
    validateNativeFetchOwnerReport(value, expected);
    for (const mutate of [
        changed => {changed.cases.pop();}, changed => {changed.cases.reverse();}, changed => {changed.cases.push(structuredClone(changed.cases[0]));},
        changed => {changed.complete = 1;}, changed => {changed.runId = 'old';}, changed => {changed.inputs.synthetic = '0'.repeat(64);},
        changed => {changed.binaries.worker = '0'.repeat(64);}, changed => {changed.extra = true;},
    ]) {const changed = structuredClone(value); mutate(changed); assert.throws(() => validateNativeFetchOwnerReport(changed, expected)); rejected++;}
    value.cases[0].state.before.status = '201'; value.cases[0].stdout = stdout(value.cases[0]);
    assert.throws(() => validateNativeFetchOwnerReport(value, expected)); rejected++;
    validateNativeFetchOwnerReport(report(shard), expected);
});
after(() => {
    assert.ok(rejected >= 1000, 'all planned active mutations ran');
    console.log(JSON.stringify({complete: true, uniqueOwnerInputs: 10, rejectedMutations: rejected, syntheticControlsOnly: true}));
});
