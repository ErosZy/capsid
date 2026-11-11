import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import {test} from 'node:test';
import {execFile} from 'node:child_process';
import {promisify} from 'node:util';
import {fileURLToPath} from 'node:url';
import {validateFanoutReport,compareFanoutReports} from '../tools/workerd-fanout-contract.mjs';
import {requiredTests} from '../tools/run-workerd-ws-gate.mjs';

const root=path.resolve(path.dirname(fileURLToPath(import.meta.url)),'..');
const sample=()=>JSON.parse(fs.readFileSync(path.join(root,'tests/fixtures/workerd-fanout-report.json'),'utf8')).report;
const expected=report=>({runId:report.runId,lock:report.referenceLock,clientLock:report.clientLock,
    inputs:report.inputs,host:'a'.repeat(64),worker:'b'.repeat(64)});
function fanoutCandidate(reference,mode,values) {
    const report=structuredClone(reference),hostCount=mode==='single-worker'?2:1;
    Object.assign(report,{mode,binaries:{host:values.host,worker:values.worker},
        topology:{workers:2,hostProcesses:hostCount,referenceServices:0},
        shutdowns:Array.from({length:hostCount},()=>({code:0,signal:null,forced:false})),
        backendStderr:Array(hostCount).fill('')});
    if(mode!=='single-worker') {
        report.endpoints=report.endpoints.slice(0,1);report.healthy=report.healthy.slice(0,1);
        for(const entry of report.discovery)entry.endpoint=report.endpoints[0];
    }
    return report;
}
test('required gate includes the actual two-Worker fanout matrix',()=>{
    for(const mode of ['reference','single-worker','static-pool','managed'])
        assert.ok(requiredTests.includes('workerd_fanout_'+mode),'required fanout CTest '+mode);
});
test('actual reference fanout sample passes without modifying its observations',()=>{
    const report=sample(),before=structuredClone(report);
    assert.equal(validateFanoutReport(report,expected(report)).workers,2);
    assert.deepEqual(report,before);
});
test('independently labelled synthetic candidates preserve the real reference semantics',()=>{
    const report=sample(),values=expected(report);
    assert.equal(compareFanoutReports(report,['single-worker','static-pool','managed'].map(mode=>fanoutCandidate(report,mode,values)),values).complete,true);
});
for(const [name,mutate] of [
    ['missing identity',r=>{delete r.discovery[0].identity.instance;}],
    ['Worker identities collapsed across groups',r=>{r.discovery[1].identity.instance=r.discovery[0].identity.instance;}],
    ['duplicate actual connection',r=>{r.discovery[1].identity.connection=r.discovery[0].identity.connection;}],
    ['foreign room membership',r=>{r.discovery[1].identity.members.push(r.discovery[0].identity.connection);}],
    ['single Worker masquerading as a pool',r=>{r.topology.workers=1;}],
    ['unexercised Worker',r=>{r.discovery=r.discovery.slice(0,3);}],
    ['wrong client source',r=>{r.client.sourceSHA256='0'.repeat(64);}],
    ['wrong fixture source',r=>{r.inputs.fixture='0'.repeat(64);}],
    ['stale run',r=>{r.runId='old';}],
    ['partial run',r=>{r.complete=false;}],
    ['missing ownership probe',r=>{r.probes.pop();}],
    ['reordered ownership probes',r=>{r.probes.reverse();}],
    ['wrong socket selected',r=>{r.probes[0].target=r.probes[0].sender;}],
    ['TypeError in place of reference Error',r=>{r.probes[0].response.attempts[0].errorName='TypeError';}],
    ['generic failure in place of ownership error',r=>{r.probes[0].response.attempts[0].message='connection refused';}],
    ['foreign send succeeds',r=>{r.probes[0].response.attempts[0].outcome='delivered';}],
    ['cross-Worker room is shared',r=>{r.probes[12].response.attempts=[structuredClone(r.probes[0].response.attempts[0])];}],
    ['missing overlapping handlers',r=>{r.overlap=[];}],
    ['handler never actually pending',r=>{r.overlap[0].heldBefore.at(-1).held=[];}],
    ['foreign output enters a pending handler',r=>{r.overlap[0].probe.attempts[0].outcome='delivered';}],
    ['wrong pending handler release',r=>{r.overlap[0].release.released=false;}],
    ['pending reply uses sender context',r=>{r.overlap[0].holdReply.connection=r.overlap[0].sender;}],
    ['missing live echo',r=>{r.echoes.pop();}],
    ['closed member retained',r=>{r.afterClose[0].at(-1).members.push(r.selected[0]);}],
    ['unclean Close',r=>{r.finalCloses[0].clean=false;}],
    ['extra peer message',r=>{r.unexpectedMessages[0].messages.push('foreign output');}],
    ['unhealthy backend',r=>{r.healthy[0].status=500;}],
    ['forced backend stop',r=>{r.shutdowns[0].forced=true;}],
])test('fanout contract rejects '+name,()=>{
    const report=sample(),values=structuredClone(expected(report));mutate(report);
    assert.throws(()=>validateFanoutReport(report,values));
});
test('identical missing overlap coverage on every runtime still fails',()=>{
    const report=sample(),values=expected(report),candidates=['single-worker','static-pool','managed'].map(mode=>fanoutCandidate(report,mode,values));
    for(const item of [report,...candidates])item.overlap=[];
    assert.throws(()=>compareFanoutReports(report,candidates,values),/overlapping-handler ownership probes/);
});

const execute=promisify(execFile);
for(const [name,change,diagnostic] of [
    ['baseline',null,null],
    ['per-connection identity',source=>source.replace('instance ??= crypto.randomUUID();','instance = crypto.randomUUID();'),/configured two Workers produce at most two instance groups/],
    ['ignored foreign send',source=>source.replace("else if (command.operation === 'send') peer.send(command.challenge);","else if (command.operation === 'send') void command.challenge;"),/foreign operation is rejected before the other handler stages output/],
    ['retained closed room member',source=>source.replace('room.delete(server);','void server;'),/closed fanout member leaves its room|fanout room cleanup observation limit/],
])test('real fanout driver '+(change?'detects ':'accepts ')+name,{timeout:20000},async()=>{
    const host=process.env.CAPSID_FANOUT_TEST_HOST,worker=process.env.CAPSID_FANOUT_TEST_WORKER;
    assert.ok(host&&worker,'actual Host/Worker required for live fanout controls');
    const archive=process.env.CAPSID_WS_REPORT_DIR;
    const dir=fs.mkdtempSync(path.join(archive??os.tmpdir(),'fanout-control-'));
    try {
        const original=fs.readFileSync(path.join(root,'tests/fixtures/workerd-ws/fanout.mjs'),'utf8');
        const fixture=path.join(dir,'fixture.mjs'),source=change?change(original):original;
        if(change)assert.notEqual(source,original,'isolated fixture mutation actually applied');
        fs.writeFileSync(fixture,source);
        const report=path.join(dir,'report.json'),args=[path.join(root,'tests/test_workerd_fanout.mjs'),
            '--host',host,'--worker',worker,'--mode','single-worker','--fixture',fixture,'--report',report];
        if(change)await assert.rejects(execute(process.execPath,args,{timeout:12000}),error=>{
            assert.equal(error.code,1);assert.equal(error.signal,null);assert.equal(error.killed,false);
            assert.match(error.stderr,diagnostic);assert.equal(JSON.parse(fs.readFileSync(report,'utf8')).complete,false);return true;
        });
        else {
            await execute(process.execPath,args,{timeout:12000});
            assert.equal(JSON.parse(fs.readFileSync(report,'utf8')).complete,true);
        }
    }finally{if(!archive)fs.rmSync(dir,{recursive:true,force:true});}
});
