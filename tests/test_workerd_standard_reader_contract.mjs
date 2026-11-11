import assert from 'node:assert/strict';
import {test} from 'node:test';
import fs from 'node:fs';
import {readStandardReaderSample} from './helpers/ws_standard_reader_sample.mjs';
import {standardReaderCases,standardReaderShardNames,standardReaderShardCases,assertStandardReaderCase,validateStandardReaderReport,compareStandardReaderReports,compareStandardReaderShardReports} from '../tools/workerd-standard-reader-contract.mjs';
import {referenceBackendModes} from '../tools/workerd-platform.mjs';
const sample=readStandardReaderSample();
const expected={runId:sample.runId,lock:sample.referenceLock,inputs:sample.inputs,host:'1'.repeat(64),worker:'2'.repeat(64)};
const routes=['getReader','direct','prototype','subclass','reflect'],allocations=['none',1,16,4096,8192];
const inventory=[...['queue','pull'].flatMap(phase=>allocations.flatMap(size=>
    ['empty','6','3,3','4095','4096','4097','2000,3000','8193'].map(length=>'standard|getReader|default|'+[phase,size,length].join('_')))),
    ...routes.flatMap(route=>['close','cancel'].flatMap(action=>allocations.map(size=>'standard|'+route+'|default|buffer_'+action+'_'+size))),
    ...routes.flatMap(route=>['source','returned'].flatMap(target=>['transfer','transferToFixedLength'].flatMap(method=>
        ['none',0,8,16,24].map(size=>'standard|'+route+'|default|transfer_'+[target,method,size].join('_')))))];
test('230 explicit allocation, ownership and transfer cases retain the original80 shape prefix',()=>assert.deepEqual(standardReaderCases,inventory));
test('actual pinned reference passes the independent full state and transport model',()=>assert.equal(validateStandardReaderReport(sample,expected).length,230));
test('three fixed shards preserve every original case in exact order',()=>{
    assert.deepEqual(standardReaderShardNames,['shapes','ownership','transfers']);
    assert.deepEqual(standardReaderShardNames.flatMap(shard=>standardReaderShardCases(shard)),inventory);
    assert.deepEqual(standardReaderShardNames.map(shard=>standardReaderShardCases(shard).length),[80,50,100]);
    assert.throws(()=>standardReaderShardCases('partial'));
});
test('the actual fixture outside-byte observer matches independent single-byte boundary controls',()=>{
    const source=fs.readFileSync(new URL('./fixtures/workerd-standard-readers.mjs',import.meta.url),'utf8');
    const start=source.indexOf('function outsideBytesZero('),end=source.indexOf('async function shapes(',start);
    assert.ok(start>=0&&end>start);const observe=Function('return ('+source.slice(start,end).trim()+')')();
    let checks=0;
    for(const capacity of [0,1,2,3,7,8,16,31,32,33])for(let offset=0;offset<=capacity;++offset)
    for(let length=0;length<=capacity-offset;++length){
        const bytes=new Uint8Array(capacity);
        for(let changed=-1;changed<capacity;++changed){
            if(changed>=0)bytes[changed]=255;
            const expected=Array.from(bytes).every((value,index)=>index>=offset&&index<offset+length||value===0);
            assert.equal(observe(bytes.buffer,offset,length),expected,'capacity='+capacity+' offset='+offset+' length='+length+' changed='+changed);
            if(changed>=0)bytes[changed]=0;checks++;
        }
    }
    assert.ok(checks>50000);
});
function shardMatrix(){
    const references=standardReaderShardNames.map(shard=>({...structuredClone(sample),shard,
        cases:sample.cases.filter(row=>standardReaderShardCases(shard).includes(row.kind)).map(row=>structuredClone(row))}));
    const candidates=referenceBackendModes(expected.lock).flatMap(mode=>references.map(reference=>({...structuredClone(reference),mode,
        binaries:{host:expected.host,worker:expected.worker},cases:reference.cases.map(row=>({...structuredClone(row),shutdown:{code:0,signal:null,forced:false}}))})));
    return{references,candidates};
}
test('every shard on every platform backend is required by the complete comparison',()=>{
    const {references,candidates}=shardMatrix();
    assert.deepEqual(compareStandardReaderShardReports(references,candidates,expected),{complete:true,casesPerBackend:230,shardsPerBackend:3,backends:referenceBackendModes(expected.lock).length});
});
for(const [name,mutate]of [
    ['missing reference shard',m=>m.references.pop()],
    ['duplicate reference shard',m=>{m.references[1]=structuredClone(m.references[0]);}],
    ['missing product shard',m=>m.candidates.pop()],
    ['duplicate product shard',m=>{m.candidates[1]=structuredClone(m.candidates[0]);}],
    ['changed input between shards',m=>{m.candidates[1].inputs.fixture='0'.repeat(64);}],
    ['changed run between shards',m=>{m.references[1].runId='different-run';}],
    ['same challenge reused across shards',m=>{m.references[1].cases[0].challenge=m.references[0].cases[0].challenge;}],
    ['same worker realm reused across shards',m=>{const row=m.references[1].cases[0],document=JSON.parse(row.initial.text);
        document.realm=JSON.parse(m.references[0].cases[0].initial.text).realm;row.initial.text=JSON.stringify(document);}],
    ['same ordered case omitted everywhere',m=>{for(const report of [...m.references,...m.candidates])if(report.shard==='ownership')report.cases.pop();}],
])test('whole family rejects '+name,()=>{const matrix=shardMatrix();mutate(matrix);assert.throws(()=>compareStandardReaderShardReports(matrix.references,matrix.candidates,expected));});
for(const row of sample.cases) {
    const operation=row.kind.split('|').at(-1).split('_')[0];
    test('actual reference '+row.kind,()=>assertStandardReaderCase(row,'reference'));
    const mutations=[['unexpected error',state=>{state.unexpected={name:'Error',message:'failed'};}],
        ['lock retained',state=>{state.locked=true;}],['extra state field',state=>{state.extra=true;}]];
    if(operation==='queue'||operation==='pull')mutations.push(
        ['incorrect run count',state=>{state.shapes[0].count++;}],
        ['zero run count',state=>{state.shapes[0].count=0;}],
        ['lost backing capacity',state=>{state.shapes[0].value.capacity=999;}],
        ['incorrect chunk length',state=>{state.shapes[0].value.length=-1;}],
        ['incorrect EOF',state=>{state.shapes.at(-1).value.done=false;}],
        ['cross realm view',state=>{state.shapes[0].value.sameRealm=false;}],
        ['incorrect returned view type',state=>{state.shapes[0].value.type='Float64Array';}],
        ['nonzero bytes outside returned range',state=>{state.shapes[0].value.outsideZero=false;}],
        ['source allocation lost',state=>{state.allocations.push({count:1,value:null});}],
        ['enqueue retains source buffer',state=>{state.enqueued=[{count:1,value:{bufferBytes:16,viewBytes:16}}];}],
        ['payload changed',state=>{state.sha256='0'.repeat(64);}],
        ['incorrect total bytes',state=>{state.total++;}],
        ['incorrect total read count',state=>{state.reads++;}],
        ['redundant identical shape runs',state=>{state.shapes.splice(0,1,{count:0,value:state.shapes[0].value},...state.shapes.slice(0,1));}]);
    else if(operation==='buffer')mutations.push(
        ['source lifetime incorrect',state=>{state.after=state.after===0?16:0;}],
        ['source view lifetime incorrect',state=>{state.viewAfter=state.viewAfter===0?16:0;}],
        ['same buffer object returned',state=>{state.sameBuffer=true;}],
        ['empty view becomes data',state=>{state.read.bytes=1;}],
        ['EOF lost',state=>{state.read.done=false;}],
        ['backing capacity lost',state=>{state.read.capacity++;}],
        ['backing bytes changed',state=>{state.initialSHA256='0'.repeat(64);}],
        ['cached write not shared correctly',state=>{state.afterCachedPrefix[0]++;}],
        ['returned write not shared correctly',state=>{state.cachedAfterReturnedFirst=-1;}],
        ['cancel callback sees wrong source state',state=>{state.inCancel=16;}]);
    else mutations.push(
        ['transfer unavailable',state=>{state.available=false;}],
        ['transfer threw',state=>{state.error={name:'TypeError',message:'failure'};}],
        ['source detachment incorrect',state=>{state.after.source=state.after.source===0?16:0;}],
        ['returned detachment incorrect',state=>{state.after.returned=state.after.returned===0?16:0;}],
        ['source view detachment incorrect',state=>{state.after.sourceView=state.after.sourceView===0?16:0;}],
        ['moved capacity incorrect',state=>{state.after.moved++;}],
        ['transfer lost original bytes or zero fill',state=>{state.after.movedPrefix.push(255);}],
        ['write sharing after transfer incorrect',state=>{state.aliasAfterMovedWrite[0]++;}],
        ['alias write sharing incorrect',state=>{state.movedAfterAliasWrite.push(1);}]);
    for(const [name,mutate]of mutations)test('reject '+name+' '+row.kind,()=>{
        const mutant=structuredClone(row),document=JSON.parse(mutant.observed.text);mutate(document.state);
        mutant.observed.text=JSON.stringify(document);assert.throws(()=>assertStandardReaderCase(mutant,'reference'));
    });
    for(const [name,mutate]of [
        ['worker unavailable',value=>{value.observed.status=503;}],
        ['worker realm changed',value=>{const document=JSON.parse(value.observed.text);document.realm='11111111-1111-4111-8111-111111111111';value.observed.text=JSON.stringify(document);}],
        ['raw downstream bytes changed',value=>{value.wire.rawTail='';}],
        ['close frame lost',value=>{value.wire.frames.pop();}],
        ['unexpected upstream I/O',value=>{value.upstreamBeforeStop.tcpRecords.push({closed:true});}],
        ['missing completion before stop',value=>{value.allSourcesEndedBeforeStop=false;}],
        ['forced shutdown',value=>{value.shutdown.forced=true;}],
        ['runtime diagnostic',value=>{value.stderr+='\nAddressSanitizer';}],
    ])test('reject transport '+name+' '+row.kind,()=>{const mutant=structuredClone(row);mutate(mutant);assert.throws(()=>assertStandardReaderCase(mutant,'reference'));});
}
test('full report identity, completion, inventory and prerequisites are mandatory',()=>{
    for(const mutate of [value=>{value.complete=false;},value=>{value.cases.pop();},value=>{value.inputs.stateModel='0'.repeat(64);},
        value=>{value.binaries.host='0'.repeat(64);},value=>{value.client.node='v0.0.0';},
        value=>{value.cases[1].challenge=value.cases[0].challenge;},value=>{value.cases[1].initial=value.cases[0].initial;}]){
        const mutant=structuredClone(sample);mutate(mutant);assert.throws(()=>validateStandardReaderReport(mutant,expected));
    }
});
test('identical wrong buffers and read shape on all backends and reference are rejected',()=>{
    const candidates=referenceBackendModes(expected.lock).map(mode=>({...structuredClone(sample),mode,
        binaries:{host:expected.host,worker:expected.worker},cases:sample.cases.map(row=>({...structuredClone(row),shutdown:{code:0,signal:null,forced:false}}))}));
    assert.deepEqual(compareStandardReaderReports(sample,candidates,expected),{complete:true,casesPerBackend:230,backends:candidates.length});
    for(const operation of ['pull_','buffer_close_','transfer_source_']){
        const reference=structuredClone(sample),changed=structuredClone(candidates);
        for(const report of [reference,...changed]){
            const row=report.cases.find(row=>row.kind.split('|').at(-1).startsWith(operation)),document=JSON.parse(row.observed.text);
            document.state.locked=true;row.observed.text=JSON.stringify(document);
        }
        assert.throws(()=>compareStandardReaderReports(reference,changed,expected));
    }
    assert.throws(()=>compareStandardReaderReports(sample,candidates.slice(1),expected));
});
