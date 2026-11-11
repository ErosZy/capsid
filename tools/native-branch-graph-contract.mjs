import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import {join} from 'node:path';
import {gunzipSync, constants} from 'node:zlib';
import {nativeReaderContextProfile, nativeReaderContextBinarySHA256,
    readerContextSHA256, readerContextStable} from './native-reader-context-contract.mjs';

export const nativeBranchGraphPrograms = Object.freeze({
    held: 'tests/fixtures/workerd-native-branch-graph.mjs',
    checkpoints: 'tests/fixtures/workerd-native-transform-error-checkpoints.mjs',
    cancel: 'tests/fixtures/workerd-native-transform-cancel-boundary.mjs',
    controls: 'tests/fixtures/workerd-native-pipe-error-controls.mjs',
    priority: 'tests/fixtures/workerd-native-pipe-cancel-errors.mjs'});
export const nativeBranchGraphFiles = Object.freeze({
    archive: 'tests/fixtures/workerd-native-branch-graph-primary.json.gz',
    lock: 'tests/fixtures/workerd-native-branch-graph-primary-lock.json',
    controls: 'tests/fixtures/native-branch-graph-oracle-control.json.gz'});
export const nativeBranchGraphGroups = Object.freeze(['held_raw', 'held_gzip', 'checkpoints', 'cancel',
    'controls_raw', 'controls_gzip', 'priority_raw', 'priority_gzip']);
const held = ['tee', 'clone'].flatMap(topology => ['drain-default', 'drain-byob', 'cancel-first',
    'values-break', 'values-prevent-cancel', 'pipe-reject', 'transform-reject'].map(action => topology + '-' + action));
held.push('nested-tee', 'clone-tee');
const graphs = {
    held,
    checkpoints: ['tee-transform-reject', 'clone-transform-reject'],
    cancel: ['tee', 'clone'].flatMap(topology => ['async-cancel', 'prevent-cancel']
        .map(mode => topology + '-transform-reject-' + mode)),
    controls: ['direct', 'direct-prevent-cancel', 'pair', 'pair-prevent-cancel',
        'mismatched', 'mismatched-prevent-cancel', 'preerrored', 'self-cycle', 'two-cycle'],
    priority: ['direct', 'pair', 'mismatched'].flatMap(kind => ['cancel-throw', 'cancel-reject-async']
        .map(mode => kind + '~' + mode))};
export const nativeBranchGraphPlans = Object.freeze(Object.entries(graphs).flatMap(([category, cases]) =>
    ['raw', 'gzip'].flatMap(bodyKind => cases.map(graph => Object.freeze({category, bodyKind, graph})))));
export function branchGraphKey(row) {
    assert.ok(Object.hasOwn(graphs, row.category)); assert.ok(['raw', 'gzip'].includes(row.bodyKind));
    assert.ok(graphs[row.category].includes(row.graph));
    return [row.category, row.bodyKind, row.graph].join('/');
}
export function branchGraphPlansForGroup(group) {
    assert.ok(nativeBranchGraphGroups.includes(group));
    const [category, bodyKind] = group.split('_');
    return nativeBranchGraphPlans.filter(plan => plan.category === category && (!bodyKind || plan.bodyKind === bodyKind));
}
export function expandBranchGraphPublicState(value, target) {
    if (Array.isArray(value)) return value.map(item => expandBranchGraphPublicState(item, target));
    if (value !== null && typeof value === 'object') return Object.fromEntries(Object.entries(value).map(([key, item]) => {
        if (key === 'url') {assert.equal(item, '@exact-upstream-url@'); return [key, target];}
        return [key, expandBranchGraphPublicState(item, target)];
    }));
    return value;
}
export function validateBranchGraphArchive(compressed, lock, programs) {
    assert.equal(lock.schema, 'capsid-native-branch-graph-primary-lock-v1');
    assert.deepEqual(lock.profile, nativeReaderContextProfile); assert.deepEqual(lock.referenceBinarySHA256, nativeReaderContextBinarySHA256);
    assert.deepEqual(Object.keys(lock.programs).sort(), Object.keys(nativeBranchGraphPrograms).sort());
    for (const [category, file] of Object.entries(nativeBranchGraphPrograms)) {
        assert.equal(lock.programs[category].file, file); assert.equal(readerContextSHA256(programs[category]), lock.programs[category].sha256);
    }
    assert.equal(lock.archive.file, nativeBranchGraphFiles.archive); assert.equal(readerContextSHA256(compressed), lock.archive.sha256);
    const raw = gunzipSync(compressed); assert.equal(readerContextSHA256(raw), lock.archive.uncompressedSHA256);
    const primary = JSON.parse(raw);
    assert.equal(primary.schema, 'capsid-native-branch-graph-primary-v1');
    assert.deepEqual(primary.profile, nativeReaderContextProfile); assert.deepEqual(primary.programs, lock.programs);
    assert.equal(primary.bothPinnedNativeReferenceAssetsHaveIdenticalPublicReports, true);
    assert.deepEqual(Object.keys(primary.provenance).sort(), ['linux', 'win32']);
    for (const platform of ['linux', 'win32']) {
        assert.deepEqual(Object.keys(primary.provenance[platform]).sort(), Object.keys(nativeBranchGraphPrograms).sort());
        for (const [category, row] of Object.entries(primary.provenance[platform])) {
            assert.equal(row.referenceLock.binary_sha256, nativeReaderContextBinarySHA256[platform]);
            assert.deepEqual(Object.fromEntries(Object.keys(nativeReaderContextProfile).map(key => [key, row.referenceLock[key]])), nativeReaderContextProfile);
            assert.equal(row.programSHA256, lock.programs[category].sha256);
            assert.match(row.evidenceSHA256, /^[a-f0-9]{64}$/); assert.match(row.runnerSHA256, /^[a-f0-9]{64}$/);
        }
    }
    assert.equal(lock.nativeEventDeadlineMilliseconds, 5000); assert.equal(lock.nativeNaturalExitDeadlineMilliseconds, 2500);
    assert.equal(lock.requestsPerRuntime, 3); assert.deepEqual(lock.protocol, {wire_version: 4, capabilities: 123, max_channels: 1});
    const keys = nativeBranchGraphPlans.map(branchGraphKey).sort(); assert.equal(keys.length, 74); assert.equal(new Set(keys).size, 74);
    assert.deepEqual(Object.keys(primary.cases).sort(), keys); assert.deepEqual(Object.keys(lock.casesSHA256).sort(), keys);
    for (const plan of nativeBranchGraphPlans) {
        const key = branchGraphKey(plan), value = primary.cases[key];
        assert.deepEqual(Object.keys(value).sort(), ['guest', 'owner']);
        assert.equal(readerContextSHA256(readerContextStable(value)), lock.casesSHA256[key], key + ' exact primary public observation');
    }
    assert.deepEqual(Object.keys(lock.packets).sort(), ['gzip', 'raw']);
    assert.equal(Object.keys(lock.observedPackets).length, 3);
    for (const kind of ['raw', 'gzip']) assert.deepEqual(lock.packets[kind], lock.observedPackets[lock.packets[kind].sha256]);
    for (const [sha256, packet] of Object.entries(lock.observedPackets)) {
        const kind = packet.bodyKind;
        assert.ok(['raw', 'gzip'].includes(kind)); assert.equal(packet.sha256, sha256);
        const prefix = Buffer.from(packet.prefix, 'base64'), suffix = Buffer.from(packet.suffix, 'base64');
        assert.equal(readerContextSHA256(Buffer.concat([prefix, suffix])), packet.sha256);
        assert.equal(readerContextSHA256(prefix), packet.prefixSHA256);
        const decode = data => kind === 'gzip' ? gunzipSync(data, {finishFlush: constants.Z_SYNC_FLUSH, maxOutputLength: 64}) : data;
        assert.deepEqual([...decode(prefix)], [1, 2, 3]); assert.deepEqual([...decode(Buffer.concat([prefix, suffix]))], Array.from({length: 17}, (_, i) => i + 1));
    }
    return {primary, lock};
}
export function loadBranchGraphPrimaries(root) {
    return validateBranchGraphArchive(readFileSync(join(root, nativeBranchGraphFiles.archive)),
        JSON.parse(readFileSync(join(root, nativeBranchGraphFiles.lock), 'utf8')),
        Object.fromEntries(Object.entries(nativeBranchGraphPrograms).map(([category, file]) => [category, readFileSync(join(root, file))])));
}
function oneJson(stdout, prefix) {
    const lines = stdout.split(/\r?\n/).filter(line => line.startsWith(prefix)); assert.equal(lines.length, 1, 'one actual ' + prefix + ' record');
    return JSON.parse(lines[0].slice(prefix.length));
}
export function assertNativeBranchGraphCase(row, primaries) {
    const key = branchGraphKey(row), value = row.native;
    assert.equal(row.programSHA256, primaries.lock.programs[row.category].sha256);
    assert.equal(value.collectionFailure, undefined, value.collectionFailure); assert.ok(!value.helperForced);
    assert.deepEqual(value.exit, {code: 0, signal: null}); assert.equal(value.suffixReleased, true);
    const target = new URL(row.targetUrl);
    assert.equal(target.protocol, 'http:'); assert.equal(target.hostname, 'localhost'); assert.match(target.port, /^\d+$/);
    assert.equal(target.pathname, '/' + row.bodyKind + '-body'); assert.equal(target.search, '?readerRoute=' + row.graph);
    assert.deepEqual({guest: value.guest, owner: value.owner}, expandBranchGraphPublicState(primaries.primary.cases[key], row.targetUrl), key + ' exact public state');
    for (const [prefix, field] of [['CONTEXT_GUEST ', 'guest'], ['CONTEXT_OWNER ', 'owner'], ['CONTEXT_PROTOCOL ', 'protocol'], ['CONTEXT_EXIT ', 'workerExit']]) {
        assert.deepEqual(oneJson(value.stdout, prefix), value[field]);
    }
    assert.equal(value.stdout.split(/\r?\n/).filter(line => line === 'CONTEXT_OWNER_READ_READY true').length, 1);
    assert.deepEqual(value.workerExit, {clean: true, processObserved: true, exitStatus: 0, forced: false, errorEvents: 0});
    assert.equal(value.protocol.requests, 3); assert.deepEqual(value.protocol.replies.map(reply => reply.id), [1, 2, 3]);
    for (const reply of value.protocol.replies) {
        assert.equal(reply.heads, 1); assert.equal(reply.ends, 1); assert.ok(Number.isSafeInteger(reply.bodyFrames) && reply.bodyFrames > 0);
        assert.ok(Number.isSafeInteger(reply.bodyBytes) && reply.bodyBytes > 0); assert.equal(reply.bodyBytes, reply.returnedCredit);
    }
    assert.equal(value.healthyReuse, true); assert.equal(value.sourceTerminalBeforeRuntimeStop, true);
    assert.equal(value.sourceBeforeGuestBodyRelease.length, 1); assert.equal(value.sourceBeforeRuntimeStop.length, 1);
    const before = value.sourceBeforeGuestBodyRelease[0], after = value.sourceBeforeRuntimeStop[0];
    assert.deepEqual(before.events, ['request', 'headers-and-prefix-sent']); assert.equal(before.fixtureEnded, false); assert.equal(before.fixtureDestroyed, false);
    assert.equal(before.peerEnded, false); assert.equal(before.peerReset, false); assert.equal(before.suffix, undefined);
    assert.equal(after.fixtureEnded, true); assert.equal(after.fixtureDestroyed, false); assert.ok(after.peerEnded || after.peerReset);
    assert.ok(after.events.indexOf('suffix-released') > after.events.indexOf('headers-and-prefix-sent'));
    assert.equal(after.rawRequest, before.rawRequest); assert.equal(after.headers, before.headers);
    const packet = primaries.lock.observedPackets[row.sourcePacketSHA256];
    assert.ok(packet); assert.equal(packet.bodyKind, row.bodyKind);
    assert.equal(before.prefix, packet.prefix); assert.equal(after.prefix, packet.prefix); assert.equal(after.suffix, packet.suffix);
    assert.equal(row.sourcePacketSHA256, packet.sha256); assert.equal(row.sourcePrefixSHA256, packet.prefixSHA256);
    const prefix = Buffer.from(packet.prefix, 'base64'), suffix = Buffer.from(packet.suffix, 'base64');
    assert.equal(row.sourcePrefixEncodedLength, prefix.length); assert.equal(row.sourceSuffixEncodedLength, suffix.length);
    const headers = Buffer.from(before.headers, 'base64').toString('ascii');
    assert.ok(headers.startsWith('HTTP/1.1 201 Graph\r\n')); assert.ok(headers.includes('Content-Length: ' + (prefix.length + suffix.length) + '\r\n'));
    assert.equal(headers.includes('Content-Encoding: gzip\r\n'), row.bodyKind === 'gzip');
    const request = Buffer.from(before.rawRequest, 'base64').toString('ascii');
    assert.ok(request.startsWith('GET /' + row.bodyKind + '-body?readerRoute=' + row.graph + ' HTTP/1.1\r\n'));
    assert.ok(new RegExp('\\r\\nHost: localhost:' + target.port + '\\r\\n', 'i').test(request));
    assert.ok(!/WORKER POISONED|TOKEN LEAK|ERROR: AddressSanitizer|LeakSanitizer:|runtime error:|AddressSanitizer:DEADLYSIGNAL/.test(value.stderr));
    return {key, exactPublicState: true, actualHeldSourceAndCredits: true, naturalExit: true};
}
