import assert from 'node:assert/strict';
import {createHash} from 'node:crypto';
import {referenceBackendModes} from './workerd-platform.mjs';

export const fetchUpgradeCases = Object.freeze(['upgrade-accept', 'upgrade-custom-headers', 'upgrade-large-message',
    'upgrade-non101', 'upgrade-non101-stream', 'upgrade-invalid-accept', 'upgrade-server-protocol', 'upgrade-server-extension',
    'upgrade-post', 'upgrade-abort-pre', 'upgrade-abort-pending', 'upgrade-redirect-manual', 'upgrade-redirect-follow']);
export const fetchUpgradeInputPaths = Object.freeze({runner: 'tests/test_websocket_fetch_upgrade.mjs',
    fixture: 'tests/fixtures/workerd-fetch-upgrade.mjs', serverPeer: 'tests/helpers/ws_fetch_upgrade_peer.mjs',
    contract: 'tools/workerd-fetch-upgrade-contract.mjs', hostBackend: 'tests/helpers/ws_host_backend.mjs',
    clientPeer: 'tests/helpers/ws_response_client.mjs'});
const uuid = /^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/;
const hash = bytes => createHash('sha256').update(bytes).digest('hex');
const patterned = length => Buffer.from(Array.from({length}, (_, index) => index % 251));
const badHandshake = kind => ['upgrade-invalid-accept', 'upgrade-server-extension'].includes(kind);
const ordinary = kind => ['upgrade-non101', 'upgrade-non101-stream', 'upgrade-redirect-manual'].includes(kind);
function fields(value, keys, label) {assert.ok(value && typeof value === 'object' && !Array.isArray(value)); assert.deepEqual(Object.keys(value).sort(), [...keys].sort(), label);}
function headers(request) {return request.split('\r\n').slice(1).map(line => {const at = line.indexOf(':'); assert.ok(at > 0); return [line.slice(0, at).toLowerCase(), line.slice(at + 1).trim()];}).sort(([a], [b]) => a.localeCompare(b));}
function expectedRequestHeaders(value, key) {
    const result = [['connection', 'Upgrade'], ['upgrade', 'websocket'], ['sec-websocket-version', '13'],
        ['sec-websocket-key', key], ['host', 'localhost:' + value.target.port]];
    if (value.kind === 'upgrade-custom-headers') result.push(['x-probe', value.challenge], ['origin', 'https://caller.example'],
        ['user-agent', 'caller/' + value.challenge], ['sec-websocket-protocol', 'chat, super']);
    if (value.kind === 'upgrade-post') result.push(['content-type', 'text/plain;charset=UTF-8']);
    return result.sort(([a], [b]) => a.localeCompare(b));
}
function frameBytes(opcode, payload) {const bytes = Buffer.from(payload); assert.ok(bytes.length < 126); return Buffer.concat([Buffer.from([128 | opcode, bytes.length]), bytes]);}
function assertWire(value) {
    const wire = value.wire;
    fields(wire, ['status', 'rawHeader', 'requestKey', 'rawTail', 'frames', 'peerTerminated', 'transportError', 'completion', 'closeReplySent'], 'complete client wire');
    assert.equal(wire.status, 101); assert.equal(wire.requestKey, value.headers.requestKey);
    const accept = createHash('sha1').update(wire.requestKey + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').digest('base64');
    assert.ok(wire.rawHeader.includes('Sec-WebSocket-Accept: ' + accept));
    const messages = ['started', ...(value.kind === 'upgrade-non101-stream' ? ['body-first'] : []),
        ...(value.kind === 'upgrade-abort-pending' ? ['ack/abort'] : []), 'done'];
    const frames = [...messages.map(text => ({opcode: 1, payload: Buffer.from(text).toString('base64')})), {opcode: 8, payload: 'A+hkb25l'}];
    assert.deepEqual(wire.frames, frames, 'complete independent client frames');
    assert.equal(wire.rawTail, Buffer.concat(frames.map(frame => frameBytes(frame.opcode, Buffer.from(frame.payload, 'base64')))).toString('base64'));
    assert.equal(wire.peerTerminated, true); assert.equal(wire.closeReplySent, true);
    assert.ok(['peer-ended', 'peer-reset'].includes(wire.completion)); assert.equal(wire.transportError, wire.completion === 'peer-reset' ? 'ECONNRESET' : null);
}
function socketState(binaryType, readyState = 1) {return {readyState, protocol: '', extensions: '', url: null, binaryType, bufferedAmount: {type: 'undefined'}};}
export function assertFetchUpgradeCase(value, mode) {
    assert.ok(['reference', 'single-worker', 'static-pool', 'managed'].includes(mode), 'known upgrade backend');
    fields(value, ['kind', 'challenge', 'target', 'initial', 'headers', 'wire', 'atClose', 'observed', 'upstream', 'checkpoint', 'checkpointUpstream', 'shutdown', 'stderr'], 'complete upgrade fields');
    assert.ok(fetchUpgradeCases.includes(value.kind)); assert.match(value.challenge, uuid);
    fields(value.target, ['host', 'port'], 'target'); assert.equal(value.target.host, 'localhost'); assert.ok(Number.isInteger(value.target.port) && value.target.port > 0 && value.target.port < 65536);
    const initial = JSON.parse(value.initial.text); assert.equal(value.initial.status, 200); assert.match(initial.realm, uuid); assert.equal(initial.state, null);
    assert.equal(value.observed.status, 200); assert.equal(value.atClose.status, 200); assert.equal(JSON.parse(value.observed.text).realm, initial.realm);
    assert.deepEqual(JSON.parse(value.atClose.text), JSON.parse(value.observed.text), 'no changes after actual EOF');
    const state = JSON.parse(value.observed.text).state;
    fields(state, ['kind', 'challenge', 'trace', 'result', 'socket', 'failure', 'firstBody'], 'complete same-realm state');
    assert.equal(state.kind, value.kind); assert.equal(state.challenge, value.challenge);
    const failed = badHandshake(value.kind) || value.kind.startsWith('upgrade-abort-');
    assert.deepEqual(state.trace, ['message', 'fetch', ...(value.kind === 'upgrade-non101-stream' ? ['body-first'] : []),
        ...(value.kind === 'upgrade-abort-pending' ? ['abort'] : []), failed ? 'failure' : 'complete', 'close']);
    const count = value.kind === 'upgrade-abort-pre' ? 0 : value.kind === 'upgrade-redirect-follow' ? 2 : 1;
    assert.equal(value.upstream.length, count, 'exact actual upstream inventory');
    const nonces = new Set();
    for (const [index, entry] of value.upstream.entries()) {
        fields(entry, ['localAddress', 'localPort', 'request', 'rawRequest', 'frames', 'peerEnded', 'peerReset',
            'fixtureEnded', 'fixtureDestroyed', 'responseStatus', 'events', 'error'], 'complete actual upstream entry');
        assert.equal(entry.localAddress, '127.0.0.1'); assert.equal(entry.localPort, value.target.port);
        assert.equal(Buffer.from(entry.rawRequest, 'base64').toString('latin1'), entry.request + '\r\n\r\n');
        assert.equal(entry.request.split('\r\n')[0], `GET /${index ? 'redirected' : 'hold'}?challenge=${value.challenge} HTTP/1.1`);
        const actual = headers(entry.request), keys = actual.filter(([name]) => name === 'sec-websocket-key');
        assert.equal(keys.length, 1); assert.match(keys[0][1], /^[A-Za-z0-9+/]{22}==$/);
        assert.equal(Buffer.from(keys[0][1], 'base64').toString('base64'), keys[0][1], 'canonical 16-byte nonce');
        assert.equal(nonces.has(keys[0][1]), false, 'redirect uses a fresh nonce'); nonces.add(keys[0][1]);
        assert.deepEqual(actual, expectedRequestHeaders(value, keys[0][1]), 'exact upgrade request headers, duplicates preserved');
        assert.equal(entry.fixtureDestroyed, false); assert.equal(entry.error, null); assert.equal(entry.peerEnded, true); assert.equal(entry.peerReset, false);
        const held = value.kind === 'upgrade-abort-pending', invalid = badHandshake(value.kind);
        const redirected = value.kind === 'upgrade-redirect-follow' && index === 0;
        const http = ordinary(value.kind) || redirected;
        assert.equal(entry.responseStatus, held ? null : http ? value.kind.startsWith('upgrade-redirect') ? 302 : 409 : 101, 'actual peer response status');
        assert.equal(entry.fixtureEnded, !held && !invalid, 'peer completed its response or Close');
        assert.deepEqual(entry.events, ['request', ...(held ? ['response-held'] : http
            ? [...(value.kind === 'upgrade-non101-stream' ? ['first-body'] : []), 'response-complete']
            : ['upgrade', ...(invalid ? [] : ['close-reply'])]), 'peer-ended'], 'complete upstream lifecycle');
        if (held || invalid || http) assert.deepEqual(entry.frames, [], 'no WebSocket data on failed or ordinary HTTP result');
    }
    if (failed) {
        assert.equal(state.result, null); assert.equal(state.socket, null); assert.equal(state.firstBody, null);
        fields(state.failure, ['name', 'message', 'sameReason'], 'failure');
        if (badHandshake(value.kind)) {assert.equal(state.failure.name, 'Error'); assert.match(state.failure.message, /^internal error; reference = [a-z0-9]{24}$/); assert.equal(state.failure.sameReason, false);}
        else {assert.deepEqual(state.failure, {name: 'Custom', message: value.challenge, sameReason: value.kind === 'upgrade-abort-pre'});}
    } else {
        assert.equal(state.failure, null);
        const entry = value.upstream.at(-1), requestKey = headers(entry.request).find(([name]) => name === 'sec-websocket-key')[1];
        const responseHeaders = [['set-cookie', 'first=1'], ['set-cookie', 'second=2'], ['x-probe', value.challenge]];
        const status = ordinary(value.kind) ? value.kind === 'upgrade-redirect-manual' ? 302 : 409 : 101;
        if (status === 101) responseHeaders.push(['connection', 'Upgrade'], ['upgrade', 'websocket'],
            ['sec-websocket-accept', createHash('sha1').update(requestKey + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').digest('base64')]);
        else responseHeaders.push(['connection', 'close'], value.kind === 'upgrade-non101-stream' ? ['transfer-encoding', 'chunked'] : ['content-length', '41']);
        if (value.kind === 'upgrade-custom-headers' || value.kind === 'upgrade-server-protocol') responseHeaders.push(['sec-websocket-protocol', 'chat']);
        if (value.kind === 'upgrade-redirect-manual') responseHeaders.push(['location', '/redirected?challenge=' + value.challenge]);
        responseHeaders.sort(([a], [b]) => a.localeCompare(b));
        const body = status === 101 ? null : value.kind === 'upgrade-non101-stream' ? patterned(180000) : Buffer.from('body/' + value.challenge);
        assert.deepEqual(state.result, {status, statusText: status === 101 ? 'Switching Protocols' : status === 302 ? 'Found' : 'Conflict',
            url: `http://localhost:${value.target.port}/${value.kind === 'upgrade-redirect-follow' ? 'redirected' : 'hold'}?challenge=${value.challenge}`,
            type: 'default', redirected: value.kind === 'upgrade-redirect-follow', bodyNull: status === 101, bodyUsed: status !== 101,
            headers: responseHeaders, cookies: ['first=1', 'second=2'], hasWebSocket: status === 101, constructor: status === 101 ? 'WebSocket' : null,
            body: body && {bytes: body.length, sha256: hash(body)}}, 'complete independent HTTP response');
        if (status === 101) {
            const binary = value.kind === 'upgrade-large-message' ? patterned(180000) : Buffer.from([2, 3, 4]);
            const close = {type: 'close', state: 3, code: 1000, reason: 'probe', wasClean: true};
            const events = [close];
            // This precise pinned-reference race is already independently
            // characterized by workerd-outbound-contract.mjs. Preserve the raw
            // error and require the complete matching Close and data exchange.
            // Product backends never receive this exception.
            if (mode === 'reference' && state.socket?.events?.length === 2) events.unshift({type: 'error', state: 3,
                message: 'Uncaught Error: Network connection lost.', error: 'Error: Network connection lost.', errorType: 'object',
                errorIsError: true, errorName: 'Error', errorMessage: 'Network connection lost.'});
            assert.deepEqual(state.socket, {before: socketState('blob'), beforeSend: {error: {name: 'TypeError', message:
                'You must call one of accept() or state.acceptWebSocket() on this WebSocket before sending messages.'}},
            after: socketState('arraybuffer'), afterClose: socketState('arraybuffer', 2), messages: [{type: 'text', value: 'echo/' + value.challenge},
                {type: 'binary', bytes: binary.length, sha256: hash(binary)}], sendMutation: {succeeded: true}, closed: {code: 1000, reason: 'probe', wasClean: true}, events}, 'actual accepted socket data and close');
            const messages = []; let pending, closed = false;
            for (const frame of entry.frames) {
                fields(frame, ['opcode', 'fin', 'masked', 'mask', 'payload'], 'complete actual upstream frame');
                assert.equal(typeof frame.fin, 'boolean'); assert.equal(closed, false, 'no frame after Close');
                assert.equal(frame.masked, true); assert.match(frame.mask, /^[0-9a-f]{8}$/);
                assert.equal(Buffer.from(frame.payload, 'base64').toString('base64'), frame.payload, 'canonical frame payload');
                assert.ok([0, 1, 2, 8].includes(frame.opcode), 'expected data or Close opcode');
                if (frame.opcode === 8) {assert.equal(frame.fin, true); assert.equal(frame.payload, 'A+hwcm9iZQ=='); assert.equal(pending, undefined); closed = true;}
                if ([1, 2].includes(frame.opcode)) {assert.equal(pending, undefined); pending = {opcode: frame.opcode, parts: []};}
                if ([0, 1, 2].includes(frame.opcode)) {assert.ok(pending); pending.parts.push(Buffer.from(frame.payload, 'base64')); if (frame.fin) {messages.push({opcode: pending.opcode, payload: Buffer.concat(pending.parts)}); pending = undefined;}}
            }
            assert.equal(pending, undefined); assert.equal(closed, true, 'actual upstream Close received');
            assert.deepEqual(messages, [{opcode: 1, payload: Buffer.from('echo/' + value.challenge)}, {opcode: 2, payload: binary}], 'actual independent upstream received bytes');
        } else assert.equal(state.socket, null);
        assert.deepEqual(state.firstBody, value.kind === 'upgrade-non101-stream' ? {bytes: 8192, sha256: hash(patterned(8192))} : null);
    }
    if (value.kind === 'upgrade-non101-stream') {
        assert.equal(value.checkpoint.status, 200); const checkpoint = JSON.parse(value.checkpoint.text);
        assert.equal(checkpoint.realm, initial.realm); assert.deepEqual(checkpoint.state.trace, ['message', 'fetch', 'body-first']);
        assert.deepEqual(checkpoint.state.firstBody, state.firstBody); assert.equal(checkpoint.state.result.body, null);
        assert.equal(value.checkpointUpstream.length, 1); assert.equal(value.checkpointUpstream[0].fixtureEnded, false); assert.equal(value.checkpointUpstream[0].peerEnded, false);
    } else if (value.kind === 'upgrade-abort-pending') {
        assert.equal(value.checkpoint.status, 200); assert.deepEqual(JSON.parse(value.checkpoint.text), {realm: initial.realm,
            state: {kind: value.kind, challenge: value.challenge, trace: ['message', 'fetch'], result: null, socket: null, failure: null, firstBody: null}});
        assert.equal(value.checkpointUpstream.length, 1); assert.equal(value.checkpointUpstream[0].peerEnded, false); assert.equal(value.checkpointUpstream[0].fixtureEnded, false);
    } else {assert.equal(value.checkpoint, null); assert.deepEqual(value.checkpointUpstream, []);}
    assertWire(value); fields(value.shutdown, ['code', 'signal', 'forced'], 'complete shutdown'); assert.equal(value.shutdown.forced, false);
    if (mode === 'reference') assert.ok(value.shutdown.code === 0 || value.shutdown.signal === 'SIGTERM');
    else assert.deepEqual(value.shutdown, {code: 0, signal: null, forced: false});
    assert.equal(typeof value.stderr, 'string'); assert.doesNotMatch(value.stderr, /AddressSanitizer|LeakSanitizer|ThreadSanitizer|runtime error:|TOKEN LEAK|POISON/);
    return {kind: value.kind, status: state.result?.status ?? null, socket: state.socket !== null, failed};
}
export function validateFetchUpgradeReport(report, expected) {
    fields(report, ['schema', 'complete', 'mode', 'runId', 'referenceLock', 'client', 'inputs', 'binaries', 'cases'], 'report fields');
    assert.equal(report.schema, 'capsid-fetch-upgrade-v1'); assert.equal(report.complete, true); assert.equal(report.runId, expected.runId);
    assert.match(report.runId, /^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$/, 'nonempty bounded run identity');
    assert.ok(['reference', ...referenceBackendModes(expected.lock)].includes(report.mode));
    assert.deepEqual(report.referenceLock, expected.lock); assert.deepEqual(report.inputs, expected.inputs);
    assert.deepEqual(Object.keys(report.inputs).sort(), Object.keys(fetchUpgradeInputPaths).sort());
    for (const value of Object.values(report.inputs)) assert.match(value, /^[a-f0-9]{64}$/);
    assert.deepEqual(report.client, {node: expected.lock.client.node_version, undici: expected.lock.client.undici_version});
    assert.deepEqual(report.binaries, {host: report.mode === 'reference' ? expected.lock.binary_sha256 : expected.host, worker: report.mode === 'reference' ? null : expected.worker});
    assert.deepEqual(report.cases.map(value => value.kind), fetchUpgradeCases);
    assert.equal(new Set(report.cases.map(value => value.challenge)).size, fetchUpgradeCases.length);
    assert.equal(new Set(report.cases.map(value => JSON.parse(value.initial.text).realm)).size, fetchUpgradeCases.length);
    return report.cases.map(value => assertFetchUpgradeCase(value, report.mode));
}
export function compareFetchUpgradeReports(reference, candidates, expected) {
    assert.equal(reference.mode, 'reference'); assert.deepEqual(candidates.map(value => value.mode).sort(), referenceBackendModes(expected.lock).sort());
    const baseline = validateFetchUpgradeReport(reference, expected);
    for (const report of candidates) assert.deepEqual(validateFetchUpgradeReport(report, expected), baseline);
    return {complete: true, casesPerBackend: fetchUpgradeCases.length, backends: candidates.length};
}
