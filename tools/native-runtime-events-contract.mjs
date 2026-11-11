import assert from 'node:assert/strict';
import {createHash} from 'node:crypto';
import {readFileSync} from 'node:fs';
import {join} from 'node:path';
import {gunzipSync} from 'node:zlib';

export const runtimeEventScenarios = Object.freeze(['baseline', 'write-fields', 'delete-fields', 'redefine-fields',
    'replace-global-constructor', 'replace-public-prototype', 'freeze-public-prototype', 'replace-intrinsics',
    'poison-inherited-reason', 'replace-global-dispatch', 'replace-weak-map-set', 'replace-weak-map-get',
    'replace-weak-map-delete', 'poison-descriptor-get', 'poison-descriptor-set', 'poison-event-init',
    'replace-public-prototype-null', 'replace-public-prototype-number']);
export const runtimeEventProfile = Object.freeze({source_commit: '8f311ae3e3cf3064983f56a6a1c8b7ee33d6b99f',
    release_tag: 'v1.20261010.1', compatibility_date: '2026-08-20', compatibility_flags: []});
export const runtimeEventFiles = Object.freeze({
    lock: 'tests/fixtures/workerd-runtime-events-primary-lock.json',
    archive: 'tests/fixtures/workerd-runtime-events-primary.json.gz',
    api: 'tests/fixtures/workerd-runtime-event-api.mjs',
    mutations: 'tests/fixtures/workerd-runtime-event-mutations.mjs'});
export const runtimeEventInputFiles = Object.freeze([...Object.values(runtimeEventFiles),
    'tools/native-runtime-events-contract.mjs', 'tests/test_native_runtime_events.mjs',
    'tests/test_native_fetch_response_limits.cc', 'src/worker_exit_status.h']);
export const runtimeEventSha256 = value => createHash('sha256').update(value).digest('hex');
export function runtimeEventStable(value) {
    if (Array.isArray(value)) return '[' + value.map(runtimeEventStable).join(',') + ']';
    if (value !== null && typeof value === 'object') return '{' + Object.keys(value).sort()
        .map(key => JSON.stringify(key) + ':' + runtimeEventStable(value[key])).join(',') + '}';
    return JSON.stringify(value);
}
const keys = value => Object.keys(value).sort();
const readonly = {enumerable: true, configurable: true, writable: false, hasValue: true,
    getterType: 'undefined', setterType: 'undefined'};
export function validateRuntimeEventArchive(compressed, lock, programBytes) {
    assert.equal(lock.schema, 'capsid-runtime-events-primary-lock-v1');
    assert.deepEqual(lock.profile, runtimeEventProfile);
    assert.equal(lock.archive.file, runtimeEventFiles.archive);
    assert.equal(runtimeEventSha256(compressed), lock.archive.sha256, 'primary gzip digest');
    const raw = gunzipSync(compressed);
    assert.equal(runtimeEventSha256(raw), lock.archive.uncompressedSha256, 'primary payload digest');
    const primary = JSON.parse(raw);
    assert.equal(primary.schema, 'capsid-runtime-events-primary-v1');
    assert.deepEqual(primary.profile, runtimeEventProfile);
    assert.equal(primary.exactObservableReportsMatchAcrossBothNativeReferenceAssets, true);
    assert.equal(primary.rawStandardWptQualificationNotClaimed, true);
    assert.deepEqual(keys(primary.programs), ['api', 'mutations']);
    assert.deepEqual(primary.programs, lock.programs);
    assert.deepEqual(keys(primary.provenance), ['linux', 'win32']);
    assert.deepEqual(keys(lock.referenceBinarySha256), ['linux', 'win32']);
    for (const platform of ['linux', 'win32']) {
        const reference = primary.provenance[platform].referenceLock;
        assert.deepEqual(Object.fromEntries(keys(runtimeEventProfile).map(key => [key, reference[key]])), runtimeEventProfile);
        assert.match(lock.referenceBinarySha256[platform], /^[a-f0-9]{64}$/);
        assert.equal(reference.binary_sha256, lock.referenceBinarySha256[platform]);
    }
    for (const kind of ['api', 'mutations']) {
        assert.equal(lock.programs[kind].file, runtimeEventFiles[kind]);
        assert.equal(runtimeEventSha256(programBytes[kind]), lock.programs[kind].sha256, kind + ' exact program bytes');
    }
    assert.equal(lock.nativeEventDeadlineMilliseconds, 5000);
    assert.equal(lock.nativeNaturalExitDeadlineMilliseconds, 2500);
    assert.deepEqual(lock.unresolvedApiConstructorIntrospectionFields, ['globalDescriptor', 'prototypeSymbols']);
    assert.deepEqual(keys(primary.reports), ['api', 'mutations']);
    assert.equal(runtimeEventSha256(runtimeEventStable(primary.reports.api)), lock.reportsSha256.api, 'API report digest');
    assert.equal(primary.reports.api.listenerResults.length, 66);
    assert.equal(primary.reports.api.promises.length, 3);
    assert.equal(primary.reports.api.constructorShape.attempts.length, 8);
    for (const attempt of primary.reports.api.constructorShape.attempts) {
        assert.equal(attempt.error.name, 'TypeError');
        assert.equal(attempt.error.message, 'Illegal constructor');
    }
    assert.deepEqual(keys(primary.reports.mutations), [...runtimeEventScenarios].sort());
    assert.deepEqual(keys(lock.reportsSha256.mutations), [...runtimeEventScenarios].sort());
    for (const scenario of runtimeEventScenarios) {
        const report = primary.reports.mutations[scenario];
        // getOwnPropertyDescriptor returns an ordinary descriptor object. The
        // probe deliberately observes its inherited get/set in these two cases.
        const observedDescriptor = {...readonly,
            getterType: scenario === 'poison-descriptor-get' ? 'function' : 'undefined',
            setterType: scenario === 'poison-descriptor-set' ? 'function' : 'undefined'};
        assert.equal(report.scenario, scenario);
        assert.equal(runtimeEventSha256(runtimeEventStable(report)), lock.reportsSha256.mutations[scenario], scenario + ' primary report digest');
        assert.equal(report.events.length, 2);
        for (const [index, event] of report.events.entries()) {
            assert.deepEqual(event.before.ownNames, ['promise', 'reason']);
            assert.deepEqual(event.before.promiseDescriptor, observedDescriptor);
            assert.deepEqual(event.before.reasonDescriptor, observedDescriptor);
            assert.equal(event.before.promiseMatches, true);
            assert.equal(event.before.reasonMatches, index === 0);
            assert.equal(event.before.reasonType, index === 0 ? 'object' : 'undefined');
        }
    }
    return {primary, lock};
}
export function loadRuntimeEventPrimaries(root) {
    const lock = JSON.parse(readFileSync(join(root, runtimeEventFiles.lock), 'utf8'));
    return validateRuntimeEventArchive(readFileSync(join(root, runtimeEventFiles.archive)), lock,
        Object.fromEntries(['api', 'mutations'].map(kind => [kind, readFileSync(join(root, runtimeEventFiles[kind]))])));
}
export function compareRuntimeEventReport(actual, kind, scenario, primaries) {
    assert.ok(kind === 'api' || kind === 'mutations');
    if (kind === 'mutations') {
        assert.ok(runtimeEventScenarios.includes(scenario));
        assert.deepEqual(actual, primaries.primary.reports.mutations[scenario], scenario + ' exact public observation');
        return {exactMatch: true, unresolvedConstructorIntrospection: {}};
    }
    const expected = primaries.primary.reports.api;
    assert.deepEqual(keys(actual), keys(expected));
    for (const key of keys(expected).filter(key => key !== 'constructorShape')) assert.deepEqual(actual[key], expected[key], key);
    assert.deepEqual(keys(actual.constructorShape), keys(expected.constructorShape));
    const unresolved = {};
    for (const key of keys(expected.constructorShape)) {
        if (primaries.lock.unresolvedApiConstructorIntrospectionFields.includes(key)) {
            if (runtimeEventStable(actual.constructorShape[key]) !== runtimeEventStable(expected.constructorShape[key])) {
                unresolved[key] = {native: actual.constructorShape[key], reference: expected.constructorShape[key]};
            }
        } else assert.deepEqual(actual.constructorShape[key], expected.constructorShape[key], 'constructor.' + key);
    }
    return {exactMatch: Object.keys(unresolved).length === 0, unresolvedConstructorIntrospection: unresolved};
}
