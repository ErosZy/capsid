import assert from 'node:assert/strict';
import {test,after} from 'node:test';
import {readReaderScopeSample} from './helpers/ws_reader_scope_sample.mjs';
import {readerScopeCases,assertReaderScopeCase,validateReaderScopeReport,compareReaderScopeReports} from '../tools/workerd-reader-scope-contract.mjs';
import {referenceBackendModes} from '../tools/workerd-platform.mjs';
const sample=readReaderScopeSample();
const expected={runId:sample.runId,lock:sample.referenceLock,inputs:sample.inputs,host:'1'.repeat(64),worker:'2'.repeat(64)};
const inventory=['native','standard'].flatMap(source=>['getReader','direct','prototype','subclass','reflect'].flatMap(route=>
    ['default','byob'].flatMap(reader=>['unread','primed'].flatMap(phase=>['read','cancel','release','closed'].map(action=>
        [source,route,reader,'foreign_'+action+'_'+phase].join('|'))))));
test('160 explicit source, construction, reader, phase and foreign operation combinations',()=>assert.deepEqual(readerScopeCases,inventory));
test('actual pinned reference passes full independent final, actor, wire and source cleanup models',()=>assert.equal(validateReaderScopeReport(sample,expected).length,160));
let stateMutations=0;
function leaves(value,prefix=[]){
    if(value===null||typeof value!=='object')return [prefix];
    if(Object.keys(value).length===0)return [prefix];
    return Object.entries(value).flatMap(([key,item])=>leaves(item,[...prefix,key]));
}
function changed(value){return value===null?false:typeof value==='boolean'?!value:typeof value==='number'?value+1:String(value)+' altered';}
for(const row of sample.cases){
    test('actual reference '+row.kind,()=>assertReaderScopeCase(row,'reference'));
    for(const observation of ['actor','observed'])test('every leaf, missing field and extra field is rejected '+observation+' '+row.kind,()=>{
        const original=JSON.parse(row[observation].text);
        for(const parts of leaves(original.state)){
            const mutant=structuredClone(row),document=JSON.parse(mutant[observation].text);let target=document.state;
            for(const key of parts.slice(0,-1))target=target[key];const key=parts.at(-1);target[key]=changed(target[key]);
            mutant[observation].text=JSON.stringify(document);assert.throws(()=>assertReaderScopeCase(mutant,'reference'),parts.join('.'));stateMutations++;
        }
        for(const key of [...Object.keys(original.state),'unexpected_extra']){
            const mutant=structuredClone(row),document=JSON.parse(mutant[observation].text);
            if(key==='unexpected_extra')document.state[key]=true;else delete document.state[key];
            mutant[observation].text=JSON.stringify(document);assert.throws(()=>assertReaderScopeCase(mutant,'reference'),key);stateMutations++;
        }
    });
    for(const [name,mutate]of [
        ['foreign request failed',value=>{value.actor.status=503;}],
        ['foreign request changed worker',value=>{const document=JSON.parse(value.actor.text);document.realm='11111111-1111-4111-8111-111111111111';value.actor.text=JSON.stringify(document);}],
        ['follow-up request changed worker',value=>{const document=JSON.parse(value.observed.text);document.realm='11111111-1111-4111-8111-111111111111';value.observed.text=JSON.stringify(document);}],
        ['missing readiness wire message',value=>value.wire.frames.splice(1,1)],
        ['changed raw downstream bytes',value=>{value.wire.rawTail='';}],
        ['forced process cleanup',value=>{value.shutdown.forced=true;}],
        ['source not ended before stop',value=>{value.allSourcesEndedBeforeStop=false;}],
        ['source completion supplied by shutdown',value=>value.upstreamAfterStop.tcpRecords.push({closed:false})],
        ['actual upstream bytes changed',value=>{if(value.kind.startsWith('native|'))value.upstreamBeforeStop.records[0].sentChunks.pop();else value.upstreamBeforeStop.records.push({request:'unexpected I/O'});}],
        ['runtime error',value=>{value.stderr+='\nWORKER POISONED';}],
        ['capture failure',value=>{value.failure='failure';}],
    ])test('reject '+name+' '+row.kind,()=>{const mutant=structuredClone(row);mutate(mutant);assert.throws(()=>assertReaderScopeCase(mutant,'reference'));});
}
for(const [name,mutate]of [
    ['incomplete',report=>{report.complete=false;}],['wrong schema',report=>{report.schema='wrong';}],
    ['stale run',report=>{report.runId='old-run';}],['missing case',report=>report.cases.pop()],
    ['duplicate case',report=>{report.cases[1]=structuredClone(report.cases[0]);}],['reordered cases',report=>report.cases.reverse()],
    ['stale source',report=>{report.inputs.fixture='0'.repeat(64);}],['unknown input',report=>{report.inputs.extra='0'.repeat(64);}],
    ['stale binary',report=>{report.binaries.host='0'.repeat(64);}],['wrong client',report=>{report.client.node='v0.0.0';}],
    ['reused challenge',report=>{report.cases[1].challenge=report.cases[0].challenge;}],
    ['reused worker identity',report=>{report.cases[1].initial.text=report.cases[0].initial.text;}],
    ['reused wire identity',report=>{report.cases[1].headers.requestKey=report.cases[0].headers.requestKey;}],
])test('report rejects '+name,()=>{const mutant=structuredClone(sample);mutate(mutant);assert.throws(()=>validateReaderScopeReport(mutant,expected));});
test('all product backends are required and identical wrong observations cannot pass',()=>{
    const candidates=referenceBackendModes(expected.lock).map(mode=>({...structuredClone(sample),mode,binaries:{host:expected.host,worker:expected.worker},
        cases:sample.cases.map(row=>({...structuredClone(row),shutdown:{code:0,signal:null,forced:false}}))}));
    assert.deepEqual(compareReaderScopeReports(sample,candidates,expected),{complete:true,casesPerBackend:160,backends:candidates.length});
    assert.throws(()=>compareReaderScopeReports(sample,candidates.slice(1),expected));
    const reference=structuredClone(sample),changed=structuredClone(candidates);
    for(const report of [reference,...changed]){const row=report.cases[0],document=JSON.parse(row.observed.text);document.state.closed.settled=false;row.observed.text=JSON.stringify(document);}
    assert.throws(()=>compareReaderScopeReports(reference,changed,expected));
});
after(()=>console.log(JSON.stringify({independentStateMutationControls:stateMutations,cases:160,observationsPerCase:2})));
