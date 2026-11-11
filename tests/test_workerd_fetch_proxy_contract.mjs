import assert from 'node:assert/strict';
import test from 'node:test';
import {createHash} from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import {spawnSync} from 'node:child_process';
import {fileURLToPath} from 'node:url';
import {readFetchProxySample} from './helpers/ws_fetch_proxy_sample.mjs';
import {fetchProxyCases,assertFetchProxyCase,validateFetchProxyReport,compareFetchProxyReports} from '../tools/workerd-fetch-proxy-contract.mjs';

const stored=readFetchProxySample();
const sample=kind=>structuredClone(stored.cases.find(value=>value.kind===kind));
function change(value,key,mutate){const document=JSON.parse(value[key].text);mutate(document.state,document);value[key].text=JSON.stringify(document);}
function matrix(platform='linux'){
    // These profile/backend copies test the contract only. Both native
    // references and all product modes must execute independently in the gate.
    const reference=structuredClone(stored);
    const lock=JSON.parse(fs.readFileSync(new URL(platform==='win32'?'./fixtures/workerd-ws/reference-lock-windows.json':'./fixtures/workerd-ws/reference-lock.json',import.meta.url),'utf8'));
    reference.referenceLock=lock;reference.binaries={host:lock.binary_sha256,worker:null};
    const expected={runId:reference.runId,lock,inputs:reference.inputs,host:'a'.repeat(64),worker:'b'.repeat(64)};
    const candidates=['single-worker','static-pool',...(platform==='linux'?['managed']:[])].map(mode=>{
        const report=structuredClone(reference);report.mode=mode;report.binaries={host:expected.host,worker:expected.worker};
        for(const value of report.cases)value.shutdown={code:0,signal:null,forced:false};return report;
    });return {reference,candidates,expected};
}
test('fixed inventory covers native transfer, source lifetime and RequestInfo conversion',()=>{
    assert.deepEqual(fetchProxyCases,['direct','rewrap','upstream-first','large-binary','client-abrupt','upstream-abrupt',
        'server-close','accepted','reuse','cross-origin','slow-upstream','upstream-first-binary','intrinsic-traps',
        'source-lifetime','source-live-sibling','source-http','source-parked-promise','source-http-request',
        'source-http-object','request-info-upgrade-url','request-info-upgrade-request','foreign-methods']);
    assert.ok(Object.isFrozen(fetchProxyCases));
});
test('caller-supplied non-UUID gate run identifiers retain exact matching',()=>{
    const m=matrix();m.expected.runId='proxy-gate-validation-20261020';
    for(const report of [m.reference,...m.candidates])report.runId=m.expected.runId;
    assert.equal(compareFetchProxyReports(m.reference,m.candidates,m.expected).complete,true);
    m.reference.runId='';assert.throws(()=>validateFetchProxyReport(m.reference,m.expected));
});
for(const platform of ['linux','win32'])test(platform+' validator profile accepts complete actual reference replay and synthetic candidates',()=>{
    const m=matrix(platform);assert.equal(validateFetchProxyReport(m.reference,m.expected).length,22);
    assert.deepEqual(compareFetchProxyReports(m.reference,m.candidates,m.expected),{complete:true,casesPerBackend:22,backends:platform==='linux'?3:2});
});
for(const kind of fetchProxyCases)for(const [label,mutate] of [
    ['missing observation',value=>{delete value.observed;}],
    ['fixture failure',value=>{value.failure='injected fixture error';}],
    ['cleanup failure',value=>{value.cleanupFailure='injected cleanup error';}],
    ['forced shutdown',value=>{value.shutdown.forced=true;}],
    ['abnormal exit',value=>{value.shutdown={code:1,signal:null,forced:false};}],
    ['poisoned runtime',value=>{value.stderr+='WORKER POISONED';}],
    ['sanitizer finding',value=>{value.stderr+='AddressSanitizer: invalid access';}],
    ['incorrect authority',value=>{value.target.host='unrelated.example';}],
    ['incorrect source port',value=>{value.target.port=value.target.port===65535?65534:65535;}],
    ['missing raw source',value=>{value.upstreamBeforeStop.pop();}],
    ['fixture termination disguised as peer EOF',value=>{value.upstreamBeforeStop[0].fixtureDestroyed=!value.upstreamBeforeStop[0].fixtureDestroyed;}],
    ['wrong raw request',value=>{value.upstreamBeforeStop[0].rawRequest='';}],
    ['upstream peer error',value=>{value.upstreamBeforeStop[0].error='injected transport failure';}],
    ['wrong source handshake',value=>{value.upstreamBeforeStop[0].responseHeader=value.upstreamBeforeStop[0].responseHeader.replace('Switching Protocols','Other');}],
    ['different VM after exchange',value=>change(value,'observed',(_state,document)=>{document.realm='00000000-0000-4000-8000-000000000001';})],
    ['wrong fetch result',value=>change(value,'observed',state=>{state.status=200;})],
    ['missing live state',value=>{value.observed.status=500;}],
    ['substituted incoming nonce',value=>{value.headers.requestKey='AAAAAAAAAAAAAAAAAAAAAA==';}],
])test(kind+' rejects '+label,()=>{
    const value=sample(kind);mutate(value);assert.throws(()=>assertFetchProxyCase(value,'reference'));
});
for(const kind of fetchProxyCases.filter(kind=>kind!=='cross-origin'))for(const [label,mutate] of [
    ['rewritten downstream bytes',value=>{value.wire.rawTail='AA==';}],
    ['wrong echo',value=>{value.echo='wrong';}],
    ['missing Close or last message',value=>{value.wire.frames.pop();}],
    ['missing source masking',value=>{value.upstreamBeforeStop[0].frames[0].masked=false;}],
]){
    // Accepted endpoints have no data frames; its empty wire is asserted by
    // the positive replay and explicit invalid-response tests below.
    if(kind==='accepted'&&(label!=='wrong echo'))continue;
    test(kind+' rejects '+label,()=>{const value=sample(kind);mutate(value);assert.throws(()=>assertFetchProxyCase(value,'reference'));});
}
for(const [kind,stage,mutate,label] of [
    ['source-lifetime','checkpoint',state=>{state.timerFired=1;},'native pump retaining the HTTP timer'],
    ['source-live-sibling','checkpoint',state=>{state.timerFired=0;},'accepted sibling losing its original scope'],
    ['source-live-sibling','checkpoint',state=>{state.afterChildTimerFired=1;},'late timer after the last JavaScript owner'],
    ['source-live-sibling','checkpoint',state=>{state.childEcho='invented';},'invented child echo'],
    ['source-live-sibling','checkpoint',state=>{state.afterTransfer.accept.error.message='wrong';},'wrong released endpoint error'],
    ['source-live-sibling','checkpoint',state=>{state.afterTransfer.close.returned={type:'boolean',value:true};},'wrong no-op return'],
    ['source-live-sibling','mark',state=>{state.sawHeaderBeforeTimer=false;},'timer firing before header observation'],
    ['source-parked-promise','postWake',state=>{state.future=1;},'foreign resolver reviving an expired continuation'],
    ['source-http','checkpoint',state=>{state.heldBodyFinished=true;},'invented complete HTTP body'],
    ['source-http','checkpoint',state=>{state.heldBodyFailure=true;},'late body failure callback'],
    ['source-http-request','observed',state=>{state.sideInitOrder.reverse();},'wrong Request conversion order'],
    ['source-http-object','observed',state=>{state.rejection.synchronous=true;},'synchronous fetch conversion throw'],
    ['source-http-object','observed',state=>{state.rejection.identity=false;},'replacement of the thrown object'],
    ['source-http-object','observed',state=>{state.rejection.calls=2;},'duplicate input conversion'],
    ['source-http-object','observed',state=>{state.rejection.hints=['default'];},'wrong primitive hint'],
    ['source-http-object','observed',state=>{state.rejection.returnedPromise=false;},'missing rejected promise'],
    ['request-info-upgrade-url','observed',state=>{state.inputConversions=2;},'URL stringified twice'],
    ['request-info-upgrade-request','observed',state=>{state.initOrder.reverse();},'init read before RequestInfo conversion'],
    ['intrinsic-traps','observed',state=>{state.traps=1;},'application method used for native forwarding'],
    ['foreign-methods','foreign',state=>{state.foreign.accept.error={name:'TypeError',message:"Can't accept() WebSocket that was already used in a response."};},'released-state error preceding the origin check'],
])test('rejects '+label,()=>{
    const value=sample(kind);change(value,stage,mutate);assert.throws(()=>assertFetchProxyCase(value,'reference'));
});
for(const kind of ['source-http','source-http-request','source-http-object'])test(kind+' requires side-body EOF while the proxy is still open',()=>{
    const value=sample(kind),item=value.checkpointUpstream[1];item.peerEnded=false;item.peerReset=false;item.events=['request','held-http'];
    assert.throws(()=>assertFetchProxyCase(value,'reference'));
});
test('request identity cannot change between actual snapshots even with a valid new handshake',()=>{
    const value=sample('source-lifetime'),item=value.checkpointUpstream[0];
    const key='AAAAAAAAAAAAAAAAAAAAAA==';item.request=item.request.replace(/Sec-WebSocket-Key: [^\r\n]+/i,'Sec-WebSocket-Key: '+key);
    item.rawRequest=Buffer.from(item.request+'\r\n\r\n','latin1').toString('base64');
    item.responseHeader=item.responseHeader.replace(/Sec-WebSocket-Accept: [^\r\n]+/i,'Sec-WebSocket-Accept: '+createHash('sha1').update(key+'258EAFA5-E914-47DA-95CA-C5AB0DC85B11').digest('base64'));
    assert.throws(()=>assertFetchProxyCase(value,'reference'),/connection identity/);
});
for(const kind of ['large-binary','slow-upstream','upstream-first-binary'])test(kind+' checks literal complete bytes independently of summary hashes',()=>{
    const value=sample(kind),item=value.wire.frames.find(item=>item.opcode===2),data=Buffer.from(item.payload,'base64');
    data[90000]^=1;item.payload=data.toString('base64');assert.throws(()=>assertFetchProxyCase(value,'reference'));
});
for(const [label,mutate] of [
    ['same missing case',report=>{report.cases.pop();}],
    ['same duplicate case',report=>{report.cases[1]=structuredClone(report.cases[0]);}],
    ['same incorrect timer',report=>{change(report.cases.find(value=>value.kind==='source-lifetime'),'checkpoint',state=>{state.timerFired=1;});}],
    ['same wrong foreign error',report=>{change(report.cases.find(value=>value.kind==='foreign-methods'),'foreign',state=>{state.foreign.accept.error.name='TypeError';});}],
    ['same lost binary payload',report=>{report.cases.find(value=>value.kind==='slow-upstream').wire.frames.pop();}],
])test('reference and all products agreeing on '+label+' still fails',()=>{
    const m=matrix();for(const report of [m.reference,...m.candidates])mutate(report);
    assert.throws(()=>compareFetchProxyReports(m.reference,m.candidates,m.expected));
});
for(const [label,mutate] of [
    ['incomplete report',m=>{m.reference.complete=false;}],
    ['stale input',m=>{m.reference.inputs={...m.reference.inputs,fixture:'0'.repeat(64)};}],
    ['stale worker',m=>{m.candidates[0].binaries.worker='0'.repeat(64);}],
    ['different run',m=>{m.candidates[0].runId='00000000-0000-4000-8000-000000000001';}],
    ['omitted mode',m=>{m.candidates.pop();}],
    ['duplicate mode',m=>{m.candidates[1].mode=m.candidates[0].mode;}],
    ['reference as product',m=>{m.expected.host=m.expected.lock.binary_sha256;for(const report of m.candidates)report.binaries.host=m.expected.host;}],
])test('rejects '+label,()=>{const m=matrix();mutate(m);assert.throws(()=>compareFetchProxyReports(m.reference,m.candidates,m.expected));});
test('product CLI cannot select the reference role before it opens a report or starts a process',()=>{
    const destination=path.join(os.tmpdir(),'capsid-invalid-proxy-role-'+process.pid+'.json');assert.equal(fs.existsSync(destination),false);
    const result=spawnSync(process.execPath,[fileURLToPath(new URL('./test_websocket_fetch_proxy.mjs',import.meta.url)),
        '--mode','reference','--host','missing-host','--worker','missing-worker','--report',destination],{encoding:'utf8',timeout:5000});
    assert.equal(result.error,undefined);assert.notEqual(result.status,0);assert.match(result.stderr,/product cannot select the reference role/);assert.equal(fs.existsSync(destination),false);
});
