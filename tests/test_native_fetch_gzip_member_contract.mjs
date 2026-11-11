import assert from 'node:assert/strict';
import {test,after} from 'node:test';
import fs from 'node:fs';
import {createHash} from 'node:crypto';
import {nativeFetchGzipMemberPlans as plans,nativeFetchGzipMemberReply,nativeFetchGzipMemberInputPaths} from '../tools/native-fetch-gzip-member-input.mjs';
import {expectedNativeFetchGzipMemberState,assertNativeFetchGzipMemberCase,assertNativeFetchGzipMemberReferenceArchive,validateNativeFetchGzipMemberReport} from '../tools/native-fetch-gzip-member-contract.mjs';

const hash=b=>createHash('sha256').update(b).digest('hex');let rejected=0;
const stdout=row=>'LIMIT_PROBE '+JSON.stringify(row.state)+'\nLIMIT_EXIT '+JSON.stringify(row.workerExit)+'\n';
function sample(plan){
    const reply=nativeFetchGzipMemberReply(plan),port=19000,request='GET /hold HTTP/1.1\r\nHost: localhost:'+port;
    const source={request,rawRequest:Buffer.from(request+'\r\n\r\n').toString('base64'),peerEnded:true,peerReset:false,fixtureEnded:true,fixtureDestroyed:false,events:['request','sent-complete','peer-ended'],sent:reply.packet.toString('base64')};
    const row={plan:structuredClone(plan),encodedBytes:reply.payload.length,payloadSHA256:hash(reply.payload),packetSHA256:hash(reply.packet),source,port,state:expectedNativeFetchGzipMemberState(plan),
        sourceEndedBeforeFinish:true,sourceBeforeFinish:structuredClone(source),shutdown:{code:0,signal:null},stdout:'',stderr:'',workerExit:{clean:true,processObserved:true,exitStatus:0,forced:false,errorEvents:0}};
    row.stdout=stdout(row);return row;
}
function reject(row,plan,mutate){const copy=structuredClone(row);mutate(copy);copy.stdout=stdout(copy);assert.throws(()=>assertNativeFetchGzipMemberCase(copy,plan));rejected++;}
for(const plan of plans)test('member/full-backing/source/exit controls '+plan.id,()=>{
    const row=sample(plan);assertNativeFetchGzipMemberCase(row,plan);
    for(const key of Object.keys(row.state))reject(row,plan,value=>{delete value.state[key];});
    for(let i=0;i<row.state.reads.length;i++){
        for(const key of Object.keys(row.state.reads[i]))reject(row,plan,value=>{delete value.state.reads[i][key];});
        for(const change of [r=>{r.inputAfterIssue=false;},r=>{r.done=1;},r=>{r.dataHash='0'.repeat(64);},r=>{r.backingHash='0'.repeat(64);},r=>{r.offset^=1;}])reject(row,plan,value=>change(value.state.reads[i]));
    }
    for(const change of [value=>{value.state.closed={pending:true};},value=>{value.state.after.locked=true;},
        value=>{value.shutdown.code=1;},value=>{value.workerExit.forced=true;},
        value=>{value.source.events.pop();value.sourceBeforeFinish=structuredClone(value.source);},
        value=>{value.source.sent='';value.sourceBeforeFinish=structuredClone(value.source);},
        value=>{value.sourceBeforeFinish.fixtureDestroyed=true;}])reject(row,plan,change);
    const current=sample(plan);assertNativeFetchGzipMemberCase(current,plan);current.state.closedStable=1;current.stdout=stdout(current);
    assert.throws(()=>assertNativeFetchGzipMemberCase(current,plan));rejected++;assertNativeFetchGzipMemberCase(sample(plan),plan);
});
test('both archived primaries cannot bless the same incorrect state or wire',()=>{
    const archive=JSON.parse(fs.readFileSync(new URL('./fixtures/workerd-native-fetch-gzip-member-report.json',import.meta.url),'utf8'));
    const fixtureHash=hash(fs.readFileSync(new URL('./fixtures/native-fetch-gzip-members.mjs',import.meta.url)));assertNativeFetchGzipMemberReferenceArchive(archive,fixtureHash);
    for(const mutate of [r=>{r.cases[0].state.reads[0].backingHash='0'.repeat(64);},r=>{r.cases[0].sourceBeforeReferenceStop.peerEnded=false;},r=>{r.binarySHA256='0'.repeat(64);},r=>{r.referenceLock.compatibility_date='2026-08-21';}]){
        const copy=structuredClone(archive);for(const item of copy.reports){const report=JSON.parse(item.originalTextUTF8);mutate(report);item.originalTextUTF8=JSON.stringify(report);item.originalReportSHA256=hash(Buffer.from(item.originalTextUTF8));}
        assert.throws(()=>assertNativeFetchGzipMemberReferenceArchive(copy,fixtureHash));rejected++;
    }
    archive.reports[0].originalTextUTF8+=' ';assert.throws(()=>assertNativeFetchGzipMemberReferenceArchive(archive,fixtureHash));rejected++;
});
test('complete current report and every current mutable value',()=>{
    const inputs=Object.fromEntries(Object.keys(nativeFetchGzipMemberInputPaths).map(key=>[key,'a'.repeat(64)])),binaries={helper:'b'.repeat(64),worker:'c'.repeat(64)};
    const report={schema:'capsid-native-fetch-gzip-members-v1',complete:true,runId:'synthetic-controls',inputs,binaries,cases:plans.map(sample)},expected={runId:report.runId,inputs:structuredClone(inputs),binaries:structuredClone(binaries)};
    validateNativeFetchGzipMemberReport(report,expected);
    for(const mutate of [r=>{r.complete=false;},r=>{r.binaries.worker='d'.repeat(64);},r=>{r.inputs.fixture='d'.repeat(64);},r=>{r.cases.pop();},r=>{r.cases[1]=structuredClone(r.cases[0]);}]){
        const copy=structuredClone(report);mutate(copy);assert.throws(()=>validateNativeFetchGzipMemberReport(copy,expected));rejected++;
    }
    validateNativeFetchGzipMemberReport(report,expected);
});
after(()=>{assert.equal(rejected,474);console.log(JSON.stringify({complete:true,uniqueCases:8,rejectedMutations:rejected}));});
