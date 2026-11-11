import assert from 'node:assert/strict';
import {createHash} from 'node:crypto';
import {referenceBackendModes} from './workerd-platform.mjs';

export const fetchResourceInputPaths = Object.freeze({runner: 'tests/test_websocket_fetch_resources.mjs',
    fixture: 'tests/fixtures/workerd-fetch-resources.mjs', contract: 'tools/workerd-fetch-resource-contract.mjs',
    hostBackend: 'tests/helpers/ws_host_backend.mjs', peer: 'tests/helpers/ws_response_client.mjs'});
export const fetchResourceCases = Object.freeze([
    'fetch-complete', 'fetch-close-pending', 'fetch-close-body', 'fetch-cancel-reader',
    ...['pre', 'live', 'body'].flatMap(stage => ['default', 'string', 'error', 'dom', 'null', 'number', 'object']
        .map(reason => 'fetch-abort-' + stage + '-' + reason)),
    'fetch-two-owners', 'fetch-abort-live-mutation', 'fetch-abort-live-shared', 'fetch-disabled-cookie-cache',
    'fetch-sync-context', 'fetch-sync-context-numeric',
    'fetch-custom-headers',
]);
// Observed on both locked native workerd assets, independently of product output.
const foreignContextError = 'Error: Cannot perform I/O on behalf of a different request. '
    + 'I/O objects (such as streams, request/response bodies, and others) created in the context '
    + "of one request handler cannot be accessed from a different request's handler. "
    + 'This is a limitation of Cloudflare Workers which allows us to improve overall performance. '
    + '(I/O type: Native)';
const uuid = /^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/;
function fields(value, names, label) {
    assert.ok(value && typeof value === 'object' && !Array.isArray(value), label + ' object');
    assert.deepEqual(Object.keys(value).sort(), [...names].sort(), label + ' complete fields');
}
function reasonFor(kind, challenge, failure = false) {
    const choice = kind.split('-').at(-1), pre = kind.startsWith('fetch-abort-pre-');
    const converted = failure && !pre;
    const type = !converted && ['string', 'number'].includes(choice) ? choice : 'object';
    const constructor = choice === 'default' || choice === 'dom' ? 'DOMException'
        : choice === 'error' || converted && ['string', 'null', 'number'].includes(choice) ? 'Error'
        : choice === 'string' ? 'String' : choice === 'number' ? 'Number' : choice === 'null' ? null : 'Object';
    return {type, name: constructor === 'DOMException' ? 'AbortError' : constructor === 'Error' ? 'Error'
        : constructor === 'Object' ? 'Custom' : null,
    message: choice === 'default' ? 'The operation was aborted'
        : ['dom', 'error', 'object', 'mutation', 'shared'].includes(choice) ? choice === 'mutation' && !failure ? 'changed' : challenge
        : converted ? choice === 'string' ? challenge : choice === 'number' ? '23' : 'null' : null,
    constructor, sameReason: !failure || pre,
    primitive: !converted && ['string', 'null', 'number'].includes(choice) ? choice === 'string' ? challenge : choice === 'number' ? '23' : 'null' : null};
}
// This specification is independent of collected reports and runtime output.
export function expectedFetchResourceState(kind, challenge, beforeRelease = false) {
    assert.ok(fetchResourceCases.includes(kind));
    const state = {kind, challenge, trace: ['message', 'fetch'], result: null, reason: null, failure: null, clones: null, other: null};
    if (kind.startsWith('fetch-sync-context')) {
        state.trace = ['message', 'before/a/ok', 'before/b/' + foreignContextError, 'fetch',
            'after/a/ok', 'after/b/' + foreignContextError, 'headers', 'callback/a/ok',
            'callback/b/' + foreignContextError, 'complete', 'close'];
        state.result = 'complete/' + challenge + '/a';
        state.other = {trace: beforeRelease ? ['message', 'checkpoint'] : ['message', 'checkpoint', 'finish', 'close'], result: null};
    } else if (kind === 'fetch-complete' || kind === 'fetch-custom-headers' || kind === 'fetch-disabled-cookie-cache') {
        state.trace.push('headers', 'complete', 'close'); state.result = 'complete/' + challenge + '/a';
    } else if (kind === 'fetch-close-pending' || kind === 'fetch-two-owners') {
        state.trace.push('finish', 'close');
        if (kind === 'fetch-two-owners') state.other = {trace: beforeRelease ? ['message', 'fetch', 'checkpoint']
            : ['message', 'fetch', 'checkpoint', 'headers', 'complete', 'close'], result: beforeRelease ? null : 'complete/' + challenge + '/b'};
    } else {
        if (kind === 'fetch-close-body' || kind === 'fetch-cancel-reader' || kind.startsWith('fetch-abort-body-')) {
            state.trace.push('headers', 'first'); state.result = {first: 'first/' + challenge + '/a', done: false};
        }
        if (kind === 'fetch-close-body') state.trace.push('finish', 'close');
        else if (kind === 'fetch-cancel-reader') state.trace.push('cancel/before', 'cancel/after', 'close');
        else {
            if (!kind.startsWith('fetch-abort-pre-')) state.trace.push('abort');
            state.trace.push(...(kind === 'fetch-abort-live-shared' ? ['failure/0', 'failure/1', 'close'] : ['failure', 'close']));
            state.reason = reasonFor(kind, challenge); state.failure = reasonFor(kind, challenge, true);
            if (kind === 'fetch-abort-live-shared') state.clones = {sameErrors: false, sameReason0: false, sameReason1: false,
                sameNested: false, sharedAlias0: true, sharedAlias1: true, firstNested: 'original', secondNested: 'original'};
        }
    }
    return state;
}
function upgrade(value, label) {
    assert.equal(value.status, 101, label + ' Upgrade'); assert.match(value.rawHeader, /^HTTP\/1\.1 101 /);
    assert.match(value.requestKey, /^[A-Za-z0-9+/]{22}==$/);
    const headers = new Map(value.rawHeader.split('\r\n').slice(1).map(line => {
        const at = line.indexOf(':'); assert.ok(at > 0); return [line.slice(0, at).toLowerCase(), line.slice(at + 1).trim()];
    }));
    assert.equal(headers.get('upgrade')?.toLowerCase(), 'websocket');
    assert.ok(headers.get('connection')?.toLowerCase().split(/\s*,\s*/).includes('upgrade'));
    assert.equal(headers.get('sec-websocket-accept'), createHash('sha1').update(value.requestKey + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').digest('base64'));
}
function textsFor(kind, role) {
    if (kind.startsWith('fetch-sync-context')) return role === 'a'
        ? ['started', 'before/a', 'after/a', 'callback/a', 'done'] : ['started', 'ack/checkpoint', 'ack/finish'];
    if (role === 'b') return ['started', 'ack/checkpoint', 'done'];
    if (['fetch-close-pending', 'fetch-two-owners'].includes(kind)) return ['started', 'ack/finish'];
    if (kind === 'fetch-close-body') return ['started', 'reading', 'ack/finish'];
    if (kind.startsWith('fetch-abort-body-')) return ['started', 'reading', 'ack/abort', 'done'];
    if (kind.startsWith('fetch-abort-live-')) return ['started', 'ack/abort', 'done'];
    return ['started', 'done'];
}
function wire(value, header, texts, label) {
    upgrade(value, label); assert.equal(value.requestKey, header.requestKey);
    const frames = texts.map(text => ({opcode: 1, payload: Buffer.from(text).toString('base64')}));
    frames.push({opcode: 8, payload: 'A+hkb25l'}); assert.deepEqual(value.frames, frames, label + ' complete frames');
    assert.equal(value.rawTail, Buffer.concat(frames.map(({opcode, payload}) => {
        const data = Buffer.from(payload, 'base64'); assert.ok(data.length < 126);
        return Buffer.concat([Buffer.from([0x80 | opcode, data.length]), data]);
    })).toString('base64'), label + ' complete raw bytes');
    assert.equal(value.peerTerminated, true, label + ' actual transport terminal');
    assert.ok(['peer-ended', 'peer-reset'].includes(value.completion), label + ' no local disposal or deadline');
    assert.equal(value.transportError, value.completion === 'peer-reset' ? 'ECONNRESET' : null);
    assert.equal(value.closeReplySent, true, label + ' actual Close ACK');
}
function http(value, realm, state, label) {
    fields(value, ['status', 'text'], label); assert.equal(value.status, 200);
    assert.deepEqual(JSON.parse(value.text), {realm, state}, label + ' complete same-realm state');
}
function requestHeaders(entry, value, index, label) {
    const target = value.targets[index];
    assert.equal(entry.localAddress, '127.0.0.1', label + ' actual listening address');
    assert.equal(entry.localPort, target.port, label + ' actual accepted socket port');
    const lines = entry.request.split('\r\n').slice(1);
    const actual = lines.map(line => {
        const colon = line.indexOf(':'); assert.ok(colon > 0, label + ' header syntax');
        return [line.slice(0, colon).toLowerCase(), line.slice(colon + 1).trim()];
    }).sort((left, right) => left[0].localeCompare(right[0]));
    const expected = [['host', target.host + ':' + target.port]];
    if (value.id === 'fetch-custom-headers') expected.push(
        ['user-agent', 'caller/' + value.challenge], ['accept-encoding', 'identity'], ['origin', 'https://caller.example'],
        ['cache-control', 'max-age=17'], ['pragma', 'caller'], ['x-capsid-header', value.challenge]);
    expected.sort((left, right) => left[0].localeCompare(right[0]));
    assert.deepEqual(actual, expected, label + ' exact independent request headers');
}
function upstream(records, value, terminal, label) {
    const count = value.id.startsWith('fetch-abort-pre-') ? 0 : value.id === 'fetch-two-owners' || value.id === 'fetch-abort-live-shared' ? 2 : 1;
    assert.equal(records.length, count, label + ' exact upstream inventory');
    for (let index = 0; index < count; ++index) {
        const entry = records[index], role = value.id === 'fetch-two-owners' && index === 1 ? 'b' : 'a';
        fields(entry, ['request', 'rawRequest', 'bytes', 'peerEnded', 'peerReset', 'fixtureEnded', 'fixtureDestroyed', 'events', 'localAddress', 'localPort'], label);
        assert.equal(entry.request.split('\r\n')[0], 'GET /hold?challenge=' + value.challenge + '&role=' + role + '&index=' +
            (value.id === 'fetch-abort-live-shared' ? index : 0) + ' HTTP/1.1', label + ' actual request target');
        assert.equal(Buffer.from(entry.rawRequest, 'base64').toString(), entry.request + '\r\n\r\n');
        assert.equal(entry.bytes, Buffer.byteLength(entry.request + '\r\n\r\n'));
        requestHeaders(entry, value, index, label);
        assert.equal(entry.fixtureDestroyed, false, label + ' fixture must not terminate observed resource');
        assert.equal(entry.peerReset, false, label + ' clean actual EOF');
        const fixtureEnded = value.id === 'fetch-complete' || value.id === 'fetch-custom-headers' || value.id === 'fetch-disabled-cookie-cache' || value.id.startsWith('fetch-sync-context') || value.id === 'fetch-two-owners' && role === 'b' && terminal;
        assert.equal(entry.fixtureEnded, fixtureEnded, label + ' exact fixture response boundary');
        const first = ['fetch-close-body', 'fetch-cancel-reader'].includes(value.id) || value.id.startsWith('fetch-abort-body-');
        const ended = terminal || value.id === 'fetch-two-owners' && role === 'a';
        assert.equal(entry.peerEnded, ended, label + ' actual peer EOF before teardown');
        assert.deepEqual(entry.events, ['request', ...(first ? ['sent-first'] : []), ...(fixtureEnded ? ['sent-complete'] : []), ...(ended ? ['peer-ended'] : [])], label + ' causal upstream events');
    }
}
export function assertFetchResourceCase(value, mode) {
    const label = value.id + ': fetch resources'; assert.ok(fetchResourceCases.includes(value.id)); assert.match(value.challenge, uuid);
    const two = value.id === 'fetch-two-owners', syncContext = value.id.startsWith('fetch-sync-context');
    fields(value, ['id', 'challenge', 'targets', 'initial', 'upgradea', 'wirea', 'beforeAction', 'observed', 'finalState', 'upstream', 'shutdown', 'stderr',
        ...(two || syncContext ? ['upgradeb', 'wireb', 'checkpoint', 'checkpointUpstream'] : []), ...(value.id === 'fetch-disabled-cookie-cache' ? ['filesystemGuard'] : [])], label);
    assert.equal(value.targets.length, two || value.id === 'fetch-abort-live-shared' ? 2 : 1, label + ' listener inventory');
    for (const target of value.targets) {
        fields(target, ['host', 'port'], label + ' target');
        assert.equal(target.host, value.id === 'fetch-sync-context-numeric' ? '127.0.0.1' : 'localhost');
        assert.ok(Number.isInteger(target.port) && target.port > 0 && target.port <= 65535);
    }
    assert.equal(new Set(value.targets.map(target => target.port)).size, value.targets.length);
    if (value.id === 'fetch-disabled-cookie-cache') {
        fields(value.filesystemGuard, ['path', 'before', 'after'], label + ' filesystem guard');
        assert.equal(value.filesystemGuard.path, '.LCK');
        for (const snapshot of [value.filesystemGuard.before, value.filesystemGuard.after]) {
            fields(snapshot, ['type', 'dev', 'ino'], label + ' filesystem identity');
            assert.equal(snapshot.type, 'directory'); assert.match(snapshot.dev, /^\d+$/); assert.match(snapshot.ino, /^\d+$/);
        }
        assert.deepEqual(value.filesystemGuard.after, value.filesystemGuard.before, label + ' empty-cookie-path guard remains intact');
    }
    const realm = JSON.parse(value.initial.text).realm; assert.match(realm, uuid); http(value.initial, realm, null, label + ' initial');
    const expected = expectedFetchResourceState(value.id, value.challenge);
    http(value.observed, realm, expected, label + ' observed'); http(value.finalState, realm, expected, label + ' final');
    upgrade(value.upgradea, label); wire(value.wirea, value.upgradea, textsFor(value.id, 'a'), label);
    upstream(value.upstream, value, true, label);
    if (value.id.startsWith('fetch-abort-live-') || value.id.startsWith('fetch-abort-body-') || value.id.startsWith('fetch-close-') || two) {
        assert.ok(value.beforeAction.length > 0); for (const entry of value.beforeAction) {
            assert.equal(entry.peerEnded, false, label + ' upstream alive before owner action');
            assert.equal(entry.peerReset, false); assert.equal(entry.fixtureEnded, false); assert.equal(entry.fixtureDestroyed, false);
        }
    }
    if (two || syncContext) {
        upgrade(value.upgradeb, label + ' b'); wire(value.wireb, value.upgradeb, textsFor(value.id, 'b'), label + ' b');
        assert.notEqual(value.upgradea.requestKey, value.upgradeb.requestKey);
        http(value.checkpoint, realm, expectedFetchResourceState(value.id, value.challenge, true), label + ' foreign live checkpoint');
        upstream(value.checkpointUpstream, value, syncContext, label + ' foreign live checkpoint');
    }
    assert.equal(typeof value.stderr, 'string');
    assert.doesNotMatch(value.stderr, /ERROR: (?:AddressSanitizer|LeakSanitizer)|WARNING: ThreadSanitizer|runtime error:|TOKEN LEAK|POISON EXIT/);
    fields(value.shutdown, ['code', 'signal', 'forced'], label + ' shutdown'); assert.equal(value.shutdown.forced, false);
    if (mode === 'reference') assert.ok(value.shutdown.code === 0 && value.shutdown.signal === null || value.shutdown.code === null && value.shutdown.signal === 'SIGTERM');
    else assert.deepEqual(value.shutdown, {code: 0, signal: null, forced: false});
    return {state: expectedFetchResourceState(value.id, 'CHALLENGE'), texts: textsFor(value.id, 'a')};
}
export function validateFetchResourceReport(report, expected) {
    fields(report, ['schema', 'complete', 'mode', 'runId', 'referenceLock', 'client', 'inputs', 'binaries', 'cases'], 'fetch resource report');
    assert.equal(report.schema, 'capsid-fetch-resources-v1'); assert.equal(report.complete, true);
    assert.equal(report.runId, expected.runId); assert.ok(['reference', ...referenceBackendModes(expected.lock)].includes(report.mode));
    assert.deepEqual(report.referenceLock, expected.lock); assert.deepEqual(report.inputs, expected.inputs);
    fields(report.inputs, Object.keys(fetchResourceInputPaths), 'fetch resource inputs');
    for (const hash of Object.values(report.inputs)) assert.match(hash, /^[0-9a-f]{64}$/);
    assert.deepEqual(report.client, {node: expected.lock.client.node_version, undici: expected.lock.client.undici_version});
    assert.deepEqual(report.binaries, {host: report.mode === 'reference' ? expected.lock.binary_sha256 : expected.host,
        worker: report.mode === 'reference' ? null : expected.worker});
    assert.deepEqual(report.cases.map(value => value.id), fetchResourceCases);
    assert.equal(new Set(report.cases.map(value => value.challenge)).size, fetchResourceCases.length);
    assert.equal(new Set(report.cases.map(value => JSON.parse(value.initial.text).realm)).size, fetchResourceCases.length);
    return report.cases.map(value => assertFetchResourceCase(value, report.mode));
}
export function compareFetchResourceReports(reference, candidates, expected) {
    assert.equal(reference.mode, 'reference');
    assert.deepEqual(candidates.map(value => value.mode).sort(), referenceBackendModes(expected.lock).sort());
    const baseline = validateFetchResourceReport(reference, expected);
    for (const report of candidates) assert.deepEqual(validateFetchResourceReport(report, expected), baseline);
    return {complete: true, casesPerBackend: fetchResourceCases.length, backends: 1 + candidates.length};
}
