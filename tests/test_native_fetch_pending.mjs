import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import {spawn} from 'node:child_process';
import {createHash} from 'node:crypto';
import {setTimeout as delay} from 'node:timers/promises';
import {fileURLToPath} from 'node:url';
import {nativeFetchPendingInputPaths,nativeFetchPendingCasesForShard} from '../tools/native-fetch-pending-input.mjs';
import {openPendingSource} from '../tools/native-fetch-pending-peer.mjs';
import {assertNativeFetchPendingCase,validateNativeFetchPendingReport} from '../tools/native-fetch-pending-contract.mjs';
const root=fileURLToPath(new URL('../',import.meta.url)),hash=file=>createHash('sha256').update(fs.readFileSync(file)).digest('hex');
const args=new Map();for(let i=2;i<process.argv.length;i+=2){assert.ok(['--helper','--worker','--shard'].includes(process.argv[i])&&process.argv[i+1]&&!args.has(process.argv[i]));args.set(process.argv[i],process.argv[i+1]);}assert.equal(args.size,3);
const helper=args.get('--helper'),worker=args.get('--worker'),shard=args.get('--shard'),plans=nativeFetchPendingCasesForShard(shard),fixture=path.join(root,nativeFetchPendingInputPaths.fixture);
const inputs=()=>Object.fromEntries(Object.entries(nativeFetchPendingInputPaths).map(([key,name])=>[key,hash(path.join(root,name))]));
const expected={shard,runId:process.env.CAPSID_WS_RUN_ID??'standalone-native-pending',inputs:inputs(),binaries:{helper:hash(helper),worker:hash(worker)}};
const report={schema:'capsid-native-fetch-pending-v1',complete:false,...expected,cases:[]};let destination;
if(process.env.CAPSID_WS_REPORT_DIR){fs.mkdirSync(process.env.CAPSID_WS_REPORT_DIR,{recursive:true});destination=path.join(process.env.CAPSID_WS_REPORT_DIR,'native-fetch-pending-'+shard+'.json');assert.equal(fs.existsSync(destination),false,'fresh actual pending-read observations');}
const save=()=>{if(destination)fs.writeFileSync(destination,JSON.stringify(report,null,2)+'\n');};save();
for(const plan of plans){
    const start=performance.now(),source=await openPendingSource(plan),row={plan,collectionComplete:false,timings:{}};report.cases.push(row);
    const child=spawn(helper,[worker,fixture,source.target,'0','8&route='+plan.route+'&api='+plan.api+'&action='+plan.action],{stdio:['pipe','pipe','pipe']});let stdout='',stderr='',resolveState,timer;
    const ready=new Promise(resolve=>{resolveState=resolve;});child.stdout.on('data',v=>{stdout+=v;const line=stdout.split(/\r?\n/).find(v=>v.startsWith('LIMIT_PROBE '));if(line)try{row.state=JSON.parse(line.slice(12));resolveState();}catch{}});child.stderr.on('data',v=>{stderr+=v;});child.stdin.on('error',()=>{});
    const closed=new Promise((resolve,reject)=>{child.once('close',(code,signal)=>resolve({code,signal}));child.once('error',reject);});const bound=new Promise((_,reject)=>{timer=setTimeout(()=>reject(new Error('native pending peer12s deadline')),12000);});let failure;
    try{
        await Promise.race([ready,closed.then(()=>{assert.ok(row.state,'helper produced actual pending-read state');}),bound]);row.timings.stateMs=performance.now()-start;row.sourceTerminalBeforeFinish=await source.terminal();row.timings.terminalMs=performance.now()-start;row.sourceBeforeFinish=structuredClone(source.data);assert.equal(row.sourceTerminalBeforeFinish,true,'both peers terminal before parent finish');
        child.stdin.end('finish\n');row.shutdown=await Promise.race([closed,bound]);row.stdout=stdout;row.stderr=stderr;const exits=stdout.split(/\r?\n/).filter(v=>v.startsWith('LIMIT_EXIT '));assert.equal(exits.length,1);row.workerExit=JSON.parse(exits[0].slice(11));row.sourceAfterFinish=structuredClone(source.data);row.collectionComplete=true;
    }catch(error){failure=error;row.collectionFailure=String(error.stack??error);row.sourceFailureSnapshot=structuredClone(source.data);}
    finally{
        clearTimeout(timer);if(child.exitCode===null&&child.signalCode===null){row.helperForced=true;child.kill();row.shutdown??=await Promise.race([closed.catch(()=>null),delay(2000).then(()=>null)]);}row.stdout??=stdout;row.stderr??=stderr;
        await source.close();row.timings.completeMs=performance.now()-start;save();console.log('NATIVE_PENDING_ROW '+JSON.stringify(row));
    }
    if(failure)throw failure;assertNativeFetchPendingCase(row,plan);
}
assert.deepEqual(inputs(),expected.inputs);assert.deepEqual({helper:hash(helper),worker:hash(worker)},expected.binaries);report.complete=true;save();validateNativeFetchPendingReport(report,expected);console.log(JSON.stringify({complete:true,shard,cases:plans.length,allSourceBodiesGated:true,allPeerTerminalsBeforeCleanup:true,allWorkerExitsNatural:true}));
