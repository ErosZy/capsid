import assert from 'node:assert/strict';
import test from 'node:test';
import fs from 'node:fs';
import {createHash} from 'node:crypto';
import {readFetchBoundarySample} from './helpers/ws_fetch_boundary_sample.mjs';
import {fetchBoundaryCases,assertFetchBoundaryCase,validateFetchBoundaryReport,compareFetchBoundaryReports} from '../tools/workerd-fetch-boundary-contract.mjs';

const stored=readFetchBoundarySample(),sample=kind=>structuredClone(stored.cases.find(value=>value.kind===kind));
const state=(value,edit)=>{const document=JSON.parse(value.observed.text);edit(document.state,document);value.observed.text=JSON.stringify(document);};
const snapshots=(value,edit)=>{edit(value.upstreamBeforeStop);value.upstreamAfterStop=structuredClone(value.upstreamBeforeStop);};
function matrix(platform='linux'){
    // These are synthetic validator profiles. The stored source is one real
    // Linux reference execution; no copy qualifies another runtime or platform.
    const reference=structuredClone(stored),lock=JSON.parse(fs.readFileSync(new URL(platform==='win32'?'./fixtures/workerd-ws/reference-lock-windows.json':'./fixtures/workerd-ws/reference-lock.json',import.meta.url)));
    reference.referenceLock=lock;reference.binaries={host:lock.binary_sha256,worker:null};
    const expected={runId:reference.runId,lock,inputs:reference.inputs,host:'a'.repeat(64),worker:'b'.repeat(64)};
    const candidates=['single-worker','static-pool',...(platform==='linux'?['managed']:[])].map(mode=>{
        const report=structuredClone(reference);report.mode=mode;report.binaries={host:expected.host,worker:expected.worker};
        for(const value of report.cases){value.shutdown={code:0,signal:null,forced:false};state(value,s=>{if(s.socket)s.socket.events=s.socket.events.filter(event=>event.type!=='error');});}
        return report;
    });return {reference,candidates,expected};
}
test('fixed 22-case inventory covers TLS trust/SNI, actual IPv6, redirects, body errors and conversion',()=>{
    assert.deepEqual(fetchBoundaryCases,['tls-upgrade','tls-proxy','tls-ip-upgrade','tls-wrong-host','tls-untrusted','tls-http',
        'ipv6-upgrade','redirect-error','redirect-loop','redirect-cross','redirect-denied','redirect-held-body',
        'body-empty','body-eof','body-truncated','body-invalid-chunk','body-truncated-coalesced','body-invalid-chunk-coalesced',
        'body-204','body-205','body-304','request-redirect-values']);assert.ok(Object.isFrozen(fetchBoundaryCases));
});
for(const platform of ['linux','win32'])test(platform+' literal validator accepts native reference replay and synthetic healthy candidates',()=>{
    const m=matrix(platform);assert.equal(validateFetchBoundaryReport(m.reference,m.expected).length,22);
    assert.deepEqual(compareFetchBoundaryReports(m.reference,m.candidates,m.expected),{complete:true,casesPerBackend:22,backends:platform==='linux'?3:2});
});
for(const kind of fetchBoundaryCases)for(const [label,mutate] of [
    ['fixture failure',v=>{v.failure='fixture failure';}],
    ['cleanup failure',v=>{v.cleanupFailure='cleanup failure';}],
    ['missing final state',v=>{delete v.observed;}],
    ['changed VM',v=>state(v,(_s,doc)=>{doc.realm='00000000-0000-4000-8000-000000000001';})],
    ['wrong challenge',v=>state(v,s=>{s.challenge='wrong';})],
    ['late source termination',v=>{v.allSourcesEndedBeforeStop=false;}],
    ['forced shutdown',v=>{v.shutdown.forced=true;}],
    ['abnormal service exit',v=>{v.shutdown={code:1,signal:null,forced:false};}],
    ['sanitizer finding',v=>{v.stderr+='AddressSanitizer: invalid access';}],
    ['runtime poison',v=>{v.stderr+='WORKER POISONED';}],
    ['incorrect allowlist',v=>{v.allowedAuthorities.push('127.0.0.3:12345');}],
    ['different target authority',v=>{v.target=v.target.replace('://','://other.');}],
    ['missing final client Close',v=>{v.wire.frames.pop();}],
    ['altered raw downstream bytes',v=>{v.wire.rawTail='AA==';}],
    ['substituted incoming nonce',v=>{v.headers.requestKey='AAAAAAAAAAAAAAAAAAAAAA==';}],
    ['extra source TCP',v=>snapshots(v,s=>{s.tcpRecords.push({role:'source'});})],
    ['observations changing after shutdown',v=>{v.upstreamAfterStop.records.push({request:'invented'});}],
])test(kind+' rejects '+label,()=>{const value=sample(kind);mutate(value);assert.throws(()=>assertFetchBoundaryCase(value,'reference'));});

for(const kind of fetchBoundaryCases.filter(kind=>!['tls-wrong-host','tls-untrusted','redirect-error','request-redirect-values'].includes(kind)))for(const [label,mutate] of [
    ['missing raw request',item=>{item.rawRequest='';}],
    ['wrong request method',item=>{item.request=item.request.replace('GET ','POST ');}],
    ['missing received chunks',item=>{item.receivedChunks=[];}],
    ['missing sent chunks',item=>{item.sentChunks=[];}],
    ['wrong raw response status',item=>{item.responseHeader=item.responseHeader.replace('HTTP/1.1 ','HTTP/1.0 ');}],
    ['fixture destruction masquerading as source EOF',item=>{item.fixtureDestroyed=true;}],
    ['live source held until service stop',item=>{item.peerEnded=false;item.peerReset=false;}],
    ['source peer error',item=>{item.error={code:'OTHER',message:'injected'};}],
])test(kind+' rejects '+label,()=>{
    const value=sample(kind);snapshots(value,s=>mutate(s.records.find(row=>row.request!==null)));assert.throws(()=>assertFetchBoundaryCase(value,'reference'));
});
for(const kind of ['tls-upgrade','tls-proxy','tls-ip-upgrade','ipv6-upgrade','redirect-cross','redirect-held-body'])for(const [label,mutate] of [
    ['missing echoed source data',v=>snapshots(v,s=>{s.records.at(-1).frames.shift();})],
    ['source frame without masking',v=>snapshots(v,s=>{s.records.at(-1).frames[0].masked=false;})],
    ['missing duplicate Set-Cookie',v=>state(v,s=>{s.result.cookies.pop();})],
    ['wrong final response URL',v=>state(v,s=>{s.result.url+='extra';})],
    ['invented ordinary HTTP body',v=>state(v,s=>{s.result.bodyNull=false;})],
])test(kind+' rejects '+label,()=>{const value=sample(kind);mutate(value);assert.throws(()=>assertFetchBoundaryCase(value,'reference'));});
for(const kind of ['tls-upgrade','tls-proxy','tls-ip-upgrade','tls-wrong-host','tls-untrusted','tls-http'])test(kind+' rejects omitted SNI',()=>{
    const value=sample(kind);snapshots(value,s=>{s.sni=[];});assert.throws(()=>assertFetchBoundaryCase(value,'reference'));
});
for(const kind of ['tls-wrong-host','tls-untrusted'])for(const [label,mutate] of [
    ['no real TLS connection',v=>snapshots(v,s=>{s.tcpRecords=[];s.records=[];})],
    ['HTTP sent before certificate rejection',v=>snapshots(v,s=>{s.records[0].request='GET / HTTP/1.1';})],
    ['unredacted error',v=>state(v,s=>{s.failure.message='certificate failed';})],
    ['invented error reference',v=>{state(v,s=>{s.failure.message='internal error; reference = '+'a'.repeat(24);});v.stderr='different error';}],
])test(kind+' rejects '+label,()=>{const value=sample(kind);mutate(value);assert.throws(()=>assertFetchBoundaryCase(value,'reference'));});
for(const kind of ['body-truncated','body-invalid-chunk','body-truncated-coalesced','body-invalid-chunk-coalesced'])for(const [label,mutate] of [
    ['dropped valid prefix',v=>state(v,s=>{s.firstBody.bytes=0;})],
    ['wrong prefix bytes',v=>state(v,s=>{s.firstBody.sha256='0'.repeat(64);})],
    ['incorrect public failure category',v=>state(v,s=>{s.failure.name='TypeError';})],
    ['premature completion',v=>state(v,s=>{s.failure=null;})],
    ['wrong disturbance result',v=>state(v,s=>{s.result.bodyUsed=true;})],
])test(kind+' rejects '+label,()=>{const value=sample(kind);mutate(value);assert.throws(()=>assertFetchBoundaryCase(value,'reference'));});
for(const kind of ['body-204','body-205','body-304'])for(const field of ['bodyNull','bodyUsed'])test(kind+' rejects wrong '+field,()=>{
    const value=sample(kind);state(value,s=>{s.result[field]=!s.result[field];});assert.throws(()=>assertFetchBoundaryCase(value,'reference'));
});
for(const kind of ['body-truncated','body-invalid-chunk','redirect-held-body'])for(const [label,mutate] of [
    ['no intermediate state',v=>{v.checkpoint=null;}],
    ['fixture completed before release',v=>{v.checkpointUpstream.records[0].fixtureEnded=true;}],
    ['source ended before release',v=>{v.checkpointUpstream.records[0].peerEnded=true;}],
    ['invented unrelated partial bytes',v=>{v.checkpointUpstream.records[0].sentChunks=['AA=='];}],
    ['omitted pending bytes',v=>{v.checkpointUpstream.records[0].sentChunks=[];}],
])test(kind+' rejects '+label,()=>{const value=sample(kind);mutate(value);assert.throws(()=>assertFetchBoundaryCase(value,'reference'));});
for(const [kind,label,mutate] of [
    ['redirect-held-body','following before held redirect body completes',v=>{v.followedBeforeRelease=true;}],
    ['redirect-denied','TCP attempt to forbidden redirect target',v=>snapshots(v,s=>{s.tcpRecords.push({...s.tcpRecords[0],role:'target',localAddress:'127.0.0.2',localPort:v.targetPort});})],
    ['redirect-loop','more or fewer than 21 actual requests',v=>snapshots(v,s=>{s.records.pop();s.tcpRecords.pop();})],
    ['redirect-loop','wrong redirect URL list',v=>state(v,s=>{s.failure.message='Too many redirects';})],
    ['redirect-error','network attempted for invalid redirect enum',v=>snapshots(v,s=>{s.tcpRecords.push({role:'source'});})],
    ['redirect-error','Request constructor accepts error enum',v=>state(v,s=>{s.requestProbe={returned:true};})],
    ['request-redirect-values','Response rejects an unrelated redirect field',v=>state(v,s=>{s.responses[0].status=null;s.responses[0].error={name:'TypeError',message:'invalid redirect'};})],
    ['request-redirect-values','Response coerces an unused object',v=>state(v,s=>{s.responses.find(x=>x.label==='object').conversions=1;})],
    ['request-redirect-values','Response.json coerces an unused object',v=>state(v,s=>{s.responses.find(x=>x.method==='json'&&x.label==='object').conversions=1;})],
    ['request-redirect-values','Response invokes an unrelated getter',v=>state(v,s=>{s.responses.find(x=>x.method==='constructor'&&x.label==='getter').getterCalls=1;})],
    ['request-redirect-values','Response.json invokes an unrelated getter',v=>state(v,s=>{s.responses.find(x=>x.method==='json'&&x.label==='getter').getterCalls=1;})],
    ['request-redirect-values','missing constructor values',v=>state(v,s=>{s.constructors.pop();})],
    ['request-redirect-values','incorrect Symbol error',v=>state(v,s=>{s.constructors.find(x=>x.label==='symbol').error.message='cannot convert symbol to string';})],
    ['request-redirect-values','duplicate string conversion',v=>state(v,s=>{s.constructors.find(x=>x.label==='object-manual').conversions=2;})],
    ['request-redirect-values','wrong conversion hint',v=>state(v,s=>{s.constructors.find(x=>x.label==='object-error').hints=['default'];})],
    ['request-redirect-values','lost thrown object identity',v=>state(v,s=>{s.constructors.find(x=>x.label==='throwing-object').sameThrown=false;})],
    ['request-redirect-values','synchronous fetch throw',v=>state(v,s=>{s.fetches[0].synchronous=true;})],
    ['request-redirect-values','invented Promise',v=>state(v,s=>{s.fetches[0].returnedPromise=false;})],
    ['request-redirect-values','lost getter exception identity',v=>state(v,s=>{s.fetches.at(-1).sameThrown=false;})],
])test('rejects '+label,()=>{const value=sample(kind);mutate(value);assert.throws(()=>assertFetchBoundaryCase(value,'reference'));});

for(const [label,mutate] of [
    ['stale binary',m=>{m.candidates[0].binaries.worker='0'.repeat(64);}],
    ['stale input',m=>{m.reference.inputs.fixture='0'.repeat(64);}],
    ['invalid expected input',m=>{m.expected.inputs.contract='invalid';}],
    ['missing required backend',m=>{m.candidates.pop();}],
    ['reference backend alias',m=>{m.candidates[0].mode='reference';}],
    ['incomplete report',m=>{m.reference.complete=false;}],
    ['different run identity',m=>{m.candidates[0].runId='other';}],
    ['same omitted case everywhere',m=>{for(const r of [m.reference,...m.candidates])r.cases.pop();}],
    ['same wrong public error everywhere',m=>{for(const r of [m.reference,...m.candidates])state(r.cases.find(c=>c.kind==='body-truncated'),s=>{s.failure.message='HTTP response body failed';});}],
    ['same repeated challenge everywhere',m=>{for(const r of [m.reference,...m.candidates])r.cases[1].challenge=r.cases[0].challenge;}],
    ['wrong certificate digest',m=>{m.reference.certificateInputs.serverSHA256='a'.repeat(64);}],
    ['wrong IP SAN',m=>{m.reference.certificateInputs.ipServerSAN='DNS:localhost';}],
    ['untrusted certificate actually trusted',m=>{
        const c=m.reference.certificateInputs;c.untrustedPEM=c.serverPEM;c.untrustedSHA256=createHash('sha256').update(c.untrustedPEM).digest('hex');
    }],
])test('complete matrix rejects '+label,()=>{const m=matrix();mutate(m);assert.throws(()=>compareFetchBoundaryReports(m.reference,m.candidates,m.expected));});
test('non-UUID run identifiers stay exact and bounded',()=>{
    const m=matrix();for(const r of [m.reference,...m.candidates])r.runId='fetch-boundary-review-20261020';m.expected.runId='fetch-boundary-review-20261020';
    assert.equal(compareFetchBoundaryReports(m.reference,m.candidates,m.expected).complete,true);m.reference.runId='';assert.throws(()=>validateFetchBoundaryReport(m.reference,m.expected));
});
test('stored capture reads are independent and immutable across test mutations',()=>{
    const a=readFetchBoundarySample();a.cases.pop();a.certificateInputs.serverSHA256='invalid';
    const b=readFetchBoundarySample();assert.equal(b.cases.length,22);assert.match(b.certificateInputs.serverSHA256,/^[a-f0-9]{64}$/);
});
