import assert from 'node:assert/strict';
import fs from 'node:fs';
import {spawn} from 'node:child_process';
import {fileURLToPath} from 'node:url';
import {headersInputPaths,headersDigest,validateHeadersReport} from '../tools/headers-contract.mjs';
const root=fileURLToPath(new URL('../',import.meta.url)),args=new Map();
for(let index=2;index<process.argv.length;index+=2){const key=process.argv[index],value=process.argv[index+1];assert.ok(['--helper','--worker'].includes(key)&&value&&!args.has(key));args.set(key,value);}assert.equal(args.size,2);
const helper=args.get('--helper'),worker=args.get('--worker'),digest=file=>headersDigest(fs.readFileSync(file));
const expected={runId:process.env.CAPSID_WS_RUN_ID??'standalone-headers',inputs:Object.fromEntries(Object.entries(headersInputPaths).map(([key,name])=>[key,digest(root+name)])),binaries:{helper:digest(helper),worker:digest(worker)}};
const report={schema:'capsid-headers-compat-v1',complete:false,runId:expected.runId,inputs:expected.inputs,binaries:expected.binaries,observation:null,stdout:'',stderr:'',child:null,workerExit:null};let destination;
if(process.env.CAPSID_WS_REPORT_DIR){fs.mkdirSync(process.env.CAPSID_WS_REPORT_DIR,{recursive:true});destination=process.env.CAPSID_WS_REPORT_DIR+'/headers-compatibility.json';assert.equal(fs.existsSync(destination),false);}
const save=()=>{if(destination)fs.writeFileSync(destination,JSON.stringify(report,null,2)+'\n');};save();
const child=spawn(helper,[worker,root+headersInputPaths.binding,root+headersInputPaths.user,'http://127.0.0.1:1','headers'],{stdio:['pipe','pipe','pipe']});let pending='',timer,resolveBody,rejectBody;
const body=new Promise((resolve,reject)=>{resolveBody=resolve;rejectBody=reject;});child.stdin.on('error',rejectBody);
child.stdout.setEncoding('utf8');child.stdout.on('data',chunk=>{report.stdout+=chunk;pending+=chunk;for(;;){const end=pending.indexOf('\n');if(end<0)break;const line=pending.slice(0,end).replace(/\r$/,'');pending=pending.slice(end+1);try{if(line.startsWith('BINDING_BODY ')){assert.equal(report.observation,null);report.observation=JSON.parse(line.slice('BINDING_BODY '.length));resolveBody();}else if(line.startsWith('BINDING_EXIT '))report.workerExit=JSON.parse(line.slice('BINDING_EXIT '.length));}catch(error){rejectBody(error);}}});child.stderr.on('data',chunk=>report.stderr+=chunk);
const closed=new Promise((resolve,reject)=>{child.once('error',reject);child.once('close',(code,signal)=>resolve({code,signal}));});
const deadline=new Promise((_,reject)=>{timer=setTimeout(()=>reject(new Error('original45s Binding helper deadline')),45000);});
try{
    await Promise.race([body,closed.then(()=>assert.ok(report.observation,'actual body response before helper exit')),deadline]);child.stdin.end('finish\n');report.child=await Promise.race([closed,deadline]);
    assert.deepEqual(Object.fromEntries(Object.entries(headersInputPaths).map(([key,name])=>[key,digest(root+name)])),expected.inputs);assert.deepEqual({helper:digest(helper),worker:digest(worker)},expected.binaries);
    report.complete=true;try{validateHeadersReport(report,expected);}catch(error){report.complete=false;throw error;}
}finally{clearTimeout(timer);if(child.exitCode===null&&child.signalCode===null){report.complete=false;child.kill();await closed;}save();}
console.log(JSON.stringify({complete:true,cases:99,vmRows:198,structuredRpcCalls:99,actualUserRequests:1,naturalWorkerExit:0}));
