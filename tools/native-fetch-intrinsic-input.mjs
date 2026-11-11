import assert from 'node:assert/strict';
import fs from 'node:fs';
import {createHash} from 'node:crypto';
import {gzipSync} from 'node:zlib';

export const nativeFetchIntrinsicPlans = Object.freeze(['raw', 'gzip'].flatMap(kind =>
    ['none', 'get', 'set'].map(api => Object.freeze({id: `${kind}-${api}`, kind, api}))));
export function nativeFetchIntrinsicCases(kind) {
    assert.ok(['raw', 'gzip'].includes(kind), 'required intrinsic shard');
    return nativeFetchIntrinsicPlans.filter(plan => plan.kind === kind);
}
export const nativeFetchIntrinsicPayload = () => Buffer.from(Array.from({length: 16}, (_, index) =>
    (index * 29 + (index >>> 8) + 7) & 255));
export function nativeFetchIntrinsicPacket(plan) {
    const body = plan.kind === 'gzip' ? gzipSync(nativeFetchIntrinsicPayload()) : nativeFetchIntrinsicPayload();
    if (plan.kind === 'gzip') body[9] = 255;
    const head = Buffer.from('HTTP/1.1 201 Created\r\nConnection: close\r\nContent-Type: application/octet-stream\r\n' +
        (plan.kind === 'gzip' ? 'Content-Encoding: gzip\r\n' : '') +
        `X-Intrinsic-Case: ${plan.id}\r\nContent-Length: ${body.length}\r\n\r\n`);
    return Buffer.concat([head, body]);
}
export function expectedNativeFetchIntrinsicState(plan) {
    assert.deepEqual(plan, nativeFetchIntrinsicPlans.find(value => value.id === plan.id));
    return {api: plan.api, failure: null, failurePhase: null, traps: [], returnedPromise: true,
        bytes: [...nativeFetchIntrinsicPayload()], metadata: {status: 201, statusText: 'Created', headers: [
            ['connection', 'close'], ...(plan.kind === 'gzip' ? [['content-encoding', 'gzip']] : []),
            ['content-length', plan.kind === 'gzip' ? '36' : '16'],
            ['content-type', 'application/octet-stream'], ['x-intrinsic-case', plan.id],
        ]}, cleanupBytes: null, cleanupError: null};
}
export function expectedNativeFetchIntrinsicSource(plan, port) {
    const request = `GET /body HTTP/1.1\r\nHost: 127.0.0.1:${port}`, packet = nativeFetchIntrinsicPacket(plan);
    return {plan, records: [{request, rawRequest: Buffer.from(request + '\r\n\r\n').toString('base64'),
        sentBytes: packet.length, sentSHA256: createHash('sha256').update(packet).digest('hex'),
        events: ['accepted', 'request', 'sent-end', 'peer-ended'], peerEnded: true, peerReset: false,
        fixtureEnded: true, fixtureDestroyed: false}], port};
}
export const nativeFetchIntrinsicInputPaths = Object.freeze({
    runner: 'tests/test_native_fetch_intrinsic.mjs', binding: 'tests/fixtures/native-fetch-intrinsic-binding.mjs',
    fixture: 'tests/fixtures/native-fetch-intrinsic.mjs', user: 'tests/fixtures/native-fetch-intrinsic-user.mjs',
    sourceModel: 'tools/native-fetch-intrinsic-input.mjs', peer: 'tools/native-fetch-intrinsic-peer.mjs',
    contract: 'tools/native-fetch-intrinsic-contract.mjs', controls: 'tests/test_native_fetch_intrinsic_contract.mjs',
    referenceCapture: 'tools/capture-native-fetch-intrinsic-reference.mjs',
    referenceArchive: 'tests/fixtures/workerd-native-fetch-intrinsic-report.json.gz',
    referenceMetadata: 'tests/fixtures/workerd-native-fetch-intrinsic-report.json',
    helper: 'tests/test_binding_fetch_retained.cc', exitObserver: 'tests/graceful_worker_exit.h',
});
const root = new URL('../', import.meta.url);
const digest = file => createHash('sha256').update(fs.readFileSync(new URL(file, root))).digest('hex');
export function nativeFetchIntrinsicPrimaryIdentities() {
    const binding = fs.readFileSync(new URL(nativeFetchIntrinsicInputPaths.binding, root), 'utf8');
    const fixture = fs.readFileSync(new URL(nativeFetchIntrinsicInputPaths.fixture, root), 'utf8');
    const core = source => source.slice(0, source.indexOf('\nexport default'));
    assert.ok(core(binding).startsWith('async function measureNativeIntrinsics('));
    assert.ok(core(binding).endsWith('\n}'));
    assert.equal(core(binding), core(fixture), 'same function in Binding and actual public workerd');
    return {fixtureSHA256: digest(nativeFetchIntrinsicInputPaths.fixture),
        bindingSHA256: digest(nativeFetchIntrinsicInputPaths.binding),
        commonFunctionSHA256: createHash('sha256').update(core(binding)).digest('hex'),
        sourceModelSHA256: digest(nativeFetchIntrinsicInputPaths.sourceModel),
        peerSHA256: digest(nativeFetchIntrinsicInputPaths.peer), contractSHA256: digest(nativeFetchIntrinsicInputPaths.contract),
        captureSHA256: digest(nativeFetchIntrinsicInputPaths.referenceCapture)};
}
