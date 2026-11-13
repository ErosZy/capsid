import assert from 'node:assert/strict';
import {createHash} from 'node:crypto';
import {referenceBackendModes} from './workerd-platform.mjs';

export const asyncInputPaths = Object.freeze({runner: 'tests/test_websocket_async_callbacks.mjs',
    fixture: 'tests/fixtures/workerd-async-callbacks.mjs', contract: 'tools/workerd-async-callback-contract.mjs',
    hostBackend: 'tests/helpers/ws_host_backend.mjs', peer: 'tests/helpers/ws_response_client.mjs',
    listenerControl:'tests/helpers/workerd_listener_control.mjs',listenerContract:'tools/workerd-fetch-byob-contract.mjs',
});
export const asyncKinds = Object.freeze(['timeout-throw', 'interval-throw', 'timeout-reject',
    'timeout-async-reject', 'microtask-throw', 'promise-throw', 'timeout-caught', 'rejection-handled',
    'http-timeout-throw', 'http-timeout-reject', 'http-microtask-throw', 'http-promise-throw',
    'timeout-throw-microtask', 'interval-clear-throw', 'microtask-primitive', 'microtask-report-override',
    'microtask-error-listener-throw', 'microtask-error-listener-close', 'rejection-listener-close',
    'rejection-listener-send', 'rejection-late-handled-send', 'rejection-late-handled-close', 'rejection-listener-throw',
    'rejection-listener-handle-next']);
// These enforce Capsid's configured execution budget. No unbounded loop is sent
// to workerd, and no reference parity claim is made for these five controls.
export const asyncBudgetKinds = Object.freeze(['timer-budget-loop', 'promise-budget-loop',
    'message-budget-loop', 'timer-budget-fresh', 'interval-budget-fresh']);
const uuid = /^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/;
function keys(value, names, label) {
    assert.ok(value && typeof value === 'object', label + ' object');
    assert.deepEqual(Object.keys(value).sort(), [...names].sort(), label + ' complete fields');
}
export function asyncSourceNames(fixtureURL) {
    return {reference: 'main', 'single-worker': fixtureURL, 'static-pool': fixtureURL,
        managed: 'file://orders/v1/bundle.mjs'};
}
function expectedState(kind, challenge, sourceName) {
    const http = kind.startsWith('http-'), base = kind.replace(/^http-/, '');
    const primitive = base === 'microtask-primitive';
    const microtask = base.startsWith('microtask-');
    const rejection = ['timeout-reject', 'timeout-async-reject', 'promise-throw'].includes(base) || base.startsWith('rejection-listener-') || base.startsWith('rejection-late-handled-');
    const late = base.startsWith('rejection-late-handled-');
    const loop = base.endsWith('budget-loop');
    const observerClose = ['microtask-error-listener-close', 'rejection-listener-close', 'rejection-late-handled-close'].includes(base);
    const trace = [http ? 'http' : 'message'];
    if (loop) trace.push('loop');
    else if (base === 'interval-throw') trace.push('interval/1', 'interval/2');
    else if (base === 'interval-budget-fresh') trace.push('interval/1', 'interval/2', 'interval/3');
    else {
        trace.push('callback');
        if (base === 'timeout-async-reject') trace.push('async');
        if (microtask) trace.push('global/error');
        if (rejection) trace.push('global/unhandledrejection');
        if (base === 'timeout-throw-microtask') trace.push('nested');
        if (base === 'timeout-caught' || base === 'rejection-handled') trace.push('caught/TypeError');
        if (late) trace.push('attach/before', 'global/rejectionhandled', 'attach/after', 'caught/TypeError');
        if (base === 'rejection-listener-handle-next') trace.push('other-caught/RangeError');
    }
    if (!loop && !observerClose) trace.push('followup');
    trace.push(http ? 'response' : 'socket/close');
    const globals = [];
    if (microtask || rejection) globals.push({type: microtask ? 'error' : 'unhandledrejection', trusted: true,
        message: microtask ? primitive ? 'Uncaught primitive/' + kind : 'Uncaught TypeError: async-callback/' + kind : null,
        name: primitive ? null : 'TypeError', errorMessage: primitive ? null : 'async-callback/' + kind,
        promise: microtask ? null : true, valueType: primitive ? 'string' : 'object', sameValue: true,
        bubbles: false, cancelable: false, defaultPrevented: false,
        filename: microtask ? primitive ? 'undefined' : sourceName : null,
        lineno: microtask ? primitive ? 0 : 4 : null, colno: microtask ? primitive ? -1 : 90 : null});
    if (late) globals.push({type: 'rejectionhandled', trusted: true, message: null, name: null,
        errorMessage: null, promise: true, valueType: 'undefined', sameValue: false, bubbles: false,
        cancelable: false, defaultPrevented: false, filename: null, lineno: null, colno: null});
    return {kind, challenge, trace, globalEvents: globals,
        socketEvents: http ? [] : [{type: 'close', readyState: 3, code: loop ? 1006 : 1000,
            reason: loop ? 'WebSocket resource limit' : observerClose ? 'observer' : 'done',
            name: null, message: null, trusted: true}],
        callbacks: loop ? 0 : base === 'interval-throw' ? 2 : base === 'interval-budget-fresh' ? 3 : 1,
        sum: base === 'timer-budget-fresh' ? 49995000 : 0,
        observerThrew: ['microtask-error-listener-throw', 'rejection-listener-throw'].includes(base)};
}
function expectedWire(kind, challenge) {
    const loop = kind.endsWith('budget-loop');
    const observerClose = ['microtask-error-listener-close', 'rejection-listener-close', 'rejection-late-handled-close'].includes(kind);
    const texts = kind === 'message-budget-loop' ? [] : ['queued:' + challenge];
    if (kind === 'rejection-listener-send') texts.push('observed:' + challenge);
    if (kind === 'rejection-late-handled-send') texts.push('handled:' + challenge);
    if (!loop && !observerClose) texts.push('after:' + challenge);
    const frames = texts.map(text => ({opcode: 1, payload: Buffer.from(text).toString('base64')}));
    if (!loop) frames.push({opcode: 8, payload: Buffer.concat([Buffer.from([3, 232]), Buffer.from(observerClose ? 'observer' : 'done')]).toString('base64')});
    return {frames, rawTail: Buffer.concat(frames.map(({opcode, payload}) => {
        const data = Buffer.from(payload, 'base64');
        return Buffer.concat([Buffer.from([0x80 | opcode, data.length]), data]);
    })).toString('base64')};
}
function upgrade(wire, label) {
    assert.equal(wire.status, 101, label + ' Upgrade status');
    assert.match(wire.rawHeader, /^HTTP\/1\.1 101 /, label + ' actual Upgrade');
    assert.match(wire.requestKey, /^[A-Za-z0-9+/]{22}==$/, label + ' request key');
    const headers = new Map(wire.rawHeader.split('\r\n').slice(1).map(line => {
        const at = line.indexOf(':'); assert.ok(at > 0, label + ' valid header');
        return [line.slice(0, at).toLowerCase(), line.slice(at + 1).trim()];
    }));
    assert.equal(headers.get('upgrade')?.toLowerCase(), 'websocket', label + ' Upgrade header');
    assert.ok(headers.get('connection')?.toLowerCase().split(/\s*,\s*/).includes('upgrade'), label + ' Connection header');
    assert.equal(headers.get('sec-websocket-accept'), createHash('sha1').update(wire.requestKey +
        '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').digest('base64'), label + ' Upgrade challenge');
}
export function assertAsyncCase(kind, value, mode, sourceName) {
    assert.ok([...asyncKinds, ...asyncBudgetKinds].includes(kind), 'known async callback case');
    const label = kind + ': async callback contract', http = kind.startsWith('http-');
    keys(value, ['id', 'challenge', 'initialState', 'stateStatus', 'stateText', 'state', 'shutdown', 'stderr', 'stdout',
        ...(http ? ['response'] : ['headers', 'wire', 'elapsedMs'])], label);
    assert.equal(value.id, kind, label + ' case identity');
    assert.match(value.challenge, uuid, label + ' fresh challenge');
    keys(value.initialState, ['realm', 'state'], label + ' initial state');
    assert.match(value.initialState.realm, uuid, label + ' realm identity');
    assert.equal(value.initialState.state, null, label + ' fresh worker state');
    assert.equal(value.stateStatus, 200, label + ' healthy same realm');
    const state = {realm: value.initialState.realm, state: expectedState(kind, value.challenge, sourceName)};
    assert.deepEqual(JSON.parse(value.stateText), state, label + ' complete HTTP state');
    assert.deepEqual(value.state, state, label + ' complete callback trace and event fields');
    if (http) {
        keys(value.response, ['status', 'text'], label + ' HTTP response');
        assert.equal(value.response.status, 200, label + ' awaited HTTP succeeds');
        assert.deepEqual(JSON.parse(value.response.text), state, label + ' original HTTP response');
    } else {
        upgrade(value.headers, label); upgrade(value.wire, label);
        assert.equal(value.headers.requestKey, value.wire.requestKey, label + ' same socket');
        const expected = expectedWire(kind, value.challenge);
        assert.deepEqual(value.wire.frames, expected.frames, label + ' exact complete frames');
        assert.equal(value.wire.rawTail, expected.rawTail, label + ' exact wire bytes');
        assert.equal(value.wire.peerTerminated, true, label + ' actual peer terminal');
        assert.ok(['peer-ended', 'peer-reset'].includes(value.wire.completion), label + ' no deadline or local disposal');
        assert.equal(value.wire.transportError, value.wire.completion === 'peer-reset' ? 'ECONNRESET' : null, label + ' actual transport result');
        assert.equal(value.wire.closeReplySent, !kind.endsWith('budget-loop'), label + ' actual Close reply');
        assert.ok(Number.isFinite(value.elapsedMs) && value.elapsedMs > 0 && value.elapsedMs < 2000, label + ' bounded completion');
        if (kind.endsWith('budget-loop')) assert.ok(value.elapsedMs >= 100, label + ' configured CPU budget actually used');
        if (kind === 'timer-budget-fresh') assert.ok(value.elapsedMs >= 160, label + ' idle wait exceeds execution budget');
        if (kind === 'interval-budget-fresh') assert.ok(value.elapsedMs >= 240, label + ' each interval has a fresh budget');
    }
    for (const key of ['stderr', 'stdout']) {
        assert.equal(typeof value[key], 'string', label + ' raw ' + key);
        assert.doesNotMatch(value[key], /ERROR: (?:AddressSanitizer|LeakSanitizer)|WARNING: ThreadSanitizer|runtime error:|TOKEN LEAK|POISON EXIT/, label + ' no runtime diagnostics');
    }
    keys(value.shutdown, ['code', 'signal', 'forced'], label + ' shutdown');
    assert.equal(value.shutdown.forced, false, label + ' cooperative shutdown');
    if (mode === 'reference') assert.ok(value.shutdown.code === 0 && value.shutdown.signal === null ||
        value.shutdown.code === null && value.shutdown.signal === 'SIGTERM', label + ' actual reference shutdown');
    else assert.deepEqual(value.shutdown, {code: 0, signal: null, forced: false}, label + ' healthy product shutdown');
    // Validate actual source names and challenge bytes before normalizing only
    // deployment provenance. Error positions, flags and order stay exact.
    const normalized = structuredClone(value.state.state); normalized.challenge = '<challenge>';
    for (const event of normalized.globalEvents) if (event.filename === sourceName) event.filename = '<source>';
    const normalizedFrames = http ? null : value.wire.frames.map(({opcode, payload}) => ({opcode,
        payload: opcode === 1 ? Buffer.from(Buffer.from(payload, 'base64').toString().replace(value.challenge, '<challenge>')).toString('base64') : payload}));
    return {state: normalized, frames: normalizedFrames};
}
export function validateAsyncReport(report, expected, family = 'shared') {
    assert.equal(report.schema, 'capsid-async-callbacks-v1');
    assert.equal(report.complete, true, 'complete async callback run');
    assert.equal(report.family, family, 'async callback report family');
    assert.equal(report.runId, expected.runId, 'current async callback run');
    assert.ok(['reference', ...referenceBackendModes(expected.lock)].includes(report.mode), 'supported async callback mode');
    assert.ok(family === 'shared' || family === 'budget' && report.mode !== 'reference', 'budget controls require a configured product');
    assert.equal(report.requestTimeoutMs, family === 'budget' ? 120 : 10000, 'fixed async callback execution budget');
    assert.equal(report.sourceName, expected.sourceNames[report.mode], 'configured async callback source name');
    assert.deepEqual(report.referenceLock, expected.lock, 'locked async callback profile');
    assert.deepEqual(report.client, {node: expected.lock.client.node_version, undici: expected.lock.client.undici_version});
    assert.deepEqual(report.inputs, expected.inputs, 'async callback input provenance');
    keys(report.inputs, Object.keys(asyncInputPaths), 'async callback inputs');
    for (const hash of Object.values(report.inputs)) assert.match(hash, /^[0-9a-f]{64}$/);
    assert.deepEqual(report.binaries, {host: report.mode === 'reference' ? expected.lock.binary_sha256 : expected.host,
        worker: report.mode === 'reference' ? null : expected.worker}, 'async callback binary provenance');
    const inventory = family === 'budget' ? asyncBudgetKinds : asyncKinds;
    assert.deepEqual(report.cases.map(value => value.id), inventory, 'complete ordered async callback inventory');
    assert.equal(new Set(report.cases.map(value => value.challenge)).size, inventory.length, 'unique live challenges');
    assert.equal(new Set(report.cases.map(value => value.initialState.realm)).size, inventory.length, 'independent case realms');
    return report.cases.map(value => assertAsyncCase(value.id, value, report.mode, report.sourceName));
}
export function compareAsyncReports(reference, candidates, budgets, expected) {
    assert.equal(reference.mode, 'reference');
    const modes = referenceBackendModes(expected.lock);
    for (const reports of [candidates, budgets]) assert.deepEqual(reports.map(value => value.mode).sort(), [...modes].sort(), 'complete async callback backend matrix');
    const baseline = validateAsyncReport(reference, expected);
    for (const report of candidates) assert.deepEqual(validateAsyncReport(report, expected), baseline, 'async callbacks match pinned workerd');
    for (const report of budgets) validateAsyncReport(report, expected, 'budget');
    return {complete: true, casesPerBackend: asyncKinds.length, backends: 1 + candidates.length,
        budgetCasesPerBackend: asyncBudgetKinds.length, budgetBackends: budgets.length};
}
