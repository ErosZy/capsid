import assert from 'node:assert/strict';
import {test} from 'node:test';
import {readNativeReaderLifecycleSample} from './helpers/ws_native_reader_lifecycle_sample.mjs';
import {nativeReaderLifecycleCases, assertNativeReaderLifecycleCase, validateNativeReaderLifecycleReport,
    compareNativeReaderLifecycleReports} from '../tools/workerd-native-reader-lifecycle-contract.mjs';
import {referenceBackendModes} from '../tools/workerd-platform.mjs';

const sample = readNativeReaderLifecycleSample();
const expected = {runId: sample.runId, lock: sample.referenceLock, inputs: sample.inputs,
    host: '1'.repeat(64), worker: '2'.repeat(64)};
const originalInventory = ['getReader', 'direct', 'prototype', 'subclass', 'reflect'].flatMap(route =>
    ['default', 'byob'].flatMap(reader => ['concurrent', 'release-pending', 'release-open', 'release-eof']
        .map(scenario => ['native', route, reader, scenario].join('|'))));
const inventory = [...originalInventory, ...['native','standard'].flatMap(source =>
    ['getReader','direct','prototype','subclass','reflect'].flatMap(route => ['default','byob'].flatMap(reader =>
        ['concurrent','release-pending','release-open','release-eof','intrinsics'].map(scenario =>
            [source,route,reader,scenario].join('|'))))).filter(kind => !originalInventory.includes(kind))];
test('one hundred explicit source, construction, reader and lifecycle combinations', () => assert.deepEqual(nativeReaderLifecycleCases, inventory));
test('actual pinned reference passes the complete independent model', () => assert.equal(validateNativeReaderLifecycleReport(sample, expected).length, 100));
for (const row of sample.cases) {
    const [source, , reader, scenario] = row.kind.split('|'), native = source === 'native';
    test('actual reference ' + row.kind, () => assertNativeReaderLifecycleCase(row, 'reference'));
    const changes = [
        ['closed identity on repeated access', state => {state.closedBeforeStable = false;}],
        ['retained final lock', state => {state.locked = true;}],
        ['body never disturbed', state => {state.bodyUsed = false;}],
        ['application intrinsic called', state => state.traps.push('WeakMap.get')],
        ['cancel never settled', state => {state.cancel.settled = false;}],
        ['unexpected error', state => {state.unexpected = {name: 'Error', message: 'failure'};}],
        ['incorrect error identity', state => {state.errorIdentity.firstClosed = !state.errorIdentity.firstClosed;}],
    ];
    if (scenario.startsWith('release-')) changes.push(
        ['closed identity after release', state => {state.closedIdentityRetained = false;}],
        ['closed identity after repeated release getter', state => {state.closedAfterStable = false;}],
        ['incorrect release lock state', state => {state.lockedAfterReleaseAttempt = !state.lockedAfterReleaseAttempt;}]);
    if (scenario === 'release-pending' && native) changes.push(
        ['pending release accepted', state => {state.release = {returnedUndefined: true};}],
        ['wrong pending release diagnostic', state => {state.release.error.message = 'Reader was released';}]);
    if (scenario === 'release-pending' && !native) changes.push(
        ['standard pending release rejected', state => {state.release = {error: {name:'TypeError'}};}],
        ['standard pending release loses shared error', state => {state.errorIdentity.firstClosed = false;}]);
    if (scenario === 'release-open') changes.push(
        ['wrong released closed diagnostic', state => {state.closedBefore.result.error.message = 'Reader was released';}],
        ['retry lost bytes', state => state.retry.result.value.bytes.pop()]);
    if (scenario === 'release-eof') changes.push(
        ['released closed rejects after EOF', state => {state.closedAfter.result = {error: {name: 'TypeError'}};}],
        ['replacement reader loses EOF', state => {state.retry.result.value.done = false;}]);
    if (scenario === 'concurrent' || scenario === 'release-pending') changes.push(
        ['pending read already settled', state => {state.beforeAction.first = true;}],
        ['pending cancellation has wrong result', state => {state.first.result = {error:{name:'WrongError'}};}],
        ['prime data lost', state => state.prime.result.value.bytes.pop()]);
    if (scenario === 'concurrent') changes.push(
        ['second read wrong pending state', state => {state.beforeAction.second = !state.beforeAction.second;}],
        ['second read wrong result', state => {state.second.result = {error:{name:'WrongError'}};}]);
    if (scenario === 'intrinsics') changes.push(
        ['intrinsic corrupts closed', state => {state.closedBefore.result = {error:{name:'Error'}};}],
        ['closed getter unstable during intrinsic replacement', state => {state.closedDuringStable = false;}],
        ['wrong intrinsic trap sequence', state => {state.traps = native ? [] : ['Promise.then'];}]);
    if (reader === 'byob') changes.push(['input not transferred', state => {
        state[(scenario === 'intrinsics' ? 'data' : scenario === 'concurrent' ? 'second' : scenario === 'release-pending' ? 'first' : 'retry') + 'Input'].after = 32;
    }]);
    for (const [name, mutate] of changes) test('reject ' + name + ' ' + row.kind, () => {
        const mutant = structuredClone(row), document = JSON.parse(mutant.observed.text);
        mutate(document.state); mutant.observed.text = JSON.stringify(document);
        assert.throws(() => assertNativeReaderLifecycleCase(mutant, 'reference'));
    });
    for (const [name, mutate] of [
        ['worker exit', value => {value.observed.status = 503;}],
        ['different worker realm', value => {const document = JSON.parse(value.observed.text);
            document.realm = '11111111-1111-4111-8111-111111111111'; value.observed.text = JSON.stringify(document);} ],
        ['missing downstream close', value => value.wire.frames.pop()],
        ['changed raw wire', value => {value.wire.rawTail = '';}],
        ['forced cleanup', value => {value.shutdown.forced = true;}],
        ['source remains open', value => {value.allSourcesEndedBeforeStop = false;}],
        ['source completed only after shutdown', value => {
            if(native)value.upstreamAfterStop.tcpRecords[0].closed = false;
            else value.upstreamAfterStop.tcpRecords.push({closed:false});
        }],
        ['incorrect actual upstream bytes', value => {
            if(native)value.upstreamBeforeStop.records[0].sentChunks.pop();
            else value.upstreamBeforeStop.records.push({request:'unexpected network I/O'});
        }],
        ['runtime diagnostic', value => {value.stderr += '\nWORKER POISONED';}],
    ]) test('reject transport and cleanup ' + name + ' ' + row.kind, () => {
        const mutant = structuredClone(row); mutate(mutant);
        assert.throws(() => assertNativeReaderLifecycleCase(mutant, 'reference'));
    });
}
test('report inputs, binaries, client, completion and full case inventory are mandatory', () => {
    for (const mutate of [report => {report.complete = false;}, report => report.cases.pop(),
        report => {report.inputs.fixture = '0'.repeat(64);}, report => {report.binaries.host = '0'.repeat(64);},
        report => {report.client.node = 'v0.0.0';}, report => {report.cases[1].challenge = report.cases[0].challenge;},
        report => {report.cases[1].initial.text = report.cases[0].initial.text;}]) {
        const mutant = structuredClone(sample); mutate(mutant);
        assert.throws(() => validateNativeReaderLifecycleReport(mutant, expected));
    }
});
test('an identical wrong lifecycle on all backends and the reference is rejected', () => {
    const candidates = referenceBackendModes(expected.lock).map(mode => ({...structuredClone(sample), mode,
        binaries: {host: expected.host, worker: expected.worker},
        cases: sample.cases.map(row => ({...structuredClone(row), shutdown: {code: 0, signal: null, forced: false}}))}));
    assert.deepEqual(compareNativeReaderLifecycleReports(sample, candidates, expected),
        {complete: true, casesPerBackend: 100, backends: candidates.length});
    const reference = structuredClone(sample), changed = structuredClone(candidates);
    for (const report of [reference, ...changed]) {
        const row = report.cases.find(row => row.kind.endsWith('|release-pending'));
        const document = JSON.parse(row.observed.text); document.state.release = {returnedUndefined: true};
        row.observed.text = JSON.stringify(document);
    }
    assert.throws(() => compareNativeReaderLifecycleReports(reference, changed, expected));
    assert.throws(() => compareNativeReaderLifecycleReports(sample, candidates.slice(1), expected));
});
