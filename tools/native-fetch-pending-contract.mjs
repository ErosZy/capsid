import assert from 'node:assert/strict';
import fs from 'node:fs';
import {createHash} from 'node:crypto';
import {gunzipSync} from 'node:zlib';
import {isDeepStrictEqual} from 'node:util';
import {nativeFetchPendingPlans,nativeFetchPendingReply,nativeFetchPendingCasesForShard,expectedNativeFetchPendingState} from './native-fetch-pending-input.mjs';
const hash=bytes=>createHash('sha256').update(bytes).digest('hex');
const equal=(actual,expected,label)=>assert.ok(isDeepStrictEqual(actual,expected),label);
function fields(value,names,label){assert.ok(value&&typeof value==='object'&&!Array.isArray(value),label);equal(Object.keys(value).sort(),names.slice().sort(),label+' fields');}
export function expectedNativeFetchPendingSource(plan,port) {
    const state=expectedNativeFetchPendingState(plan),packet=nativeFetchPendingReply(plan),phase={action:plan.action,preControl:state.preControl,second:state.second,release:state.releaseWhilePending};
    const control=new URL('/control','http://localhost:'+port);control.searchParams.set('phase',JSON.stringify(phase));
    const record=(role,target,sent,ended)=>{
        const request='GET '+target+' HTTP/1.1\r\nHost: localhost:'+port;
        return {role,rawRequest:Buffer.from(request+'\r\n\r\n').toString('base64'),request,peerEnded:true,peerReset:false,fixtureEnded:ended,fixtureDestroyed:false,
            sent:sent.map(bytes=>bytes.toString('base64')),events:['accepted','request',...(role==='source'?['sent-head',...(ended?['sent-end']:[])]:['sent-end']),'peer-ended']};
    };
    const ack=Buffer.from('HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok');
    return {plan,records:[record('source','/hold',[packet.head,...(plan.action==='overlap'?[packet.encoded]:[])],plan.action==='overlap'),record('control',control.pathname+control.search,[ack],true)],
        controlBeforeBody:true,bodyReleased:plan.action==='overlap',failure:null,port,phase};
}
export function assertNativeFetchPendingSource(source,plan) {
    fields(source,['plan','records','controlBeforeBody','bodyReleased','failure','port','phase'],'native pending source');
    assert.ok(Number.isSafeInteger(source.port)&&source.port>0&&source.port<65536,'actual loopback port');
    const expected=expectedNativeFetchPendingSource(plan,source.port);equal(source.plan,plan,'exact current plan');equal(source.controlBeforeBody,true,'control reached while body withheld');equal(source.bodyReleased,expected.bodyReleased,'only overlap releases body');equal(source.failure,null,'source healthy');equal(source.phase,expected.phase,'actual control request proves pending state');equal(source.records.length,2,'one body and one control request');
    for(const [index,record]of source.records.entries()){
        fields(record,['role','rawRequest','request','peerEnded','peerReset','fixtureEnded','fixtureDestroyed','sent','events'],'complete peer record');
        assert.equal(typeof record.peerEnded,'boolean');assert.equal(typeof record.peerReset,'boolean');assert.ok(record.peerEnded||record.peerReset,'actual FIN or reset before stop');
        const model=expected.records[index];for(const key of ['role','rawRequest','request','fixtureEnded','fixtureDestroyed','sent'])equal(record[key],model[key],key+' exact source evidence');
        equal(record.events,model.events.slice(0,-1).concat(record.peerEnded?['peer-ended']:[]),'actual source event order');
        for(const value of [record.rawRequest,...record.sent])equal(Buffer.from(value,'base64').toString('base64'),value,'canonical full wire bytes');
    }
    return source;
}
function timing(value){fields(value,['stateMs','terminalMs','completeMs'],'collection timing');assert.ok(Object.values(value).every(v=>Number.isFinite(v)&&v>=0));assert.ok(value.stateMs<=value.terminalMs&&value.terminalMs<=value.completeMs,'actual monotonic collection order');}
export function assertNativeFetchPendingCase(row,plan) {
    fields(row,['plan','collectionComplete','timings','state','sourceTerminalBeforeFinish','sourceBeforeFinish','sourceAfterFinish','shutdown','stdout','stderr','workerExit'],'native pending case');
    equal(row.plan,plan,'mandatory case');equal(row.collectionComplete,true,'actual complete observation');timing(row.timings);
    equal(row.state,expectedNativeFetchPendingState(plan),'independent complete typed/backing/error/trace model');
    assertNativeFetchPendingSource(row.sourceBeforeFinish,plan);equal(row.sourceAfterFinish,row.sourceBeforeFinish,'worker teardown supplies no completion');equal(row.sourceTerminalBeforeFinish,true,'both native peers ended before parent finish');
    equal(row.shutdown,{code:0,signal:null},'real helper exit');equal(row.workerExit,{clean:true,processObserved:true,exitStatus:0,forced:false,errorEvents:0},'natural OS worker exit');equal(row.stderr,'','no runtime diagnostics');
    assert.equal(typeof row.stdout,'string');const lines=row.stdout.trim().split(/\r?\n/);
    equal(lines.filter(v=>v.startsWith('LIMIT_PROBE ')),['LIMIT_PROBE '+JSON.stringify(row.state)],'actual ABI state, no substituted report');equal(lines.filter(v=>v.startsWith('LIMIT_EXIT ')),['LIMIT_EXIT '+JSON.stringify(row.workerExit)],'actual OS observation');
    assert.ok(lines.every(v=>v.startsWith('LIMIT_PROBE ')||v.startsWith('LIMIT_EXIT ')||/^LIMIT_AUX type=(?:6|10|11) request=\d+ status=\d+ payload=.*$/.test(v)),'only complete declared ABI observations');
    assert.doesNotMatch(row.stdout,/TOKEN LEAK|POISON|AddressSanitizer|runtime error:/);return row.state;
}
export function validateNativeFetchPendingReport(report,expected) {
    fields(report,['schema','complete','shard','runId','inputs','binaries','cases'],'native pending report');equal(report.schema,'capsid-native-fetch-pending-v1','report schema');equal(report.complete,true,'report completed');
    equal(report.shard,expected.shard,'selected shard');equal(report.runId,expected.runId,'actual run');equal(report.inputs,expected.inputs,'current inputs');equal(report.binaries,expected.binaries,'actual runtime identities');
    const plans=nativeFetchPendingCasesForShard(report.shard);equal(report.cases.map(r=>r.plan.id),plans.map(r=>r.id),'complete ordered mandatory cases');return report.cases.map((row,index)=>assertNativeFetchPendingCase(row,plans[index]));
}
export function assertNativeFetchPendingReferenceArchive(archive,identities) {
    fields(archive,['schema','reports'],'primary archive');equal(archive.schema,'capsid-native-fetch-pending-primary-v1','primary schema');equal(archive.reports.length,2,'two actual fixed platforms');
    for(const [index,item]of archive.reports.entries()){
        fields(item,['platform','originalTextUTF8','originalReportSHA256'],'actual original primary');equal(item.platform,index?'linux':'native','platform order');equal(hash(item.originalTextUTF8),item.originalReportSHA256,'original raw report bytes');const report=JSON.parse(item.originalTextUTF8);
        fields(report,['schema','complete','fixtureSHA256','peerSHA256','sourceModelSHA256','captureSHA256','referenceLock','binarySHA256','client','cases','shutdown','stderr'],'actual primary report');equal(report.schema,'capsid-native-fetch-pending-reference-v1','reference schema');equal(report.complete,true,'primary completed');
        for(const key of ['fixtureSHA256','peerSHA256','sourceModelSHA256','captureSHA256'])equal(report[key],identities[key],'current primary input '+key);
        equal(report.referenceLock.source_commit,'8f311ae3e3cf3064983f56a6a1c8b7ee33d6b99f','pinned commit');equal(report.referenceLock.release_tag,'v1.20261010.1','pinned release');equal(report.referenceLock.compatibility_date,'2026-08-20','fixed date');equal(report.referenceLock.compatibility_flags,[],'fixed flags');
        equal(report.binarySHA256,index?'50f31156a9a4c7d549f1b4753dd61eb527e1b6b0cd416e5ba3d02d64dfdee5dd':'c8799724c4221af61dde76ed071aa5996b25c9bd5d247da94c166718cabe612a','pinned platform binary');equal(report.referenceLock.binary_sha256,report.binarySHA256,'lock identity');equal(report.client,{node:'v24.14.0',undici:'7.21.0'},'locked reference client');equal(report.stderr,'','primary diagnostics');assert.ok(report.shutdown.code===0&&report.shutdown.signal===null||report.shutdown.code===null&&report.shutdown.signal==='SIGTERM','observed reference stop');
        equal(report.cases.map(r=>r.plan.id),nativeFetchPendingPlans.map(r=>r.id),'all150 real primary observations');
        for(const [caseIndex,row]of report.cases.entries()){
            fields(row,['plan','timings','state','sourceTerminalBeforeReferenceStop','sourceBeforeReferenceStop','sourceAfterReferenceStop'],'complete primary case');const plan=nativeFetchPendingPlans[caseIndex];equal(row.plan,plan,'primary plan');equal(row.state,expectedNativeFetchPendingState(plan),'primary obeys independent model');timing(row.timings);assertNativeFetchPendingSource(row.sourceBeforeReferenceStop,plan);equal(row.sourceAfterReferenceStop,row.sourceBeforeReferenceStop,'primary teardown supplies no completion');equal(row.sourceTerminalBeforeReferenceStop,true,'both primary peers terminal');
        }
    }
    return archive;
}
export const nativeFetchPendingPrimaryIdentities=Object.freeze(Object.fromEntries([
    ['fixtureSHA256','../tests/fixtures/native-fetch-pending.mjs'],['peerSHA256','./native-fetch-pending-peer.mjs'],['sourceModelSHA256','./native-fetch-pending-input.mjs'],['captureSHA256','./capture-native-fetch-pending-reference.mjs'],
].map(([key,name])=>[key,hash(fs.readFileSync(new URL(name,import.meta.url)))])));
const compressed=fs.readFileSync(new URL('../tests/fixtures/workerd-native-fetch-pending-report.json.gz',import.meta.url));
const raw=gunzipSync(compressed,{maxOutputLength:16*1024*1024}),meta=JSON.parse(fs.readFileSync(new URL('../tests/fixtures/workerd-native-fetch-pending-report.json',import.meta.url),'utf8'));
equal(meta.schema,'capsid-native-fetch-pending-primary-metadata-v1','primary metadata schema');equal(meta.compressedSHA256,hash(compressed),'compressed primary bytes');equal(meta.rawSHA256,hash(raw),'lossless original primary archive');equal(meta.rawBytes,raw.length,'lossless raw length');equal(meta.identities,nativeFetchPendingPrimaryIdentities,'exact current reference inputs');
const primary=JSON.parse(raw);assertNativeFetchPendingReferenceArchive(primary,nativeFetchPendingPrimaryIdentities);
export function nativeFetchPendingPrimaryArchive(){return JSON.parse(raw);}
