import assert from 'node:assert/strict';
import test from 'node:test';
import fs from 'node:fs';
import {asyncKinds, asyncBudgetKinds, asyncSourceNames, assertAsyncCase, validateAsyncReport, compareAsyncReports} from '../tools/workerd-async-callback-contract.mjs';

// These are stored actual runs used only to test report validation. The gate
// always executes fresh native reference and product processes.
const stored = JSON.parse(fs.readFileSync(new URL('./fixtures/workerd-async-callback-report.json', import.meta.url), 'utf8'));
const storedBudget = JSON.parse(fs.readFileSync(new URL('./fixtures/workerd-async-budget-report.json', import.meta.url), 'utf8'));
function matrix() {
    const reference = structuredClone(stored);
    const expected = {runId: reference.runId, lock: reference.referenceLock, inputs: reference.inputs,
        sourceNames: asyncSourceNames('file:///qualification/callbacks.mjs'), host: 'a'.repeat(64), worker: 'b'.repeat(64)};
    const candidates = ['single-worker', 'static-pool'].map(mode => {
        const report = structuredClone(reference); report.mode = mode; report.sourceName = expected.sourceNames[mode];
        report.binaries = {host: expected.host, worker: expected.worker};
        for (const value of report.cases) {
            value.shutdown = {code: 0, signal: null, forced: false};
            for (const event of value.state.state.globalEvents) if (event.filename === 'main') event.filename = report.sourceName;
            value.stateText = JSON.stringify(value.state);
            if (value.response) value.response.text = value.stateText;
        }
        return report;
    });
    const budgets = candidates.map(({mode}) => ({...structuredClone(storedBudget), mode,
        runId: expected.runId, referenceLock: expected.lock, inputs: expected.inputs, sourceName: expected.sourceNames[mode],
        binaries: {host: expected.host, worker: expected.worker}}));
    return {reference, candidates, budgets, expected};
}
test('literal shared inventory keeps 24 error, Promise, HTTP and listener I/O cases', () => {
    assert.deepEqual(asyncKinds, ['timeout-throw', 'interval-throw', 'timeout-reject', 'timeout-async-reject',
        'microtask-throw', 'promise-throw', 'timeout-caught', 'rejection-handled', 'http-timeout-throw',
        'http-timeout-reject', 'http-microtask-throw', 'http-promise-throw', 'timeout-throw-microtask',
        'interval-clear-throw', 'microtask-primitive', 'microtask-report-override', 'microtask-error-listener-throw',
        'microtask-error-listener-close', 'rejection-listener-close', 'rejection-listener-send',
        'rejection-late-handled-send', 'rejection-late-handled-close', 'rejection-listener-throw', 'rejection-listener-handle-next']);
    assert.deepEqual(asyncBudgetKinds, ['timer-budget-loop', 'promise-budget-loop', 'message-budget-loop', 'timer-budget-fresh', 'interval-budget-fresh']);
});
test('actual error source stays at line 4 column 90 with room for real Close ACK', () => {
    const fixture = fs.readFileSync(new URL('./fixtures/workerd-async-callbacks.mjs', import.meta.url), 'utf8');
    assert.equal(fixture.split('\n')[3].indexOf('new TypeError'), 90);
    assert.match(fixture, /base === 'rejection-late-handled-close' \? 180 : 60/);
    const runner = fs.readFileSync(new URL('./test_websocket_async_callbacks.mjs', import.meta.url), 'utf8');
    assert.match(runner, /await delay\(250\)/);
});
test('stored actual observations satisfy absolute contracts and the native matrix', () => {
    const {reference, candidates, budgets, expected} = matrix();
    assert.deepEqual(compareAsyncReports(reference, candidates, budgets, expected),
        {complete: true, casesPerBackend: 24, backends: 3, budgetCasesPerBackend: 5, budgetBackends: 2});
});
const rewrite = value => {value.stateText = JSON.stringify(value.state); if (value.response) value.response.text = value.stateText;};
for (const [name, kind, mutate, diagnostic] of [
    ['worker restarts', 'timeout-throw', value => {value.state.realm = '00000000-0000-4000-8000-000000000000'; rewrite(value);}, /complete HTTP state/],
    ['callback never runs', 'timeout-throw', value => {value.state.state.callbacks = 0; rewrite(value);}, /complete HTTP state/],
    ['interval loses its second callback', 'interval-throw', value => {value.state.state.callbacks = 1; rewrite(value);}, /complete HTTP state/],
    ['self clear is ignored', 'interval-clear-throw', value => {value.state.state.callbacks = 2; rewrite(value);}, /complete HTTP state/],
    ['nested microtask is lost after timer error', 'timeout-throw-microtask', value => {value.state.state.trace.splice(2, 1); rewrite(value);}, /complete HTTP state/],
    ['sync timer incorrectly dispatches global error', 'timeout-throw', value => {value.state.state.globalEvents.push({type: 'error'}); rewrite(value);}, /complete HTTP state/],
    ['Promise rejection incorrectly dispatches error', 'promise-throw', value => {value.state.state.globalEvents[0].type = 'error'; rewrite(value);}, /complete HTTP state/],
    ['same public reportError replacement wins', 'microtask-report-override', value => {value.state.state.trace.push('replacement'); rewrite(value);}, /complete HTTP state/],
    ['wrong error identity', 'microtask-throw', value => {value.state.state.globalEvents[0].sameValue = false; rewrite(value);}, /complete HTTP state/],
    ['wrong event cancelability', 'timeout-reject', value => {value.state.state.globalEvents[0].cancelable = true; rewrite(value);}, /complete HTTP state/],
    ['wrong event trust', 'microtask-throw', value => {value.state.state.globalEvents[0].trusted = false; rewrite(value);}, /complete HTTP state/],
    ['forged source file', 'microtask-throw', value => {value.state.state.globalEvents[0].filename = 'invented'; rewrite(value);}, /complete HTTP state/],
    ['wrong source line', 'microtask-throw', value => {value.state.state.globalEvents[0].lineno = 5; rewrite(value);}, /complete HTTP state/],
    ['off by one source column', 'microtask-throw', value => {value.state.state.globalEvents[0].colno = 91; rewrite(value);}, /complete HTTP state/],
    ['primitive acquires a fake source', 'microtask-primitive', value => {value.state.state.globalEvents[0].filename = 'main'; rewrite(value);}, /complete HTTP state/],
    ['handled rejection is reported unhandled', 'rejection-handled', value => {value.state.state.trace.push('global/unhandledrejection'); rewrite(value);}, /complete HTTP state/],
    ['late handler event waits until after catch', 'rejection-late-handled-send', value => {value.state.state.trace.splice(4, 1); value.state.state.trace.splice(6, 0, 'global/rejectionhandled'); rewrite(value);}, /complete HTTP state/],
    ['late handled event leaks reason', 'rejection-late-handled-send', value => {value.state.state.globalEvents[1].errorMessage = 'leaked reason'; rewrite(value);}, /complete HTTP state/],
    ['listener failure loses following task', 'rejection-listener-throw', value => {value.state.state.trace.splice(-2, 1); rewrite(value);}, /complete HTTP state/],
    ['listener fails to handle the next pending rejection', 'rejection-listener-handle-next', value => {value.state.state.globalEvents.push({type:'unhandledrejection'}); rewrite(value);}, /complete HTTP state/],
    ['listener cannot close its original socket', 'rejection-listener-close', value => {value.state.state.socketEvents[0].reason = 'done'; rewrite(value);}, /complete HTTP state/],
    ['listener close leaves a late timer active', 'microtask-error-listener-close', value => {value.state.state.trace.splice(-1, 0, 'followup'); rewrite(value);}, /complete HTTP state/],
    ['original HTTP response fails', 'http-timeout-throw', value => {value.response.status = 503;}, /awaited HTTP succeeds/],
    ['original HTTP body is missing', 'http-microtask-throw', value => {value.response.text = '{}';}, /original HTTP response/],
    ['missing listener send', 'rejection-listener-send', value => {value.wire.frames.splice(1, 1);}, /exact complete frames/],
    ['wrong challenge payload', 'timeout-throw', value => {value.wire.frames[0].payload = Buffer.from('queued:old').toString('base64');}, /exact complete frames/],
    ['truncated raw bytes', 'timeout-throw', value => {value.wire.rawTail = '';}, /exact wire bytes/],
    ['forged Upgrade challenge', 'timeout-throw', value => {value.wire.requestKey = 'a'.repeat(22) + '==';}, /Upgrade challenge/],
    ['local disposal masquerades as peer Close', 'timeout-throw', value => {value.wire.completion = 'client-disposed';}, /no deadline or local disposal/],
    ['transport never terminates', 'timeout-throw', value => {value.wire.peerTerminated = false;}, /actual peer terminal/],
    ['forced process stop', 'timeout-throw', value => {value.shutdown.forced = true;}, /cooperative shutdown/],
    ['token leak', 'timeout-throw', value => {value.stderr += 'TOKEN LEAK retained=1';}, /no runtime diagnostics/],
]) test('async callback contract rejects ' + name, () => {
    const value = structuredClone(stored.cases.find(value => value.id === kind)); mutate(value);
    assert.throws(() => assertAsyncCase(kind, value, 'reference', 'main'), diagnostic);
});
for (const [name, mutate, diagnostic] of [
    ['missing case everywhere', reports => reports.forEach(report => report.cases.pop()), /complete ordered async callback inventory/],
    ['duplicate case everywhere', reports => reports.forEach(report => report.cases[1] = structuredClone(report.cases[0])), /complete ordered async callback inventory/],
    ['same wrong flags everywhere', reports => reports.forEach(report => {report.cases[4].state.state.globalEvents[0].cancelable = true; rewrite(report.cases[4]);}), /complete HTTP state/],
    ['stale run', reports => {reports[1].runId = 'old';}, /current async callback run/],
    ['different fixture', reports => {reports[1].inputs.fixture = 'c'.repeat(64);}, /input provenance/],
    ['different Worker', reports => {reports[1].binaries.worker = 'c'.repeat(64);}, /binary provenance/],
    ['partial run', reports => {reports[1].complete = false;}, /complete async callback run/],
    ['weakened shared timeout', reports => {reports[1].requestTimeoutMs = 30000;}, /fixed async callback execution budget/],
    ['source alias', reports => {reports[1].sourceName = 'main';}, /configured async callback source name/],
]) test('async callback matrix rejects ' + name, () => {
    const m = matrix(); mutate([m.reference, ...m.candidates]);
    assert.throws(() => compareAsyncReports(m.reference, m.candidates, m.budgets, m.expected), diagnostic);
});
for (const [name, mutate, diagnostic] of [
    ['missing loop', report => {report.cases.shift();}, /complete ordered async callback inventory/],
    ['unconfigured timeout', report => {report.requestTimeoutMs = 10000;}, /fixed async callback execution budget/],
    ['loop never executes', report => {report.cases[0].elapsedMs = 1;}, /configured CPU budget actually used/],
    ['finite timer uses stale budget', report => {report.cases[3].state.state.sum = 0; rewrite(report.cases[3]);}, /complete HTTP state/],
    ['idle wait was removed', report => {report.cases[3].elapsedMs = 80;}, /idle wait exceeds execution budget/],
    ['intervals share one expired budget', report => {report.cases[4].state.state.callbacks = 1; rewrite(report.cases[4]);}, /complete HTTP state/],
    ['timer kills the whole realm', report => {report.cases[0].stateStatus = 503;}, /healthy same realm/],
    ['unbounded reference execution', report => {report.mode = 'reference';}, /budget controls require a configured product/],
]) test('async budget contract rejects ' + name, () => {
    const m = matrix(); mutate(m.budgets[0]);
    assert.throws(() => validateAsyncReport(m.budgets[0], m.expected, 'budget'), diagnostic);
});
test('both shared and budget reports require every supported backend', () => {
    const m = matrix();
    assert.throws(() => compareAsyncReports(m.reference, m.candidates.slice(0, 1), m.budgets, m.expected), /complete async callback backend matrix/);
    assert.throws(() => compareAsyncReports(m.reference, m.candidates, m.budgets.slice(0, 1), m.expected), /complete async callback backend matrix/);
});
