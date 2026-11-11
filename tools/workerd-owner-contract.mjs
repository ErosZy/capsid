import assert from 'node:assert/strict';
import {createHash} from 'node:crypto';
import {referenceBackendModes} from './workerd-platform.mjs';

export const ownerInputPaths = Object.freeze({runner: 'tests/test_websocket_owner_lifecycle.mjs',
    fixture: 'tests/fixtures/workerd-owner-lifecycle.mjs', contract: 'tools/workerd-owner-contract.mjs',
    hostBackend: 'tests/helpers/ws_host_backend.mjs', peer: 'tests/helpers/ws_response_client.mjs'});
// Independent inventory: the HTTP detached-task policy is tested separately by
// worker_terminal_continuation and must not be presented as a shared result.
export const ownerKinds = Object.freeze(['live-one-shot', 'live-interval', 'close-with-ack',
    'close-delayed-ack', 'error-timer-before', 'error-listener-timer', 'error-final-close-timer',
    'http-await', 'live-returned-then', 'live-detached-then', 'close-returned-await',
    'close-detached-await', 'close-external-returned', 'close-external-detached', 'cross-clear', 'same-clear',
    'half-open-close-await']);

function keys(value, names, label) {
    assert.ok(value && typeof value === 'object', label + ' object');
    assert.deepEqual(Object.keys(value).sort(), [...names].sort(), label + ' complete fields');
}
function stateFor(kind) {
    const timerFired = kind === 'live-interval' ? 2 : ['live-one-shot', 'close-delayed-ack',
        'http-await', 'live-returned-then', 'live-detached-then', 'cross-clear', 'half-open-close-await'].includes(kind) ? 1 : 0;
    const error = kind.startsWith('error-');
    const trace = error ? ['microtask', 'nested-microtask',
        kind === 'error-final-close-timer' ? 'late-TypeError' : 'late-accepted'] :
        kind.startsWith('live-') && kind.endsWith('-then') ? ['handler-return', 'microtask', 'timer'] :
        kind === 'cross-clear' ? ['http-clear', 'timer'] : kind === 'same-clear' ? ['same-clear'] :
        Array(timerFired).fill('timer');
    const events = kind === 'http-await' ? [] : [{type: 'message', readyState: 1}];
    if (error) events.push({type: 'error', readyState: 3,
        message: 'Uncaught TypeError: owner-lifecycle/' + kind, errorName: 'TypeError',
        errorMessage: 'owner-lifecycle/' + kind, trusted: true});
    else if (kind !== 'http-await') {
        if (kind === 'same-clear') events.push({type: 'message', readyState: 1});
        events.push({type: 'close', readyState: kind === 'half-open-close-await' ? 2 : 3});
    }
    return {state: {kind, timerFired, trace, events}};
}
function wireFor(kind) {
    const texts = ['queued'];
    if (['live-one-shot', 'live-returned-then', 'live-detached-then', 'cross-clear'].includes(kind)) texts.push('timer');
    if (kind === 'live-interval') texts.push('timer-1', 'timer-2');
    if (kind === 'same-clear') texts.push('cleared');
    if (kind === 'half-open-close-await') texts.push('after-peer-close');
    if (kind === 'error-timer-before' || kind === 'error-listener-timer') texts.push('late');
    const frames = texts.map(text => ({opcode: 1, payload: Buffer.from(text).toString('base64')}));
    if (!(kind === 'error-timer-before' || kind === 'error-listener-timer'))
        frames.push({opcode: 8, payload: 'A+hkb25l'});
    const bytes = Buffer.concat(frames.map(({opcode, payload}) => {
        const data = Buffer.from(payload, 'base64');
        return Buffer.concat([Buffer.from([0x80 | opcode, data.length]), data]);
    }));
    return {frames, rawTail: bytes.toString('base64')};
}
function upgrade(wire, label) {
    assert.equal(wire.status, 101, label + ' Upgrade status');
    assert.match(wire.rawHeader, /^HTTP\/1\.1 101 /, label + ' raw Upgrade status');
    assert.match(wire.requestKey, /^[A-Za-z0-9+/]{22}==$/, label + ' actual request challenge');
    const headers = new Map(wire.rawHeader.split('\r\n').slice(1).map(line => {
        const at = line.indexOf(':'); assert.ok(at > 0, label + ' valid response header');
        return [line.slice(0, at).toLowerCase(), line.slice(at + 1).trim()];
    }));
    assert.equal(headers.get('upgrade')?.toLowerCase(), 'websocket', label + ' Upgrade header');
    assert.ok(headers.get('connection')?.toLowerCase().split(/\s*,\s*/).includes('upgrade'), label + ' Connection header');
    assert.equal(headers.get('sec-websocket-accept'), createHash('sha1')
        .update(wire.requestKey + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').digest('base64'), label + ' Upgrade challenge');
}

export function assertOwnerCase(kind, value, mode) {
    assert.ok(ownerKinds.includes(kind), 'known connection owner case');
    const label = kind + ': connection owner contract';
    const extra = kind === 'http-await' ? ['initialHTTP'] : ['headers', 'wire'];
    if (kind === 'close-delayed-ack') extra.push('beforeManualAck', 'beforeAckHTTP', 'manualCloseSent');
    if (kind === 'cross-clear') extra.push('clearResponse');
    if (kind.startsWith('close-external-')) extra.push('resumeResponse');
    if (kind === 'half-open-close-await') extra.push('manualCloseSent');
    keys(value, ['id', 'stateStatus', 'stateText', 'state', 'shutdown', 'stderr', ...extra], label);
    assert.equal(value.id, kind, label + ' actual case identity');
    assert.equal(value.stateStatus, 200, label + ' same realm remains healthy');
    assert.deepEqual(JSON.parse(value.stateText), stateFor(kind), label + ' actual HTTP state');
    assert.deepEqual(value.state, stateFor(kind), label + ' complete lifecycle trace');
    if (kind === 'http-await') {
        assert.equal(value.initialHTTP.status, 200, label + ' awaited HTTP status');
        assert.deepEqual(JSON.parse(value.initialHTTP.body), {accepted: true, ...stateFor(kind)}, label + ' awaited HTTP body');
    } else {
        upgrade(value.headers, label); upgrade(value.wire, label);
        assert.equal(value.headers.requestKey, value.wire.requestKey, label + ' same physical socket');
        const expected = wireFor(kind);
        assert.deepEqual(value.wire.frames, expected.frames, label + ' exact complete frames');
        assert.equal(value.wire.rawTail, expected.rawTail, label + ' exact raw bytes');
        assert.equal(value.wire.peerTerminated, true, label + ' actual peer terminal');
        assert.ok(['peer-ended', 'peer-reset'].includes(value.wire.completion), label + ' no deadline or local disposal');
        assert.equal(value.wire.transportError, value.wire.completion === 'peer-reset' ? 'ECONNRESET' : null,
            label + ' actual transport result');
        assert.equal(value.wire.closeReplySent, kind !== 'close-delayed-ack' && kind !== 'half-open-close-await' &&
            !(kind === 'error-timer-before' || kind === 'error-listener-timer'), label + ' actual peer Close reply');
    }
    if (kind === 'half-open-close-await') assert.equal(value.manualCloseSent, true, label + ' peer initiated half Close');
    if (kind === 'close-delayed-ack') {
        assert.equal(value.beforeManualAck.peerTerminated, false, label + ' Close does not terminate before ACK');
        assert.equal(value.beforeManualAck.completion, null, label + ' pending real transport');
        assert.equal(value.beforeManualAck.transportError, null, label + ' pending transport has no error');
        assert.equal(value.beforeManualAck.closeReplySent, false, label + ' automatic ACK disabled');
        assert.deepEqual(value.beforeManualAck.frames, wireFor(kind).frames, label + ' pre-ACK complete frames');
        assert.equal(value.beforeAckHTTP.status, 200, label + ' pre-ACK healthy HTTP');
        const middle = stateFor(kind); middle.state.events.pop();
        assert.deepEqual(JSON.parse(value.beforeAckHTTP.body), middle, label + ' timer remains live while awaiting ACK');
        assert.equal(value.manualCloseSent, true, label + ' actual manual ACK');
    }
    if (kind === 'cross-clear') assert.deepEqual(value.clearResponse, {status: 200, text: '{"before":0}'}, label + ' foreign clear attempted before firing');
    if (kind.startsWith('close-external-')) assert.deepEqual(value.resumeResponse,
        {status: 200, text: '{"resumed":true}'}, label + ' foreign request really resolved the retained Promise');
    assert.equal(typeof value.stderr, 'string', label + ' raw stderr');
    assert.doesNotMatch(value.stderr, /ERROR: (?:AddressSanitizer|LeakSanitizer)|runtime error:|TOKEN LEAK|POISON EXIT/,
        label + ' no runtime diagnostics');
    keys(value.shutdown, ['code', 'signal', 'forced'], label + ' shutdown');
    assert.equal(value.shutdown.forced, false, label + ' cooperative shutdown');
    if (mode === 'reference') assert.ok(value.shutdown.code === 0 && value.shutdown.signal === null ||
        value.shutdown.code === null && value.shutdown.signal === 'SIGTERM', label + ' actual reference shutdown');
    else assert.deepEqual(value.shutdown, {code: 0, signal: null, forced: false}, label + ' healthy product shutdown');
    return {state: value.state, wire: kind === 'http-await' ? null : wireFor(kind)};
}

export function validateOwnerReport(report, expected) {
    assert.equal(report.schema, 'capsid-websocket-owner-lifecycle-v1');
    assert.equal(report.complete, true, 'complete connection owner run');
    assert.equal(report.runId, expected.runId, 'current connection owner run');
    assert.ok(['reference', ...referenceBackendModes(expected.lock)].includes(report.mode), 'supported connection owner mode');
    assert.deepEqual(report.referenceLock, expected.lock, 'locked connection owner profile');
    assert.deepEqual(report.client, {node: expected.lock.client.node_version, undici: expected.lock.client.undici_version});
    assert.deepEqual(report.inputs, expected.inputs, 'connection owner input provenance');
    keys(report.inputs, Object.keys(ownerInputPaths), 'connection owner inputs');
    for (const hash of Object.values(report.inputs)) assert.match(hash, /^[0-9a-f]{64}$/);
    assert.deepEqual(report.binaries, {host: report.mode === 'reference' ? expected.lock.binary_sha256 : expected.host,
        worker: report.mode === 'reference' ? null : expected.worker}, 'connection owner binary provenance');
    assert.deepEqual(report.cases.map(value => value.id), ownerKinds, 'complete ordered connection owner inventory');
    return report.cases.map(value => assertOwnerCase(value.id, value, report.mode));
}

export function compareOwnerReports(reference, candidates, expected) {
    assert.equal(reference.mode, 'reference');
    assert.deepEqual(candidates.map(value => value.mode).sort(), referenceBackendModes(expected.lock).sort());
    const baseline = validateOwnerReport(reference, expected);
    for (const report of candidates) assert.deepEqual(validateOwnerReport(report, expected), baseline,
        'connection owner lifecycle matches the pinned workerd profile');
    return {complete: true, casesPerBackend: ownerKinds.length, backends: 1 + candidates.length};
}
