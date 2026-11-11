import assert from 'node:assert/strict';
import {gzipSync} from 'node:zlib';

const plans = [];
for (const bytes of [65536, 8*1024*1024])
    for (const action of ['stream-cancel', 'reader-cancel', 'read-cancel', 'abort', 'retain'])
        plans.push({id: 'gzip'+bytes+'-'+action, bytes, view: 64, gzip: true, action});
export const nativeFetchOwnerPlans = Object.freeze(plans.map(Object.freeze));
export const nativeFetchOwnerShards = Object.freeze(['cancel', 'retirement']);
export function nativeFetchOwnerCasesForShard(shard) {
    assert.ok(nativeFetchOwnerShards.includes(shard), 'mandatory native owner shard');
    return nativeFetchOwnerPlans.filter(plan => shard === 'cancel' ? plan.action.endsWith('cancel') : !plan.action.endsWith('cancel'));
}
export const nativeFetchOwnerInputPaths = Object.freeze({
    runner: 'tests/test_native_fetch_owner.mjs', fixture: 'tests/fixtures/native-fetch-owner.mjs',
    helper: 'tests/test_native_fetch_owner.cc', contract: 'tools/native-fetch-owner-contract.mjs',
    sourceModel: 'tools/native-fetch-owner-input.mjs', referenceArchive: 'tests/fixtures/workerd-native-fetch-owner-report.json',
});
// Only the two fixed immutable encoded input constructions are cached. Each
// caller receives fresh buffers; every current plan and report is validated.
const encodedModels = new Map();
export function nativeFetchOwnerReply(plan) {
    assert.deepEqual(plan, nativeFetchOwnerPlans.find(row => row.id === plan.id), 'complete fixed owner input');
    if (!encodedModels.has(plan.bytes)) {
        const payload = gzipSync(Buffer.alloc(plan.bytes)); payload[9] = 255;
        encodedModels.set(plan.bytes, payload.toString('base64'));
    }
    const payload = Buffer.from(encodedModels.get(plan.bytes), 'base64');
    const head = Buffer.from('HTTP/1.1 201 Created\r\nConnection: close\r\nContent-Type: application/octet-stream\r\nContent-Encoding: gzip\r\n'+
        'X-Decoded-Bytes: '+plan.bytes+'\r\nContent-Length: '+payload.length+'\r\n\r\n');
    return {payload, packet: Buffer.concat([head, payload])};
}
