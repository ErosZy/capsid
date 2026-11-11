import assert from 'node:assert/strict';
import {test} from 'node:test';
import {requiredTests} from '../tools/run-workerd-ws-gate.mjs';
import fs from 'node:fs';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';
import {createHash} from 'node:crypto';
import {execFile} from 'node:child_process';
import {promisify} from 'node:util';
import {fileURLToPath} from 'node:url';
import {validateInteropReport,compareInteropReports} from '../tools/workerd-interop-contract.mjs';
import {prepareMutationRunner, requiredMutationCases} from '../tools/workerd-mutation-contract.mjs';

const root=path.resolve(path.dirname(fileURLToPath(import.meta.url)),'..');
const sample=()=>JSON.parse(fs.readFileSync(path.join(root,'tests/fixtures/websocket-interop-report.json'),'utf8')).report;
const expected=r=>({runId:r.runId,lock:structuredClone(r.referenceLock),clientLock:structuredClone(r.clientLock),
    inputs:structuredClone(r.inputs),host:'a'.repeat(64),worker:'b'.repeat(64)});
const execute=promisify(execFile);
const python=process.env.CAPSID_WS_INTEROP_PYTHON??'python3';

test('mutation source line endings preserve every independent fault scope',()=>{
    const source=fs.readFileSync(path.join(root,'tests/test_workerd_ws_fixtures.mjs'),'utf8').replace(/\r\n/g,'\n');
    const crlf=source.replace(/\n/g,'\r\n');
    assert.notEqual(source,crlf,'the CRLF control must change actual input bytes');
    for(const {name} of requiredMutationCases)
        assert.equal(prepareMutationRunner(crlf,name),prepareMutationRunner(source,name),name+' has the same generated program');
});

test('mutation handler block validation remains strict for both line endings',()=>{
    const source=fs.readFileSync(path.join(root,'tests/test_workerd_ws_fixtures.mjs'),'utf8').replace(/\r\n/g,'\n');
    const block='observation.handlerErrors = await observeHandlerErrors(port, routePrefix, assert);\n        assertHandlerErrorDiagnostics(observation.handlerErrors, server.stderr(), assert);';
    assert.equal(source.split(block).length,2);
    for(const eol of ['\n','\r\n']) {
        const serialize=value=>value.replace(/\n/g,eol);
        assert.throws(()=>prepareMutationRunner(serialize(source.replace(block,'')),'wire-code'),/single complete handler block/);
        assert.throws(()=>prepareMutationRunner(serialize(source.replace(block,block+'\n'+block)),'wire-code'),/single complete handler block/);
    }
});

test('required gate executes the independent-client matrix on all four backends',()=>{
    assert.deepEqual(requiredTests.filter(name=>name.startsWith('websocket_interop_')).sort(),[
        'websocket_interop_managed','websocket_interop_reference',
        'websocket_interop_single-worker','websocket_interop_static-pool',
    ]);
});

test('real reference sample passes without modifying raw observations',()=>{
    const report=sample(),before=structuredClone(report);
    assert.equal(validateInteropReport(report,expected(report)).cases.length,22);
    assert.deepEqual(report,before);
});
test('four-backend comparison includes both clients and independent protocol peer',()=>{
    const reference=sample(),e=expected(reference),candidates=['single-worker','static-pool','managed'].map(mode=>({
        ...structuredClone(reference),mode,binaries:{host:e.host,worker:e.worker},shutdown:{code:0,signal:null,forced:false},
    }));
    assert.deepEqual(compareInteropReports(reference,candidates,e),{complete:true,casesPerBackend:22,backends:4,clients:['node-ws','python-websockets'],peerSessionsPerBackend:{node:2,python:2}});
});
for(const [name,mutate] of [
    ['missing Python cases',r=>{r.cases=r.cases.filter(x=>!x.name.startsWith('python/'));}],
    ['duplicate case',r=>{r.cases[1]=structuredClone(r.cases[0]);}],
    ['extra case',r=>{r.cases.push(structuredClone(r.cases[0]));}],
    ['partial report',r=>{r.complete=false;}],
    ['stale run',r=>{r.runId='old';}],
    ['wrong Node version',r=>{r.clients.node.node='v0.0.0';}],
    ['Python replaced by Node',r=>{r.clients.python=structuredClone(r.clients.node);}],
    ['modified Python implementation',r=>{r.clients.python.sourceSHA256='0'.repeat(64);}],
    ['wrong dependency wheel lock',r=>{r.clientLock.websockets.wheel.sha256='0'.repeat(64);}],
    ['unbound worker',r=>{r.binaries.host='0'.repeat(64);}],
    ['wrong Python program',r=>{r.inputs.pythonProgram='0'.repeat(64);}],
    ['wrong reference flags',r=>{r.referenceLock.compatibility_flags=['other'];}],
    ['empty observations',r=>{r.cases[0].observed={};}],
    ['corrupt large echo',r=>{r.cases[0].observed.messages[3].sha256='0'.repeat(64);}],
    ['binary changes to text',r=>{r.cases[0].observed.messages[2].type='text';}],
    ['missing empty binary',r=>{r.cases[0].observed.messages.splice(2,1);}],
    ['missing fragment Pong',r=>{r.cases.find(x=>x.name.endsWith('fragment-ping')).observed.pingPong=1;}],
    ['server Close changes code',r=>{r.cases[1].observed.close.code=1000;}],
    ['fake clean close',r=>{r.cases[0].observed.close.clean=false;}],
    ['modified Node ws implementation',r=>{r.clients.node.sourceSHA256='0'.repeat(64);}],
    ['missing certificate attempt',r=>{r.cases.find(x=>x.name==='python/tls/untrusted').tcpAccepted=0;}],
    ['generic Python connection error',r=>{r.cases.find(x=>x.name==='python/tls/untrusted').observed.failure.errorName='ConnectionRefusedError';}],
    ['wrong Python verification reason',r=>{r.cases.find(x=>x.name==='python/tls/wrongHost').observed.failure.verifyCode=18;}],
    ['Node generic failure instead of verification',r=>{r.cases.find(x=>x.name==='node/tls/untrusted').observed.failure.errors[0].code='ECONNREFUSED';}],
    ['TLS unexpectedly opened',r=>{r.cases.find(x=>x.name==='node/tls/untrusted').observed.failure.opened=true;}],
    ['wrong Node ws TLS error payload',r=>{r.cases.find(x=>x.name==='node/tls/untrusted').observed.failure.errors[0].name='TypeError';}],
    ['TLS failure reaches WebSocket handler',r=>{r.peers.python.records.push(structuredClone(r.peers.python.records[0]));}],
    ['native outbound data lost',r=>{r.cases.find(x=>x.name==='outbound/python/ws').observed.events.splice(2,1);}],
    ['missing independent peer session',r=>{r.peers.python.records.pop();}],
    ['missing Node peer session',r=>{r.peers.node.records.pop();}],
    ['peer receives wrong bytes',r=>{r.peers.python.records[0].messages[3].sha256='0'.repeat(64);}],
    ['wrong outbound SNI',r=>{r.peers.python.records[1].sni=null;}],
    ['unfinished Python peer',r=>{r.peers.python.active=1;}],
    ['killed Python peer',r=>{r.peers.python.signal='SIGKILL';}],
    ['forced backend exit',r=>{r.shutdown.forced=true;}],
    ['worker unhealthy',r=>{r.healthy.status=500;}],
]) test(`independent-client contract rejects ${name}`,()=>{
    const report=sample(),e=expected(report);mutate(report);assert.throws(()=>validateInteropReport(report,e));
});
test('identically missing Python coverage on every backend still fails',()=>{
    const reference=sample(),e=expected(reference),candidates=['single-worker','static-pool','managed'].map(mode=>({
        ...structuredClone(reference),mode,binaries:{host:e.host,worker:e.worker},shutdown:{code:0,signal:null,forced:false},
    }));
    for(const report of [reference,...candidates])report.cases=report.cases.filter(x=>!x.name.startsWith('python/'));
    assert.throws(()=>compareInteropReports(reference,candidates,e),/complete ordered independent-client scenario inventory/);
});

function frame(opcode,data) {
    const body=Buffer.from(data),header=Buffer.alloc(body.length<126?2:body.length<65536?4:10);header[0]=0x80|opcode;
    if(header.length===2)header[1]=body.length;
    else if(header.length===4){header[1]=126;header.writeUInt16BE(body.length,2);}
    else{header[1]=127;header.writeBigUInt64BE(BigInt(body.length),2);}
    return Buffer.concat([header,body]);
}
async function echoPeer(corrupt,action) {
    const sockets=new Set(),errors=[];
    const server=net.createServer(socket=>{
        sockets.add(socket);socket.on('close',()=>sockets.delete(socket));socket.on('error',()=>{});
        let input=Buffer.alloc(0),handshake=false;
        socket.on('data',chunk=>{
            try {
                input=Buffer.concat([input,chunk]);
                if(!handshake) {
                    const end=input.indexOf('\r\n\r\n');if(end<0)return;
                    const head=input.subarray(0,end).toString(),key=/^sec-websocket-key: ([^\r\n]+)/mi.exec(head)?.[1];
                    assert.ok(key);assert.doesNotMatch(head,/sec-websocket-extensions:/i,'actual independent input omits compression');
                    const accept=createHash('sha1').update(key+'258EAFA5-E914-47DA-95CA-C5AB0DC85B11').digest('base64');
                    socket.write('HTTP/1.1 101 Switching Protocols\r\nConnection: Upgrade\r\nUpgrade: websocket\r\nSec-WebSocket-Accept: '+accept+'\r\nSec-WebSocket-Protocol: chat\r\nx-interop: yes\r\n\r\n');
                    input=input.subarray(end+4);handshake=true;
                }
                while(input.length>=2) {
                    assert.ok(input[0]&128);assert.ok(input[1]&128);
                    let size=input[1]&127,offset=2;
                    if(size===126){if(input.length<4)return;size=input.readUInt16BE(2);offset=4;}
                    else if(size===127){if(input.length<10)return;size=Number(input.readBigUInt64BE(2));offset=10;}
                    assert.ok(size<=200000);if(input.length<offset+4+size)return;
                    const opcode=input[0]&15,mask=input.subarray(offset,offset+4),data=Buffer.from(input.subarray(offset+4,offset+4+size));
                    for(let i=0;i<data.length;i++)data[i]^=mask[i%4];input=input.subarray(offset+4+size);
                    if(opcode===8){socket.end(frame(8,data));return;}
                    assert.ok(opcode===1||opcode===2);
                    socket.write(frame(opcode,corrupt?Buffer.concat([Buffer.from('wrong:'),data]):data));
                }
            }catch(error){errors.push(error);socket.destroy();}
        });
    });
    await new Promise(resolve=>server.listen(0,'127.0.0.1',resolve));
    try {await action(`ws://127.0.0.1:${server.address().port}/inbound`);assert.deepEqual(errors,[]);}
    finally {for(const socket of sockets)socket.destroy();await new Promise(resolve=>server.close(resolve));}
}
for(const client of ['node','python'])for(const corrupt of [false,true])test(`real ${client} client ${corrupt?'rejects corrupt':'accepts exact'} peer bytes`,async()=>{
    await echoPeer(corrupt,async uri=>{
        const config={uri,case:'live-control',challenge:'live-control',operation:'exchange',ca:''};
        const command=client==='node'?process.execPath:python;
        const parameters=client==='node'?[path.join(root,'tests/helpers/ws_interop_node.mjs'),JSON.stringify(config)]
            :['-I',path.join(root,'tests/helpers/ws_interop_python.py'),'client',...Object.entries(config).flatMap(([k,v])=>['--'+k,String(v)])];
        try {
            const result=await execute(command,parameters,{timeout:10000,maxBuffer:1024*1024});
            assert.equal(corrupt,false,'corrupted peer must fail');assert.equal(JSON.parse(result.stdout).messages.length,4);
        }catch(error) {
            assert.equal(corrupt,true,`positive client control failed: ${error.stderr??error}`);
            assert.equal(error.code,1);assert.equal(error.killed,false);assert.equal(error.signal,null);
            assert.match(error.stderr,client==='node'?/independent Node ws client receives exact bytes/:/independent client received exact echo/);
        }
    });
});
test('actual modified Python source fails despite an unchanged package version',async()=>{
    const temp=fs.mkdtempSync(path.join(os.tmpdir(),'capsid-interop-identity-'));
    try {
        const location=JSON.parse((await execute(python,['-I','-c','import json,websockets;print(json.dumps(websockets.__file__))'])).stdout);
        const site=path.join(temp,'site'),packagePath=path.join(site,'websockets');fs.mkdirSync(site);
        fs.cpSync(path.dirname(location),packagePath,{recursive:true});fs.appendFileSync(path.join(packagePath,'frames.py'),'\n# isolated identity negative control\n');
        const helper=path.join(temp,'tests/helpers/ws_interop_python.py');fs.mkdirSync(path.dirname(helper),{recursive:true});
        fs.copyFileSync(path.join(root,'tests/helpers/ws_interop_python.py'),helper);fs.mkdirSync(path.join(temp,'tests/fixtures'));
        fs.copyFileSync(path.join(root,'tests/fixtures/websocket-interop-lock.json'),path.join(temp,'tests/fixtures/websocket-interop-lock.json'));
        await assert.rejects(execute(python,['-I','-c',
            'import sys,runpy;sys.path.insert(0,sys.argv[1]);sys.argv=[sys.argv[2],"identity"];runpy.run_path(sys.argv[0],run_name="__main__")',site,helper],{timeout:10000}),
            error=>error.code===1&&!error.killed&&error.signal===null&&/locked Python WebSocket implementation/.test(error.stderr));
    }finally{fs.rmSync(temp,{recursive:true,force:true});}
});
test('actual modified Node ws source fails despite an unchanged package version',async()=>{
    const temp=fs.mkdtempSync(path.join(os.tmpdir(),'capsid-interop-node-identity-'));
    try {
        const source=process.env.CAPSID_WS_INTEROP_NODE_PACKAGE;assert.ok(source,'locked Node ws package is installed');
        const copy=path.join(temp,'ws');fs.cpSync(source,copy,{recursive:true});
        fs.appendFileSync(path.join(copy,'lib/receiver.js'),'\n// isolated identity negative control\n');
        await assert.rejects(execute(process.execPath,[path.join(root,'tests/helpers/ws_interop_node.mjs'),'identity'],
            {timeout:10000,env:{...process.env,CAPSID_WS_INTEROP_NODE_PACKAGE:copy}}),
            error=>error.code===1&&!error.killed&&error.signal===null&&/locked Node ws implementation/.test(error.stderr));
    }finally{fs.rmSync(temp,{recursive:true,force:true});}
});
test('missing Pair feature is a failing fixture run, never a successful skip',async()=>{
    const temp=fs.mkdtempSync(path.join(os.tmpdir(),'capsid-interop-profile-'));
    try {
        fs.cpSync(path.join(root,'tests/fixtures/workerd-ws'),path.join(temp,'fixtures/workerd-ws'),{recursive:true});
        const profile=path.join(temp,'missing-profile.js');fs.writeFileSync(profile,'export default {};\n');
        const source=fs.readFileSync(path.join(root,'tests/test_workerd_ws_fixtures.mjs'),'utf8');
        const original=JSON.stringify(path.join(root,'js/profile-manifest.js'));
        const isolated=prepareMutationRunner(source);assert.equal(isolated.split(original).length,2);
        const driver=path.join(temp,'driver.mjs');fs.writeFileSync(driver,isolated.replace(original,JSON.stringify(profile)));
        await assert.rejects(execute(process.execPath,[driver,'--host','missing-host','--worker','missing-worker'],{timeout:10000}),
            error=>{assert.equal(error.code,1,'a missing required API fails instead of exit 77');assert.match(error.stderr,/required WebSocketPair feature/);return true;});
    }finally{fs.rmSync(temp,{recursive:true,force:true});}
});
