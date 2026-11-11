import assert from 'node:assert/strict';
import {gzipSync, gunzipSync, constants} from 'node:zlib';
import {createHash} from 'node:crypto';

const hash = value => createHash('sha256').update(value).digest('hex');
const plans = [];
for (const gzip of [false, true]) for (const bytes of [65536, 8388608]) for (const view of [8192, 65536]) {
    plans.push(Object.freeze({id: (gzip ? 'gzip' : 'raw')+bytes+'-abort-prefix-view'+view, bytes, view, gzip}));
}
export const nativeFetchAbortPrefixPlans = Object.freeze(plans);
export const nativeFetchAbortPrefixInputPaths = Object.freeze({
    runner: 'tests/test_native_fetch_abort_prefix.mjs', fixture: 'tests/fixtures/native-fetch-abort-prefix.mjs',
    helper: 'tests/test_native_fetch_response_limits.cc', contract: 'tools/native-fetch-abort-prefix-contract.mjs',
    sourceModel: 'tools/native-fetch-abort-prefix-input.mjs', referenceArchive: 'tests/fixtures/workerd-native-fetch-abort-prefix-report.json',
});
const replies = new Map(), expectedStates = new Map();
const reason = () => ({name: 'Error', message: 'prefix probe stop', sameReason: false});
for (const plan of plans) {
    const payload = plan.gzip ? gzipSync(Buffer.alloc(plan.bytes)) : Buffer.alloc(plan.bytes);
    if (plan.gzip) payload[9] = 255;
    const head = Buffer.from('HTTP/1.1 201 Created\r\nConnection: close\r\nContent-Type: application/octet-stream\r\n'+
        (plan.gzip ? 'Content-Encoding: gzip\r\n' : '')+'X-Decoded-Bytes: '+plan.bytes+'\r\nContent-Length: '+payload.length+'\r\n\r\n');
    replies.set(plan.id, {payload, packet: Buffer.concat([head, payload])});
    // For these exact gzip inputs independently decode the body portion of
    // the initial4KiB wire window. Raw abort discards further raw input.
    // These bounded construction models never store validation results.
    const total = plan.gzip ? gunzipSync(payload.subarray(0, 4096-head.length), {finishFlush: constants.Z_SYNC_FLUSH}).length : 64;
    assert.equal(total, plan.gzip ? (plan.bytes === 65536 ? 65536 : 4042604) : 64);
    const read = (length, size) => {
        const backing = Buffer.alloc(size+16, 165); backing.fill(0, 8, 8+length);
        return Object.freeze({done: false, inputAfterIssue: 0, inputAfter: 0, type: 'Uint8Array', offset: 8,
            length, capacity: size+16, dataHash: hash(Buffer.alloc(length)), backingHash: hash(backing)});
    };
    const reads = [read(64, 64)];
    for (let remaining = total-64; remaining > 0; remaining -= plan.view) reads.push(read(Math.min(remaining, plan.view), plan.view));
    reads.push(Object.freeze({error: Object.freeze(reason()), inputAfterIssue: 0, inputAfter: 0, capacity: plan.view+16}));
    expectedStates.set(plan.id, {failure: null, reads: Object.freeze(reads), total,
        headers: [['connection', 'close'], ...(plan.gzip ? [['content-encoding', 'gzip']] : []),
            ['content-length', String(payload.length)], ['content-type', 'application/octet-stream'], ['x-decoded-bytes', String(plan.bytes)]],
        expectedBytes: plan.bytes, closedStable: true, aborted: true, closed: {error: reason()}, after: {bodyUsed: true, locked: false}});
}
export function nativeFetchAbortPrefixReply(plan) {
    assert.deepEqual(plan, plans.find(item => item.id === plan.id));
    const value = replies.get(plan.id); return {payload: Buffer.from(value.payload), packet: Buffer.from(value.packet)};
}
export function expectedNativeFetchAbortPrefixState(plan) {
    assert.deepEqual(plan, plans.find(item => item.id === plan.id));
    return structuredClone(expectedStates.get(plan.id));
}
