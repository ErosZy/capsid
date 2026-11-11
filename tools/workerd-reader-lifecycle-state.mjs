import assert from 'node:assert/strict';

const routes = ['getReader', 'direct', 'prototype', 'subclass', 'reflect'];
const readers = ['default', 'byob'];
const lifecycle = ['concurrent', 'release-pending', 'release-open', 'release-eof'];
const originalCases = routes.flatMap(route => readers.flatMap(reader => lifecycle.map(scenario =>
    ['native', route, reader, scenario].join('|'))));
export const readerLifecycleCases = Object.freeze([...originalCases,
    ...['native', 'standard'].flatMap(source => routes.flatMap(route => readers.flatMap(reader =>
        [...lifecycle, 'intrinsics'].map(scenario => [source, route, reader, scenario].join('|')))))
        .filter(kind => !originalCases.includes(kind))]);
const payload = [0, 102, 105, 114, 115, 116];
const fulfilled = () => ({settled: true, result: {fulfilled: true, returnedUndefined: true}});
const description = (name, message, tag = null) => ({type: 'object', name, message, tag,
    keys: tag === null ? [] : ['tag'], isTypeError: name === 'TypeError'});
const rejected = error => ({settled: true, result: {error}});
const pendingRead = 'This ReadableStream only supports a single pending read request at a time.';
const pendingRelease = 'Cannot call releaseLock() on a reader with outstanding read promises.';
const releasedReader = 'This ReadableStream reader has been released.';
export function expectedReaderLifecycleState(kind, challenge) {
    assert.ok(readerLifecycleCases.includes(kind));
    const [source, , reader, scenario] = kind.split('|'), byob = reader === 'byob', native = source === 'native';
    const input = () => ({before: byob ? 32 : null, after: byob ? 0 : null});
    const read = (content, done = false, emptyDefault = false) => ({settled: true, result: {value: {
        done, type: done && !byob && !emptyDefault ? null : 'Uint8Array',
        offset: emptyDefault ? 0 : done ? byob ? native ? 0 : 8 : null : byob ? 8 : 0,
        length: done && !byob && !emptyDefault ? null : content.length,
        backingBytes: byob ? 32 : emptyDefault || !done ? 4096 : null,
        bytes: done && !byob && !emptyDefault ? null : content}}});
    const released = () => rejected(description('TypeError', releasedReader));
    const failedRelease = scenario === 'release-open' || !native && scenario === 'release-pending';
    const state = {kind, challenge, traps: scenario === 'intrinsics' && native ? ['Promise.then'] : [],
        unexpected: null, closedBeforeStable: true, closedBefore: failedRelease ? released() : fulfilled()};
    if (scenario === 'concurrent' || scenario === 'release-pending') {
        state.primeInput = input(); state.prime = read(byob ? payload.slice(0, 1) : payload);
        state.firstInput = input();
        state.first = native ? rejected(description(null, null, 'consumer-stop')) : scenario === 'release-pending'
            ? released() : byob ? read(payload.slice(1)) : read([], true, true);
        if (scenario === 'concurrent') {
            state.secondInput = input(); state.second = native ? rejected(description('TypeError', pendingRead)) : read([], true, !byob);
        }
        state.beforeAction = {first: false, second: scenario === 'concurrent' ? native : null, closed: false};
    }
    if (scenario === 'release-eof') {
        state.dataInput = input(); state.data = read(payload);
        state.eofInput = input(); state.eof = read([], true);
    }
    if (scenario === 'intrinsics') {
        state.dataInput = input(); state.data = native
            ? rejected(description('Error', 'application trap Promise.then')) : read(payload);
        state.closedDuringStable = true;
    }
    if (scenario.startsWith('release-')) {
        state.release = scenario === 'release-pending' && native
            ? {error: description('TypeError', pendingRelease)} : {returnedUndefined: true};
        state.lockedAfterReleaseAttempt = scenario === 'release-pending' && native;
        state.closedAfterStable = true; state.closedIdentityRetained = true;
        state.closedAfter = failedRelease ? released() : fulfilled();
    }
    if (scenario === 'release-open' || scenario === 'release-eof') {
        state.retryInput = input(); state.retry = read(scenario === 'release-eof' ? [] : payload, scenario === 'release-eof');
        state.replacementClosed = fulfilled();
    } else if (scenario === 'release-pending' && !native) {
        state.retryInput = input(); state.retry = read([], true, !byob); state.retryBeforeCancel = false;
        state.replacementClosed = fulfilled();
    }
    state.cancel = fulfilled();
    const sameError = scenario === 'release-eof' || scenario === 'intrinsics' || !native && scenario !== 'release-open';
    state.errorIdentity = {firstClosed: sameError, firstAfter: sameError};
    state.locked = false; state.bodyUsed = native ? true : null;
    return state;
}
