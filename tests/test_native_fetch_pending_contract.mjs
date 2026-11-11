import assert from 'node:assert/strict';
import {test} from 'node:test';
import {createHash} from 'node:crypto';
import {nativeFetchPendingPlans,nativeFetchPendingShards,nativeFetchPendingCasesForShard,expectedNativeFetchPendingState} from '../tools/native-fetch-pending-input.mjs';
import {assertNativeFetchPendingCase,validateNativeFetchPendingReport,expectedNativeFetchPendingSource,assertNativeFetchPendingReferenceArchive,nativeFetchPendingPrimaryArchive,nativeFetchPendingPrimaryIdentities} from '../tools/native-fetch-pending-contract.mjs';
const clone=structuredClone,hash=v=>createHash('sha256').update(v).digest('hex');let rejected=0;
const rejects=(label,action)=>{assert.throws(action,undefined,label);rejected++;};
const exit={clean:true,processObserved:true,exitStatus:0,forced:false,errorEvents:0};
const syncOutput=row=>{row.stdout='LIMIT_PROBE '+JSON.stringify(row.state)+'\nLIMIT_EXIT '+JSON.stringify(row.workerExit)+'\n';return row;};
function makeCase(plan){const source=expectedNativeFetchPendingSource(plan,12345);return syncOutput({plan,collectionComplete:true,timings:{stateMs:1,terminalMs:2,completeMs:3},state:expectedNativeFetchPendingState(plan),sourceTerminalBeforeFinish:true,sourceBeforeFinish:source,sourceAfterFinish:clone(source),shutdown:{code:0,signal:null},stderr:'',workerExit:clone(exit)});}
function makeReport(shard){const expected={shard,runId:'synthetic-native-pending-controls',inputs:{fixture:'a'.repeat(64)},binaries:{helper:'b'.repeat(64),worker:'c'.repeat(64)}};return {expected,report:{schema:'capsid-native-fetch-pending-v1',complete:true,...expected,cases:nativeFetchPendingCasesForShard(shard).map(makeCase)}};}
test('all150 independent typed/backing/constructor/cancellation models are valid verifier inputs',()=>{
    assert.equal(nativeFetchPendingPlans.length,150);assert.equal(new Set(nativeFetchPendingPlans.map(p=>p.id)).size,150);
    for(const plan of nativeFetchPendingPlans)assertNativeFetchPendingCase(makeCase(plan),plan);
});
test('all150 coherent observations reject changed semantics even when stdout agrees',()=>{
    for(const plan of nativeFetchPendingPlans)for(const mutate of [
        s=>{if(s.first.error)s.first.error.sameReason=true;else s.first.done=true;},s=>s.first.initialInputBytes=99,
        s=>s.second.returnedPromise=false,s=>s.second.inputAfterIssue=99,s=>s.second.error.message='different pending error',
        s=>s.releaseWhilePending.syncError.name='Error',s=>s.preControl.firstPending=false,s=>s.preControl.closedPending=false,
        s=>s.construction.constructorMatches=!s.construction.constructorMatches,s=>s.closedStable=false,s=>s.released=false,
        s=>[s.trace[0],s.trace[1]]=[s.trace[1],s.trace[0]],s=>s.after.bodyUsed=false,s=>s.after.locked=true,
        s=>{if(s.first.backing)s.first.backing[0]^=1;else s.first.error.nativePrototype=false;},s=>s.tails.push(clone(s.tails[0])),
    ]){const row=makeCase(plan);mutate(row.state);syncOutput(row);rejects(plan.id,()=>assertNativeFetchPendingCase(row,plan));}
});
test('all six source/action classes require full actual wire, gating, terminal order and natural exit',()=>{
    for(const shard of nativeFetchPendingShards){const plan=nativeFetchPendingCasesForShard(shard).find(p=>p.route==='getReader'&&p.api==='read-Uint8Array');
        const sourceMutations=[s=>s.controlBeforeBody=false,s=>s.bodyReleased=!s.bodyReleased,s=>s.failure='source failed',s=>s.records.pop(),
            s=>s.records[0].fixtureDestroyed=true,s=>s.records[0].sent[0]=Buffer.from('bad header').toString('base64'),
            s=>s.records[0].sent.push('AA=='),s=>s.records[0].rawRequest=Buffer.from('different request').toString('base64'),
            s=>s.records[0].role='control',s=>{s.records[0].peerEnded=false;s.records[0].peerReset=false;},
            s=>s.records[0].events.reverse(),s=>s.phase.preControl.firstPending=false,s=>s.records[1].rawRequest='AA=='];
        for(const mutate of sourceMutations){const row=makeCase(plan);mutate(row.sourceBeforeFinish);row.sourceAfterFinish=clone(row.sourceBeforeFinish);rejects(shard,()=>assertNativeFetchPendingCase(row,plan));}
        for(const mutate of [r=>r.sourceAfterFinish.records[0].fixtureDestroyed=true,r=>r.sourceTerminalBeforeFinish=false,
            r=>r.workerExit.forced=true,r=>r.shutdown.code=1,r=>r.stderr='runtime error:',r=>r.timings.stateMs=-1,r=>r.timings.completeMs=0]){
            const row=makeCase(plan);mutate(row);syncOutput(row);rejects(shard,()=>assertNativeFetchPendingCase(row,plan));}
    }
});
test('complete shard reports reject omitted/duplicated/reordered rows and forged identities',()=>{
    for(const shard of nativeFetchPendingShards){const {report,expected}=makeReport(shard);validateNativeFetchPendingReport(report,expected);
        for(const mutate of [r=>r.cases.shift(),r=>r.cases.push(clone(r.cases[0])),r=>[r.cases[0],r.cases[1]]=[r.cases[1],r.cases[0]],
            r=>r.runId+='-other',r=>r.binaries.worker='d'.repeat(64),r=>r.inputs.fixture='d'.repeat(64),r=>r.complete=false,
            r=>r.schema+='-other',r=>r.shard='other',r=>r.cases[0].collectionComplete=false,r=>delete r.cases[0].timings,
            r=>r.cases[0].extra=true,r=>delete r.inputs]){
            const row=clone(report);mutate(row);rejects(shard,()=>validateNativeFetchPendingReport(row,expected));
        }
    }
});
test('an already accepted mutable report and case are revalidated after mutation',()=>{
    const {report,expected}=makeReport('gzip_abort');validateNativeFetchPendingReport(report,expected);report.cases[0].state.trace.reverse();syncOutput(report.cases[0]);rejects('same accepted report',()=>validateNativeFetchPendingReport(report,expected));
    const plan=nativeFetchPendingPlans[0],row=makeCase(plan);assertNativeFetchPendingCase(row,plan);row.state.first.bytes[0]^=1;syncOutput(row);rejects('same accepted case',()=>assertNativeFetchPendingCase(row,plan));
});
test('both actual primary files must match the independent model even with coherent rewritten hashes',()=>{
    const archive=nativeFetchPendingPrimaryArchive();assertNativeFetchPendingReferenceArchive(archive,nativeFetchPendingPrimaryIdentities);
    for(const mutate of [r=>r.cases[0].state.first.backing[0]^=1,r=>r.cases[0].state.second.inputAfterIssue=42,
        r=>r.cases.filter(v=>v.plan.kind==='gzip'&&v.plan.action==='abort').forEach(v=>v.state.trace=['second.rejected','closed.rejected','first.rejected','abort.called','tail0.rejected']),
        r=>r.cases[0].sourceBeforeReferenceStop.fixtureDestroyed=true,r=>r.cases[0].sourceBeforeReferenceStop.records[0].fixtureDestroyed=true,
        r=>r.cases[0].sourceBeforeReferenceStop.controlBeforeBody=false,r=>r.cases.pop(),r=>r.referenceLock.compatibility_flags.push('changed'),
        r=>r.referenceLock.compatibility_date='2026-08-21',r=>r.binarySHA256='d'.repeat(64),r=>r.client.node='other',r=>r.peerSHA256='d'.repeat(64)]){
        const changed=clone(archive);for(const item of changed.reports){const document=JSON.parse(item.originalTextUTF8);mutate(document);item.originalTextUTF8=JSON.stringify(document);item.originalReportSHA256=hash(item.originalTextUTF8);}rejects('both actual primaries coherently damaged',()=>assertNativeFetchPendingReferenceArchive(changed,nativeFetchPendingPrimaryIdentities));
    }
    for(const mutate of [a=>a.reports.pop(),a=>a.reports.reverse(),a=>a.reports[0].originalReportSHA256='d'.repeat(64)]){const changed=clone(archive);mutate(changed);rejects('primary provenance',()=>assertNativeFetchPendingReferenceArchive(changed,nativeFetchPendingPrimaryIdentities));}
});
test('control counts describe verifier mutations, separately from actual runtime executions',()=>{
    assert.equal(rejected,2615);console.log('NATIVE_PENDING_CONTROLS '+JSON.stringify({modelCases:150,rejectedMutations:rejected,actualProductProgramsExecuted:0}));
});
