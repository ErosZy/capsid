import assert from 'node:assert/strict';
import {test,after} from 'node:test';
import {headersExpectedRows,headersWireTypes,headersPrimarySamples,headersDigest,assertHeadersRows,assertHeadersObservation,validateHeadersPrimaries,validateHeadersReport,headersInputPaths} from '../tools/headers-contract.mjs';
let rejected=0;
const expectReject=fn=>{assert.throws(fn);rejected++;};
const mutate=(value,path)=>{let parent=value;for(const key of path.slice(0,-1))parent=parent[key];const key=path.at(-1),prior=parent[key];parent[key]=prior===null?false:typeof prior==='boolean'?Number(prior):typeof prior==='number'?prior+1:prior+'!';};
const leaves=(value,path=[])=>value&&typeof value==='object'?Object.entries(value).flatMap(([key,child])=>leaves(child,[...path,key])):[path];
test('99 independently modeled full Headers cases, both actual fixed primary captures',()=>{assertHeadersRows(headersExpectedRows());validateHeadersPrimaries(headersPrimarySamples());});
test('every primitive leaf and every row field is checked, with strict scalar types',()=>{
    const rows=headersExpectedRows();
    for(let index=0;index<99;index++){
        for(const path of leaves(rows[index])){const bad=structuredClone(rows);mutate(bad,[index,...path]);expectReject(()=>assertHeadersRows(bad));}
        for(const key of ['id','value','error','trace']){const bad=structuredClone(rows);delete bad[index][key];expectReject(()=>assertHeadersRows(bad));}
    }
});
test('wrong order/count, conversion traces, sentinel identity and mutable revalidation fail',()=>{
    const rows=headersExpectedRows();for(const change of [bad=>bad.pop(),bad=>bad.push(structuredClone(bad[0])),bad=>bad.reverse(),bad=>{bad[1]=structuredClone(bad[0]);},bad=>{bad[0].extra=true;}]){
        const bad=structuredClone(rows);change(bad);expectReject(()=>assertHeadersRows(bad));
    }
    assertHeadersRows(rows);rows[0].id='changed-after-pass';expectReject(()=>assertHeadersRows(rows));
});
test('simultaneously wrong primary captures do not establish an oracle',()=>{
    const source=headersPrimarySamples();
    for(const operation of ['row','trace','identity','profile','raw-hash','order']){
        const bad=structuredClone(source);
        if(operation==='order')bad.captures.reverse();
        else for(const capture of bad.captures){
            if(operation==='raw-hash'){capture.rawUtf8+=' ';continue;}
            const report=JSON.parse(capture.rawUtf8);
            if(operation==='row')report.observed.rows[0].id='both-wrong';
            if(operation==='trace')report.observed.rows[10].trace.push('both-wrong');
            if(operation==='identity')report.inputs['upgrade58bgp-headers-common.mjs']='0'.repeat(64);
            if(operation==='profile')report.referenceLock.compatibility_date='2026-08-21';
            report.response=JSON.stringify(report.observed);capture.rawUtf8=JSON.stringify(report);capture.sha256=headersDigest(capture.rawUtf8);
        }
        expectReject(()=>validateHeadersPrimaries(bad));
    }
    validateHeadersPrimaries(source);source.captures[0].rawUtf8+=' ';expectReject(()=>validateHeadersPrimaries(source));
});
test('User and Binding observations, real prototype counts and99 individual RPCs',()=>{
    const rows=headersExpectedRows(),observation={user:structuredClone(rows),binding:structuredClone(rows),types:headersWireTypes(rows),rpcCalls:99};assertHeadersObservation(observation);
    for(const change of [bad=>{bad.user[0].id=bad.binding[0].id='jointly-wrong';},bad=>{bad.binding.pop();},bad=>{bad.rpcCalls=98;},bad=>{bad.types.objectNullPrototypes=false;},bad=>{bad.types.arrayPrototypes=false;},bad=>{bad.types.objects++;},bad=>{bad.types.arrays++;}]){
        const bad=structuredClone(observation);change(bad);expectReject(()=>assertHeadersObservation(bad));
    }
    assertHeadersObservation(observation);observation.binding[0].trace.push('late');expectReject(()=>assertHeadersObservation(observation));
});
test('complete current report/provenance/full transcript/actual exit, no acceptance cache',()=>{
    const rows=headersExpectedRows(),observation={user:structuredClone(rows),binding:structuredClone(rows),types:headersWireTypes(rows),rpcCalls:99};
    const inputs=Object.fromEntries(Object.keys(headersInputPaths).map(key=>[key,'a'.repeat(64)])),binaries={helper:'b'.repeat(64),worker:'c'.repeat(64)},workerExit={reaped:true,channelClosed:true,code:0};
    const report={schema:'capsid-headers-compat-v1',complete:true,runId:'synthetic-headers',inputs,binaries,observation,stdout:'',stderr:'',child:{code:0,signal:null},workerExit};
    const transcript=value=>'memoryLimit=0 actualFeatures=0 expectedFeatures=0\nBINDING_BODY '+JSON.stringify(value.observation)+'\nBINDING_EXIT '+JSON.stringify(value.workerExit)+'\n';report.stdout=transcript(report);const expected={runId:report.runId,inputs:structuredClone(inputs),binaries:structuredClone(binaries)};validateHeadersReport(report,expected);
    for(const change of [bad=>{bad.complete=1;},bad=>{bad.schema='wrong';},bad=>{bad.runId='changed';},bad=>{bad.inputs.runtime='d'.repeat(64);},bad=>{bad.binaries.worker='d'.repeat(64);},bad=>{bad.child.code=1;},bad=>{bad.workerExit.code=1;},bad=>{bad.stderr='unexpected';},bad=>{bad.observation.rpcCalls=0;}]){
        const bad=structuredClone(report);change(bad);bad.stdout=transcript(bad);expectReject(()=>validateHeadersReport(bad,expected));
    }
    const bad=structuredClone(report);bad.stdout+='extra\n';expectReject(()=>validateHeadersReport(bad,expected));validateHeadersReport(report,expected);report.observation.user[0].id='mutated';report.stdout=transcript(report);expectReject(()=>validateHeadersReport(report,expected));
});
test('fixed expected values are copied independently; callers cannot mutate the model',()=>{
    const first=headersExpectedRows();first[0].id='caller-mutated';first[0].value.entries.push(['x','bad']);assert.equal(headersExpectedRows()[0].id,'constructor-empty');assert.deepEqual(headersExpectedRows()[0].value.entries,[]);
});
after(()=>{assert.equal(rejected,1305);console.log(JSON.stringify({complete:true,uniqueHeaderCases:99,rejectedMutations:rejected}));});
