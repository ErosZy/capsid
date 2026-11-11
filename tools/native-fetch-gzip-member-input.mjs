import assert from 'node:assert/strict';
import {gzipSync} from 'node:zlib';

const plans=[];
for(const pieces of [[4096,4096],[4096,17],[0,4096,4096],[4096,0,4096]])for(const view of [8192,16384])
    plans.push(Object.freeze({id:'members-'+pieces.join('-')+'-view'+view,bytes:pieces.reduce((a,b)=>a+b,0),view,gzip:true,pieces:Object.freeze(pieces.slice())}));
assert.equal(plans.length,8);assert.equal(new Set(plans.map(p=>p.id)).size,8);
export const nativeFetchGzipMemberPlans=Object.freeze(plans);
export const nativeFetchGzipMemberInputPaths=Object.freeze({runner:'tests/test_native_fetch_gzip_members.mjs',fixture:'tests/fixtures/native-fetch-gzip-members.mjs',
    helper:'tests/test_native_fetch_response_limits.cc',contract:'tools/native-fetch-gzip-member-contract.mjs',sourceModel:'tools/native-fetch-gzip-member-input.mjs',referenceArchive:'tests/fixtures/workerd-native-fetch-gzip-member-report.json'});
export function nativeFetchGzipMemberReply(plan){
    assert.deepEqual(plan,plans.find(p=>p.id===plan.id),'fixed gzip member inventory');
    const payload=Buffer.concat(plan.pieces.map(size=>{const bytes=gzipSync(Buffer.alloc(size));bytes[9]=255;return bytes;}));
    const head=Buffer.from('HTTP/1.1 201 Created\r\nConnection: close\r\nContent-Type: application/octet-stream\r\nContent-Encoding: gzip\r\nX-Decoded-Bytes: '+plan.bytes+'\r\nContent-Length: '+payload.length+'\r\n\r\n');
    return {payload,packet:Buffer.concat([head,payload])};
}
