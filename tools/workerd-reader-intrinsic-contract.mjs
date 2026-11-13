import assert from 'node:assert/strict';
import {createHash} from 'node:crypto';
import {referenceBackendModes} from './workerd-platform.mjs';
import {exactFields} from './workerd-native-fetch-contract.mjs';

export const readerIntrinsicCases = Object.freeze(
    ['getReader', 'direct', 'prototype', 'subclass', 'reflect'].flatMap(route =>
        ['WeakMap.get', 'Promise.then', 'WeakMap.get+Promise.then', 'all'].flatMap(selection =>
            ['sync', 'deferred'].map(phase => [route, selection, phase].join('|')))));
export const readerIntrinsicInputPaths = Object.freeze({runner: 'tests/test_websocket_reader_intrinsics.mjs',
    fixture: 'tests/fixtures/workerd-reader-intrinsics.mjs', contract: 'tools/workerd-reader-intrinsic-contract.mjs',
    hostBackend: 'tests/helpers/ws_host_backend.mjs', peer: 'tests/helpers/ws_response_client.mjs',
    fields: 'tools/workerd-native-fetch-contract.mjs', platform: 'tools/workerd-platform.mjs',
    reference: 'tools/workerd-reference.mjs',
    listenerControl:'tests/helpers/workerd_listener_control.mjs',listenerContract:'tools/workerd-fetch-byob-contract.mjs',
});
const uuid = /^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/;

export function expectedReaderIntrinsicState(kind, challenge) {
    assert.ok(readerIntrinsicCases.includes(kind));
    const deferred = kind.endsWith('|deferred');
    return {kind, challenge,
        trace: ['listener', ...(deferred ? ['second-listener', 'mutated'] : ['mutated', 'second-listener']),
            'read', 'restored', 'cancelled', 'released'],
        traps: [], readError: null, unexpected: null, closedStable: true, inputBefore: 32, inputAfter: 0,
        read: {done: false, type: 'Uint8Array', offset: 8, length: 6,
            backing: [...Array(8).fill(165), 0, 102, 105, 114, 115, 116, ...Array(18).fill(165)]},
        closedDuringStable: true, closed: 'fulfilled', cancelReturnedUndefined: true, locked: false};
}
function assertUpgrade(value) {
    assert.equal(value.status, 101);
    assert.match(value.rawHeader, /^HTTP\/1\.1 101 /);
    assert.match(value.requestKey, /^[A-Za-z0-9+/]{22}==$/);
    const headers = new Map(value.rawHeader.split('\r\n').slice(1).map(line => {
        const at = line.indexOf(':'); assert.ok(at > 0);
        return [line.slice(0, at).toLowerCase(), line.slice(at + 1).trim()];
    }));
    assert.equal(headers.get('upgrade')?.toLowerCase(), 'websocket');
    assert.ok(headers.get('connection')?.toLowerCase().split(/\s*,\s*/).includes('upgrade'));
    assert.equal(headers.get('sec-websocket-accept'), createHash('sha1').update(value.requestKey +
        '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').digest('base64'));
}
export function assertReaderIntrinsicCase(value, mode) {
    exactFields(value, ['id', 'challenge', 'initialState', 'stateStatus', 'stateText', 'state', 'shutdown',
        'stderr', 'stdout', 'headers', 'wire', 'elapsedMs'], 'intrinsic case');
    assert.ok(readerIntrinsicCases.includes(value.id)); assert.match(value.challenge, uuid);
    exactFields(value.initialState, ['realm', 'state'], 'initial realm');
    assert.match(value.initialState.realm, uuid); assert.equal(value.initialState.state, null);
    const state = {realm: value.initialState.realm,
        state: expectedReaderIntrinsicState(value.id, value.challenge)};
    assert.equal(value.stateStatus, 200, 'healthy same worker after prototype replacements');
    assert.deepEqual(JSON.parse(value.stateText), state); assert.deepEqual(value.state, state);
    assertUpgrade(value.headers); assertUpgrade(value.wire);
    assert.equal(value.headers.requestKey, value.wire.requestKey);
    const frames = ['started:', 'done:'].map(prefix => ({opcode: 1,
        payload: Buffer.from(prefix + value.challenge).toString('base64')}));
    frames.push({opcode: 8, payload: Buffer.from([3, 232, 100, 111, 110, 101]).toString('base64')});
    assert.deepEqual(value.wire.frames, frames, 'both messages and the exact close survive dispatch');
    const bytes = Buffer.concat(frames.map(frame => {
        const payload = Buffer.from(frame.payload, 'base64');
        return Buffer.concat([Buffer.from([0x80 | frame.opcode, payload.length]), payload]);
    }));
    assert.equal(value.wire.rawTail, bytes.toString('base64'));
    assert.equal(value.wire.peerTerminated, true); assert.equal(value.wire.closeReplySent, true);
    assert.ok(['peer-ended', 'peer-reset'].includes(value.wire.completion));
    assert.equal(value.wire.transportError, value.wire.completion === 'peer-reset' ? 'ECONNRESET' : null);
    assert.ok(Number.isFinite(value.elapsedMs) && value.elapsedMs > 0 && value.elapsedMs < 4000);
    assert.equal(typeof value.stderr, 'string'); assert.equal(typeof value.stdout, 'string');
    assert.doesNotMatch(value.stderr + value.stdout,
        /WORKER POISONED|worker_crash|AddressSanitizer|LeakSanitizer|ThreadSanitizer|runtime error:|TOKEN LEAK|POISON EXIT|Uncaught/);
    exactFields(value.shutdown, ['code', 'signal', 'forced'], 'shutdown');
    assert.equal(value.shutdown.forced, false);
    if (mode === 'reference') assert.ok(value.shutdown.code === 0 && value.shutdown.signal === null ||
        value.shutdown.code === null && value.shutdown.signal === 'SIGTERM');
    else assert.deepEqual(value.shutdown, {code: 0, signal: null, forced: false});
    return {...state.state, challenge: '<challenge>'};
}
export function readerIntrinsicSourceNames(fixtureURL) {
    return {reference: 'main', 'single-worker': fixtureURL, 'static-pool': fixtureURL,
        managed: 'file://orders/v1/bundle.mjs'};
}
export function validateReaderIntrinsicReport(report, expected) {
    exactFields(report, ['schema', 'complete', 'runId', 'mode', 'family', 'requestTimeoutMs', 'sourceName',
        'referenceLock', 'client', 'inputs', 'binaries', 'cases'], 'intrinsic report');
    assert.equal(report.schema, 'capsid-reader-intrinsics-v1'); assert.equal(report.complete, true);
    assert.equal(report.runId, expected.runId); assert.match(report.runId, /^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$/);
    assert.ok(['reference', ...referenceBackendModes(expected.lock)].includes(report.mode));
    assert.equal(report.family, 'shared'); assert.equal(report.requestTimeoutMs, 10000);
    assert.equal(report.sourceName, expected.sourceNames[report.mode]);
    assert.deepEqual(report.referenceLock, expected.lock);
    assert.deepEqual(report.client, {node: expected.lock.client.node_version, undici: expected.lock.client.undici_version});
    assert.deepEqual(report.inputs, expected.inputs);
    assert.deepEqual(Object.keys(report.inputs).sort(), Object.keys(readerIntrinsicInputPaths).sort());
    for (const hash of Object.values(report.inputs)) assert.match(hash, /^[a-f0-9]{64}$/);
    assert.deepEqual(report.binaries, {host: report.mode === 'reference' ? expected.lock.binary_sha256 : expected.host,
        worker: report.mode === 'reference' ? null : expected.worker});
    assert.deepEqual(report.cases.map(value => value.id), readerIntrinsicCases);
    for (const values of [report.cases.map(value => value.challenge), report.cases.map(value => value.initialState.realm)])
        assert.equal(new Set(values).size, readerIntrinsicCases.length, 'every case uses a fresh worker and challenge');
    return report.cases.map(value => assertReaderIntrinsicCase(value, report.mode));
}
export function compareReaderIntrinsicReports(reference, candidates, expected) {
    assert.equal(reference.mode, 'reference');
    assert.deepEqual(candidates.map(report => report.mode).sort(), referenceBackendModes(expected.lock).sort());
    const baseline = validateReaderIntrinsicReport(reference, expected);
    for (const report of candidates) assert.deepEqual(validateReaderIntrinsicReport(report, expected), baseline);
    return {complete: true, casesPerBackend: readerIntrinsicCases.length, backends: candidates.length};
}
