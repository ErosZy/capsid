import assert from 'node:assert/strict';
import fs from 'node:fs';
import {createHash} from 'node:crypto';
import {isDeepStrictEqual} from 'node:util';
import {nativeFetchLimitPlans, nativeFetchLimitShards, nativeFetchLimitCasesForShard,
    nativeFetchLimitShape, nativeFetchLimitReply} from './native-fetch-limit-input.mjs';

function fields(value, names, label) {
    assert.ok(value && typeof value === 'object' && !Array.isArray(value), label);
    assert.deepEqual(Object.keys(value).sort(), [...names].sort(), label+' complete fields');
}
// Check every current value and exact type without constructing enormous
// pretty-printed array diffs for each deliberately corrupted control report.
function equal(actual, expected, label = 'complete typed state') {
    assert.ok(isDeepStrictEqual(actual, expected), label);
}
const archive = JSON.parse(fs.readFileSync(new URL('../tests/fixtures/workerd-native-fetch-limit-report.json', import.meta.url), 'utf8'));
fields(archive, ['schema', 'provenance', 'reports'], 'primary native limit reference archive');
assert.equal(archive.schema, 'capsid-native-fetch-limit-reference-v1');
assert.equal(archive.provenance.length, 2);
assert.equal(archive.reports.length, 2);
const fixtureSHA256 = createHash('sha256').update(fs.readFileSync(new URL('../tests/fixtures/native-fetch-response-limits.mjs', import.meta.url))).digest('hex');
const baseline = new Map();
for (const [index, report] of archive.reports.entries()) {
    const provenance = archive.provenance[index];
    assert.equal(provenance.platform, index === 0 ? 'native' : 'linux');
    assert.match(provenance.originalReportSHA256, /^[0-9a-f]{64}$/);
    assert.equal(report.complete, true);
    assert.equal(report.cases.length, 8);
    assert.equal(report.fixtureSHA256, fixtureSHA256);
    assert.equal(report.referenceLock.source_commit, '8f311ae3e3cf3064983f56a6a1c8b7ee33d6b99f');
    assert.equal(report.referenceLock.release_tag, 'v1.20261010.1');
    assert.equal(report.referenceLock.compatibility_date, '2026-08-20');
    assert.deepEqual(report.referenceLock.compatibility_flags, []);
    assert.equal(report.binarySHA256, index === 0 ? 'c8799724c4221af61dde76ed071aa5996b25c9bd5d247da94c166718cabe612a' : '50f31156a9a4c7d549f1b4753dd61eb527e1b6b0cd416e5ba3d02d64dfdee5dd');
    assert.deepEqual(report.client, {node: 'v24.14.0', undici: '7.21.0'});
    const digest = createHash('sha256').update(JSON.stringify(report.cases.map(row => row.state))).digest('hex');
    assert.equal(digest, provenance.fullStateSHA256);
    const shapes = new Set();
    for (const row of report.cases) {
        const key = nativeFetchLimitShape(row.plan);
        assert.ok(!shapes.has(key)); shapes.add(key);
        assert.equal(row.sourceEndedBeforeReferenceStop, true);
        assert.equal(row.source.peerEnded, true);
        assert.equal(row.source.peerReset, false);
        assert.equal(row.source.fixtureDestroyed, false);
        assert.equal(row.source.sent, nativeFetchLimitReply(row.plan).packet.toString('base64'));
        if (index === 0) baseline.set(key, structuredClone(row.state));
        else equal(row.state, baseline.get(key), 'both actual pinned reference states');
    }
    assert.equal(shapes.size, 8);
}
assert.equal(baseline.size, 8);

export function expectedNativeFetchLimitState(plan) {
    assert.deepEqual(plan, nativeFetchLimitPlans.find(row => row.id === plan.id), 'fixed independent native limit inventory');
    const state = structuredClone(baseline.get(nativeFetchLimitShape(plan)));
    assert.ok(state);
    if (!plan.limit || plan.bytes <= plan.limit) return state;
    const error = {name: 'TypeError', message: `fetch response body exceeded configured limit of ${plan.limit} bytes`};
    state.failure = error;
    state.closed = {error};
    state.reads = [];
    // The policy counts decoded bytes. Each accepted complete 4 KiB decoder
    // output remains observable; the overflowing output is never committed.
    if (plan.gzip && plan.bytes === 8197 && plan.limit === 4096) {
        const data = Array.from({length: 4096}, (_, index) => (index*29+17)%251);
        const capacity = plan.view ? plan.view+16 : 4096, offset = plan.view ? 8 : 0;
        const backing = Array(capacity).fill(plan.view ? 165 : 0);
        backing.splice(offset, data.length, ...data);
        state.reads.push({inputAfter: plan.view ? 0 : null, done: false, type: 'Uint8Array',
            offset, length: data.length, capacity, bytes: data, backing});
    }
    return state;
}

function assertByteModel(state, plan) {
    let cursor = 0;
    for (const row of state.reads) {
        if (row.done) continue; // The complete primary state also checks EOF backing.
        assert.equal(row.type, 'Uint8Array');
        assert.equal(row.offset, plan.view ? 8 : 0);
        assert.equal(row.capacity, plan.view ? plan.view+16 : 4096);
        assert.equal(row.inputAfter, plan.view ? 0 : null);
        const bytes = Array.from({length: row.length}, (_, index) => ((cursor+index)*29+17)%251);
        const backing = Array(row.capacity).fill(plan.view ? 165 : 0);
        backing.splice(row.offset, bytes.length, ...bytes);
        equal(row.bytes, bytes, 'independent decoded byte sequence');
        equal(row.backing, backing, 'all outside-view and unread bytes');
        cursor += row.length;
    }
    assert.equal(cursor, expectedNativeFetchLimitState(plan).reads.reduce((n, row) => n+(row.length ?? 0), 0));
}

export function assertNativeFetchLimitCase(row, plan) {
    fields(row, ['plan', 'source', 'port', 'encodedBytes', 'state', 'sourceEndedBeforeFinish',
        'sourceBeforeFinish', 'shutdown', 'stdout', 'stderr', 'workerExit'], 'actual native limit observation');
    assert.deepEqual(row.plan, plan);
    equal(row.state, expectedNativeFetchLimitState(plan), 'complete typed primary state plus independent Capsid quota policy');
    assertByteModel(row.state, plan);
    assert.ok(Number.isSafeInteger(row.port) && row.port > 0 && row.port < 65536);
    const reply = nativeFetchLimitReply(plan);
    assert.equal(row.encodedBytes, reply.payload.length);
    fields(row.source, ['request', 'rawRequest', 'peerEnded', 'peerReset', 'fixtureEnded', 'fixtureDestroyed', 'events', 'sent'], 'actual source');
    equal(row.sourceBeforeFinish, row.source, 'source ended before parent allowed worker cleanup');
    assert.equal(row.sourceEndedBeforeFinish, true);
    assert.equal(row.source.peerEnded, true);
    assert.equal(row.source.peerReset, false);
    assert.equal(row.source.fixtureDestroyed, false);
    assert.equal(row.source.fixtureEnded, true);
    assert.deepEqual(row.source.events, ['request', 'sent-complete', 'peer-ended']);
    assert.equal(row.source.sent, reply.packet.toString('base64'), 'actual encoded bytes and headers');
    const raw = Buffer.from(row.source.rawRequest, 'base64');
    assert.equal(raw.toString('base64'), row.source.rawRequest, 'canonical complete raw request bytes');
    assert.equal(raw.toString('ascii'), row.source.request+'\r\n\r\n');
    const headers = row.source.request.split('\r\n');
    assert.equal(headers.shift(), 'GET /hold HTTP/1.1');
    assert.deepEqual(headers.filter(line => line.slice(0, line.indexOf(':')).toLowerCase() === 'host')
        .map(line => line.slice(line.indexOf(':')+1).trim()), ['localhost:'+row.port]);
    assert.deepEqual(row.workerExit, {clean: true, processObserved: true, exitStatus: 0, forced: false, errorEvents: 0});
    assert.deepEqual(row.shutdown, {code: 0, signal: null});
    assert.equal(row.stderr, '');
    assert.equal(typeof row.stdout, 'string');
    const lines = row.stdout.trim().split(/\r?\n/);
    equal(lines.filter(line => line.startsWith('LIMIT_PROBE ')), ['LIMIT_PROBE '+JSON.stringify(row.state)]);
    equal(lines.filter(line => line.startsWith('LIMIT_EXIT ')), ['LIMIT_EXIT '+JSON.stringify(row.workerExit)]);
    assert.ok(lines.every(line => line.startsWith('LIMIT_PROBE ') || line.startsWith('LIMIT_EXIT ') ||
        /^LIMIT_AUX type=(?:6|10|11) request=\d+ status=\d+ payload=.*$/.test(line)), 'only actual known auxiliary ABI events');
    return row.state;
}

export function validateNativeFetchLimitReport(report, expected) {
    fields(report, ['schema', 'complete', 'shard', 'runId', 'inputs', 'binaries', 'cases'], 'native limit report');
    assert.equal(report.schema, 'capsid-native-fetch-limits-v1');
    assert.equal(report.complete, true);
    assert.ok(nativeFetchLimitShards.includes(report.shard));
    assert.equal(report.shard, expected.shard);
    assert.equal(report.runId, expected.runId);
    assert.deepEqual(report.inputs, expected.inputs);
    assert.deepEqual(report.binaries, expected.binaries);
    const plans = nativeFetchLimitCasesForShard(report.shard);
    assert.deepEqual(report.cases.map(row => row.plan.id), plans.map(plan => plan.id));
    return report.cases.map((row, index) => assertNativeFetchLimitCase(row, plans[index]));
}
