import assert from 'node:assert/strict';
import {test} from 'node:test';
import {nativeFetchIntrinsicPlans, nativeFetchIntrinsicCases, nativeFetchIntrinsicPacket,
    expectedNativeFetchIntrinsicState, expectedNativeFetchIntrinsicSource} from '../tools/native-fetch-intrinsic-input.mjs';
import {assertNativeFetchIntrinsicCase, validateNativeFetchIntrinsicReport,
    validateNativeFetchIntrinsicPrimary, readNativeFetchIntrinsicPrimary} from '../tools/native-fetch-intrinsic-contract.mjs';

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
    const row = {plan, collectionComplete: true, packetBase64: nativeFetchIntrinsicPacket(plan).toString('base64'),
        sourceTerminalBeforeContinue: true, workerExit: {reaped: true, channelClosed: true, code: 0},
        helperExit: {code: 0, signal: null}, stderr: ''};
    for (const index of [1, 2]) row['request' + index] = {summary: expectedNativeFetchIntrinsicState(plan), requestId: String(index)};
    for (const key of ['sourceBeforeContinue', 'sourceBeforeStop', 'sourceAfterExit', 'sourceFinalBeforeCleanup'])
        row[key] = expectedNativeFetchIntrinsicSource(plan, 12345);
    return output(row);
}
function makeReport(kind) {
    const expected = {kind, runId: 'synthetic-intrinsic-controls', inputs: {binding: 'a'.repeat(64)},
        binaries: {helper: 'b'.repeat(64), worker: 'c'.repeat(64)}};
    return {expected, report: {schema: 'capsid-native-fetch-intrinsic-v1', complete: true, ...expected,
        cases: nativeFetchIntrinsicCases(kind).map(makeCase)}};
}

test('all six controls require the full successful state and both real requests', () => {
    assert.equal(nativeFetchIntrinsicPlans.length, 6);
    for (const plan of nativeFetchIntrinsicPlans) {
        assertNativeFetchIntrinsicCase(makeCase(plan), plan);
        for (const mutate of [
            row => row.request1.requestId = '2', row => row.request2.requestId = '1',
            row => row.request1.summary.bytes[0] ^= 1, row => row.request1.summary.bytes.pop(),
            row => row.request1.summary.traps.push('WeakMap.' + plan.api),
            row => row.request1.summary.failure = {name: 'Error', message: 'application WeakMap.' + plan.api, sameSentinel: true},
            row => row.request1.summary.failurePhase = 'fetch', row => row.request1.summary.returnedPromise = false,
            row => row.request1.summary.metadata.headers.pop(), row => row.request1.summary.metadata.status = 200,
            row => row.request1.summary.cleanupBytes = 16, row => row.request1.summary.cleanupError = {name: 'Error'},
            row => row.request1.summary.api = 'wrong', row => row.request1.summary.extra = true,
        ]) {
            const row = makeCase(plan); mutate(row);
            if (row.request1.requestId === '1') row.request2.summary = structuredClone(row.request1.summary);
            output(row); rejects(plan.id, () => assertNativeFetchIntrinsicCase(row, plan));
        }
    }
});

test('all six controls reject coherent wire, event, FIN, fixture completion and peer count changes', () => {
    for (const plan of nativeFetchIntrinsicPlans) for (const mutate of [
        value => value.records[0].request += 'x', value => value.records[0].rawRequest = 'AA==',
        value => value.records[0].sentBytes--, value => value.records[0].sentSHA256 = '0'.repeat(64),
        value => { value.records[0].peerEnded = false; value.records[0].events.pop(); },
        value => value.records[0].peerReset = true, value => value.records[0].fixtureDestroyed = true,
        value => value.records[0].fixtureEnded = false, value => value.records[0].events.reverse(),
        value => value.records.pop(), value => value.records.push(structuredClone(value.records[0])),
    ]) {
        const row = makeCase(plan); mutate(row.sourceBeforeContinue);
        for (const key of ['sourceBeforeStop', 'sourceAfterExit', 'sourceFinalBeforeCleanup']) row[key] = structuredClone(row.sourceBeforeContinue);
        rejects(plan.id, () => assertNativeFetchIntrinsicCase(row, plan));
    }
});

test('all six controls require natural exit and lossless actual helper output', () => {
    for (const plan of nativeFetchIntrinsicPlans) for (const mutate of [
        row => row.sourceTerminalBeforeContinue = false, row => row.packetBase64 = 'AA==',
        row => row.helperExit.code = 1, row => { row.workerExit.code = 1; output(row); },
        row => { row.workerExit.reaped = false; output(row); }, row => { row.workerExit.channelClosed = false; output(row); },
        row => row.stderr = 'runtime diagnostic', row => row.stdout = row.stdout.replace('BINDING_REQUEST 2\n', ''),
        row => row.stdout += 'unknown output\n', row => row.stdout = row.stdout.replace('BINDING_PHASE 1 {', 'BINDING_PHASE 1 {"extra":true,'),
        row => row.collectionComplete = false, row => row.sourceFinalBeforeCleanup.records[0].fixtureDestroyed = true,
        row => row.helperForced = true, row => delete row.request1,
    ]) {
        const row = makeCase(plan); mutate(row); rejects(plan.id, () => assertNativeFetchIntrinsicCase(row, plan));
    }
});

test('both complete product shards require exact current identities and mandatory ordered cases', () => {
    for (const kind of ['raw', 'gzip']) {
        const {expected, report} = makeReport(kind); validateNativeFetchIntrinsicReport(report, expected);
        for (const mutate of [value => value.cases.pop(), value => value.cases.reverse(),
            value => value.cases.push(structuredClone(value.cases[0])), value => value.runId += '-wrong',
            value => value.inputs.binding = 'f'.repeat(64), value => value.binaries.worker = 'f'.repeat(64),
            value => value.binaries.helper = 'f'.repeat(64), value => value.kind = 'wrong',
            value => value.schema += '-wrong', value => value.complete = false, value => value.extra = true]) {
            const bad = structuredClone(report); mutate(bad); rejects(kind, () => validateNativeFetchIntrinsicReport(bad, expected));
        }
    }
});

test('both actual pinned archives reject coherent false states, changed profile, incomplete peers or wrong input identity', () => {
    const archive = readNativeFetchIntrinsicPrimary();
    for (const platform of ['win32', 'linux']) {
        const actual = archive.platforms[platform]; validateNativeFetchIntrinsicPrimary(actual, platform);
        for (const mutate of [
            value => value.complete = false, value => value.fixtureSHA256 = '0'.repeat(64),
            value => value.bindingSHA256 = '0'.repeat(64), value => value.commonFunctionSHA256 = '0'.repeat(64),
            value => value.peerSHA256 = '0'.repeat(64), value => value.sourceModelSHA256 = '0'.repeat(64),
            value => value.contractSHA256 = '0'.repeat(64), value => value.captureSHA256 = '0'.repeat(64),
            value => value.binarySHA256 = '0'.repeat(64), value => value.referenceLock.source_commit = '0'.repeat(40),
            value => value.referenceLock.compatibility_date = '2026-01-01', value => value.referenceLock.compatibility_flags.push('wrong'),
            value => value.client.undici = 'wrong', value => value.cases.pop(), value => value.cases.reverse(),
            value => value.cases[0].state.traps.push('WeakMap.get'), value => value.cases[0].state.bytes[0] ^= 1,
            value => value.cases[0].sourceTerminalBeforeReferenceStop = false,
            value => { value.cases[0].sourceBeforeReferenceStop.records[0].fixtureDestroyed = true;
                value.cases[0].sourceAfterReferenceStop = structuredClone(value.cases[0].sourceBeforeReferenceStop); },
            value => value.stderr = 'diagnostic', value => value.shutdown.code = 1,
        ]) {
            const bad = structuredClone(actual); mutate(bad); rejects(platform, () => validateNativeFetchIntrinsicPrimary(bad, platform));
        }
    }
});

test('previously accepted mutable reports are revalidated', () => {
    const {expected, report} = makeReport('raw'); validateNativeFetchIntrinsicReport(report, expected);
    report.cases[1].request1.summary.traps.push('WeakMap.get');
    report.cases[1].request2.summary = structuredClone(report.cases[1].request1.summary); output(report.cases[1]);
    rejects('accepted product mutated', () => validateNativeFetchIntrinsicReport(report, expected));
    const actual = readNativeFetchIntrinsicPrimary().platforms.win32; validateNativeFetchIntrinsicPrimary(actual, 'win32');
    actual.cases[1].state.bytes[0] ^= 1;
    rejects('accepted primary mutated', () => validateNativeFetchIntrinsicPrimary(actual, 'win32'));
});

test('control counts remain distinct from actual product execution', () => {
    assert.equal(rejected, 300);
    console.log('NATIVE_FETCH_INTRINSIC_CONTROLS ' + JSON.stringify({modelCases: 6, rejectedMutations: rejected, actualProductProgramsExecuted: 0}));
});
