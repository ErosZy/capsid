import assert from 'node:assert/strict';
import {test} from 'node:test';
import {readNativeReaderSample} from './helpers/ws_fetch_reader_sample.mjs';
import {nativeReaderCases,assertNativeReaderCase,validateNativeReaderReport,compareNativeReaderReports} from '../tools/workerd-fetch-reader-contract.mjs';
import {referenceBackendModes} from '../tools/workerd-platform.mjs';
const sample=readNativeReaderSample();
const expected={lock:sample.referenceLock,runId:sample.runId,inputs:sample.inputs,host:'1'.repeat(64),worker:'2'.repeat(64)};
const inventory=[0,1,6,4095,4096,4097,8192,16384,65536,65537,1048613].map(length=>`chunks-default-${length}-0`)
    .concat(['chunks-byob-1048613-131075','chunks-byob-1048613-1048613','chunks-least-1048613-131075']);
test('independent inventory retains every size, byte-window boundary and full/partial BYOB minimum',()=>assert.deepEqual(nativeReaderCases,inventory));
test('actual pinned reference has all exact shapes, complete backing hashes and closed ordering',()=>assert.equal(validateNativeReaderReport(sample,expected).length,14));
for(const value of sample.cases){
    const state=JSON.parse(value.observed.text).state;
    test('actual complete reader reference '+value.kind,()=>assertNativeReaderCase(value,'reference'));
    for(const [name,mutate] of [
        ['changed complete digest',s=>{s.sha256='0'.repeat(64);}],['truncated byte total',s=>{s.total++;}],
        ['unexpected read error',s=>{s.unexpected={name:'Error',message:'failed'};}],['invalid source bytes',s=>{s.invalidBytes=1;}],
        ['retained lock',s=>{s.lockedAfterRelease=true;}],['body not disturbed',s=>{s.bodyUsed=false;}],
        ['missing closed fulfillment',s=>{s.closed=null;}],['EOF returned as data',s=>{s.reads.at(-1).done=false;}],
        ['missing EOF',s=>{s.reads.pop();}],['repeated EOF',s=>{s.reads.push(structuredClone(s.reads.at(-1)));}],
        ['extra observation',s=>{s.extra=true;}],
    ])test('reject reader '+name+' '+value.kind,()=>{
        const mutant={...value,observed:{...value.observed}},document=JSON.parse(mutant.observed.text);mutate(document.state);mutant.observed.text=JSON.stringify(document);
        assert.throws(()=>assertNativeReaderCase(mutant,'reference'));
    });
    for(let index=0;index<state.reads.length;index++)for(const [name,mutate] of [
        ['chunk size',r=>{r.byteLength=r.byteLength===null?0:r.byteLength+1;}],
        ['backing hash',r=>{r.backingSHA256='0'.repeat(64);}],
        ['closed order',r=>{r.closedAfterRead=r.closedAfterRead?null:{fulfilled:true};}],
    ])test('reject exact '+name+' '+index+' '+value.kind,()=>{
        const mutant={...value,observed:{...value.observed}},document=JSON.parse(mutant.observed.text);mutate(document.state.reads[index]);mutant.observed.text=JSON.stringify(document);
        assert.throws(()=>assertNativeReaderCase(mutant,'reference'));
    });
    if(value.kind==='chunks-default-1048613-0')test('reject equal aggregate bytes split into the historical512-byte shape',()=>{
        const mutant=structuredClone(value),document=JSON.parse(mutant.observed.text),read=document.state.reads[0];
        read.byteLength=512;document.state.reads.splice(1,0,...Array.from({length:7},()=>structuredClone(read)));
        mutant.observed.text=JSON.stringify(document);assert.throws(()=>assertNativeReaderCase(mutant,'reference'));
    });
}
test('same precise chunk error on the reference and every synthetic backend is rejected',()=>{
    const candidates=referenceBackendModes(expected.lock).map(mode=>({...sample,mode,binaries:{host:expected.host,worker:expected.worker},
        cases:sample.cases.map(value=>({...value,shutdown:{code:0,signal:null,forced:false}}))}));
    assert.deepEqual(compareNativeReaderReports(sample,candidates,expected),{complete:true,casesPerBackend:14,backends:3});
    const reference=structuredClone(sample),changed=candidates.map(report=>structuredClone(report));
    for(const report of [reference,...changed]){
        const value=report.cases.find(value=>value.kind==='chunks-byob-1048613-131075'),document=JSON.parse(value.observed.text);
        document.state.reads[0].byteLength=131584;value.observed.text=JSON.stringify(document);
    }
    assert.throws(()=>compareNativeReaderReports(reference,changed,expected));
});
