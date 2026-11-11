import assert from 'node:assert/strict';
import fs from 'node:fs';
import {isDeepStrictEqual} from 'node:util';
import {createHash} from 'node:crypto';
import {referenceBackendModes} from './workerd-platform.mjs';

export const ordinaryFetchInputPaths = Object.freeze({runner: 'tests/test_websocket_ordinary_fetch.mjs',
    fixture: 'tests/fixtures/workerd-ordinary-fetch.mjs', contract: 'tools/workerd-ordinary-fetch-contract.mjs',
    referenceArchive: 'tests/fixtures/workerd-ordinary-fetch-report.json',
    hostBackend: 'tests/helpers/ws_host_backend.mjs', peer: 'tests/helpers/ws_response_client.mjs'});
export const ordinaryFetchShards = Object.freeze(['core', 'pipelines', 'gated', 'views', 'bodies']);
const coreCases = Object.freeze([
    'ordinary-meta-fixed', 'ordinary-meta-chunked', 'ordinary-meta-empty', 'ordinary-meta-204',
    'ordinary-meta-redirect-manual', 'ordinary-meta-redirect-follow', 'ordinary-meta-duplicate-headers', 'ordinary-meta-head',
    'ordinary-clone-both', 'ordinary-tee-both', 'ordinary-tee-cancel-one', 'ordinary-values-full', 'ordinary-values-break',
    'ordinary-values-prevent-cancel-break', 'ordinary-pipeTo', 'ordinary-pipeThrough', 'ordinary-clone-after-lock',
    'ordinary-clone-after-read', 'ordinary-clone-used-unlocked', 'ordinary-clone-eof-unlocked', 'ordinary-clone-null-head',
    'ordinary-clone-redirect-follow', 'ordinary-headers-copies', 'ordinary-headers-conversions', 'ordinary-tee-after-lock',
    'ordinary-headers-mutable-conversions', 'ordinary-headers-unicode-names', 'ordinary-headers-unicode-copies',
    'ordinary-headers-value-edges', 'ordinary-headers-iteration-snapshot',
]);
const pipelineCases = Object.freeze(['tee-cancel-both', 'clone-cancel-both', 'values-break', 'pipeTo-abort',
    'pipeThrough-cancel', 'reader-cancel', 'pipeThrough-prevent-cancel', 'pipeThrough-mixed-pair', 'pipeThrough-standard-source',
    'pipeThrough-transform-cancel-throw', 'pipeThrough-transform-cancel-reject', 'pipeThrough-standard-cancel-reject',
    'pipeThrough-nested', 'pipeThrough-self-feedback'].map(value => 'ordinary-held-' + value));
export const ordinaryFetchGatedPlans = Object.freeze(Object.fromEntries(Object.entries({
    'ordinary-gated-default-large': [3944, 4096, 157],
    'ordinary-gated-default-aligned': [4096, 4096, 5],
    'ordinary-gated-default-fragmented': [17, 31, 49],
    'ordinary-gated-byob-fragmented': [16, 16, 16, 16, 16, 16, 1],
    'ordinary-gated-default-cancel': [17], 'ordinary-gated-byob-cancel': [16],
    'ordinary-gated-default-empty': [], 'ordinary-gated-byob-empty': [],
}).map(([id, plan]) => [id, Object.freeze(plan)])));
const viewCases = Object.freeze(['getReader', 'direct', 'prototype', 'subclass', 'reflect'].flatMap(route =>
    ['read', 'least'].flatMap(method => ['Uint16Array', 'Float64Array', 'DataView'].map(view =>
        `ordinary-binding-body-matrix-${route}-${method}-${view}`))));
export const ordinaryFetchNativePlans = Object.freeze({"ordinary-native-shape-default-small":{"bytes":97,"padding":0},"ordinary-native-shape-default-large":{"bytes":8197,"padding":0},"ordinary-native-shape-getReader-read":{"bytes":97,"padding":0},"ordinary-native-shape-getReader-least":{"bytes":97,"padding":0},"ordinary-native-shape-direct-read":{"bytes":97,"padding":0},"ordinary-native-shape-direct-least":{"bytes":97,"padding":0},"ordinary-native-shape-prototype-read":{"bytes":97,"padding":0},"ordinary-native-shape-prototype-least":{"bytes":97,"padding":0},"ordinary-native-shape-subclass-read":{"bytes":97,"padding":0},"ordinary-native-shape-subclass-least":{"bytes":97,"padding":0},"ordinary-native-shape-reflect-read":{"bytes":97,"padding":0},"ordinary-native-shape-reflect-least":{"bytes":97,"padding":0},"ordinary-native-shape-default-large-p0-b16389":{"padding":0,"bytes":16389},"ordinary-native-shape-default-large-p1-b8197":{"padding":1,"bytes":8197},"ordinary-native-shape-default-large-p1-b16389":{"padding":1,"bytes":16389},"ordinary-native-shape-default-large-p7-b8197":{"padding":7,"bytes":8197},"ordinary-native-shape-default-large-p7-b16389":{"padding":7,"bytes":16389},"ordinary-native-shape-default-large-p300-b8197":{"padding":300,"bytes":8197},"ordinary-native-shape-default-large-p300-b16389":{"padding":300,"bytes":16389},"ordinary-native-shape-default-large-p3900-b8197":{"padding":3900,"bytes":8197},"ordinary-native-shape-default-large-p3900-b16389":{"padding":3900,"bytes":16389},"ordinary-native-shape-default-large-p4097-b8197":{"padding":4097,"bytes":8197},"ordinary-native-shape-default-large-p4097-b16389":{"padding":4097,"bytes":16389}});
const bodyCases = Object.freeze([...Object.keys(ordinaryFetchNativePlans), "ordinary-headers-read-conversions-order"]);
const inventory = Object.freeze({core: coreCases, pipelines: pipelineCases,
    gated: Object.freeze(Object.keys(ordinaryFetchGatedPlans)), views: viewCases, bodies: bodyCases});
export function ordinaryFetchCasesForShard(shard) {
    assert.ok(ordinaryFetchShards.includes(shard), 'mandatory known ordinary-fetch shard');
    return inventory[shard];
}
export const ordinaryFetchCases = Object.freeze(ordinaryFetchShards.flatMap(shard => inventory[shard]));
const uuid = /^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/;
function fields(value, names, label) {
    assert.ok(value && typeof value === 'object' && !Array.isArray(value), label + ' object');
    assert.deepEqual(Object.keys(value).sort(), [...names].sort(), label + ' complete fields');
}
function rebind(value, fromChallenge, toChallenge, fromPort, toPort) {
    if (typeof value === 'string') return value.replaceAll(fromChallenge, toChallenge)
        .replaceAll('http://localhost:' + fromPort, 'http://localhost:' + toPort);
    if (Array.isArray(value)) return value.map(item => rebind(item, fromChallenge, toChallenge, fromPort, toPort));
    if (value && typeof value === 'object') return Object.fromEntries(Object.entries(value)
        .map(([key, item]) => [key, rebind(item, fromChallenge, toChallenge, fromPort, toPort)]));
    return value;
}
const archive = JSON.parse(fs.readFileSync(new URL('../tests/fixtures/workerd-ordinary-fetch-report.json', import.meta.url), 'utf8'));
fields(archive, ['schema', 'provenance', 'reports'], 'reference archive');
assert.equal(archive.schema, 'capsid-ordinary-fetch-reference-archive-v1');
assert.deepEqual(Object.keys(archive.reports), ordinaryFetchShards);
assert.equal(archive.provenance.length, 10);
function canonical(value) {
    if (Array.isArray(value)) return value.map(canonical);
    if (value && typeof value === 'object') return Object.fromEntries(Object.keys(value).sort().map(key => [key, canonical(value[key])]));
    return value;
}
for (const shard of ordinaryFetchShards) {
    assert.deepEqual(archive.reports[shard].cases.map(value => value.id), inventory[shard], 'frozen independent case inventory');
    const provenance = archive.provenance.filter(value => value.shard === shard);
    assert.deepEqual(provenance.map(value => value.platform), ['native', 'linux']);
    assert.equal(provenance[0].normalizedStateSha256, provenance[1].normalizedStateSha256, 'both actual workerd states agree');
    const states = archive.reports[shard].cases.map(sample => rebind(JSON.parse(sample.observed.text).state,
        sample.challenge, 'CHALLENGE', sample.targets[0].port, 'PORT'));
    const serialized = JSON.stringify(canonical(states)).replace(/[\u007f-\uffff]/g, character =>
        '\\u' + character.charCodeAt(0).toString(16).padStart(4, '0'));
    assert.equal(createHash('sha256').update(serialized).digest('hex'), provenance[0].normalizedStateSha256,
        'complete archived states retain their recorded independent reference digest');
    for (const source of provenance) {
        assert.equal(source.referenceLock.source_commit, '8f311ae3e3cf3064983f56a6a1c8b7ee33d6b99f');
        assert.equal(source.referenceLock.release_tag, 'v1.20261010.1');
        assert.equal(source.referenceLock.compatibility_date, '2026-08-20');
        assert.deepEqual(source.referenceLock.compatibility_flags, []);
        assert.match(source.sha256, /^[0-9a-f]{64}$/);
    }
}
// Cache immutable expectations only. Every actual observation is still
// parsed, strictly compared and independently checked on each invocation.
const expectedStateCache = new Map(), nativeModelCache = new Map();
function freezeExpected(value) {
    if (value && typeof value === 'object' && !Object.isFrozen(value)) {
        for (const child of Object.values(value)) freezeExpected(child);
        Object.freeze(value);
    }
    return value;
}
export function expectedOrdinaryFetchState(id, challenge, port) {
    const cacheKey = JSON.stringify([id, challenge, port]);
    if (expectedStateCache.has(cacheKey)) return expectedStateCache.get(cacheKey);
    const sample = archive.reports[ordinaryFetchShards.find(shard => inventory[shard].includes(id))]?.cases.find(value => value.id === id);
    assert.ok(sample, 'known ordinary-fetch state');
    // Header conversion/iterator/error/lock expectations come from the two pinned
    // workerd assets. Only the challenge and source URL are rebound; no output
    // fields or payload bytes are discarded. Byte sources have separate models.
    const result = freezeExpected(rebind(JSON.parse(sample.observed.text).state,
        sample.challenge, challenge, sample.targets[0].port, port));
    if (expectedStateCache.size >= 512) expectedStateCache.delete(expectedStateCache.keys().next().value);
    expectedStateCache.set(cacheKey, result);
    return result;
}
const payload = (length, offset = 0) => Array.from({length}, (_, index) => ((index + offset) * 29 + 17) % 251);
function expectedRead(length, offset, byob, capacity, cursor, done) {
    if (done && !byob) return {inputAfter: null, done: true, type: null, offset: null,
        length: null, capacity: null, bytes: null, backing: null};
    const backing = Array(capacity).fill(byob ? 165 : 0), bytes = payload(length, cursor);
    backing.splice(offset, length, ...bytes);
    return {inputAfter: byob ? 0 : null, done, type: 'Uint8Array', offset, length, capacity, bytes, backing};
}
function independentBody(state, id) {
    if (Object.hasOwn(ordinaryFetchNativePlans, id)) {
        const plan = ordinaryFetchNativePlans[id], byob = !id.includes('-default-');
        const modelKey = id + '/' + state.challenge.length;
        let model = nativeModelCache.get(modelKey);
        if (!model) {
            const lengths = [];
            let remaining = plan.bytes;
            const packet = Buffer.from(sourcePacket(id, state.challenge, false).packet, 'base64');
            const headerLength = packet.indexOf('\r\n\r\n') + 4;
            let capacity = byob ? 16 : 4096 - headerLength % 4096;
            while (remaining) {
                const length = Math.min(remaining, capacity); lengths.push(length);
                remaining -= length; capacity = byob ? 16 : 4096;
            }
            let cursor = 0;
            const reads = lengths.map(length => {const row = expectedRead(length, byob ? 8 : 0, byob, byob ? 32 : 4096, cursor, false); cursor += length; return row;});
            reads.push(expectedRead(0, 0, byob, byob ? 32 : 4096, cursor, true));
            model = freezeExpected({reads, bytes: payload(plan.bytes)});
            nativeModelCache.set(modelKey, model);
        }
        assert.deepEqual(state.reads, model.reads, id + ' independent un-gated bytes/backing/detachment/EOF model');
        assert.deepEqual(state.result, {bytes: model.bytes});
        assert.deepEqual(state.readerBrand, {byob, default: !byob});
        assert.equal(state.closedStable, true); assert.equal(state.closedSettled, true);
        assert.equal(state.after.bodyUsed, true); assert.equal(state.after.locked, false);
    } else if (viewCases.includes(id)) {
        const [route, method, view] = id.split('-').slice(-3);
        const reads = [32, 32, 32, 1, 0].map((length, index) => expectedRead(length, length ? 8 : 0, true, 64, index * 32, index === 4));
        assert.deepEqual(state.rows, [{route, method, view, failure: null, reads, brand: true, closedStable: true,
            closed: 'fulfilled', lockedAfter: false}], id + ' independent 97-byte view/backing/detachment/EOF model');
    } else if (Object.hasOwn(ordinaryFetchGatedPlans, id)) {
        const plan = ordinaryFetchGatedPlans[id], byob = id.includes('-byob-'), cancel = id.endsWith('-cancel');
        let cursor = 0;
        const reads = plan.map(length => {const row = expectedRead(length, byob ? 8 : 0, byob, byob ? 32 : 4096, cursor, false); cursor += length; return row;});
        if (!cancel) reads.push(expectedRead(0, 0, byob, byob ? 32 : 4096, cursor, true));
        assert.deepEqual(state.reads, reads, id + ' independent gated body/backing/EOF model');
        assert.deepEqual(state.result, {bytes: payload(cursor)});
        assert.deepEqual(state.readerBrand, {byob, default: !byob});
        assert.equal(state.closedStable, true); assert.equal(state.closed, 'fulfilled');
        assert.deepEqual(state.after, {bodyUsed: true, locked: false});
        if (cancel) assert.equal(state.cancelReturnedUndefined, true);
    } else {
        const empty = ['ordinary-meta-empty', 'ordinary-meta-204', 'ordinary-meta-redirect-manual',
            'ordinary-meta-head', 'ordinary-clone-null-head'].includes(id);
        // Validate every recorded bytes array independently of the snapshot.
        const visit = (value, trail) => {
            if (!value || typeof value !== 'object') return;
            for (const [key, child] of Object.entries(value)) {
                if (key === 'bytes') {
                    let length = id.startsWith('ordinary-held-') ? 17 : empty ? 0 : 97;
                    if (['ordinary-clone-after-read', 'ordinary-clone-used-unlocked', 'ordinary-clone-eof-unlocked'].includes(id)
                        && (trail.startsWith('result') || trail.startsWith('copyResult'))) length = 0;
                    if (trail.startsWith('remaining')) length = 0;
                    const bytes = payload(length).map(byte => id === 'ordinary-pipeThrough' ? (byte + 1) & 255 : byte);
                    assert.deepEqual(child, bytes, id + ' independent payload at ' + trail);
                } else visit(child, trail ? trail + '.' + key : key);
            }
        };
        visit(state, '');
        if (id === 'ordinary-held-values-break') assert.deepEqual(state.values, [payload(17)]);
        if (id === 'ordinary-held-pipeTo-abort') assert.deepEqual(state.pipeEvents[0], {write: payload(17)});
    }
}
function wire(value, upgrade, id, terminal) {
    const keys = ['status', 'rawHeader', 'requestKey', 'rawTail', 'frames', 'peerTerminated', 'transportError', 'completion', 'closeReplySent'];
    fields(value, keys, id + ' wire');
    assert.equal(value.status, 101);
    assert.equal(Buffer.from(value.requestKey, 'base64').length, 16);
    assert.equal(Buffer.from(value.requestKey, 'base64').toString('base64'), value.requestKey);
    const lines = value.rawHeader.split('\r\n');
    assert.equal(lines.shift(), 'HTTP/1.1 101 Switching Protocols');
    const headers = lines.map(line => {const colon = line.indexOf(':'); assert.ok(colon > 0); return [line.slice(0, colon).toLowerCase(), line.slice(colon + 1).trim()];}).sort();
    const accept = createHash('sha1').update(value.requestKey + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').digest('base64');
    assert.deepEqual(headers, [['connection', 'Upgrade'], ['sec-websocket-accept', accept], ['upgrade', 'websocket']]);
    if (!terminal) {
        assert.deepEqual(value, {...value, rawTail: '', frames: [], peerTerminated: false, transportError: null, completion: null, closeReplySent: false});
        return;
    }
    assert.equal(value.rawHeader, upgrade.rawHeader); assert.equal(value.requestKey, upgrade.requestKey);
    const texts = ['started', ...(Object.hasOwn(ordinaryFetchGatedPlans, id) ? ['headers',
        ...ordinaryFetchGatedPlans[id].map((length, index) => 'read/' + index + '/' + length)] : []), 'done'];
    const frames = [...texts.map(text => ({opcode: 1, payload: Buffer.from(text).toString('base64')})),
        {opcode: 8, payload: Buffer.concat([Buffer.from([3, 232]), Buffer.from('done')]).toString('base64')}];
    assert.deepEqual(value.frames, frames, id + ' complete WebSocket transcript');
    assert.equal(value.rawTail, Buffer.concat(frames.map(frame => {const bytes = Buffer.from(frame.payload, 'base64');
        return Buffer.concat([Buffer.from([0x80 | frame.opcode, bytes.length]), bytes]);})).toString('base64'));
    assert.equal(value.peerTerminated, true); assert.equal(value.closeReplySent, true);
    assert.ok(['peer-ended', 'peer-reset'].includes(value.completion));
    assert.equal(value.transportError, value.completion === 'peer-reset' ? 'ECONNRESET' : null);
}
function sourcePacket(id, challenge, landing) {
    const held = id.startsWith('ordinary-held-'), gated = Object.hasOwn(ordinaryFetchGatedPlans, id);
    let status = id === 'ordinary-meta-204' ? 204 : 201, phrase = status === 204 ? 'No Content' : 'Created';
    let body = Buffer.from(['ordinary-meta-empty', 'ordinary-meta-204'].includes(id) ? [] : payload(ordinaryFetchNativePlans[id]?.bytes ?? 97));
    const headers = ['Connection: close', 'Content-Type: application/octet-stream', 'X-Probe: ' + challenge];
    if (id.includes('redirect-') && !landing) {status = 302; phrase = 'Found'; body = Buffer.alloc(0); headers.push('Location: /landing?challenge=' + challenge);}
    if (id === 'ordinary-meta-duplicate-headers') headers.push('X-List: first', 'X-List: second', 'Set-Cookie: a=1', 'Set-Cookie: b=2');
    if (ordinaryFetchNativePlans[id]?.padding) headers.push('X-Padding: ' + 'p'.repeat(ordinaryFetchNativePlans[id].padding));
    const cancel = id.endsWith('-cancel'), plan = ordinaryFetchGatedPlans[id];
    const declared = gated ? cancel ? 1048673 : plan.reduce((a, b) => a + b, 0) : held ? 1048673 : body.length;
    headers.push(id === 'ordinary-meta-chunked' ? 'Transfer-Encoding: chunked' : 'Content-Length: ' + declared);
    const head = Buffer.from(`HTTP/1.1 ${status} ${phrase}\r\n${headers.join('\r\n')}\r\n\r\n`);
    if (id === 'ordinary-meta-head' || id === 'ordinary-clone-null-head' || status === 204 || gated) body = Buffer.alloc(0);
    else if (id === 'ordinary-meta-chunked') body = Buffer.concat([Buffer.from('11\r\n'), body.subarray(0, 17), Buffer.from('\r\n50\r\n'), body.subarray(17), Buffer.from('\r\n0\r\n\r\n')]);
    return {packet: Buffer.concat([head, held ? body.subarray(0, 17) : body]).toString('base64'), declared};
}
function upstream(records, value, terminal) {
    const id = value.id, held = id.startsWith('ordinary-held-'), gated = Object.hasOwn(ordinaryFetchGatedPlans, id);
    const count = id.includes('redirect-follow') ? 2 : 1;
    assert.ok(Array.isArray(records));
    if (terminal) assert.equal(records.length, count, id + ' exact actual request count');
    else assert.ok(records.length >= 1 && records.length <= count);
    for (const [index, entry] of records.entries()) {
        fields(entry, ['request', 'rawRequest', 'bytes', 'peerEnded', 'peerReset', 'fixtureEnded', 'fixtureDestroyed', 'events',
            'localAddress', 'localPort', 'sent', ...(gated ? ['bodyPackets', 'progress', 'declaredBodyBytes'] : ['held', ...(held ? ['declaredBodyBytes', 'sentBodyBytes'] : [])])], id + ' source');
        const {host, port} = value.targets[0];
        const path = index ? '/landing?challenge=' + value.challenge : '/hold?challenge=' + value.challenge + '&role=a';
        const method = ['ordinary-meta-head', 'ordinary-clone-null-head'].includes(id) ? 'HEAD' : 'GET';
        assert.equal(entry.request.split('\r\n')[0], method + ' ' + path + ' HTTP/1.1');
        const headers = entry.request.split('\r\n').slice(1).map(line => {const colon = line.indexOf(':'); assert.ok(colon > 0);
            return [line.slice(0, colon).toLowerCase(), line.slice(colon + 1).trim()];}).sort();
        assert.deepEqual(headers, [['host', host + ':' + port]], id + ' ordinary HTTP has no Upgrade or hidden headers');
        assert.equal(entry.rawRequest, Buffer.from(entry.request + '\r\n\r\n').toString('base64'));
        assert.equal(entry.bytes, Buffer.byteLength(entry.request + '\r\n\r\n'));
        assert.equal(entry.localAddress, '127.0.0.1'); assert.equal(entry.localPort, port);
        assert.equal(entry.fixtureDestroyed, false); assert.equal(entry.peerReset, false);
        for (const key of ['peerEnded', 'fixtureEnded']) assert.equal(typeof entry[key], 'boolean');
        if (terminal) {assert.equal(entry.peerEnded, true); assert.equal(entry.fixtureEnded, true);}
        const {packet, declared} = sourcePacket(id, value.challenge, index !== 0);
        assert.equal(entry.sent, packet, id + ' exact independent response wire bytes');
        assert.ok(Array.isArray(entry.events)); assert.equal(entry.events[0], 'request');
        assert.equal(new Set(entry.events).size, entry.events.length, 'unique causal source events');
        if (!gated) {
            assert.equal(entry.held, held);
            if (held) {assert.equal(entry.declaredBodyBytes, 1048673); assert.equal(entry.sentBodyBytes, 17);}
            const expected = ['request', held ? 'held-body' : 'sent-complete', ...(entry.peerEnded ? ['peer-ended'] : []),
                ...(held && entry.peerEnded ? ['server-half-close'] : [])];
            assert.deepEqual(entry.events, expected);
            assert.equal(entry.fixtureEnded, !held || entry.peerEnded);
        } else {
            const plan = ordinaryFetchGatedPlans[id], cancel = id.endsWith('-cancel');
            assert.equal(entry.declaredBodyBytes, declared);
            // Before header acknowledgement the body cannot have been released.
            if (!terminal) {
                assert.deepEqual(entry.bodyPackets, []); assert.deepEqual(entry.progress, []);
                const events = ['request', 'headers', ...(plan.length ? [] : ['server-complete']), ...(entry.peerEnded ? ['peer-ended'] : [])];
                assert.deepEqual(entry.events, events); assert.equal(entry.fixtureEnded, plan.length === 0);
            } else {
                let cursor = 0;
                assert.deepEqual(entry.bodyPackets, plan.map(length => {const bytes = payload(length, cursor); cursor += length; return Buffer.from(bytes).toString('base64');}));
                assert.deepEqual(entry.progress, plan.map((length, index) => ({index, length})));
                const events = entry.events, at = event => {const result = events.indexOf(event); assert.ok(result >= 0, id + ' missing ' + event); return result;};
                const allowed = ['request', 'headers', 'headers-observed', 'peer-ended',
                    ...plan.flatMap((length, index) => ['body/' + index + '/' + length, 'read/' + index + '/' + length]),
                    ...(!plan.length || id.includes('-byob-') && !cancel ? ['server-complete'] : ['server-half-close'])];
                // A default reader may finish Content-Length before the final ACK
                // reaches the runner. Model causal constraints, not OS scheduling.
                if (plan.length && !id.includes('-byob-') && !cancel && events.includes('server-complete')) {
                    allowed[allowed.indexOf('server-half-close')] = 'server-complete';
                }
                assert.deepEqual([...events].sort(), allowed.sort(), id + ' complete causal event set');
                assert.ok(at('request') < at('headers') && at('headers') < at('headers-observed'));
                for (const [index, length] of plan.entries()) {
                    assert.ok(at(index ? 'read/' + (index - 1) + '/' + plan[index - 1] : 'headers-observed') < at('body/' + index + '/' + length));
                    assert.ok(at('body/' + index + '/' + length) < at('read/' + index + '/' + length));
                }
                if (events.includes('server-half-close')) assert.ok(at('peer-ended') < at('server-half-close'));
                if (cancel) assert.ok(at('body/0/' + plan[0]) < at('peer-ended'));
                if (events.includes('server-complete')) {
                    assert.ok(at('server-complete') < at('peer-ended'));
                    assert.ok(at(plan.length ? 'body/' + (plan.length - 1) + '/' + plan.at(-1) : 'headers') < at('server-complete'));
                }
            }
        }
    }
}
export function assertOrdinaryFetchCase(value, mode) {
    fields(value, ['id', 'challenge', 'targets', 'initial', 'upgradea', 'beforeAction', 'wirea', 'observed', 'finalState',
        'upstream', 'shutdown', 'stderr'], 'ordinary fetch case');
    assert.ok(ordinaryFetchCases.includes(value.id)); assert.match(value.challenge, uuid);
    assert.equal(value.targets.length, 1); fields(value.targets[0], ['host', 'port'], 'source target');
    const {host, port} = value.targets[0]; assert.equal(host, 'localhost'); assert.ok(Number.isInteger(port) && port > 0 && port <= 65535);
    fields(value.initial, ['status', 'text'], 'initial HTTP state'); assert.equal(value.initial.status, 200);
    const initial = JSON.parse(value.initial.text); fields(initial, ['realm', 'state'], 'initial realm');
    assert.match(initial.realm, uuid); assert.equal(initial.state, null);
    const expected = expectedOrdinaryFetchState(value.id, value.challenge, port);
    for (const key of ['observed', 'finalState']) {
        fields(value[key], ['status', 'text'], key); assert.equal(value[key].status, 200);
        const actual = JSON.parse(value[key].text);
        assert.ok(isDeepStrictEqual(actual, {realm: initial.realm, state: expected}), value.id + ' complete ' + key + ' state differs');
        independentBody(actual.state, value.id);
    }
    wire(value.upgradea, null, value.id, false); wire(value.wirea, value.upgradea, value.id, true);
    upstream(value.beforeAction, value, false); upstream(value.upstream, value, true);
    for (const [index, entry] of value.beforeAction.entries()) {
        const after = value.upstream[index];
        assert.deepEqual(after.events.slice(0, entry.events.length), entry.events, 'actual source history is retained');
        for (const [key, item] of Object.entries(entry)) if (!['events', 'peerEnded', 'fixtureEnded', 'bodyPackets', 'progress'].includes(key))
            assert.deepEqual(after[key], item, 'source identity retained: ' + key);
    }
    assert.equal(typeof value.stderr, 'string');
    assert.doesNotMatch(value.stderr, /ERROR: (?:AddressSanitizer|LeakSanitizer)|WARNING: ThreadSanitizer|runtime error:|TOKEN LEAK|POISON EXIT/);
    fields(value.shutdown, ['code', 'signal', 'forced'], 'natural shutdown'); assert.equal(value.shutdown.forced, false);
    if (mode === 'reference') assert.ok(value.shutdown.code === 0 && value.shutdown.signal === null || value.shutdown.code === null && value.shutdown.signal === 'SIGTERM');
    else assert.deepEqual(value.shutdown, {code: 0, signal: null, forced: false});
    return expectedOrdinaryFetchState(value.id, 'CHALLENGE', 'PORT');
}
export function validateOrdinaryFetchReport(report, expected) {
    fields(report, ['schema', 'complete', 'shard', 'mode', 'runId', 'referenceLock', 'client', 'inputs', 'binaries', 'cases'], 'ordinary fetch report');
    assert.equal(report.schema, 'capsid-ordinary-fetch-v1'); assert.equal(report.complete, true);
    const cases = ordinaryFetchCasesForShard(report.shard); assert.equal(report.shard, expected.shard);
    assert.equal(report.runId, expected.runId); assert.equal(typeof report.runId, 'string'); assert.ok(report.runId.length > 0);
    assert.ok(['reference', ...referenceBackendModes(expected.lock)].includes(report.mode));
    assert.deepEqual(report.referenceLock, expected.lock); assert.deepEqual(report.inputs, expected.inputs);
    fields(report.inputs, Object.keys(ordinaryFetchInputPaths), 'ordinary fetch input identities');
    for (const hash of Object.values(report.inputs)) assert.match(hash, /^[0-9a-f]{64}$/);
    assert.deepEqual(report.client, {node: expected.lock.client.node_version, undici: expected.lock.client.undici_version});
    assert.deepEqual(report.binaries, {host: report.mode === 'reference' ? expected.lock.binary_sha256 : expected.host,
        worker: report.mode === 'reference' ? null : expected.worker});
    assert.deepEqual(report.cases.map(value => value.id), cases, 'complete ordinary-fetch shard, no filters or duplicates');
    assert.equal(new Set(report.cases.map(value => value.challenge)).size, cases.length);
    assert.equal(new Set(report.cases.map(value => JSON.parse(value.initial.text).realm)).size, cases.length);
    assert.equal(new Set(report.cases.map(value => value.upgradea.requestKey)).size, cases.length);
    return report.cases.map(value => assertOrdinaryFetchCase(value, report.mode));
}
export function compareOrdinaryFetchShardReports(references, candidates, expected) {
    assert.deepEqual(references.map(value => value.shard), ordinaryFetchShards);
    assert.deepEqual(candidates.map(value => value.mode), referenceBackendModes(expected.lock));
    const result = [];
    for (const [index, shard] of ordinaryFetchShards.entries()) {
        const reference = references[index]; assert.equal(reference.mode, 'reference');
        const baseline = validateOrdinaryFetchReport(reference, {...expected, shard});
        for (const group of candidates) {
            fields(group, ['mode', 'reports'], 'ordinary fetch backend');
            assert.deepEqual(group.reports.map(value => value.shard), ordinaryFetchShards);
            const report = group.reports[index]; assert.equal(report.mode, group.mode);
            assert.deepEqual(validateOrdinaryFetchReport(report, {...expected, shard}), baseline);
        }
        result.push({shard, cases: inventory[shard].length});
    }
    return {complete: true, casesPerBackend: ordinaryFetchCases.length, backends: 1 + candidates.length, shards: result};
}
