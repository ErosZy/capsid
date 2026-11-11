import assert from 'node:assert/strict';
import {test, after} from 'node:test';
import {spawnSync} from 'node:child_process';
import {fileURLToPath} from 'node:url';
import {readOrdinaryFetchSamples} from './helpers/ws_ordinary_fetch_sample.mjs';
import {referenceBackendModes} from '../tools/workerd-platform.mjs';
import {assertOrdinaryFetchCase, ordinaryFetchCases, ordinaryFetchShards, ordinaryFetchCasesForShard,
    validateOrdinaryFetchReport, compareOrdinaryFetchShardReports} from '../tools/workerd-ordinary-fetch-contract.mjs';

let mutationChecks = 0;
function reject(operation, ...message) {++mutationChecks; return assert.throws(operation, ...message);}
after(() => console.log(JSON.stringify({ordinaryFetchCases: 106, rejectedMutations: mutationChecks})));

function matrix() {
    const references = readOrdinaryFetchSamples(), first = references[0];
    for (const report of references) report.runId = first.runId;
    const expected = {lock: first.referenceLock, runId: first.runId, inputs: first.inputs, host: 'a'.repeat(64), worker: 'b'.repeat(64)};
    const candidates = referenceBackendModes(expected.lock).map(mode => ({mode, reports: references.map(source => {
        const report = structuredClone(source); report.mode = mode;
        report.binaries = {host: expected.host, worker: expected.worker};
        for (const value of report.cases) value.shutdown = {code: 0, signal: null, forced: false};
        return report;
    })}));
    return {references, candidates, expected};
}
const check = value => compareOrdinaryFetchShardReports(value.references, value.candidates, value.expected);
test('ordinary-fetch complete frozen reference states and all five mandatory shards', () => {
    const m = matrix();
    assert.equal(ordinaryFetchCases.length, 106); assert.equal(new Set(ordinaryFetchCases).size, 106);
    assert.deepEqual(ordinaryFetchShards.map(shard => ordinaryFetchCasesForShard(shard).length), [30, 14, 8, 30, 24]);
    assert.deepEqual(check(m), {complete: true, casesPerBackend: 106, backends: 1 + m.candidates.length,
        shards: [{shard: 'core', cases: 30}, {shard: 'pipelines', cases: 14}, {shard: 'gated', cases: 8}, {shard: 'views', cases: 30}, {shard: 'bodies', cases: 24}]});
});
for (const shard of ordinaryFetchShards) test(shard + ': reject incomplete inventory and provenance', () => {
    const report = readOrdinaryFetchSamples().find(value => value.shard === shard);
    const expected = {lock: report.referenceLock, inputs: report.inputs, runId: report.runId, shard};
    const mutations = [value => value.cases.pop(), value => value.cases.reverse(), value => value.cases.push(value.cases[0]),
        value => value.cases[1].challenge = value.cases[0].challenge,
        value => value.cases[1].initial = value.cases[0].initial,
        value => value.cases[1].upgradea.requestKey = value.cases[0].upgradea.requestKey,
        value => value.complete = false, value => value.complete = 1,
        value => value.schema = 'capsid-ordinary-fetch-diagnostic-v1', value => value.shard = 'all',
        value => value.mode = 'unknown', value => value.runId = 'wrong',
        value => value.referenceLock.compatibility_flags.push('unqualified'),
        value => value.referenceLock.source_commit = 'f'.repeat(40),
        value => value.client.node = 'v0.0.0', value => value.inputs.fixture = 'f'.repeat(64),
        value => delete value.inputs.contract, value => value.inputs.unchecked = 'f'.repeat(64),
        value => value.binaries.host = 'f'.repeat(64), value => value.binaries.worker = 'f'.repeat(64),
        value => value.ignored = true, value => delete value.shard];
    for (const mutate of mutations) {const bad = structuredClone(report); mutate(bad); reject(() => validateOrdinaryFetchReport(bad, expected));}
});
for (const sample of readOrdinaryFetchSamples().flatMap(report => report.cases)) {
    test(sample.id + ': reject changed state, schema, wire, bytes and natural cleanup', () => {
        assertOrdinaryFetchCase(sample, 'reference');
        const controls = [
            value => delete value.observed, value => value.failure = 'hidden', value => value.unknown = 1,
            value => value.targets.push(value.targets[0]), value => value.targets[0].port = 0,
            value => value.initial.status = 201, value => value.observed.status = 201,
            value => value.finalState.status = 201, value => value.shutdown.forced = true,
            value => value.shutdown.code = 7, value => value.stderr = 'ERROR: AddressSanitizer: test',
            value => value.wirea.frames.pop(), value => value.wirea.rawTail = '',
            value => value.wirea.peerTerminated = false, value => value.wirea.closeReplySent = false,
            value => value.wirea.completion = 'local-disposal', value => value.wirea.transportError = 'ETIMEDOUT',
            value => value.upgradea.status = 200, value => value.upgradea.rawHeader += '\r\nX-Unknown: yes',
            value => value.upgradea.rawHeader = value.upgradea.rawHeader.replace('Sec-WebSocket-Accept:', 'Invalid-Accept:'),
            value => value.upstream.push(value.upstream[0]), value => value.upstream[0].fixtureDestroyed = true,
            value => value.upstream[0].peerEnded = false, value => value.upstream[0].peerReset = true,
            value => value.upstream[0].events.reverse(), value => value.upstream[0].sent += 'AA==',
            value => value.upstream[0].rawRequest += 'AA==', value => value.upstream[0].bytes += 1,
            value => value.upstream[0].localAddress = '::1', value => value.upstream[0].localPort += 1,
            value => value.upstream[0].request += '\r\nUpgrade: websocket',
            value => value.beforeAction[0].sent = '', value => value.beforeAction[0].fixtureDestroyed = true,
        ];
        if (!sample.id.startsWith('ordinary-gated-')) controls.push(
            value => delete value.upstream[0].held, value => delete value.beforeAction[0].held);
        for (const mutate of controls) {
            const bad = structuredClone(sample); mutate(bad);
            reject(() => assertOrdinaryFetchCase(bad, 'reference'), sample.id + ' must reject mutation');
        }
        // Change every non-buffer leaf. For byte/backing arrays exercise first,
        // middle, last and number/boolean substitutions without quadratic scans.
        const state = JSON.parse(sample.observed.text).state, paths = [];
        function collect(value, path = []) {
            if (Array.isArray(value) && value.length && value.every(item => typeof item === 'number')) {
                for (const index of new Set([0, Math.floor(value.length / 2), value.length - 1])) paths.push([...path, index]);
            } else if (value && typeof value === 'object') {
                for (const [key, child] of Object.entries(value)) collect(child, [...path, key]);
            } else paths.push(path);
        }
        collect(state);
        for (const path of paths) {
            const bad = structuredClone(sample), document = JSON.parse(bad.observed.text);
            let parent = document.state;
            for (const key of path.slice(0, -1)) parent = parent[key];
            const key = path.at(-1), old = parent[key];
            parent[key] = typeof old === 'number' ? old === 0 ? false : old === 1 ? true : old + 1
                : typeof old === 'boolean' ? Number(old) : old === null ? 'missing' : old + '/changed';
            // Mutating both delayed observations still must fail, even if both
            // live reference and product were to share this implementation bug.
            bad.observed.text = bad.finalState.text = JSON.stringify(document);
            reject(() => assertOrdinaryFetchCase(bad, 'reference'), sample.id + ' ' + path.join('.'));
        }
        for (const key of Object.keys(state)) {
            const bad = structuredClone(sample), document = JSON.parse(bad.observed.text); delete document.state[key];
            bad.observed.text = bad.finalState.text = JSON.stringify(document);
            reject(() => assertOrdinaryFetchCase(bad, 'reference'));
        }
    });
}
test('reject equally wrong reference and every product backend', () => {
    for (const [index, shard] of ordinaryFetchShards.entries()) {
        const m = matrix();
        for (const report of [m.references[index], ...m.candidates.map(group => group.reports[index])]) {
            const value = report.cases[0], document = JSON.parse(value.observed.text);
            document.state.failure = {name: 'Error', message: 'shared wrong implementation'};
            value.observed.text = value.finalState.text = JSON.stringify(document);
        }
        reject(() => check(m), shard);
    }
});
test('reject missing, duplicated, re-ordered and aliased backends and shards', () => {
    for (const mutate of [m => m.references.pop(), m => m.references.reverse(),
        m => m.candidates.pop(), m => m.candidates.push(m.candidates[0]),
        m => m.candidates[0].reports.pop(), m => m.candidates[0].reports.reverse(),
        m => m.candidates[0].reports[0].mode = 'reference']) {
        const m = matrix(); mutate(m); reject(() => check(m));
    }
    for (const shard of [undefined, '', 'all', 'core,pipelines', '__proto__', 'unknown'])
        reject(() => ordinaryFetchCasesForShard(shard));
});
test('driver rejects missing shard, unknown filters, duplicate options and reference/product aliases', () => {
    for (const args of [[], ['--shard', 'all'], ['--shard', 'core', '--shard', 'views'],
        ['--kinds', 'ordinary-meta-fixed'], ['--reference-from-env', '2', '--shard', 'core'],
        ['--reference-from-env', '1', '--host', 'unused', '--shard', 'core']]) {
        const result = spawnSync(process.execPath, [fileURLToPath(new URL('./test_websocket_ordinary_fetch.mjs', import.meta.url)), ...args],
            {encoding: 'utf8', timeout: 5000, env: {...process.env, CAPSID_WORKERD_REF: 'unused'}});
        assert.equal(result.status, 1, 'explicit rejection, not missing dependency or timeout');
        assert.match(result.stderr, /AssertionError/); assert.doesNotMatch(result.stderr, /ERR_MODULE_NOT_FOUND|SyntaxError/);
    }
});

test('immutable expected snapshots cannot be poisoned between actual report validations', () => {
    const sample = readOrdinaryFetchSamples().find(report => report.shard === 'bodies').cases[1];
    const expected = assertOrdinaryFetchCase(sample, 'reference');
    assert.equal(Object.isFrozen(expected.reads[0].backing), true);
    assert.throws(() => {expected.reads[0].backing[0] = 255;}, TypeError);
    assertOrdinaryFetchCase(sample, 'reference');
});
