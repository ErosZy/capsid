import assert from 'node:assert/strict';
import {createHash} from 'node:crypto';

const routes = ['getReader','direct','prototype','subclass','reflect'];
const allocations = ['none','1','16','4096','8192'];
const lengths = ['empty','6','3,3','4095','4096','4097','2000,3000','8193'];
export const standardReaderCases = Object.freeze([
    ...['queue','pull'].flatMap(phase => allocations.flatMap(allocation => lengths.map(length =>
        ['standard','getReader','default',[phase,allocation,length].join('_')].join('|')))),
    ...routes.flatMap(route => ['close','cancel'].flatMap(action => allocations.map(allocation =>
        ['standard',route,'default',['buffer',action,allocation].join('_')].join('|')))),
    ...routes.flatMap(route => ['source','returned'].flatMap(target => ['transfer','transferToFixedLength'].flatMap(method =>
        ['none','0','8','16','24'].map(length => ['standard',route,'default',['transfer',target,method,length].join('_')].join('|'))))),
]);
const sha = bytes => createHash('sha256').update(bytes).digest('hex');
function runs(values) {
    const result = [];
    for (const value of values) {
        const last = result.at(-1);
        if (last && JSON.stringify(last.value) === JSON.stringify(value)) last.count++;
        else result.push({count:1,value});
    }
    return result;
}
export function expectedStandardReaderState(kind, challenge) {
    assert.ok(standardReaderCases.includes(kind));
    const [operation,a,b,c] = kind.split('|').at(-1).split('_');
    if (operation === 'queue' || operation === 'pull') {
        const capacity = a === 'none' ? 4096 : Number(a), sizes = b === 'empty' ? [] : b.split(',').map(Number);
        const total = sizes.reduce((sum,size) => sum+size,0), shapes = [];
        for (const size of operation === 'queue' ? [total] : sizes) for (let at=0;at<size;at+=capacity)
            shapes.push({done:false,length:Math.min(capacity,size-at),offset:0,capacity,outsideZero:true,type:'Uint8Array',sameRealm:true});
        shapes.push(operation === 'pull' && total === 0 ?
            {done:true,length:0,offset:0,capacity,outsideZero:true,type:'Uint8Array',sameRealm:true} :
            {done:true,length:null,offset:null,capacity:null,outsideZero:null,type:null,sameRealm:null});
        return {kind,challenge,unexpected:null,
            allocations:runs(operation==='queue'?[]:Array(Math.max(1,sizes.length)).fill(capacity)),
            enqueued:runs(sizes.map(() => ({bufferBytes:0,viewBytes:0}))),shapes:runs(shapes),reads:shapes.length,total,
            sha256:sha(Buffer.from(Array.from({length:total},(_,i)=>i%251))),locked:false};
    }
    if (operation === 'buffer') {
        const capacity = b === 'none' ? 4096 : Number(b), prefix = Array(Math.min(capacity,8)).fill(165);
        return {kind,challenge,unexpected:null,before:capacity,...(a==='cancel'?{inCancel:0}:{}),
            after:a==='close'?capacity:0,viewAfter:a==='close'?capacity:0,sameBuffer:false,
            read:{done:true,bytes:0,capacity},initialPrefix:prefix,initialSHA256:sha(Buffer.alloc(capacity,165)),
            afterCachedPrefix:a==='close'?prefix.map(()=>90):prefix,cachedAfterReturnedFirst:a==='close'?204:null,locked:false};
    }
    assert.equal(operation,'transfer');
    const count = c === 'none' ? 16 : Number(c), prefix=Array(16).fill(165), shared=count===16;
    return {kind,challenge,unexpected:null,
        before:{done:true,valueLength:0,source:16,returned:16,sameBuffer:false,sourcePrefix:prefix,returnedPrefix:prefix},
        available:true,error:null,after:{source:a==='source'?0:16,returned:a==='source'?16:0,
            sourceView:a==='source'?0:16,valueLength:0,moved:count,aliasPrefix:prefix,
            movedPrefix:Array.from({length:Math.min(count,20)},(_,i)=>i<16?165:0)},
        aliasAfterMovedWrite:Array(16).fill(shared?90:165),movedAfterAliasWrite:Array(Math.min(count,20)).fill(shared?204:90),locked:false};
}
