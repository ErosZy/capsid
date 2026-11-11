import assert from 'node:assert/strict';
import fs from 'node:fs';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';
import {spawn} from 'node:child_process';
import {fileURLToPath} from 'node:url';
import {setTimeout as delay} from 'node:timers/promises';
import {nativeFetchPendingPlans,expectedNativeFetchPendingState} from './native-fetch-pending-input.mjs';
import {openPendingSource} from './native-fetch-pending-peer.mjs';
import {readLock,verifyBinary,verifyClient,fileDigest} from './workerd-reference.mjs';
assert.equal(process.argv.length,4);assert.equal(process.argv[2],'--output');const target=process.argv[3];assert.equal(fs.existsSync(target),false,'fresh primary report');
const binary=process.env.CAPSID_WORKERD_REF,lock=readLock();verifyBinary(binary,lock);verifyClient(lock);const fixture=fileURLToPath(new URL('../tests/fixtures/native-fetch-pending.mjs',import.meta.url));
const report={schema:'capsid-native-fetch-pending-reference-v1',complete:false,fixtureSHA256:fileDigest(fixture),peerSHA256:fileDigest(fileURLToPath(new URL('./native-fetch-pending-peer.mjs',import.meta.url))),sourceModelSHA256:fileDigest(fileURLToPath(new URL('./native-fetch-pending-input.mjs',import.meta.url))),captureSHA256:fileDigest(fileURLToPath(import.meta.url)),referenceLock:lock,binarySHA256:fileDigest(binary),client:{node:process.version,undici:process.versions.undici},cases:[],shutdown:null,stderr:''};
const save=()=>fs.writeFileSync(target,JSON.stringify(report,null,2)+'\n');save();const directory=fs.mkdtempSync(path.join(os.tmpdir(),'capsid-native-pending-primary-'));assert.ok(path.isAbsolute(directory)&&path.basename(directory).startsWith('capsid-native-pending-primary-'));
const listener=net.createServer();await new Promise(resolve=>listener.listen(0,'127.0.0.1',resolve));const port=listener.address().port;await new Promise(resolve=>listener.close(resolve));fs.copyFileSync(fixture,path.join(directory,'main.mjs'));
const config=path.join(directory,'config.capnp');fs.writeFileSync(config,`using Workerd = import "/workerd/workerd.capnp"; const config :Workerd.Config = (services=[(name="main",worker=(compatibilityDate=${JSON.stringify(lock.compatibility_date)},compatibilityFlags=${JSON.stringify(lock.compatibility_flags)},modules=[(name="main",esModule=embed "main.mjs")])),(name="internet",network=(allow=["local"]))],sockets=[(name="http",address="127.0.0.1:${port}",http=(),service="main")]);`);
const child=spawn(binary,['serve',config],{stdio:['ignore','pipe','pipe']});child.stdout.resume();child.stderr.on('data',v=>{report.stderr+=v;});const closed=new Promise((resolve,reject)=>{child.once('close',(code,signal)=>resolve({code,signal}));child.once('error',reject);});
const heldSources=[];
try{
    const end=performance.now()+10000;for(;;)try{const response=await fetch('http://127.0.0.1:'+port+'/healthy',{signal:AbortSignal.timeout(500)});assert.equal(await response.text(),'healthy');break;}catch(error){assert.ok(performance.now()<end&&child.exitCode===null&&child.signalCode===null,String(error)+report.stderr);await delay(20);}
    for(const plan of nativeFetchPendingPlans){
        const start=performance.now(),source=await openPendingSource(plan),row={plan,timings:{}};report.cases.push(row);heldSources.push({source,row,start});
        try{
            const query=new URLSearchParams({target:source.target,route:plan.route,api:plan.api,action:plan.action}),response=await fetch('http://127.0.0.1:'+port+'/probe?'+query,{signal:AbortSignal.timeout(12000)});assert.equal(response.status,200);row.state=await response.json();row.timings.stateMs=performance.now()-start;row.sourceTerminalBeforeReferenceStop=await source.terminal();row.timings.terminalMs=performance.now()-start;row.sourceBeforeReferenceStop=structuredClone(source.data);assert.equal(row.sourceTerminalBeforeReferenceStop,true);assert.equal(source.data.controlBeforeBody,true);assert.equal(source.data.failure,null);assert.deepEqual(row.state,expectedNativeFetchPendingState(plan),'actual primary and independent model '+plan.id);
        }finally{row.timings.completeMs=performance.now()-start;save();}
    }
    report.complete=true;
}finally{
    child.kill('SIGTERM');let timer;try{report.shutdown=await Promise.race([closed,new Promise((_,reject)=>{timer=setTimeout(()=>reject(new Error('reference stop deadline')),2000);})]);}finally{
        clearTimeout(timer);
        try{for(const {source,row,start} of heldSources){row.sourceAfterReferenceStop=structuredClone(source.data);row.timings.completeMs=performance.now()-start;assert.deepEqual(row.sourceAfterReferenceStop,row.sourceBeforeReferenceStop,'actual reference process stop must not supply peer completion');}}
        catch(error){report.complete=false;throw error;}
        finally{save();await Promise.all(heldSources.map(({source})=>source.close()));fs.rmSync(directory,{recursive:true,force:true});}
    }
}
assert.equal(fileDigest(binary),report.binarySHA256);assert.equal(fileDigest(fixture),report.fixtureSHA256);assert.equal(report.stderr,'');console.log(JSON.stringify({complete:report.complete,cases:report.cases.length,output:target}));
