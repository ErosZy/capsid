import assert from 'node:assert/strict';
import {gzipSync} from 'node:zlib';

export const unreadBindingKinds = Object.freeze(['raw', 'gzip']);
export const unreadBindingStages = Object.freeze(['unread', 'locked-default', 'locked-byob',
    'pending-default', 'pending-byob', 'pending-least']);
export const unreadBindingPlans = Object.freeze(unreadBindingKinds.flatMap(kind => unreadBindingStages.flatMap(stage =>
    [1, 4].map(count => Object.freeze({id: `${kind}-${stage}-${count}`, kind, stage, count})))));
export function unreadBindingCases(kind) {
    assert.ok(unreadBindingKinds.includes(kind), 'mandatory body kind');
    return unreadBindingPlans.filter(plan => plan.kind === kind);
}
export function unreadBindingHead(kind, slot) {
    assert.ok(unreadBindingKinds.includes(kind));
    const payload = Buffer.from(Array.from({length: 16}, (_, index) => index + 1));
    const encoded = kind === 'gzip' ? gzipSync(payload) : payload;
    return Buffer.from('HTTP/1.1 201 Created\r\nConnection: close\r\nContent-Type: application/octet-stream\r\n' +
        (kind === 'gzip' ? 'Content-Encoding: gzip\r\n' : '') +
        `X-Unread-Slot: ${slot}\r\nContent-Length: ${encoded.length}\r\n\r\n`);
}
export function expectedUnreadBindingState(plan, request) {
    assert.deepEqual(plan, unreadBindingPlans.find(value => value.id === plan.id));
    assert.ok(request === 1 || request === 2);
    const pending = plan.stage.startsWith('pending-'), reader = plan.stage !== 'unread';
    const byob = pending && plan.stage !== 'pending-default';
    return {stage: plan.stage, count: plan.count, requestId: String(request), openSignalAborted: false,
        resources: Array.from({length: plan.count}, (_, slot) => ({slot, status: 201,
            bodyUsed: pending, locked: reader, headers: [['connection', 'close'],
                ...(plan.kind === 'gzip' ? [['content-encoding', 'gzip']] : []),
                ['content-length', plan.kind === 'gzip' ? '36' : '16'],
                ['content-type', 'application/octet-stream'], ['x-unread-slot', String(slot)]],
            hasReader: reader, closedPending: reader ? true : null, firstPending: pending ? true : null,
            inputAfterIssue: byob ? 0 : null}))};
}
export function expectedUnreadBindingSources(plan, port, closed = false) {
    return Array.from({length: plan.count}, (_, slot) => {
        const request = `GET /body?slot=${slot} HTTP/1.1\r\nHost: 127.0.0.1:${port}`;
        return {request, rawRequest: Buffer.from(request + '\r\n\r\n').toString('base64'),
            sent: [unreadBindingHead(plan.kind, slot).toString('base64')],
            events: ['accepted', 'request', 'sent-head', ...(closed ? ['peer-ended'] : [])],
            peerEnded: closed, peerReset: false, fixtureEnded: false, fixtureDestroyed: false, slot};
    });
}
export const unreadBindingInputPaths = Object.freeze({
    runner: 'tests/test_binding_fetch_unread.mjs', controls: 'tests/test_binding_fetch_unread_contract.mjs',
    binding: 'tests/fixtures/binding-fetch-unread-package.mjs', user: 'tests/fixtures/binding-fetch-unread-user.mjs',
    sourceModel: 'tools/binding-fetch-unread-input.mjs', contract: 'tools/binding-fetch-unread-contract.mjs',
    helper: 'tests/test_binding_fetch_retained.cc', exitObserver: 'tests/graceful_worker_exit.h',
    policy: 'docs/binding-technical-design.md', rpcPolicy: 'src/binding_rpc.h', rpcBackend: 'src/binding_rpc.cc',
});
