import assert from 'node:assert/strict';
import {createHash} from 'node:crypto';
import {referenceBackendModes} from './workerd-platform.mjs';
import {exactFields as fields} from './workerd-native-fetch-contract.mjs';
import {assertFetchTransitionCertificates} from './workerd-fetch-transition-contract.mjs';

import {readerLifecycleCases as nativeReaderLifecycleCases,
    expectedReaderLifecycleState as expectedNativeReaderLifecycleState} from './workerd-reader-lifecycle-state.mjs';
export {nativeReaderLifecycleCases, expectedNativeReaderLifecycleState};
export const nativeReaderLifecycleInputPaths = Object.freeze({
    runner: 'tests/test_websocket_native_reader_lifecycle.mjs',
    fixture: 'tests/fixtures/workerd-native-reader-lifecycle.mjs',
    upstream: 'tests/helpers/ws_native_reader_lifecycle_peer.mjs',
    upstreamBase: 'tests/helpers/ws_fetch_transition_peer.mjs',
    controlPeer: 'tests/helpers/ws_response_client.mjs', hostBackend: 'tests/helpers/ws_host_backend.mjs',
    contract: 'tools/workerd-native-reader-lifecycle-contract.mjs',
    stateModel: 'tools/workerd-reader-lifecycle-state.mjs',
    certificateContract: 'tools/workerd-fetch-transition-contract.mjs',
    fields: 'tools/workerd-native-fetch-contract.mjs', platform: 'tools/workerd-platform.mjs',
    reference: 'tools/workerd-reference.mjs'});
const uuid = /^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/;
const payload = [0, 102, 105, 114, 115, 116];
const bytes = value => {assert.equal(typeof value, 'string'); const result = Buffer.from(value, 'base64');
    assert.equal(result.toString('base64'), value); return result;};
const port = value => assert.ok(Number.isInteger(value) && value > 0 && value < 65536);
function assertWire(value, nonces, messages = ['started', 'done']) {
    for (const [name, final] of [['headers', false], ['wire', true]]) {
        const wire = value[name];
        fields(wire, ['status', 'rawHeader', 'requestKey', 'rawTail', 'frames', 'peerTerminated',
            'transportError', 'completion', 'closeReplySent'], 'downstream wire');
        assert.equal(wire.status, 101); assert.match(wire.requestKey, /^[A-Za-z0-9+/]{22}==$/);
        if (final) assert.equal(wire.requestKey, value.headers.requestKey);
        else {assert.ok(!nonces.has(wire.requestKey)); nonces.add(wire.requestKey);}
        const accept = createHash('sha1').update(wire.requestKey + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').digest('base64');
        const lines = wire.rawHeader.split('\r\n'); assert.equal(lines.shift(), 'HTTP/1.1 101 Switching Protocols');
        assert.deepEqual(lines.map(line => {const at = line.indexOf(':'); assert.ok(at > 0);
            return [line.slice(0, at).toLowerCase(), line.slice(at + 1).trim()];}).sort(),
        [['connection', 'Upgrade'], ['upgrade', 'websocket'], ['sec-websocket-accept', accept]].sort());
        if (!final) {
            assert.deepEqual(wire.frames, []); assert.equal(wire.rawTail, ''); assert.equal(wire.peerTerminated, false);
            assert.equal(wire.transportError, null); assert.equal(wire.completion, null); assert.equal(wire.closeReplySent, false);
            continue;
        }
        const frames = [...messages.map(text => ({opcode: 1, payload: Buffer.from(text).toString('base64')})),
            {opcode: 8, payload: Buffer.from([3, 232, 100, 111, 110, 101]).toString('base64')}];
        assert.deepEqual(wire.frames, frames);
        assert.deepEqual(bytes(wire.rawTail), Buffer.concat(frames.map(frame => {
            const data = bytes(frame.payload); return Buffer.concat([Buffer.from([128 | frame.opcode, data.length]), data]);
        })));
        assert.equal(wire.peerTerminated, true); assert.ok(['peer-ended', 'peer-reset'].includes(wire.completion));
        assert.equal(wire.transportError, wire.completion === 'peer-reset' ? 'ECONNRESET' : null);
        assert.equal(wire.closeReplySent, true);
    }
}
function assertUpstream(value, nonces) {
    const snapshot = value.upstreamBeforeStop, eof = value.kind.endsWith('|release-eof');
    fields(snapshot, ['records', 'tcpRecords', 'sni', 'tlsErrors'], 'upstream snapshot');
    assert.deepEqual(value.upstreamAfterStop, snapshot, 'service shutdown cannot supply the missing source completion');
    assert.equal(value.allSourcesEndedBeforeStop, true); assert.deepEqual(snapshot.tlsErrors, []);
    if (value.kind.startsWith('standard|')) {
        assert.deepEqual(snapshot, {records: [], tcpRecords: [], sni: [], tlsErrors: []},
            'a standard byte source must not perform upstream I/O');
        return;
    }
    assert.deepEqual(snapshot.sni, [{role: 'source', servername: 'localhost'}]);
    assert.equal(snapshot.records.length, 1); assert.equal(snapshot.tcpRecords.length, 1);
    const tcp = snapshot.tcpRecords[0];
    fields(tcp, ['role', 'localAddress', 'localPort', 'remoteAddress', 'remotePort', 'ended', 'closed', 'error'], 'TCP evidence');
    assert.equal(tcp.role, 'source'); assert.equal(tcp.localPort, value.sourcePort); port(tcp.remotePort);
    assert.ok(['127.0.0.1', '::1'].includes(tcp.localAddress)); assert.equal(tcp.remoteAddress, tcp.localAddress);
    assert.equal(tcp.closed, true); assert.equal(typeof tcp.ended, 'boolean'); assert.equal(tcp.error, null);
    const row = snapshot.records[0];
    fields(row, ['role', 'localAddress', 'localPort', 'remoteAddress', 'remotePort', 'servername', 'request', 'rawRequest',
        'responseHeader', 'responseStatus', 'frames', 'sentFrames', 'receivedChunks', 'sentChunks', 'events',
        'peerEnded', 'peerReset', 'fixtureEnded', 'fixtureDestroyed', 'error'], 'HTTP evidence');
    for (const key of ['role', 'localAddress', 'localPort', 'remoteAddress', 'remotePort']) assert.equal(row[key], tcp[key]);
    assert.equal(row.servername, 'localhost'); assert.equal(row.error, null); assert.equal(row.fixtureDestroyed, false);
    assert.equal(row.fixtureEnded, eof); assert.ok(row.peerEnded || row.peerReset);
    assert.equal(typeof row.peerEnded, 'boolean'); assert.equal(typeof row.peerReset, 'boolean');
    assert.deepEqual(row.frames, []); assert.deepEqual(row.sentFrames, []);
    assert.deepEqual(Buffer.concat(row.receivedChunks.map(bytes)), bytes(row.rawRequest));
    assert.equal(bytes(row.rawRequest).toString('latin1'), row.request + '\r\n\r\n');
    const lines = row.request.split('\r\n'); assert.equal(lines.shift(), 'GET /probe?challenge=' + value.challenge + ' HTTP/1.1');
    const headers = lines.map(line => {const at = line.indexOf(':'); assert.ok(at > 0);
        return [line.slice(0, at).toLowerCase(), line.slice(at + 1).trim()];});
    const keys = headers.filter(([name]) => name === 'sec-websocket-key'); assert.equal(keys.length, 1);
    const key = keys[0][1]; assert.match(key, /^[A-Za-z0-9+/]{22}==$/); assert.ok(!nonces.has(key)); nonces.add(key);
    assert.deepEqual(headers.sort(), [['connection', 'Upgrade'], ['host', 'localhost:' + value.sourcePort],
        ['sec-websocket-key', key], ['sec-websocket-version', '13'], ['upgrade', 'websocket'], ['x-probe', value.challenge]].sort());
    const header = 'HTTP/1.1 ' + (eof ? '200 OK' : '409 Conflict') + '\r\nContent-Length: ' + (eof ? 6 : 100)
        + '\r\nX-Probe: ' + value.challenge + '\r\nConnection: close';
    assert.equal(row.responseHeader, header); assert.equal(row.responseStatus, eof ? 200 : 409);
    assert.deepEqual(Buffer.concat(row.sentChunks.map(bytes)), Buffer.concat([Buffer.from(header + '\r\n\r\n'), Buffer.from(payload)]));
    assert.deepEqual(row.events, row.peerEnded ? ['request', 'response', 'peer-ended'] : ['request', 'response']);
}
export function assertReaderLifecycleEnvelope(value, mode, expectedState, options = {}) {
    fields(value, ['kind', 'challenge', 'target', 'sourcePort', 'allowedAuthorities', 'initial', 'headers', 'wire', 'observed',
        'allSourcesEndedBeforeStop', 'upstreamBeforeStop', 'shutdown', 'stderr', 'upstreamAfterStop', 'failure', 'cleanupFailure',
        ...(options.actorState === undefined ? [] : ['actor'])], 'native reader lifecycle case');
    assert.match(value.challenge, uuid); port(value.sourcePort);
    assert.equal(value.target, 'https://localhost:' + value.sourcePort + '/probe?challenge=' + value.challenge);
    assert.deepEqual(value.allowedAuthorities, ['localhost:' + value.sourcePort]);
    assert.equal(value.failure, null); assert.equal(value.cleanupFailure, null); assert.equal(typeof value.stderr, 'string');
    assert.doesNotMatch(value.stderr, /AddressSanitizer|LeakSanitizer|ThreadSanitizer|runtime error:|TOKEN LEAK|POISON|Uncaught/);
    for (const name of ['initial', 'observed']) {fields(value[name], ['status', 'text'], 'same worker observation'); assert.equal(value[name].status, 200);}
    const initial = JSON.parse(value.initial.text), observed = JSON.parse(value.observed.text);
    fields(initial, ['realm', 'state'], 'initial document'); fields(observed, ['realm', 'state'], 'observed document');
    assert.match(initial.realm, uuid); assert.equal(initial.state, null); assert.equal(observed.realm, initial.realm);
    assert.deepEqual(observed.state, expectedState);
    if (options.actorState !== undefined) {
        fields(value.actor, ['status', 'text'], 'foreign request observation'); assert.equal(value.actor.status, 200);
        const actor = JSON.parse(value.actor.text); fields(actor, ['realm', 'state'], 'foreign request document');
        assert.equal(actor.realm, initial.realm); assert.deepEqual(actor.state, options.actorState);
    }
    const nonces = new Set(); assertWire(value, nonces, options.messages); assertUpstream(value, nonces);
    fields(value.shutdown, ['code', 'signal', 'forced'], 'service shutdown'); assert.equal(value.shutdown.forced, false);
    if (mode === 'reference') assert.ok(value.shutdown.code === 0 && value.shutdown.signal === null ||
        value.shutdown.code === null && value.shutdown.signal === 'SIGTERM');
    else assert.deepEqual(value.shutdown, {code: 0, signal: null, forced: false});
    return {...observed.state, challenge: '<challenge>'};
}
export function assertNativeReaderLifecycleCase(value, mode) {
    assert.ok(nativeReaderLifecycleCases.includes(value.kind));
    return assertReaderLifecycleEnvelope(value, mode, expectedNativeReaderLifecycleState(value.kind, value.challenge));
}
export function validateNativeReaderLifecycleReport(report, expected) {
    fields(report, ['schema', 'complete', 'mode', 'runId', 'referenceLock', 'client', 'inputs', 'binaries', 'certificateInputs', 'cases'], 'native reader lifecycle report');
    assert.equal(report.schema, 'capsid-native-reader-lifecycle-v1'); assert.equal(report.complete, true);
    assert.equal(report.runId, expected.runId); assert.match(report.runId, /^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$/);
    assert.ok(['reference', ...referenceBackendModes(expected.lock)].includes(report.mode));
    assert.deepEqual(report.referenceLock, expected.lock); assert.deepEqual(report.inputs, expected.inputs);
    assert.deepEqual(Object.keys(report.inputs).sort(), Object.keys(nativeReaderLifecycleInputPaths).sort());
    for (const hash of Object.values(report.inputs)) assert.match(hash, /^[a-f0-9]{64}$/);
    assert.deepEqual(report.client, {node: expected.lock.client.node_version, undici: expected.lock.client.undici_version});
    assert.deepEqual(report.binaries, {host: report.mode === 'reference' ? expected.lock.binary_sha256 : expected.host,
        worker: report.mode === 'reference' ? null : expected.worker});
    assertFetchTransitionCertificates(report.certificateInputs);
    assert.deepEqual(report.cases.map(value => value.kind), nativeReaderLifecycleCases);
    assert.equal(new Set(report.cases.map(value => value.challenge)).size, nativeReaderLifecycleCases.length);
    assert.equal(new Set(report.cases.map(value => JSON.parse(value.initial.text).realm)).size, nativeReaderLifecycleCases.length);
    return report.cases.map(value => assertNativeReaderLifecycleCase(value, report.mode));
}
export function compareNativeReaderLifecycleReports(reference, candidates, expected) {
    assert.equal(reference.mode, 'reference');
    assert.deepEqual(candidates.map(value => value.mode).sort(), referenceBackendModes(expected.lock).sort());
    const baseline = validateNativeReaderLifecycleReport(reference, expected);
    for (const candidate of candidates) assert.deepEqual(validateNativeReaderLifecycleReport(candidate, expected), baseline);
    return {complete: true, casesPerBackend: nativeReaderLifecycleCases.length, backends: candidates.length};
}
