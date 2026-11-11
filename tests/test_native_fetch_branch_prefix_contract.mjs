import assert from 'node:assert/strict';
import {test} from 'node:test';
import {branchPrefixPlans, branchPrefixCases, branchPrefixPacket, expectedBranchPrefixState,
    expectedBranchPrefixSource} from '../tools/native-fetch-branch-prefix-input.mjs';
import {assertBranchPrefixCase, validateBranchPrefixReport, validateBranchPrefixPrimary,
    readBranchPrefixPrimary} from '../tools/native-fetch-branch-prefix-contract.mjs';

let rejected = 0;
const rejects = (label, action) => {assert.throws(action, undefined, label); rejected++;};
const sourceKeys = ['sourceBeforeContinue', 'sourceBeforeStop', 'sourceAfterExit', 'sourceFinalBeforeCleanup'];
function output(row) {
    const features = process.platform === 'win32' ? 0 : 1;
    row.stdout = [`memoryLimit=0 actualFeatures=${features} expectedFeatures=${features}`, 'BINDING_REQUEST 1',
        'BINDING_PHASE 1 ' + JSON.stringify(row.request1), 'BINDING_REQUEST 2',
        'BINDING_PHASE 2 ' + JSON.stringify(row.request2), 'BINDING_EXIT ' + JSON.stringify(row.workerExit)].join('\n') + '\n';
    return row;
}
function cohere(row) {
    row.request2.summary = structuredClone(row.request1.summary);
    for (const key of sourceKeys) for (const [index, property] of [[0, 'prepared'], [2, 'afterPrefix'], [4, 'afterAction']]) {
        const snapshot = row[key], data = row.request1.summary[property], phase = snapshot.phases[index].phase;
        snapshot.phases[index].data = structuredClone(data);
        const request = `GET /control?${new URLSearchParams({phase, data: JSON.stringify(data)})} HTTP/1.1\r\nHost: 127.0.0.1:${snapshot.port}`;
        snapshot.records[index + 1].request = request;
        snapshot.records[index + 1].rawRequest = Buffer.from(request + '\r\n\r\n').toString('base64');
    }
    return output(row);
}
function makeCase(plan) {
    const row = {plan, collectionComplete: true, packetBase64: branchPrefixPacket(plan).toString('base64'),
        sourceTerminalBeforeContinue: true, workerExit: {reaped: true, channelClosed: true, code: 0},
        helperExit: {code: 0, signal: null}, stderr: '', request1: {summary: expectedBranchPrefixState(plan), requestId: '1'},
        request2: {summary: expectedBranchPrefixState(plan), requestId: '2'}};
    for (const key of sourceKeys) row[key] = expectedBranchPrefixSource(plan, 12345);
    return output(row);
}
function makeReport(method) {
    const expected = {method, runId: 'synthetic-branch-prefix-controls', inputs: {binding: 'a'.repeat(64)},
        binaries: {helper: 'b'.repeat(64), worker: 'c'.repeat(64)}};
    return {expected, report: {schema: 'capsid-native-fetch-branch-prefix-v1', complete: true, ...expected,
        cases: branchPrefixCases(method).map(makeCase)}};
}
function readMutation(state, field) {
    const first = state.branches[0].first;
    if (!first.value) {first.error = {...first.error, message: first.error.message + ' wrong ' + field}; return;}
    const value = first.value;
    if (field === 'type') value.type = 'Float64Array';
    else if (field === 'length') value.length--;
    else if (field === 'capacity') value.capacity--;
    else {
        const backing = Buffer.from(value.backing, 'base64'); backing[field === 'data' ? value.offset : 0] ^= 1;
        value.backing = backing.toString('base64');
        if (field === 'data') {const bytes = Buffer.from(value.bytes, 'base64'); bytes[0] ^= 1; value.bytes = bytes.toString('base64');}
    }
}

test('both typed-minimum shards require all32 fixed source/control/read scenarios', () => {
    assert.equal(branchPrefixPlans.length, 32);
    for (const method of ['wide', 'full']) assert.equal(branchPrefixCases(method).length, 16);
    for (const plan of branchPrefixPlans) assertBranchPrefixCase(makeCase(plan), plan);
});

test('all32 cases reject coherent wrong pending, cancellation, byte, backing, minimum and EOF states', () => {
    const mutations = [
        s => s.failure = {name: 'Error', message: 'wrong'},
        s => s.prepared[0].firstState = 'fulfilled', s => s.afterPrefix[1].firstState = 'fulfilled',
        s => s.afterAction[0].firstState = s.afterAction[0].firstState === 'pending' ? 'fulfilled' : 'pending',
        s => s.before.status = 200, s => s.afterGraph.bodyUsed = !s.afterGraph.bodyUsed,
        s => {if (s.copy) s.copy.locked = true; else s.after.bodyUsed = false;},
        s => s.branches[0].closedStable = false, s => s.branches[0].released = false,
        s => s.branches[0].lockedAfterRelease = true, s => s.branches[0].first.inputBefore--,
        s => s.branches[0].first.returnedPromise = false, s => s.branches[0].first.inputAfterIssue = 1,
        s => s.branches[0].first.inputAfter = 1,
        ...['type', 'length', 'capacity', 'data', 'guard'].map(field => s => readMutation(s, field)),
        s => {const tail = s.branches[0].tails.at(-1); if (tail.error) tail.error.message += ' wrong'; else tail.done = false;},
    ];
    assert.equal(mutations.length, 20);
    for (const plan of branchPrefixPlans) for (const mutate of mutations) {
        const row = makeCase(plan); mutate(row.request1.summary); cohere(row);
        rejects(plan.id, () => assertBranchPrefixCase(row, plan));
    }
});

test('all32 cases require exact input packet, six actual peers, five control phases and source lifetime', () => {
    const mutations = [
        s => {s.records[0].request = s.records[0].request.replace('GET', 'POST');
            s.records[0].rawRequest = Buffer.from(s.records[0].request + '\r\n\r\n').toString('base64');},
        s => s.records[0].rawRequest = 'AA==', s => s.records[0].sent.pop(),
        s => s.phases[0].bodyReleasedBefore = true, s => s.phases[1].data = {wrong: true},
        s => s.phases[2].data[1].firstState = 'fulfilled',
        s => s.phases[4].sourceEndedBefore = !s.phases[4].sourceEndedBefore,
        s => {s.records[0].peerEnded = false; s.records[0].events.pop();},
        s => s.records[0].peerReset = true, s => s.records[0].fixtureDestroyed = true,
        s => s.records[0].fixtureEnded = !s.records[0].fixtureEnded,
        s => s.records.pop(), s => s.records.push(structuredClone(s.records[0])),
        s => s.phases[3].sourceEndedBefore = 'false', s => s.prefixReleased = false,
    ];
    assert.equal(mutations.length, 15);
    for (const plan of branchPrefixPlans) for (const mutate of mutations) {
        const row = makeCase(plan); mutate(row.sourceBeforeContinue);
        for (const key of sourceKeys.slice(1)) row[key] = structuredClone(row.sourceBeforeContinue);
        rejects(plan.id, () => assertBranchPrefixCase(row, plan));
    }
});

test('all32 cases require both actual requests, natural exit and complete helper observations', () => {
    const mutations = [
        row => row.sourceTerminalBeforeContinue = false, row => row.helperExit.code = 1,
        row => {row.workerExit.code = 1; output(row);}, row => {row.workerExit.reaped = false; output(row);},
        row => {row.workerExit.channelClosed = false; output(row);}, row => row.stderr = 'runtime diagnostic',
        row => row.stdout += 'unknown output\n', row => {row.request1.requestId = '2'; output(row);},
        row => {row.request2.requestId = '1'; output(row);}, row => row.collectionComplete = false,
        row => row.helperForced = true, row => row.packetBase64 = 'AA==',
        row => {
            const packet = Buffer.from(row.packetBase64, 'base64'); packet[packet.indexOf('\r\n\r\n') + 4 + 136] ^= 1;
            row.packetBase64 = packet.toString('base64');
            for (const key of sourceKeys) {
                const prefix = Buffer.from(row[key].records[0].sent[1], 'base64'); prefix[136] ^= 1;
                row[key].records[0].sent[1] = prefix.toString('base64');
            }
        },
        row => row.stdout = row.stdout.replace('BINDING_REQUEST 2\n', ''), row => row.extra = true,
    ];
    assert.equal(mutations.length, 15);
    for (const plan of branchPrefixPlans) for (const mutate of mutations) {
        const row = makeCase(plan); mutate(row); rejects(plan.id, () => assertBranchPrefixCase(row, plan));
    }
});

test('both full product shards require ordered cases and exact current input/binary/run identity', () => {
    const mutations = [value => value.cases.pop(), value => value.cases.reverse(),
        value => value.cases.push(structuredClone(value.cases[0])), value => value.runId += '-wrong',
        value => value.inputs.binding = 'f'.repeat(64), value => value.binaries.worker = 'f'.repeat(64),
        value => value.binaries.helper = 'f'.repeat(64), value => value.method = 'wrong',
        value => value.schema += '-wrong', value => value.complete = false];
    assert.equal(mutations.length, 10);
    for (const method of ['wide', 'full']) {
        const {expected, report} = makeReport(method); validateBranchPrefixReport(report, expected);
        for (const mutate of mutations) {
            const bad = structuredClone(report); mutate(bad);
            rejects(method, () => validateBranchPrefixReport(bad, expected));
        }
    }
});

test('both actual public archives require fixed inputs/profile and complete independent states', () => {
    const archive = readBranchPrefixPrimary();
    const mutations = [
        value => value.complete = false, value => value.fixtureSHA256 = '0'.repeat(64),
        value => value.bindingSHA256 = '0'.repeat(64), value => value.commonFunctionSHA256 = '0'.repeat(64),
        value => value.sourceModelSHA256 = '0'.repeat(64), value => value.peerSHA256 = '0'.repeat(64),
        value => value.contractSHA256 = '0'.repeat(64), value => value.captureSHA256 = '0'.repeat(64),
        value => value.binarySHA256 = '0'.repeat(64), value => value.referenceLock.source_commit = '0'.repeat(40),
        value => value.referenceLock.compatibility_date = '2026-01-01', value => value.referenceLock.compatibility_flags.push('wrong'),
        value => value.client.undici = 'wrong', value => value.cases.pop(), value => value.cases.reverse(),
        value => value.cases[0].state.afterPrefix[1].firstState = 'fulfilled', value => readMutation(value.cases[0].state, 'data'),
        value => value.cases[0].sourceTerminalBeforeReferenceStop = false,
        value => value.stderr = 'diagnostic', value => value.shutdown.code = 1,
        value => value.referenceLock.release_tag = 'v1.0.0', value => value.referenceLock.binary_sha256 = '0'.repeat(64),
        value => value.client.node = 'wrong', value => value.extra = true,
    ];
    assert.equal(mutations.length, 24);
    for (const platform of ['win32', 'linux']) {
        const actual = archive.platforms[platform]; validateBranchPrefixPrimary(actual, platform);
        for (const mutate of mutations) {
            const bad = structuredClone(actual); mutate(bad);
            rejects(platform, () => validateBranchPrefixPrimary(bad, platform));
        }
    }
});

test('matching false product and public states are independently rejected for every plan and platform', () => {
    const archive = readBranchPrefixPrimary();
    for (const platform of ['win32', 'linux']) for (const [index, plan] of branchPrefixPlans.entries()) for (const field of ['capacity', 'data']) {
        const primary = structuredClone(archive.platforms[platform]), row = makeCase(plan);
        readMutation(primary.cases[index].state, field); readMutation(row.request1.summary, field); cohere(row);
        assert.deepEqual(primary.cases[index].state, row.request1.summary);
        rejects(platform + plan.id, () => validateBranchPrefixPrimary(primary, platform));
        rejects(platform + plan.id, () => assertBranchPrefixCase(row, plan));
    }
});

test('accepted mutable product and primary reports are checked again', () => {
    const {expected, report} = makeReport('wide'); validateBranchPrefixReport(report, expected);
    report.cases[0].request1.summary.afterPrefix[1].firstState = 'fulfilled'; cohere(report.cases[0]);
    rejects('modified product', () => validateBranchPrefixReport(report, expected));
    const primary = readBranchPrefixPrimary().platforms.win32; validateBranchPrefixPrimary(primary, 'win32');
    primary.cases[0].state.afterPrefix[1].firstState = 'fulfilled';
    rejects('modified primary', () => validateBranchPrefixPrimary(primary, 'win32'));
});

test('synthetic rejection counts stay distinct from actual programs', () => {
    assert.equal(rejected, 1926);
    console.log('NATIVE_FETCH_BRANCH_PREFIX_CONTROLS ' + JSON.stringify({modelCases: 32, rejectedMutations: rejected, actualProductProgramsExecuted: 0}));
});
