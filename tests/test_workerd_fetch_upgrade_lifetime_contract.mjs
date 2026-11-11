import assert from 'node:assert/strict';
import test from 'node:test';
import fs from 'node:fs';
import {spawnSync} from 'node:child_process';
import {fileURLToPath} from 'node:url';
import {fetchUpgradeLifetimeCases,assertFetchUpgradeLifetimeCase,validateFetchUpgradeLifetimeReport,
    compareFetchUpgradeLifetimeReports} from '../tools/workerd-fetch-upgrade-lifetime-contract.mjs';

// Exact native Windows workerd observations. Profile copies and candidates
// below exercise validators only; actual Linux/product qualification executes
// fresh binaries, records and complete case inventories.
const stored=JSON.parse(fs.readFileSync(new URL('./fixtures/workerd-fetch-upgrade-lifetime-report.json',import.meta.url),'utf8'));
const knownReferenceError={type:'error',state:3,message:'Uncaught Error: Network connection lost.',error:'Error: Network connection lost.',
    errorType:'object',errorIsError:true,errorName:'Error',errorMessage:'Network connection lost.'};
const legacyCases=fetchUpgradeLifetimeCases.filter(kind=>!kind.startsWith('await-')&&kind!=='fetched-disconnect-close');
const documents=['beforeAction','afterAction','closedParent','checkpoint','afterRelease','observed','finalState'];
function change(value,key,mutate) {const document=JSON.parse(value[key].text);mutate(document.state,document);value[key].text=JSON.stringify(document);}
function sample(kind) {return structuredClone(stored.cases.find(value=>value.kind===kind));}
function snapshots(value) {return ['beforeUpstream','closedUpstream','checkpointUpstream','afterReleaseUpstream','upstream'].filter(key=>value[key]);}
function request(entry,mutate) {entry.request=mutate(entry.request);entry.rawRequest=Buffer.from(entry.request+'\r\n\r\n','latin1').toString('base64');}
function matrix(platform='win32') {
    const reference=structuredClone(stored),lock=JSON.parse(fs.readFileSync(new URL(platform==='win32'?'./fixtures/workerd-ws/reference-lock-windows.json':'./fixtures/workerd-ws/reference-lock.json',import.meta.url),'utf8'));
    reference.referenceLock=lock;reference.binaries={host:lock.binary_sha256,worker:null};
    const expected={runId:reference.runId,lock,inputs:reference.inputs,host:'a'.repeat(64),worker:'b'.repeat(64)};
    const candidates=['single-worker','static-pool',...(platform==='linux'?['managed']:[])].map(mode=>{
        const report=structuredClone(reference);report.mode=mode;report.binaries={host:expected.host,worker:expected.worker};
        for(const value of report.cases) {
            value.shutdown={code:0,signal:null,forced:false};
            for(const key of documents)if(value[key])change(value,key,state=>{
                for(const owner of Object.values(state.owners??{a:state}))if(owner?.socket)owner.socket.events=owner.socket.events.filter(event=>event.type!=='error');
            });
        }
        return report;
    });
    return {reference,candidates,expected};
}
test('literal complete inventory distinguishes parent transports from shared I/O-origin lifetimes',()=>{
    assert.deepEqual(fetchUpgradeLifetimeCases,['close-pending','close-body','close-unread','close-unaccepted','accepted-independent','two-owners','cancel-reader','abort-body',
        'same-origin-parent-pending','same-origin-child-pending','same-origin-parent-complete','same-origin-child-complete','await-open-parent','await-close-parent','await-abrupt-parent','await-abrupt-last','fetched-disconnect-close','await-deferred-abrupt-parent','await-deferred-abrupt-last']);
    assert.ok(Object.isFrozen(fetchUpgradeLifetimeCases));
});
for(const platform of ['win32','linux'])test(platform+' validator profile accepts complete reference replay and synthetic healthy candidates',()=>{
    const m=matrix(platform);assert.equal(validateFetchUpgradeLifetimeReport(m.reference,m.expected).length,19);
    assert.deepEqual(compareFetchUpgradeLifetimeReports(m.reference,m.candidates,m.expected),{complete:true,casesPerBackend:19,backends:platform==='linux'?3:2});
});
for(const kind of legacyCases)for(const [name,mutate] of [
    ['missing parent Close',value=>value.wires.a.frames.pop()],
    ['missing parent EOF',value=>{value.wires.a.peerTerminated=false;}],
    ['missing actual Close reply',value=>{value.wires.a.closeReplySent=false;}],
    ['changed raw parent bytes',value=>{value.wires.a.rawTail='';}],
    ['missing owner field',value=>change(value,'beforeAction',state=>{delete state.owners.a.headers;})],
    ['invented settled read',value=>change(value,'finalState',state=>{state.owners.a.second={done:true,value:null};})],
    ['late callback',value=>change(value,'finalState',state=>state.owners.a.trace.push('late'))],
    ['foreign realm',value=>change(value,'observed',(_state,document)=>{document.realm='00000000-0000-4000-8000-000000000001';})],
    ['wrong challenge',value=>{value.challenge='00000000-0000-4000-8000-000000000001';}],
    ['missing actual upstream',value=>value.upstream.pop()],
    ['wrong target port',value=>{value.targets[0].port=value.targets[0].port===65535?65534:65535;}],
    ['missing upstream EOF',value=>{value.upstream[0].peerEnded=false;value.upstream[0].events=value.upstream[0].events.filter(event=>event!=='peer-ended');}],
    ['local disposal hidden as terminal',value=>{value.upstream[0].fixtureDestroyed=true;}],
    ['reset hidden as reference EOF',value=>{value.upstream[0].peerEnded=false;value.upstream[0].peerReset=true;}],
    ['wrong lifetime decision',value=>{value.terminalAfterParent=!value.terminalAfterParent;}],
    ['forced cleanup',value=>{value.shutdown.forced=true;}],
    ['abnormal worker exit',value=>{value.shutdown.code=1;value.shutdown.signal=null;}],
    ['sanitizer report',value=>{value.stderr+='ERROR: AddressSanitizer: invalid access';}],
    ['rewritten request between snapshots',value=>request(value.upstream[0],text=>text.replace(/Sec-WebSocket-Key: [^\r\n]+/i,'Sec-WebSocket-Key: AAAAAAAAAAAAAAAAAAAAAA=='))],
])test(kind+' rejects '+name,()=>{const value=sample(kind);assert.throws(()=>{mutate(value);assertFetchUpgradeLifetimeCase(value,'reference');});});
for(const kind of legacyCases)for(const [name,mutate] of [
    ['missing Upgrade',text=>text.split('\r\n').filter(line=>!/^upgrade:/i.test(line)).join('\r\n')],
    ['missing Connection',text=>text.split('\r\n').filter(line=>!/^connection:/i.test(line)).join('\r\n')],
    ['wrong nonce',text=>text.replace(/Sec-WebSocket-Key: [^\r\n]+/i,'Sec-WebSocket-Key: invalid')],
    ['wrong version',text=>text.replace(/Sec-WebSocket-Version: 13/i,'Sec-WebSocket-Version: 12')],
    ['wrong Host authority',text=>text.replace(/Host: [^\r\n]+/i,'Host: unrelated.example')],
    ['unexpected request body header',text=>text+'\r\nContent-Length: 1'],
])test(kind+' rejects '+name,()=>{const value=sample(kind);for(const key of snapshots(value))request(value[key][0],mutate);assert.throws(()=>assertFetchUpgradeLifetimeCase(value,'reference'));});
for(const kind of ['close-body','cancel-reader','abort-body'])for(const field of ['value','done','bodyUsed'])test(kind+' requires complete consumed first marker '+field,()=>{
    const value=sample(kind);for(const key of ['beforeAction','closedParent','observed','finalState'])change(value,key,state=>{state.owners.a.first[field]=field==='value'?'partial':!state.owners.a.first[field];});
    assert.throws(()=>assertFetchUpgradeLifetimeCase(value,'reference'));
});
for(const kind of ['accepted-independent','two-owners',...fetchUpgradeLifetimeCases.filter(kind=>kind.startsWith('same-origin-'))]) {
    const role=kind==='two-owners'?'b':'a';
    for(const [name,mutate] of [
        ['missing post-parent data',socket=>socket.messages.pop()],
        ['lost child Close',socket=>{socket.closed=null;}],
        ['wrong child code',socket=>{socket.closed.code=1006;}],
        ['unclean child Close',socket=>{socket.closed.wasClean=false;}],
        ['unaccepted independent endpoint',socket=>{socket.accepted=false;}],
        ['early child event',socket=>socket.events.unshift({type:'error',message:'unexpected'})],
    ])test(kind+' rejects '+name,()=>{
        const value=sample(kind);for(const key of ['observed','finalState'])change(value,key,state=>mutate(state.owners[role].socket));assert.throws(()=>assertFetchUpgradeLifetimeCase(value,'reference'));
    });
    for(const [name,mutate] of [
        ['unmasked client frame',frame=>{frame.masked=false;}],['invalid mask',frame=>{frame.mask='bad';}],
        ['wrong raw payload',frame=>{frame.payload=Buffer.from('wrong').toString('base64');}],['truncated frame',frame=>{frame.fin=false;}],
    ])test(kind+' rejects '+name,()=>{const value=sample(kind),index=kind==='two-owners'?1:0;mutate(value.upstream[index].frames[0]);assert.throws(()=>assertFetchUpgradeLifetimeCase(value,'reference'));});
    test(kind+' permits only the exact reference Close race',()=>{
        const value=sample(kind);for(const key of ['observed','finalState'])change(value,key,state=>{state.owners[role].socket.events=[structuredClone(knownReferenceError),state.owners[role].socket.events.at(-1)];});
        assertFetchUpgradeLifetimeCase(value,'reference');assert.throws(()=>assertFetchUpgradeLifetimeCase(value,'single-worker'));
        for(const key of ['observed','finalState'])change(value,key,state=>{state.owners[role].socket.events[0].errorMessage='other';});assert.throws(()=>assertFetchUpgradeLifetimeCase(value,'reference'));
    });
}
for(const kind of fetchUpgradeLifetimeCases.filter(kind=>kind.startsWith('same-origin-')))for(const [name,mutate] of [
    ['premature pending fetch cancellation',value=>{value.checkpointUpstream[1].peerEnded=true;value.checkpointUpstream[1].events.push('peer-ended');}],
    ['last connection leaks pending resource',value=>{value.upstream[1].peerEnded=false;value.upstream[1].events=value.upstream[1].events.filter(event=>event!=='peer-ended');}],
    ['lost shared origin checkpoint',value=>{value.checkpoint=null;}],
])test(kind+' rejects '+name,()=>{const value=sample(kind);assert.throws(()=>{mutate(value);assertFetchUpgradeLifetimeCase(value,'reference');});});
for(const kind of ['same-origin-parent-complete','same-origin-child-complete'])for(const [name,mutate] of [
    ['stopped original timer',value=>change(value,'checkpoint',state=>{state.owners.b.timerTicks=JSON.parse(value.closedParent.text).state.owners.b.timerTicks;})],
    ['timer runs after final close',value=>change(value,'finalState',state=>{state.owners.b.timerTicks++;})],
    ['backwards timer count',value=>change(value,'afterRelease',state=>{state.owners.b.timerTicks=-1;})],
    ['fractional timer count',value=>change(value,'beforeAction',state=>{state.owners.b.timerTicks=0.5;})],
    ['lost original promise continuation',value=>change(value,'afterRelease',state=>{state.owners.b.first=null;state.owners.b.trace.pop();})],
    ['wrong released response body',value=>change(value,'afterRelease',state=>{state.owners.b.first.value='wrong';})],
    ['missing released transport EOF',value=>{value.afterReleaseUpstream[1].peerEnded=false;}],
])test(kind+' rejects '+name,()=>{const value=sample(kind);mutate(value);assert.throws(()=>assertFetchUpgradeLifetimeCase(value,'reference'));});
for(const platform of ['win32','linux'])for(const [name,mutate] of [
    ['missing backend',m=>m.candidates.pop()],['duplicate backend',m=>{m.candidates[1].mode='single-worker';}],
    ['stale source',m=>{m.candidates[0].inputs.contract='c'.repeat(64);}],['stale binary',m=>{m.candidates[0].binaries.worker='c'.repeat(64);}],
    ['wrong run identity',m=>{m.candidates[0].runId='different';}],['wrong reference profile',m=>{m.reference.referenceLock.compatibility_date='2026-08-21';}],
    ['incomplete collection',m=>{m.candidates[0].complete=false;}],['reordered inventory',m=>m.candidates[0].cases.reverse()],
    ['all sides omit same case',m=>{m.reference.cases.pop();for(const report of m.candidates)report.cases.pop();}],
    ['all sides make same lifecycle mistake',m=>{for(const report of [m.reference,...m.candidates])report.cases[0].terminalAfterParent=false;}],
    ['all sides use same wrong body',m=>{for(const report of [m.reference,...m.candidates])change(report.cases.find(value=>value.kind==='same-origin-child-complete'),'afterRelease',state=>{state.owners.b.first.value='wrong';});}],
    ['foreign realm reuse',m=>{m.candidates[0].cases[1].initial.text=m.candidates[0].cases[0].initial.text;}],
])test(platform+' matrix rejects '+name,()=>{const m=matrix(platform);mutate(m);assert.throws(()=>compareFetchUpgradeLifetimeReports(m.reference,m.candidates,m.expected));});
for(const args of [['--filter','close-pending'],['--case','close-pending'],['--mode','reference']])test('driver rejects weakened or aliased selector '+args.join(' '),()=>{
    const result=spawnSync(process.execPath,[fileURLToPath(new URL('./test_websocket_fetch_upgrade_lifetime.mjs',import.meta.url)),...args],{encoding:'utf8'});assert.notEqual(result.status,0);
});

const awaitDocuments=['beforeAction','afterAction','checkpoint','afterRelease','observed','finalState'];
const awaitSnapshots=['beforeUpstream','actionUpstream','checkpointUpstream','releaseUpstream','upstream'];
for(const kind of fetchUpgradeLifetimeCases.filter(kind=>kind.startsWith('await-')))for(const [name,mutate] of [
    ['missing timer field',value=>change(value,'beforeAction',state=>{delete state.timerTicks;})],
    ['wrong realm',value=>change(value,'afterRelease',(_state,document)=>{document.realm='00000000-0000-4000-8000-000000000001';})],
    ['wrong state challenge',value=>change(value,'observed',state=>{state.challenge='different';})],
    ['failed state HTTP response',value=>{value.checkpoint.status=500;}],
    ['wrong parent readyState before close',value=>change(value,'observed',state=>{state.parentClose.before=0;})],
    ['wrong parent readyState after close',value=>change(value,'observed',state=>{state.parentClose.after=0;})],
    ['close call failure',value=>change(value,'observed',state=>{state.parentClose.closeFailure={name:'Error',message:'failed'};})],
    ['incorrect parent EOF attribution',value=>{value.wire.peerTerminated=!value.wire.peerTerminated;}],
    ['missing actual parent completion',value=>{value.wire.completion='deadline';}],
    ['incorrect parent Close reply',value=>{value.wire.closeReplySent=!value.wire.closeReplySent;}],
    ['missing raw parent bytes',value=>{value.wire.rawTail='';}],
    ['missing parent frame',value=>{value.wire.frames.pop();}],
    ['wrong target port',value=>{value.targets[0].port=value.targets[0].port===65535?65534:65535;}],
    ['wrong pending request lifetime',value=>{value.pendingTerminal=!value.pendingTerminal;}],
    ['missing actual final upstream EOF',value=>{value.upstream[0].peerEnded=false;}],
    ['fixture disposal presented as EOF',value=>{value.upstream[0].fixtureDestroyed=true;}],
    ['upstream transport error',value=>{value.upstream[0].error='unexpected';}],
    ['missing causal upstream event',value=>{value.upstream[0].events.pop();}],
    ['invented wire frame',value=>{value.upstream[0].frames.push({opcode:1,fin:true,masked:true,mask:'00000000',payload:'YQ=='});}],
    ['late callback',value=>change(value,'finalState',state=>state.trace.push('late'))],
    ['wrong completed response',value=>change(value,'observed',state=>{state.response={status:409,text:'wrong',bodyUsed:true};})],
    ['continued final timer',value=>change(value,'finalState',state=>{state.timerTicks++;})],
    ['wrong timer at remaining-owner checkpoint',value=>change(value,'checkpoint',state=>{state.timerTicks=JSON.parse(value.afterAction.text).state.timerTicks+(kind==='await-abrupt-last'?1:0);})],
    ['forced process cleanup',value=>{value.shutdown.forced=true;}],
    ['abnormal process exit',value=>{value.shutdown.code=1;value.shutdown.signal=null;}],
    ['runtime poison',value=>{value.stderr+='WORKER POISONED: terminal continuation leak';}],
    ['immutable request rewrite',value=>request(value.upstream[0],text=>text.replace(/Sec-WebSocket-Key: [^\r\n]+/i,'Sec-WebSocket-Key: AAAAAAAAAAAAAAAAAAAAAA=='))],
    ['all snapshots carry same extra default header',value=>{for(const key of awaitSnapshots)for(const entry of value[key])request(entry,text=>text+'\r\nUser-Agent: invented');}],
    ['invented early response headers',value=>{value.beforeUpstream.at(-1).responseStatus=409;}],
])test(kind+' independent expectation rejects '+name,()=>{
    const value=sample(kind),before=structuredClone(value);mutate(value);assert.notDeepEqual(value,before,'negative control changes the observed case');
    assert.throws(()=>assertFetchUpgradeLifetimeCase(value,'reference'));
});
for(const kind of ['await-open-parent','await-close-parent','await-abrupt-parent'])for(const [name,mutate] of [
    ['lost awaiting handler',state=>{state.response=null;state.trace=state.trace.filter(item=>item!=='headers'&&item!=='resumed');}],
    ['wrong response bodyUsed',state=>{state.response.bodyUsed=false;}],
    ['lost accepted child message',state=>{state.socket.messages.pop();}],
    ['missing child Close event',state=>{state.socket.events=[];}],
])test(kind+' rejects '+name,()=>{
    const value=sample(kind);for(const key of ['observed','finalState'])change(value,key,mutate);assert.throws(()=>assertFetchUpgradeLifetimeCase(value,'reference'));
});
for(const platform of ['win32','linux'])for(const [name,mutate] of [
    ['all sides lose the same awaiting handler',value=>{for(const key of ['afterRelease','observed','finalState'])change(value,key,state=>{state.response=null;state.trace=state.trace.filter(item=>item!=='headers'&&item!=='resumed');});}],
    ['all sides ignore local close after disconnect',value=>{for(const key of awaitDocuments)if(JSON.parse(value[key].text).state.parentClose)change(value,key,state=>{state.parentClose.after=1;});}],
])test(platform+' rejects '+name,()=>{
    const m=matrix(platform);for(const report of [m.reference,...m.candidates])mutate(report.cases.find(value=>value.kind==='await-abrupt-parent'));
    assert.throws(()=>compareFetchUpgradeLifetimeReports(m.reference,m.candidates,m.expected));
});

for(const [name,mutate] of [
    ['incoming close behavior applied to fetched socket',value=>change(value,'observed',state=>{state.states.after=2;state.states.microtask=3;state.states.timer=3;})],
    ['wrong callback entry state',value=>change(value,'observed',state=>{state.states.before=3;})],
    ['missing microtask state',value=>change(value,'observed',state=>{delete state.states.microtask;})],
    ['wrong timer state',value=>change(value,'observed',state=>{state.states.timer=3;})],
    ['wrong fetched HTTP status',value=>change(value,'before',state=>{state.status=409;})],
    ['wrong state before accept',value=>change(value,'before',state=>{state.beforeAccept=0;})],
    ['wrong abnormal Close code',value=>change(value,'observed',state=>{state.close.code=1000;})],
    ['unexpected error event',value=>change(value,'observed',state=>{state.errors.push({message:'wrong'});})],
    ['late handler callback',value=>change(value,'finalState',state=>state.trace.push('late'))],
    ['foreign realm',value=>change(value,'observed',(_state,document)=>{document.realm='00000000-0000-4000-8000-000000000001';})],
    ['missing final same-realm response',value=>{value.finalState.status=500;}],
    ['missing parent EOF',value=>{value.wire.peerTerminated=false;}],
    ['missing actual parent Close reply',value=>{value.wire.closeReplySent=false;}],
    ['missing raw parent frame',value=>{value.wire.frames.pop();}],
    ['wrong raw parent bytes',value=>{value.wire.rawTail='';}],
    ['missing intentional fixture action',value=>{value.upstreamAction={};}],
    ['wrong target port',value=>{value.target.port=value.target.port===65535?65534:65535;}],
    ['local disposal presented as remote EOF',value=>{value.upstream[0].peerEnded=true;}],
    ['all snapshots invent remote EOF',value=>{value.upstream[0].peerEnded=value.upstreamBefore[0].peerEnded=true;}],
    ['invented upstream Close frame',value=>{value.upstream[0].frames.push({opcode:8,fin:true,masked:true,mask:'00000000',payload:'A+hjaGlsZA=='});}],
    ['all snapshots omit request header',value=>{for(const entries of [value.upstreamBefore,value.upstream])request(entries[0],text=>text.replace(/\r\nConnection: [^\r\n]+/i,''));}],
    ['all snapshots include extra default header',value=>{for(const entries of [value.upstreamBefore,value.upstream])request(entries[0],text=>text+'\r\nUser-Agent: invented');}],
    ['unmasked actual frame',value=>{value.upstream[0].frames[0].masked=false;}],
    ['partial actual frame',value=>{value.upstream[0].frames[0].fin=false;}],
    ['wrong frame payload',value=>{value.upstream[0].frames[0].payload='YQ==';}],
    ['wrong handshake status',value=>{value.upstream[0].responseStatus=409;}],
    ['fixture error',value=>{value.upstream[0].error='unexpected';}],
    ['forced process cleanup',value=>{value.shutdown.forced=true;}],
    ['runtime poison',value=>{value.stderr+='WORKER POISONED';}],
])test('fetched-disconnect-close rejects '+name,()=>{
    const value=sample('fetched-disconnect-close'),before=structuredClone(value);mutate(value);assert.notDeepEqual(value,before);
    assert.throws(()=>assertFetchUpgradeLifetimeCase(value,'reference'));
});
for(const platform of ['win32','linux'])test(platform+' rejects the same incoming endpoint close semantics on all fetched endpoints',()=>{
    const m=matrix(platform);for(const report of [m.reference,...m.candidates])for(const key of ['observed','finalState'])
        change(report.cases.find(value=>value.kind==='fetched-disconnect-close'),key,state=>{state.states.after=2;state.states.microtask=3;state.states.timer=3;});
    assert.throws(()=>compareFetchUpgradeLifetimeReports(m.reference,m.candidates,m.expected));
});

for(const kind of ['await-deferred-abrupt-parent','await-deferred-abrupt-last'])for(const [name,mutate] of [
    ['lost awaiting handler',state=>{state.response=null;state.trace=state.trace.filter(item=>!['headers','resumed'].includes(item));}],
    ['unconsumed completed response',state=>{state.response.bodyUsed=false;}],
    ['lost local close acknowledgement',state=>{state.trace=state.trace.filter(item=>item!=='local-close');}],
    ['missing local close entry state',state=>{delete state.parentClose.localBefore;}],
    ['wrong local close entry state',state=>{state.parentClose.localBefore=1;}],
    ['missing local close exit state',state=>{delete state.parentClose.localAfter;}],
    ['outgoing direction still open',state=>{state.parentClose.localAfter=2;}],
])test(kind+' rejects '+name,()=>{
    const value=sample(kind),before=structuredClone(value);
    for(const key of ['afterRelease','observed','finalState'])change(value,key,mutate);
    assert.notDeepEqual(value,before);assert.throws(()=>assertFetchUpgradeLifetimeCase(value,'reference'));
});
for(const kind of ['await-deferred-abrupt-parent','await-deferred-abrupt-last'])for(const [name,mutate] of [
    ['all snapshots hide early pending EOF',value=>{for(const key of ['beforeUpstream','actionUpstream','checkpointUpstream']){value[key].at(-1).peerEnded=true;value[key].at(-1).events.push('peer-ended');}}],
    ['missing held request checkpoint',value=>{value.checkpointUpstream.pop();}],
    ['timer stops before local close',value=>change(value,'checkpoint',state=>{state.timerTicks=JSON.parse(value.afterAction.text).state.timerTicks;})],
    ['callback runs after local close',value=>{for(const key of ['observed','finalState'])change(value,key,state=>{state.trace.push('late');});}],
])test(kind+' rejects '+name,()=>{
    const value=sample(kind),before=structuredClone(value);mutate(value);
    assert.notDeepEqual(value,before);assert.throws(()=>assertFetchUpgradeLifetimeCase(value,'reference'));
});
for(const platform of ['win32','linux'])for(const kind of ['await-deferred-abrupt-parent','await-deferred-abrupt-last'])for(const [name,mutate] of [
    ['premature EOF',value=>{value.checkpointUpstream.at(-1).peerEnded=true;}],
    ['ignored local close',value=>{for(const key of ['afterRelease','observed','finalState'])change(value,key,state=>{state.parentClose.localAfter=2;});}],
    ['lost response continuation',value=>{for(const key of ['afterRelease','observed','finalState'])change(value,key,state=>{state.response=null;state.trace=state.trace.filter(item=>item!=='resumed');});}],
])test(platform+' rejects '+kind+' same wrong '+name+' on every side',()=>{
    const m=matrix(platform);for(const report of [m.reference,...m.candidates])mutate(report.cases.find(value=>value.kind===kind));
    assert.throws(()=>compareFetchUpgradeLifetimeReports(m.reference,m.candidates,m.expected));
});
