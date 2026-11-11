import assert from 'node:assert/strict';
import test from 'node:test';
import {readFileSync} from 'node:fs';
import {dirname, join, resolve} from 'node:path';
import {fileURLToPath} from 'node:url';
import {gunzipSync, gzipSync} from 'node:zlib';
import {readerContextSHA256, readerContextStable} from '../tools/native-reader-context-contract.mjs';
import {assertNativeBranchGraphCase, branchGraphKey, loadBranchGraphPrimaries, nativeBranchGraphFiles,
    nativeBranchGraphPrograms, nativeBranchGraphPlans, validateBranchGraphArchive} from '../tools/native-branch-graph-contract.mjs';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const primaries = loadBranchGraphPrimaries(root);
const archive = readFileSync(join(root, nativeBranchGraphFiles.archive));
const programs = Object.fromEntries(Object.entries(nativeBranchGraphPrograms).map(([category, file]) => [category, readFileSync(join(root, file))]));
const sampleBytes = readFileSync(join(root, nativeBranchGraphFiles.controls));
assert.equal(primaries.lock.nativeControlSamples.file, nativeBranchGraphFiles.controls);
assert.equal(readerContextSHA256(sampleBytes), primaries.lock.nativeControlSamples.sha256);
const samples = JSON.parse(gunzipSync(sampleBytes));
assert.equal(samples.schema, 'capsid-native-branch-graph-control-samples-v1');
const clone = value => structuredClone(value);
const sample = (key = 'controls/gzip/pair') => clone(samples.positive.win32.find(row => branchGraphKey(row) === key));
function raw(row, prefix, value) {
    row.native.stdout = row.native.stdout.split(/\r?\n/).map(line => line.startsWith(prefix)
        ? prefix + JSON.stringify(value) : line).join('\n');
}
for (const platform of ['win32', 'linux']) {
    test(platform + ' all74 actual regenerated-worker observations qualify', () => {
        assert.deepEqual(samples.positive[platform].map(branchGraphKey).sort(), nativeBranchGraphPlans.map(branchGraphKey).sort());
        for (const row of samples.positive[platform]) assertNativeBranchGraphCase(row, primaries);
    });
    test(platform + ' all28 actual earlier timing, token and error-priority failures are rejected', () => {
        assert.equal(samples.previousFailures[platform].length, 28);
        for (const row of samples.previousFailures[platform]) {
            const positive = samples.positive[platform].find(value => branchGraphKey(value) === branchGraphKey(row));
            assert.doesNotThrow(() => assertNativeBranchGraphCase(positive, primaries));
            assert.throws(() => assertNativeBranchGraphCase(row, primaries));
        }
    });
}
for (const [label, key, mutate] of [
    ['stale writable lock', 'controls/gzip/pair', row => {row.native.guest.controls[0].liveLocks.writable = true;}],
    ['stale source lock', 'controls/gzip/pair', row => {row.native.guest.controls[0].liveLocks.source = true;}],
    ['self-cycle changed from an error to successful EOF', 'controls/raw/self-cycle', row => {row.native.guest.controls[0].read = {phase: 'fulfilled', done: true, bytes: null};}],
    ['late source error replacing the original transform error', 'priority/raw/direct~cancel-throw', row => {row.native.owner.controls[0].pipe.error.name = 'RangeError';}],
    ['pipe promise blocked on source cancellation', 'priority/raw/direct~cancel-reject-async', row => {row.native.guest.controls[0].pipe = {phase: 'unobserved'};}],
    ['missing BYOB transfer', 'held/raw/tee-drain-byob', row => {row.native.guest.jobs[0].lastInputCapacityAfterIssue = 80;}],
    ['a returned data byte changed', 'held/raw/tee-drain-default', row => {row.native.owner.jobs[1].chunks[0].bytes[2] ^= 1;}],
    ['reference URL not checked against the actual target', 'held/raw/clone-drain-default', row => {row.native.owner.before.url = 'http://localhost:1/wrong';}],
    ['second source request', 'controls/gzip/pair', row => {row.native.sourceBeforeGuestBodyRelease.push(clone(row.native.sourceBeforeGuestBodyRelease[0]));}],
    ['source suffix released before both HTTP barriers', 'controls/gzip/pair', row => {row.native.sourceBeforeGuestBodyRelease[0].fixtureEnded = true;}],
    ['source prefix changed', 'controls/gzip/pair', row => {row.native.sourceBeforeGuestBodyRelease[0].prefix = Buffer.alloc(3).toString('base64');}],
    ['source terminated while the other branch was pending', 'held/gzip/tee-cancel-first', row => {row.native.sourceBeforeGuestBodyRelease[0].peerEnded = true;}],
    ['source reclaimed by fixture cleanup', 'controls/gzip/pair', row => {row.native.sourceBeforeRuntimeStop[0].fixtureDestroyed = true;}],
    ['missing physical source terminal', 'controls/gzip/pair', row => {row.native.sourceBeforeRuntimeStop[0].peerEnded = false; row.native.sourceBeforeRuntimeStop[0].peerReset = false;}],
    ['changed actual source suffix', 'controls/gzip/pair', row => {row.native.sourceBeforeRuntimeStop[0].suffix = Buffer.alloc(14).toString('base64');}],
    ['gzip header removed', 'controls/gzip/pair', row => {
        for (const source of [row.native.sourceBeforeGuestBodyRelease[0], row.native.sourceBeforeRuntimeStop[0]])
            source.headers = Buffer.from(Buffer.from(source.headers, 'base64').toString('ascii').replace('Content-Encoding: gzip\r\n', '')).toString('base64');
    }],
    ['wrong response owner', 'controls/gzip/pair', row => {row.native.protocol.replies[1].id = 1; raw(row, 'CONTEXT_PROTOCOL ', row.native.protocol);}],
    ['duplicate response terminal', 'controls/gzip/pair', row => {row.native.protocol.replies[0].ends = 2; raw(row, 'CONTEXT_PROTOCOL ', row.native.protocol);}],
    ['incorrect returned byte credit', 'controls/gzip/pair', row => {row.native.protocol.replies[0].returnedCredit++; raw(row, 'CONTEXT_PROTOCOL ', row.native.protocol);}],
    ['missing overlap barrier', 'controls/gzip/pair', row => {row.native.stdout = row.native.stdout.replace('CONTEXT_OWNER_READ_READY true', 'CONTEXT_OWNER_READ_READY false');}],
    ['contradictory raw public result', 'controls/gzip/pair', row => {raw(row, 'CONTEXT_GUEST ', {...row.native.guest, unexpected: true});}],
    ['duplicate raw public result', 'controls/gzip/pair', row => {row.native.stdout += '\nCONTEXT_GUEST ' + JSON.stringify(row.native.guest) + '\n';}],
    ['failed native helper', 'controls/gzip/pair', row => {row.native.exit.code = 1;}],
    ['forced helper stop', 'controls/gzip/pair', row => {row.native.helperForced = true;}],
    ['missing healthy reuse', 'controls/gzip/pair', row => {row.native.healthyReuse = false;}],
    ['unobserved OS exit', 'controls/gzip/pair', row => {row.native.workerExit.processObserved = false; raw(row, 'CONTEXT_EXIT ', row.native.workerExit);}],
    ['nonzero natural process exit', 'controls/gzip/pair', row => {row.native.workerExit.exitStatus = 256; raw(row, 'CONTEXT_EXIT ', row.native.workerExit);}],
    ['forced worker cleanup', 'controls/gzip/pair', row => {row.native.workerExit.forced = true; raw(row, 'CONTEXT_EXIT ', row.native.workerExit);}],
    ['sanitizer error', 'controls/gzip/pair', row => {row.native.stderr += '\nERROR: AddressSanitizer: heap-use-after-free\n';}],
    ['terminal continuation leak', 'controls/gzip/pair', row => {row.native.stderr += '\nWORKER POISONED: terminal continuation leak\n';}],
    ['changed exact fixture', 'controls/gzip/pair', row => {row.programSHA256 = '0'.repeat(64);}]
]) test('rejects ' + label, () => {
    const row = sample(key); assert.doesNotThrow(() => assertNativeBranchGraphCase(row, primaries));
    mutate(row); assert.throws(() => assertNativeBranchGraphCase(row, primaries));
});
test('rejects changed primary compressed bytes', () => {
    const changed = Buffer.from(archive); changed[changed.length - 1] ^= 1;
    assert.throws(() => validateBranchGraphArchive(changed, primaries.lock, programs));
});
test('rejects changed exact fixture bytes', () => {
    const changed = {...programs, held: Buffer.concat([programs.held, Buffer.from('\n')])};
    assert.throws(() => validateBranchGraphArchive(archive, primaries.lock, changed));
});
for (const [label, mutate] of [
    ['compatibility profile', lock => {lock.profile.compatibility_flags.push('other-policy');}],
    ['native reference binary', lock => {lock.referenceBinarySHA256.linux = '0'.repeat(64);}],
    ['native event budget', lock => {lock.nativeEventDeadlineMilliseconds = 15000;}],
    ['native natural exit budget', lock => {lock.nativeNaturalExitDeadlineMilliseconds = 5000;}]
]) test('rejects a changed ' + label, () => {
    const lock = clone(primaries.lock); mutate(lock);
    assert.throws(() => validateBranchGraphArchive(archive, lock, programs));
});
for (const [label, mutate] of [
    ['missing branch cell', value => {delete value.cases['held/raw/tee-drain-default'];}],
    ['replaced transform error', value => {value.cases['controls/raw/self-cycle'].owner.controls[0].read = {phase: 'fulfilled', done: true, bytes: null};}]
]) test('resealing archive bytes cannot conceal ' + label, () => {
    const primary = clone(primaries.primary); mutate(primary);
    const raw = Buffer.from(readerContextStable(primary) + '\n'), compressed = gzipSync(raw), lock = clone(primaries.lock);
    lock.archive.sha256 = readerContextSHA256(compressed); lock.archive.uncompressedSHA256 = readerContextSHA256(raw);
    assert.throws(() => validateBranchGraphArchive(compressed, lock, programs));
});
