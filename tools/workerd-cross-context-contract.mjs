import assert from 'node:assert/strict';
import {createHash} from 'node:crypto';
import {referenceBackendModes} from './workerd-platform.mjs';

export const crossContextInputPaths = Object.freeze({runner: 'tests/test_websocket_cross_context.mjs',
    fixture: 'tests/fixtures/workerd-cross-context.mjs', contract: 'tools/workerd-cross-context-contract.mjs',
    hostBackend: 'tests/helpers/ws_host_backend.mjs', peer: 'tests/helpers/ws_response_client.mjs',
    listenerControl:'tests/helpers/workerd_listener_control.mjs',listenerContract:'tools/workerd-fetch-byob-contract.mjs',
});
export const crossContextCases = Object.freeze([
    ['reaction-a-settle-b', [['a','create'],['a','attach'],['b','resolve']]],
    ['reaction-b-settle-a', [['a','create'],['b','attach'],['a','resolve']]],
    ['reaction-b-settle-http', [['a','create'],['b','attach'],['http','resolve']]],
    ['reaction-http-created', [['http','create'],['a','attach'],['b','resolve']]],
    ['reject-a-settle-b', [['a','create'],['b','reject']]],
    ['reject-a-settle-http', [['a','create'],['http','reject']]],
    ['reject-a-late-catch-b', [['a','create'],['a','reject'],['b','catch']]],
    ['reject-a-late-catch-http', [['a','create'],['a','reject'],['http','catch']]],
    ['reaction-a-throw-settle-b', [['a','create'],['a','attach-throw'],['b','resolve']]],
    ['thenable-a-settle-b', [['a','create'],['a','attach'],['b','resolve-thenable']]],
    ['thenable-b-settle-a', [['a','create'],['b','attach'],['a','resolve-thenable']]],
    ['origin-closed-b-reaction', [['a','create'],['b','attach'],['a','finish'],['b','resolve']]],
    ['origin-closed-b-thenable', [['a','create'],['b','attach'],['a','finish'],['b','resolve-thenable']]],
    ['origin-closed-b-reject', [['a','create'],['a','finish'],['b','reject']]],
    ['origin-closed-settled', [['a','create'],['a','resolve'],['a','finish'],['b','attach']]],
    ['module-origin-reaction-b', [['a','use-module'],['b','attach'],['a','resolve']]],
    ['origin-closed-resolve-then-attach', [['a','create'],['a','finish'],['b','resolve'],['b','attach']]],
    ['origin-http-resolve-then-attach', [['http','create'],['b','resolve'],['b','attach']]],
    ['reaction-a-order-b', [['a','create'],['a','attach'],['b','resolve-order']]],
    ['reaction-b-order-a', [['a','create'],['b','attach'],['a','resolve-order']]],
    ['reaction-a-order-a', [['a','create'],['a','attach'],['a','resolve-order']]],
    ['reject-a-order-b', [['a','create'],['b','reject-order']]],
    ['await-a-settle-b', [['a','create'],['a','await'],['b','resolve']]],
    ['await-b-settle-a', [['a','create'],['b','await'],['a','resolve']]],
    ['all-b-settle-a', [['a','create'],['b','all'],['a','resolve']]],
    ['module-origin-await-b', [['a','use-module'],['b','await'],['a','resolve']]],
].map(([id, commands]) => Object.freeze({id, commands: Object.freeze(commands.map(pair => Object.freeze(pair)))})));

const foreign = 'Error: Cannot perform I/O on behalf of a different request. '
    + 'I/O objects (such as streams, request/response bodies, and others) created in the context '
    + "of one request handler cannot be accessed from a different request's handler. "
    + 'This is a limitation of Cloudflare Workers which allows us to improve overall performance. (I/O type: Native)';
function fields(value, names, label) {
    assert.ok(value && typeof value === 'object' && !Array.isArray(value), label + ' object');
    assert.deepEqual(Object.keys(value).sort(), [...names].sort(), label + ' complete fields');
}
function event(type, samePromise = true) {
    return {type, samePromise, sameReason: type === 'unhandledrejection',
        reasonType: type === 'unhandledrejection' ? 'object' : 'undefined',
        trusted: true, cancelable: false, bubbles: false, defaultPrevented: false};
}
// Independent behavioral specification established on both pinned native
// workerd assets. It consumes the fixed commands, never an observed report.
function expectedFor(id) {
    const spec = crossContextCases.find(value => value.id === id); assert.ok(spec);
    const trace = [], events = [], texts = {a: [], b: []};
    let origin, settled = false, rejected = false, attached, closed = false, reported = false;
    const writes = (where, owner) => {
        for (const role of ['a', 'b']) {
            trace.push(where + '/' + role + '/' + (role === owner ? 'ok' : foreign));
            if (role === owner) texts[role].push(where + '/' + role);
        }
    };
    const notify = (type, owner, same = true) => {
        events.push(event(type, same)); trace.push('event/' + type); writes(type, owner);
    };
    const react = (callback, owner) => {
        if (callback.command === 'attach-throw') {trace.push('reaction/' + callback.role); notify('unhandledrejection', callback.role, false);}
        else {
            const type = callback.command === 'attach' ? 'reaction' : callback.command;
            trace.push(type + '/' + callback.role); writes(type, owner);
        }
    };
    for (const [role, command] of spec.commands) {
        trace.push(role + '/' + command);
        const ack = () => {if (role !== 'http') texts[role].push('ack/' + role + '/' + command);};
        if (command === 'create' || command === 'use-module') {origin = command === 'use-module' ? null : role; ack();}
        else if (command === 'finish') {closed = true; ack(); trace.push('close/' + role);}
        else if (['attach','attach-throw','await','all'].includes(command)) {
            attached = {role, command}; ack();
            if (settled && !rejected) react(attached, role);
        } else if (command === 'catch') {
            assert.ok(reported); trace.push('catch/before'); notify('rejectionhandled', role);
            trace.push('catch/after'); ack(); trace.push('catch/' + role + '/true'); writes('catch', role);
        } else {
            const order = command.endsWith('-order'), thenable = command === 'resolve-thenable';
            if (thenable) {trace.push('getter/' + role); writes('getter', role);}
            ack();
            if (thenable) {trace.push('thenable/' + role); writes('thenable', role);}
            const live = origin !== 'http' && !(origin === 'a' && closed);
            if (order) trace.push('settle/after', 'micro/before');
            const deliver = () => {
                if (!live) return;
                settled = true; rejected = command.startsWith('reject');
                if (rejected) {reported = true; notify('unhandledrejection', origin ?? role);}
                else if (attached) react(attached, origin === null ? role : attached.role);
            };
            const sameTurn = origin === role && (!attached || attached.role === role);
            if (!order || sameTurn) deliver();
            if (order) {
                trace.push('micro/after', 'micro/nested-before', 'micro/nested-after', 'timer/settler');
                if (!sameTurn) deliver();
            }
        }
    }
    const state = {trace: [...trace], events: structuredClone(events)};
    for (const role of ['a','b']) {
        if (role === 'a' && closed) continue;
        texts[role].push('ack/' + role + '/finish'); trace.push(role + '/finish', 'close/' + role);
    }
    return {state, finalState: {trace, events}, texts};
}
function upgrade(value, label) {
    assert.equal(value.status, 101, label + ' Upgrade status');
    assert.match(value.rawHeader, /^HTTP\/1\.1 101 /, label + ' complete response status');
    assert.match(value.requestKey, /^[A-Za-z0-9+/]{22}==$/, label + ' fresh physical challenge');
    const headers = new Map(value.rawHeader.split('\r\n').slice(1).map(line => {
        const at = line.indexOf(':'); assert.ok(at > 0, label + ' complete header');
        return [line.slice(0, at).toLowerCase(), line.slice(at + 1).trim()];
    }));
    assert.equal(headers.get('upgrade')?.toLowerCase(), 'websocket');
    assert.ok(headers.get('connection')?.toLowerCase().split(/\s*,\s*/).includes('upgrade'));
    assert.equal(headers.get('sec-websocket-accept'), createHash('sha1')
        .update(value.requestKey + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').digest('base64'), label + ' actual Upgrade response');
}
function http(value, expected, realm, challenge, label) {
    fields(value, ['status', 'text'], label); assert.equal(value.status, 200, label + ' healthy same realm');
    assert.deepEqual(JSON.parse(value.text), {realm, challenge, ...expected}, label + ' complete actual HTTP state');
}
function wire(value, header, texts, label) {
    upgrade(value, label); assert.equal(value.requestKey, header.requestKey, label + ' original physical connection');
    const frames = texts.map(text => ({opcode: 1, payload: Buffer.from(text).toString('base64')}));
    frames.push({opcode: 8, payload: 'A+hkb25l'});
    assert.deepEqual(value.frames, frames, label + ' complete frames');
    const raw = Buffer.concat(frames.map(({opcode, payload}) => {
        const data = Buffer.from(payload, 'base64'); assert.ok(data.length < 126);
        return Buffer.concat([Buffer.from([0x80 | opcode, data.length]), data]);
    })).toString('base64');
    assert.equal(value.rawTail, raw, label + ' exact raw transport bytes');
    assert.equal(value.peerTerminated, true, label + ' actual peer terminal');
    assert.ok(['peer-ended', 'peer-reset'].includes(value.completion), label + ' no deadline or local disposal');
    assert.equal(value.transportError, value.completion === 'peer-reset' ? 'ECONNRESET' : null);
    assert.equal(value.closeReplySent, true, label + ' actual Close ACK');
}
export function assertCrossContextCase(value, mode) {
    const spec = crossContextCases.find(entry => entry.id === value.id); assert.ok(spec, 'known cross-context input');
    const label = value.id + ': cross-context';
    fields(value, ['id','challenge','commands','initial','upgradea','upgradeb','wirea','wireb','state','finalState','shutdown','stderr'], label);
    assert.match(value.challenge, /^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/);
    assert.equal(value.initial.status, 200); const {realm} = JSON.parse(value.initial.text);
    assert.match(realm, /^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/);
    http(value.initial, {trace: [], events: []}, realm, value.challenge, label + ' initial');
    assert.deepEqual(value.commands.map(({role, command}) => [role, command]), spec.commands, label + ' exact command inventory');
    for (const command of value.commands) {
        if (command.role === 'http') {
            fields(command, ['role','command','status','text'], label + ' HTTP command');
            assert.equal(command.status, 200); assert.deepEqual(JSON.parse(command.text), {realm, challenge: value.challenge});
        } else {
            fields(command, ['role','command','seen'], label + ' connection command');
            assert.ok(Array.isArray(command.seen) && command.seen.length);
            assert.equal(command.seen.at(-1), 'ack/' + command.role + '/' + command.command, label + ' causal peer ACK');
        }
    }
    const expected = expectedFor(value.id);
    for (const role of ['a','b']) {
        const unread = [...expected.texts[role]];
        for (const command of value.commands.filter(entry => entry.role === role)) {
            const seen = [], ack = 'ack/' + role + '/' + command.command;
            while (unread.length) {const text = unread.shift(); seen.push(text); if (text === ack) break;}
            assert.deepEqual(command.seen, seen, label + ' complete causal peer observations');
        }
    }
    http(value.state, expected.state, realm, value.challenge, label + ' observed');
    http(value.finalState, expected.finalState, realm, value.challenge, label + ' closed');
    for (const role of ['a','b']) {upgrade(value['upgrade' + role], label + role); wire(value['wire' + role], value['upgrade' + role], expected.texts[role], label + role);}
    assert.notEqual(value.upgradea.requestKey, value.upgradeb.requestKey, label + ' distinct connections');
    assert.equal(typeof value.stderr, 'string');
    assert.doesNotMatch(value.stderr, /ERROR: (?:AddressSanitizer|LeakSanitizer)|WARNING: ThreadSanitizer|runtime error:|TOKEN LEAK|POISON EXIT/);
    fields(value.shutdown, ['code','signal','forced'], label + ' shutdown'); assert.equal(value.shutdown.forced, false);
    if (mode === 'reference') assert.ok(value.shutdown.code === 0 && value.shutdown.signal === null ||
        value.shutdown.code === null && value.shutdown.signal === 'SIGTERM');
    else assert.deepEqual(value.shutdown, {code: 0, signal: null, forced: false});
    return {state: expected.state, finalState: expected.finalState, texts: expected.texts};
}
export function validateCrossContextReport(report, expected) {
    fields(report, ['schema','complete','mode','runId','referenceLock','client','inputs','binaries','cases'], 'cross-context report');
    assert.equal(report.schema, 'capsid-cross-context-v1'); assert.equal(report.complete, true, 'complete cross-context run');
    assert.equal(report.runId, expected.runId, 'fresh cross-context run');
    assert.ok(['reference', ...referenceBackendModes(expected.lock)].includes(report.mode));
    assert.deepEqual(report.referenceLock, expected.lock); assert.deepEqual(report.inputs, expected.inputs);
    fields(report.inputs, Object.keys(crossContextInputPaths), 'cross-context inputs');
    for (const hash of Object.values(report.inputs)) assert.match(hash, /^[0-9a-f]{64}$/);
    assert.deepEqual(report.client, {node: expected.lock.client.node_version, undici: expected.lock.client.undici_version});
    assert.deepEqual(report.binaries, {host: report.mode === 'reference' ? expected.lock.binary_sha256 : expected.host,
        worker: report.mode === 'reference' ? null : expected.worker});
    assert.deepEqual(report.cases.map(value => value.id), crossContextCases.map(value => value.id), 'complete ordered cross-context inventory');
    assert.equal(new Set(report.cases.map(value => value.challenge)).size, crossContextCases.length, 'fresh case challenges');
    assert.equal(new Set(report.cases.map(value => JSON.parse(value.initial.text).realm)).size, crossContextCases.length, 'fresh case realms');
    return report.cases.map(value => assertCrossContextCase(value, report.mode));
}
export function compareCrossContextReports(reference, candidates, expected) {
    assert.equal(reference.mode, 'reference');
    assert.deepEqual(candidates.map(value => value.mode).sort(), referenceBackendModes(expected.lock).sort());
    const baseline = validateCrossContextReport(reference, expected);
    for (const report of candidates) assert.deepEqual(validateCrossContextReport(report, expected), baseline);
    return {complete: true, casesPerBackend: crossContextCases.length, backends: 1 + candidates.length};
}
