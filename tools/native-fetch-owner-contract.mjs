import assert from 'node:assert/strict';
import fs from 'node:fs';
import {createHash} from 'node:crypto';
import {isDeepStrictEqual} from 'node:util';
import {nativeFetchOwnerPlans, nativeFetchOwnerShards, nativeFetchOwnerCasesForShard, nativeFetchOwnerReply} from './native-fetch-owner-input.mjs';

const hash = value => createHash('sha256').update(value).digest('hex');
const equal = (actual, expected, label = 'complete current typed state') => assert.ok(isDeepStrictEqual(actual, expected), label);
function fields(value, names, label) {
    assert.ok(value && typeof value === 'object' && !Array.isArray(value), label);
    equal(Object.keys(value).sort(), [...names].sort(), label+' complete fields');
}
export function expectedNativeFetchOwnerState(plan) {
    const reply = nativeFetchOwnerReply(plan);
    const headers = [['connection', 'close'], ['content-encoding', 'gzip'], ['content-length', String(reply.payload.length)],
        ['content-type', 'application/octet-stream'], ['x-decoded-bytes', String(plan.bytes)]];
    const metadata = bodyUsed => ({status: 201, bodyUsed, locked: false, headers: structuredClone(headers)});
    const read = done => ({done, inputAfterIssue: 0, inputAfter: 0, type: 'Uint8Array', offset: done ? 0 : 8,
        length: done ? 0 : 64, capacity: 80, bytes: done ? [] : Array(64).fill(0),
        backing: done ? Array(80).fill(165) : [...Array(8).fill(165), ...Array(64).fill(0), ...Array(8).fill(165)]});
    const state = {action: plan.action, failure: null, reads: [], before: metadata(false), after: metadata(plan.action !== 'retain')};
    if (plan.action === 'retain') state.retained = true;
    else if (plan.action === 'stream-cancel') state.cancel = {fulfilled: true};
    else {
        state.closedStable = true;
        if (plan.action === 'abort') {
            const error = {name: 'Error', message: 'owner probe stop', sameReason: false};
            state.aborted = true; state.closed = {error: structuredClone(error)};
            state.reads.push({error, inputAfterIssue: 0, inputAfter: 0});
        } else {
            state.cancel = {fulfilled: true}; state.closed = {fulfilled: true};
            if (plan.action === 'read-cancel') state.reads.push(read(false));
            state.reads.push(read(true));
        }
    }
    return state;
}
function assertRawRequest(source, port) {
    assert.ok(Number.isSafeInteger(port) && port > 0 && port < 65536);
    const raw = Buffer.from(source.rawRequest, 'base64'); equal(raw.toString('base64'), source.rawRequest, 'canonical full request bytes');
    equal(raw.toString(), source.request+'\r\n\r\n');
    equal(raw.toString(), 'GET /hold HTTP/1.1\r\nHost: localhost:'+port+'\r\n\r\n', 'complete controlled request bytes');
    const lines = source.request.split('\r\n'); equal(lines.shift(), 'GET /hold HTTP/1.1');
    equal(lines.filter(line => line.slice(0, line.indexOf(':')).toLowerCase() === 'host')
        .map(line => line.slice(line.indexOf(':')+1).trim()), ['localhost:'+port]);
}
function assertSource(source, plan, port, requireTerminal, reference = false) {
    fields(source, ['request', 'rawRequest', 'peerEnded', 'peerReset', 'fixtureEnded', 'fixtureDestroyed', 'events', 'sent', ...(reference ? ['packetSHA256'] : [])], 'actual native owner source');
    equal(typeof source.peerEnded, 'boolean'); equal(typeof source.peerReset, 'boolean');
    equal(source.fixtureEnded, true); equal(source.fixtureDestroyed, false);
    // Cancellation can close with FIN or reset while unread bytes remain.
    // Both must be real peer observations before any fixture/worker teardown.
    if (requireTerminal) assert.ok(source.peerEnded || source.peerReset, 'actual peer terminal before host cleanup');
    equal(source.events, ['request', 'sent-complete', ...(source.peerEnded ? ['peer-ended'] : [])]);
    equal(source.sent, nativeFetchOwnerReply(plan).packet.toString('base64'), 'all actual encoded bytes and headers');
    if (reference) equal(source.packetSHA256, hash(nativeFetchOwnerReply(plan).packet));
    assertRawRequest(source, port);
}
export function assertNativeFetchOwnerReferenceArchive(archive, fixtureSHA256) {
    fields(archive, ['schema', 'reports'], 'native owner reference archive');
    equal(archive.schema, 'capsid-native-fetch-owner-reference-v1'); equal(archive.reports.length, 2);
    const states = [];
    for (const [index, item] of archive.reports.entries()) {
        fields(item, ['platform', 'originalReportSHA256', 'originalTextUTF8'], 'actual primary provenance');
        equal(item.platform, index ? 'linux' : 'native'); equal(typeof item.originalTextUTF8, 'string');
        equal(hash(item.originalTextUTF8), item.originalReportSHA256, 'exact original primary document bytes');
        const report = JSON.parse(item.originalTextUTF8);
        equal(report.complete, true); equal(report.fixtureSHA256, fixtureSHA256); equal(report.cases.length, 10);
        equal(report.referenceLock.source_commit, '8f311ae3e3cf3064983f56a6a1c8b7ee33d6b99f');
        equal(report.referenceLock.release_tag, 'v1.20261010.1'); equal(report.referenceLock.compatibility_date, '2026-08-20');
        equal(report.referenceLock.compatibility_flags, []);
        equal(report.binarySHA256, index ? '50f31156a9a4c7d549f1b4753dd61eb527e1b6b0cd416e5ba3d02d64dfdee5dd' : 'c8799724c4221af61dde76ed071aa5996b25c9bd5d247da94c166718cabe612a');
        equal(report.referenceLock.binary_sha256, report.binarySHA256);
        equal(report.client, {node: 'v24.14.0', undici: '7.21.0'});
        for (const [caseIndex, row] of report.cases.entries()) {
            const plan = nativeFetchOwnerPlans[caseIndex]; equal(row.plan, plan);
            equal(row.state, expectedNativeFetchOwnerState(plan), 'independent primary typed/byte/backing/error model');
            equal(row.encodedBytes, nativeFetchOwnerReply(plan).payload.length);
            // A reference may retain an unread body. Its pre-stop source
            // snapshot remains explicit; Capsid has its own per-request cleanup.
            assertSource(row.sourceBeforeReferenceStop, plan, row.port, plan.action !== 'retain', true);
            equal(row.payloadSHA256, hash(nativeFetchOwnerReply(plan).payload));
            equal(row.packetSHA256, hash(nativeFetchOwnerReply(plan).packet));
            equal(row.sourceEndedBeforeReferenceStop, row.sourceBeforeReferenceStop.peerEnded);
            equal(row.sourceTerminalBeforeReferenceStop, row.sourceBeforeReferenceStop.peerEnded || row.sourceBeforeReferenceStop.peerReset);
            if (!index) states.push(structuredClone(row.state));
            else equal(row.state, states[caseIndex], 'both actual fixed primary states');
        }
    }
    return states;
}
const fixtureSHA256 = hash(fs.readFileSync(new URL('../tests/fixtures/native-fetch-owner.mjs', import.meta.url)));
assertNativeFetchOwnerReferenceArchive(JSON.parse(fs.readFileSync(new URL('../tests/fixtures/workerd-native-fetch-owner-report.json', import.meta.url), 'utf8')), fixtureSHA256);

export function assertNativeFetchOwnerCase(row, plan) {
    fields(row, ['plan', 'source', 'port', 'encodedBytes', 'state', 'sourceTerminalBeforeFinish', 'sourceBeforeFinish',
        'shutdown', 'stdout', 'stderr', 'workerExit'], 'actual native owner case');
    equal(row.plan, plan); equal(row.encodedBytes, nativeFetchOwnerReply(plan).payload.length);
    equal(row.state, expectedNativeFetchOwnerState(plan), 'full independent typed and primary owner state');
    assertSource(row.source, plan, row.port, true);
    equal(row.sourceBeforeFinish, row.source, 'actual source terminal before host acknowledgment');
    equal(row.sourceTerminalBeforeFinish, true);
    equal(row.workerExit, {clean: true, processObserved: true, exitStatus: 0, forced: false, errorEvents: 0});
    equal(row.shutdown, {code: 0, signal: null}); equal(row.stderr, '');
    equal(typeof row.stdout, 'string');
    const lines = row.stdout.trim().split(/\r?\n/);
    const config = {heapBytes: 67108864, responseLimit: 0, wireVersion: 4, capabilities: 123, maxChannels: 1};
    equal(lines.filter(line => line.startsWith('OWNER_CONFIG ')).map(line => JSON.parse(line.slice(13))), [config]);
    equal(lines.filter(line => line.startsWith('OWNER_PROBE ')), ['OWNER_PROBE '+JSON.stringify(row.state)]);
    equal(lines.filter(line => line.startsWith('OWNER_EXIT ')), ['OWNER_EXIT '+JSON.stringify(row.workerExit)]);
    assert.ok(lines.every(line => line.startsWith('OWNER_CONFIG ') || line.startsWith('OWNER_PROBE ') || line.startsWith('OWNER_EXIT ') ||
        /^OWNER_AUX type=(?:6|10|11) request=\d+ status=\d+ payload=.*$/.test(line)), 'only actual expected public ABI observations');
    return row.state;
}
export function validateNativeFetchOwnerReport(report, expected) {
    fields(report, ['schema', 'complete', 'shard', 'runId', 'inputs', 'binaries', 'cases'], 'native owner report');
    equal(report.schema, 'capsid-native-fetch-owner-v1'); equal(report.complete, true);
    assert.ok(nativeFetchOwnerShards.includes(report.shard)); equal(report.shard, expected.shard); equal(report.runId, expected.runId);
    equal(report.inputs, expected.inputs); equal(report.binaries, expected.binaries);
    const plans = nativeFetchOwnerCasesForShard(report.shard);
    equal(report.cases.map(row => row.plan.id), plans.map(plan => plan.id));
    return report.cases.map((row, index) => assertNativeFetchOwnerCase(row, plans[index]));
}
