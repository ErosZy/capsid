import assert from 'node:assert/strict';
import test from 'node:test';
import {readFileSync} from 'node:fs';
import {dirname, join, resolve} from 'node:path';
import {fileURLToPath} from 'node:url';
import {gunzipSync, gzipSync} from 'node:zlib';
import {assertNativeReaderContextCase, loadNativeReaderContextPrimaries, nativeReaderContextFiles,
    readerContextSHA256, readerContextStable, validateNativeReaderContextArchive} from '../tools/native-reader-context-contract.mjs';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const primaries = loadNativeReaderContextPrimaries(root);
const archive = readFileSync(join(root, nativeReaderContextFiles.archive));
const program = readFileSync(join(root, nativeReaderContextFiles.fixture));
const sampleBytes = readFileSync(join(root, nativeReaderContextFiles.controls));
assert.equal(primaries.lock.nativeControlSamples.file, nativeReaderContextFiles.controls);
assert.equal(readerContextSHA256(sampleBytes), primaries.lock.nativeControlSamples.sha256);
const samples = JSON.parse(gunzipSync(sampleBytes));
assert.equal(samples.schema, 'capsid-native-reader-context-control-samples-v1');
assert.equal(samples.cases.length, 16);
const clone = value => structuredClone(value);
const row = operation => clone(samples.cases.find(value => value.operation === operation));
function raw(row, prefix, value) {
    row.nativeStdout = row.nativeStdout.split(/\r?\n/).map(line => line.startsWith(prefix)
        ? prefix + JSON.stringify(value) : line).join('\n');
}

test('both native reference assets and all16 real native control samples qualify', () => {
    assert.equal(primaries.primary.provenance.win32.referenceLock.binary, 'workerd-windows-64');
    assert.equal(primaries.primary.provenance.linux.referenceLock.binary, 'workerd-linux-64');
    for (const sample of samples.cases) assert.equal(assertNativeReaderContextCase(sample, primaries).cleanNaturalExit, true);
});

for (const [label, operation, mutate] of [
    ['old releaseLock error', 'read', value => {value.owner.releaseError = {name: 'TypeError', message: 'Cannot call releaseLock() on a reader with outstanding read promises.'};}],
    ['old unreleased public lock', 'read', value => {value.owner.lockedAfterRelease = true;}],
    ['lost foreign input transfer', 'read', value => {value.guest.afterIssue.capacity = 24;}],
    ['wrong validation priority', 'invalid-view', value => {value.guest.result.error.name = 'Error';}],
    ['wrong minimum getter exception', 'min-getter-error', value => {value.guest.result.error.message = 'generic';}],
    ['missing owner read', 'invalid-view', value => {value.owner.reads.pop();}],
    ['missing context rejection', 'read&retain=1', value => {value.guest.result.phase = 'fulfilled';}],
    ['extra public observation', 'read', value => {value.owner.unexpected = true;}],
    ['missing overlap barrier', 'read', value => {value.ownerEnteredReadBeforeSourceRelease = false;}],
    ['source released before guest', 'read', value => {value.sourceBeforeGuestBodyRelease[0].fixtureEnded = true;}],
    ['source terminated by fixture cleanup', 'read', value => {value.sourceBeforeNativeStop[0].fixtureDestroyed = true;}],
    ['missing actual source terminal', 'read', value => {value.sourceBeforeNativeStop[0].peerEnded = false; value.sourceBeforeNativeStop[0].peerReset = false;}],
    ['changed upstream payload byte', 'invalid-view', value => {value.sourceBeforeNativeStop[0].payload = Buffer.alloc(17).toString('base64');}],
    ['wrong response ownership', 'read', value => {value.protocol.replies[1].id = 1; raw(value, 'CONTEXT_PROTOCOL ', value.protocol);}],
    ['duplicate response terminal', 'read', value => {value.protocol.replies[0].ends = 2; raw(value, 'CONTEXT_PROTOCOL ', value.protocol);}],
    ['missing response head', 'read', value => {value.protocol.replies[0].heads = 0; raw(value, 'CONTEXT_PROTOCOL ', value.protocol);}],
    ['incorrect returned credit', 'read', value => {value.protocol.replies[0].returnedCredit++; raw(value, 'CONTEXT_PROTOCOL ', value.protocol);}],
    ['missing healthy reuse', 'read', value => {value.healthyReuse = false;}],
    ['forced helper cleanup', 'read', value => {value.helperForced = true;}],
    ['unobserved native exit', 'read', value => {value.workerExit.processObserved = false; raw(value, 'CONTEXT_EXIT ', value.workerExit);}],
    ['nonzero actual worker exit', 'read&retain=1', value => {value.workerExit.exitStatus = 256; raw(value, 'CONTEXT_EXIT ', value.workerExit);}],
    ['forced actual worker cleanup', 'read', value => {value.workerExit.forced = true; raw(value, 'CONTEXT_EXIT ', value.workerExit);}],
    ['worker error event', 'read', value => {value.workerExit.errorEvents = 1; raw(value, 'CONTEXT_EXIT ', value.workerExit);}],
    ['failed native helper', 'read', value => {value.nativeExit.code = 1;}],
    ['contradictory raw exit record', 'read', value => {raw(value, 'CONTEXT_EXIT ', {...value.workerExit, exitStatus: 1});}],
    ['duplicate raw guest record', 'read', value => {value.nativeStdout += '\nCONTEXT_GUEST ' + JSON.stringify(value.guest) + '\n';}]
]) test('rejects ' + label, () => {
    const changed = row(operation);
    assert.doesNotThrow(() => assertNativeReaderContextCase(changed, primaries));
    mutate(changed);
    assert.throws(() => assertNativeReaderContextCase(changed, primaries));
});

test('rejects corrupted primary bytes', () => {
    const changed = Buffer.from(archive); changed[changed.length - 1] ^= 1;
    assert.throws(() => validateNativeReaderContextArchive(changed, primaries.lock, program));
});
test('rejects changed program bytes', () => {
    assert.throws(() => validateNativeReaderContextArchive(archive, primaries.lock, Buffer.concat([program, Buffer.from('\n')])));
});
test('rejects a different compatibility profile', () => {
    const lock = clone(primaries.lock); lock.profile.compatibility_flags.push('some-other-policy');
    assert.throws(() => validateNativeReaderContextArchive(archive, lock, program));
});
test('rejects a different reference binary', () => {
    const lock = clone(primaries.lock); lock.referenceBinarySHA256.linux = '0'.repeat(64);
    assert.throws(() => validateNativeReaderContextArchive(archive, lock, program));
});
for (const [label, mutate] of [
    ['changed public state', value => {value.cases.read.owner.lockedAfterRelease = true;}],
    ['missing retained-reader case', value => {delete value.cases['read&retain=1'];}]
]) test('resealing archive bytes cannot conceal ' + label, () => {
    const primary = clone(primaries.primary); mutate(primary);
    const raw = Buffer.from(readerContextStable(primary) + '\n');
    const compressed = gzipSync(raw);
    const lock = clone(primaries.lock);
    lock.archive.sha256 = readerContextSHA256(compressed); lock.archive.uncompressedSHA256 = readerContextSHA256(raw);
    assert.throws(() => validateNativeReaderContextArchive(compressed, lock, program));
});
