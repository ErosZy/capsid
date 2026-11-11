import assert from 'node:assert/strict';
import {test} from 'node:test';
import {createHash} from 'node:crypto';
import {nativeFetchDeadlinePlans, nativeFetchDeadlineShards, nativeFetchDeadlineCasesForShard,
    expectedNativeFetchDeadlineState} from '../tools/native-fetch-deadline-input.mjs';
import {nativeFetchDeadlineExit, expectedNativeFetchDeadlineSource, assertNativeFetchDeadlineCase,
    validateNativeFetchDeadlineReport, nativeFetchDeadlinePrimaryArchive,
    nativeFetchDeadlinePrimaryIdentities, assertNativeFetchDeadlineReferenceArchive} from '../tools/native-fetch-deadline-contract.mjs';

const clone = structuredClone, hash = value => createHash('sha256').update(value).digest('hex');
let rejected = 0;
function rejects(label, action) { assert.throws(action, undefined, label); rejected++; }
const compatibility = 'sha256:' + 'a'.repeat(64), manifest = 'b'.repeat(64);
function event(type, request, payload = Buffer.alloc(0), flags = 0, status = 0, credit = 0) {
    return `DEADLINE_EVENT type=${type} request=${request} flags=${flags} status=${status} credit=${credit} payload=${payload.toString('hex')}`;
}
function audit(port) {
    const prefix = Buffer.alloc(28);
    prefix.writeUInt32LE(1); prefix.writeUInt32LE(3, 4); prefix.writeUInt32LE(1, 8);
    prefix.writeBigUInt64LE(1234n, 12); prefix.writeUInt32LE(1, 20);
    return Buffer.concat([prefix, ...['', '', 'net', 'host', `localhost:${port}`, manifest].flatMap(value => {
        const size = Buffer.alloc(2); size.writeUInt16LE(Buffer.byteLength(value));
        return [size, Buffer.from(value)];
    })]);
}
function response(request, value, json = false) {
    const body = Buffer.from(value), contentType = json ? 'application/json' : 'text/plain;charset=UTF-8';
    const prefix = Buffer.from(json ? '000001000c00636f6e74656e742d7479706510000000' :
        '000001000c00636f6e74656e742d7479706518000000', 'hex');
    return [event(3, request, Buffer.concat([prefix, Buffer.from(contentType)]), 1, 200, body.length),
        event(4, request, body), event(5, request)];
}
function output(row, asan = false) {
    const plan = row.plan, host = plan.trigger === 'host', port = row.sourceBeforeFinish.port;
    const lines = [`DEADLINE_CONFIG timeout=${host ? 750 : 5000} heap=67108864 limit=0 process=${asan ? 0 : 268435456} platform=windows ready=${asan ? 0 : 1} compatibility=${compatibility} manifest=${manifest}`,
        event(1, 0, Buffer.from(compatibility), asan ? 0 : 1), event(10, 1, audit(port))];
    if (plan.stage !== 'headless') lines.push(event(10, 1, audit(port)));
    if (host) {
        if (plan.stage !== 'headless') lines.push(event(6, 1, Buffer.concat([
            Buffer.from('\x03\x00logNATIVE_DEADLINE_READY '), Buffer.from(JSON.stringify(expectedNativeFetchDeadlineState(plan, true)))])));
        lines.push(event(9, 1, Buffer.from('request deadline exceeded'), 1),
            'DEADLINE_TIMEOUT request=1 flags=1 status=0 payload=request deadline exceeded');
    } else lines.push(...response(1, JSON.stringify(row.state), true));
    lines.push(`DEADLINE_ELAPSED milliseconds=${host ? 750 : 500}`, 'LIMIT_PROBE ' + JSON.stringify(row.state),
        ...response(2, 'healthy'), 'LIMIT_EXIT ' + JSON.stringify(row.workerExit));
    row.stdout = lines.join('\n') + '\n';
    return row;
}
function makeCase(plan) {
    const source = expectedNativeFetchDeadlineSource(plan, 12345);
    return output({plan, collectionComplete: true, timings: {stateMs: 800, terminalMs: 850, completeMs: 900},
        state: plan.trigger === 'host' ? 'HOST_TIMEOUT' : expectedNativeFetchDeadlineState(plan),
        sourceTerminalBeforeFinish: true, sourceBeforeFinish: source, sourceAfterFinish: clone(source),
        shutdown: {code: 0, signal: null}, stderr: '', workerExit: clone(nativeFetchDeadlineExit)});
}
function makeReport(shard) {
    const expected = {shard, runId: 'synthetic-deadline-controls', inputs: {fixture: 'c'.repeat(64)},
        binaries: {helper: 'd'.repeat(64), worker: 'e'.repeat(64)}};
    return {expected, report: {schema: 'capsid-native-fetch-deadline-v1', complete: true, ...expected,
        cases: nativeFetchDeadlineCasesForShard(shard).map(makeCase)}};
}
function replaceLog(row, mutate) {
    row.stdout = row.stdout.replace(/(DEADLINE_EVENT type=6 request=1 flags=0 status=0 credit=0 payload=)([0-9a-f]+)/,
        (_, prefix, hex) => {
            const data = Buffer.from(hex, 'hex'), start = Buffer.from('\x03\x00logNATIVE_DEADLINE_READY ');
            const state = JSON.parse(data.subarray(start.length).toString());
            mutate(state);
            return prefix + Buffer.concat([start, Buffer.from(JSON.stringify(state))]).toString('hex');
        });
}

test('all30 independent cases preserve separate public and host policies, including default ASan limits', () => {
    assert.equal(nativeFetchDeadlinePlans.length, 30);
    assert.equal(new Set(nativeFetchDeadlinePlans.map(plan => plan.id)).size, 30);
    for (const plan of nativeFetchDeadlinePlans) {
        assertNativeFetchDeadlineCase(makeCase(plan), plan);
        assertNativeFetchDeadlineCase(output(makeCase(plan), true), plan);
    }
});

test('all public cases reject coherent changed types, identity, trace, detachment, retry and metadata', () => {
    for (const plan of nativeFetchDeadlinePlans.filter(plan => plan.trigger === 'signal')) {
        const mutations = [state => state.reason.sameReason = false, state => state.reason.nativePrototype = false,
            state => state.reason.message = 'other timeout', state => state.aborted = false,
            state => state.fetchIssued = false, state => state.trace.push('extra.fulfilled')];
        if (plan.stage === 'headless') mutations.push(state => state.failure.sameReason = true,
            state => state.failure.name = 'AbortError', state => state.before = {});
        else mutations.push(state => state.before.status = 200, state => state.preControl.firstPending = false,
            state => state.preControl.closedPending = false, state => state.control.text = 'other',
            state => state.first.returnedPromise = false, state => state.first.inputAfterIssue = 24,
            state => state.first.error.sameReason = true, state => state.first.error.nativePrototype = false,
            state => state.retry.error.name = 'AbortError', state => state.retry.inputAfter = 24,
            state => state.after.locked = true, state => state.after.bodyUsed = false,
            state => state.after.headers.pop(), state => state.released = false,
            state => state.trace.reverse());
        for (const mutate of mutations) {
            const row = makeCase(plan); mutate(row.state); output(row);
            rejects(plan.id, () => assertNativeFetchDeadlineCase(row, plan));
        }
    }
});

test('host deadline requires actual readiness, pending ownership and complete C ABI timeout', () => {
    for (const plan of nativeFetchDeadlinePlans.filter(plan => plan.trigger === 'host')) {
        const changes = [row => row.stdout = row.stdout.replace('DEADLINE_EVENT type=9 request=1 flags=1', 'DEADLINE_EVENT type=9 request=2 flags=1'),
            row => row.stdout = row.stdout.replace('DEADLINE_EVENT type=9 request=1 flags=1', 'DEADLINE_EVENT type=9 request=1 flags=0'),
            row => row.stdout = row.stdout.replace(Buffer.from('request deadline exceeded').toString('hex'), Buffer.from('other deadline').toString('hex')),
            row => row.stdout = row.stdout.replace('DEADLINE_TIMEOUT request=1', 'DEADLINE_TIMEOUT request=2'),
            row => { row.state = {success: true}; output(row); }];
        if (plan.stage !== 'headless') for (const mutate of [state => state.control.status = 500,
            state => state.preControl.response.locked = !state.preControl.response.locked,
            state => state.trace.push('signal.abort'), state => state.fetchIssued = false])
            changes.push(row => replaceLog(row, mutate));
        if (['default', 'byob', 'least'].includes(plan.stage)) for (const mutate of [
            state => state.preControl.firstPending = false, state => state.preControl.closedPending = false,
            state => state.closedStable = false]) changes.push(row => replaceLog(row, mutate));
        for (const mutate of changes) {
            const row = makeCase(plan); mutate(row);
            rejects(plan.id, () => assertNativeFetchDeadlineCase(row, plan));
        }
    }
});

test('all30 cases require actual full wire, exact event bytes and timing, pre-cleanup closure and natural exit', () => {
    for (const plan of nativeFetchDeadlinePlans) {
        for (const mutate of [source => source.records[0].fixtureDestroyed = true,
            source => source.records[0].fixtureEnded = true,
            source => { source.records[0].peerEnded = false; source.records[0].peerReset = false; source.records[0].events.pop(); },
            source => source.records[0].rawRequest = 'AA==', source => source.records[0].sent.push('AA=='),
            source => source.records[0].events.reverse(), source => source.failure = 'source failed',
            source => source.controlBeforeBody = !source.controlBeforeBody, source => source.records.pop()]) {
            const row = makeCase(plan); mutate(row.sourceBeforeFinish); row.sourceAfterFinish = clone(row.sourceBeforeFinish);
            rejects(plan.id, () => assertNativeFetchDeadlineCase(row, plan));
        }
        for (const mutate of [row => row.sourceAfterFinish.records[0].fixtureDestroyed = true,
            row => row.sourceTerminalBeforeFinish = false, row => row.shutdown.code = 1,
            row => { row.workerExit.forced = true; output(row); }, row => row.stderr = 'runtime error:',
            row => row.timings.stateMs = -1, row => row.timings.completeMs = 700,
            row => row.stdout = row.stdout.replace(/DEADLINE_ELAPSED milliseconds=\d+/, 'DEADLINE_ELAPSED milliseconds=1'),
            row => row.stdout = row.stdout.replace('heap=67108864', 'heap=134217728'),
            row => row.stdout = row.stdout.replace('limit=0', 'limit=1024'),
            row => row.stdout = row.stdout.replace('DEADLINE_EVENT type=3 request=2', 'DEADLINE_EVENT type=3 request=1'),
            row => row.stdout = row.stdout.replace('DEADLINE_EVENT type=4 request=2', 'DEADLINE_EVENT type=4 request=2 flags=9 extra='),
            row => row.stdout = row.stdout.replace('payload=6865616c746879', 'payload=6865616c74687900'),
            row => row.stdout = row.stdout.replace('payload=6865616c746879', 'payload=6865616c74687'),
            row => row.stdout += 'LIMIT_EXIT ' + JSON.stringify(nativeFetchDeadlineExit) + '\n',
            row => row.stdout = row.stdout.replace('DEADLINE_EVENT type=10 request=1 flags=0', 'DEADLINE_EVENT type=10 request=2 flags=0')]) {
            const row = makeCase(plan); mutate(row);
            rejects(plan.id, () => assertNativeFetchDeadlineCase(row, plan));
        }
    }
});

test('all four shards reject missing/reordered cases and changed inputs, binaries and run identities', () => {
    for (const shard of nativeFetchDeadlineShards) {
        const {report, expected} = makeReport(shard);
        validateNativeFetchDeadlineReport(report, expected);
        for (const mutate of [value => value.cases.shift(), value => value.cases.push(clone(value.cases[0])),
            value => value.cases.reverse(), value => value.runId += '-other', value => value.inputs.fixture = 'f'.repeat(64),
            value => value.binaries.worker = 'f'.repeat(64), value => value.complete = false,
            value => value.schema += '-other', value => value.shard = 'other', value => value.cases[0].collectionComplete = false,
            value => delete value.cases[0].timings, value => value.cases[0].extra = true]) {
            const changed = clone(report); mutate(changed);
            rejects(shard, () => validateNativeFetchDeadlineReport(changed, expected));
        }
    }
});

test('accepted mutable observations are revalidated instead of cached', () => {
    const {report, expected} = makeReport('raw_signal');
    validateNativeFetchDeadlineReport(report, expected);
    report.cases[0].state.reason.sameReason = false; output(report.cases[0]);
    rejects('changed previously accepted report', () => validateNativeFetchDeadlineReport(report, expected));
    const plan = nativeFetchDeadlinePlans.find(plan => plan.id === 'gzip-byob-host-request'), row = makeCase(plan);
    assertNativeFetchDeadlineCase(row, plan);
    replaceLog(row, state => state.preControl.firstPending = false);
    rejects('changed previously accepted actual case', () => assertNativeFetchDeadlineCase(row, plan));
});

test('integer-clock boundary tolerance is one millisecond and rejects earlier observations', () => {
    for (const plan of nativeFetchDeadlinePlans) {
        const row = makeCase(plan), boundary = plan.trigger === 'host' ? 749 : 499;
        row.stdout = row.stdout.replace(/DEADLINE_ELAPSED milliseconds=\d+/, `DEADLINE_ELAPSED milliseconds=${boundary}`);
        assertNativeFetchDeadlineCase(row, plan);
        row.stdout = row.stdout.replace(`milliseconds=${boundary}`, `milliseconds=${boundary - 1}`);
        rejects('earlier than integer-clock boundary ' + plan.id, () => assertNativeFetchDeadlineCase(row, plan));
    }
});

test('both actual public primaries require complete semantics and provenance despite coherent rewritten hashes', () => {
    const archive = nativeFetchDeadlinePrimaryArchive(), identities = nativeFetchDeadlinePrimaryIdentities();
    assertNativeFetchDeadlineReferenceArchive(archive, identities);
    for (const mutate of [report => report.cases[0].state.reason.sameReason = false,
        report => report.cases[0].state.failure.nativePrototype = false,
        report => report.cases[1].state.first.inputAfterIssue = 24,
        report => report.cases[2].state.trace.reverse(), report => report.cases.pop(),
        report => report.cases[0].sourceBeforeReferenceStop.records[0].fixtureDestroyed = true,
        report => report.cases[0].sourceTerminalBeforeReferenceStop = false,
        report => report.referenceLock.compatibility_flags.push('changed'), report => report.binarySHA256 = 'f'.repeat(64),
        report => report.referenceLock.compatibility_date = '2026-08-21', report => report.peerSHA256 = 'f'.repeat(64),
        report => report.client.node = 'other', report => report.stderr = 'primary warning']) {
        const changed = clone(archive);
        for (const item of changed.reports) {
            const report = JSON.parse(item.originalTextUTF8); mutate(report);
            item.originalTextUTF8 = JSON.stringify(report); item.originalReportSHA256 = hash(item.originalTextUTF8);
        }
        rejects('coherent actual primary damage', () => assertNativeFetchDeadlineReferenceArchive(changed, identities));
    }
    for (const mutate of [value => value.reports.pop(), value => value.reports.reverse(),
        value => value.reports[0].originalReportSHA256 = 'f'.repeat(64)]) {
        const changed = clone(archive); mutate(changed);
        rejects('actual primary provenance', () => assertNativeFetchDeadlineReferenceArchive(changed, identities));
    }
});

test('controls count model mutations separately from actual runtime executions', () => {
    assert.equal(rejected, 1232);
    console.log('NATIVE_DEADLINE_CONTROLS ' + JSON.stringify({modelCases: 30, rejectedMutations: rejected,
        actualProductProgramsExecuted: 0}));
});
