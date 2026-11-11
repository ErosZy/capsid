import assert from 'node:assert/strict';
import fs from 'node:fs';
import net from 'node:net';
import {spawn} from 'node:child_process';
import {createHash} from 'node:crypto';
import {fileURLToPath} from 'node:url';
import {setTimeout as delay} from 'node:timers/promises';
import {cancelBindingKinds,cancelBindingRoutes,cancelBindingViews,cancelBindingMethods,cancelBindingPacket,cancelBindingInputPaths} from '../tools/binding-fetch-cancel-input.mjs';
import {assertCancelBindingCase,validateCancelBindingReport} from '../tools/binding-fetch-cancel-contract.mjs';
const root=fileURLToPath(new URL('../',import.meta.url)),args=new Map();
for(let index=2;index<process.argv.length;index+=2){const key=process.argv[index],value=process.argv[index+1];assert.ok(['--helper','--worker','--kind','--route'].includes(key)&&value&&!args.has(key));args.set(key,value);}
assert.equal(args.size,4);const helper=args.get('--helper'),worker=args.get('--worker'),kind=args.get('--kind'),route=args.get('--route');assert.ok(cancelBindingKinds.includes(kind)&&cancelBindingRoutes.includes(route));
const binding=root+cancelBindingInputPaths.binding,user=root+cancelBindingInputPaths.user,hash=file=>createHash('sha256').update(fs.readFileSync(file)).digest('hex');
const expected={runId:process.env.CAPSID_WS_RUN_ID??'standalone-binding-cancel',kind,route,inputs:Object.fromEntries(Object.entries(cancelBindingInputPaths).map(([key,name])=>[key,hash(root+name)])),binaries:{helper:hash(helper),worker:hash(worker)}};
const report={schema:'capsid-binding-fetch-cancel-v1',complete:false,runId:expected.runId,kind,route,inputs:expected.inputs,binaries:expected.binaries,cases:[]};let destination;
if(process.env.CAPSID_WS_REPORT_DIR){fs.mkdirSync(process.env.CAPSID_WS_REPORT_DIR,{recursive:true});destination=process.env.CAPSID_WS_REPORT_DIR+'/binding-fetch-cancel-'+kind+'-'+route+'.json';assert.equal(fs.existsSync(destination),false);}
const save=()=>{if(destination)fs.writeFileSync(destination,JSON.stringify(report,null,2)+'\n');};save();
for(const view of cancelBindingViews)for(const method of cancelBindingMethods){
    const item={kind,route,view,method,events:[]};report.cases.push(item);const packet=cancelBindingPacket(kind),split=packet.indexOf('\r\n\r\n')+4;assert.ok(split>4);
    const sources=[],sockets=new Set(),sourceSockets=new Map();let sequence=0,resolvePhase2,rejectPhase2;
    const event=name=>item.events.push({name,sequence:sequence++});
    const server=net.createServer({allowHalfOpen:true},socket=>{
        const source={path:null,request:null,rawRequest:'',peerEnded:false,peerReset:false,headersSent:false,bodySent:false,fixtureEnded:false,fixtureDestroyed:false,events:[]};sources.push(source);sockets.add(socket);sourceSockets.set(source,socket);let input=Buffer.alloc(0);
        const sourceEvent=name=>source.events.push({name,sequence:sequence++});
        socket.on('error',error=>{source.peerReset=error.code==='ECONNRESET';source.error=error.code;sourceEvent('peer-error');});
        socket.on('end',()=>{source.peerEnded=true;sourceEvent('peer-ended');if(source.path==='/pending')socket.end();});
        socket.on('close',()=>sockets.delete(socket));
        socket.on('data',chunk=>{input=Buffer.concat([input,chunk]);source.rawRequest=input.toString('base64');if(source.request!==null)return;const end=input.indexOf('\r\n\r\n');if(end<0)return;
            source.request=input.subarray(0,end).toString();source.path=source.request.split(' ')[1];assert.ok(['/body','/pending'].includes(source.path));sourceEvent('request');
            source.headersSent=true;source.sentHeader=packet.subarray(0,split).toString('base64');sourceEvent('headers-only');socket.write(packet.subarray(0,split));
        });
    });
    await new Promise((resolve,reject)=>{server.once('error',reject);server.listen(0,'127.0.0.1',resolve);});item.port=server.address().port;
    const child=spawn(helper,[worker,binding,user,'http://127.0.0.1:'+item.port,[route,view,method].join(',')],{stdio:['pipe','pipe','pipe']});let stdout='',stderr='',pending='',timer;
    const phase2=new Promise((resolve,reject)=>{resolvePhase2=resolve;rejectPhase2=reject;});child.stdin.on('error',error=>{item.controlError=error.code;});
    const continueAfterCanceledSource=async()=>{
        const end=performance.now()+3000;const canceled=()=>sources.find(source=>source.path==='/pending');
        while(!(canceled()?.peerEnded||canceled()?.peerReset)&&performance.now()<end)await delay(5);
        item.beforeRequest2=structuredClone(sources);item.cancelSampleSequence=sequence++;
        assert.equal(sources.length,2);const pool=sources.find(source=>source.path==='/body'),aborted=canceled();
        assert.ok(pool.headersSent&&!pool.bodySent&&!pool.peerEnded&&!pool.peerReset&&!pool.fixtureEnded,'completed-call pool must survive request1');
        assert.ok(aborted.headersSent&&!aborted.bodySent&&!aborted.fixtureEnded&&(aborted.peerEnded||aborted.peerReset),'pending call I/O closes before request2 without body input');
        event('continue-request2');child.stdin.write('continue\n');
    };
    child.stdout.setEncoding('utf8');child.stdout.on('data',chunk=>{stdout+=chunk;pending+=chunk;
        for(;;){const end=pending.indexOf('\n');if(end<0)break;const line=pending.slice(0,end).replace(/\r$/,'');pending=pending.slice(end+1);
            try{
                if(line==='BINDING_REQUEST 1')event('request1-begun');
                else if(line.startsWith('BINDING_PHASE 1 ')){
                    assert.equal(item.request1,undefined);item.request1=JSON.parse(line.slice('BINDING_PHASE 1 '.length));event('request1-ended');item.afterRequest1=structuredClone(sources);item.request1SampleSequence=sequence++;
                    continueAfterCanceledSource().catch(rejectPhase2);
                }else if(line==='BINDING_REQUEST 2'){
                    assert.ok(item.beforeRequest2);event('request2-begun');assert.equal(sources.length,2);
                    const source=sources.find(source=>source.path==='/body');assert.ok(!source.bodySent&&!source.peerEnded&&!source.peerReset);source.bodySent=true;source.fixtureEnded=true;source.sent=packet.toString('base64');source.events.push({name:'body-after-request2',sequence:sequence++});sourceSockets.get(source).end(packet.subarray(split));
                }else if(line.startsWith('BINDING_PHASE 2 ')){
                    assert.equal(item.request2,undefined);item.request2=JSON.parse(line.slice('BINDING_PHASE 2 '.length));event('request2-ended');resolvePhase2();
                }else if(line.startsWith('BINDING_EXIT '))item.workerExit=JSON.parse(line.slice('BINDING_EXIT '.length));
            }catch(error){rejectPhase2(error);}
        }
    });child.stderr.on('data',chunk=>stderr+=chunk);
    const closed=new Promise((resolve,reject)=>{child.once('error',reject);child.once('close',(code,signal)=>resolve({code,signal}));});
    const deadline=new Promise((_,reject)=>{timer=setTimeout(()=>reject(new Error('original45s retained helper deadline')),45000);});
    try{
        await Promise.race([phase2,closed.then(()=>assert.ok(item.request2,'second actual response before exit: '+stderr)),deadline]);
        const end=performance.now()+3000;while(!sources.every(source=>source.peerEnded||source.peerReset)&&performance.now()<end)await delay(5);item.beforeStop=structuredClone(sources);item.finalSampleSequence=sequence++;event('finish');child.stdin.end('finish\n');item.child=await Promise.race([closed,deadline]);
    }catch(error){item.failure=String(error.stack??error);throw error;}
    finally{
        clearTimeout(timer);if(child.exitCode===null&&child.signalCode===null){item.helperForced=true;child.kill();await closed;}
        item.stdout=stdout;item.stderr=stderr;item.finalSources=structuredClone(sources);for(const socket of sockets)socket.destroy();await new Promise(resolve=>server.close(resolve));save();
    }
    assertCancelBindingCase(item,kind,route,view,method);
    console.log(JSON.stringify({kind,route,view,method,complete:true,requests:2,rpcCalls:10,completedCalls:9,canceledCalls:1,poolSourceLiveAfterFirstRequest:true,pendingSourceEndedBeforeSecondRequest:true}));
}
assert.deepEqual(Object.fromEntries(Object.entries(cancelBindingInputPaths).map(([key,name])=>[key,hash(root+name)])),expected.inputs);assert.deepEqual({helper:hash(helper),worker:hash(worker)},expected.binaries);report.complete=true;save();validateCancelBindingReport(report,expected);console.log(JSON.stringify({complete:true,kind,route,inputs:26,requests:52,completedCalls:234,canceledCalls:26}));
