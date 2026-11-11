import assert from 'node:assert/strict';
import {createHash} from 'node:crypto';
import {readFileSync} from 'node:fs';
import {join} from 'node:path';
import {gunzipSync} from 'node:zlib';

const operations = ['read', 'readAtLeast', 'invalid-view', 'detached-view', 'wrong-receiver',
    'min-getter-error', 'read-prototype', 'read-reflect'];
export const nativeReaderContextOperations = Object.freeze([...operations, ...operations.map(value => value + '&retain=1')]);
export const nativeReaderContextFiles = Object.freeze({
    fixture: 'tests/fixtures/workerd-native-reader-context.mjs',
    archive: 'tests/fixtures/workerd-native-reader-context-primary.json.gz',
    lock: 'tests/fixtures/workerd-native-reader-context-primary-lock.json',
    controls: 'tests/fixtures/native-reader-context-oracle-control.json.gz'});
export const nativeReaderContextProfile = Object.freeze({source_commit: '8f311ae3e3cf3064983f56a6a1c8b7ee33d6b99f',
    release_tag: 'v1.20261010.1', compatibility_date: '2026-08-20', compatibility_flags: []});
export const nativeReaderContextBinarySHA256 = Object.freeze({
    win32: 'c8799724c4221af61dde76ed071aa5996b25c9bd5d247da94c166718cabe612a',
    linux: '50f31156a9a4c7d549f1b4753dd61eb527e1b6b0cd416e5ba3d02d64dfdee5dd'});
export const readerContextSHA256 = value => createHash('sha256').update(value).digest('hex');
export function readerContextStable(value) {
    if (Array.isArray(value)) return '[' + value.map(readerContextStable).join(',') + ']';
    if (value !== null && typeof value === 'object') return '{' + Object.keys(value).sort()
        .map(key => JSON.stringify(key) + ':' + readerContextStable(value[key])).join(',') + '}';
    return JSON.stringify(value);
}
export function validateNativeReaderContextArchive(compressed, lock, program) {
    assert.equal(lock.schema, 'capsid-native-reader-context-primary-lock-v1');
    assert.deepEqual(lock.profile, nativeReaderContextProfile);
    assert.deepEqual(lock.referenceBinarySHA256, nativeReaderContextBinarySHA256);
    assert.equal(lock.program.file, nativeReaderContextFiles.fixture);
    assert.equal(readerContextSHA256(program), lock.program.sha256, 'exact pure ES fixture');
    assert.equal(lock.archive.file, nativeReaderContextFiles.archive);
    assert.equal(readerContextSHA256(compressed), lock.archive.sha256, 'primary compressed digest');
    const raw = gunzipSync(compressed);
    assert.equal(readerContextSHA256(raw), lock.archive.uncompressedSHA256, 'primary payload digest');
    const primary = JSON.parse(raw);
    assert.equal(primary.schema, 'capsid-native-reader-context-primary-v1');
    assert.deepEqual(primary.profile, nativeReaderContextProfile);
    assert.deepEqual(primary.program, lock.program);
    assert.equal(primary.exactPublicReportsMatchAcrossBothNativeReferenceAssets, true);
    assert.deepEqual(Object.keys(primary.provenance).sort(), ['linux', 'win32']);
    for (const platform of ['linux', 'win32']) {
        const row = primary.provenance[platform];
        assert.equal(row.referenceLock.binary_sha256, nativeReaderContextBinarySHA256[platform]);
        assert.deepEqual(Object.fromEntries(Object.keys(nativeReaderContextProfile).map(key => [key, row.referenceLock[key]])), nativeReaderContextProfile);
        assert.match(row.evidenceSHA256, /^[a-f0-9]{64}$/);
        assert.match(row.runnerSHA256, /^[a-f0-9]{64}$/);
    }
    assert.equal(lock.nativeEventDeadlineMilliseconds, 5000);
    assert.equal(lock.nativeNaturalExitDeadlineMilliseconds, 2500);
    assert.equal(lock.requestsPerRuntime, 3);
    assert.deepEqual(lock.protocol, {wire_version: 4, capabilities: 123, max_channels: 1});
    const expectedNames = [...nativeReaderContextOperations].sort();
    assert.deepEqual(Object.keys(primary.cases).sort(), expectedNames);
    assert.deepEqual(Object.keys(lock.casesSHA256).sort(), expectedNames);
    for (const operation of nativeReaderContextOperations) {
        const state = primary.cases[operation];
        assert.equal(readerContextSHA256(readerContextStable(state)), lock.casesSHA256[operation], operation + ' primary observation');
        assert.deepEqual(Object.keys(state).sort(), ['guest', 'owner']);
        assert.equal(state.guest.operation, operation.split('&')[0]);
        assert.equal(state.owner.releaseAttempted, !operation.includes('&retain=1'));
        assert.equal(state.owner.lockedAfterRelease, operation.includes('&retain=1'));
        assert.equal(state.owner.releaseError, undefined);
    }
    return {primary, lock};
}
export function loadNativeReaderContextPrimaries(root) {
    return validateNativeReaderContextArchive(readFileSync(join(root, nativeReaderContextFiles.archive)),
        JSON.parse(readFileSync(join(root, nativeReaderContextFiles.lock), 'utf8')),
        readFileSync(join(root, nativeReaderContextFiles.fixture)));
}
function oneJsonLine(stdout, prefix) {
    const lines = stdout.split(/\r?\n/).filter(line => line.startsWith(prefix));
    assert.equal(lines.length, 1, 'one actual ' + prefix + ' record');
    return JSON.parse(lines[0].slice(prefix.length));
}
export function assertNativeReaderContextCase(row, primaries) {
    assert.ok(nativeReaderContextOperations.includes(row.operation));
    assert.equal(row.collectionFailure, undefined, row.collectionFailure);
    assert.ok(!row.helperForced, 'helper must not be force stopped');
    assert.equal(row.ownerReadyBeforeGuest, true);
    assert.equal(row.ownerEnteredReadBeforeSourceRelease, true);
    assert.deepEqual(row.nativeExit, {code: 0, signal: null});
    const state = {guest: row.guest, owner: row.owner};
    assert.deepEqual(state, primaries.primary.cases[row.operation], row.operation + ' exact public state');
    assert.deepEqual(oneJsonLine(row.nativeStdout, 'CONTEXT_GUEST '), state.guest);
    assert.deepEqual(oneJsonLine(row.nativeStdout, 'CONTEXT_OWNER '), state.owner);
    assert.equal(row.nativeStdout.split(/\r?\n/).filter(line => line === 'CONTEXT_OWNER_READ_READY true').length, 1);
    const protocol = oneJsonLine(row.nativeStdout, 'CONTEXT_PROTOCOL ');
    assert.deepEqual(protocol, row.protocol);
    assert.equal(protocol.requests, 3);
    assert.deepEqual(protocol.replies.map(reply => reply.id), [1, 2, 3]);
    for (const reply of protocol.replies) {
        assert.equal(reply.heads, 1); assert.equal(reply.ends, 1);
        assert.ok(Number.isSafeInteger(reply.bodyFrames) && reply.bodyFrames > 0);
        assert.ok(Number.isSafeInteger(reply.bodyBytes) && reply.bodyBytes > 0);
        assert.equal(reply.bodyBytes, reply.returnedCredit, 'return exactly consumed native bytes');
    }
    const exit = oneJsonLine(row.nativeStdout, 'CONTEXT_EXIT ');
    assert.deepEqual(exit, row.workerExit);
    assert.deepEqual(exit, {clean: true, processObserved: true, exitStatus: 0, forced: false, errorEvents: 0});
    assert.equal(row.healthyReuse, true);
    assert.equal(row.sourceTerminalBeforeNativeStop, true);
    assert.equal(row.sourceBeforeGuestBodyRelease.length, 1);
    const before = row.sourceBeforeGuestBodyRelease[0];
    assert.deepEqual(before.events.slice(0, 2), ['request', 'headers-sent']);
    assert.ok(!before.events.includes('body-released'), 'upstream bytes held through both request barriers');
    assert.equal(before.fixtureEnded, false); assert.equal(before.fixtureDestroyed, false);
    assert.equal(before.payload, undefined);
    assert.equal(before.peerEnded, before.events.includes('peer-ended'));
    const rejectsForeignScope = ['read', 'readAtLeast', 'read-prototype', 'read-reflect'].includes(row.operation.split('&')[0]);
    if (!rejectsForeignScope) {
        assert.equal(before.peerEnded, false); assert.equal(before.peerReset, false);
        assert.deepEqual(before.events, ['request', 'headers-sent']);
    }
    assert.equal(row.sourceBeforeNativeStop.length, 1);
    const after = row.sourceBeforeNativeStop[0];
    assert.equal(after.fixtureDestroyed, false);
    assert.equal(after.fixtureEnded, true);
    assert.ok(after.peerEnded || after.peerReset, 'actual source terminal before fixture cleanup');
    assert.ok(after.events.indexOf('body-released') > after.events.indexOf('headers-sent'));
    assert.equal(after.rawRequest, before.rawRequest);
    assert.equal(Buffer.from(after.payload, 'base64').toString('hex'), '0102030405060708090a0b0c0d0e0f1011');
    const request = Buffer.from(before.rawRequest, 'base64').toString('ascii');
    assert.match(request, /^GET \/body HTTP\/1\.1\r\n/);
    return {operation: row.operation, exactPublicState: true, healthyReuse: true, cleanNaturalExit: true};
}
