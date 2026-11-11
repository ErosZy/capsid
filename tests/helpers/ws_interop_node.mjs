import assert from 'node:assert/strict';
import {createHash} from 'node:crypto';
import fs from 'node:fs';
import {loadNodeWS} from './ws_interop_node_sdk.mjs';

const {WebSocket,identity}=loadNodeWS();
if(process.argv[2]==='identity')console.log(JSON.stringify(identity));
else {
const config=JSON.parse(process.argv[2]);
assert.notEqual(process.env.NODE_TLS_REJECT_UNAUTHORIZED,'0','TLS verification is enabled');
const summarize=data=>{
    const bytes=Buffer.from(data);
    return {type:typeof data==='string'?'text':'binary',length:bytes.length,sha256:createHash('sha256').update(bytes).digest('hex')};
};
async function run() {
    const ws=new WebSocket(config.uri,['chat','super'],{perMessageDeflate:false,
        ...(config.uri.startsWith('wss:')?{ca:fs.readFileSync(config.ca),rejectUnauthorized:true}:{})});
    ws.binaryType='arraybuffer';
    const queue=[],waiters=[],errors=[];
    let opened=false,openResolve,openReject,closeResolve,timer;
    const open=new Promise((resolve,reject)=>{openResolve=resolve;openReject=reject;});open.catch(()=>{});
    const close=new Promise(resolve=>{closeResolve=resolve;});
    ws.onopen=()=>{opened=true;openResolve();};
    ws.onmessage=event=>{
        const data=typeof event.data==='string'?event.data:Buffer.from(event.data);
        if(waiters.length)waiters.shift().resolve(data);else queue.push(data);
    };
    ws.onerror=event=>{
        errors.push({name:event.error?.name??null,message:event.error?.message??null,code:event.error?.code??null});
        if(config.operation!=='tls-reject')console.error('Node ws error:',event.error);
    };
    ws.onclose=event=>{
        closeResolve({code:event.code,reason:event.reason,clean:event.wasClean});
        if(!opened)openReject(new Error('closed before open'));
        for(const waiter of waiters.splice(0))waiter.reject(new Error('closed before expected message'));
    };
    const result={identity,uri:config.uri,case:config.case,messages:[],pingPong:0};
    const next=()=>queue.length?Promise.resolve(queue.shift()):new Promise((resolve,reject)=>waiters.push({resolve,reject}));
    const exercise=async()=>{
        if(config.operation==='tls-reject') {
            const terminal=await close;result.failure={opened,errors,close:terminal};return;
        }
        await open;assert.equal(ws.protocol,'chat');assert.equal(ws.extensions,'');result.protocol=ws.protocol;
        if(config.operation==='server-close') {
            ws.send('__close_server__');result.close=await close;
        }else {
            const large=Buffer.alloc(180000);for(let i=0;i<large.length;i++)large[i]=(i*17+29)%251;
            for(const message of ['challenge:'+config.challenge,'hello\0界😀',Buffer.alloc(0),large]) {
                ws.send(message);const echoed=await next();assert.equal(typeof echoed,typeof message);
                assert.deepEqual(echoed,message,'independent Node ws client receives exact bytes');result.messages.push(summarize(echoed));
            }
            ws.close(1000,'client\0终');result.close=await close;
        }
        assert.deepEqual(errors,[],'successful Node exchange has no error events');assert.deepEqual(queue,[],'no extra messages');
    };
    try {await Promise.race([exercise(),new Promise((_,reject)=>{timer=setTimeout(()=>reject(new Error('Node ws interop deadline')),6000);})]);return result;}
    finally {clearTimeout(timer);if(ws.readyState<2)ws.close();}
}
try {console.log(JSON.stringify(await run()));}catch(error){console.error(error);process.exitCode=1;}
}
