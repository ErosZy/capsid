import assert from 'node:assert/strict';
import {test} from 'node:test';
import {gzipSync,gunzipSync} from 'node:zlib';
import {createHash} from 'node:crypto';
import {readFetchBYOBSample} from './helpers/ws_fetch_byob_sample.mjs';
import {fetchBYOBCases,fetchBYOBDeferredStandardCases,assertFetchBYOBCase,validateFetchBYOBReport,
    compareFetchBYOBReports,compareFetchBYOBShardReports,fetchBYOBShardNames,fetchBYOBShardCases,parseWorkerdListenPort} from '../tools/workerd-fetch-byob-contract.mjs';

import {backingRangeFilled,patternErrors} from './fixtures/workerd-fetch-byob.mjs';
test('reference readiness uses the actual bound socket event for LF and CRLF',()=>{
    for(const end of ['\n','\r\n'])for(const port of [1,38337,64924,65535])
        assert.equal(parseWorkerdListenPort(JSON.stringify({event:'listen',socket:'http',port})+end),port);
});
for(const [name,event] of [
    ['wrong event',{event:'ready',socket:'http',port:12345}],
    ['wrong socket',{event:'listen',socket:'other',port:12345}],
    ['unbound zero',{event:'listen',socket:'http',port:0}],
    ['negative port',{event:'listen',socket:'http',port:-1}],
    ['overflow port',{event:'listen',socket:'http',port:65536}],
    ['fractional port',{event:'listen',socket:'http',port:1.5}],
    ['string port',{event:'listen',socket:'http',port:'12345'}],
    ['missing port',{event:'listen',socket:'http'}],
    ['unknown field',{event:'listen',socket:'http',port:12345,assumedReady:true}],
    ['array control',[]],['null control',null],
])test('reject reference listener '+name,()=>assert.throws(()=>parseWorkerdListenPort(JSON.stringify(event))));
test('reject truncated or malformed reference listener control',()=>{
    for(const input of ['','{','{"event":"listen"} trailing'])assert.throws(()=>parseWorkerdListenPort(input));
});
test('batched backing observer checks every byte including empty and unaligned ranges',t=>{
    let checks=0;
    for(const fill of [0,165])for(let length=0;length<=24;length++)for(let start=0;start<=length;start++)for(let end=start;end<=length;end++){
        const bytes=new Uint8Array(length);bytes.fill(fill);
        assert.equal(backingRangeFilled(bytes.buffer,start,end,fill),true);checks++;
        for(let index=0;index<length;index++){
            bytes[index]^=1;
            assert.equal(backingRangeFilled(bytes.buffer,start,end,fill),bytes.subarray(start,end).every(byte=>byte===fill));checks++;
            bytes[index]^=1;
        }
    }
    t.diagnostic(`${checks} independent backing-range checks`);
});
test('batched payload observer matches independent byte checks at every view alignment',t=>{
    let checks=0;
    for(const position of [0,1,2,3,17,249,250,251,252,1048600])for(const offset of [0,1,2,3,4,7,8,15,16,31])for(let length=0;length<=21;length++){
        const bytes=new Uint8Array(offset+length+3);bytes.fill(165);
        const view=new Uint8Array(bytes.buffer,offset,length);for(let i=0;i<length;i++)view[i]=((position+i)*29+17)%251;
        assert.equal(patternErrors(view,position),0);checks++;
        for(let index=0;index<bytes.length;index++){
            bytes[index]^=1;assert.equal(patternErrors(view,position),index>=offset&&index<offset+length?1:0);checks++;bytes[index]^=1;
        }
        view.fill(0);assert.equal(patternErrors(view,position),Array.from(view).filter((byte,i)=>byte!==((position+i)*29+17)%251).length);checks++;
    }
    t.diagnostic(`${checks} independent payload checks`);
});

const sample=readFetchBYOBSample();
const expected={lock:sample.referenceLock,runId:sample.runId,inputs:sample.inputs,host:'1'.repeat(64),worker:'2'.repeat(64)};
// Independent reviewed inventory. The builder cannot silently lose a route,
// view, minimum, partial cancellation or incomplete-element EOF case.
const routes=['getReader','direct','prototype','subclass','reflect'];
const views=['Uint8Array','Uint16Array','Int32Array','Float64Array','DataView'];
const methods=['default','min2','min3','least2','least3'];
const standard=['Uint8Array-default','Uint8Array-min2','Uint8Array-min3','Uint8Array-least2','Uint8Array-least3',
    'Uint16Array-default','Uint16Array-min2','Uint16Array-least2','Int32Array-default',
    'DataView-default','DataView-min2','DataView-min3','DataView-least2','DataView-least3'];
const additionalViews=['Int8Array','Uint8ClampedArray','Int16Array','Uint32Array','Float16Array','Float32Array','BigInt64Array','BigUint64Array'];
const inventory=[...routes.flatMap(route=>views.flatMap(view=>methods.flatMap(method=>['held','eof'].map(phase=>`native-${route}-${view}-${method}-${phase}`)))),
    ...['getReader','direct'].flatMap(route=>standard.flatMap(spec=>['held','eof'].map(phase=>`standard-${route}-${spec}-${phase}`))),
    ...additionalViews.flatMap(view=>methods.flatMap(method=>['held','eof'].map(phase=>`native-getReader-${view}-${method}-${phase}`))),
    ...routes.flatMap(route=>['default','byob'].map(reader=>`brand-${route}-${reader}-held`)),
    ...['getReader','direct'].flatMap(route=>['min3','least3'].flatMap(method=>['held','eof'].map(phase=>`standard-${route}-Float64Array-${method}-${phase}`))),
    ...routes.flatMap(route=>['read','least'].map(method=>`validation-${route}-${method}`)),
    ...routes.flatMap(route=>['empty','small'].map(phase=>`body-${route}-default-${phase}`)),
    ...routes.flatMap(route=>['minimum','least'].map(method=>`body-${route}-${method}-large`)),
    'body-getReader-default-large',
    ...['getReader','direct'].flatMap(route=>['Uint16Array-min3','Uint16Array-least3',
        'Int32Array-min2','Int32Array-min3','Int32Array-least2','Int32Array-least3',
        'Float64Array-default','Float64Array-min2','Float64Array-least2'].flatMap(spec=>
            ['held','eof'].map(phase=>`standard-${route}-${spec}-${phase}`)))];
inventory.push(...['getReader','direct'].flatMap(route=>['queued','pending'].flatMap(phase=>
    ['Uint8Array','Uint16Array','Int32Array','Float64Array'].map(view=>`standard-close-${route}-${phase}-${view}`))));

test('487 exact cases retain all471 and add sixteen EOF timing groups with528 observations',()=>{
    assert.equal(inventory.length,487);assert.equal(inventory.filter(kind=>kind.startsWith('native-')).length,330);
    assert.equal(inventory.filter(kind=>kind.startsWith('standard-')).length,116);assert.equal(inventory.filter(kind=>kind.startsWith('brand-')).length,10);
    assert.equal(inventory.filter(kind=>kind.startsWith('body-')).length,21);assert.equal(inventory.filter(kind=>kind.startsWith('validation-')).length,10);
    assert.equal(new Set(inventory.filter(kind=>kind.startsWith('native-')).map(kind=>kind.split('-')[2])).size,13);
    assert.deepEqual(fetchBYOBCases,inventory);assert.deepEqual(sample.cases.map(value=>value.kind),inventory);
    assert.deepEqual(fetchBYOBDeferredStandardCases,[]);assert.equal(new Set(inventory).size,487);
});
test('actual pinned Linux capture validates with all raw evidence',()=>assert.equal(validateFetchBYOBReport(sample,expected).length,487));
test('eleven actual large bodies contain the independently reviewed 1 MiB plus 37 byte payload',()=>{
    const values=sample.cases.filter(value=>value.kind.startsWith('body-')&&value.kind.endsWith('-large'));
    assert.equal(values.length,11);
    for(const value of values){
        const state=JSON.parse(value.observed.text).state;assert.equal(state.total,1048613);
        assert.equal(state.sha256,'9a2e43fba44ff55f51b65e1478eae6147b7a42476178b0ddcaea11cc93256edc');
        if(!value.kind.includes('-default-'))assert.deepEqual(state.shapes,
            [[false,16,1048613,1048677,0,true],[true,0,0,1048677,0,true]]);
    }
});
for(const value of sample.cases){
    test('actual reference '+value.kind,()=>assert.deepEqual(assertFetchBYOBCase(value,'reference'),{kind:value.kind}));
    if(value.kind.startsWith('standard-close-')){
        const alter=(v,mutate)=>{const document=JSON.parse(v.observed.text);mutate(document.state);v.observed.text=JSON.stringify(document);};
        for(const [name,mutate] of [
            ['missing row',s=>{s.rows.pop();}],['row order',s=>{s.rows.reverse();}],
            ['read left pending',s=>{s.rows[0].read={pending:true};}],
            ['empty close reported as data',s=>{s.rows[0].read.value.done=false;}],
            ['missing empty EOF',s=>{s.rows[0].eof=null;}],
            ['input retained',s=>{s.rows[0].detached=false;}],
            ['wrong payload backing',s=>{s.rows[3].read.value.backing[8]=0;}],
            ['extra controller exception',s=>{s.rows[0].closeThrow={name:'TypeError',message:'failed'};}],
            ['reader lock retained',s=>{s.rows[0].lockedAfterRelease=true;}],
            ['incorrect promise order',s=>{s.rows[0].trace.reverse();}],
            ['extra state',s=>{s.unreviewed=true;}],['extra row field',s=>{s.rows[0].unreviewed=true;}],
        ])test('reject standard close '+name+' '+value.kind,()=>{
            const mutant=structuredClone(value);alter(mutant,mutate);assert.throws(()=>assertFetchBYOBCase(mutant,'reference'));
        });
        for(let index=0;index<33;index++)test('reject changed standard close row '+index+' '+value.kind,()=>{
            const mutant=structuredClone(value);alter(mutant,s=>{
                const row=s.rows[index];if(row.read.error)row.read.error.message='wrong';else row.read.value.backing[0]^=1;
            });assert.throws(()=>assertFetchBYOBCase(mutant,'reference'));
        });
        continue;
    }
    if(value.kind.startsWith('validation-')){
        const replace=(v,mutate)=>{const d=JSON.parse(v.observed.text);mutate(d.state);v.observed.text=JSON.stringify(d);};
        const state=JSON.parse(value.observed.text).state;
        assert.equal(state.rows.length,value.kind.endsWith('-read')?44:40);
        for(const [name,mutate] of [
            ['capture failure',v=>{v.failure='failed';}],['forced shutdown',v=>{v.shutdown.forced=true;}],
            ['missing wire bytes',v=>{v.wire.rawTail='';}],['cleanup supplies completion',v=>{v.allSourcesEndedBeforeStop=false;}],
            ...[
                ['missing row',s=>{s.rows.pop();}],['duplicate row',s=>{s.rows[1]=structuredClone(s.rows[0]);}],
                ['row order',s=>{s.rows.reverse();}],['unreviewed row',s=>{s.rows[0].unreviewed=true;}],
                ['unexpected error',s=>{s.unexpected={name:'Error',message:'failed'};}],
                ['repeated getter',s=>{s.rows[0].events.unshift('get-min');}],
                ['missing conversion',s=>{s.rows.find(row=>row.scenario==='object-capacity').events=s.rows[0].events;}],
                ['wrong buffer detachment',s=>{s.rows[0].afterCall.bufferBytes=0;}],
                ['wrong view length',s=>{s.rows[0].afterCall.viewBytes=0;}],
                ['original backing altered',s=>{s.rows[0].originalBacking[0]=0;}],
                ['callback identity lost',s=>{s.rows.find(row=>row.scenario==='conversion-error').read.error.sameSentinel=false;}],
                ['error came from another realm',s=>{s.rows[0].read.error.isTypeError=false;}],
                ['RangeError accepted',s=>{s.rows[0].read.error.isRangeError=true;}],
                ['construction collapsed',s=>{s.rows[0].construction.directPrototype=!s.rows[0].construction.directPrototype;}],
                ['reader lock retained',s=>{s.rows[0].lockedAfterRelease=true;}],
                ['cancel result changed',s=>{s.rows[0].cancelReturnedUndefined=false;}],
                ['missing retry',s=>{delete s.rows[0].retry;}],
                ['retry data altered',s=>{s.rows[0].retry.bytes[0]=0;}],
                ['missing EOF',s=>{delete s.rows[0].eof;}],
                ['EOF reported as data',s=>{s.rows[0].eof.done=false;}],
            ].map(([name,mutate])=>[name,v=>replace(v,mutate)]),
        ])test('reject validation '+name+' '+value.kind,()=>{
            const mutant=structuredClone(value);mutate(mutant);assert.throws(()=>assertFetchBYOBCase(mutant,'reference'));
        });
        for(let index=0;index<state.rows.length;index++)test('reject changed validation observation '+index+' '+value.kind,()=>{
            const mutant=structuredClone(value);replace(mutant,s=>{
                const row=s.rows[index];
                if(row.synchronous){row.read={error:row.synchronous};delete row.synchronous;row.events.push('returned');}
                else if(row.read.error)row.read.error.message='wrong';
                else row.read.value.bytes[0]=0;
            });assert.throws(()=>assertFetchBYOBCase(mutant,'reference'));
        });
        continue;
    }
    if(value.kind.startsWith('body-')){
        const replace=(v,mutate)=>{const d=JSON.parse(v.observed.text);mutate(d.state);v.observed.text=JSON.stringify(d);};
        for(const [name,mutate] of [
            ['failed capture',v=>{v.failure='failed';}],['forced shutdown',v=>{v.shutdown.forced=true;}],
            ['completion supplied by teardown',v=>{v.allSourcesEndedBeforeStop=false;}],
            ['missing wire bytes',v=>{v.wire.rawTail='';}],
            ['foreign observation realm',v=>{const d=JSON.parse(v.observed.text);d.realm=v.challenge;v.observed.text=JSON.stringify(d);}],
            ...[
                ['wrong body total',s=>{s.total++;}],['wrong complete byte digest',s=>{s.sha256='0'.repeat(64);}],
                ['invalid source bytes',s=>{s.invalidBytes=1;}],['non-byte result',s=>{s.unexpectedViews=1;}],
                ['read error',s=>{s.unexpected={name:'Error',message:'failed'};}],['missing closed',s=>{s.closed=null;}],
                ['body not disturbed',s=>{s.bodyUsed=false;}],['retained reader lock',s=>{s.lockedAfterRelease=true;}],
                ['wrong response metadata',s=>{s.response.status=409;}],['wrong content length',s=>{s.response.contentLength='wrong';}],
                ['missing EOF read',s=>{s.shapes.pop();}],['EOF reported as data',s=>{s.shapes.at(-1)[0]=false;}],
                ['unreviewed state field',s=>{s.unreviewed=true;}],
                ['construction route collapsed',s=>{s.construction.directPrototype=!s.construction.directPrototype;}],
            ].map(([name,mutate])=>[name,v=>replace(v,mutate)]),
        ])test('reject body '+name+' '+value.kind,()=>{
            const mutant=structuredClone(value);mutate(mutant);assert.throws(()=>assertFetchBYOBCase(mutant,'reference'));
        });
        if(!value.kind.endsWith('-empty')){
            for(const [name,mutate] of [
                ['non-advancing read',s=>{s.shapes[0][2]=0;}],['wrong output offset',s=>{s.shapes[0][1]=1;}],
                ['wrong backing capacity',s=>{s.shapes[0][3]=512;}],['input retained',s=>{s.shapes[0][4]=32;}],
                ['outside bytes modified',s=>{s.shapes[0][5]=false;}],['unreviewed shape column',s=>{s.shapes[0].push(0);}],
            ])test('reject body '+name+' '+value.kind,()=>{
                const mutant=structuredClone(value);replace(mutant,mutate);assert.throws(()=>assertFetchBYOBCase(mutant,'reference'));
            });
            test('reject changed actual upstream body '+value.kind,()=>{
                const mutant=structuredClone(value);
                for(const snapshot of [mutant.upstreamBeforeStop,mutant.upstreamAfterStop]){
                    const row=snapshot.records[0],encoded=row.sentChunks.at(-1),body=gunzipSync(Buffer.from(encoded.payload,'base64'));body[body.length-1]^=1;
                    row.sentChunks[row.sentChunks.length-1]={encoding:'gzip-base64',rawBytes:body.length,
                        sha256:createHash('sha256').update(body).digest('hex'),payload:gzipSync(body,{level:9}).toString('base64')};
                }
                assert.throws(()=>assertFetchBYOBCase(mutant,'reference'));
            });
            for(const [name,change] of [
                ['encoding',v=>{v.encoding='unknown';}],['length',v=>{v.rawBytes++;}],
                ['digest',v=>{v.sha256='0'.repeat(64);}],['gzip bytes',v=>{v.payload='AAAA';}],
                ['unreviewed field',v=>{v.unreviewed=true;}],
            ])test('reject body transcript '+name+' '+value.kind,()=>{
                const mutant=structuredClone(value);
                for(const snapshot of [mutant.upstreamBeforeStop,mutant.upstreamAfterStop])change(snapshot.records[0].sentChunks.at(-1));
                assert.throws(()=>assertFetchBYOBCase(mutant,'reference'));
            });
        }
        if(!value.kind.includes('-default-'))for(const [name,mutate] of [
            ['offset EOF not reset',s=>{s.shapes.at(-1)[1]=16;}],
            ['minimum returned before crossing credit window',s=>{s.shapes[0][2]=65536;}],
            ['EOF buffer not detached',s=>{s.shapes.at(-1)[4]=s.input.bufferBytes;}],
            ['EOF outside bytes modified',s=>{s.shapes.at(-1)[5]=false;}],
            ['wrong input view offset',s=>{s.input.byteOffset=0;}],
            ['wrong minimum',s=>{s.input.min=1;}],
        ])test('reject body '+name+' '+value.kind,()=>{
            const mutant=structuredClone(value);replace(mutant,mutate);assert.throws(()=>assertFetchBYOBCase(mutant,'reference'));
        });
        continue;
    }
    for(const [name,mutate] of [
        ['failed capture',v=>{v.failure='failed';}],['forced shutdown',v=>{v.shutdown.forced=true;}],
        ['completion supplied by teardown',v=>{v.allSourcesEndedBeforeStop=false;}],
        ['missing wire bytes',v=>{v.wire.rawTail='';}],
        ['foreign observation realm',v=>{const s=JSON.parse(v.observed.text);s.realm=v.challenge;v.observed.text=JSON.stringify(s);}],
        ...(value.kind.startsWith('brand-')?[]:[
            ['buffer not detached',v=>{const s=JSON.parse(v.observed.text);s.state.afterCall.bufferBytes=s.state.afterCall.bufferBytes===0?32:0;v.observed.text=JSON.stringify(s);}],
            ['wrong reaction order',v=>{const s=JSON.parse(v.observed.text);s.state.trace.reverse();v.observed.text=JSON.stringify(s);}],
        ]),
        ['missing byte checkpoint',v=>{const s=JSON.parse(v.observed.text);s.state.prime.bytes=[1];v.observed.text=JSON.stringify(s);}],
        ['reader lock retained',v=>{const s=JSON.parse(v.observed.text);s.state.lockedAfterRelease=true;v.observed.text=JSON.stringify(s);}],
        ['construction route collapsed',v=>{const s=JSON.parse(v.observed.text);s.state.construction.directPrototype=!s.state.construction.directPrototype;v.observed.text=JSON.stringify(s);}],
    ])test('reject '+name+' '+value.kind,()=>{const mutant=structuredClone(value);mutate(mutant);assert.throws(()=>assertFetchBYOBCase(mutant,'reference'));});
    if(value.kind.startsWith('brand-')){
        const replace=(v,mutate)=>{const d=JSON.parse(v.observed.text);mutate(d.state);v.observed.text=JSON.stringify(d);};
        for(const [name,mutate] of [
            ['option getter observed',s=>{s.optionGets=1;}],
            ['wrong input detached',s=>{s.wrongInputBytes=0;}],['released wrong input detached',s=>{s.releasedWrongInputBytes=0;}],
            ['pending read settled by wrong method',s=>{s.beforeCancel.readSettled=true;}],
            ['closed settled by wrong method',s=>{s.beforeCancel.closedSettled=true;}],
            ['stream unlocked by wrong release',s=>{s.beforeCancel.locked=false;}],
            ['cancellation contaminated',s=>{s.read.error.message='invalid/'+s.challenge;}],
        ])test('reject brand '+name+' '+value.kind,()=>{
            const mutant=structuredClone(value);replace(mutant,mutate);assert.throws(()=>assertFetchBYOBCase(mutant,'reference'));
        });
        const state=JSON.parse(value.observed.text).state;
        for(const field of ['attempts','afterReleaseAttempts'])for(let index=0;index<state[field].length;index++){
            for(const [name,change] of [
                ['asynchronous rejection',attempt=>{attempt.synchronous=false;}],
                ['incorrect error type',attempt=>{attempt.error.name='RangeError';}],
                ['incorrect error text',attempt=>{attempt.error.message='wrong';}],
                ['accepted borrowed operation',attempt=>{delete attempt.error;attempt.returnedUndefined=true;}],
            ])test('reject brand '+field+' '+index+' '+name+' '+value.kind,()=>{
                const mutant=structuredClone(value);replace(mutant,s=>change(s[field][index]));assert.throws(()=>assertFetchBYOBCase(mutant,'reference'));
            });
        }
    }
    const state=JSON.parse(value.observed.text).state;
    if(state.errorIdentity)for(const [name,mutate] of [
        ['closed error identity',s=>{s.errorIdentity.readClosed=false;}],
        ['cancel error identity',s=>{s.errorIdentity.readCancel=false;}],
        ['closed diagnostic reference',s=>{s.closed.error.message='internal error; reference = '+'0'.repeat(24);}],
        ['cancel error class',s=>{s.cancel.error.name='TypeError';}],
    ])test('reject incomplete-element EOF '+name+' '+value.kind,()=>{
        const mutant=structuredClone(value),document=JSON.parse(mutant.observed.text);mutate(document.state);
        mutant.observed.text=JSON.stringify(document);assert.throws(()=>assertFetchBYOBCase(mutant,'reference'));
    });
    for(const [field,replacement] of state.read.value?[
        ['type','ArrayBuffer'],['done',true],['byteOffset',0],['byteLength',99],['bufferBytes',99],['bytes',[0]],['backing',[0]]
    ]:[['message','wrong'],['name','RangeError']])test('reject wrong read '+field+' '+value.kind,()=>{
        const mutant=structuredClone(value),document=JSON.parse(mutant.observed.text);
        document.state.read[state.read.value?'value':'error'][field]=replacement;mutant.observed.text=JSON.stringify(document);
        assert.throws(()=>assertFetchBYOBCase(mutant,'reference'));
    });
}
for(const [name,mutate] of [
    ['missing EOF diagnostic',r=>{r.cases.find(v=>v.kind==='standard-getReader-Uint16Array-min3-eof').stderr='';}],
    ['reused EOF diagnostic',r=>{
        const errors=r.cases.filter(v=>v.kind.startsWith('standard-')&&!v.kind.startsWith('standard-close-')&&JSON.parse(v.observed.text).state.read?.error?.name==='Error');
        assert.equal(errors.length,12);
        const original=JSON.parse(errors[0].observed.text).state.read.error.message;
        const previous=JSON.parse(errors[1].observed.text).state.read.error.message;
        errors[1].observed.text=errors[1].observed.text.replaceAll(previous,original);
        errors[1].stderr=errors[1].stderr.replaceAll(previous.slice(-24),original.slice(-24));
    }],
    ['missing case',r=>r.cases.pop()],['duplicate case',r=>{r.cases[1]=structuredClone(r.cases[0]);}],
    ['wrong source digest',r=>{r.inputs.fixture='0'.repeat(64);}],['incomplete',r=>{r.complete=false;}],
    ['foreign reference binary',r=>{r.binaries.host='0'.repeat(64);}],['wrong client runtime',r=>{r.client.node='v0.0.0';}],
    ['wrong certificate bytes',r=>{r.certificateInputs.serverPEM+='\n';}],['missing source identity',r=>{delete r.inputs.upstreamBase;}],
    ['reused VM',r=>{r.cases[1].initial.text=r.cases[0].initial.text;}],['reused challenge',r=>{r.cases[1].challenge=r.cases[0].challenge;}],
    ['unknown report field',r=>{r.unreviewed=true;}],['unknown JS view presented as qualified',r=>{r.cases[0].kind='standard-getReader-ArrayBuffer-default-held';}],
])test('reject report '+name,()=>{const mutant=structuredClone(sample);mutate(mutant);assert.throws(()=>validateFetchBYOBReport(mutant,expected));});
test('full native backend inventory validates synthetic controls only',()=>{
    const candidates=['single-worker','static-pool','managed'].map(mode=>{
        const report=structuredClone(sample);report.mode=mode;report.binaries={host:expected.host,worker:expected.worker};
        for(const value of report.cases)value.shutdown={code:0,signal:null,forced:false};return report;
    });
    assert.deepEqual(compareFetchBYOBReports(sample,candidates,expected),{complete:true,casesPerBackend:487,backends:3});
    const reference=structuredClone(sample),mutants=structuredClone(candidates);
    for(const report of [reference,...mutants]){const state=JSON.parse(report.cases[0].observed.text);state.state.read.value.done=true;report.cases[0].observed.text=JSON.stringify(state);}
    assert.throws(()=>compareFetchBYOBReports(reference,mutants,expected),'matching wrong results cannot pass');
    for(const [label,change] of [
        ['capacity error class',(r)=>{const value=r.cases.find(value=>value.kind==='standard-getReader-Float64Array-min3-held');
            const d=JSON.parse(value.observed.text);d.state.read.error.name='RangeError';value.observed.text=JSON.stringify(d);}],
        ['validation getter order',(r)=>{const value=r.cases.find(value=>value.kind==='validation-getReader-read');
            const d=JSON.parse(value.observed.text);d.state.rows[0].events.unshift('get-min');value.observed.text=JSON.stringify(d);}],
        ['validation exception identity',(r)=>{const value=r.cases.find(value=>value.kind==='validation-direct-least');
            const d=JSON.parse(value.observed.text);d.state.rows.find(row=>row.scenario==='conversion-error').read.error.sameSentinel=false;value.observed.text=JSON.stringify(d);}],
    ]){
        const ref=structuredClone(sample),products=structuredClone(candidates);
        for(const report of [ref,...products])change(report);
        assert.throws(()=>compareFetchBYOBReports(ref,products,expected),'matching wrong '+label+' cannot pass');
    }
});

function shards(mode){
    return [['readers',0,100],['constructors',100,200],['reflect-standard',200,306],['views-brands',306,414],['bodies',414,435],['standard-edges',435,471],['standard-close',471,487]].map(([shard,start,end])=>{
        const report=structuredClone({...sample,cases:sample.cases.slice(start,end)});report.shard=shard;report.mode=mode;
        if(mode!=='reference'){report.binaries={host:expected.host,worker:expected.worker};
            for(const value of report.cases)value.shutdown={code:0,signal:null,forced:false};}
        return report;
    });
}
test('seven fixed shards retain every reviewed case and fresh VM',()=>{
    assert.deepEqual(fetchBYOBShardNames,['readers','constructors','reflect-standard','views-brands','bodies','standard-edges','standard-close']);
    assert.deepEqual(Object.values(fetchBYOBShardCases).map(v=>v.length),[100,100,106,108,21,36,16]);
    assert.deepEqual(Object.values(fetchBYOBShardCases).flat(),inventory);
    assert.deepEqual(compareFetchBYOBShardReports(shards('reference'),['single-worker','static-pool','managed'].map(shards),expected),
        {complete:true,casesPerBackend:487,backends:3,shardsPerBackend:7});
});
for(const [name,mutate] of [
    ['missing shard',r=>r.pop()],['duplicate shard',r=>{r[1]=structuredClone(r[0]);}],
    ['wrong shard name',r=>{r[0].shard='other';}],['missing shard identity',r=>{delete r[0].shard;}],
    ['mixed backend',r=>{r[1].mode='single-worker';}],['case absent',r=>r[0].cases.pop()],
    ['VM reused between shards',r=>{r[1].cases[0].initial.text=r[0].cases[0].initial.text;}],
    ['challenge reused between shards',r=>{r[1].cases[0].challenge=r[0].cases[0].challenge;}],
    ['stale shard inputs',r=>{r[1].inputs.fixture='0'.repeat(64);}],
])test('reject partition '+name,()=>{
    const reference=shards('reference');mutate(reference);
    assert.throws(()=>compareFetchBYOBShardReports(reference,['single-worker','static-pool','managed'].map(shards),expected));
});
