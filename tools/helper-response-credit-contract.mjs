import assert from 'node:assert/strict';
import {responseLimitCases, expectedResponseLimitState, validateResponseLimitTranscript} from './fetch-response-limit-contract.mjs';

export const helperCreditKinds = Object.freeze(['deadline', 'limits', 'limits_encoded', 'discard', 'negotiation']);
export const helperCreditInputPaths = Object.freeze({
    driver: 'tests/test_helper_response_credit.mjs', contract: 'tools/helper-response-credit-contract.mjs',
    controls: 'tests/test_helper_response_credit_contract.mjs', fixture: 'tests/fixtures/helper-response-credit.mjs',
    consumer: 'tests/response_credit_probe.h', deadline: 'tests/test_native_fetch_deadline.cc',
    limits: 'tests/test_fetch_response_limits.cc', discard: 'tests/test_worker_channel_discard.cc',
    negotiation: 'tests/test_worker_v4_negotiation.cc', limitsFixture: 'tests/fixtures/fetch-response-limits.mjs',
    limitsContract: 'tools/fetch-response-limit-contract.mjs',
});
export const helperCreditPlans = kind => {
    assert.ok(helperCreditKinds.includes(kind));
    return kind === 'deadline' ? [31, 32, 33, 101].map(bytes => ({id: kind + '-' + bytes, bytes})) : [{id: kind}];
};
const fields = (value, names) => {
    assert.ok(value && typeof value === 'object' && !Array.isArray(value));
    assert.deepEqual(Object.keys(value).sort(), names.slice().sort());
};
export function helperCreditBody(bytes) {
    const result = Buffer.alloc(bytes);
    for (let i = 0; i < bytes; ++i) {const n = i % 36; result[i] = n < 26 ? 97 + n : 48 + n - 26;}
    return result;
}
export function helperCreditExpectedFlows(kind, plan) {
    if (kind === 'deadline') return [helperCreditBody(plan.bytes), Buffer.from('healthy')].map((body, i) => ({window: 32, group: 0, request: i + 1, body}));
    if (kind === 'discard') return Array.from({length: 24}, (_, i) => ({window: 16, group: 0, request: i + 1,
        body: Buffer.from(i % 2 ? 'healthy' : '{"done":false,"bytes":[7,8,9],"closed":true,"cancelled":true,"locked":false}')}));
    if (kind === 'negotiation') return [0, 31, 63, 123, 155, 251, 255].map(group => ({window: 3, group, request: 1, body: Buffer.from('v4-carriage')}));
    return responseLimitCases(kind === 'limits_encoded' ? 'gzip' : null).map(scenario => ({window: 256, group: scenario.limit,
        request: scenario.channel, json: expectedResponseLimitState(scenario)}));
}
function parseHex(text) {assert.match(text, /^(?:[0-9a-f]{2})*$/); return Buffer.from(text, 'hex');}
function parseFlows(lines) {
    const flows = [], other = []; let pending = [];
    for (const line of lines) {
        if (line.startsWith('RESPONSE_WINDOW_PART ')) {
            const part = JSON.parse(line.slice(21));
            fields(part, ['window', 'group', 'request', 'bytes', 'granted', 'bodyHex']);
            assert.ok(Number.isSafeInteger(part.bytes) && part.bytes > 0 && part.bytes <= part.window);
            assert.equal(part.granted, part.bytes, 'grant exactly consumed bytes');
            assert.equal(parseHex(part.bodyHex).length, part.bytes); pending.push(part);
        } else if (line.startsWith('RESPONSE_WINDOW_ROW ')) {
            const row = JSON.parse(line.slice(20));
            fields(row, ['window', 'group', 'request', 'bytes', 'granted', 'parts', 'bodyHex']);
            assert.ok(pending.length > 0); assert.equal(row.granted, row.bytes);
            assert.deepEqual(row.parts, pending.map(part => part.bytes));
            assert.equal(row.bytes, row.parts.reduce((sum, bytes) => sum + bytes, 0));
            assert.deepEqual(pending.map(part => [part.window, part.group, part.request]), pending.map(() => [row.window, row.group, row.request]));
            const body = parseHex(row.bodyHex); assert.equal(body.length, row.bytes);
            assert.deepEqual(Buffer.concat(pending.map(part => parseHex(part.bodyHex))), body);
            assert.ok(row.parts.length >= Math.ceil(body.length / row.window));
            flows.push({row, parts: pending, body}); pending = [];
        } else {
            assert.ok(!line.startsWith('RESPONSE_WINDOW'), 'no unknown probe marker'); other.push(line);
        }
    }
    assert.deepEqual(pending, [], 'every actual piece belongs to a completed response');
    return {flows, other};
}
const eventPattern = /^DEADLINE_EVENT type=(\d+) request=(\d+) flags=(\d+) status=(\d+) credit=(\d+) payload=([0-9a-f]*)$/;
const nativeExit = {clean: true, processObserved: true, exitStatus: 0, forced: false, errorEvents: 0};
function deadlineOutput(lines, other, flows, plan) {
    const config = /^DEADLINE_CONFIG timeout=5000 heap=67108864 limit=0 process=(0|268435456) platform=(windows|posix) ready=(0|1) compatibility=(sha256:[0-9a-f]{64}) manifest=([0-9a-f]{64})$/.exec(other.shift());
    assert.ok(config); assert.equal(Number(config[3]), config[2] === 'windows' && config[1] === '0' ? 0 : 1);
    const events = [], markers = [];
    for (const line of other) {
        const match = eventPattern.exec(line);
        if (match) events.push({type: Number(match[1]), request: Number(match[2]), flags: Number(match[3]), status: Number(match[4]), credit: Number(match[5]), payload: parseHex(match[6])});
        else markers.push(line);
    }
    assert.deepEqual(events.shift(), {type: 1, request: 0, flags: Number(config[3]), status: 0, credit: 0, payload: Buffer.from(config[4])});
    for (let i = 0; i < 2; ++i) {
        const flow = flows[i], request = i + 1, contentType = 'text/plain;charset=UTF-8';
        const prefix = Buffer.alloc(6), size = Buffer.alloc(4); prefix.writeUInt16LE(1, 2); prefix.writeUInt16LE(12, 4); size.writeUInt32LE(Buffer.byteLength(contentType));
        const head = Buffer.concat([prefix, Buffer.from('content-type'), size, Buffer.from(contentType)]);
        const fixed = flow.body.length <= flow.row.window;
        assert.deepEqual(events.shift(), {type: 3, request, flags: fixed ? 1 : 0, status: 200, credit: fixed ? flow.body.length : 0, payload: head});
        for (const part of flow.parts) assert.deepEqual(events.shift(), {type: 4, request, flags: 0, status: 0, credit: 0, payload: parseHex(part.bodyHex)});
        assert.deepEqual(events.shift(), {type: 5, request, flags: 0, status: 0, credit: 0, payload: Buffer.alloc(0)});
    }
    assert.deepEqual(events, []);
    assert.match(markers[0], /^DEADLINE_ELAPSED milliseconds=\d+$/);
    assert.ok(Number(markers[0].split('=')[1]) < 5000);
    assert.equal(markers[1], 'LIMIT_PROBE ' + helperCreditBody(plan.bytes));
    assert.equal(markers[2], 'LIMIT_EXIT ' + JSON.stringify(nativeExit)); assert.equal(markers.length, 3);
    for (let i = 0; i < lines.length; ++i) {
        const event = eventPattern.exec(lines[i]); if (!event) continue;
        if (event[1] === '4') assert.ok(lines[i + 1].startsWith('RESPONSE_WINDOW_PART '));
        if (event[1] === '5') assert.ok(lines[i + 1].startsWith('RESPONSE_WINDOW_ROW '));
    }
}
export function assertHelperCreditCase(row, kind, plan) {
    fields(row, ['plan', 'collectionComplete', 'stdout', 'stderr', 'helperExit']);
    assert.deepEqual(row.plan, plan); assert.equal(row.collectionComplete, true);
    assert.deepEqual(row.helperExit, {code: 0, signal: null}); assert.equal(row.stderr, '');
    const lines = row.stdout.trimEnd().split(/\r?\n/), {flows, other} = parseFlows(lines), expected = helperCreditExpectedFlows(kind, plan);
    assert.equal(flows.length, expected.length);
    for (const [i, flow] of flows.entries()) {
        const model = expected[i]; assert.deepEqual([flow.row.window, flow.row.group, flow.row.request], [model.window, model.group, model.request]);
        if (model.body) assert.deepEqual(flow.body, model.body);
        else {
            const state = JSON.parse(flow.body.toString('utf8')); assert.deepEqual(state, model.json);
            assert.equal(flow.body.toString('utf8'), JSON.stringify(state), 'complete canonical JSON bytes');
        }
    }
    if (kind === 'deadline') deadlineOutput(lines, other, flows, plan);
    else if (kind === 'discard') assert.deepEqual(other, ['PASS: 12 deterministic reader-cancel crossings; complete, fragmented, empty and unfinished DATA; one-slot reuse; natural exit']);
    else if (kind === 'negotiation') assert.deepEqual(other, ['worker v4 negotiation: PASS']);
    else validateResponseLimitTranscript(other.join('\n') + '\n', {encoding: kind === 'limits_encoded' ? 'gzip' : null});
    return {responses: flows.length, bytes: flows.reduce((sum, flow) => sum + flow.body.length, 0),
        pieces: flows.reduce((sum, flow) => sum + flow.parts.length, 0),
        spanningResponses: flows.filter(flow => flow.body.length > flow.row.window).length};
}
export function validateHelperCreditReport(report, expected) {
    fields(report, ['schema', 'complete', 'kind', 'runId', 'inputs', 'binaries', 'cases', 'failures']);
    assert.equal(report.schema, 'capsid-helper-response-credit-v1'); assert.equal(report.complete, true); assert.deepEqual(report.failures, []);
    for (const key of ['kind', 'runId', 'inputs', 'binaries']) assert.deepEqual(report[key], expected[key]);
    const plans = helperCreditPlans(expected.kind); assert.deepEqual(report.cases.map(row => row.plan), plans);
    return report.cases.map((row, i) => assertHelperCreditCase(row, expected.kind, plans[i]));
}
