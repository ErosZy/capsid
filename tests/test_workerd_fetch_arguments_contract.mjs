import assert from 'node:assert/strict';
import {test} from 'node:test';
import fs from 'node:fs';
import {readNativeArgumentSample} from './helpers/ws_fetch_arguments_sample.mjs';
import {nativeArgumentKinds,nativeArgumentRoutes,nativeArgumentShardCases,nativeArgumentCapacity,
    assertNativeArgumentCase,assertNativeArgumentState,validateNativeArgumentReport,compareNativeArgumentShardReports,
    expectedNativeArgumentState,nativeArgumentStateTemplate}
    from '../tools/workerd-fetch-arguments-contract.mjs';
import {referenceBackendModes} from '../tools/workerd-platform.mjs';

const routes=['getReader','direct','prototype','subclass','reflect'];
const views=['Int8Array','Uint8Array','Uint8ClampedArray','Int16Array','Uint16Array','Int32Array','Uint32Array',
    'Float16Array','Float32Array','Float64Array','BigInt64Array','BigUint64Array','DataView'];
const inventory=routes.flatMap(route=>views.flatMap(view=>['read','least'].map(method=>`native-arguments-${route}-${view}-${method}`)));
const sample=readNativeArgumentSample();
const expected={lock:sample.referenceLock,runId:sample.runId,inputs:sample.inputs,host:'1'.repeat(64),worker:'2'.repeat(64)};
test('independent130 groups cover all13 views, five construction routes, two methods and5460 native fetches',()=>{
    assert.equal(inventory.length,130);assert.equal(new Set(inventory).size,130);assert.deepEqual(nativeArgumentKinds,inventory);
    assert.deepEqual(nativeArgumentRoutes,routes);assert.deepEqual(sample.cases.map(value=>value.kind),inventory);
    assert.deepEqual(routes.map(route=>nativeArgumentShardCases[route].length),[26,26,26,26,26]);
    const fixture=fs.readFileSync(new URL('./fixtures/workerd-fetch-arguments.mjs',import.meta.url),'utf8');
    assert.ok(fixture.includes('const response=await fetch('));assert.ok(fixture.includes('const stream=response.body;'));
    assert.doesNotMatch(fixture,/new ReadableStream\s*\(/,'argument tests use the actual fetched native body');
});
test('unmodified pinned Linux reference satisfies the independent complete contract',()=>{
    const checked=validateNativeArgumentReport(sample,expected);
    assert.equal(checked.length,130);assert.equal(checked.reduce((sum,value)=>sum+value.observations,0),5460);
});
test('closed independent source connections may reuse a TCP tuple',()=>{
    const value=structuredClone(sample.cases[0]),snapshot=value.upstreamBeforeStop;
    assert.equal(snapshot.tcpRecords[0].closed,true);assert.equal(snapshot.tcpRecords[1].closed,true);
    for(const key of ['localAddress','localPort','remoteAddress','remotePort']){
        snapshot.tcpRecords[1][key]=snapshot.tcpRecords[0][key];
        snapshot.records[1][key]=snapshot.records[0][key];
    }
    value.upstreamAfterStop=structuredClone(snapshot);
    assertNativeArgumentCase(value,'reference');
});
test('a repeated source handshake is rejected even with a different TCP port',()=>{
    const value=structuredClone(sample.cases[0]),snapshot=value.upstreamBeforeStop;
    const port=snapshot.tcpRecords[1].remotePort;
    snapshot.records[1]=structuredClone(snapshot.records[0]);snapshot.tcpRecords[1]=structuredClone(snapshot.tcpRecords[0]);
    snapshot.records[1].remotePort=port;snapshot.tcpRecords[1].remotePort=port;
    value.upstreamAfterStop=structuredClone(snapshot);
    assert.throws(()=>assertNativeArgumentCase(value,'reference'));
});
for(const value of sample.cases){
    test('actual native argument reference '+value.kind,()=>assertNativeArgumentCase(value,'reference'));
    const state=JSON.parse(value.observed.text).state;
    assert.equal(state.rows.length,value.kind.endsWith('-read')?44:40);
    const row=(state,name)=>{const value=state.rows.find(row=>row.scenario===name);assert.ok(value);return value;};
    const mutateState=(entry,mutate)=>{const document=JSON.parse(entry.observed.text);mutate(document.state);entry.observed.text=JSON.stringify(document);};
    for(const [name,mutate] of [
        ['omitted row',s=>{s.rows.pop();}],['reordered rows',s=>{s.rows.reverse();}],['extra state',s=>{s.unreviewed=true;}],
        ['repeated conversion getter',s=>{row(s,'object-capacity').events.push('get-min');}],
        ['wrong error identity',s=>{row(s,'conversion-error').read.error.sameSentinel=false;}],
        ['wrong error realm',s=>{row(s,'invalid-view').read.error.isTypeError=false;}],
        ['released reader kept locked',s=>{row(s,'released-zero').afterCall.locked=true;}],
        ['invalid read disturbs body',s=>{row(s,'invalid-view').afterCall.used=true;}],
        ['successful read does not disturb body',s=>{row(s,'valid-minimum').afterCall.used=false;}],
        ['success retains input',s=>{row(s,'valid-minimum').afterCall.bufferBytes=32;}],
        ['detached input appears attached',s=>{row(s,'detached-capacity').afterCall.bufferBytes=32;}],
        ['retry loses queued bytes',s=>{row(s,'invalid-view').retry.bytes[0]=0;}],
        ['retry has wrong backing',s=>{row(s,'invalid-view').retry.backing[0]=0;}],
        ['outer backing altered on refusal',s=>{row(s,'invalid-view').originalBacking[0]=0;}],
        ['EOF is data',s=>{row(s,'valid-minimum').eof.done=false;}],
        ['EOF backing is wrong',s=>{row(s,'valid-minimum').eof.backing[0]=1;}],
        ['retained final reader lock',s=>{s.rows[0].lockedAfterRelease=true;}],
        ['missing final body use',s=>{s.rows[0].bodyUsed=false;}],
        ['unexpected remaining payload',s=>{s.rows[0].remaining.push({done:false});}],
        ['different response metadata',s=>{s.rows[0].response.status=409;}],
        ['cancel result changed',s=>{s.rows[0].cancelReturnedUndefined=false;}],
        ['synchronous receiver rejection becomes async',s=>{const value=row(s,'wrong-receiver');value.read={error:value.synchronous};delete value.synchronous;}],
    ])test('reject native argument '+name+' '+value.kind,()=>{
        const mutant={...value,observed:{...value.observed}};mutateState(mutant,mutate);assert.throws(()=>assertNativeArgumentCase(mutant,'reference'));
    });
    for(let index=0;index<state.rows.length;index++)test('reject changed native observation '+index+' '+value.kind,()=>{
        const mutant=structuredClone(state),value=mutant.rows[index];
        if(value.synchronous)value.synchronous.message='wrong';else if(value.read.error)value.read.error.message='wrong';else value.read.value.bytes[0]=0;
        assert.throws(()=>assertNativeArgumentState(mutant,state.kind,state.challenge));
    });
    for(const [name,mutate] of [
        ['missing real source request',s=>{s.records.pop();}],
        ['lost TCP completion',s=>{s.tcpRecords[0].closed=false;}],
        ['changed actual payload',s=>{s.records[0].sentChunks[1]=Buffer.alloc(16).toString('base64');}],
        ['missing request nonce',s=>{s.records[0].rawRequest='';}],
        ['injected source error',s=>{s.records[0].error={message:'failed'};}],
        ['duplicate source connection',s=>{s.tcpRecords[1]=structuredClone(s.tcpRecords[0]);s.records[1]=structuredClone(s.records[0]);}],
    ])test('reject native source '+name+' '+value.kind,()=>{
        const mutant=structuredClone(value);mutate(mutant.upstreamBeforeStop);mutant.upstreamAfterStop=structuredClone(mutant.upstreamBeforeStop);
        assert.throws(()=>assertNativeArgumentCase(mutant,'reference'));
    });
}
for(const [name,mutate] of [
    ['missing group',r=>{r.cases.pop();}],['reordered groups',r=>{r.cases.reverse();}],['duplicate group',r=>{r.cases[1]=r.cases[0];}],
    ['stale input',r=>{r.inputs.fixture='0'.repeat(64);}],['unknown input',r=>{r.inputs.other='0'.repeat(64);}],
    ['wrong client',r=>{r.client.node='v0.0.0';}],['wrong reference',r=>{r.binaries.host='0'.repeat(64);}],
    ['missing actual completion',r=>{r.complete=false;}],['unknown capacity',r=>{r.capacity={channelsTotal:128};}],
    ['missing captured TLS authority',r=>{r.certificateInputs.caPEM='';}],
    ['same VM reused',r=>{const document=JSON.parse(r.cases[1].initial.text);document.realm=JSON.parse(r.cases[0].initial.text).realm;r.cases[1].initial.text=JSON.stringify(document);document.state=JSON.parse(r.cases[1].observed.text).state;r.cases[1].observed.text=JSON.stringify(document);}],
])test('reject native argument report '+name,()=>{
    const mutant=structuredClone(sample);mutate(mutant);assert.throws(()=>validateNativeArgumentReport(mutant,expected));
});
// These candidates are synthetic validator controls. Real backend qualification
// always uses fresh captures from the actual binaries and bounded capacity.
function shardReports(mode){
    return routes.map(route=>({...sample,shard:route,mode,
        capacity:mode==='reference'?null:nativeArgumentCapacity,
        binaries:mode==='reference'?sample.binaries:{host:expected.host,worker:expected.worker},
        cases:sample.cases.filter(value=>nativeArgumentShardCases[route].includes(value.kind))
            .map(value=>mode==='reference'?value:{...value,shutdown:{code:0,signal:null,forced:false}})}));
}
test('five route shards require every supported backend and all5460 rows',()=>{
    const reference=shardReports('reference'),candidates=referenceBackendModes(expected.lock).map(shardReports);
    assert.deepEqual(compareNativeArgumentShardReports(reference,candidates,expected),
        {complete:true,groupsPerBackend:130,observationsPerBackend:5460,shardsPerBackend:5,backends:3});
    assert.throws(()=>compareNativeArgumentShardReports(reference.slice(1),candidates,expected));
    assert.throws(()=>compareNativeArgumentShardReports(reference,candidates.slice(1),expected));
});

test('mutable construction results cannot alter another model or the fixed input bytes',()=>{
    const kind=nativeArgumentKinds[0],challenge='model-independence';
    const left=expectedNativeArgumentState(kind,challenge),right=expectedNativeArgumentState(kind,challenge);
    const row=s=>s.rows.find(r=>r.scenario==='valid-minimum');
    row(left).read.value.bytes[0]=99;row(left).read.value.backing[8]=88;
    assert.equal(row(right).read.value.bytes[0],1);assert.equal(row(right).read.value.backing[8],1);
    assert.equal(row(expectedNativeArgumentState(kind,challenge)).read.value.bytes[0],1);
    assert.throws(()=>assertNativeArgumentState(left,kind,challenge));
    assertNativeArgumentState(right,kind,challenge);
});
test('only immutable input templates are shared and the FIFO capacity remains32',()=>{
    const kind=nativeArgumentKinds[0];
    // Fill the cache with unrelated keys, then track one exact32-entry window.
    for(let i=0;i<32;i++)nativeArgumentStateTemplate(kind,'template-prime-'+i);
    const first=nativeArgumentStateTemplate(kind,'template-window-0');
    const refused=first.rows.find(row=>row.scenario==='invalid-view');
    assert.ok(Object.isFrozen(first)&&Object.isFrozen(first.rows)&&Object.isFrozen(refused.retry.bytes));
    assert.throws(()=>{refused.retry.bytes[0]=99;},TypeError);
    for(let i=1;i<32;i++)nativeArgumentStateTemplate(kind,'template-window-'+i);
    assert.equal(nativeArgumentStateTemplate(kind,'template-window-0'),first);
    nativeArgumentStateTemplate(kind,'template-window-32');
    assert.notEqual(nativeArgumentStateTemplate(kind,'template-window-0'),first);
    assert.throws(()=>nativeArgumentStateTemplate(kind,null));
    assert.throws(()=>nativeArgumentStateTemplate('unknown-kind','template-window-0'));
});
test('a previously accepted mutable state is checked again after mutation',()=>{
    const kind=nativeArgumentKinds[0],challenge='repeat-current-state';
    const state=expectedNativeArgumentState(kind,challenge);
    assertNativeArgumentState(state,kind,challenge);
    const retry=state.rows.find(row=>row.scenario==='invalid-view').retry;
    retry.bytes[0]=99;
    assert.throws(()=>assertNativeArgumentState(state,kind,challenge));
    retry.bytes[0]=1;assertNativeArgumentState(state,kind,challenge);
    state.rows[0].unknown=true;assert.throws(()=>assertNativeArgumentState(state,kind,challenge));
});
