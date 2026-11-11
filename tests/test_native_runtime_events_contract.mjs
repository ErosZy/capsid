import assert from 'node:assert/strict';
import test from 'node:test';
import {readFileSync} from 'node:fs';
import {resolve, dirname, join} from 'node:path';
import {fileURLToPath} from 'node:url';
import {gzipSync} from 'node:zlib';
import {compareRuntimeEventReport, loadRuntimeEventPrimaries, runtimeEventFiles, runtimeEventScenarios,
    runtimeEventSha256, runtimeEventStable, validateRuntimeEventArchive} from '../tools/native-runtime-events-contract.mjs';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const primaries = loadRuntimeEventPrimaries(root);
const compressed = readFileSync(join(root, runtimeEventFiles.archive));
const programs = Object.fromEntries(['api', 'mutations'].map(kind => [kind, readFileSync(join(root, runtimeEventFiles[kind]))]));
const clone = value => structuredClone(value);
test('both native reference assets attest the same complete primary observations', () => {
    assert.equal(primaries.primary.provenance.linux.referenceLock.binary, 'workerd-linux-64');
    assert.equal(primaries.primary.provenance.win32.referenceLock.binary, 'workerd-windows-64');
    assert.equal(primaries.primary.reports.api.listenerResults.length, 66);
    assert.equal(runtimeEventScenarios.length, 18);
    assert.equal(compareRuntimeEventReport(primaries.primary.reports.api, 'api', 'api', primaries).exactMatch, true);
    for (const scenario of runtimeEventScenarios) assert.equal(compareRuntimeEventReport(
        primaries.primary.reports.mutations[scenario], 'mutations', scenario, primaries).exactMatch, true);
});
test('the two recorded constructor reflection gaps remain visible in the result', () => {
    const value = clone(primaries.primary.reports.api);
    value.constructorShape.globalDescriptor = {nativeBinding: true};
    value.constructorShape.prototypeSymbols = ['Symbol(Symbol.toStringTag)'];
    const result = compareRuntimeEventReport(value, 'api', 'api', primaries);
    assert.equal(result.exactMatch, false);
    assert.deepEqual(Object.keys(result.unresolvedConstructorIntrospection).sort(), ['globalDescriptor', 'prototypeSymbols']);
});
for (const [label, mutate] of [
    ['mutable promise data', value => {value.events[0].before.promiseDescriptor.writable = true;}],
    ['wrong rejectionhandled reason', value => {value.events[1].before.reasonMatches = true;}],
    ['wrong promise identity', value => {value.events[0].before.promiseMatches = false;}],
    ['lost native event', value => {value.events.pop();}],
    ['unexpected native output', value => {value.extra = true;}],
    ['missing own field', value => {value.events[0].before.ownNames.pop();}]
]) test('rejects ' + label, () => {
    const value = clone(primaries.primary.reports.mutations.baseline); mutate(value);
    assert.throws(() => compareRuntimeEventReport(value, 'mutations', 'baseline', primaries));
});
for (const [label, mutate] of [
    ['constructible public constructor', value => {delete value.constructorShape.attempts[0].error;}],
    ['wrong listener error', value => {value.listenerResults.find(row => row.error).error.message = 'generic error';}],
    ['wrong option getter order', value => {value.listenerResults.find(row => row.trace.length > 1).trace.reverse();}],
    ['wrong public constructor length', value => {value.constructorShape.length = 2;}]
]) test('rejects ' + label, () => {
    const value = clone(primaries.primary.reports.api); mutate(value);
    assert.throws(() => compareRuntimeEventReport(value, 'api', 'api', primaries));
});
test('rejects corrupted compressed primary bytes', () => {
    const changed = Buffer.from(compressed); changed[changed.length - 1] ^= 1;
    assert.throws(() => validateRuntimeEventArchive(changed, primaries.lock, programs));
});
test('rejects changed program bytes', () => {
    assert.throws(() => validateRuntimeEventArchive(compressed, primaries.lock,
        {...programs, mutations: Buffer.concat([programs.mutations, Buffer.from('\n')])}));
});
test('rejects a different compatibility profile', () => {
    const lock = clone(primaries.lock); lock.profile.compatibility_flags = ['capture-enabled'];
    assert.throws(() => validateRuntimeEventArchive(compressed, lock, programs));
});
test('resealing archive bytes cannot conceal a changed individual observation', () => {
    const changed = clone(primaries.primary);
    changed.reports.mutations.baseline.events[1].before.reasonType = 'object';
    const raw = Buffer.from(runtimeEventStable(changed) + '\n');
    const archive = gzipSync(raw);
    const lock = clone(primaries.lock);
    lock.archive.sha256 = runtimeEventSha256(archive); lock.archive.uncompressedSha256 = runtimeEventSha256(raw);
    assert.throws(() => validateRuntimeEventArchive(archive, lock, programs));
});
test('an omitted scenario is rejected even if the archive digests are resealed', () => {
    const changed = clone(primaries.primary); delete changed.reports.mutations['replace-public-prototype-null'];
    const raw = Buffer.from(runtimeEventStable(changed) + '\n'); const archive = gzipSync(raw);
    const lock = clone(primaries.lock);
    lock.archive.sha256 = runtimeEventSha256(archive); lock.archive.uncompressedSha256 = runtimeEventSha256(raw);
    assert.throws(() => validateRuntimeEventArchive(archive, lock, programs));
});
