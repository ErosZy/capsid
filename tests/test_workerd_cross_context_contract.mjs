import assert from 'node:assert/strict';
import test from 'node:test';
import fs from 'node:fs';
import {spawnSync} from 'node:child_process';
import {fileURLToPath} from 'node:url';
import {crossContextCases, assertCrossContextCase, validateCrossContextReport, compareCrossContextReports} from '../tools/workerd-cross-context-contract.mjs';

// Actual Windows reference output is replayed only to test this validator.
// Platform gates must launch fresh pinned assets and bind current input hashes.
const stored = JSON.parse(fs.readFileSync(new URL('./fixtures/workerd-cross-context-report.json', import.meta.url), 'utf8'));
function matrix() {
    const reference = structuredClone(stored), expected = {runId: reference.runId, lock: reference.referenceLock,
        inputs: reference.inputs, host: 'a'.repeat(64), worker: 'b'.repeat(64)};
    const candidates = ['single-worker','static-pool'].map(mode => ({...structuredClone(reference), mode,
        binaries: {host: expected.host, worker: expected.worker}, cases: reference.cases.map(value => ({...structuredClone(value),
            shutdown: {code: 0, signal: null, forced: false}}))}));
    return {reference, candidates, expected};
}
function changeState(value, mutate, key = 'state') {
    const state = JSON.parse(value[key].text); mutate(state); value[key].text = JSON.stringify(state);
}
test('literal cross-context inventory separates creation, following, settlement and late handling', () => {
    assert.deepEqual(crossContextCases.map(value => value.id), [
        'reaction-a-settle-b','reaction-b-settle-a','reaction-b-settle-http','reaction-http-created',
        'reject-a-settle-b','reject-a-settle-http','reject-a-late-catch-b','reject-a-late-catch-http',
        'reaction-a-throw-settle-b','thenable-a-settle-b','thenable-b-settle-a','origin-closed-b-reaction',
        'origin-closed-b-thenable','origin-closed-b-reject','origin-closed-settled','module-origin-reaction-b',
        'origin-closed-resolve-then-attach','origin-http-resolve-then-attach','reaction-a-order-b',
        'reaction-b-order-a','reaction-a-order-a','reject-a-order-b','await-a-settle-b','await-b-settle-a',
        'all-b-settle-a','module-origin-await-b',
    ]);
    assert.ok(Object.isFrozen(crossContextCases) && crossContextCases.every(value =>
        Object.isFrozen(value) && Object.isFrozen(value.commands) && value.commands.every(Object.isFrozen)));
});
test('actual stored workerd observations meet independent absolute contracts', () => {
    const {reference, candidates, expected} = matrix();
    assert.deepEqual(compareCrossContextReports(reference, candidates, expected), {complete: true, casesPerBackend: 26, backends: 3});
    assert.equal(validateCrossContextReport(reference, expected).length, 26);
});
for (const spec of crossContextCases) for (const [name, mutate] of [
    ['missing final trace entry', value => changeState(value, state => state.trace.pop())],
    ['extra callback after final terminal', value => changeState(value, state => state.trace.push('late-callback'), 'finalState')],
    ['wrong realm', value => changeState(value, state => state.realm = '00000000-0000-4000-8000-000000000001')],
    ['different case challenge', value => {value.challenge = '00000000-0000-4000-8000-000000000002';}],
    ['truncated raw bytes', value => {value.wirea.rawTail = '';}],
    ['missing actual frame', value => {value.wireb.frames.pop();}],
    ['local socket disposal', value => {value.wirea.completion = 'client-disposed';}],
    ['no real Close ACK', value => {value.wireb.closeReplySent = false;}],
    ['forged Upgrade response', value => {value.upgradea.requestKey = 'A'.repeat(22) + '==';}],
    ['incomplete causal observations', value => {value.commands.find(command => command.role !== 'http').seen.unshift('forged');}],
    ['forced process stop', value => {value.shutdown.forced = true;}],
    ['sanitizer failure', value => {value.stderr += '\nWARNING: ThreadSanitizer: data race';}],
]) test(spec.id + ' rejects ' + name, () => {
    const value = structuredClone(stored.cases.find(entry => entry.id === spec.id)); mutate(value);
    assert.throws(() => assertCrossContextCase(value, 'reference'));
});
for (const spec of crossContextCases) test(spec.id + ' rejects equally wrong state on all backends', () => {
    const {reference, candidates, expected} = matrix();
    for (const report of [reference, ...candidates]) changeState(report.cases.find(value => value.id === spec.id),
        state => state.trace.push('shared-wrong-ownership'));
    assert.throws(() => compareCrossContextReports(reference, candidates, expected), /complete actual HTTP state/);
});
for (const id of ['reject-a-settle-b','reject-a-late-catch-b','reaction-a-throw-settle-b']) {
    for (const field of ['samePromise','sameReason','reasonType','trusted','cancelable','bubbles','defaultPrevented']) test(id + ' rejects wrong event ' + field, () => {
        const value = structuredClone(stored.cases.find(entry => entry.id === id));
        changeState(value, state => {state.events[0][field] = typeof state.events[0][field] === 'boolean' ? !state.events[0][field] : 'undefined';});
        assert.throws(() => assertCrossContextCase(value, 'reference'), /complete actual HTTP state/);
    });
}
for (const [name, mutate] of [
    ['missing case', reports => reports.forEach(value => value.cases.pop())],
    ['duplicate case', reports => reports.forEach(value => value.cases[1] = structuredClone(value.cases[0]))],
    ['stale run', reports => {reports[1].runId = 'old-run';}],
    ['different fixture', reports => {reports[1].inputs.fixture = 'c'.repeat(64);}],
    ['different contract', reports => {reports[1].inputs.contract = 'c'.repeat(64);}],
    ['different client', reports => {reports[1].client.node = 'v0.0.0';}],
    ['different profile', reports => {reports[1].referenceLock.compatibility_date = '2026-01-01';}],
    ['different Worker', reports => {reports[1].binaries.worker = 'c'.repeat(64);}],
    ['missing backend', reports => {reports.pop();}],
    ['replayed realm', reports => {changeState(reports[1].cases[1], state => state.realm = JSON.parse(reports[1].cases[0].initial.text).realm, 'initial');}],
    ['replayed challenge', reports => {reports[1].cases[1].challenge = reports[1].cases[0].challenge;}],
    ['partial collection', reports => {reports[1].complete = false;}],
]) test('cross-context report rejects ' + name, () => {
    const {reference, candidates, expected} = matrix(), reports = [reference, ...candidates]; mutate(reports);
    assert.throws(() => compareCrossContextReports(reports[0], reports.slice(1), expected));
});
for (const args of [
    ['--kinds','reaction-a-settle-b'], ['--mode'], ['--mode','single-worker','--mode','static-pool'],
    ['--reference-from-env','0'], ['--reference-from-env','1','--host','product'],
]) test('cross-context driver rejects inventory filtering or malformed options ' + args.join(' '), () => {
    const result = spawnSync(process.execPath, [fileURLToPath(new URL('./test_websocket_cross_context.mjs', import.meta.url)),
        ...args], {encoding: 'utf8', timeout: 5000, env: {...process.env, CAPSID_WORKERD_REF: 'unopened-reference-asset'}});
    assert.notEqual(result.status, 0); assert.notEqual(result.error?.code, 'ETIMEDOUT');
    assert.doesNotMatch(result.stderr, /MODULE_NOT_FOUND|Cannot find module/);
    assert.doesNotMatch(result.stderr, /ENOENT|CAPSID_WORKERD_REF required/);
    assert.match(result.stderr, /known cross-context option|unique valued cross-context option|CAPSID_WORKERD_REF required|AssertionError/);
});
