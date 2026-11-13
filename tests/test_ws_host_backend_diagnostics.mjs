import assert from 'node:assert/strict';
import {test} from 'node:test';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import {startCapsid,stopCapsid} from './helpers/ws_host_backend.mjs';

// This host exercises the actual startup transport and process cleanup. It
// supplies no product or workerd conformance evidence.
const fakeHost=`#!${process.execPath}
import fs from 'node:fs';
import http from 'node:http';
const config=JSON.parse(fs.readFileSync(process.argv[process.argv.indexOf('--host-config')+1]));
const fault=process.env.CAPSID_TEST_ADMIN_FAULT;
const server=http.createServer((request,response)=>{
    request.resume();
    request.on('end',()=>{
        if((fault==='deploy-reset'&&request.url==='/v1/deploy')||
            (fault==='operation-reset'&&request.url.startsWith('/v1/operations/'))){
            request.socket.destroy();return;
        }
        if(fault==='invalid-json'&&request.url==='/v1/deploy'){
            response.writeHead(202,{'Connection':'close'});response.end('invalid JSON');return;
        }
        if(fault==='truncated-response'&&request.url.startsWith('/v1/operations/')){
            response.writeHead(200,{'Content-Length':'1000','Connection':'close'});
            response.flushHeaders();response.write('{');
            setImmediate(()=>response.destroy());return;
        }
        response.writeHead(request.url==='/v1/deploy'?202:200,{'Connection':'close'});
        response.end(JSON.stringify(request.url==='/v1/deploy'?{operationId:'operation-1'}:{state:'active'}));
    });
});
process.on('SIGTERM',()=>{
    process.stderr.write('diagnostic host stopped\\n');
    server.close(()=>process.exit(0));
});
server.listen(config.admin.unix,()=>process.stderr.write('diagnostic host ready\\n'));
`;

async function fixture(operation) {
    const directory=fs.mkdtempSync(path.join(os.tmpdir(),'capsid-startup-'));
    try {
        const host=path.join(directory,'fake-host.mjs'),bundle=path.join(directory,'bundle.mjs');
        fs.writeFileSync(host,fakeHost,{mode:0o700});fs.writeFileSync(bundle,'export default {};');
        await operation({directory,host,bundle});
    } finally {fs.rmSync(directory,{recursive:true,force:true});}
}

for(const [fault,method,target] of [
    ['deploy-reset','POST','/v1/deploy'],
    ['operation-reset','GET','/v1/operations/operation-1'],
    ['invalid-json','POST','/v1/deploy'],
    ['truncated-response','GET','/v1/operations/operation-1'],
]) {
    test(`managed startup retains ${fault} request, stderr and natural cleanup`,
        {skip:process.platform==='win32',timeout:10000},async()=>fixture(async({directory,host,bundle})=>{
            await assert.rejects(startCapsid(host,'unused-worker',bundle,directory,'managed',
                {env:{CAPSID_TEST_ADMIN_FAULT:fault}}),error=>{
                assert.ok(error.cause instanceof Error);
                assert.ok(error.message.includes(`admin ${method} ${target}`));
                if(fault.endsWith('reset'))assert.equal(error.code,'ECONNRESET');
                const observation=error.observation;
                assert.equal(observation.phase,'capsid-startup');
                assert.deepEqual(observation.adminRequest,{method,target});
                assert.ok(observation.failure.stack.includes(`admin ${method} ${target}`));
                assert.equal(typeof observation.failure.causeStack,'string');
                if(fault.endsWith('reset'))assert.equal(observation.failure.code,'ECONNRESET');
                assert.match(observation.stderr,/diagnostic host ready/);
                assert.match(observation.stderr,/diagnostic host stopped/);
                assert.deepEqual(observation.shutdown,{code:0,signal:null,forced:false});
                assert.equal(observation.cleanupFailure,null);
                assert.deepEqual(JSON.parse(JSON.stringify(observation)),observation);
                assert.throws(()=>process.kill(observation.pid,0),{code:'ESRCH'});
                return true;
            });
        }));
}

test('healthy managed startup still deploys and stops normally',
    {skip:process.platform==='win32',timeout:10000},async()=>fixture(async({directory,host,bundle})=>{
        const backend=await startCapsid(host,'unused-worker',bundle,directory,'managed',
            {env:{CAPSID_TEST_ADMIN_FAULT:'healthy'}});
        try {assert.ok(backend.port>0);assert.equal((await backend.admin('/v1/operations/operation-1')).json.state,'active');}
        finally {assert.deepEqual(await stopCapsid(backend.child),{code:0,signal:null,forced:false});}
    }));

test('invalid readiness retains the nonzero child exit after its stop channel closes',
    {skip:process.platform==='win32',timeout:10000},async()=>fixture(async({directory,host,bundle})=>{
        fs.writeFileSync(host,`#!${process.execPath}
import fs from 'node:fs';
fs.closeSync(4);
setTimeout(()=>{
    fs.writeSync(3,'invalid readiness JSON\\n');
    setTimeout(()=>process.exit(7),50);
},50);
`,{mode:0o700});
        await assert.rejects(startCapsid(host,'unused-worker',bundle,directory),error=>{
            assert.ok(error.cause instanceof SyntaxError);
            assert.equal(error.observation.mode,'single-worker');
            assert.deepEqual(error.observation.shutdown,{code:7,signal:null,forced:false});
            assert.equal(error.observation.cleanupFailure,null);
            assert.throws(()=>process.kill(error.observation.pid,0),{code:'ESRCH'});
            return true;
        });
    }));
