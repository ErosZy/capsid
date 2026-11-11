import assert from 'node:assert/strict';
import fs from 'node:fs';
import {test} from 'node:test';
import {createHash} from 'node:crypto';
import {fileURLToPath} from 'node:url';
import {nativeFetchArgumentKinds as kinds, nativeFetchArgumentRoutes as routes, nativeFetchArgumentViews as views,
    nativeFetchArgumentMethods as methods, nativeFetchArgumentInputPaths, nativeFetchArgumentPacket,
    expectedNativeFetchArgumentState, nativeFetchArgumentPrimary, assertNativeFetchArgumentCase,
    assertNativeFetchArgumentSources, validateNativeFetchArgumentReport} from '../tools/native-fetch-arguments-contract.mjs';

const hash = bytes => createHash('sha256').update(bytes).digest('hex');
test('every measured source input exists in the repository', () => {
    for (const name of Object.values(nativeFetchArgumentInputPaths)) {
        assert.equal(fs.statSync(fileURLToPath(new URL('../' + name, import.meta.url))).isFile(), true, name);
    }
});
function sample(kind, route, view, method, platform = 'native') {
    const state = structuredClone(expectedNativeFetchArgumentState(kind, route, view, method));
    const primary = nativeFetchArgumentPrimary(platform, kind, route, view, method);
    const port = 12345, raw = Buffer.from('GET /body HTTP/1.1\r\nHost: localhost:' + port + '\r\n\r\n').toString('base64');
    const records = state.rows.map((_, index) => ({rawRequest: raw, localAddress: '::1', remoteAddress: '::1',
        peerEnded: true, peerReset: false, fixtureEnded: true, fixtureDestroyed: false,
        events: ['request', 'source-complete', 'peer-ended'].map((name, offset) => ({name, sequence: index * 3 + offset}))}));
    const workerExit = {clean: true, processObserved: true, exitStatus: 0, forced: false, errorEvents: 0};
    return {bodyKind: kind, route, view, method, port, state,
        primary: {archiveSHA256: primary.archiveSHA256, reportSHA256: primary.reportSHA256},
        packetSHA256: hash(nativeFetchArgumentPacket(kind)), sourcesBeforeStop: records,
        finalSources: structuredClone(records), child: {code: 0, signal: null}, workerExit,
        stdout: 'LIMIT_PROBE ' + JSON.stringify(state) + '\nLIMIT_EXIT ' + JSON.stringify(workerExit) + '\n', stderr: ''};
}
for (const kind of kinds) for (const route of routes) test(kind + '/' + route + ': every view and native-state/resource counterexample', () => {
    let observations = 0;
    for (const view of views) for (const method of methods) {
        const value = sample(kind, route, view, method);
        observations += assertNativeFetchArgumentCase(value, 'native', kind, route, view, method);
        assertNativeFetchArgumentCase(sample(kind, route, view, method, 'linux'), 'linux', kind, route, view, method);
        for (const mutate of [
            bad => {bad.state.rows[0].afterCall.bufferBytes ^= 1;},
            bad => {bad.state.rows[0].construction.directPrototype = !bad.state.rows[0].construction.directPrototype;},
            bad => {bad.state.rows[0].events.push('unexpected-coercion');},
            bad => {bad.state.rows[0].eof.backing[0] = 255;},
            bad => {bad.workerExit.forced = true;},
            bad => {bad.workerExit.processObserved = false;},
            bad => {bad.child.code = 1;},
            bad => {bad.sourcesBeforeStop[0].peerEnded = false; bad.finalSources[0].peerEnded = false;},
            bad => {bad.sourcesBeforeStop.pop(); bad.finalSources.pop();},
            bad => {bad.sourcesBeforeStop[0].fixtureDestroyed = true; bad.finalSources[0].fixtureDestroyed = true;},
            bad => {bad.primary.reportSHA256 = '0'.repeat(64);},
            bad => {bad.packetSHA256 = '0'.repeat(64);},
            bad => {bad.stderr = 'AddressSanitizer: heap-buffer-overflow';},
        ]) {
            const bad = structuredClone(value); mutate(bad);
            assert.throws(() => assertNativeFetchArgumentCase(bad, 'native', kind, route, view, method));
        }
    }
    assert.equal(observations, 1092);
});
test('source evidence rejects duplicate event identities, reorder and routed-channel requests', () => {
    const value = sample('raw', 'getReader', 'Uint8Array', 'read');
    for (const mutate of [
        rows => {rows[1].events[0].sequence = rows[0].events[0].sequence;},
        rows => {rows[0].events.reverse();},
        rows => {rows[0].rawRequest = Buffer.from('GET /body HTTP/1.1\r\nHost: localhost:12345\r\nUpgrade: websocket\r\n\r\n').toString('base64');},
        rows => {rows[0].remoteAddress = '203.0.113.1';},
    ]) {
        const rows = structuredClone(value.sourcesBeforeStop); mutate(rows);
        assert.throws(() => assertNativeFetchArgumentSources(rows, value.state.rows.length, 12345));
    }
});
test('complete report checks every constructor/view/API selection afresh', () => {
    const inputs = Object.fromEntries(Object.keys(nativeFetchArgumentInputPaths).map(key => [key, 'a'.repeat(64)]));
    const expected = {runId: 'native-arguments-controls', bodyKind: 'gzip', route: 'reflect', referencePlatform: 'native',
        inputs, binaries: {worker: 'b'.repeat(64), helper: 'c'.repeat(64)}};
    const report = {schema: 'capsid-native-fetch-arguments-v1', complete: true, ...expected,
        cases: views.flatMap(view => methods.map(method => sample('gzip', 'reflect', view, method)))};
    assert.deepEqual(validateNativeFetchArgumentReport(report, expected), {complete: true, groups: 26, nestedObservations: 1092});
    for (const mutate of [
        bad => {bad.complete = false;},
        bad => {bad.cases.pop();},
        bad => {bad.cases.reverse();},
        bad => {bad.inputs.fixture = 'd'.repeat(64);},
        bad => {bad.binaries.helper = 'd'.repeat(64);},
        bad => {bad.cases[0].state.rows[0].bodyUsed = false;},
    ]) {
        const bad = structuredClone(report); mutate(bad);
        assert.throws(() => validateNativeFetchArgumentReport(bad, expected));
    }
    report.cases[0].state.rows[0].eof.done = false;
    assert.throws(() => validateNativeFetchArgumentReport(report, expected));
});
test('returned primary states have independent mutable object graphs', () => {
    const first = nativeFetchArgumentPrimary('native', 'raw', 'getReader', 'Uint8Array', 'read');
    first.state.rows[0].eof.backing[0] = 91;
    const next = nativeFetchArgumentPrimary('native', 'raw', 'getReader', 'Uint8Array', 'read');
    assert.equal(next.state.rows[0].eof.backing[0], 0);
    assert.notEqual(first.state, next.state);
});
