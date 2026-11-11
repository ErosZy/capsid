import assert from 'node:assert/strict';
import {gzipSync} from 'node:zlib';

export const nativeFetchDeadlinePlans = Object.freeze(['raw', 'gzip'].flatMap(kind =>
    ['headless', 'unread', 'default', 'byob', 'least'].flatMap(stage => [
        {id: `${kind}-${stage}-signal`, kind, stage, trigger: 'signal', link: 'timeout'},
        ...['implicit', 'request'].map(link =>
            ({id: `${kind}-${stage}-host-${link}`, kind, stage, trigger: 'host', link})),
    ].map(Object.freeze))));
export const nativeFetchDeadlineShards = Object.freeze(['raw_signal', 'gzip_signal', 'raw_host', 'gzip_host']);
export function nativeFetchDeadlineCasesForShard(shard) {
    assert.ok(nativeFetchDeadlineShards.includes(shard), 'mandatory deadline shard');
    return nativeFetchDeadlinePlans.filter(plan => `${plan.kind}_${plan.trigger}` === shard);
}
export function nativeFetchDeadlineReply(plan) {
    const payload = Buffer.from(Array.from({length: 16}, (_, index) => index + 1));
    const encoded = plan.kind === 'gzip' ? gzipSync(payload) : payload;
    if (plan.kind === 'gzip') encoded[9] = 255;
    const head = Buffer.from('HTTP/1.1 201 Created\r\nConnection: close\r\nContent-Type: application/octet-stream\r\n' +
        (plan.kind === 'gzip' ? 'Content-Encoding: gzip\r\n' : '') +
        `X-Deadline-Case: ${plan.id}\r\nContent-Length: ${encoded.length}\r\n\r\n`);
    return {head, encoded};
}
export function nativeFetchDeadlineMetadata(plan, bodyUsed = false, locked = false) {
    return {status: 201, bodyUsed, locked, headers: [
        ['connection', 'close'], ...(plan.kind === 'gzip' ? [['content-encoding', 'gzip']] : []),
        ['content-length', String(nativeFetchDeadlineReply(plan).encoded.length)],
        ['content-type', 'application/octet-stream'], ['x-deadline-case', plan.id],
    ]};
}
export function nativeFetchDeadlinePreControl(plan) {
    const pending = ['default', 'byob', 'least'].includes(plan.stage);
    return {firstPending: pending ? true : null, closedPending: pending ? true : null,
        response: nativeFetchDeadlineMetadata(plan, pending, pending)};
}
export function expectedNativeFetchDeadlineState(plan, readiness = false) {
    assert.deepEqual(plan, nativeFetchDeadlinePlans.find(value => value.id === plan.id));
    const error = (sameReason = false) => ({name: 'TimeoutError', message: 'The operation was aborted due to timeout',
        sameReason, nativePrototype: true});
    const state = {stage: plan.stage, trigger: plan.trigger, link: plan.link, failure: null, trace: [], fetchIssued: true};
    if (plan.stage === 'headless') {
        assert.equal(readiness, false);
        return {...state, failure: error(), trace: ['signal.abort'], reason: error(true), aborted: true};
    }
    state.before = nativeFetchDeadlineMetadata(plan);
    const pending = ['default', 'byob', 'least'].includes(plan.stage);
    if (pending) state.closedStable = true;
    state.preControl = nativeFetchDeadlinePreControl(plan);
    state.control = {status: 200, text: 'ok'};
    if (readiness) return state;
    state.trace = ['signal.abort', ...(pending ? ['closed.rejected'] : []), 'first.rejected', 'retry.rejected'];
    state.reason = error(true);
    if (pending) state.closed = {error: error()};
    const byob = plan.stage !== 'default';
    for (const label of ['first', 'retry']) state[label] = {label, inputBefore: byob ? 24 : null,
        returnedPromise: true, inputAfterIssue: byob ? 0 : null, inputAfter: byob ? 0 : null, error: error()};
    return {...state, aborted: true, released: true, after: nativeFetchDeadlineMetadata(plan, true, false)};
}
export const nativeFetchDeadlineInputPaths = Object.freeze({
    runner: 'tests/test_native_fetch_deadline.mjs', fixture: 'tests/fixtures/native-fetch-deadline.mjs',
    sourceModel: 'tools/native-fetch-deadline-input.mjs', peer: 'tools/native-fetch-deadline-peer.mjs',
    contract: 'tools/native-fetch-deadline-contract.mjs', controls: 'tests/test_native_fetch_deadline_contract.mjs',
    referenceCapture: 'tools/capture-native-fetch-deadline-reference.mjs',
    referenceArchive: 'tests/fixtures/workerd-native-fetch-deadline-report.json.gz',
    referenceMetadata: 'tests/fixtures/workerd-native-fetch-deadline-report.json',
    helper: 'tests/test_native_fetch_deadline.cc', exitObserver: 'tests/graceful_worker_exit.h',
});
