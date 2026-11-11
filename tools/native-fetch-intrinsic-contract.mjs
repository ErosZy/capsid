import assert from 'node:assert/strict';
import fs from 'node:fs';
import {gunzipSync} from 'node:zlib';
import {createHash} from 'node:crypto';
import {isDeepStrictEqual} from 'node:util';
import {nativeFetchIntrinsicPlans, nativeFetchIntrinsicCases, expectedNativeFetchIntrinsicState,
    expectedNativeFetchIntrinsicSource, nativeFetchIntrinsicPacket, nativeFetchIntrinsicPrimaryIdentities} from './native-fetch-intrinsic-input.mjs';

const equal = (actual, expected, label) => assert.ok(isDeepStrictEqual(actual, expected), label);
function fields(value, names, label) {
    assert.ok(value && typeof value === 'object' && !Array.isArray(value), label);
    equal(Object.keys(value).sort(), names.slice().sort(), label + ' fields');
}
function source(value, plan) {
    assert.ok(Number.isSafeInteger(value?.port) && value.port > 0 && value.port < 65536, 'actual source port');
    equal(value, expectedNativeFetchIntrinsicSource(plan, value.port), 'full raw request, response, events and pre-cleanup FIN');
}
export function assertNativeFetchIntrinsicCase(row, plan) {
    fields(row, ['plan', 'collectionComplete', 'packetBase64', 'request1', 'sourceTerminalBeforeContinue',
        'sourceBeforeContinue', 'request2', 'sourceBeforeStop', 'workerExit', 'helperExit', 'sourceAfterExit',
        'stdout', 'stderr', 'sourceFinalBeforeCleanup'], 'actual intrinsic case');
    equal(row.plan, plan, 'current required plan'); equal(row.collectionComplete, true, 'complete observation');
    equal(row.packetBase64, nativeFetchIntrinsicPacket(plan).toString('base64'), 'complete actual sent response packet');
    for (const index of [1, 2]) equal(row['request' + index],
        {summary: expectedNativeFetchIntrinsicState(plan), requestId: String(index)}, 'full actual state in both User requests');
    equal(row.sourceTerminalBeforeContinue, true, 'source completes before second User request and Worker stop');
    source(row.sourceBeforeContinue, plan);
    for (const key of ['sourceBeforeStop', 'sourceAfterExit', 'sourceFinalBeforeCleanup'])
        equal(row[key], row.sourceBeforeContinue, 'fixture or Worker teardown cannot supply missing completion');
    equal(row.helperExit, {code: 0, signal: null}, 'actual helper exit');
    equal(row.workerExit, {reaped: true, channelClosed: true, code: 0}, 'natural Worker OS exit');
    equal(row.stderr, '', 'no runtime diagnostics');
    assert.equal(typeof row.stdout, 'string');
    const lines = row.stdout.trimEnd().split(/\r?\n/), features = process.platform === 'win32' ? 0 : 1;
    equal(lines.length, 6, 'complete actual helper output');
    equal(lines[0], `memoryLimit=0 actualFeatures=${features} expectedFeatures=${features}`, 'unchanged helper limits');
    equal([lines[1], lines[3]], ['BINDING_REQUEST 1', 'BINDING_REQUEST 2'], 'actual request sequence');
    for (const [request, index] of [[1, 2], [2, 4]]) {
        assert.ok(lines[index].startsWith('BINDING_PHASE ' + request + ' '));
        equal(JSON.parse(lines[index].slice(16)), row['request' + request], 'actual C ABI payload');
    }
    assert.ok(lines[5].startsWith('BINDING_EXIT '));
    equal(JSON.parse(lines[5].slice(13)), row.workerExit, 'actual reaped OS exit report');
}
export function validateNativeFetchIntrinsicReport(report, expected) {
    fields(report, ['schema', 'complete', 'kind', 'runId', 'inputs', 'binaries', 'cases'], 'intrinsic report');
    equal(report.schema, 'capsid-native-fetch-intrinsic-v1', 'schema'); equal(report.complete, true, 'actual complete report');
    for (const key of ['kind', 'runId', 'inputs', 'binaries']) equal(report[key], expected[key], 'actual identity ' + key);
    const plans = nativeFetchIntrinsicCases(report.kind);
    equal(report.cases.map(row => row.plan), plans, 'all required ordered cases');
    report.cases.forEach((row, index) => assertNativeFetchIntrinsicCase(row, plans[index]));
}
const binaries = {win32: 'c8799724c4221af61dde76ed071aa5996b25c9bd5d247da94c166718cabe612a',
    linux: '50f31156a9a4c7d549f1b4753dd61eb527e1b6b0cd416e5ba3d02d64dfdee5dd'};
export function validateNativeFetchIntrinsicPrimary(report, platform, identities = nativeFetchIntrinsicPrimaryIdentities()) {
    fields(report, ['schema', 'complete', ...Object.keys(identities), 'referenceLock', 'binarySHA256', 'client',
        'cases', 'shutdown', 'stderr'], 'actual primary report');
    equal(report.schema, 'capsid-native-fetch-intrinsic-reference-v1', 'primary schema');
    equal(report.complete, true, 'actual primary completed');
    for (const [key, value] of Object.entries(identities)) equal(report[key], value, 'primary input ' + key);
    assert.ok(Object.hasOwn(binaries, platform));
    equal(report.binarySHA256, binaries[platform], 'actual pinned executable');
    equal(report.referenceLock.binary_sha256, binaries[platform], 'pinned platform lock');
    equal(report.referenceLock.source_commit, '8f311ae3e3cf3064983f56a6a1c8b7ee33d6b99f', 'pinned source');
    equal(report.referenceLock.release_tag, 'v1.20261010.1', 'pinned release');
    equal(report.referenceLock.compatibility_date, '2026-08-20', 'pinned date');
    equal(report.referenceLock.compatibility_flags, [], 'pinned flags');
    equal(report.client, {node: 'v24.14.0', undici: '7.21.0'}, 'pinned actual client');
    equal(report.stderr, '', 'no reference diagnostics');
    equal(report.shutdown, platform === 'win32' ? {code: null, signal: 'SIGTERM'} : {code: 0, signal: null}, 'actual reference shutdown');
    equal(report.cases.map(row => row.plan), nativeFetchIntrinsicPlans, 'all mandatory primary cases');
    for (const [index, row] of report.cases.entries()) {
        const plan = nativeFetchIntrinsicPlans[index];
        fields(row, ['plan', 'packetBase64', 'httpStatus', 'state', 'sourceTerminalBeforeReferenceStop',
            'sourceBeforeReferenceStop', 'collectionComplete', 'sourceAfterReferenceStop'], 'actual primary case');
        equal(row.httpStatus, 200, 'actual public User response'); equal(row.collectionComplete, true, 'actual reference case');
        equal(row.packetBase64, nativeFetchIntrinsicPacket(plan).toString('base64'), 'actual primary response wire');
        equal(row.state, expectedNativeFetchIntrinsicState(plan), 'complete public workerd state');
        equal(row.sourceTerminalBeforeReferenceStop, true, 'primary source finishes before teardown');
        source(row.sourceBeforeReferenceStop, plan);
        equal(row.sourceAfterReferenceStop, row.sourceBeforeReferenceStop, 'reference teardown supplies no missing completion');
    }
}
export function readNativeFetchIntrinsicPrimary() {
    const metadata = JSON.parse(fs.readFileSync(new URL('../tests/fixtures/workerd-native-fetch-intrinsic-report.json', import.meta.url), 'utf8'));
    const compressed = fs.readFileSync(new URL('../tests/fixtures/workerd-native-fetch-intrinsic-report.json.gz', import.meta.url));
    const digest = value => createHash('sha256').update(value).digest('hex'), raw = gunzipSync(compressed);
    fields(metadata, ['schema', 'compressedSHA256', 'decodedSHA256', 'platforms'], 'primary archive metadata');
    equal(metadata.schema, 'capsid-native-fetch-intrinsic-archive-v1', 'archive schema');
    equal(metadata.compressedSHA256, digest(compressed), 'complete compressed archive identity');
    equal(metadata.decodedSHA256, digest(raw), 'complete decoded archive identity');
    const report = JSON.parse(raw);
    fields(report, ['schema', 'platforms'], 'primary archive');
    equal(report.schema, 'capsid-native-fetch-intrinsic-archive-v1', 'actual archive schema');
    equal(Object.keys(report.platforms).sort(), ['linux', 'win32'], 'both actual platforms');
    equal(metadata.platforms, ['win32', 'linux'], 'ordered actual platform coverage');
    for (const platform of ['win32', 'linux']) validateNativeFetchIntrinsicPrimary(report.platforms[platform], platform);
    return report;
}
