import assert from 'node:assert/strict';

const routes=['getReader','direct'],phases=['queued','pending'];
const widths={Uint8Array:1,Uint16Array:2,Int32Array:4,Float64Array:8};
const lengths=[0,1,3,4,5,7,8,9,12,15,16],methods=['default','read','least'];
export const standardCloseCases=Object.freeze(routes.flatMap(route=>phases.flatMap(phase=>
    Object.keys(widths).map(view=>`standard-close-${route}-${phase}-${view}`))));
const cases=new Set(standardCloseCases);
const error=message=>({error:{name:'Error',message}});
const value=(done,type,offset,bytes,backing)=>({done,type,offset,bytes,backing});
function selection(kind){
    assert.ok(cases.has(kind),'reviewed standard close scenario');
    const [,,route,phase,type]=kind.split('-');return {route,phase,type,width:widths[type]};
}
function failsAtQueuedEOF(phase,length,minimum,width){
    return phase==='queued'&&length>0&&length<minimum&&length>=width&&length%width!==0;
}
export function standardCloseDiagnosticReferences(observed,kind){
    if(!cases.has(kind))return [];
    const {phase,width}=selection(kind),ids=[];
    for(let index=0;index<lengths.length*methods.length;index++){
        const length=lengths[Math.floor(index/methods.length)],method=methods[index%methods.length];
        const minimum=(method==='default'?1:2)*width;
        if(!failsAtQueuedEOF(phase,length,minimum,width))continue;
        const message=observed?.rows?.[index]?.read?.error?.message;
        assert.equal(typeof message,'string');const match=/^internal error; reference = ([0-9a-v]{24})$/.exec(message);
        assert.ok(match,'fresh standard-close EOF diagnostic');ids.push(match[1]);
    }
    return ids;
}
function expectedRow(phase,type,width,length,method,diagnostic){
    const minimum=(method==='default'?1:2)*width,payload=Array.from({length},(_,i)=>i+1);
    const aligned=length-length%width,failed=failsAtQueuedEOF(phase,length,minimum,width);
    const queuedRemainder=phase==='queued'&&length>=minimum&&length%width!==0;
    const copied=phase==='pending'||length<minimum?length:aligned;
    const backing=[...Array(8).fill(165),...payload.slice(0,copied),...Array(56-copied).fill(165)];
    const read=failed?error(diagnostic):{value:value(length===0,type,8,payload.slice(0,aligned),backing)};
    const eofBytes=queuedRemainder?payload.slice(aligned):[];
    const eof=failed?null:{value:value(!queuedRemainder,'Uint8Array',0,eofBytes,[...eofBytes,...Array(32-eofBytes.length).fill(0)])};
    const closed=failed?error(diagnostic):queuedRemainder?{pending:true}:{value:{fulfilled:true}};
    const beforeTrace=failed?['source-close','closed-error','read-error']:
        phase==='pending'||queuedRemainder?['source-close','read','closed']:['source-close','closed','read'];
    return {length,method,trace:[...beforeTrace,'cleanup'],read,eof,closed,closeThrow:null,
        cancel:failed?error(diagnostic):{returnedUndefined:true},unexpected:null,
        prime:value(false,'Uint8Array',0,[0],[0]),detached:true,
        beforeCleanup:{read,closed,trace:beforeTrace},errorIdentity:{readClosed:true,readCancel:true},lockedAfterRelease:false};
}
export function assertStandardCloseState(observed,kind,challenge,stderr){
    const {phase,type,width}=selection(kind),ids=standardCloseDiagnosticReferences(observed,kind);
    const logged=[...stderr.matchAll(/(?:wdErrId = |reference = )([0-9a-v]{24})(?![0-9a-v])/g)].map(match=>match[1]);
    assert.deepEqual(logged,ids,'every actual EOF diagnostic is logged once in scenario order');
    assert.equal(new Set(ids).size,ids.length,'independent EOF errors have fresh diagnostic references');
    let diagnostic=0;
    const rows=lengths.flatMap(length=>methods.map(method=>{
        const minimum=(method==='default'?1:2)*width;
        const message=failsAtQueuedEOF(phase,length,minimum,width)?'internal error; reference = '+ids[diagnostic++]:undefined;
        return expectedRow(phase,type,width,length,method,message);
    }));
    assert.deepEqual(observed,{kind,challenge,rows,unexpected:null},'independent EOF timing, complete backing and partial-element model');
    return ids;
}
