import assert from 'node:assert/strict';
import fs from 'node:fs';
import {createHash} from 'node:crypto';
import {nativeFetchGzipMemberPlans as plans,nativeFetchGzipMemberReply} from './native-fetch-gzip-member-input.mjs';

const hash=b=>createHash('sha256').update(b).digest('hex');
function fields(value,names){assert.ok(value&&typeof value==='object'&&!Array.isArray(value));assert.deepEqual(Object.keys(value).sort(),names.slice().sort());}
export function expectedNativeFetchGzipMemberState(plan){
    const reply=nativeFetchGzipMemberReply(plan),lengths=[...plan.pieces.filter(n=>n>0),0];
    const reads=lengths.map((length,index)=>{const done=index===lengths.length-1,offset=done?0:8,backing=Buffer.alloc(plan.view+16,165);backing.fill(0,offset,offset+length);
        return {done,inputAfterIssue:0,inputAfter:0,type:'Uint8Array',offset,length,capacity:plan.view+16,dataHash:hash(Buffer.alloc(length)),backingHash:hash(backing)};});
    return {failure:null,reads,total:plan.bytes,headers:[['connection','close'],['content-encoding','gzip'],['content-length',String(reply.payload.length)],['content-type','application/octet-stream'],['x-decoded-bytes',String(plan.bytes)]],
        expectedBytes:plan.bytes,closedStable:true,closed:{fulfilled:true},after:{bodyUsed:true,locked:false}};
}
function sourceModel(source,row,plan,primary=false){
    fields(source,['request','rawRequest','peerEnded','peerReset','fixtureEnded','fixtureDestroyed','events',...(primary?['packetSHA256']:['sent'])]);
    const reply=nativeFetchGzipMemberReply(plan),request='GET /hold HTTP/1.1\r\nHost: localhost:'+row.port+'\r\n\r\n';
    assert.ok(Number.isSafeInteger(row.port)&&row.port>0&&row.port<65536);
    assert.equal(source.request,request.slice(0,-4));assert.equal(source.rawRequest,Buffer.from(request).toString('base64'));
    assert.equal(source.peerEnded,true);assert.equal(source.peerReset,false);assert.equal(source.fixtureEnded,true);assert.equal(source.fixtureDestroyed,false);
    assert.deepEqual(source.events,['request','sent-complete','peer-ended']);
    if(primary)assert.equal(source.packetSHA256,hash(reply.packet));else assert.equal(source.sent,reply.packet.toString('base64'));
}
export function assertNativeFetchGzipMemberReferenceArchive(archive,fixtureSHA256){
    fields(archive,['schema','reports']);assert.equal(archive.schema,'capsid-native-fetch-gzip-member-reference-v1');assert.equal(archive.reports.length,2);
    for(const [index,item] of archive.reports.entries()){
        fields(item,['platform','originalTextUTF8','originalReportSHA256']);assert.equal(item.platform,index===0?'native':'linux');assert.equal(hash(Buffer.from(item.originalTextUTF8)),item.originalReportSHA256);
        const report=JSON.parse(item.originalTextUTF8);assert.equal(report.complete,true);assert.equal(report.cases.length,8);assert.equal(report.fixtureSHA256,fixtureSHA256);
        assert.equal(report.referenceLock.source_commit,'8f311ae3e3cf3064983f56a6a1c8b7ee33d6b99f');assert.equal(report.referenceLock.release_tag,'v1.20261010.1');
        assert.equal(report.referenceLock.compatibility_date,'2026-08-20');assert.deepEqual(report.referenceLock.compatibility_flags,[]);
        assert.equal(report.binarySHA256,index===0?'c8799724c4221af61dde76ed071aa5996b25c9bd5d247da94c166718cabe612a':'50f31156a9a4c7d549f1b4753dd61eb527e1b6b0cd416e5ba3d02d64dfdee5dd');
        assert.deepEqual(report.client,{node:'v24.14.0',undici:'7.21.0'});assert.equal(report.stderr,'');assert.ok(report.shutdown.signal==='SIGTERM'||(report.shutdown.code===0&&report.shutdown.signal===null));
        for(const [i,row] of report.cases.entries()){
            const plan=plans[i],reply=nativeFetchGzipMemberReply(plan);assert.deepEqual(row.plan,plan);assert.deepEqual(row.state,expectedNativeFetchGzipMemberState(plan));
            assert.equal(row.encodedBytes,reply.payload.length);assert.equal(row.payloadSHA256,hash(reply.payload));assert.equal(row.packetSHA256,hash(reply.packet));
            sourceModel(row.sourceBeforeReferenceStop,row,plan,true);assert.equal(row.sourceEndedBeforeReferenceStop,true);
        }
    }
}
const archive=JSON.parse(fs.readFileSync(new URL('../tests/fixtures/workerd-native-fetch-gzip-member-report.json',import.meta.url),'utf8'));
assertNativeFetchGzipMemberReferenceArchive(archive,hash(fs.readFileSync(new URL('../tests/fixtures/native-fetch-gzip-members.mjs',import.meta.url))));
export function assertNativeFetchGzipMemberCase(row,plan){
    fields(row,['plan','encodedBytes','payloadSHA256','packetSHA256','source','port','state','sourceEndedBeforeFinish','sourceBeforeFinish','shutdown','stdout','stderr','workerExit']);
    assert.deepEqual(row.plan,plan);assert.deepEqual(row.state,expectedNativeFetchGzipMemberState(plan));
    const reply=nativeFetchGzipMemberReply(plan);assert.equal(row.encodedBytes,reply.payload.length);assert.equal(row.payloadSHA256,hash(reply.payload));assert.equal(row.packetSHA256,hash(reply.packet));
    assert.deepEqual(row.sourceBeforeFinish,row.source);sourceModel(row.sourceBeforeFinish,row,plan);assert.equal(row.sourceEndedBeforeFinish,true);
    assert.deepEqual(row.workerExit,{clean:true,processObserved:true,exitStatus:0,forced:false,errorEvents:0});assert.deepEqual(row.shutdown,{code:0,signal:null});assert.equal(row.stderr,'');
    const lines=row.stdout.trim().split(/\r?\n/);assert.deepEqual(lines.filter(s=>s.startsWith('LIMIT_PROBE ')),['LIMIT_PROBE '+JSON.stringify(row.state)]);assert.deepEqual(lines.filter(s=>s.startsWith('LIMIT_EXIT ')),['LIMIT_EXIT '+JSON.stringify(row.workerExit)]);
    assert.ok(lines.every(s=>s.startsWith('LIMIT_PROBE ')||s.startsWith('LIMIT_EXIT ')||/^LIMIT_AUX type=(?:6|10|11) request=\d+ status=\d+ payload=.*$/.test(s)));
    return row.state;
}
export function validateNativeFetchGzipMemberReport(report,expected){
    fields(report,['schema','complete','runId','inputs','binaries','cases']);assert.equal(report.schema,'capsid-native-fetch-gzip-members-v1');assert.equal(report.complete,true);
    assert.equal(report.runId,expected.runId);assert.deepEqual(report.inputs,expected.inputs);assert.deepEqual(report.binaries,expected.binaries);
    assert.deepEqual(report.cases.map(row=>row.plan.id),plans.map(plan=>plan.id));return report.cases.map((row,i)=>assertNativeFetchGzipMemberCase(row,plans[i]));
}
