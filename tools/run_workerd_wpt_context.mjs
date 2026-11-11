import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import net from 'node:net';
import {spawn} from 'node:child_process';
import {fileURLToPath} from 'node:url';
import {parseArgs} from 'node:util';
import {setTimeout as delay} from 'node:timers/promises';
import {root,lockPath,readLock,verifyClient,verifyBinary,fileDigest} from './workerd-reference.mjs';
import {referencePlatform,referenceBackendModes} from './workerd-platform.mjs';
import {startCapsid,stopCapsid} from '../tests/helpers/ws_host_backend.mjs';
import {profileV2File,auditV2Metadata,adaptCompleteWptProgram} from './workerd-wpt-profile-v2.mjs';

const {values:args}=parseArgs({options:Object.fromEntries(['build','registration','reference','output'].map(n=>[n,{type:'string'}]))});
for(const key of ['build','registration','output']) assert.ok(args[key],'required --'+key);
const read=file=>JSON.parse(fs.readFileSync(file,'utf8'));
const lock=readLock();verifyClient(lock);
const reference=args.reference??process.env.CAPSID_WORKERD_REF;
assert.ok(reference,'locked reference is required');verifyBinary(reference,lock);
const profile=read(path.join(root,profileV2File)),manifest=read(path.join(root,'tests/wpt/manifest.json'));
auditV2Metadata(profile,manifest,lock);
const output=path.resolve(args.output);assert.equal(fs.existsSync(output),false,'fresh output directory');
fs.mkdirSync(output,{recursive:true});
const runtime=fs.mkdtempSync(path.join(os.tmpdir(),'cwc-'));
const ext=referencePlatform(lock)==='win32'?'.exe':'';
const host=path.resolve(args.build,'capsid-host'+ext),worker=path.resolve(args.build,'capsid-worker'+ext);
const registration=read(args.registration);
const selected=profile.conflicts.map(contract=>{
    const name='worker_wpt_'+contract.path.replace(/[^A-Za-z0-9]/g,'_');
    const matches=registration.tests.filter(t=>t.name===name);assert.equal(matches.length,1);
    const test=matches[0];assert.equal(test.command[3],'wpt');assert.equal(test.command[4],'https://wpt.local/'+contract.path);
    assert.equal(fs.realpathSync(test.command[1]),fs.realpathSync(worker));
    return {path:contract.path,originalBundle:test.command[2]};
});
const measuredFiles=[fileURLToPath(import.meta.url),path.join(root,'tools/workerd-wpt-profile-v2.mjs'),
    path.join(root,profileV2File),path.join(root,'tests/wpt/manifest.json'),path.join(root,'tests/wpt/adapter.js'),
    path.join(root,'tests/wpt/app.js'),path.join(root,'tests/wpt/guard-rejection-events.mjs'),
    path.join(root,'tests/helpers/ws_host_backend.mjs'),lockPath,process.execPath,host,worker,reference,path.resolve(args.registration),
    ...selected.map(c=>c.originalBundle)];
const measuredInputs=Object.fromEntries(measuredFiles.map(file=>[file,fileDigest(file)]));
const record={schema:'capsid-workerd-wpt-context-v2',complete:false,status:'running',referenceLock:lock,
    binaries:{host:fileDigest(host),worker:fileDigest(worker),reference:fileDigest(reference),node:fileDigest(process.execPath)},
    measuredInputs,requestDeadlineMilliseconds:15000,wptInternalBudgetModified:false,
    standardFailuresRetained:true,runtimeDirectory:runtime,cases:[]};
const destination=path.join(output,'context.json');
const save=()=>fs.writeFileSync(destination,JSON.stringify(record,null,2)+'\n');
save();
async function unusedPort() {
    const server=net.createServer();
    await new Promise((resolve,reject)=>{server.once('error',reject);server.listen(0,'127.0.0.1',resolve);});
    const port=server.address().port;await new Promise(resolve=>server.close(resolve));return port;
}
async function observe(target,role,directory) {
    const response=await fetch(target,{signal:AbortSignal.timeout(record.requestDeadlineMilliseconds)});
    role.httpStatus=response.status;const body=await response.text();
    fs.writeFileSync(path.join(directory,'response.json'),body);assert.equal(response.status,200);
    role.report=JSON.parse(body);
    assert.equal(role.report.ranNothing,false);assert.ok(role.report.total>0);
}
try {
    for(const [index,entry] of selected.entries()) {
        const bytes=fs.readFileSync(entry.originalBundle),adapted=adaptCompleteWptProgram(bytes);
        const row={...entry,originalSHA256:fileDigest(entry.originalBundle),bodyPrefixSHA256:adapted.bodyPrefixSHA256,
            adaptedSHA256:adapted.adaptedSHA256,roles:[]};
        record.cases.push(row);save();
        for(const mode of ['reference',...referenceBackendModes(lock)]) {
            const archive=path.join(output,'c'+index,mode),directory=path.join(runtime,'c'+index,mode);
            fs.mkdirSync(archive,{recursive:true});fs.mkdirSync(directory,{recursive:true,mode:0o700});
            const bundle=path.join(directory,'main.mjs');fs.writeFileSync(bundle,adapted.program);
            const archivedBundle=path.join(archive,'main.mjs');fs.copyFileSync(bundle,archivedBundle);
            const role={mode,complete:false,bundle:archivedBundle,bundleSHA256:fileDigest(bundle),runtimeBundle:bundle};
            row.roles.push(role);save();
            let backend,primary,closed,stdout='',stderr='';
            try {
                if(mode==='reference') {
                    const port=await unusedPort(),config=path.join(directory,'config.capnp');
                    fs.writeFileSync(config,'using Workerd = import "/workerd/workerd.capnp";\n'+
                        'const config :Workerd.Config = (services=[(name="main",worker=(compatibilityDate='+
                        JSON.stringify(lock.compatibility_date)+',compatibilityFlags='+JSON.stringify(lock.compatibility_flags)+
                        ',modules=[(name="main",esModule=embed "main.mjs")]))],sockets=[(name="http",address="127.0.0.1:'+port+
                        '",http=(),service="main")]);\n');
                    primary=spawn(reference,['serve',config],{stdio:['ignore','pipe','pipe']});
                    closed=new Promise((resolve,reject)=>{primary.once('error',reject);primary.once('close',resolve);});
                    role.command=primary.spawnargs;role.pid=primary.pid;
                    primary.stdout.on('data',chunk=>stdout+=chunk);primary.stderr.on('data',chunk=>stderr+=chunk);
                    const deadline=Date.now()+10000;
                    for(;;) {
                        assert.equal(primary.exitCode,null,stderr);
                        try {
                            const socket=net.createConnection({host:'127.0.0.1',port});
                            await new Promise((resolve,reject)=>{socket.once('connect',resolve);socket.once('error',reject);});socket.destroy();break;
                        } catch(error) {assert.ok(Date.now()<deadline,String(error));await delay(25);}
                    }
                    await observe(`http://127.0.0.1:${port}/`,role,archive);
                } else {
                    backend=await startCapsid(host,worker,bundle,directory,mode,{requestTimeoutMs:15000});
                    role.command=backend.child.spawnargs;role.pid=backend.child.pid;
                    await observe(`http://127.0.0.1:${backend.port}/@capsid/orders/`,role,archive);
                }
            } finally {
                if(backend) {
                    role.shutdown=await stopCapsid(backend.child);stderr=backend.stderr();
                    assert.deepEqual(role.shutdown,{code:0,signal:null,forced:false});
                }
                if(primary) {
                    role.cleanupRequested=true;role.forced=false;
                    if(primary.exitCode===null && primary.signalCode===null) primary.kill('SIGTERM');
                    let timer;
                    await Promise.race([closed,new Promise(resolve=>{timer=setTimeout(()=>{role.forced=true;primary.kill('SIGKILL');resolve();},5000);})]);
                    clearTimeout(timer);await closed;
                    role.cleanupExit={code:primary.exitCode,signal:primary.signalCode};role.naturalReferenceExitNotClaimed=true;
                    assert.equal(role.forced,false,'reference shutdown completes without force kill');
                }
                role.stdout=stdout;role.stderr=stderr;
                fs.writeFileSync(path.join(archive,'stdout.log'),stdout);fs.writeFileSync(path.join(archive,'stderr.log'),stderr);
                save();
            }
            assert.equal(fileDigest(bundle),row.adaptedSHA256);assert.equal(fileDigest(archivedBundle),row.adaptedSHA256);
            assert.doesNotMatch(stderr,/AddressSanitizer|LeakSanitizer|runtime error:/);
            role.complete=true;save();
        }
    }
    for(const [file,hash] of Object.entries(measuredInputs)) assert.equal(fileDigest(file),hash,'input unchanged: '+file);
    record.complete=true;record.status='captured-with-standard-results';save();
    console.log(JSON.stringify({complete:true,files:5,reports:record.cases.reduce((sum,row)=>sum+row.roles.length,0),standardPassed:false,report:destination}));
} catch(error) {
    record.status='failed';record.error=String(error.stack??error);save();throw error;
}
