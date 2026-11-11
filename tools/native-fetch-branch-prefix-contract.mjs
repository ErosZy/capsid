import assert from 'node:assert/strict';
import fs from 'node:fs';
import {brotliDecompressSync} from 'node:zlib';
import {createHash} from 'node:crypto';
import {isDeepStrictEqual} from 'node:util';
import {branchPrefixPlans, branchPrefixCases, branchPrefixPacket, expectedBranchPrefixState,
    expectedBranchPrefixSource, branchPrefixPrimaryIdentities} from './native-fetch-branch-prefix-input.mjs';

const equal = (actual, expected, label) => assert.ok(isDeepStrictEqual(actual, expected), label);
function fields(value, keys, label) {
    assert.ok(value && typeof value === 'object' && !Array.isArray(value), label);
    equal(Object.keys(value).sort(), keys.slice().sort(), label + ' fields');
}
function source(value, plan) {
    const release = ['continue', 'cancel0', 'cancel1', 'eof'].includes(plan.action);
    // This first post-action control connection races the source FIN. Preserve
    // its actual bit; the next acknowledged control must observe source end.
    const earlyFin = value?.phases?.[3]?.sourceEndedBefore;
    assert.equal(typeof earlyFin, 'boolean', 'actual first post-action FIN observation');
    if (release) equal(earlyFin, false, 'source remains held until final control');
    equal(value, expectedBranchPrefixSource(plan, value.port, earlyFin),
        'complete prefix/body/control wire, pending state, acknowledged stop and pre-cleanup FIN');
}
export function assertBranchPrefixCase(row, plan) {
    fields(row, ['plan', 'collectionComplete', 'packetBase64', 'request1', 'sourceTerminalBeforeContinue',
        'sourceBeforeContinue', 'request2', 'sourceBeforeStop', 'workerExit', 'helperExit', 'sourceAfterExit',
        'stdout', 'stderr', 'sourceFinalBeforeCleanup'], 'actual native branch prefix case');
    equal(row.plan, plan, 'required plan'); equal(row.collectionComplete, true, 'complete collection');
    equal(row.packetBase64, branchPrefixPacket(plan).toString('base64'), 'complete source packet');
    const state = expectedBranchPrefixState(plan);
    for (const index of [1, 2]) equal(row['request' + index], {summary: state, requestId: String(index)},
        'every state, read, typed minimum, backing, EOF and cancellation in both actual requests');
    equal(row.sourceTerminalBeforeContinue, true, 'source closes before second User request and Worker stop');
    source(row.sourceBeforeContinue, plan);
    for (const key of ['sourceBeforeStop', 'sourceAfterExit', 'sourceFinalBeforeCleanup'])
        equal(row[key], row.sourceBeforeContinue, 'teardown cannot supply missing source completion');
    equal(row.helperExit, {code: 0, signal: null}, 'actual helper exit');
    equal(row.workerExit, {reaped: true, channelClosed: true, code: 0}, 'natural reaped Worker exit');
    equal(row.stderr, '', 'no runtime diagnostics'); assert.equal(typeof row.stdout, 'string');
    const lines = row.stdout.trimEnd().split(/\r?\n/), features = process.platform === 'win32' ? 0 : 1;
    equal(lines.length, 6, 'complete helper output');
    equal(lines[0], `memoryLimit=0 actualFeatures=${features} expectedFeatures=${features}`, 'unchanged helper budget and features');
    equal([lines[1], lines[3]], ['BINDING_REQUEST 1', 'BINDING_REQUEST 2'], 'real User request order');
    for (const [request, index] of [[1, 2], [2, 4]]) {
        assert.ok(lines[index].startsWith('BINDING_PHASE ' + request + ' '));
        equal(JSON.parse(lines[index].slice(16)), row['request' + request], 'actual lossless C ABI payload');
    }
    assert.ok(lines[5].startsWith('BINDING_EXIT '));
    equal(JSON.parse(lines[5].slice(13)), row.workerExit, 'actual OS exit report');
}
export function validateBranchPrefixReport(report, expected) {
    fields(report, ['schema', 'complete', 'method', 'runId', 'inputs', 'binaries', 'cases'], 'branch prefix report');
    equal(report.schema, 'capsid-native-fetch-branch-prefix-v1', 'product schema'); equal(report.complete, true, 'complete actual report');
    for (const key of ['method', 'runId', 'inputs', 'binaries']) equal(report[key], expected[key], 'current identity ' + key);
    const plans = branchPrefixCases(report.method);
    equal(report.cases.map(row => row.plan), plans, 'all mandatory ordered prefix cases');
    report.cases.forEach((row, index) => assertBranchPrefixCase(row, plans[index]));
}
const binaries = {win32: 'c8799724c4221af61dde76ed071aa5996b25c9bd5d247da94c166718cabe612a',
    linux: '50f31156a9a4c7d549f1b4753dd61eb527e1b6b0cd416e5ba3d02d64dfdee5dd'};
export function validateBranchPrefixPrimary(report, platform, identities = branchPrefixPrimaryIdentities()) {
    fields(report, ['schema', 'complete', ...Object.keys(identities), 'referenceLock', 'binarySHA256', 'client',
        'cases', 'shutdown', 'stderr'], 'actual branch prefix primary');
    equal(report.schema, 'capsid-native-fetch-branch-prefix-reference-v1', 'primary schema');
    equal(report.complete, true, 'complete actual primary');
    for (const [key, value] of Object.entries(identities)) equal(report[key], value, 'primary input ' + key);
    assert.ok(Object.hasOwn(binaries, platform));
    equal(report.binarySHA256, binaries[platform], 'fixed executable');
    equal(report.referenceLock.binary_sha256, binaries[platform], 'fixed platform lock');
    equal(report.referenceLock.source_commit, '8f311ae3e3cf3064983f56a6a1c8b7ee33d6b99f', 'fixed source');
    equal(report.referenceLock.release_tag, 'v1.20261010.1', 'fixed release');
    equal(report.referenceLock.compatibility_date, '2026-08-20', 'fixed compatibility date');
    equal(report.referenceLock.compatibility_flags, [], 'fixed compatibility flags');
    equal(report.client, {node: 'v24.14.0', undici: '7.21.0'}, 'fixed actual client');
    equal(report.stderr, '', 'no reference diagnostics');
    equal(report.shutdown, platform === 'win32' ? {code: null, signal: 'SIGTERM'} : {code: 0, signal: null}, 'actual reference shutdown');
    equal(report.cases.map(row => row.plan), branchPrefixPlans, 'all required ordered primary cases');
    for (const [index, row] of report.cases.entries()) {
        const plan = branchPrefixPlans[index];
        fields(row, ['plan', 'packetBase64', 'httpStatus', 'state', 'sourceTerminalBeforeReferenceStop',
            'sourceBeforeReferenceStop', 'collectionComplete', 'sourceAfterReferenceStop'], 'actual primary case');
        equal(row.httpStatus, 200, 'actual public response'); equal(row.collectionComplete, true, 'actual complete primary case');
        equal(row.packetBase64, branchPrefixPacket(plan).toString('base64'), 'actual primary response wire');
        equal(row.state, expectedBranchPrefixState(plan), 'independent complete public state model');
        equal(row.sourceTerminalBeforeReferenceStop, true, 'reference source completes before reference teardown');
        source(row.sourceBeforeReferenceStop, plan);
        equal(row.sourceAfterReferenceStop, row.sourceBeforeReferenceStop, 'reference stop supplies no missing completion');
    }
}
export function readBranchPrefixPrimary() {
    const metadata = JSON.parse(fs.readFileSync(new URL('../tests/fixtures/workerd-native-fetch-branch-prefix-report.json', import.meta.url), 'utf8'));
    const compressed = fs.readFileSync(new URL('../tests/fixtures/workerd-native-fetch-branch-prefix-report.json.br', import.meta.url));
    const digest = bytes => createHash('sha256').update(bytes).digest('hex');
    fields(metadata, ['schema', 'compressedSHA256', 'decodedSHA256', 'platforms'], 'primary archive metadata');
    equal(metadata.schema, 'capsid-native-fetch-branch-prefix-archive-v1', 'archive schema');
    equal(metadata.compressedSHA256, digest(compressed), 'complete compressed identity');
    const raw = brotliDecompressSync(compressed, {maxOutputLength: 64 * 1024 * 1024});
    equal(metadata.decodedSHA256, digest(raw), 'complete decoded identity');
    const archive = JSON.parse(raw);
    fields(archive, ['schema', 'platforms'], 'actual primary archive');
    equal(archive.schema, metadata.schema, 'actual archive schema');
    equal(Object.keys(archive.platforms).sort(), ['linux', 'win32'], 'both fixed public platforms');
    equal(metadata.platforms, ['win32', 'linux'], 'ordered platform coverage');
    for (const platform of ['win32', 'linux']) validateBranchPrefixPrimary(archive.platforms[platform], platform);
    equal(archive.platforms.win32.cases.map(row => row.state), archive.platforms.linux.cases.map(row => row.state),
        'both actual public platforms agree on every full state');
    return archive;
}
