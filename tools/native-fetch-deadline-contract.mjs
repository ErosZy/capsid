import assert from 'node:assert/strict';
import fs from 'node:fs';
import {createHash} from 'node:crypto';
import {gunzipSync} from 'node:zlib';
import {isDeepStrictEqual} from 'node:util';
import {nativeFetchDeadlinePlans, nativeFetchDeadlineReply, nativeFetchDeadlinePreControl,
    nativeFetchDeadlineCasesForShard, expectedNativeFetchDeadlineState} from './native-fetch-deadline-input.mjs';

const hash = bytes => createHash('sha256').update(bytes).digest('hex');
const equal = (actual, expected, label) => assert.ok(isDeepStrictEqual(actual, expected), label);
function fields(value, names, label) {
    assert.ok(value && typeof value === 'object' && !Array.isArray(value), label);
    equal(Object.keys(value).sort(), names.slice().sort(), `${label} fields`);
}
export const nativeFetchDeadlineExit = Object.freeze({clean: true, processObserved: true,
    exitStatus: 0, forced: false, errorEvents: 0});

export function expectedNativeFetchDeadlineSource(plan, port) {
    const record = (role, target, sent, ended) => {
        const request = `GET ${target} HTTP/1.1\r\nHost: localhost:${port}`;
        return {role, rawRequest: Buffer.from(request + '\r\n\r\n').toString('base64'), request,
            sent: sent.map(bytes => bytes.toString('base64')),
            events: ['accepted', 'request', ...(role === 'source' && sent.length ? ['sent-head'] : []),
                ...(ended ? ['sent-end'] : []), 'peer-ended'],
            peerEnded: true, peerReset: false, fixtureEnded: ended, fixtureDestroyed: false};
    };
    const headless = plan.stage === 'headless';
    const source = {plan, records: [record('source', '/hold', headless ? [] : [nativeFetchDeadlineReply(plan).head], false)],
        failure: null, controlBeforeBody: !headless, port};
    if (!headless) {
        source.phase = {stage: plan.stage, trigger: plan.trigger, link: plan.link,
            preControl: nativeFetchDeadlinePreControl(plan)};
        const control = new URL('/control', `http://localhost:${port}`);
        control.searchParams.set('phase', JSON.stringify(source.phase));
        const ack = Buffer.from('HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok');
        source.records.push(record('control', control.pathname + control.search, [ack], true));
    }
    return source;
}
export function assertNativeFetchDeadlineSource(source, plan) {
    assert.ok(Number.isSafeInteger(source.port) && source.port > 0 && source.port < 65536, 'actual loopback port');
    const model = expectedNativeFetchDeadlineSource(plan, source.port);
    fields(source, Object.keys(model), 'complete deadline source');
    equal(Object.fromEntries(Object.entries(source).filter(([key]) => key !== 'records')),
        Object.fromEntries(Object.entries(model).filter(([key]) => key !== 'records')), 'complete source metadata');
    equal(source.records.length, model.records.length, 'only expected source/control connections');
    for (const [index, actual] of source.records.entries()) {
        assert.equal(typeof actual.peerEnded, 'boolean');
        assert.equal(typeof actual.peerReset, 'boolean');
        assert.ok(actual.peerEnded || actual.peerReset, 'actual peer FIN or reset before cleanup');
        const expected = model.records[index];
        expected.peerEnded = actual.peerEnded;
        expected.peerReset = actual.peerReset;
        if (!actual.peerEnded) expected.events.pop();
        equal(actual, expected, 'complete HTTP bytes, gating and actual source events');
        for (const value of [actual.rawRequest, ...actual.sent])
            equal(Buffer.from(value, 'base64').toString('base64'), value, 'canonical full wire bytes');
    }
    return source;
}
function timing(value, bound = Infinity) {
    fields(value, ['stateMs', 'terminalMs', 'completeMs'], 'actual collection timing');
    assert.ok(Object.values(value).every(number => Number.isFinite(number) && number >= 0));
    assert.ok(value.stateMs <= value.terminalMs && value.terminalMs <= value.completeMs && value.completeMs < bound,
        'actual monotonic collection timing');
}
const eventPattern = /^DEADLINE_EVENT type=(\d+) request=(\d+) flags=(\d+) status=(\d+) credit=(\d+) payload=([0-9a-f]*)$/;
function eventFields(event, type, request, flags = 0, status = 0, credit = 0) {
    equal(Object.fromEntries(Object.entries(event).filter(([key]) => key !== 'payload')),
        {type, request, flags, status, credit}, 'exact ABI event type/owner/flags/status/credit');
}
function audit(event, source, manifest) {
    eventFields(event, 10, 1);
    const data = event.payload;
    assert.ok(data.length >= 28, 'complete audit prefix');
    equal([data.readUInt32LE(0), data.readUInt32LE(4), data.readUInt32LE(8),
        data.readUInt32LE(20), data.readUInt32LE(24)], [1, 3, 1, 1, 0], 'actual configured egress audit');
    const pid = data.readBigUInt64LE(12);
    assert.ok(pid > 0n, 'actual audit worker identity');
    let offset = 28;
    const values = [];
    for (let index = 0; index < 6; index++) {
        assert.ok(offset + 2 <= data.length, 'complete audit field size');
        const size = data.readUInt16LE(offset);
        offset += 2;
        assert.ok(offset + size <= data.length, 'complete audit field');
        values.push(data.subarray(offset, offset + size).toString('utf8'));
        offset += size;
    }
    equal(offset, data.length, 'no truncated or trailing audit bytes');
    equal(values, ['', '', 'net', 'host', `localhost:${source.port}`, manifest], 'actual resource and linked manifest');
    return pid;
}
function response(events, request, body, contentType) {
    equal(events.length, 3, 'one complete fixed response');
    const prefix = Buffer.alloc(6), size = Buffer.alloc(4);
    prefix.writeUInt16LE(1, 2);
    prefix.writeUInt16LE(12, 4);
    size.writeUInt32LE(Buffer.byteLength(contentType));
    const head = Buffer.concat([prefix, Buffer.from('content-type'), size, Buffer.from(contentType)]);
    eventFields(events[0], 3, request, 1, 200, body.length);
    eventFields(events[1], 4, request);
    eventFields(events[2], 5, request);
    equal(events.map(event => event.payload), [head, body, Buffer.alloc(0)], 'complete response bytes, no prefix acceptance');
}
export function assertNativeFetchDeadlineOutput(row, plan) {
    assert.equal(typeof row.stdout, 'string');
    const lines = row.stdout.trimEnd().split(/\r?\n/);
    const config = /^DEADLINE_CONFIG timeout=(\d+) heap=(\d+) limit=(\d+) process=(\d+) platform=(windows|posix) ready=(\d+) compatibility=(sha256:[0-9a-f]{64}) manifest=([0-9a-f]{64})$/.exec(lines.shift());
    assert.ok(config, 'complete actual configuration and linked runtime identity');
    const [, timeout, heap, limit, processLimit, platform, readyFlags, compatibility, manifest] = config;
    equal([Number(timeout), Number(heap), Number(limit)], [plan.trigger === 'host' ? 750 : 5000, 67108864, 0],
        'unchanged actual timeout and default heap/body limit');
    assert.ok(['0', '268435456'].includes(processLimit), 'default instrumented/ordinary process limit');
    equal(Number(readyFlags), platform === 'windows' && processLimit === '0' ? 0 : 1, 'configured platform defaults');
    const events = [], markers = [];
    for (const line of lines) {
        const match = eventPattern.exec(line);
        if (!match) markers.push({at: events.length, line});
        else {
            const [type, request, flags, status, credit] = match.slice(1, 6).map(Number);
            assert.ok([type, request, flags, status, credit].every(Number.isSafeInteger), 'exact ABI integer fields');
            assert.equal(match[6].length % 2, 0, 'complete hex payload');
            events.push({type, request, flags, status, credit, payload: Buffer.from(match[6], 'hex')});
        }
    }
    const ready = events.shift();
    eventFields(ready, 1, 0, Number(readyFlags));
    equal(ready.payload.toString('ascii'), compatibility, 'actual Worker and linked runtime compatibility');
    const count = plan.stage === 'headless' ? 1 : 2;
    const audits = events.splice(0, count).map(event => audit(event, row.sourceBeforeFinish, manifest));
    assert.ok(audits.every(pid => pid === audits[0]), 'source/control owned by same actual Worker');
    let at = 1 + count;
    if (plan.trigger === 'host') {
        if (plan.stage !== 'headless') {
            const log = events.shift(), prefix = Buffer.from('\x03\x00logNATIVE_DEADLINE_READY ');
            eventFields(log, 6, 1);
            equal(log.payload.subarray(0, prefix.length), prefix, 'complete readiness log envelope');
            equal(JSON.parse(log.payload.subarray(prefix.length).toString()), expectedNativeFetchDeadlineState(plan, true),
                'actual pending/unread/control state before host deadline');
            at++;
        }
        const timeoutEvent = events.shift();
        eventFields(timeoutEvent, 9, 1, 1);
        equal(timeoutEvent.payload, Buffer.from('request deadline exceeded'), 'exact host timeout payload');
        at++;
        equal(markers.shift(), {at, line: 'DEADLINE_TIMEOUT request=1 flags=1 status=0 payload=request deadline exceeded'},
            'actual single host timeout notification');
    } else {
        const probe = markers.filter(marker => marker.line.startsWith('LIMIT_PROBE '));
        equal(probe.length, 1, 'one actual public state');
        response(events.splice(0, 3), 1, Buffer.from(probe[0].line.slice(12)), 'application/json');
        at += 3;
    }
    const elapsed = markers.shift();
    equal(elapsed.at, at, 'elapsed measured at actual deadline/response');
    const elapsedMatch = /^DEADLINE_ELAPSED milliseconds=(\d+)$/.exec(elapsed.line);
    assert.ok(elapsedMatch, 'actual monotonic event duration');
    const milliseconds = Number(elapsedMatch[1]);
    // C API/libuv deadlines and the duration record use integer monotonic
    // milliseconds. Account for one millisecond of boundary quantization;
    // this does not assert sub-millisecond timer equivalence or extend budgets.
    assert.ok(milliseconds >= (plan.trigger === 'host' ? 749 : 499) && milliseconds < 5000);
    assert.ok(milliseconds <= row.timings.stateMs + 1, 'state follows actual observed event');
    const state = markers.shift();
    equal(state.at, at, 'actual state emitted before healthy request');
    assert.ok(state.line.startsWith('LIMIT_PROBE '));
    equal(JSON.parse(state.line.slice(12)), row.state, 'full actual ABI state agrees with report');
    response(events, 2, Buffer.from('healthy'), 'text/plain;charset=UTF-8');
    const exit = markers.shift();
    equal(exit.at, at + 3, 'natural exit follows actual healthy response');
    assert.ok(exit.line.startsWith('LIMIT_EXIT '));
    equal(JSON.parse(exit.line.slice(11)), nativeFetchDeadlineExit, 'actual natural OS exit');
    equal(markers, [], 'no unknown/extra/truncated ABI output');
    return {milliseconds, compatibility, manifest, readyFlags: Number(readyFlags)};
}
export function assertNativeFetchDeadlineCase(row, plan) {
    fields(row, ['plan', 'collectionComplete', 'timings', 'state', 'sourceTerminalBeforeFinish',
        'sourceBeforeFinish', 'sourceAfterFinish', 'shutdown', 'stdout', 'stderr', 'workerExit'], 'actual deadline case');
    equal(row.plan, plan, 'mandatory actual plan');
    equal(row.collectionComplete, true, 'actual observation complete');
    timing(row.timings, 12000);
    equal(row.state, plan.trigger === 'host' ? 'HOST_TIMEOUT' : expectedNativeFetchDeadlineState(plan),
        'complete independent typed/prototype/identity/detachment/trace model');
    equal(row.sourceTerminalBeforeFinish, true, 'actual peers terminal before helper finish');
    assertNativeFetchDeadlineSource(row.sourceBeforeFinish, plan);
    equal(row.sourceAfterFinish, row.sourceBeforeFinish, 'helper/Worker shutdown supplies no peer completion');
    equal(row.shutdown, {code: 0, signal: null}, 'actual helper exit');
    equal(row.workerExit, nativeFetchDeadlineExit, 'actual Worker natural OS exit');
    equal(row.stderr, '', 'no runtime diagnostics');
    return assertNativeFetchDeadlineOutput(row, plan);
}
export function validateNativeFetchDeadlineReport(report, expected) {
    fields(report, ['schema', 'complete', 'shard', 'runId', 'inputs', 'binaries', 'cases'], 'deadline report');
    equal(report.schema, 'capsid-native-fetch-deadline-v1', 'report schema');
    equal(report.complete, true, 'actual complete report');
    for (const key of ['shard', 'runId', 'inputs', 'binaries']) equal(report[key], expected[key], 'actual identity ' + key);
    const plans = nativeFetchDeadlineCasesForShard(report.shard);
    equal(report.cases.map(row => row.plan), plans, 'all mandatory ordered cases, no omissions/duplicates');
    return report.cases.map((row, index) => assertNativeFetchDeadlineCase(row, plans[index]));
}
export function nativeFetchDeadlinePrimaryIdentities() {
    return Object.fromEntries([
        ['fixtureSHA256', '../tests/fixtures/native-fetch-deadline.mjs'], ['peerSHA256', './native-fetch-deadline-peer.mjs'],
        ['sourceModelSHA256', './native-fetch-deadline-input.mjs'], ['captureSHA256', './capture-native-fetch-deadline-reference.mjs'],
        ['contractSHA256', './native-fetch-deadline-contract.mjs'],
    ].map(([key, name]) => [key, hash(fs.readFileSync(new URL(name, import.meta.url)))]));
}
export function assertNativeFetchDeadlineReferenceArchive(archive, identities) {
    fields(archive, ['schema', 'reports'], 'lossless deadline primary archive');
    equal(archive.schema, 'capsid-native-fetch-deadline-primary-v1', 'primary schema');
    equal(archive.reports.length, 2, 'actual Windows and Linux primaries');
    for (const [index, item] of archive.reports.entries()) {
        fields(item, ['platform', 'originalTextUTF8', 'originalReportSHA256'], 'original primary');
        equal(item.platform, index ? 'linux' : 'native', 'mandatory platform order');
        equal(hash(item.originalTextUTF8), item.originalReportSHA256, 'actual raw UTF8 bytes');
        const report = JSON.parse(item.originalTextUTF8);
        fields(report, ['schema', 'complete', 'fixtureSHA256', 'peerSHA256', 'sourceModelSHA256', 'captureSHA256', 'contractSHA256',
            'referenceLock', 'binarySHA256', 'client', 'cases', 'shutdown', 'stderr'], 'actual primary report');
        equal(report.schema, 'capsid-native-fetch-deadline-reference-v1', 'reference schema');
        equal(report.complete, true, 'actual primary completed');
        for (const key of Object.keys(identities)) equal(report[key], identities[key], 'current input ' + key);
        equal(report.referenceLock.source_commit, '8f311ae3e3cf3064983f56a6a1c8b7ee33d6b99f', 'pinned source');
        equal(report.referenceLock.release_tag, 'v1.20261010.1', 'pinned release');
        equal(report.referenceLock.compatibility_date, '2026-08-20', 'fixed date');
        equal(report.referenceLock.compatibility_flags, [], 'fixed profile');
        equal(report.binarySHA256, index ? '50f31156a9a4c7d549f1b4753dd61eb527e1b6b0cd416e5ba3d02d64dfdee5dd' :
            'c8799724c4221af61dde76ed071aa5996b25c9bd5d247da94c166718cabe612a', 'actual platform binary');
        equal(report.referenceLock.binary_sha256, report.binarySHA256, 'matching lock');
        equal(report.client, {node: 'v24.14.0', undici: '7.21.0'}, 'locked client');
        equal(report.stderr, '', 'actual primary diagnostics');
        assert.ok(report.shutdown.code === 0 && report.shutdown.signal === null ||
            report.shutdown.code === null && report.shutdown.signal === 'SIGTERM', 'observed reference stop');
        const plans = nativeFetchDeadlinePlans.filter(plan => plan.trigger === 'signal');
        equal(report.cases.map(row => row.plan), plans, 'all ten public observations; host policy excluded');
        for (const [rowIndex, row] of report.cases.entries()) {
            fields(row, ['plan', 'timings', 'state', 'sourceTerminalBeforeReferenceStop',
                'sourceBeforeReferenceStop', 'sourceAfterReferenceStop'], 'complete actual primary row');
            const plan = plans[rowIndex];
            equal(row.state, expectedNativeFetchDeadlineState(plan), 'real primary agrees with independent state');
            timing(row.timings);
            assert.ok(row.timings.stateMs >= 500 && row.timings.terminalMs < 12000, 'actual public timer duration');
            equal(row.sourceTerminalBeforeReferenceStop, true, 'reference peers ended before process stop');
            assertNativeFetchDeadlineSource(row.sourceBeforeReferenceStop, plan);
            equal(row.sourceAfterReferenceStop, row.sourceBeforeReferenceStop, 'reference teardown supplies no completion');
        }
    }
    return archive;
}
export function nativeFetchDeadlinePrimaryArchive() {
    const compressed = fs.readFileSync(new URL('../tests/fixtures/workerd-native-fetch-deadline-report.json.gz', import.meta.url));
    const raw = gunzipSync(compressed, {maxOutputLength: 8 * 1024 * 1024});
    const meta = JSON.parse(fs.readFileSync(new URL('../tests/fixtures/workerd-native-fetch-deadline-report.json', import.meta.url), 'utf8'));
    fields(meta, ['schema', 'compressedSHA256', 'rawSHA256', 'rawBytes', 'identities'], 'primary metadata');
    equal(meta.schema, 'capsid-native-fetch-deadline-primary-metadata-v1', 'metadata schema');
    equal(meta.compressedSHA256, hash(compressed), 'actual compressed bytes');
    equal(meta.rawSHA256, hash(raw), 'lossless archive bytes');
    equal(meta.rawBytes, raw.length, 'lossless raw length');
    equal(meta.identities, nativeFetchDeadlinePrimaryIdentities(), 'exact current reference inputs');
    return assertNativeFetchDeadlineReferenceArchive(JSON.parse(raw), meta.identities);
}
