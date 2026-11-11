import assert from 'node:assert/strict';
import {gzipSync} from 'node:zlib';

const plans = [];
for (const limit of [0, 96, 97, 100]) plans.push({id: 'raw97-limit'+limit, bytes: 97, limit, view: 0});
for (const limit of [0, 96, 97, 100, 120]) plans.push({id: 'gzip97-limit'+limit, bytes: 97, limit, view: 0, gzip: true});
plans.push({id: 'gzip97-byob64-limit100', bytes: 97, limit: 100, view: 64, gzip: true});
for (const limit of [0, 8197, 9000]) plans.push({id: 'gzip8197-byob8192-limit'+limit, bytes: 8197, limit, view: 8192, gzip: true});
for (const view of [0, 8192]) plans.push({id: 'gzip8197-limit4096-view'+view, bytes: 8197, limit: 4096, view, gzip: true});
for (const limit of [0, 9000]) plans.push({id: 'raw8197-byob8192-limit'+limit, bytes: 8197, limit, view: 8192});
for (const operation of ['crc', 'truncated']) plans.push({id: 'gzip97-'+operation+'-limit100', bytes: 97, limit: 100, view: 0, gzip: true, operation});
assert.equal(plans.length, 19);
assert.equal(new Set(plans.map(plan => plan.id)).size, 19);
export const nativeFetchLimitPlans = Object.freeze(plans.map(Object.freeze));
export const nativeFetchLimitShards = Object.freeze(['boundaries', 'geometry']);
export function nativeFetchLimitCasesForShard(shard) {
    assert.ok(nativeFetchLimitShards.includes(shard), 'mandatory native limit shard');
    return shard === 'boundaries' ? nativeFetchLimitPlans.slice(0, 10) : nativeFetchLimitPlans.slice(10);
}
export const nativeFetchLimitInputPaths = Object.freeze({
    runner: 'tests/test_native_fetch_response_limits.mjs',
    fixture: 'tests/fixtures/native-fetch-response-limits.mjs',
    helper: 'tests/test_native_fetch_response_limits.cc',
    contract: 'tools/native-fetch-limit-contract.mjs',
    sourceModel: 'tools/native-fetch-limit-input.mjs',
    referenceArchive: 'tests/fixtures/workerd-native-fetch-limit-report.json',
});
export function nativeFetchLimitShape(plan) {
    return [plan.bytes, plan.view, plan.gzip ?? false, plan.operation ?? 'valid'].join('/');
}
export function nativeFetchLimitReply(plan) {
    const body = Buffer.from(Array.from({length: plan.bytes}, (_, index) => (index*29+17)%251));
    let payload = plan.gzip ? gzipSync(body) : body;
    if (plan.gzip) payload[9] = 255; // Controlled gzip OS byte on both actual platforms.
    if (plan.operation === 'crc') payload[payload.length-8] ^= 1;
    if (plan.operation === 'truncated') payload = payload.subarray(0, payload.length-5);
    const head = Buffer.from('HTTP/1.1 201 Created\r\nConnection: close\r\nContent-Type: application/octet-stream\r\n'+
        (plan.gzip ? 'Content-Encoding: gzip\r\n' : '')+'Content-Length: '+payload.length+'\r\n\r\n');
    return {body, payload, packet: Buffer.concat([head, payload])};
}
