import assert from 'node:assert/strict';
import {createHash} from 'node:crypto';
import {heldCodecBranchBasePlans as codecBranchBasePlans,heldCodecBranchReply as codecBranchReply} from './native-fetch-held-branch-input.mjs';
import {codecBranchPlain} from './native-fetch-codec-branch-input.mjs';
import {httpCodecPlan} from './workerd-http-codec-input.mjs';

export const plans=codecBranchBasePlans.flatMap(base=>['unstarted','started-child'].map(style=>({...base,baseId:base.id,id:base.id+'-'+style,style})));
export const basePlan=plan=>codecBranchBasePlans.find(item=>item.id===plan.baseId);
const hash=bytes=>createHash('sha256').update(bytes).digest('hex');
const fields=(value,names)=>assert.deepEqual(Object.keys(value).sort(),names.slice().sort());
export function readModel(data,capacity,done=false) {
    const backing=Buffer.alloc(capacity+16,165),offset=done?0:8;data.copy(backing,offset);
    return {done,inputAfterIssue:0,inputAfter:0,type:'Uint8Array',offset,length:data.length,capacity:capacity+16,
        dataHash:hash(data),backingHash:hash(backing)};
}
export function expectedState(plan) {
    assert.deepEqual(plan,plans.find(item=>item.id===plan.id));
    const base=basePlan(plan),body=codecBranchPlain(base),operation=base.operation;
    const child=plan.style==='started-child',skip=child?32:0;
    const failure=operation==='valid'?null:{name:'TypeError',message:['truncated','member-second-truncated','trailing-byte'].includes(operation)
        ?'Gzip compressed stream ended prematurely.':'Gzip decompression failed.'};
    if(child&&['header-invalid','reserved'].includes(operation))return {style:plan.style,prefix:null,branches:[],failure};
    const split=length=>{const result=[];while(length){const count=Math.min(length,plan.view);result.push(count);length-=count;}return result;};
    const sizes=index=>{
        if(operation==='valid'||operation==='truncated'||operation==='trailing-byte')return split(body.length-skip);
        if(['crc','isize-invalid'].includes(operation))return split(Math.max(0,Math.floor((body.length-skip-1)/plan.view)*plan.view));
        if(['header-invalid','reserved'].includes(operation))return [];
        const first=httpCodecPlan(base.id).split-skip;
        if(operation==='member-second-crc')return [first];
        if(operation==='member-second-truncated')return child&&index===1?[body.length-skip]:[first,body.length-httpCodecPlan(base.id).split];
        throw new Error('Unknown independent codec input');
    };
    const head=codecBranchReply(base).packet.toString('latin1').split('\r\n\r\n')[0].split('\r\n');
    assert.equal(head.shift(),'HTTP/1.1 201 Created');
    const headers=head.map(line=>{const at=line.indexOf(':');return [line.slice(0,at).toLowerCase(),line.slice(at+1).trim()];}).sort(([a],[b])=>a<b?-1:a>b?1:0);
    const branches=[0,1].map(index=>{
        let cursor=skip;const reads=sizes(index).map(length=>{const data=body.subarray(cursor,cursor+length);cursor+=length;return readModel(data,plan.view);});
        if(!failure)reads.push(readModel(Buffer.alloc(0),plan.view,true));
        return {failure,reads,total:cursor-skip,status:201,statusText:'Created',headers,
            closedStable:true,closed:failure?{error:failure}:{fulfilled:true},after:{bodyUsed:true,locked:false}};
    });
    return {style:plan.style,prefix:child?{done:false,bytes:[...body.subarray(0,32)],offset:0,capacity:32,inputAfterIssue:0,inputAfter:0}:null,
        branches,failure:null,prepared:[{bodyUsed:false,locked:false},{bodyUsed:false,locked:false}],outer:{bodyUsed:true,locked:true}};
}
function source(value,row,plan) {
    fields(value,['request','rawRequest','peerEnded','peerReset','fixtureEnded','fixtureDestroyed','events','sent']);
    assert.ok(Number.isSafeInteger(row.port)&&row.port>0&&row.port<65536);
    const request='GET /hold HTTP/1.1\r\nHost: localhost:'+row.port+'\r\n\r\n';
    assert.equal(value.request,request.slice(0,-4));assert.equal(value.rawRequest,Buffer.from(request).toString('base64'));
    assert.equal(value.peerEnded,true);assert.equal(value.peerReset,false);assert.equal(value.fixtureEnded,false);assert.equal(value.fixtureDestroyed,false);
    assert.deepEqual(value.events,['request','sent-held-http-chunk','peer-ended']);assert.equal(value.sent,codecBranchReply(basePlan(plan)).packet.toString('base64'));
}
function common(row,plan) {
    assert.deepEqual(row.plan,plan);assert.deepEqual(row.state,expectedState(plan));
    const reply=codecBranchReply(basePlan(plan));
    assert.equal(row.encodedBytes,reply.payload.length);assert.equal(row.payloadSHA256,hash(reply.payload));assert.equal(row.packetSHA256,hash(reply.packet));
}
export function assertProductRow(row,plan) {
    fields(row,['plan','encodedBytes','payloadSHA256','packetSHA256','source','port','state','sourceEndedBeforeFinish','sourceBeforeFinish','shutdown','stdout','stderr','workerExit']);
    common(row,plan);source(row.sourceBeforeFinish,row,plan);assert.deepEqual(row.source,{...row.sourceBeforeFinish,fixtureDestroyed:true});assert.equal(row.sourceEndedBeforeFinish,true);
    assert.deepEqual(row.workerExit,{clean:true,processObserved:true,exitStatus:0,forced:false,errorEvents:0});
    assert.deepEqual(row.shutdown,{code:0,signal:null});assert.equal(row.stderr,'');
    const lines=row.stdout.trim().split(/\r?\n/);assert.deepEqual(lines.filter(s=>s.startsWith('LIMIT_PROBE ')),['LIMIT_PROBE '+JSON.stringify(row.state)]);
    assert.deepEqual(lines.filter(s=>s.startsWith('LIMIT_EXIT ')),['LIMIT_EXIT '+JSON.stringify(row.workerExit)]);
    assert.ok(lines.every(s=>s.startsWith('LIMIT_PROBE ')||s.startsWith('LIMIT_EXIT ')||/^LIMIT_AUX type=(?:6|10|11) request=\d+ status=\d+ payload=.*$/.test(s)));
}
export function assertReferenceRow(row,plan) {
    fields(row,['plan','collectionComplete','port','encodedBytes','payloadSHA256','packetSHA256','httpStatus','state','sourceEndedBeforeReferenceStop','sourceClosedBeforeReferenceStop','sourceBeforeReferenceStop','healthy','sourceAfterCleanup']);
    common(row,plan);assert.equal(row.collectionComplete,true);assert.equal(row.httpStatus,200);source(row.sourceBeforeReferenceStop,row,plan);
    assert.equal(row.sourceEndedBeforeReferenceStop,true);assert.equal(row.sourceClosedBeforeReferenceStop,false);assert.deepEqual(row.sourceAfterCleanup,{...row.sourceBeforeReferenceStop,fixtureDestroyed:true});
    assert.deepEqual(row.healthy,{status:200,body:'healthy'});
}
