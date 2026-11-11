import assert from 'node:assert/strict';
import fs from 'node:fs';
import {test} from 'node:test';

const archive=JSON.parse(fs.readFileSync(new URL('./fixtures/native-fetch-encoded-contract-cases.json',import.meta.url),'utf8'));
assert.equal(archive.schema,'capsid-native-fetch-encoded-contract-cases-v1');
assert.equal(archive.complete,true);
assert.deepEqual(archive.groups.map(row=>row.kind),['finished','held','owner']);
const rewriteOutput=row=>{
    const lines=row.stdout.trim().split(/\r?\n/);
    row.stdout=lines.map(line=>line.startsWith('LIMIT_PROBE ')?'LIMIT_PROBE '+JSON.stringify(row.state):line).join('\n')+'\n';
};
for(const group of archive.groups){
    const name=group.kind==='finished'?'codec-branch':group.kind==='held'?'held-branch':'held-owner';
    const model=await import('../tools/native-fetch-'+name+'-contract.mjs');
    test(group.kind+' independent formula accepts complete actual rows',()=>{
        assert.equal(group.acceptedRows.length,model.plans.length);
        for(const [index,row] of group.acceptedRows.entries())model.assertProductRow(row,model.plans[index]);
    });
    test(group.kind+' rejects the actual pre-fix regression',()=>{
        assert.equal(group.actualFailedRows.length,model.plans.length);
        let rejected=0;
        for(const [index,row] of group.actualFailedRows.entries()){
            try{model.assertProductRow(row,model.plans[index]);}catch(error){assert.equal(error.name,'AssertionError');rejected++;}
        }
        assert.equal(rejected,group.expectedOriginalFailures);
    });
    test(group.kind+' rejects coherent public-state and resource mutations',()=>{
        let rejected=0;
        for(const [index,row] of group.acceptedRows.entries()){
            const controls=[
                value=>value.state.style='different-style',
                value=>{if(value.state.outer)value.state.outer.locked=false;else value.state.outer={bodyUsed:true,locked:false};},
                value=>value.sourceEndedBeforeFinish=false,
                value=>value.workerExit.forced=true,
                value=>value.shutdown.code=1,
                value=>{value.payloadSHA256='0'.repeat(64);value.packetSHA256='0'.repeat(64);},
                value=>{for(const key of ['source','sourceBeforeFinish'])value[key].fixtureDestroyed=true;},
            ];
            if(row.state.branches.length)controls.push(
                value=>value.state.branches[0].closedStable=false,
                value=>value.state.branches[0].after.locked=true,
            );
            if(row.state.branches[0]?.reads.length)controls.push(
                value=>value.state.branches[0].reads[0].dataHash='0'.repeat(64),
                value=>value.state.branches[0].reads[0].backingHash='0'.repeat(64),
                value=>value.state.branches[0].reads[0].inputAfterIssue=1,
            );
            if(group.kind==='owner')controls.push(
                value=>{value.state.ownerGate.sourcePeerEnded=false;value.marker.state.sourcePeerEnded=false;value.marker.sourceBeforeReply.peerEnded=false;},
                value=>value.state.retainedAfterGate[0].bodyUsed=false,
                value=>value.marker.originalBoundMilliseconds=4000,
            );
            for(const mutate of controls){const value=structuredClone(row);mutate(value);rewriteOutput(value);assert.throws(()=>model.assertProductRow(value,model.plans[index]));rejected++;}
        }
        assert.ok(rejected>=group.acceptedRows.length*7);
    });
}
