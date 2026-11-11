import assert from 'node:assert/strict';
import {test} from 'node:test';
import {helperCreditKinds, helperCreditPlans, helperCreditBody, helperCreditExpectedFlows,
    assertHelperCreditCase, validateHelperCreditReport} from '../tools/helper-response-credit-contract.mjs';
import {responseLimitCases, expectedResponseLimitState} from '../tools/fetch-response-limit-contract.mjs';

const nativeExit = {clean: true, processObserved: true, exitStatus: 0, forced: false, errorEvents: 0};
function flow(model) {
    const body = model.body ?? Buffer.from(JSON.stringify(model.json)), parts = [];
    for (let at = 0; at < body.length; at += model.window) {
        const piece = body.subarray(at, at + model.window);
        parts.push({window: model.window, group: model.group, request: model.request, bytes: piece.length, granted: piece.length, bodyHex: piece.toString('hex')});
    }
    const row = {window: model.window, group: model.group, request: model.request, bytes: body.length, granted: body.length,
        parts: parts.map(part => part.bytes), bodyHex: body.toString('hex')};
    return {body, parts, row};
}
const partLine = part => 'RESPONSE_WINDOW_PART ' + JSON.stringify(part), rowLine = row => 'RESPONSE_WINDOW_ROW ' + JSON.stringify(row);
const eventLine = (type, request, flags, status, credit, payload) => `DEADLINE_EVENT type=${type} request=${request} flags=${flags} status=${status} credit=${credit} payload=${payload.toString('hex')}`;
function makeCase(kind, plan) {
    const flows = helperCreditExpectedFlows(kind, plan).map(flow), lines = [];
    if (kind === 'deadline') {
        const compatibility = 'sha256:' + 'a'.repeat(64);
        lines.push('DEADLINE_CONFIG timeout=5000 heap=67108864 limit=0 process=268435456 platform=posix ready=1 compatibility=' + compatibility + ' manifest=' + 'b'.repeat(64));
        lines.push(eventLine(1, 0, 1, 0, 0, Buffer.from(compatibility)));
        for (const [index, item] of flows.entries()) {
            const request = index + 1, prefix = Buffer.alloc(6), size = Buffer.alloc(4), type = 'text/plain;charset=UTF-8';
            prefix.writeUInt16LE(1, 2); prefix.writeUInt16LE(12, 4); size.writeUInt32LE(type.length);
            const fixed = item.body.length <= item.row.window;
            lines.push(eventLine(3, request, fixed ? 1 : 0, 200, fixed ? item.body.length : 0, Buffer.concat([prefix, Buffer.from('content-type'), size, Buffer.from(type)])));
            for (const part of item.parts) lines.push(eventLine(4, request, 0, 0, 0, Buffer.from(part.bodyHex, 'hex')), partLine(part));
            lines.push(eventLine(5, request, 0, 0, 0, Buffer.alloc(0)), rowLine(item.row));
            if (index === 0) lines.push('DEADLINE_ELAPSED milliseconds=1', 'LIMIT_PROBE ' + helperCreditBody(plan.bytes));
        }
        lines.push('LIMIT_EXIT ' + JSON.stringify(nativeExit));
    } else if (kind.startsWith('limits')) {
        const scenarios = responseLimitCases(kind === 'limits_encoded' ? 'gzip' : null);
        for (let i = 0; i < scenarios.length; ++i) {
            const model = scenarios[i], item = flows[i], encoded = model.encoding === 'gzip';
            const early = model.action === 'full' && model.limit !== 0 && model.known && !encoded && model.length > model.limit;
            // The native encoded source sends a 10-byte header, one 5-byte
            // stored-block header per fragment, and a 13-byte final block/trailer.
            const sent = model.action === 'full' ? (encoded ? 23 + model.length + (model.length ? (model.fragmented ? 15 : 5) : 0) : model.length) : encoded ? 25 : 5;
            const credit = model.action === 'full' ? (encoded && model.fragmented && model.length ? 25 : 0) : encoded ? (model.action === 'cancel' ? 17 : 25) : model.action === 'cancel' ? 2 : 5;
            lines.push(...item.parts.map(partLine), rowLine(item.row), 'LIMIT_ROW ' + JSON.stringify({limit: model.limit, fragmented: model.fragmented, channel: model.channel,
                sent, credit,
                cancels: model.action !== 'full' || early ? 1 : 0, live: false, observation: expectedResponseLimitState(model)}));
            if ((i + 1) % 160 === 0) {
                const exit = {limit: model.limit, shutdownResult: 0, flushResult: 0, eventResult: 0, channelClosed: true, errorEvents: 0,
                    processObserved: true, exitStatus: 0, waitError: 0, destroyForced: false};
                lines.push('LIMIT_WORKER_EXIT ' + JSON.stringify(exit), `LIMIT_WORKER_COMPLETE limit=${model.limit} rows=160 slots=1 naturalExit=true`);
            }
        }
    } else {
        for (const item of flows) lines.push(...item.parts.map(partLine), rowLine(item.row));
        lines.push(kind === 'discard' ? 'PASS: 12 deterministic reader-cancel crossings; complete, fragmented, empty and unfinished DATA; one-slot reuse; natural exit' : 'worker v4 negotiation: PASS');
    }
    return {plan, collectionComplete: true, stdout: lines.join('\n') + '\n', stderr: '', helperExit: {code: 0, signal: null}};
}
function makeReport(kind) {
    const expected = {kind, runId: 'synthetic-helper-credit-controls', inputs: {fixture: 'a'.repeat(64)}, binaries: {helper: 'b'.repeat(64), worker: 'c'.repeat(64)}};
    return {expected, report: {schema: 'capsid-helper-response-credit-v1', complete: true, ...expected, cases: helperCreditPlans(kind).map(plan => makeCase(kind, plan)), failures: []}};
}
let rejected = 0;
const rejects = fn => {assert.throws(fn); rejected++;};
test('all five helper protocols require complete actual pieces, exact bytes and original semantics', () => {
    assert.equal(helperCreditBody(40).toString(), 'abcdefghijklmnopqrstuvwxyz0123456789abcd');
    for (const kind of helperCreditKinds) {const {report, expected} = makeReport(kind); validateHelperCreditReport(report, expected);}
});
test('coherent body, credit, window, ordering, completion and diagnostic mutations are rejected', () => {
    for (const kind of helperCreditKinds) {
        const plan = helperCreditPlans(kind)[0];
        for (const change of ['credit', 'data', 'window', 'missing', 'duplicate', 'diagnostic', 'exit', 'incomplete']) {
            const row = makeCase(kind, plan), lines = row.stdout.trimEnd().split('\n');
            if (change === 'credit') {
                for (let i = 0; i < lines.length; ++i) if (lines[i].startsWith('RESPONSE_WINDOW_PART ') || lines[i].startsWith('RESPONSE_WINDOW_ROW ')) {
                    const isPart = lines[i].startsWith('RESPONSE_WINDOW_PART '), value = JSON.parse(lines[i].slice(isPart ? 21 : 20)); value.granted = 0;
                    lines[i] = (isPart ? partLine : rowLine)(value);
                }
            } else if (change === 'data') {
                const index = lines.findIndex(line => line.startsWith('RESPONSE_WINDOW_PART ')), part = JSON.parse(lines[index].slice(21));
                const bytes = Buffer.from(part.bodyHex, 'hex'); bytes[0] ^= 1; part.bodyHex = bytes.toString('hex'); lines[index] = partLine(part);
                const end = lines.findIndex(line => line.startsWith('RESPONSE_WINDOW_ROW ')), value = JSON.parse(lines[end].slice(20));
                const body = Buffer.from(value.bodyHex, 'hex'); body[0] ^= 1; value.bodyHex = body.toString('hex'); lines[end] = rowLine(value);
                if (kind === 'deadline') lines[index - 1] = eventLine(4, 1, 0, 0, 0, bytes);
            } else if (change === 'window') {
                for (let i = 0; i < lines.length; ++i) if (lines[i].startsWith('RESPONSE_WINDOW_PART ') || lines[i].startsWith('RESPONSE_WINDOW_ROW ')) {
                    const isPart = lines[i].startsWith('RESPONSE_WINDOW_PART '), value = JSON.parse(lines[i].slice(isPart ? 21 : 20)); value.window *= 2;
                    lines[i] = (isPart ? partLine : rowLine)(value);
                }
            } else if (change === 'missing') lines.splice(lines.findIndex(line => line.startsWith('RESPONSE_WINDOW_ROW ')), 1);
            else if (change === 'duplicate') {const end = lines.findIndex(line => line.startsWith('RESPONSE_WINDOW_ROW ')); lines.splice(end + 1, 0, ...lines.slice(0, end + 1).filter(line => line.startsWith('RESPONSE_WINDOW')));}
            else if (change === 'diagnostic') row.stderr = 'runtime diagnostic';
            else if (change === 'exit') row.helperExit.code = 1;
            else row.collectionComplete = false;
            row.stdout = lines.join('\n') + '\n'; rejects(() => assertHelperCreditCase(row, kind, plan));
        }
    }
});
test('fixed and streaming head metadata follows the initial window at every boundary', () => {
    for (const plan of helperCreditPlans('deadline')) {
        const valid = makeCase('deadline', plan); assertHelperCreditCase(valid, 'deadline', plan);
        const bodies = [helperCreditBody(plan.bytes), Buffer.from('healthy')];
        for (const request of [1, 2]) for (const change of ['flags', 'credit', 'both']) {
            const row = structuredClone(valid);
            row.stdout = row.stdout.split('\n').map(line => {
                const match = /^DEADLINE_EVENT type=3 request=(\d+) flags=(\d+) status=200 credit=(\d+) payload=([0-9a-f]*)$/.exec(line);
                if (!match || Number(match[1]) !== request) return line;
                const fixed = bodies[request - 1].length <= 32;
                return eventLine(3, request, change === 'credit' ? Number(match[2]) : fixed ? 0 : 1, 200,
                    change === 'flags' ? Number(match[3]) : fixed ? 0 : bodies[request - 1].length, Buffer.from(match[4], 'hex'));
            }).join('\n');
            rejects(() => assertHelperCreditCase(row, 'deadline', plan));
        }
    }
});
test('reports and already accepted mutable objects are always revalidated', () => {
    for (const kind of helperCreditKinds) {
        const {report, expected} = makeReport(kind); validateHelperCreditReport(report, expected);
        for (const change of [r => r.cases.pop(), r => r.cases.push(structuredClone(r.cases[0])), r => {r.complete = false;}, r => {r.runId += '-old';},
            r => {r.inputs.fixture = 'd'.repeat(64);}, r => {r.binaries.worker = 'd'.repeat(64);}, r => {r.failures.push({id: 'ignored'});}, r => {r.schema += '-other';}]) {
            const value = structuredClone(report); change(value); rejects(() => validateHelperCreditReport(value, expected));
        }
        report.cases[0].stdout += 'unexpected output\n'; rejects(() => validateHelperCreditReport(report, expected));
    }
    assert.equal(rejected, 109); console.log(JSON.stringify({rejectedHelperCreditMutations: rejected}));
});
