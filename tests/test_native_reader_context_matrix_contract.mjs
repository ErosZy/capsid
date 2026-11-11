import assert from 'node:assert/strict';
import test from 'node:test';
import {readFileSync} from 'node:fs';
import {dirname, join, resolve} from 'node:path';
import {fileURLToPath} from 'node:url';
import {gunzipSync, gzipSync} from 'node:zlib';
import {readerContextSHA256, readerContextStable} from '../tools/native-reader-context-contract.mjs';
import {assertReaderContextMatrixCase, loadReaderContextMatrixPrimaries, readerContextMatrixFiles,
    readerContextMatrixPlans, readerContextMatrixKey, validateReaderContextMatrixArchive}
    from '../tools/native-reader-context-matrix-contract.mjs';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const primaries = loadReaderContextMatrixPrimaries(root);
const archive = readFileSync(join(root, readerContextMatrixFiles.archive));
const program = readFileSync(join(root, readerContextMatrixFiles.fixture));
const sampleBytes = readFileSync(join(root, readerContextMatrixFiles.controls));
assert.equal(primaries.lock.nativeControlSamples.file, readerContextMatrixFiles.controls);
assert.equal(readerContextSHA256(sampleBytes), primaries.lock.nativeControlSamples.sha256);
const samples = JSON.parse(gunzipSync(sampleBytes));
assert.equal(samples.schema, 'capsid-native-reader-context-matrix-control-samples-v1');
const clone = value => structuredClone(value);
const key = 'gzip/pending/reflect/read';
const sample = () => clone(samples.positive.win32.find(row => readerContextMatrixKey(row) === key));
function raw(row, prefix, value) {
    row.nativeStdout = row.nativeStdout.split(/\r?\n/).map(line => line.startsWith(prefix)
        ? prefix + JSON.stringify(value) : line).join('\n');
}

for (const platform of ['win32', 'linux']) {
    test(platform + ' all320 actual corrected observations qualify', () => {
        const expected = readerContextMatrixPlans.map(readerContextMatrixKey).sort();
        assert.deepEqual(samples.positive[platform].map(readerContextMatrixKey).sort(), expected);
        for (const row of samples.positive[platform]) assertReaderContextMatrixCase(row, primaries);
    });
    test(platform + ' all80 actual previous pending-slot failures are rejected', () => {
        const expected = readerContextMatrixPlans.filter(plan => plan.ownerMode === 'pending' &&
            ['read', 'readAtLeast', 'read-prototype', 'read-reflect'].includes(plan.operation.split('&')[0]))
            .map(readerContextMatrixKey).sort();
        assert.equal(expected.length, 80);
        assert.deepEqual(samples.previousFailures[platform].map(readerContextMatrixKey).sort(), expected);
        for (const row of samples.previousFailures[platform]) {
            const positive = samples.positive[platform].find(value => readerContextMatrixKey(value) === readerContextMatrixKey(row));
            assert.doesNotThrow(() => assertReaderContextMatrixCase(positive, primaries));
            assert.throws(() => assertReaderContextMatrixCase(row, primaries));
        }
    });
}
for (const [label, mutate] of [
    ['foreign context error taking precedence over a pending read', row => {row.guest.result.error.name = 'Error';}],
    ['owner EOF left blocked', row => {row.owner.eof = false;}],
    ['owner data byte changed', row => {row.owner.reads[0].bytes[16] ^= 1;}],
    ['owner sentinel overwritten', row => {row.owner.reads[0].backing[0] = 0;}],
    ['owner zero EOF backing overwritten', row => {row.owner.reads[1].backing[79] = 0;}],
    ['owner initial read prematurely fulfilled', row => {row.guest.ownerInitialBefore.phase = 'fulfilled';}],
    ['owner initial read rejected by the guest', row => {row.guest.ownerInitialAfterCheckpoint.phase = 'rejected';}],
    ['guest buffer not transferred', row => {row.guest.afterIssue.capacity = 24;}],
    ['constructor prototype path changed', row => {row.guest.construction.directPrototype = true;}],
    ['missing overlap barrier', row => {row.ownerEnteredReadBeforeSourceRelease = false;}],
    ['source payload released early', row => {row.sourceBeforeGuestBodyRelease[0].fixtureEnded = true;}],
    ['pending source closed before guest checkpoint', row => {row.sourceBeforeGuestBodyRelease[0].peerReset = true;}],
    ['source stopped by fixture cleanup', row => {row.sourceBeforeNativeStop[0].fixtureDestroyed = true;}],
    ['missing actual source terminal', row => {row.sourceBeforeNativeStop[0].peerEnded = false; row.sourceBeforeNativeStop[0].peerReset = false;}],
    ['gzip changed to undecodable bytes', row => {row.sourceBeforeNativeStop[0].payload = Buffer.alloc(37).toString('base64');}],
    ['body kind selector changed', row => {row.bodyKind = 'raw';}],
    ['request route selector changed', row => {
        for (const source of [row.sourceBeforeGuestBodyRelease[0], row.sourceBeforeNativeStop[0]]) {
            source.rawRequest = Buffer.from(Buffer.from(source.rawRequest, 'base64').toString('ascii')
                .replace('readerRoute=reflect', 'readerRoute=direct')).toString('base64');
        }
    }],
    ['content encoding header omitted', row => {
        for (const source of [row.sourceBeforeGuestBodyRelease[0], row.sourceBeforeNativeStop[0]]) {
            source.headers = Buffer.from(Buffer.from(source.headers, 'base64').toString('ascii')
                .replace('Content-Encoding: gzip\r\n', '')).toString('base64');
        }
    }],
    ['response ownership changed', row => {row.protocol.replies[1].id = 1; raw(row, 'CONTEXT_PROTOCOL ', row.protocol);}],
    ['duplicate response terminal', row => {row.protocol.replies[0].ends = 2; raw(row, 'CONTEXT_PROTOCOL ', row.protocol);}],
    ['missing response head', row => {row.protocol.replies[0].heads = 0; raw(row, 'CONTEXT_PROTOCOL ', row.protocol);}],
    ['incorrect returned credit', row => {row.protocol.replies[0].returnedCredit++; raw(row, 'CONTEXT_PROTOCOL ', row.protocol);}],
    ['missing healthy reuse', row => {row.healthyReuse = false;}],
    ['forced helper cleanup', row => {row.helperForced = true;}],
    ['native helper failed', row => {row.nativeExit.code = 1;}],
    ['unobserved worker exit', row => {row.workerExit.processObserved = false; raw(row, 'CONTEXT_EXIT ', row.workerExit);}],
    ['nonzero actual worker exit', row => {row.workerExit.exitStatus = 1; raw(row, 'CONTEXT_EXIT ', row.workerExit);}],
    ['forced worker exit', row => {row.workerExit.forced = true; raw(row, 'CONTEXT_EXIT ', row.workerExit);}],
    ['worker error event', row => {row.workerExit.errorEvents = 1; raw(row, 'CONTEXT_EXIT ', row.workerExit);}],
    ['contradictory raw guest report', row => {raw(row, 'CONTEXT_GUEST ', {...row.guest, bodyKind: 'raw'});}],
    ['duplicate raw guest record', row => {row.nativeStdout += '\nCONTEXT_GUEST ' + JSON.stringify(row.guest) + '\n';}],
    ['sanitizer finding', row => {row.nativeStderr += '\nERROR: AddressSanitizer: heap-use-after-free\n';}]
]) test('rejects ' + label, () => {
    const row = sample();
    assert.doesNotThrow(() => assertReaderContextMatrixCase(row, primaries));
    mutate(row);
    assert.throws(() => assertReaderContextMatrixCase(row, primaries));
});
test('rejects corrupted primary compressed bytes', () => {
    const changed = Buffer.from(archive); changed[changed.length - 1] ^= 1;
    assert.throws(() => validateReaderContextMatrixArchive(changed, primaries.lock, program));
});
test('rejects changed exact fixture bytes', () => {
    assert.throws(() => validateReaderContextMatrixArchive(archive, primaries.lock, Buffer.concat([program, Buffer.from('\n')])));
});
test('rejects a different reference profile', () => {
    const lock = clone(primaries.lock); lock.profile.compatibility_flags.push('different-policy');
    assert.throws(() => validateReaderContextMatrixArchive(archive, lock, program));
});
test('rejects a different native reference binary', () => {
    const lock = clone(primaries.lock); lock.referenceBinarySHA256.win32 = '0'.repeat(64);
    assert.throws(() => validateReaderContextMatrixArchive(archive, lock, program));
});
for (const [label, mutate] of [
    ['missing matrix cell', value => {delete value.cases[key];}],
    ['changed exact public observation', value => {value.cases[key].owner.eof = false;}]
]) test('resealing archive bytes cannot conceal ' + label, () => {
    const primary = clone(primaries.primary); mutate(primary);
    const bytes = Buffer.from(readerContextStable(primary) + '\n');
    const compressed = gzipSync(bytes); const lock = clone(primaries.lock);
    lock.archive.sha256 = readerContextSHA256(compressed); lock.archive.uncompressedSHA256 = readerContextSHA256(bytes);
    assert.throws(() => validateReaderContextMatrixArchive(compressed, lock, program));
});
