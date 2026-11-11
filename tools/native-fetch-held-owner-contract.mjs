import assert from 'node:assert/strict';
import {createHash} from 'node:crypto';
import {heldCodecBranchBasePlans,heldCodecBranchReply} from './native-fetch-held-owner-input.mjs';
import {expectedState as codecState,readModel} from './native-fetch-held-branch-contract.mjs';
export {readModel};
export const plans=heldCodecBranchBasePlans.flatMap(base=>['unstarted','started-child','cancel-control'].map(style=>({...base,baseId:base.id,id:base.id+'-'+style,style})));
export const basePlan=plan=>heldCodecBranchBasePlans.find(item=>item.id===plan.baseId);
const fields=(value,names)=>assert.deepEqual(Object.keys(value).sort(),names.slice().sort());
const hash=bytes=>createHash('sha256').update(bytes).digest('hex');
export const gate={sourcePeerEnded:true,sourcePeerReset:false,fixtureEnded:false,fixtureDestroyed:false};
export function expectedState(plan) {
    assert.deepEqual(plan,plans.find(item=>item.id===plan.id));
    let state;
    if(plan.style==='cancel-control')state={style:plan.style,prefix:null,branches:[],failure:null,prepared:[{bodyUsed:false,locked:false},{bodyUsed:false,locked:false}],cancel:[null,null],outer:{bodyUsed:true,locked:true}};
    else state=codecState(plan);
    return {...state,ownerGate:{...gate},retainedAfterGate:[{bodyUsed:true,locked:false},{bodyUsed:true,locked:false}],originalAfterGate:{bodyUsed:true,locked:true}};
}
function source(value,row,plan) {
    fields(value,['request','rawRequest','peerEnded','peerReset','fixtureEnded','fixtureDestroyed','events','sent']);
    const request='GET /hold HTTP/1.1\r\nHost: localhost:'+row.port+'\r\n\r\n';
    assert.equal(value.request,request.slice(0,-4));assert.equal(value.rawRequest,Buffer.from(request).toString('base64'));
    assert.equal(value.peerEnded,true);assert.equal(value.peerReset,false);assert.equal(value.fixtureEnded,false);assert.equal(value.fixtureDestroyed,false);
    assert.deepEqual(value.events,['request','sent-held-http-chunk','peer-ended']);assert.equal(value.sent,heldCodecBranchReply(basePlan(plan)).packet.toString('base64'));
}
function common(row,plan) {
    assert.deepEqual(row.plan,plan);assert.deepEqual(row.state,expectedState(plan));
    assert.ok(Number.isSafeInteger(row.port)&&row.port>0&&row.port<65536);
    const reply=heldCodecBranchReply(basePlan(plan));
    assert.equal(row.encodedBytes,reply.payload.length);assert.equal(row.payloadSHA256,hash(reply.payload));assert.equal(row.packetSHA256,hash(reply.packet));
    fields(row.marker,['request','rawRequest','originalBoundMilliseconds','sourceBeforeReply','state']);
    const marker='GET /hold/marker HTTP/1.1\r\nHost: localhost:'+row.port+'\r\n\r\n';
    assert.equal(row.marker.request,marker.slice(0,-4));assert.equal(row.marker.rawRequest,Buffer.from(marker).toString('base64'));
    assert.equal(row.marker.originalBoundMilliseconds,3000);assert.deepEqual(row.marker.state,gate);source(row.marker.sourceBeforeReply,row,plan);
}
export function assertProductRow(row,plan) {
    fields(row,['plan','encodedBytes','payloadSHA256','packetSHA256','source','marker','port','state','sourceEndedBeforeFinish','sourceBeforeFinish','shutdown','stdout','stderr','workerExit']);
    common(row,plan);source(row.sourceBeforeFinish,row,plan);assert.deepEqual(row.sourceBeforeFinish,row.marker.sourceBeforeReply);assert.equal(row.sourceEndedBeforeFinish,true);
    assert.deepEqual(row.source,{...row.sourceBeforeFinish,fixtureDestroyed:true});
    assert.deepEqual(row.workerExit,{clean:true,processObserved:true,exitStatus:0,forced:false,errorEvents:0});assert.deepEqual(row.shutdown,{code:0,signal:null});assert.equal(row.stderr,'');
    const lines=row.stdout.trim().split(/\r?\n/);assert.deepEqual(lines.filter(s=>s.startsWith('LIMIT_PROBE ')),['LIMIT_PROBE '+JSON.stringify(row.state)]);
    assert.deepEqual(lines.filter(s=>s.startsWith('LIMIT_EXIT ')),['LIMIT_EXIT '+JSON.stringify(row.workerExit)]);
    assert.ok(lines.every(s=>s.startsWith('LIMIT_PROBE ')||s.startsWith('LIMIT_EXIT ')||/^LIMIT_AUX type=(?:6|10|11) request=\d+ status=\d+ payload=.*$/.test(s)));
}
export function assertReferenceRow(row,plan) {
    fields(row,['plan','collectionComplete','port','encodedBytes','payloadSHA256','packetSHA256','source','marker','httpStatus','state','sourceEndedBeforeReferenceStop','sourceClosedBeforeReferenceStop','sourceBeforeReferenceStop','healthy','sourceAfterCleanup']);
    common(row,plan);assert.equal(row.collectionComplete,true);assert.equal(row.httpStatus,200);source(row.sourceBeforeReferenceStop,row,plan);
    assert.deepEqual(row.sourceBeforeReferenceStop,row.marker.sourceBeforeReply);assert.equal(row.sourceEndedBeforeReferenceStop,true);assert.equal(row.sourceClosedBeforeReferenceStop,false);
    assert.deepEqual(row.sourceAfterCleanup,{...row.sourceBeforeReferenceStop,fixtureDestroyed:true});assert.deepEqual(row.source,row.sourceAfterCleanup);assert.deepEqual(row.healthy,{status:200,body:'healthy'});
}
