import assert from 'node:assert/strict';
import {test,after} from 'node:test';
import {bindingQuotaVariants,bindingQuotaInputPaths,requestQuotaExpected,requestQuotaTemplate,workerQuotaExpected,workerQuotaTemplate,
    bindingQuotaExit,requestQuotaTranscript,workerQuotaTranscript} from '../tools/binding-quota-input.mjs';
import {assertRequestQuotaObservation,assertWorkerQuotaObservation,validateBindingQuotaReport} from '../tools/binding-quota-contract.mjs';

let rejected=0;
const reject=fn=>{assert.throws(fn);rejected++;};
const transcript=(row,kind,platform)=>kind==='request'?requestQuotaTranscript(row,platform):workerQuotaTranscript(row,platform);
const observation=(kind,variant='plain',platform='win32')=>{
    const value=kind==='request'?{variant,...requestQuotaExpected(variant)}:workerQuotaExpected();
    Object.assign(value,{stdout:'',stderr:'',workerExit:bindingQuotaExit(),child:{code:0,signal:null}});
    value.stdout=transcript(value,kind,platform);return value;
};
const check=(row,kind,variant='plain',platform='win32')=>kind==='request'?assertRequestQuotaObservation(row,variant,platform):assertWorkerQuotaObservation(row,platform);
const bad=(row,kind,change,variant='plain',sync=true)=>{
    const value=structuredClone(row);change(value);if(sync)value.stdout=transcript(value,kind,'win32');reject(()=>check(value,kind,variant));
};
const set=(value,path,item)=>{let parent=value;for(const key of path.slice(0,-1))parent=parent[key];parent[path.at(-1)]=item;};
const del=(value,path)=>{let parent=value;for(const key of path.slice(0,-1))parent=parent[key];delete parent[path.at(-1)];};

test('fixed64/request and1024/worker models and all three native platform proof formats',()=>{
    for(const platform of ['win32','linux','darwin']){
        for(const variant of bindingQuotaVariants)check(observation('request',variant,platform),'request',variant,platform);
        const row=observation('worker','plain',platform);check(row,'worker','plain',platform);
        assert.equal(row.responses.at(-1).value.slices.length,17);assert.equal(row.responses.at(-1).value.late.length,1088);
        assert.equal(row.logs.length,34);assert.equal(row.events.length,80);
    }
});

test('request refusal precedes cloning and completion recovers the64th slot for every input kind',()=>{
    for(const variant of bindingQuotaVariants){
        const row=observation('request',variant);
        for(const key of Object.keys(row.request1))bad(row,'request',value=>{delete value.request1[key];},variant);
        for(const [path,item] of [
            [['request1','overflowPhase'],'synchronous'],[['request1','observed'],1],[['request1','overflow','accepted'],true],
            [['request1','overflow','message'],'binding call quota exceeded'],[['request1','before','data','heldSignalsAborted',0],true],
            [['request1','recovered','data','heldCount'],64],[['request1','recovered','data','call','requestId'],'2']])bad(row,'request',value=>set(value,path,item),variant);
        for(const at of [0,31,62]){
            for(const key of Object.keys(row.request2.stats.data.held[at].call))bad(row,'request',value=>{delete value.request2.stats.data.held[at].call[key];},variant);
            for(const key of Object.keys(row.request2.stats.data.aborted[at].reason))bad(row,'request',value=>{delete value.request2.stats.data.aborted[at].reason[key];},variant);
            for(const [path,item] of [
                [['request2','stats','data','heldSignalsAborted',at],false],[['request2','stats','data','aborted',at,'event','targetSame'],false],
                [['request2','late',at,'status'],'fulfilled'],[['request2','late',at,'id'],-1]])bad(row,'request',value=>set(value,path,item),variant);
        }
        for(const [path,item] of [[['request2','stats','types','objectNullPrototypes'],false],[['request2','stats','types','arrayPrototypes'],false],
            [['request2','stats','data','completedSignalsAborted',0],true],[['request2','stats','data','call','requestId'],'1']])bad(row,'request',value=>set(value,path,item),variant);
    }
});

test('worker refusal, one-parent cancellation,960 unaffected calls and64-slot refill',()=>{
    const row=observation('worker');
    for(let index=0;index<5;index++)for(const [path,item] of [[['phase'],'synchronous'],[['observed'],1],[['result','accepted'],true],[['result','message'],'binding request quota exceeded']])
        bad(row,'worker',value=>set(value,['responses',index,'value',...path],item));
    const before=['responses',5,'value','recoveryBefore'];
    for(const [path,item] of [[['summary','data','active'],959],[['summary','data','peak'],1023],[['summary','data','total'],1023],
        [['summary','data','groups',0,'aborted'],0],[['summary','data','groups',8,'aborted'],1],[['summary','data','completedSignalsAborted',0],true],
        [['summary','data','call','requestId'],'1'],[['late',63,'parent'],'21'],[['late',0,'status'],'fulfilled']])bad(row,'worker',value=>set(value,[...before,...path],item));
    const final=['responses',5,'value'];
    for(const [path,item] of [[['active'],1],[['peak'],1025],[['total'],1087],[['completedSignalsAborted',0],true],[['call','requestId'],'21']])
        bad(row,'worker',value=>set(value,[...final,'summary','data',...path],item));
    for(let index=0;index<17;index++){
        bad(row,'worker',value=>{value.responses[5].value.summary.data.groups[index].aborted=63;});
        for(const at of [0,31,63])for(const [path,item] of [[['call','requestId'],'0'],[['aborted'],false]])
            bad(row,'worker',value=>set(value,[...final,'slices',index,'data','items',at,...path],item));
    }
});

test('all controller/context/reason/event fields and real clone prototypes at representative boundaries',()=>{
    const row=observation('worker'),root=['responses',5,'value'];
    for(const index of [0,8,16]){
        const prefix=[...root,'slices',index];
        for(const key of Object.keys(row.responses[5].value.slices[index].types))bad(row,'worker',value=>del(value,[...prefix,'types',key]));
        for(const at of [0,31,63]){
            const item=row.responses[5].value.slices[index].data.items[at],path=[...prefix,'data','items',at];
            for(const key of Object.keys(item))bad(row,'worker',value=>del(value,[...path,key]));
            for(const key of Object.keys(item.call))bad(row,'worker',value=>del(value,[...path,'call',key]));
            for(const key of Object.keys(item.reason))bad(row,'worker',value=>del(value,[...path,'reason',key]));
            for(const key of Object.keys(item.abortEvents[0]))bad(row,'worker',value=>del(value,[...path,'abortEvents',0,key]));
            bad(row,'worker',value=>set(value,[...path,'reason','code'],true));
        }
    }
    for(const at of [0,63,64,1023,1024,1087])for(const [path,item] of [[['parent'],'0'],[['slot'],-1],[['status'],'fulfilled']])
        bad(row,'worker',value=>set(value,[...root,'late',at,...path],item));
});

test('complete binary log payloads,80-event order, no canceled response frames and actual exit',()=>{
    const row=observation('worker');
    for(const change of [value=>{value.logs[16].raw='\x03\x00logquota-aborted:2:64';},value=>{value.logs[0].request=1;},
        value=>{value.events[40].request=2;},value=>{value.events[41].index=15;},value=>{value.responses[0].request=1;},
        value=>{value.events.push({type:'canceled-event',request:1,eventType:4});},value=>{value.events.pop();},value=>{value.logs.pop();}])bad(row,'worker',change,'plain',false);
    for(const kind of ['request','worker']){
        const value=observation(kind);
        for(const change of [item=>{item.child.code=1;},item=>{item.workerExit.code=1;},item=>{item.stderr='unexpected';},item=>{item.stdout+='extra\n';}])bad(value,kind,change,'plain',false);
        reject(()=>check(value,kind,'plain','linux'));
    }
});

const syntheticReport=kind=>{
    const inputs=Object.fromEntries(Object.keys(bindingQuotaInputPaths).map(key=>[key,'a'.repeat(64)]));
    return {schema:'capsid-binding-quota-v1',complete:true,runId:'synthetic-binding-quota',kind,platform:'win32',inputs,
        binaries:{helper:'b'.repeat(64),worker:'c'.repeat(64)},observations:kind==='request'?bindingQuotaVariants.map(variant=>observation(kind,variant)):[observation(kind)]};
};
const identity=report=>({runId:report.runId,kind:report.kind,platform:report.platform,inputs:structuredClone(report.inputs),binaries:structuredClone(report.binaries)});
test('full current report/provenance and repeated mutable validation',()=>{
    for(const kind of ['request','worker']){
        const report=syntheticReport(kind),expected=identity(report);validateBindingQuotaReport(report,expected);
        for(const key of Object.keys(report)){const value=structuredClone(report);delete value[key];reject(()=>validateBindingQuotaReport(value,expected));}
        for(const change of [value=>{value.complete=1;},value=>{value.runId='wrong';},value=>{value.kind='wrong';},value=>{value.platform='linux';},
            value=>{value.inputs.workerRuntime='d'.repeat(64);},value=>{value.binaries.worker='d'.repeat(64);},value=>{value.observations.pop();}]){
            const value=structuredClone(report);change(value);reject(()=>validateBindingQuotaReport(value,expected));
        }
        if(kind==='request')report.observations[0].request2.stats.data.call.signalNew=false;
        else report.observations[0].responses[5].value.slices[16].data.items[63].call.signalNew=false;
        report.observations[0].stdout=transcript(report.observations[0],kind,'win32');reject(()=>validateBindingQuotaReport(report,expected));
    }
});

test('fixed immutable input templates do not accept caller changes',()=>{
    assert.ok(Object.isFrozen(requestQuotaTemplate('plain').request2.stats.data.held[0].call));
    assert.ok(Object.isFrozen(workerQuotaTemplate().responses[5].value.slices[16].data.items[63].abortEvents));
    assert.throws(()=>{requestQuotaTemplate('plain').request2.stats.data.held[0].call.signalNew=false;},TypeError);
    assert.throws(()=>{workerQuotaTemplate().responses[5].value.late.pop();},TypeError);
    const first=requestQuotaExpected('plain');first.request2.late[0].status='fulfilled';assert.equal(requestQuotaExpected('plain').request2.late[0].status,'rejected');
    const second=workerQuotaExpected();second.responses[5].value.summary.data.active=1;assert.equal(workerQuotaExpected().responses[5].value.summary.data.active,0);
});

test('Darwin complete reports require POSIX READY features and reject unsupported platforms',()=>{
    for(const kind of ['request','worker']){
        const report=syntheticReport(kind);report.platform='darwin';
        for(const row of report.observations)row.stdout=transcript(row,kind,'darwin');
        validateBindingQuotaReport(report,identity(report));
        const wrong=structuredClone(report);
        wrong.observations[0].stdout=wrong.observations[0].stdout.replace('actualFeatures=1 expectedFeatures=1','actualFeatures=0 expectedFeatures=0');
        reject(()=>validateBindingQuotaReport(wrong,identity(report)));
        const unsupported=structuredClone(report);unsupported.platform='freebsd';
        reject(()=>validateBindingQuotaReport(unsupported,identity(unsupported)));
    }
});
after(()=>{assert.equal(rejected,681);console.log(JSON.stringify({complete:true,rejectedMutations:rejected,requestVariants:4,workerPeak:1024}));});
