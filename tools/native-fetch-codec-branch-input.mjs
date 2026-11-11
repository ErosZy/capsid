import assert from 'node:assert/strict';
import {httpCodecPlan,httpCodecReply,httpCodecPlainBytes} from './workerd-http-codec-input.mjs';

const ids=[
    'ordinary-codec-gzip-small',
    'ordinary-codec-gzip-header-invalid',
    'codec-edge-crc-97','codec-edge-crc-4096','codec-edge-crc-4097','codec-edge-crc-8197',
    'codec-edge-truncated-97','codec-edge-truncated-4096','codec-edge-truncated-8197',
    'codec-edge-member-second-crc','codec-edge-member-second-truncated','codec-edge-trailing-byte',
    'codec-supp-reserved','codec-supp-isize-invalid',
];
assert.equal(ids.length,14);assert.equal(new Set(ids).size,14);
export const codecBranchBasePlans=Object.freeze(ids.map(id=>Object.freeze({id,bytes:httpCodecPlan(id).bytes,
    operation:httpCodecPlan(id).operation??'valid',view:8192})));
export const codecBranchInputPaths=Object.freeze({
    sourceModel:'tools/workerd-http-codec-input.mjs',
    branchSourceModel:'tools/native-fetch-codec-branch-input.mjs',
    branchFixture:'tests/fixtures/native-fetch-encoded-branches.mjs',
    helper:'tests/test_native_fetch_response_limits.cc',
});
export function codecBranchReply(plan) {
    assert.deepEqual(plan,codecBranchBasePlans.find(item=>item.id===plan.id));
    const {payload,packet}=httpCodecReply(plan.id,'branch-codec-r1');
    return {payload:Buffer.from(payload),packet:Buffer.from(packet)};
}
export function codecBranchPlain(plan) {return httpCodecPlainBytes(plan.id);}
