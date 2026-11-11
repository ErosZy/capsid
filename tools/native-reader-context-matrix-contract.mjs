import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import {join} from 'node:path';
import {gunzipSync} from 'node:zlib';
import {nativeReaderContextOperations, nativeReaderContextProfile, nativeReaderContextBinarySHA256,
    readerContextSHA256, readerContextStable} from './native-reader-context-contract.mjs';

export const readerContextMatrixFiles = Object.freeze({
    fixture: 'tests/fixtures/workerd-native-reader-context-matrix.mjs',
    archive: 'tests/fixtures/workerd-native-reader-context-matrix-primary.json.gz',
    lock: 'tests/fixtures/workerd-native-reader-context-matrix-primary-lock.json',
    controls: 'tests/fixtures/native-reader-context-matrix-oracle-control.json.gz'});
export const readerContextMatrixRoutes = Object.freeze(['getReader', 'direct', 'prototype', 'subclass', 'reflect']);
export const readerContextMatrixGroups = Object.freeze(['raw', 'gzip'].flatMap(bodyKind =>
    ['none', 'pending'].flatMap(ownerMode => readerContextMatrixRoutes.map(route =>
        Object.freeze({bodyKind, ownerMode, route})))));
export const readerContextMatrixPlans = Object.freeze(readerContextMatrixGroups.flatMap(group =>
    nativeReaderContextOperations.map(operation => Object.freeze({...group, operation}))));
export function readerContextMatrixKey(row) {
    assert.ok(['raw', 'gzip'].includes(row.bodyKind));
    assert.ok(['none', 'pending'].includes(row.ownerMode));
    assert.ok(readerContextMatrixRoutes.includes(row.route));
    assert.ok(nativeReaderContextOperations.includes(row.operation));
    return [row.bodyKind, row.ownerMode, row.route, row.operation].join('/');
}
const data = Array.from({length: 17}, (_, index) => index + 1);
const validIo = new Set(['read', 'readAtLeast', 'read-prototype', 'read-reflect']);
const initial = () => ({phase: 'pending', before: {capacity: 80, offset: 8, length: 64, backing: Array(80).fill(165)},
    afterIssue: {capacity: 0, offset: 0, length: 0, backing: {errorName: 'TypeError'}}});
function successfulOwner(owner) {
    assert.equal(owner.failure, null); assert.equal(owner.eof, true);
    assert.equal(owner.bodyUsed, true); assert.equal(owner.lockedBeforeRelease, true);
    assert.deepEqual(owner.reads, [
        {done: false, type: 'Uint8Array', offset: 8, length: 17, capacity: 80,
            bytes: data, backing: [...Array(8).fill(165), ...data, ...Array(55).fill(165)]},
        {done: true, type: 'Uint8Array', offset: 0, length: 0, capacity: 80,
            bytes: [], backing: Array(80).fill(165)}]);
}
function primarySemantics(state, plan) {
    assert.deepEqual(Object.keys(state).sort(), ['guest', 'owner']);
    const {guest, owner} = state;
    const operation = plan.operation.split('&')[0];
    const construction = {instanceOfReader: true, constructorMatches: true,
        directPrototype: !['subclass', 'reflect'].includes(plan.route),
        prototypeParent: ['subclass', 'reflect'].includes(plan.route)};
    for (const value of [guest, owner]) {
        assert.equal(value.route, plan.route); assert.equal(value.bodyKind, plan.bodyKind);
        assert.equal(value.ownerPending, plan.ownerMode === 'pending');
        assert.deepEqual(value.construction, construction);
    }
    assert.equal(guest.operation, operation);
    assert.deepEqual(owner.finalGuest, guest);
    assert.equal(owner.releaseAttempted, !plan.operation.includes('&retain=1'));
    assert.equal(owner.lockedAfterRelease, plan.operation.includes('&retain=1'));
    assert.equal(owner.releaseError, undefined);
    if (plan.ownerMode === 'pending') {
        assert.deepEqual(guest.ownerInitialBefore, initial());
        assert.deepEqual(guest.ownerInitialAfterCheckpoint, initial());
        assert.deepEqual(owner.initialBeforeAwait, initial());
        successfulOwner(owner);
        assert.deepEqual(owner.initialAfterAwait, {...initial(), phase: 'fulfilled', value: owner.reads[0]});
        if (validIo.has(operation)) {
            assert.deepEqual(guest.result, {phase: 'rejected', error: {name: 'TypeError',
                message: 'This ReadableStream only supports a single pending read request at a time.'}});
            assert.equal(guest.afterIssue.capacity, 0); assert.equal(guest.afterCheckpoint.capacity, 0);
        }
    } else {
        assert.equal(guest.ownerInitialBefore, null); assert.equal(guest.ownerInitialAfterCheckpoint, null);
        assert.equal(owner.initialBeforeAwait, null);
        if (!validIo.has(operation)) successfulOwner(owner);
    }
}
export function validateReaderContextMatrixArchive(compressed, lock, program) {
    assert.equal(lock.schema, 'capsid-native-reader-context-matrix-primary-lock-v1');
    assert.deepEqual(lock.profile, nativeReaderContextProfile);
    assert.deepEqual(lock.referenceBinarySHA256, nativeReaderContextBinarySHA256);
    assert.equal(lock.program.file, readerContextMatrixFiles.fixture);
    assert.equal(readerContextSHA256(program), lock.program.sha256);
    assert.equal(lock.archive.file, readerContextMatrixFiles.archive);
    assert.equal(readerContextSHA256(compressed), lock.archive.sha256);
    const raw = gunzipSync(compressed);
    assert.equal(readerContextSHA256(raw), lock.archive.uncompressedSHA256);
    const primary = JSON.parse(raw);
    assert.equal(primary.schema, 'capsid-native-reader-context-matrix-primary-v1');
    assert.deepEqual(primary.profile, nativeReaderContextProfile); assert.deepEqual(primary.program, lock.program);
    assert.equal(primary.exactPublicReportsMatchAcrossBothNativeReferenceAssets, true);
    assert.deepEqual(Object.keys(primary.provenance).sort(), ['linux', 'win32']);
    for (const platform of ['linux', 'win32']) {
        const source = primary.provenance[platform];
        assert.equal(source.referenceLock.binary_sha256, nativeReaderContextBinarySHA256[platform]);
        assert.deepEqual(Object.fromEntries(Object.keys(nativeReaderContextProfile).map(key =>
            [key, source.referenceLock[key]])), nativeReaderContextProfile);
        assert.match(source.evidenceSHA256, /^[a-f0-9]{64}$/); assert.match(source.runnerSHA256, /^[a-f0-9]{64}$/);
    }
    assert.equal(lock.nativeEventDeadlineMilliseconds, 5000);
    assert.equal(lock.nativeNaturalExitDeadlineMilliseconds, 2500);
    assert.equal(lock.requestsPerRuntime, 3);
    assert.deepEqual(lock.protocol, {wire_version: 4, capabilities: 123, max_channels: 1});
    const keys = readerContextMatrixPlans.map(readerContextMatrixKey).sort();
    assert.equal(keys.length, 320); assert.equal(new Set(keys).size, 320);
    assert.deepEqual(Object.keys(primary.cases).sort(), keys); assert.deepEqual(Object.keys(lock.casesSHA256).sort(), keys);
    for (const plan of readerContextMatrixPlans) {
        const key = readerContextMatrixKey(plan), state = primary.cases[key];
        assert.equal(readerContextSHA256(readerContextStable(state)), lock.casesSHA256[key], key + ' primary public report');
        primarySemantics(state, plan);
    }
    return {primary, lock};
}
export function loadReaderContextMatrixPrimaries(root) {
    return validateReaderContextMatrixArchive(readFileSync(join(root, readerContextMatrixFiles.archive)),
        JSON.parse(readFileSync(join(root, readerContextMatrixFiles.lock), 'utf8')),
        readFileSync(join(root, readerContextMatrixFiles.fixture)));
}
function oneJson(stdout, prefix) {
    const lines = stdout.split(/\r?\n/).filter(line => line.startsWith(prefix));
    assert.equal(lines.length, 1, 'one actual ' + prefix + ' record');
    return JSON.parse(lines[0].slice(prefix.length));
}
export function assertReaderContextMatrixCase(row, primaries) {
    const key = readerContextMatrixKey(row);
    assert.equal(row.collectionFailure, undefined, row.collectionFailure);
    assert.ok(!row.helperForced); assert.equal(row.ownerReadyBeforeGuest, true);
    assert.equal(row.ownerEnteredReadBeforeSourceRelease, true);
    assert.deepEqual(row.nativeExit, {code: 0, signal: null});
    assert.deepEqual({guest: row.guest, owner: row.owner}, primaries.primary.cases[key], key + ' exact public state');
    assert.deepEqual(oneJson(row.nativeStdout, 'CONTEXT_GUEST '), row.guest);
    assert.deepEqual(oneJson(row.nativeStdout, 'CONTEXT_OWNER '), row.owner);
    assert.equal(row.nativeStdout.split(/\r?\n/).filter(line => line === 'CONTEXT_OWNER_READ_READY true').length, 1);
    const protocol = oneJson(row.nativeStdout, 'CONTEXT_PROTOCOL ');
    assert.deepEqual(protocol, row.protocol); assert.equal(protocol.requests, 3);
    assert.deepEqual(protocol.replies.map(reply => reply.id), [1, 2, 3]);
    for (const reply of protocol.replies) {
        assert.equal(reply.heads, 1); assert.equal(reply.ends, 1);
        assert.ok(Number.isSafeInteger(reply.bodyFrames) && reply.bodyFrames > 0);
        assert.ok(Number.isSafeInteger(reply.bodyBytes) && reply.bodyBytes > 0);
        assert.equal(reply.bodyBytes, reply.returnedCredit);
    }
    const exit = oneJson(row.nativeStdout, 'CONTEXT_EXIT ');
    assert.deepEqual(exit, row.workerExit);
    assert.deepEqual(exit, {clean: true, processObserved: true, exitStatus: 0, forced: false, errorEvents: 0});
    assert.equal(row.healthyReuse, true); assert.equal(row.sourceTerminalBeforeNativeStop, true);
    assert.equal(row.sourceBeforeGuestBodyRelease.length, 1); assert.equal(row.sourceBeforeNativeStop.length, 1);
    const before = row.sourceBeforeGuestBodyRelease[0], after = row.sourceBeforeNativeStop[0];
    assert.deepEqual(before.events.slice(0, 2), ['request', 'headers-sent']);
    assert.ok(!before.events.includes('body-released')); assert.equal(before.payload, undefined);
    assert.equal(before.fixtureEnded, false); assert.equal(before.fixtureDestroyed, false);
    assert.equal(before.peerEnded, before.events.includes('peer-ended'));
    if (row.ownerMode === 'pending' || !validIo.has(row.operation.split('&')[0])) {
        assert.equal(before.peerEnded, false); assert.equal(before.peerReset, false);
        assert.deepEqual(before.events, ['request', 'headers-sent']);
    }
    assert.equal(after.fixtureEnded, true); assert.equal(after.fixtureDestroyed, false);
    assert.ok(after.peerEnded || after.peerReset);
    assert.ok(after.events.indexOf('body-released') > after.events.indexOf('headers-sent'));
    assert.equal(after.rawRequest, before.rawRequest); assert.equal(after.headers, before.headers);
    const packet = Buffer.from(after.payload, 'base64');
    assert.equal(readerContextSHA256(packet), row.sourcePacketSHA256);
    assert.deepEqual(Array.from(row.bodyKind === 'gzip' ? gunzipSync(packet) : packet), data);
    const headers = Buffer.from(before.headers, 'base64').toString('ascii');
    assert.ok(headers.startsWith('HTTP/1.1 201 Context\r\n'));
    assert.ok(headers.includes('Content-Length: ' + packet.length + '\r\n'));
    assert.equal(headers.includes('Content-Encoding: gzip\r\n'), row.bodyKind === 'gzip');
    const prefix = (row.ownerMode === 'pending' ? 'pending-' : '') + (row.bodyKind === 'gzip' ? 'gzip-' : '');
    const request = Buffer.from(before.rawRequest, 'base64').toString('ascii');
    assert.ok(request.startsWith('GET /' + prefix + 'body?readerRoute=' + row.route + ' HTTP/1.1\r\n'));
    assert.ok(!/ERROR: AddressSanitizer|LeakSanitizer:|AddressSanitizer:DEADLYSIGNAL|runtime error:/.test(row.nativeStderr));
    return {key, exactPublicState: true, healthyReuse: true, cleanNaturalExit: true};
}
