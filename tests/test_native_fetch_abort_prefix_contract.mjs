import assert from 'node:assert/strict';
import {test, after} from 'node:test';
import fs from 'node:fs';
import {createHash} from 'node:crypto';
import {nativeFetchAbortPrefixPlans as plans, nativeFetchAbortPrefixReply, nativeFetchAbortPrefixInputPaths, expectedNativeFetchAbortPrefixState} from '../tools/native-fetch-abort-prefix-input.mjs';
import {assertNativeFetchAbortPrefixCase, assertNativeFetchAbortPrefixReferenceArchive, validateNativeFetchAbortPrefixReport} from '../tools/native-fetch-abort-prefix-contract.mjs';

const hash = value => createHash('sha256').update(value).digest('hex');
let rejected = 0;
const output = row => 'LIMIT_PROBE '+JSON.stringify(row.state)+'\nLIMIT_EXIT '+JSON.stringify(row.workerExit)+'\n';
function sample(plan) {
    const reply = nativeFetchAbortPrefixReply(plan), port = 19000, request = 'GET /hold HTTP/1.1\r\nHost: localhost:'+port;
    const source = {request, rawRequest: Buffer.from(request+'\r\n\r\n').toString('base64'), peerEnded: false, peerReset: true,
        fixtureEnded: true, fixtureDestroyed: false, packetSHA256: hash(reply.packet), events: ['request', 'sent-complete']};
    const row = {plan: structuredClone(plan), encodedBytes: reply.payload.length, payloadSHA256: hash(reply.payload), packetSHA256: hash(reply.packet),
        source, port, state: expectedNativeFetchAbortPrefixState(plan), sourceEndedBeforeFinish: false, sourceTerminalBeforeFinish: true,
        sourceBeforeFinish: structuredClone(source), shutdown: {code: 0, signal: null}, stdout: '', stderr: '',
        workerExit: {clean: true, processObserved: true, exitStatus: 0, forced: false, errorEvents: 0}};
    row.stdout = output(row); return row;
}
function reject(row, plan, change) {
    const bad = structuredClone(row); change(bad); bad.stdout = output(bad);
    assert.throws(() => assertNativeFetchAbortPrefixCase(bad, plan)); rejected++;
}
for (const plan of plans) test('abort prefix complete typed/byte/error/lifecycle model '+plan.id, () => {
    const row = sample(plan); assertNativeFetchAbortPrefixCase(row, plan);
    for (const key of Object.keys(row.state)) reject(row, plan, bad => {delete bad.state[key];});
    // Exercise first/middle/terminal records. Validation compares every record,
    // including each payload hash, whole backing and error-state detachment.
    for (const index of new Set([0, Math.floor(row.state.reads.length/2), row.state.reads.length-1])) {
        for (const key of Object.keys(row.state.reads[index])) reject(row, plan, bad => {delete bad.state.reads[index][key];});
        const changes = row.state.reads[index].error
            ? [value => {value.inputAfterIssue = false;}, value => {value.inputAfter = 1;}, value => {value.error.sameReason = true;}, value => {value.error.name = 'AbortError';}]
            : [value => {value.inputAfterIssue = false;}, value => {value.done = 1;}, value => {value.dataHash = '0'.repeat(64);}, value => {value.backingHash = '0'.repeat(64);}, value => {value.offset ^= 1;}];
        for (const change of changes) reject(row, plan, bad => change(bad.state.reads[index]));
    }
    for (const change of [bad => {bad.state.closed.error.sameReason = true;}, bad => {bad.state.after.locked = true;}, bad => {bad.state.aborted = 1;},
        bad => {bad.sourceTerminalBeforeFinish = false;}, bad => {bad.sourceBeforeFinish.fixtureDestroyed = true;},
        bad => {bad.sourceBeforeFinish.peerEnded = false; bad.sourceBeforeFinish.peerReset = false;}, bad => {bad.packetSHA256 = '0'.repeat(64);},
        bad => {bad.workerExit.forced = true;}, bad => {bad.shutdown.code = 1;}, bad => {bad.state.reads.at(-1).done = true;}]) reject(row, plan, change);
    const mutable = sample(plan); assertNativeFetchAbortPrefixCase(mutable, plan); mutable.state.closedStable = 1; mutable.stdout = output(mutable);
    assert.throws(() => assertNativeFetchAbortPrefixCase(mutable, plan)); rejected++;
});
test('all actual primaries cannot bless the same incorrect state', () => {
    const archive = JSON.parse(fs.readFileSync(new URL('./fixtures/workerd-native-fetch-abort-prefix-report.json', import.meta.url), 'utf8'));
    const fixture = hash(fs.readFileSync(new URL('./fixtures/native-fetch-abort-prefix.mjs', import.meta.url)));
    assertNativeFetchAbortPrefixReferenceArchive(archive, fixture);
    for (const profile of ['raw', 'gzip']) for (const change of [report => {report.cases[0].state.total++;}, report => {report.cases[0].state.reads.at(-1).inputAfter = false;},
        report => {report.binarySHA256 = '0'.repeat(64);}, report => {report.referenceLock.compatibility_date = '2026-08-21';}]) {
        const bad = structuredClone(archive);
        for (const item of bad.reports) if (item.profile === profile) {
            const report = JSON.parse(item.originalTextUTF8); change(report); item.originalTextUTF8 = JSON.stringify(report); item.originalReportSHA256 = hash(Buffer.from(item.originalTextUTF8));
        }
        assert.throws(() => assertNativeFetchAbortPrefixReferenceArchive(bad, fixture)); rejected++;
    }
    archive.reports[0].originalTextUTF8 += ' '; assert.throws(() => assertNativeFetchAbortPrefixReferenceArchive(archive, fixture)); rejected++;
});
test('current mutable report is checked completely on every call', () => {
    const inputs = Object.fromEntries(Object.keys(nativeFetchAbortPrefixInputPaths).map(key => [key, 'a'.repeat(64)])), binaries = {helper: 'b'.repeat(64), worker: 'c'.repeat(64)};
    const report = {schema: 'capsid-native-fetch-abort-prefix-v1', complete: true, runId: 'synthetic-controls', inputs, binaries, cases: plans.map(sample)};
    const expected = {runId: report.runId, inputs: structuredClone(inputs), binaries: structuredClone(binaries)};
    validateNativeFetchAbortPrefixReport(report, expected);
    for (const change of [bad => {bad.complete = false;}, bad => {bad.inputs.fixture = 'd'.repeat(64);}, bad => {bad.binaries.worker = 'd'.repeat(64);}, bad => {bad.cases.pop();}, bad => {bad.cases[1] = structuredClone(bad.cases[0]);}]) {
        const bad = structuredClone(report); change(bad); assert.throws(() => validateNativeFetchAbortPrefixReport(bad, expected)); rejected++;
    }
    report.cases[0].state.closedStable = 1; report.cases[0].stdout = output(report.cases[0]);
    assert.throws(() => validateNativeFetchAbortPrefixReport(report, expected)); rejected++;
});
after(() => {assert.equal(rejected, 407); console.log(JSON.stringify({complete: true, uniqueCases: plans.length, rejectedMutations: rejected}));});
