import assert from 'node:assert/strict';
import {gzipSync} from 'node:zlib';
export const nativeFetchPendingPlans = Object.freeze(['raw', 'gzip'].flatMap(kind => ['getReader','direct','prototype','subclass','reflect'].flatMap(route =>
    ['default','read-Uint8Array','read-Uint16Array','least-Uint16Array','least-DataView'].flatMap(api =>
        ['overlap','cancel','abort'].map(action => Object.freeze({id:[kind,route,api,action].join('-'),kind,route,api,action}))))));
export const nativeFetchPendingPayload = Buffer.from(Array.from({length:16},(_,i)=>i+1));
export function nativeFetchPendingReply(plan) {
    const encoded = plan.kind === 'raw' ? Buffer.from(nativeFetchPendingPayload) : gzipSync(nativeFetchPendingPayload); if (plan.kind==='gzip') encoded[9]=255;
    const head = Buffer.from('HTTP/1.1 201 Created\r\nConnection: close\r\nContent-Type: application/octet-stream\r\n'+
        (plan.kind==='gzip'?'Content-Encoding: gzip\r\n':'')+'X-Pending-Case: '+plan.id+'\r\nContent-Length: '+encoded.length+'\r\n\r\n');
    return {head, encoded};
}
export const nativeFetchPendingShards=Object.freeze(['raw_overlap','raw_cancel','raw_abort','gzip_overlap','gzip_cancel','gzip_abort']);
export function nativeFetchPendingCasesForShard(shard) {
    assert.ok(nativeFetchPendingShards.includes(shard),'mandatory native pending shard');
    return nativeFetchPendingPlans.filter(plan=>plan.kind+'_'+plan.action===shard);
}
export const nativeFetchPendingInputPaths=Object.freeze({runner:'tests/test_native_fetch_pending.mjs',fixture:'tests/fixtures/native-fetch-pending.mjs',
    sourceModel:'tools/native-fetch-pending-input.mjs',peer:'tools/native-fetch-pending-peer.mjs',contract:'tools/native-fetch-pending-contract.mjs',
    controls:'tests/test_native_fetch_pending_contract.mjs',referenceCapture:'tools/capture-native-fetch-pending-reference.mjs',
    referenceArchive:'tests/fixtures/workerd-native-fetch-pending-report.json.gz',referenceMetadata:'tests/fixtures/workerd-native-fetch-pending-report.json',
    helper:'tests/test_native_fetch_response_limits.cc',exitObserver:'tests/graceful_worker_exit.h'});
export function expectedNativeFetchPendingState(plan) {
    assert.deepEqual(plan,nativeFetchPendingPlans.find(row=>row.id===plan.id));
    const byob=plan.api!=='default',derived=['subclass','reflect'].includes(plan.route),size=nativeFetchPendingReply(plan).encoded.length;
    const headers=[['connection','close'],...(plan.kind==='gzip'?[['content-encoding','gzip']]:[]),['content-length',String(size)],['content-type','application/octet-stream'],['x-pending-case',plan.id]];
    const meta=(used,locked)=>({status:201,bodyUsed:used,locked,headers:structuredClone(headers)});
    const error=(name,message)=>({name,message,sameReason:false,nativePrototype:true});
    const reason=()=>error('Error','native pending stop');
    const base=label=>({label,initialInputBytes:byob?24:null,returnedPromise:true,inputAfterIssue:byob?0:null,inputAfter:byob?0:null});
    const rejected=(label,why)=>({...base(label),error:why});
    const eof=label=>({...base(label),done:true,type:byob?'Uint8Array':null,offset:byob?0:null,length:byob?0:null,capacity:byob?24:null,bytes:byob?[]:null,backing:byob?Array(24).fill(165):null});
    const chunk=(label,offset)=>({...base(label),done:false,type:'Uint8Array',offset:byob?8:0,length:byob?8:16,capacity:byob?24:4096,
        bytes:[...nativeFetchPendingPayload.subarray(offset,offset+(byob?8:16))],backing:byob?[...Array(8).fill(165),...nativeFetchPendingPayload.subarray(offset,offset+8),...Array(8).fill(165)]:[...nativeFetchPendingPayload,...Array(4080).fill(0)]});
    const state={route:plan.route,api:plan.api,action:plan.action,failure:null,trace:[],tails:[],before:meta(false,false),
        construction:{instanceOfReader:true,constructorMatches:!derived,directPrototype:!derived,prototypeParent:derived},closedStable:true,
        second:rejected('second',error('TypeError','This ReadableStream only supports a single pending read request at a time.')),
        releaseWhilePending:{syncError:error('TypeError','Cannot call releaseLock() on a reader with outstanding read promises.')},
        preControl:{firstPending:true,closedPending:true,response:meta(true,true)},control:{status:200,text:'ok'}};
    if(plan.action==='overlap'){
        state.first=chunk('first',0);state.tails=byob?[chunk('tail0',8),eof('tail1')]:[eof('tail0')];state.closed={fulfilled:true};
        state.trace=byob?['second.rejected','first.fulfilled','tail0.fulfilled','closed.fulfilled','tail1.fulfilled']
            :['second.rejected','first.fulfilled','closed.fulfilled','tail0.fulfilled'];
    }else if(plan.action==='cancel'){
        state.closed={fulfilled:true};state.cancellation={fulfilled:true};state.first=rejected('first',reason());state.tails=[eof('tail0')];
        state.trace=['second.rejected','closed.fulfilled','cancel.fulfilled','first.rejected','tail0.fulfilled'];
    }else{
        state.aborted=true;state.closed={error:reason()};state.first=rejected('first',reason());state.tails=[rejected('tail0',reason())];
        state.trace=['second.rejected','abort.called','closed.rejected','first.rejected','tail0.rejected'];
    }
    state.released=true;state.after=meta(true,false);return state;
}
