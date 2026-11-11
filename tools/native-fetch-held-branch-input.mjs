import assert from 'node:assert/strict';
import {codecBranchBasePlans,codecBranchReply,codecBranchInputPaths} from './native-fetch-codec-branch-input.mjs';
const ids=['ordinary-codec-gzip-header-invalid','codec-edge-crc-97','codec-edge-crc-8197','codec-edge-member-second-crc','codec-supp-reserved','codec-supp-isize-invalid'];
export const heldCodecBranchBasePlans=Object.freeze(ids.map(id=>codecBranchBasePlans.find(plan=>plan.id===id)));
assert.ok(heldCodecBranchBasePlans.every(Boolean));
export const heldCodecBranchInputPaths=Object.freeze({...codecBranchInputPaths,heldSourceModel:'tools/native-fetch-held-branch-input.mjs'});
export function heldCodecBranchReply(plan) {
    assert.deepEqual(plan,heldCodecBranchBasePlans.find(item=>item.id===plan.id));
    const original=codecBranchReply(plan),at=original.packet.indexOf('\r\n\r\n');
    assert.ok(at>0);
    const head=original.packet.subarray(0,at).toString().replace(/Content-Length: [0-9]+/,'Transfer-Encoding: chunked');
    assert.ok(head.includes('Transfer-Encoding: chunked')&&!head.includes('Content-Length:'));
    const packet=Buffer.concat([Buffer.from(head+'\r\n\r\n'+original.payload.length.toString(16)+'\r\n'),original.payload,Buffer.from('\r\n')]);
    return {payload:original.payload,packet};
}
