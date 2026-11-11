import assert from 'node:assert/strict';
import fs from 'node:fs';
import {createHash} from 'node:crypto';
import {spawn} from 'node:child_process';
import {fileURLToPath} from 'node:url';
import {bindingQuotaInputPaths, bindingQuotaVariants} from '../tools/binding-quota-input.mjs';
import {validateBindingQuotaReport} from '../tools/binding-quota-contract.mjs';

const root=fileURLToPath(new URL('../',import.meta.url)),args=new Map();
for(let index=2;index<process.argv.length;index+=2){
    const key=process.argv[index],value=process.argv[index+1];
    assert.ok(['--helper','--worker','--kind'].includes(key)&&value&&!args.has(key));args.set(key,value);
}
assert.equal(args.size,3);const helper=args.get('--helper'),worker=args.get('--worker'),kind=args.get('--kind');
assert.ok(['request','worker'].includes(kind));
const hash=file=>createHash('sha256').update(fs.readFileSync(file)).digest('hex');
const inputHashes=()=>Object.fromEntries(Object.entries(bindingQuotaInputPaths).map(([key,name])=>[key,hash(root+name)]));
const binaryHashes=()=>({helper:hash(helper),worker:hash(worker)});
const expected={runId:process.env.CAPSID_WS_RUN_ID??'standalone-binding-quota',kind,platform:process.platform,inputs:inputHashes(),binaries:binaryHashes()};
const report={schema:'capsid-binding-quota-v1',complete:false,...expected,observations:[]};let destination;
if(process.env.CAPSID_WS_REPORT_DIR){
    fs.mkdirSync(process.env.CAPSID_WS_REPORT_DIR,{recursive:true});destination=process.env.CAPSID_WS_REPORT_DIR+'/binding-quota-'+kind+'.json';assert.equal(fs.existsSync(destination),false);
}
const save=()=>{if(destination)fs.writeFileSync(destination,JSON.stringify(report,null,2)+'\n');};save();

for(const variant of kind==='request'?bindingQuotaVariants:['worker']){
    const observation=kind==='request'?{variant,stdout:'',stderr:''}:{stdout:'',stderr:'',responses:[],logs:[],events:[]};report.observations.push(observation);
    const command=kind==='request'
        ? [worker,root+bindingQuotaInputPaths.requestBinding,root+bindingQuotaInputPaths.requestUser,'http://127.0.0.1:1',variant]
        : [worker,root+bindingQuotaInputPaths.workerBinding,root+bindingQuotaInputPaths.workerUser];
    const child=spawn(helper,command,{stdio:['pipe','pipe','pipe']});let pending='',timer,parseError;
    child.stdin.on('error',error=>parseError??=String(error));
    child.stdout.setEncoding('utf8');child.stdout.on('data',chunk=>{
        observation.stdout+=chunk;pending+=chunk;
        for(;;){
            const end=pending.indexOf('\n');if(end<0)break;
            const line=pending.slice(0,end).replace(/\r$/,'');pending=pending.slice(end+1);
            try{
                if(kind==='request'){
                    if(line.startsWith('BINDING_PHASE 1 ')){
                        assert.equal(observation.request1,undefined);observation.request1=JSON.parse(line.slice('BINDING_PHASE 1 '.length));child.stdin.write('continue\n');
                    }else if(line.startsWith('BINDING_PHASE 2 ')){
                        assert.equal(observation.request2,undefined);observation.request2=JSON.parse(line.slice('BINDING_PHASE 2 '.length));child.stdin.end('finish\n');
                    }else if(line.startsWith('BINDING_EXIT ')){
                        assert.equal(observation.workerExit,undefined);observation.workerExit=JSON.parse(line.slice('BINDING_EXIT '.length));
                    }
                }else if(line.startsWith('QUOTA_RESPONSE ')){
                    const match=/^QUOTA_RESPONSE (\d+) (.*)$/.exec(line);assert.ok(match);
                    const request=Number(match[1]);observation.responses.push({request,value:JSON.parse(match[2])});observation.events.push({type:'response',request});
                    if(request===23)child.stdin.end('finish\n');
                }else if(line.startsWith('QUOTA_LOG ')){
                    const match=/^QUOTA_LOG (\d+) ([0-9a-f]*)$/.exec(line);assert.ok(match&&match[2].length%2===0);
                    observation.logs.push({request:Number(match[1]),raw:Buffer.from(match[2],'hex').toString('utf8')});observation.events.push({type:'log',index:observation.logs.length-1});
                }else if(line.startsWith('QUOTA_REQUEST ')){
                    const match=/^QUOTA_REQUEST (\d+) (\w+) (\w+)$/.exec(line);assert.ok(match);
                    observation.events.push({type:'request',request:Number(match[1]),step:match[2],variant:match[3]});
                }else if(line.startsWith('QUOTA_CANCEL ')){
                    const match=/^QUOTA_CANCEL (\d+)$/.exec(line);assert.ok(match);observation.events.push({type:'cancel',request:Number(match[1])});
                }else if(line.startsWith('QUOTA_CANCELED_EVENT ')){
                    const match=/^QUOTA_CANCELED_EVENT (\d+) (\d+)$/.exec(line);assert.ok(match);observation.events.push({type:'canceled-event',request:Number(match[1]),eventType:Number(match[2])});
                }else if(line.startsWith('QUOTA_EXIT ')){
                    assert.equal(observation.workerExit,undefined);observation.workerExit=JSON.parse(line.slice('QUOTA_EXIT '.length));
                }
            }catch(error){parseError??=String(error.stack??error);}
        }
    });
    child.stderr.on('data',chunk=>observation.stderr+=chunk);
    const closed=new Promise((resolve,reject)=>{child.once('error',reject);child.once('close',(code,signal)=>resolve({code,signal}));});
    try{
        observation.child=await Promise.race([closed,new Promise((_,reject)=>{timer=setTimeout(()=>reject(new Error('original45s Binding helper deadline')),45000);})]);
        assert.equal(parseError,undefined);assert.equal(pending,'');assert.deepEqual(observation.child,{code:0,signal:null});
        assert.deepEqual(observation.workerExit,{reaped:true,channelClosed:true,code:0});assert.equal(observation.stderr,'');
    }finally{
        clearTimeout(timer);
        if(child.exitCode===null&&child.signalCode===null){report.complete=false;observation.helperForced=true;child.kill();await closed;}
        if(parseError)observation.parseError=parseError;save();
    }
}
assert.deepEqual(inputHashes(),expected.inputs);assert.deepEqual(binaryHashes(),expected.binaries);
report.complete=true;
try{validateBindingQuotaReport(report,expected);}catch(error){report.complete=false;throw error;}finally{save();}
console.log(JSON.stringify({complete:true,kind,observations:report.observations.length,actualUserRequests:kind==='request'?8:23,
    completedStructuredRpcCalls:kind==='request'?12:19,canceledStructuredRpcCalls:kind==='request'?252:1088,
    quotaRefusalsBeforeClone:kind==='request'?4:5,naturalWorkerExits:kind==='request'?4:1}));
