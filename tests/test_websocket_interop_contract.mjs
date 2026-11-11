import assert from 'node:assert/strict';
import {test} from 'node:test';
import {requiredTests} from '../tools/run-workerd-ws-gate.mjs';
import fs from 'node:fs';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';
import {createHash} from 'node:crypto';
import {execFile,spawnSync} from 'node:child_process';
import {promisify} from 'node:util';
import {fileURLToPath} from 'node:url';
import {validateInteropReport,compareInteropReports} from '../tools/workerd-interop-contract.mjs';
import {prepareMutationRunner, requiredMutationCases} from '../tools/workerd-mutation-contract.mjs';
import {referencePlatform, referenceArchitecture, referenceBackendModes, referenceLockName} from '../tools/workerd-platform.mjs';
import {readLock, verifyBinary} from '../tools/workerd-reference.mjs';

const root=path.resolve(path.dirname(fileURLToPath(import.meta.url)),'..');
const sample=()=>JSON.parse(fs.readFileSync(path.join(root,'tests/fixtures/websocket-interop-report.json'),'utf8')).report;
const expected=r=>({runId:r.runId,lock:structuredClone(r.referenceLock),clientLock:structuredClone(r.clientLock),
    inputs:structuredClone(r.inputs),host:'a'.repeat(64),worker:'b'.repeat(64)});
const execute=promisify(execFile);
const python=process.env.CAPSID_WS_INTEROP_PYTHON??'python3';

for(const autocrlf of ['false','true']) test(`fixture checkout preserves LF and CRLF bytes with core.autocrlf=${autocrlf}`,()=>{
    const temporaryRoot=fs.realpathSync(os.tmpdir());
    const directory=fs.mkdtempSync(path.join(temporaryRoot,'capsid-fixture-checkout-'));
    assert.equal(path.dirname(fs.realpathSync(directory)),temporaryRoot);
    const git=(args,input)=>{
        const result=spawnSync('git',['-C',directory,...args],
            {input,encoding:'utf8',timeout:10000,windowsHide:true});
        assert.ifError(result.error);
        assert.equal(result.status,0,result.stderr);
        return result.stdout.trim();
    };
    try {
        git(['init','--quiet']);
        const fixtures=['tests/fixtures/workerd-ws/api-compat.mjs','tests/fixtures/workerd-crypto.mjs'];
        const originals=fixtures.map(file=>fs.readFileSync(path.join(root,file)));
        assert.ok(!originals[0].includes(Buffer.from('\r\n')),'original LF reference fixture');
        assert.ok(originals[1].includes(Buffer.from('\r\n')),'original CRLF reference fixture');
        for(const [index,file] of fixtures.entries()) {
            const blob=git(['hash-object','--no-filters','-w','--stdin'],originals[index]);
            git(['update-index','--add','--cacheinfo',`100644,${blob},${file}`]);
        }
        const attributes=git(['hash-object','--no-filters','-w','--stdin'],fs.readFileSync(path.join(root,'.gitattributes')));
        git(['update-index','--add','--cacheinfo',`100644,${attributes},.gitattributes`]);
        const tree=git(['write-tree']);
        const commit=git(['-c','user.name=Fixture Control','-c','user.email=fixture@example.invalid','commit-tree',tree],
            'Preserve original reference fixture blobs\n');
        git(['update-ref','HEAD',commit]);
        git(['-c',`core.autocrlf=${autocrlf}`,'checkout','--force','--quiet','HEAD']);
        for(const [index,file] of fixtures.entries())
            assert.deepEqual(fs.readFileSync(path.join(directory,file)),originals[index],'exact checkout bytes: '+file);
        assert.equal(git(['-c',`core.autocrlf=${autocrlf}`,'status','--porcelain']),'',
            'original fixture checkout must remain clean for Release identity');
    } finally {
        assert.equal(path.dirname(directory),temporaryRoot);
        assert.ok(path.basename(directory).startsWith('capsid-fixture-checkout-'));
        fs.rmSync(directory,{recursive:true,force:true});
    }
});

for (const [platform,arch,asset,filename,binaryHash,assetHash] of [
    ['linux','x64','workerd-linux-64.gz','reference-lock.json',
        '50f31156a9a4c7d549f1b4753dd61eb527e1b6b0cd416e5ba3d02d64dfdee5dd','a4530a03ab3da1e1cb0173887133a720bd48cd236135990d7cbc561c16091530'],
    ['win32','x64','workerd-windows-64.gz','reference-lock-windows.json',
        'c8799724c4221af61dde76ed071aa5996b25c9bd5d247da94c166718cabe612a','8b9377af95e2649bb1d0ea20525ac31769f38c1d85f1b5922a3bbe0856462a82'],
    ['darwin','x64','workerd-darwin-64.gz','reference-lock-darwin-x64.json',
        '9be36f0b9e867ffe081415beebf075f8fd2d5c27bc0a9e239ded1375bd09f27f','77df22f80e26e34e4159b8869317001cdd6ea3ec2516476ba50d7b34eef74b6a'],
    ['darwin','arm64','workerd-darwin-arm64.gz','reference-lock-darwin-arm64.json',
        '2995714d79e9af6a0810eb7705a1f994b8ed977a266cb8976662027107761401','5b2b304b2d59b1f870b2115a1ffbfdd4d1729f8e671704ed8732166a21c7583b'],
]) test(`${platform}/${arch} binds the fixed source profile and native asset`,()=>{
    assert.equal(referenceLockName(platform,arch),filename);
    const lock=readLock(path.join(root,'tests/fixtures/workerd-ws',filename));
    const linux=readLock(path.join(root,'tests/fixtures/workerd-ws/reference-lock.json'));
    assert.equal(lock.release_asset,asset);
    assert.equal(lock.binary_sha256,binaryHash,'reviewed official executable bytes');
    assert.equal(lock.release_asset_sha256,assetHash,'reviewed official compressed asset');
    assert.equal(referencePlatform(lock),platform);
    assert.equal(referenceArchitecture(lock),arch);
    assert.deepEqual(referenceBackendModes(lock),platform==='linux'?['single-worker','static-pool','managed']:['single-worker','static-pool']);
    for(const field of ['source_commit','release_tag','binary_version_string','compatibility_date','compatibility_flags','client'])
        assert.deepEqual(lock[field],linux[field],'same fixed source profile '+field);
    if(platform!==process.platform)
        assert.throws(()=>verifyBinary('must-not-be-opened',lock),/locked workerd executable platform/);
    else if(arch!==process.arch)
        assert.throws(()=>verifyBinary('must-not-be-opened',lock),/locked workerd executable architecture/);
});

test('unsupported reference assets cannot borrow a supported platform or architecture',()=>{
    for(const asset of ['workerd-darwin-ppc.gz','workerd-linux-arm64.gz','workerd-darwin-64.gz.extra']) {
        assert.throws(()=>referencePlatform({release_asset:asset}),/unsupported locked workerd platform asset/);
        assert.throws(()=>referenceArchitecture({release_asset:asset}),/unsupported locked workerd platform asset/);
    }
});

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
