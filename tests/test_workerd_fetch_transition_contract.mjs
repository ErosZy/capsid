import assert from 'node:assert/strict';
import test from 'node:test';
import fs from 'node:fs';
import {createHash} from 'node:crypto';
import {readFetchTransitionSample} from './helpers/ws_fetch_transition_sample.mjs';
import {fetchTransitionCases,assertFetchTransitionCase,validateFetchTransitionReport,compareFetchTransitionReports} from '../tools/workerd-fetch-transition-contract.mjs';

const stored=readFetchTransitionSample(),sample=kind=>structuredClone(stored.cases.find(value=>value.kind===kind));
const state=(value,edit)=>{const document=JSON.parse(value.observed.text);edit(document.state,document);value.observed.text=JSON.stringify(document);};
const snapshots=(value,edit)=>{edit(value.upstreamBeforeStop);value.upstreamAfterStop=structuredClone(value.upstreamBeforeStop);};
const nativeReaders=fetchTransitionCases.filter(kind=>kind.startsWith('tls-body-reader-probe-'));
const network=fetchTransitionCases.slice(0,15),scopes=fetchTransitionCases.slice(15,20);
function matrix(platform='linux'){
    // Synthetic platform/backend copies exercise validators only. The sample
    // contains one real Linux workerd execution; actual gates run every mode.
    const reference=structuredClone(stored),lock=JSON.parse(fs.readFileSync(new URL(platform==='win32'?'./fixtures/workerd-ws/reference-lock-windows.json':'./fixtures/workerd-ws/reference-lock.json',import.meta.url)));
    reference.referenceLock=lock;reference.binaries={host:lock.binary_sha256,worker:null};
    const expected={runId:reference.runId,lock,inputs:reference.inputs,host:'a'.repeat(64),worker:'b'.repeat(64)};
    const candidates=['single-worker','static-pool',...(platform==='linux'?['managed']:[])].map(mode=>{
        const report=structuredClone(reference);report.mode=mode;report.binaries={host:expected.host,worker:expected.worker};
        for(const value of report.cases){value.shutdown={code:0,signal:null,forced:false};state(value,s=>{if(s.socket)s.socket.events=s.socket.events.filter(event=>event.type!=='error');});}return report;
    });return {reference,candidates,expected};
}
test('fixed 46-case inventory includes TLS, cancellation, creation scopes and four reader construction routes',()=>{
    assert.deepEqual(fetchTransitionCases,['tls-ipv6-upgrade','tls-ipv6-proxy','tls-ipv6-wrong-host','redirect-http-to-https',
        'redirect-https-to-http','redirect-https-to-https','redirect-https-denied','redirect-http-to-https-wrong-host',
        'tls-body-reader-cancel','tls-body-abort','tls-body-abort-before-read','tls-upgrade-abort-pending','tls-handshake-abort','redirect-held-abort','tls-body-context-end',
        'http-created-promise','event-created-promise','http-created-timer','http-created-promise-after-close','http-created-timer-after-close',
        'tls-body-reader-probe-object','tls-body-reader-probe-default','tls-body-reader-probe-string','tls-body-reader-probe-error','tls-body-reader-probe-null',
        'tls-body-reader-probe-no-pending','tls-body-reader-probe-before-first','tls-body-reader-probe-eof','standard-reader-probe','tls-body-reader-probe-pending-two',
        'tls-body-reader-probe-direct-object','tls-body-reader-probe-direct-before-first','tls-body-reader-probe-direct-pending-two','standard-reader-probe-direct-pending-two',
        'tls-body-reader-probe-prototype-object','tls-body-reader-probe-prototype-before-first','tls-body-reader-probe-prototype-pending-two','standard-reader-probe-prototype-pending-two',
        'tls-body-reader-probe-subclass-object','tls-body-reader-probe-subclass-before-first','tls-body-reader-probe-subclass-pending-two','standard-reader-probe-subclass-pending-two',
        'tls-body-reader-probe-reflect-object','tls-body-reader-probe-reflect-before-first','tls-body-reader-probe-reflect-pending-two','standard-reader-probe-reflect-pending-two']);assert.ok(Object.isFrozen(fetchTransitionCases));
});
for(const platform of ['linux','win32'])test(platform+' literal validator accepts the real reference replay and synthetic healthy matrix',()=>{
    const m=matrix(platform);assert.equal(validateFetchTransitionReport(m.reference,m.expected).length,46);
    assert.deepEqual(compareFetchTransitionReports(m.reference,m.candidates,m.expected),{complete:true,casesPerBackend:46,backends:platform==='linux'?3:2});
});
for(const kind of fetchTransitionCases)for(const [label,mutate] of [
    ['fixture failure',v=>{v.failure='injected';}],['cleanup failure',v=>{v.cleanupFailure='injected';}],
    ['missing terminal state',v=>{delete v.observed;}],['changed VM',v=>state(v,(_s,d)=>{d.realm='00000000-0000-4000-8000-000000000001';})],
    ['wrong challenge',v=>state(v,s=>{s.challenge='wrong';})],['late source termination',v=>{v.allSourcesEndedBeforeStop=false;}],
    ['forced shutdown',v=>{v.shutdown.forced=true;}],['abnormal exit',v=>{v.shutdown={code:1,signal:null,forced:false};}],
    ['sanitizer finding',v=>{v.stderr+='AddressSanitizer: invalid access';}],['runtime poison',v=>{v.stderr+='WORKER POISONED';}],
    ['incorrect permission intersection',v=>{v.allowedAuthorities.push('127.0.0.3:12345');}],['different target authority',v=>{v.target=v.target.replace('://','://other.');}],
    ['missing client Close',v=>{v.wire.frames.pop();}],['altered raw client bytes',v=>{v.wire.rawTail='AA==';}],
    ['substituted incoming nonce',v=>{v.headers.requestKey='AAAAAAAAAAAAAAAAAAAAAA==';}],
    ['invented TCP connection',v=>snapshots(v,s=>{s.tcpRecords.push({role:'source'});})],
    ['observations changing only at shutdown',v=>{v.upstreamAfterStop.records.push({request:'invented'});}],
    ['unexpected foreign resolver request',v=>{v.foreign={status:200,text:'other'};}],
])test(kind+' rejects '+label,()=>{const v=sample(kind);mutate(v);assert.throws(()=>assertFetchTransitionCase(v,'reference'));});

for(const kind of fetchTransitionCases.filter(kind=>stored.cases.find(c=>c.kind===kind).upstreamBeforeStop.records.some(r=>r.request!==null)))for(const [label,mutate] of [
    ['missing raw request',r=>{r.rawRequest='';}],['wrong request method',r=>{r.request=r.request.replace('GET ','POST ');}],
    ['missing received chunks',r=>{r.receivedChunks=[];}],['invented sent bytes',r=>{r.sentChunks=['AA=='];}],
    ['false response status',r=>{r.responseStatus=500;}],['fixture destruction reported as peer completion',r=>{r.fixtureDestroyed=true;}],
    ['source held until fixture stop',r=>{r.peerEnded=false;r.peerReset=false;}],['source peer error',r=>{r.error={code:'OTHER',message:'injected'};}],
])test(kind+' rejects '+label,()=>{const v=sample(kind);snapshots(v,s=>mutate(s.records.find(r=>r.request!==null)));assert.throws(()=>assertFetchTransitionCase(v,'reference'));});
for(const kind of ['tls-ipv6-upgrade','tls-ipv6-proxy','redirect-http-to-https','redirect-https-to-http','redirect-https-to-https'])for(const [label,mutate] of [
    ['missing source echo',v=>snapshots(v,s=>{s.records.at(-1).frames.shift();})],
    ['source frame without masking',v=>snapshots(v,s=>{s.records.at(-1).frames[0].masked=false;})],
    ['missing duplicate Set-Cookie',v=>state(v,s=>{s.result.cookies.pop();})],['wrong final URL',v=>state(v,s=>{s.result.url+='extra';})],
    ['ordinary body invented for upgrade',v=>state(v,s=>{s.result.bodyNull=false;})],
])test(kind+' rejects '+label,()=>{const v=sample(kind);mutate(v);assert.throws(()=>assertFetchTransitionCase(v,'reference'));});
for(const kind of ['tls-ipv6-upgrade','tls-ipv6-proxy','tls-ipv6-wrong-host','redirect-http-to-https','redirect-https-to-http','redirect-https-to-https',...nativeReaders])test(kind+' rejects omitted SNI',()=>{
    const v=sample(kind);snapshots(v,s=>{s.sni=[];});assert.throws(()=>assertFetchTransitionCase(v,'reference'));
});
for(const kind of ['tls-ipv6-wrong-host','redirect-http-to-https-wrong-host','redirect-https-denied'])for(const [label,mutate] of [
    ['missing real TCP attempt',v=>snapshots(v,s=>{s.tcpRecords=[];s.records=[];})],
    ['unredacted public error',v=>state(v,s=>{s.failure.message='certificate mismatch';})],
    ['invented diagnostic identifier',v=>{state(v,s=>{s.failure.message='internal error; reference = '+'a'.repeat(24);});v.stderr='different';}],
])test(kind+' rejects '+label,()=>{const v=sample(kind);mutate(v);assert.throws(()=>assertFetchTransitionCase(v,'reference'));});

for(const kind of nativeReaders)for(const [label,edit] of [
    ['rejected closed',s=>{s.closed={error:{type:'object'}};}],['rejected cancel',s=>{s.cancel={error:{type:'object'}};}],
    ['non-undefined cancel return',s=>{s.cancel.returnedUndefined=false;}],['body not disturbed',s=>{s.bodyUsed=false;}],
    ['lock lost before release',s=>{s.locked=false;}],['lock retained after release',s=>{s.lockedAfterRelease=true;}],
    ['post-cancel read rejected',s=>{s.after={error:{type:'object'}};}],['repeated cancellation rejected',s=>{s.again={error:{type:'object'}};}],
    ['incorrect reaction order',s=>{s.trace.reverse();}],
])test(kind+' rejects '+label,()=>{const v=sample(kind);state(v,edit);assert.throws(()=>assertFetchTransitionCase(v,'reference'));});
for(const kind of nativeReaders.filter(k=>!['-no-pending','-before-first','-eof'].some(s=>k.endsWith(s))))for(const [label,edit] of [
    ['pending read fulfills done',s=>{s.pending={value:{done:true,bytes:0}};}],
    ['cancel reason changed to undefined',s=>{s.pending.error={type:'undefined',name:null,message:null,primitive:'undefined',sameReason:false};}],
    ['original reason object reused',s=>{s.pending.error.sameReason=true;}],
    ['read rejection observed before cancel fulfillment',s=>{s.trace=['read-error','closed','cancel'];}],
])test(kind+' rejects '+label,()=>{const v=sample(kind);state(v,edit);assert.throws(()=>assertFetchTransitionCase(v,'reference'));});
for(const kind of network.filter(k=>k.includes('abort')||k==='tls-body-reader-cancel'))test(kind+' rejects undefined serialized cancellation reason',()=>{
    const v=sample(kind);state(v,s=>{s.failure={type:'undefined',name:null,message:null,primitive:'undefined'};});assert.throws(()=>assertFetchTransitionCase(v,'reference'));
});
for(const kind of scopes)for(const field of ['timerFired','resolverCalled','future','parentClosed'])test(kind+' rejects wrong '+field,()=>{
    const v=sample(kind);state(v,s=>{s[field]=!s[field];});assert.throws(()=>assertFetchTransitionCase(v,'reference'));
});
for(const kind of ['tls-body-reader-cancel','tls-body-abort','tls-body-abort-before-read','tls-upgrade-abort-pending','tls-handshake-abort','redirect-held-abort','tls-body-context-end','http-created-promise-after-close','http-created-timer-after-close'])for(const [label,mutate] of [
    ['missing pending checkpoint',v=>{v.checkpoint=null;}],['changed pending VM',v=>{const d=JSON.parse(v.checkpoint.text);d.realm='00000000-0000-4000-8000-000000000001';v.checkpoint.text=JSON.stringify(d);}],
    ['callback ran before external cancellation',v=>{const d=JSON.parse(v.checkpoint.text);d.state.future=true;v.checkpoint.text=JSON.stringify(d);}],
    ['invented pending TCP',v=>{v.checkpointUpstream.tcpRecords.push({role:'source'});}],
])test(kind+' rejects '+label,()=>{const v=sample(kind);mutate(v);assert.throws(()=>assertFetchTransitionCase(v,'reference'));});

for(const [kind,label,mutate] of [
    ['standard-reader-probe','native cancellation applied to standard stream',v=>state(v,s=>{s.pending={error:{type:'object',name:null,message:null,primitive:null,sameReason:false}};s.trace=['closed','cancel','read-error'];})],
    ['standard-reader-probe','closed observed before pending standard read',v=>state(v,s=>{s.trace=['closed','read','cancel'];})],
    ['tls-body-reader-probe-pending-two','second pending read accepted',v=>state(v,s=>{s.second={value:{done:true,bytes:0}};})],
    ['tls-body-reader-probe-pending-two','generic second-read error',v=>state(v,s=>{s.second.error.message='read pending';})],
    ['tls-body-reader-probe-default','wrong default cancellation message',v=>state(v,s=>{s.pending.error.message='Cancelled';})],
    ['tls-body-reader-probe-null','null cancellation reason preserved as primitive',v=>state(v,s=>{s.pending.error={type:'object',name:null,message:null,primitive:'null',sameReason:true};})],
    ['tls-body-reader-probe-before-first','first read invented',v=>state(v,s=>{s.first={done:false,bytes:5,sha256:'a'.repeat(64)};})],
    ['tls-body-reader-probe-eof','finite response not completed by fixture',v=>snapshots(v,s=>{s.records[0].fixtureEnded=false;})],
    ['tls-body-reader-cancel','lost valid first body bytes',v=>state(v,s=>{s.firstBody.bytes=0;})],
    ['tls-body-context-end','continuation runs after final owner Close',v=>state(v,s=>{s.future=true;s.trace.push('complete');})],
    ['http-created-promise-after-close','foreign resolver omitted',v=>{v.foreign=null;}],
    ['redirect-held-abort','target dialed before held body ends',v=>snapshots(v,s=>{s.tcpRecords.push({...s.tcpRecords[0],role:'target',localPort:v.targetPort});})],
    ['tls-upgrade-abort-pending','server headers sent before abort',v=>snapshots(v,s=>{s.records[0].sentChunks=[Buffer.from('HTTP/1.1 101 Switching Protocols\r\n\r\n').toString('base64')];})],
    ['tls-handshake-abort','no real ClientHello',v=>snapshots(v,s=>{s.records[0].receivedChunks=[];})],
    ['tls-handshake-abort','invented raw HTTP as TLS phase',v=>snapshots(v,s=>{s.records[0].receivedChunks=[Buffer.from('GET / HTTP/1.1\r\n\r\n').toString('base64')];})],
    ['tls-handshake-abort','raw ClientHello SNI altered',v=>snapshots(v,s=>{const r=s.records[0],b=Buffer.concat(r.receivedChunks.map(c=>Buffer.from(c,'base64'))),at=b.indexOf('localhost');assert.ok(at>=0);b[at]=120;r.receivedChunks=[b.toString('base64')];})],
    ['redirect-http-to-https','redirect nonce reused',v=>snapshots(v,s=>{const a=s.records[0],b=s.records[1],key=r=>/Sec-WebSocket-Key: ([^\r]+)/i.exec(r.request)[1];b.request=b.request.replace(key(b),key(a));})],
    ['redirect-http-to-https','Authorization retained across origins',v=>snapshots(v,s=>{s.records[1].request+='\r\nAuthorization: auth/'+v.challenge;})],
])test('rejects '+label,()=>{const v=sample(kind);mutate(v);assert.throws(()=>assertFetchTransitionCase(v,'reference'));});
for(const [label,mutate] of [
    ['stale worker binary',m=>{m.candidates[0].binaries.worker='0'.repeat(64);}],['stale fixture source',m=>{m.reference.inputs.fixture='0'.repeat(64);}],
    ['invalid expected input digest',m=>{m.expected.inputs.contract='invalid';}],['missing required backend',m=>{m.candidates.pop();}],
    ['reference alias as candidate',m=>{m.candidates[0].mode='reference';}],['incomplete report',m=>{m.reference.complete=false;}],['changed run identifier',m=>{m.candidates[0].runId='other';}],
    ['same omitted case everywhere',m=>{for(const r of [m.reference,...m.candidates])r.cases.pop();}],
    ['same incorrect reader result everywhere',m=>{for(const r of [m.reference,...m.candidates])state(r.cases.find(c=>c.kind==='tls-body-reader-probe-object'),s=>{s.pending={value:{done:true,bytes:0}};});}],
    ['same late scope continuation everywhere',m=>{for(const r of [m.reference,...m.candidates])state(r.cases.find(c=>c.kind==='http-created-promise-after-close'),s=>{s.future=true;});}],
    ['same repeated challenge everywhere',m=>{for(const r of [m.reference,...m.candidates])r.cases[1].challenge=r.cases[0].challenge;}],
    ['wrong certificate digest',m=>{m.reference.certificateInputs.ipv6ServerSHA256='a'.repeat(64);}],['wrong IPv6 SAN',m=>{m.reference.certificateInputs.ipv6ServerSAN='IP Address:127.0.0.1';}],
    ['IPv6 certificate actually IPv4-only',m=>{const c=m.reference.certificateInputs;c.ipv6ServerPEM=c.ipServerPEM;c.ipv6ServerSHA256=createHash('sha256').update(c.ipv6ServerPEM).digest('hex');}],
])test('complete matrix rejects '+label,()=>{const m=matrix();mutate(m);assert.throws(()=>compareFetchTransitionReports(m.reference,m.candidates,m.expected));});
test('stored capture reads stay independent across test mutations',()=>{const a=readFetchTransitionSample();a.cases.pop();const b=readFetchTransitionSample();assert.equal(b.cases.length,46);});

for(const form of ['direct','prototype','subclass','reflect']){
    const kinds=[`tls-body-reader-probe-${form}-object`,`tls-body-reader-probe-${form}-before-first`,
        `tls-body-reader-probe-${form}-pending-two`,`standard-reader-probe-${form}-pending-two`];
    for(const kind of kinds)for(const field of ['instanceOfBase','expectedPrototype','expectedConstructor','prototypeConstructorIsBase'])
        test(kind+' rejects incorrect '+field,()=>{const v=sample(kind);state(v,s=>{s.construction[field]=false;});assert.throws(()=>assertFetchTransitionCase(v,'reference'));});
    for(const kind of kinds)test(kind+' rejects another construction route',()=>{const v=sample(kind);state(v,s=>{s.construction.mode='getReader';});assert.throws(()=>assertFetchTransitionCase(v,'reference'));});
    for(const field of ['pending','second'])test(form+' standard stream retains queued '+field,()=>{
        const v=sample(`standard-reader-probe-${form}-pending-two`);state(v,s=>{s[field]={error:{type:'object',name:'TypeError',message:'native-only error',primitive:null,sameReason:false}};});assert.throws(()=>assertFetchTransitionCase(v,'reference'));
    });
    test(form+' native stream rejects a second concurrent read',()=>{
        const v=sample(`tls-body-reader-probe-${form}-pending-two`);state(v,s=>{s.second={value:{done:true,bytes:0}};});assert.throws(()=>assertFetchTransitionCase(v,'reference'));
    });
}
