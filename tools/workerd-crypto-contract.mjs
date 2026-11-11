import assert from 'node:assert/strict';
import {createHash, createHmac} from 'node:crypto';
import {referenceBackendModes} from './workerd-platform.mjs';

export const cryptoInputPaths = Object.freeze({
    runner: 'tests/test_websocket_crypto.mjs', fixture: 'tests/fixtures/workerd-crypto.mjs',
    contract: 'tools/workerd-crypto-contract.mjs',
    hostBackend: 'tests/helpers/ws_host_backend.mjs', peer: 'tests/helpers/ws_response_client.mjs',
});
export const cryptoCases = Object.freeze([
    ...['digest', 'import', 'generate', 'export', 'sign', 'verify']
        .flatMap(operation => ['order', 'close'].map(when => `crypto-${operation}-${when}`)),
    'crypto-digest-data-copy', 'crypto-digest-view-copy', 'crypto-sign-data-copy',
    'crypto-verify-data-copy', 'crypto-verify-signature-copy', 'crypto-import-data-copy',
    'crypto-digest-algorithm-getter', 'crypto-digest-throwing-getter',
    'crypto-digest-invalid-data', 'crypto-digest-invalid-name', 'crypto-digest-invalid-receiver',
    'crypto-sign-invalid-key', 'crypto-import-invalid-format', 'crypto-import-empty-usages',
    'crypto-generate-zero-length', 'crypto-export-invalid-key',
    'crypto-digest-sha1', 'crypto-digest-sha384', 'crypto-digest-sha512', 'crypto-digest-empty',
    'crypto-sign-sha1', 'crypto-sign-sha384', 'crypto-sign-sha512', 'crypto-sign-empty',
    'crypto-verify-wrong-signature', 'crypto-verify-short-signature',
    'crypto-generate-empty-usages', 'crypto-digest-forged-receiver',
    'crypto-key-map-prototype', 'crypto-subtle-set-prototype',
]);
const uuid = /^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/;
const data = Buffer.from([1, 2, 3, 4]), key = Buffer.alloc(32);
const messages = Object.freeze({
    'crypto-digest-invalid-data': ['TypeError', "Failed to execute 'digest' on 'SubtleCrypto': parameter 2 is not of type 'BufferSource'."],
    'crypto-digest-invalid-name': ['NotSupportedError', 'Unrecognized or unimplemented digest algorithm requested.'],
    'crypto-digest-invalid-receiver': ['TypeError', 'Illegal invocation: function called with incorrect `this` reference. See https://developers.cloudflare.com/workers/observability/errors/#illegal-invocation-errors for details.'],
    'crypto-sign-invalid-key': ['TypeError', "Failed to execute 'sign' on 'SubtleCrypto': parameter 2 is not of type 'CryptoKey'."],
    'crypto-import-invalid-format': ['NotSupportedError', 'Unrecognized key import format "missing".'],
    'crypto-import-empty-usages': ['SyntaxError', 'Secret/private CryptoKeys must have at least one usage.'],
    'crypto-generate-zero-length': ['OperationError', 'HMAC key length must be a non-zero unsigned long integer (requested 0).'],
    'crypto-export-invalid-key': ['TypeError', "Failed to execute 'exportKey' on 'SubtleCrypto': parameter 2 is not of type 'CryptoKey'."],
    'crypto-generate-empty-usages': ['SyntaxError', 'Secret/private CryptoKeys must have at least one usage.'],
    'crypto-digest-forged-receiver': ['TypeError', 'Illegal invocation: function called with incorrect `this` reference. See https://developers.cloudflare.com/workers/observability/errors/#illegal-invocation-errors for details.'],
    'crypto-subtle-set-prototype': ['TypeError', 'Illegal invocation: function called with incorrect `this` reference. See https://developers.cloudflare.com/workers/observability/errors/#illegal-invocation-errors for details.'],
});

// Independent protocol expectations: bytes come from Node's crypto implementation,
// and conversion/error/timing observations were verified on both pinned workerd assets.
// Never derive an expectation from a product report or from the reference being checked.
export function expectedCryptoState(kind, challenge) {
    assert.ok(cryptoCases.includes(kind), 'known crypto case');
    const state = {kind, challenge, trace: ['message', 'before'], reads: [],
        returned: 'promise', result: null, failure: null, mutation: null};
    if (kind.endsWith('-prototype')) state.observations = [];
    if (['crypto-digest-algorithm-getter', 'crypto-digest-throwing-getter',
        'crypto-digest-invalid-data', 'crypto-sign-invalid-key', 'crypto-import-invalid-format'].includes(kind))
        state.reads.push('algorithm.name');
    const throwingGetter = kind === 'crypto-digest-throwing-getter';
    if (throwingGetter || Object.hasOwn(messages, kind)) {
        state.failure = {type: 'object', name: throwingGetter ? null : messages[kind][0],
            message: throwingGetter ? null : messages[kind][1], sameSentinel: throwingGetter};
        if (kind === 'crypto-digest-invalid-receiver' || kind === 'crypto-digest-forged-receiver' || kind === 'crypto-subtle-set-prototype') {
            state.returned = 'throw';
            state.trace.push('throw', 'after', 'microtask/1', 'microtask/2', 'microtask/3', 'close');
        } else if (throwingGetter || ['crypto-digest-invalid-data', 'crypto-sign-invalid-key', 'crypto-export-invalid-key'].includes(kind)) {
            state.trace.push('after', 'failure', 'microtask/1', 'microtask/2', 'microtask/3', 'close');
        } else state.trace.push('after', 'microtask/1', 'failure', 'microtask/2', 'microtask/3', 'close');
        return state;
    }
    if (kind.endsWith('-copy')) {
        state.mutation = {succeeded: true, error: null};
        state.trace.push('mutated');
    }
    state.trace.push('after', 'microtask/1', 'complete', 'microtask/2', 'microtask/3', 'close');
    const hash = kind.endsWith('-sha1') ? 'sha1' : kind.endsWith('-sha384') ? 'sha384'
        : kind.endsWith('-sha512') ? 'sha512' : 'sha256';
    const bytes = kind.endsWith('-empty') ? Buffer.alloc(0) : data;
    if (kind.startsWith('crypto-digest-')) state.result = [...createHash(hash).update(bytes).digest()];
    else if (kind.startsWith('crypto-sign-') || kind === 'crypto-key-map-prototype') state.result = [...createHmac(hash, key).update(bytes).digest()];
    else if (kind.startsWith('crypto-verify-')) state.result = !['crypto-verify-wrong-signature', 'crypto-verify-short-signature'].includes(kind);
    else if (kind.startsWith('crypto-export-')) state.result = [...key];
    else {
        state.result = {type: 'secret', algorithm: {name: 'HMAC', hash: {name: 'SHA-256'},
            length: kind.startsWith('crypto-generate-') ? 512 : 256}, usages: ['sign']};
        if (kind === 'crypto-import-data-copy') state.result.raw = [...key];
    }
    return state;
}

function fields(value, names, label) {
    assert.ok(value && typeof value === 'object' && !Array.isArray(value), label + ' object');
    assert.deepEqual(Object.keys(value).sort(), [...names].sort(), label + ' complete fields');
}
function state(value, realm, expected, label) {
    fields(value, ['status', 'text'], label);
    assert.equal(value.status, 200, label + ' actual state response');
    assert.deepEqual(JSON.parse(value.text), {realm, state: expected}, label + ' complete same-realm state');
}
function wire(value, header, kind) {
    assert.equal(header.status, 101, 'actual Upgrade status');
    assert.match(header.rawHeader, /^HTTP\/1\.1 101 /);
    assert.match(header.requestKey, /^[A-Za-z0-9+/]{22}==$/);
    const headers = new Map(header.rawHeader.split('\r\n').slice(1).map(line => {
        const at = line.indexOf(':'); assert.ok(at > 0);
        return [line.slice(0, at).toLowerCase(), line.slice(at + 1).trim()];
    }));
    assert.equal(headers.get('upgrade')?.toLowerCase(), 'websocket');
    assert.ok(headers.get('connection')?.toLowerCase().split(/\s*,\s*/).includes('upgrade'));
    assert.equal(headers.get('sec-websocket-accept'), createHash('sha1')
        .update(header.requestKey + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').digest('base64'));
    for (const name of ['status', 'requestKey', 'rawHeader']) assert.equal(value[name], header[name], 'same actual Upgrade ' + name);
    const texts = kind.endsWith('-close') ? ['started'] : ['started', 'done'];
    const frames = texts.map(text => ({opcode: 1, payload: Buffer.from(text).toString('base64')}));
    frames.push({opcode: 8, payload: 'A+hkb25l'});
    assert.deepEqual(value.frames, frames, 'complete crypto frames');
    assert.equal(value.rawTail, Buffer.concat(frames.map(({opcode, payload}) => {
        const bytes = Buffer.from(payload, 'base64');
        return Buffer.concat([Buffer.from([0x80 | opcode, bytes.length]), bytes]);
    })).toString('base64'), 'complete crypto raw bytes');
    assert.equal(value.peerTerminated, true, 'actual peer termination');
    assert.ok(['peer-ended', 'peer-reset'].includes(value.completion), 'actual EOF or peer reset');
    assert.equal(value.transportError, value.completion === 'peer-reset' ? 'ECONNRESET' : null);
    assert.equal(value.closeReplySent, true, 'actual client Close ACK');
}
export function assertCryptoCase(value, mode) {
    fields(value, ['kind', 'challenge', 'initial', 'headers', 'wire', 'atClose', 'observed', 'shutdown', 'stderr'], 'crypto case');
    assert.ok(cryptoCases.includes(value.kind)); assert.match(value.challenge, uuid);
    const initial = JSON.parse(value.initial.text);
    assert.match(initial.realm, uuid); state(value.initial, initial.realm, null, 'initial');
    const expected = expectedCryptoState(value.kind, value.challenge);
    state(value.atClose, initial.realm, expected, 'at Close');
    state(value.observed, initial.realm, expected, 'after terminal observation');
    wire(value.wire, value.headers, value.kind);
    fields(value.shutdown, ['forced', 'code', 'signal'], 'shutdown');
    assert.equal(value.shutdown.forced, false, 'normal shutdown');
    if (mode === 'reference') assert.ok(value.shutdown.signal === 'SIGTERM' || value.shutdown.code === 0, 'reference stop');
    else assert.deepEqual(value.shutdown, {code: 0, signal: null, forced: false}, 'product clean exit');
    assert.equal(typeof value.stderr, 'string');
    assert.doesNotMatch(value.stderr, /AddressSanitizer|LeakSanitizer|ThreadSanitizer|runtime error:|TOKEN LEAK|WORKER POISONED/, 'no runtime diagnostics');
}
export function validateCryptoReport(report, mode, expected) {
    fields(report, ['schema', 'complete', 'mode', 'runId', 'referenceLock', 'client', 'inputs', 'binaries', 'cases'], 'crypto report');
    assert.equal(report.schema, 'capsid-webcrypto-compat-v1');
    assert.equal(report.complete, true, 'complete crypto collection');
    assert.equal(report.mode, mode);
    assert.ok(typeof report.runId === 'string' && report.runId.length > 0, 'explicit crypto run identity');
    assert.deepEqual(Object.keys(report.inputs).sort(), Object.keys(cryptoInputPaths).sort(), 'all crypto inputs');
    for (const hash of Object.values(report.inputs)) assert.match(hash, /^[a-f0-9]{64}$/);
    assert.deepEqual(report.client, {node: report.referenceLock.client.node_version, undici: report.referenceLock.client.undici_version}, 'locked client');
    assert.match(report.binaries.host, /^[a-f0-9]{64}$/);
    if (mode === 'reference') {
        assert.equal(report.binaries.host, report.referenceLock.binary_sha256); assert.equal(report.binaries.worker, null);
    } else assert.match(report.binaries.worker, /^[a-f0-9]{64}$/);
    assert.deepEqual(report.cases.map(value => value.kind), cryptoCases, 'complete ordered crypto inventory');
    assert.equal(new Set(report.cases.map(value => value.challenge)).size, cryptoCases.length, 'independent case challenges');
    if (expected) {
        assert.equal(report.runId, expected.runId); assert.deepEqual(report.referenceLock, expected.lock);
        assert.deepEqual(report.inputs, expected.inputs, 'current exact crypto inputs');
        if (mode !== 'reference') assert.deepEqual(report.binaries, {host: expected.host, worker: expected.worker}, 'actual current binaries');
    }
    for (const value of report.cases) assertCryptoCase(value, mode);
    return {complete: true, cases: cryptoCases.length};
}
export function compareCryptoReports(reference, candidates, expected) {
    validateCryptoReport(reference, 'reference', expected);
    assert.deepEqual(candidates.map(report => report.mode), referenceBackendModes(expected.lock), 'all required native crypto backends');
    for (const candidate of candidates) validateCryptoReport(candidate, candidate.mode, expected);
    return {complete: true, casesPerBackend: cryptoCases.length, backends: candidates.length};
}
