import assert from 'node:assert/strict';
import fs from 'node:fs';
import {spawn} from 'node:child_process';
import {test} from 'node:test';
import {setTimeout as delay} from 'node:timers/promises';
import {fileURLToPath} from 'node:url';

const workerPath=fileURLToPath(new URL('../bench/fake/fake-worker.sh',import.meta.url));
function processIdentity(pid) {
    try {
        const stat=fs.readFileSync(`/proc/${pid}/stat`,'utf8');
        const fields=stat.slice(stat.lastIndexOf(')')+2).split(' ');
        return {pid,state:fields[0],parent:Number(fields[1]),start:fields[19]};
    } catch(error) {if(error.code==='ENOENT' || error.code==='ESRCH')return null;throw error;}
}
function children(pid) {
    try {
        return fs.readFileSync(`/proc/${pid}/task/${pid}/children`,'utf8').trim().split(/\s+/)
            .filter(Boolean).map(Number).map(processIdentity).filter(Boolean);
    } catch(error) {if(error.code==='ENOENT' || error.code==='ESRCH')return [];throw error;}
}
async function waitFor(predicate,timeout=2000) {
    const deadline=performance.now()+timeout;
    while(!predicate()) {if(performance.now()>=deadline)return false;await delay(10);}
    return true;
}
function sameProcess(identity) {return processIdentity(identity.pid)?.start===identity.start;}

for(const signal of ['SIGTERM','SIGINT']) test(`fake worker reaps its actual child on ${signal}`,async t=>{
    const child=spawn('bash',[workerPath],{stdio:'ignore'});
    let spawnError;child.on('error',error=>{spawnError=error;});
    const owned=[];
    try {
        assert.ok(await waitFor(()=>spawnError || children(child.pid).length>0),'fake worker starts a real child');
        assert.ifError(spawnError);
        const parent=processIdentity(child.pid),descendants=children(child.pid);
        assert.ok(parent);assert.equal(descendants.length,1,'one observed sleep process');
        owned.push(parent,...descendants);
        assert.equal(descendants[0].parent,child.pid);
        assert.ok(child.kill(signal),'signal reaches the fake worker');
        assert.ok(await waitFor(()=>child.exitCode!==null || child.signalCode!==null),'fake worker exits within its deadline');
        const reaped=await waitFor(()=>descendants.every(identity=>!sameProcess(identity)),1000);
        t.diagnostic(JSON.stringify({signal,parent,descendants,remaining:descendants.map(x=>processIdentity(x.pid))}));
        assert.ok(reaped,'fake worker left its sleep child alive after shutdown');
    } finally {
        // Only kill observed identities from this invocation. A reused PID
        // must never turn test cleanup into an unrelated process operation.
        for(const identity of owned.reverse()) if(sameProcess(identity)) {
            try {process.kill(identity.pid,'SIGKILL');} catch(error) {if(error.code!=='ESRCH')throw error;}
        }
        if(child.exitCode===null && child.signalCode===null)child.kill('SIGKILL');
    }
});
