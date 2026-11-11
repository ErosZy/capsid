import assert from 'node:assert/strict';
import fs from 'node:fs';
import {createHash} from 'node:crypto';
import {nativeFetchAbortPrefixPlans as plans, nativeFetchAbortPrefixReply, expectedNativeFetchAbortPrefixState} from './native-fetch-abort-prefix-input.mjs';

const hash = value => createHash('sha256').update(value).digest('hex');
const fields = (value, names) => {
    assert.ok(value && typeof value === 'object' && !Array.isArray(value));
    assert.deepEqual(Object.keys(value).sort(), names.slice().sort());
};
function stateModel(row, plan) {
    assert.deepEqual(row.plan, plan); assert.deepEqual(row.state, expectedNativeFetchAbortPrefixState(plan));
    const reply = nativeFetchAbortPrefixReply(plan);
    assert.equal(row.encodedBytes, reply.payload.length); assert.equal(row.payloadSHA256, hash(reply.payload)); assert.equal(row.packetSHA256, hash(reply.packet));
}
function sourceModel(row, primary) {
    const source = primary ? row.sourceBeforeReferenceStop : row.sourceBeforeFinish;
    fields(source, ['request', 'rawRequest', 'peerEnded', 'peerReset', 'fixtureEnded', 'fixtureDestroyed', 'packetSHA256', 'events']);
    assert.ok(Number.isSafeInteger(row.port) && row.port > 0 && row.port < 65536);
    const request = 'GET /hold HTTP/1.1\r\nHost: localhost:'+row.port+'\r\n\r\n';
    assert.equal(source.request, request.slice(0, -4)); assert.equal(source.rawRequest, Buffer.from(request).toString('base64'));
    assert.equal(typeof source.peerEnded, 'boolean'); assert.equal(typeof source.peerReset, 'boolean'); assert.ok(source.peerEnded || source.peerReset);
    assert.equal(source.fixtureEnded, true); assert.equal(source.fixtureDestroyed, false); assert.equal(source.packetSHA256, row.packetSHA256);
    assert.deepEqual(source.events, ['request', 'sent-complete', ...(source.peerEnded ? ['peer-ended'] : [])]);
    assert.equal(primary ? row.sourceEndedBeforeReferenceStop : row.sourceEndedBeforeFinish, source.peerEnded);
    assert.equal(primary ? row.sourceTerminalBeforeReferenceStop : row.sourceTerminalBeforeFinish, true);
    assert.deepEqual(row.source, source, 'terminal observation precedes host/reference stop and teardown');
}
export function assertNativeFetchAbortPrefixReferenceArchive(archive, fixtureSHA256) {
    fields(archive, ['schema', 'reports']); assert.equal(archive.schema, 'capsid-native-fetch-abort-prefix-reference-v1'); assert.equal(archive.reports.length, 4);
    for (const [index, item] of archive.reports.entries()) {
        fields(item, ['profile', 'platform', 'originalTextUTF8', 'originalReportSHA256']);
        assert.equal(item.profile, index < 2 ? 'raw' : 'gzip'); assert.equal(item.platform, index%2 ? 'linux' : 'native');
        assert.equal(hash(Buffer.from(item.originalTextUTF8)), item.originalReportSHA256);
        const report = JSON.parse(item.originalTextUTF8); assert.equal(report.complete, true); assert.equal(report.cases.length, 4); assert.equal(report.fixtureSHA256, fixtureSHA256);
        assert.equal(report.referenceLock.source_commit, '8f311ae3e3cf3064983f56a6a1c8b7ee33d6b99f'); assert.equal(report.referenceLock.release_tag, 'v1.20261010.1');
        assert.equal(report.referenceLock.compatibility_date, '2026-08-20'); assert.deepEqual(report.referenceLock.compatibility_flags, []);
        assert.equal(report.binarySHA256, index%2 ? '50f31156a9a4c7d549f1b4753dd61eb527e1b6b0cd416e5ba3d02d64dfdee5dd' : 'c8799724c4221af61dde76ed071aa5996b25c9bd5d247da94c166718cabe612a');
        assert.equal(report.referenceLock.binary_sha256, report.binarySHA256); assert.deepEqual(report.client, {node: 'v24.14.0', undici: '7.21.0'});
        assert.equal(report.stderr, ''); assert.ok(report.shutdown.signal === 'SIGTERM' || (report.shutdown.code === 0 && report.shutdown.signal === null));
        for (const [i, row] of report.cases.entries()) {stateModel(row, plans[(index < 2 ? 0 : 4)+i]); sourceModel(row, true);}
    }
}
const archive = JSON.parse(fs.readFileSync(new URL('../tests/fixtures/workerd-native-fetch-abort-prefix-report.json', import.meta.url), 'utf8'));
assertNativeFetchAbortPrefixReferenceArchive(archive, hash(fs.readFileSync(new URL('../tests/fixtures/native-fetch-abort-prefix.mjs', import.meta.url))));
export function assertNativeFetchAbortPrefixCase(row, plan) {
    fields(row, ['plan', 'encodedBytes', 'payloadSHA256', 'packetSHA256', 'source', 'port', 'state', 'sourceEndedBeforeFinish', 'sourceTerminalBeforeFinish', 'sourceBeforeFinish', 'shutdown', 'stdout', 'stderr', 'workerExit']);
    stateModel(row, plan); sourceModel(row, false); assert.deepEqual(row.shutdown, {code: 0, signal: null}); assert.equal(row.stderr, '');
    assert.deepEqual(row.workerExit, {clean: true, processObserved: true, exitStatus: 0, forced: false, errorEvents: 0});
    const lines = row.stdout.trim().split(/\r?\n/);
    assert.deepEqual(lines.filter(line => line.startsWith('LIMIT_PROBE ')), ['LIMIT_PROBE '+JSON.stringify(row.state)]);
    assert.deepEqual(lines.filter(line => line.startsWith('LIMIT_EXIT ')), ['LIMIT_EXIT '+JSON.stringify(row.workerExit)]);
    assert.ok(lines.every(line => line.startsWith('LIMIT_PROBE ') || line.startsWith('LIMIT_EXIT ') || /^LIMIT_AUX type=(?:6|10|11) request=\d+ status=\d+ payload=.*$/.test(line)));
    return row.state;
}
export function validateNativeFetchAbortPrefixReport(report, expected) {
    fields(report, ['schema', 'complete', 'runId', 'inputs', 'binaries', 'cases']); assert.equal(report.schema, 'capsid-native-fetch-abort-prefix-v1');
    assert.equal(report.complete, true); assert.equal(report.runId, expected.runId); assert.deepEqual(report.inputs, expected.inputs); assert.deepEqual(report.binaries, expected.binaries);
    assert.deepEqual(report.cases.map(row => row.plan.id), plans.map(plan => plan.id));
    return report.cases.map((row, index) => assertNativeFetchAbortPrefixCase(row, plans[index]));
}
