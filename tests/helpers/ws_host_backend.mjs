import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';
import http from 'node:http';
import net from 'node:net';
import { pathToFileURL } from 'node:url';
import { setTimeout as delay } from 'node:timers/promises';

const parentControls = new WeakMap();
const stopRequests = new WeakMap();
const childClosures = new WeakMap();

// Preserve the scenario failure when cleanup also fails. Both errors remain
// inspectable, and a cleanup failure still fails an otherwise healthy scenario.
export async function withCleanup(operation, cleanup) {
    let result, primary;
    try { result = await operation(); } catch (error) { primary = error; }
    try { await cleanup(); } catch (error) {
        if (primary) throw new AggregateError([primary, error],
            `${primary.message}; cleanup: ${error.message}`, {cause: primary});
        throw error;
    }
    if (primary) throw primary;
    return result;
}

async function admin(socketPath, target, body, method=body?'POST':'GET') {
    return new Promise((resolve,reject) => {
        const fail=error=>{
            const failure=new Error(`admin ${method} ${target}: ${error.message}`,{cause:error});
            if(error.code!==undefined)failure.code=error.code;
            failure.adminRequest={method,target};
            reject(failure);
        };
        const request=http.request({socketPath,path:target,method,headers:body?{'content-type':'application/json'}:{}},response => {
            const chunks=[]; response.on('data',c=>chunks.push(c));
            response.once('error',fail);
            response.on('end',()=>{try {const text=Buffer.concat(chunks).toString();resolve({status:response.statusCode,text,...(target==='/metrics'?{}:{json:JSON.parse(text)})});} catch(error){fail(error);}});
        });
        request.on('error',fail); request.end(body?JSON.stringify(body):undefined);
        request.setTimeout(5000,()=>request.destroy(new Error(`admin request deadline: ${target}`)));
    });
}
// bind() publishes the pathname before listen() accepts connections. Probe
// only this startup condition under the original absolute deadline; deployment
// and operation requests below remain one-shot, including ECONNREFUSED.
async function waitForAdminListener(child,socketPath,deadline,stderr) {
    for(;;) {
        assert.ok(Date.now()<deadline&&child.exitCode===null&&child.signalCode===null,
            `admin startup: ${stderr()}`);
        try {
            await new Promise((resolve,reject)=>{
                const socket=net.createConnection({path:socketPath});
                let settled=false;
                const finish=error=>{
                    if(settled)return;
                    settled=true;clearTimeout(timer);
                    child.removeListener('exit',exited);child.removeListener('error',failed);
                    socket.destroy();
                    if(error)reject(error);else resolve();
                };
                const exited=()=>finish(new Error(`admin startup: host exited: ${stderr()}`));
                const failed=error=>finish(new Error('admin startup: host spawn failed',{cause:error}));
                const timer=setTimeout(()=>finish(new Error('admin listener startup deadline')),
                    Math.max(1,deadline-Date.now()));
                socket.once('connect',()=>finish());socket.once('error',finish);
                child.once('exit',exited);child.once('error',failed);
                if(child.exitCode!==null||child.signalCode!==null)exited();
            });
            return;
        } catch(error) {
            if(!['ENOENT','ECONNREFUSED'].includes(error.code))throw error;
            await delay(Math.min(25,Math.max(0,deadline-Date.now())));
        }
    }
}
async function unusedPort() {
    const listener=net.createServer();
    await new Promise((resolve,reject)=>{listener.once('error',reject);listener.listen(0,'127.0.0.1',resolve);});
    const port=listener.address().port;
    await new Promise(resolve=>listener.close(resolve)); return port;
}
export async function startCapsid(host,worker,bundle,dir,mode='single-worker',options={}) {
    let socketPath, port;
    let args=['--mode',mode,'--worker',worker,'--source-bundle',bundle,'--source-name',pathToFileURL(bundle).href,
        '--application','orders','--listen','127.0.0.1:0','--routing','path','--public-scheme','http',
        '--public-authority','public.example','--request-timeout-ms',String(options.requestTimeoutMs ?? 10000),'--strict-sandbox','off','--ready-fd','3'];
    const workers=options.workers ?? 1;
    if(mode==='static-pool') args.push('--workers',String(workers));
    const capacity=options.channelCapacity ?? {};
    if(mode!=='managed') {
        for(const [key,flag] of [['channelsTotal','channels-total'],['pendingDialsTotal','pending-dials-total'],['channelQueueBytesTotal','channel-queue-bytes-total']])
            if(capacity[key]!==undefined)args.push('--'+flag,String(capacity[key]));
        if(options.listenerChannels!==undefined)args.push('--listener-channels',String(options.listenerChannels));
        if(options.listenerChannelQueueBytes!==undefined)args.push('--listener-channel-queue-bytes',String(options.listenerChannelQueueBytes));
    }
    const permissions=options.permissions ?? {modules:[]};
    if(mode!=='managed' && options.permissions) {
        const config=path.join(dir,'capsid.json');
        fs.writeFileSync(config,JSON.stringify({apiVersion:'capsid/app-v1',entry:path.basename(bundle),permissions,pool:{minReady:workers,maxWorkers:workers},...(options.worker?{worker:options.worker}:{})}));
        args.push('--capsid-json',config);
    }
    if(mode==='managed') {
        port=await unusedPort();
        const applicationsRoot=path.join(dir,'apps'),stateRoot=path.join(dir,'state'),runRoot=path.join(dir,'run'),secrets=path.join(dir,'secrets');
        for(const root of [applicationsRoot,stateRoot,runRoot,path.join(secrets,'orders')]) fs.mkdirSync(root,{recursive:true,mode:0o700});
        const version=path.join(applicationsRoot,'orders','v1');fs.mkdirSync(version,{recursive:true,mode:0o700});
        fs.copyFileSync(bundle,path.join(version,'bundle.mjs'));
        fs.writeFileSync(path.join(version,'capsid.json'),JSON.stringify({apiVersion:'capsid/app-v1',entry:'bundle.mjs',permissions,pool:{minReady:workers,maxWorkers:workers},
            ...(options.worker?{worker:options.worker}:{}),
            ...(options.requestTimeoutMs ? {request:{timeout:`${options.requestTimeoutMs}ms`}} : {})}));
        socketPath=path.join(runRoot,'admin.sock');
        const config=path.join(dir,'host.json');
        fs.writeFileSync(config,JSON.stringify({apiVersion:'capsid/host-v1',applicationsRoot,stateRoot,
            secretRootTemplate:path.join(secrets,'{application}'),admin:{unix:socketPath,mode:'0600'},
            permissions:{modules:[],environmentNames:[],fsReadRoots:[],fetchTargets:options.fetchTargets ?? [],storageNamespaces:[],stdioStreams:[]},
            isolation:{mode:'strict',required:[]},capacity:{workersTotal:workers,startupsConcurrent:1,...options.hostCapacity,...capacity},
            ...(options.channels?{channels:options.channels}:{}),
            ...(options.recovery?{recovery:options.recovery}:{}),
            listeners:[{name:'public',tcp:`127.0.0.1:${port}`,publicScheme:'http',publicAuthority:'public.example',trusted:false,routing:{mode:'path'},
                limits:{...(options.listenerChannels===undefined?{}:{channels:options.listenerChannels}),...(options.listenerChannelQueueBytes===undefined?{}:{channelQueueBytes:options.listenerChannelQueueBytes})}}]}));
        args=['--mode','managed','--host-config',config,'--worker',worker];
    }
    if(mode!=='managed') args.push('--stop-fd','4');
    const child=spawn(host,args,{stdio:mode==='managed'?['ignore','pipe','pipe','pipe']:['ignore','pipe','pipe','pipe','pipe'],env:{...process.env,...options.env}});
    childClosures.set(child,new Promise(resolve=>child.once('close',resolve)));
    if(mode!=='managed') {
        const control={stream:child.stdio[4],error:null};
        control.stream.on('error',error=>{control.error=error;});
        parentControls.set(child,control);
    }
    let stderr='';child.stderr.setEncoding('utf8');child.stderr.on('data',c=>stderr+=c);
    try {
        if(mode==='managed') {
            const deadline=Date.now()+20000;
            await waitForAdminListener(child,socketPath,deadline,()=>stderr);
            const deploy=await admin(socketPath,'/v1/deploy',{app:'orders',version:'v1'});
            assert.equal(deploy.status,202,JSON.stringify(deploy.json));
            for(;;) {
                const operation=await admin(socketPath,`/v1/operations/${deploy.json.operationId}`);
                assert.notEqual(operation.json.state,'failed',`${JSON.stringify(operation.json)} ${stderr}`);
                if(operation.json.state==='active') break;
                assert.ok(Date.now()<deadline,`deploy deadline: ${stderr}`);await delay(25);
            }
        } else {
            const line=await new Promise((resolve,reject)=>{
                let buffer='';const timer=setTimeout(()=>reject(new Error(`ready timeout: ${stderr}`)),15000);
                child.stdio[3].setEncoding('utf8');child.stdio[3].on('data',chunk=>{buffer+=chunk;if(buffer.includes('\n')){clearTimeout(timer);resolve(buffer.split('\n')[0]);}});
                child.once('error',reject);child.once('exit',()=>{clearTimeout(timer);reject(new Error(`host exited: ${stderr}`));});
            });
            const ready=JSON.parse(line);assert.equal(ready.schema,'capsid-host-ready-v1');port=ready.port;
        }
        return {child,port,stderr:()=>stderr,socketPath,admin:(target,body,method)=>admin(socketPath,target,body,method)};
    } catch(error) {
        let shutdown,cleanupFailure;
        try {shutdown=await stopCapsid(child);} catch(cleanupError) {cleanupFailure=cleanupError;}
        const message=`host startup (${mode}): ${error.message}\n${stderr}`;
        const failure=cleanupFailure
            ?new AggregateError([error,cleanupFailure],`${message}; cleanup: ${cleanupFailure.message}`,{cause:error})
            :new Error(message,{cause:error});
        if(error.code!==undefined)failure.code=error.code;
        // Callers have no backend on this path. Attach the evidence here, after
        // cleanup, so their existing failureObservation capture can retain it.
        failure.observation={phase:'capsid-startup',mode,pid:child.pid??null,
            adminRequest:error.adminRequest??null,stderr,shutdown:shutdown??null,
            failure:{name:error.name,message:error.message,code:error.code??null,
                stack:error.stack??null,causeStack:error.cause?.stack??null},
            cleanupFailure:cleanupFailure?.stack??null};
        throw failure;
    }
}
export function stopCapsid(child) {
    if(!stopRequests.has(child)) stopRequests.set(child,stopOnce(child));
    return stopRequests.get(child);
}
async function stopOnce(child) {
    if(child.exitCode!==null || child.signalCode!==null) {
        await childClosures.get(child);
        return {code:child.exitCode,signal:child.signalCode,forced:false};
    }
    // ChildProcess exit precedes closure of its stdio. Returning only after
    // close also makes it safe for the driver itself to finish on Windows.
    const exited=childClosures.get(child) ?? new Promise(resolve=>child.once('close',resolve));
    const control=parentControls.get(child);
    if(control) control.stream.end(Buffer.from([1]));
    else child.kill('SIGTERM');
    let timer,forced=false;
    await Promise.race([exited,new Promise(resolve=>{timer=setTimeout(()=>{forced=true;child.kill('SIGKILL');resolve();},15000);})]);
    clearTimeout(timer);await exited;
    if(control?.error && child.exitCode!==0) throw new Error('parent stop control failed', {cause:control.error});
    return {code:child.exitCode,signal:child.signalCode,forced};
}
