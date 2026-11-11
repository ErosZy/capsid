import strictAssert from 'node:assert/strict';
import {crossRequestMessage} from './workerd-fanout-contract.mjs';
import {referenceBackendModes} from './workerd-platform.mjs';

// The case inventory is independent of reports. Stress repeats are invocations
// of one scenario, not additional semantic coverage.
const commonCounts = Object.freeze({
    'constructor-invalid-url-protocols': 1,
    'ws://127.0.0.1/echo:echo': 1, 'ws://localhost/echo:echo': 2,
    'ws://localhost/typed:typed': 1, 'ws://localhost/large:large': 1,
    'ws://localhost/coalesced:coalesced': 1, 'wss://localhost/echo:echo': 1,
    'wss://127.0.0.1/echo:echo': 1, 'wss://localhost/untrusted:echo': 1,
    'ws://localhost/badkey:echo': 141, 'ws://localhost/not101:echo': 1,
    'ws://localhost/redirect:echo': 1, 'ws://example.invalid/echo:echo': 1,
    'ws://169.254.169.254/echo:echo': 1, 'ws://does-not-exist.invalid/echo:echo': 1,
    'ws://localhost/badprotocol:echo': 1, 'ws://localhost/echo:cancel': 1,
    'ws://localhost/stall:cancel-later': 1, 'wss://localhost/echo:cancel-later': 1,
    'ws://localhost/abrupt:echo': 1, 'out-of-order-dials': 1,
    'ws://localhost/abrupt:abrupt-observe': 1,
    'ws://localhost/abrupt:abrupt-send-in': 1,
    'ws://localhost/abrupt:abrupt-close-in': 1,
    'ws://localhost/abrupt:abrupt-send-after': 1,
    'ws://localhost/abrupt:abrupt-close-after': 1,
    'ws://localhost/abrupt:abrupt-both-after': 1,
    'pending-after-HTTP': 1, 'accepted-after-HTTP': 1, 'denied:ws://localhost/echo:echo': 1,
    'owned-pending-success': 1, 'owned-pending-reject': 1, 'owned-pending-stall': 3,
    'owned-accepted-after-HTTP': 1, 'owned-streamed-before-HTTP-end': 1,
});
export const outboundDeviations = Object.freeze([
    'unrequested-subprotocol',
]);
export const abruptOperations = Object.freeze([
    'observe', 'send-in', 'close-in', 'send-after', 'close-after', 'both-after',
]);
const nativeDiagnostics = Object.freeze({
    'wss://127.0.0.1/echo:echo': 'WebSocket TLS verification failed',
    'wss://localhost/untrusted:echo': 'WebSocket TLS verification failed',
    'ws://localhost/badkey:echo': 'Invalid WebSocket handshake response',
    'ws://example.invalid/echo:echo': 'WebSocket egress denied',
    'ws://169.254.169.254/echo:echo': 'WebSocket egress denied',
    'ws://does-not-exist.invalid/echo:echo': 'WebSocket DNS failed',
    'denied:ws://localhost/echo:echo': 'WebSocket egress denied',
    'ws://localhost/badprotocol:echo': 'Unrequested WebSocket subprotocol',
    'ws://localhost/echo:cancel': 'WebSocket connection cancelled',
    'ws://localhost/stall:cancel-later': 'WebSocket connection cancelled',
    'wss://localhost/echo:cancel-later': 'WebSocket connection cancelled',
    'ws://localhost/stall:echo': 'WebSocket dial timed out',
});
const closeEvent = {type:'close',state:3,code:1000,reason:'done\0终',clean:true};
const networkError = {type:'error',state:3,message:'Uncaught Error: Network connection lost.',
    error:'Error: Network connection lost.',errorType:'object',errorIsError:true,
    errorName:'Error',errorMessage:'Network connection lost.'};

function construction(result, assert) {
    assert.equal(result.events[0].type, 'construct');
    assert.equal(result.events[0].state, 0);
    assert.equal(result.sendRejected, true); assert.equal(result.acceptRejected, true);
    assert.match(result.events[0].url, /^wss?:\/\//);
}

export function assertOutboundDisconnect(result, operation, assert = strictAssert) {
    construction(result, assert);
    assert.deepEqual(result.events.map(e => e.type), ['construct', 'open', 'close']);
    assert.deepEqual(result.events[1], {type:'open', state:1, protocol:'chat'});
    assert.deepEqual(result.events[2], {type:'close', state:1, code:1006,
        reason:'WebSocket disconnected without sending Close frame.', clean:false});
    if (operation !== undefined) {
        assert.ok(abruptOperations.includes(operation));
        const expected = {operation, closes:1, afterCallbackMethods:1,
            microtask:2, timer:2, afterTimerMethods:2};
        if (operation.endsWith('-in')) expected.call = {result:'undefined', state:1};
        if (operation.endsWith('-after') && operation !== 'both-after') {
            expected.call = {result:'undefined', state:2};
        }
        if (operation === 'both-after') expected.calls = Array.from({length:2}, () => ({result:'undefined', state:2}));
        assert.deepEqual(result.lifecycle, expected, `${operation}: post-EOF state and method behavior`);
    }
    return result;
}

export function assertOutboundHttpOwnership(result, accepted, mode, assert = strictAssert, streamed = false) {
    assert.deepEqual(result.response, {status:202, text:streamed ? 'streamed' : 'ended'});
    assert.equal(result.state.returnedState, accepted && !streamed ? 1 : 0);
    assert.deepEqual(result.state.readyState, {error:'Error',
        message:crossRequestMessage},
        'cross-request readyState uses the exact pinned reference diagnostic');
    const normalized = structuredClone(result);
    if (accepted) {
        const clean = assertOutboundSuccess(result.state, ['after HTTP'], 'chat', mode, assert);
        normalized.state.events = clean.events;
        assert.equal(result.state.protocol, 'chat');
        assert.equal(result.state.extensions, null);
        assert.deepEqual(result.peer, {closed:true, messages:['reply:after HTTP'], closes:[1000]});
    } else {
        construction(result.state, assert);
        assert.deepEqual(result.state.events.map(e => e.type), ['construct'], 'no callback after unconnected HTTP owner ended');
        assert.equal(result.state.protocol, '');
        assert.equal(result.state.extensions, '');
        assert.deepEqual(result.peer, {closed:true, messages:[], closes:[]}, 'pending transport is stopped');
    }
    return normalized;
}

export function assertOutboundSuccess(result, expected, selected = 'chat', mode, assert = strictAssert) {
    construction(result, assert);
    const normalized = structuredClone(result);
    // The pinned reference sometimes emits exactly this extra error while
    // both wire Close frames and the final clean Close are intact. Reproduced
    // in isolation with a flushed matching reply before TCP end. Preserve the
    // original trace in reports; accept only this precise reference variant.
    if (mode === 'workerd' && normalized.events.length === expected.length + 4) {
        assert.deepEqual(normalized.events.at(-2), networkError, 'documented reference close race');
        normalized.events.splice(-2, 1);
    }
    assert.deepEqual(normalized.events.map(e => e.type), ['construct','open',...expected.map(()=>'message'),'close']);
    assert.deepEqual(normalized.events[1], {type:'open',state:1,protocol:selected});
    assert.deepEqual(normalized.events.filter(e=>e.type==='message').map(e=>e.data), expected);
    assert.deepEqual(normalized.events.at(-1), closeEvent);
    return normalized;
}

function failure(name, result, mode, assert) {
    construction(result, assert);
    assert.deepEqual(result.events.map(e => e.type), ['construct','error','close']);
    const event = result.events[1];
    const status = name === 'ws://localhost/not101:echo' ? 200 : name === 'ws://localhost/redirect:echo' ? 302 : null;
    const errorName = status ? 'TypeError' : 'Error';
    let diagnostic;
    if (status) diagnostic = `Failed to establish the WebSocket connection: expected server to reply with HTTP status code 101 (switching protocols), but received ${status} instead.`;
    else if (mode !== 'workerd') {
        diagnostic = nativeDiagnostics[name]; assert.ok(diagnostic, `known native diagnostic: ${name}`);
    } else if (name.endsWith(':cancel') || name.endsWith(':cancel-later')) diagnostic = 'Network connection lost.';
    else {
        assert.match(event.errorMessage, /^internal error; reference = [a-z0-9]+$/, `${name}: reference diagnostic ID`);
        diagnostic = event.errorMessage;
    }
    assert.deepEqual(event, {type:'error',state:3,errorType:'object',errorIsError:true,errorName,
        errorMessage:diagnostic,error:`${errorName}: ${diagnostic}`,message:`Uncaught ${errorName}: ${diagnostic}`});
    assert.deepEqual(result.events.at(-1), {type:'close',state:3,code:1006,reason:'Failed to establish websocket connection',clean:false});
    const normalized = structuredClone(result);
    // Only engine-specific diagnostic strings differ. The type, event order,
    // readiness, close payload and constructor behavior continue to compare.
    for (const field of ['message','error','errorMessage']) delete normalized.events[1][field];
    if (name === 'ws://localhost/stall:echo') {
        assert.ok(result.elapsedMs >= 9000 && result.elapsedMs < 15000, 'bounded dial deadline');
        delete normalized.elapsedMs;
    }
    return normalized;
}

function normalizeCase(name, result, mode, assert) {
    const ok = (value, data = ['hello'], protocol = 'chat') => assertOutboundSuccess(value, data, protocol, mode, assert);
    if (name === 'constructor-invalid-url-protocols') {
        assert.deepEqual(result, Array(5).fill('SyntaxError')); return result;
    }
    if (name === 'out-of-order-dials') {
        assert.equal(result.length, 3); return result.map(r => ok(r));
    }
    if (name === 'pending-after-HTTP') {
        assert.deepEqual(result, [], 'pending constructor callbacks end with the HTTP owner');
        return result;
    }
    if (name.startsWith('owned-')) return assertOutboundHttpOwnership(result,
        name === 'owned-accepted-after-HTTP' || name === 'owned-streamed-before-HTTP-end', mode, assert,
        name === 'owned-streamed-before-HTTP-end');
    if (name === 'accepted-after-HTTP') return ok(result);
    if (name === 'ws://localhost/badprotocol:echo') {
        if (mode === 'workerd') ok(result, ['hello'], 'unrequested'); else failure(name, result, mode, assert);
        return {contract:'unrequested-subprotocol'};
    }
    if (name === 'ws://localhost/abrupt:echo') return assertOutboundDisconnect(result, undefined, assert);
    if (name.startsWith('ws://localhost/abrupt:abrupt-')) {
        return assertOutboundDisconnect(result, name.slice('ws://localhost/abrupt:abrupt-'.length), assert);
    }
    if (Object.hasOwn(nativeDiagnostics,name) || /\/(?:not101|redirect):echo$/.test(name)) return failure(name,result,mode,assert);
    if (name === 'ws://localhost/typed:typed') return ok(result,[{size:3,correct:true}]);
    if (name === 'ws://localhost/large:large') return ok(result,[{size:180000,correct:true}]);
    if (name === 'ws://localhost/coalesced:coalesced') return ok(result,['coalesced','界']);
    return ok(result);
}

function normalizePorts(value) {
    if (Array.isArray(value)) return value.map(normalizePorts);
    if (!value || typeof value !== 'object') return value;
    const result = Object.fromEntries(Object.entries(value).map(([k,v])=>[k,normalizePorts(v)]));
    if (result.type === 'construct') { const url = new URL(result.url); url.port = ''; result.url = url.href; }
    return result;
}

export function compareOutboundReports(reference, candidates, expected) {
    const assert = strictAssert;
    assert.ok(reference && Array.isArray(candidates), 'outbound reports required');
    assert.deepEqual(candidates.map(r=>r.mode).sort(), referenceBackendModes(expected.lock).sort());
    const {lock,runner,host,worker} = expected;
    function validate(report, mode) {
        assert.equal(report.mode, mode); assert.equal(report.complete, true);
        assert.equal(report.sourceSHA256, runner, 'outbound runner identity');
        assert.deepEqual(report.referenceLock, lock);
        assert.deepEqual(report.client, {node:lock.client.node_version,undici:lock.client.undici_version,peer:'independent raw Node TCP/TLS peer'});
        assert.equal(report.hostSHA256, mode === 'workerd' ? lock.binary_sha256 : host);
        if (mode !== 'workerd') assert.equal(report.workerSHA256, worker);
        assert.ok(Number.isInteger(report.assertions) && report.assertions > 0);
        assert.deepEqual(report.deviations, outboundDeviations);
        assert.equal(report.sni, 'localhost');
        assert.ok(report.frames >= 20 && report.masks >= 15 && report.masks <= report.frames && report.nonces >= 150);
        assert.deepEqual(report.shutdowns.map(s=>s.label).sort(), ['allowed','denied','owned']);
        for (const shutdown of report.shutdowns) {
            assert.equal(shutdown.forced, false);
            assert.ok(shutdown.code === 0 && shutdown.signal === null ||
                mode === 'workerd' && shutdown.code === null && shutdown.signal === 'SIGTERM');
        }
        const counts = {};
        for (const c of report.cases) counts[c.name] = (counts[c.name] ?? 0) + 1;
        const required = {...commonCounts, ...(mode === 'workerd' ? {} : {'ws://localhost/stall:echo':1})};
        assert.deepEqual(counts, required, `${mode}: full outbound scenario manifest and repeat counts`);
        assert.equal(report.caseInvocations, report.cases.length);
        assert.equal(report.scenarioTypes, Object.keys(required).length);
        const normalized = {};
        for (const c of report.cases) {
            const result = normalizePorts(normalizeCase(c.name, c.result, mode, assert));
            (normalized[c.name] ??= []).push(result);
        }
        // This is a Capsid product deadline, validated above, not a claim
        // about workerd's default timeout.
        delete normalized['ws://localhost/stall:echo'];
        return normalized;
    }
    const baseline = validate(reference, 'workerd');
    for (const candidate of candidates) assert.deepEqual(validate(candidate, candidate.mode), baseline,
        `${candidate.mode}: outbound observed behavior`);
    return {complete:true,modes:candidates.map(r=>r.mode),
        invocations:Object.fromEntries([reference,...candidates].map(r=>[r.mode,r.caseInvocations])),
        scenarios:Object.keys(commonCounts), deviations:outboundDeviations,
        referenceCloseRaceEvents:reference.cases.reduce((n,c)=>n+countCloseRaces(c.result),0)};
}

function countCloseRaces(result) {
    if (Array.isArray(result)) return result.reduce((n,r)=>n+countCloseRaces(r),0);
    if (result?.state?.events) return countCloseRaces(result.state);
    return result?.events?.at(-1)?.clean && result.events.some(e=>e.type==='message')
        ? result.events.filter(e=>e.type==='error').length : 0;
}
