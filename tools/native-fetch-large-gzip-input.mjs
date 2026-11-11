import assert from 'node:assert/strict';
import {gzipSync,gunzipSync,constants} from 'node:zlib';
import {createHash} from 'node:crypto';
const MiB=1048576,hash=b=>createHash('sha256').update(b).digest('hex');
export const nativeFetchLargeReferencePlans=Object.freeze([{id:'raw1m-view1m',bytes:MiB,view:MiB},{id:'gzip8m-view1m',bytes:8*MiB,view:MiB,gzip:true},{id:'gzip96m-view1m',bytes:96*MiB,view:MiB,gzip:true}].map(Object.freeze));
export const nativeFetchLargeGzipPlans=Object.freeze(nativeFetchLargeReferencePlans.slice(1));
export const nativeFetchLargeGzipInputPaths=Object.freeze({runner:'tests/test_native_fetch_large_gzip.mjs',fixture:'tests/fixtures/native-fetch-gzip-members.mjs',helper:'tests/test_native_fetch_response_limits.cc',
    contract:'tools/native-fetch-large-gzip-contract.mjs',sourceModel:'tools/native-fetch-large-gzip-input.mjs',referenceArchive:'tests/fixtures/workerd-native-fetch-large-gzip-report.json'});
const replies=new Map(),lengthModels=new Map();
for(const plan of nativeFetchLargeReferencePlans){
    const plain=Buffer.alloc(plan.bytes),payload=plan.gzip?gzipSync(plain):plain;if(plan.gzip)payload[9]=255;
    const head=Buffer.from('HTTP/1.1 201 Created\r\nConnection: close\r\nContent-Type: application/octet-stream\r\n'+(plan.gzip?'Content-Encoding: gzip\r\n':'')+'X-Decoded-Bytes: '+plan.bytes+'\r\nContent-Length: '+payload.length+'\r\n\r\n');
    replies.set(plan.id,{payload,packet:Buffer.concat([head,payload])});
    if(plan.gzip){
        const lengths=[];let previous=0;
        // The first4KiB wire read includes the exact HTTP response header.
        // Independently decode the remaining body window and subsequent16KiB input windows with
        // Node zlib. Split only each producer opportunity by caller capacity.
        for(let end=4096-head.length;;end+=16384){
            const at=Math.min(end,payload.length),decoded=at===payload.length?plan.bytes:gunzipSync(payload.subarray(0,at),{finishFlush:constants.Z_SYNC_FLUSH}).length;
            let bytes=decoded-previous;assert.ok(bytes>0);previous=decoded;
            while(bytes){const take=Math.min(bytes,plan.view);lengths.push(take);bytes-=take;}
            if(at===payload.length)break;
        }
        lengths.push(0);lengthModels.set(plan.id,Object.freeze(lengths));
    }
}
export function nativeFetchLargeGzipReply(plan){assert.deepEqual(plan,nativeFetchLargeReferencePlans.find(p=>p.id===plan.id));const value=replies.get(plan.id);return {payload:Buffer.from(value.payload),packet:Buffer.from(value.packet)};}
export function expectedNativeFetchLargeGzipLengths(plan){assert.ok(lengthModels.has(plan.id));return lengthModels.get(plan.id);}
// These are bounded immutable construction models, never validation results.
const byteModels=new Map();
for(const lengths of lengthModels.values())for(const length of lengths){
    const done=length===0,key=String(length);if(byteModels.has(key))continue;const backing=Buffer.alloc(MiB+16,165);backing.fill(0,done?0:8,(done?0:8)+length);
    byteModels.set(key,Object.freeze({done,inputAfterIssue:0,inputAfter:0,type:'Uint8Array',offset:done?0:8,length,capacity:MiB+16,dataHash:hash(Buffer.alloc(length)),backingHash:hash(backing)}));
}
export function expectedNativeFetchLargeGzipState(plan){
    const reply=nativeFetchLargeGzipReply(plan);
    return {failure:null,reads:expectedNativeFetchLargeGzipLengths(plan).map(length=>({...byteModels.get(String(length))})),total:plan.bytes,
        headers:[['connection','close'],['content-encoding','gzip'],['content-length',String(reply.payload.length)],['content-type','application/octet-stream'],['x-decoded-bytes',String(plan.bytes)]],
        expectedBytes:plan.bytes,closedStable:true,closed:{fulfilled:true},after:{bodyUsed:true,locked:false}};
}
