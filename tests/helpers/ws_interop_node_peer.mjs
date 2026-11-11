import assert from 'node:assert/strict';
import http from 'node:http';
import https from 'node:https';
import fs from 'node:fs';
import {createHash} from 'node:crypto';
import {loadNodeWS} from './ws_interop_node_sdk.mjs';

const summary=data=>({type:typeof data==='string'?'text':'binary',length:Buffer.byteLength(data),sha256:createHash('sha256').update(data).digest('hex')});
export async function startNodePeer(certs) {
    const {WebSocketServer,identity}=loadNodeWS(),connections=new Set(),tasks=new Set(),records=[],servers=[];
    const listen=async tlsOptions=>{
        const server=tlsOptions?https.createServer(tlsOptions):http.createServer();
        server.on('tlsClientError',()=>{});
        const wss=new WebSocketServer({server,perMessageDeflate:false,maxPayload:1024*1024,
            handleProtocols:protocols=>protocols.has('chat')?'chat':false});
        wss.on('connection',(ws,request)=>{
            connections.add(ws);ws.on('error',()=>{});
            const url=new URL(request.url,'http://localhost'),challenge=url.searchParams.get('challenge');
            const record={case:url.searchParams.get('case'),protocol:ws.protocol,extensions:ws.extensions,
                messages:[],pingPong:0,sni:request.socket.servername??null};records.push(record);
            const queue=[],waiters=[];let closeResolve;
            const closed=new Promise(resolve=>{closeResolve=resolve;});
            ws.on('message',(data,binary)=>{
                const value=binary?Buffer.from(data):data.toString();
                if(waiters.length)waiters.shift().resolve(value);else queue.push(value);
            });
            ws.onclose=event=>{
                connections.delete(ws);closeResolve({code:event.code,reason:event.reason,clean:event.wasClean});
                for(const pending of waiters.splice(0))pending.reject(new Error('peer closed before expected echo'));
            };
            const next=()=>queue.length?Promise.resolve(queue.shift()):new Promise((resolve,reject)=>waiters.push({resolve,reject}));
            const send=(data,options)=>new Promise((resolve,reject)=>ws.send(data,options,error=>error?reject(error):resolve()));
            const exercise=async()=>{
                assert.equal(ws.protocol,'chat');assert.equal(ws.extensions,'');
                const large=Buffer.alloc(180000);for(let i=0;i<large.length;i++)large[i]=(i*17+29)%251;
                for(const value of ['challenge:'+challenge,'hello\0界😀',Buffer.alloc(0),large]) {
                    await send(value);const echoed=await next();assert.equal(typeof echoed,typeof value);assert.deepEqual(echoed,value,'Node ws peer exact worker echo');record.messages.push(summary(echoed));
                }
                for(const [value,text] of [[Buffer.from('分片\0😀'),true],[Buffer.from(Array.from({length:60240},(_,i)=>i%251)),false]]) {
                    await send(value.subarray(0,1),{binary:!text,fin:false});
                    const pong=new Promise(resolve=>ws.once('pong',resolve));ws.ping('peer-fragment');
                    assert.equal((await pong).toString(),'peer-fragment');record.pingPong++;
                    await send(value.subarray(1,4),{binary:!text,fin:false});await send(value.subarray(4),{binary:!text,fin:true});
                    const echoed=await next(),expected=text?value.toString():value;assert.equal(typeof echoed,typeof expected);assert.deepEqual(echoed,expected,'Node ws peer fragmented echo');record.messages.push(summary(echoed));
                }
                ws.close(1000,'remote\0终');record.close=await closed;assert.deepEqual(queue,[],'no extra worker echo');
            };
            let timer;
            const task=Promise.race([exercise(),new Promise((_,reject)=>{timer=setTimeout(()=>reject(new Error('Node ws peer deadline')),6000);})])
                .catch(error=>{record.failure={name:error.name,message:error.message};ws.terminate();})
                .finally(()=>{clearTimeout(timer);tasks.delete(task);});tasks.add(task);
        });
        await new Promise(resolve=>server.listen(0,'127.0.0.1',resolve));servers.push({server,wss});return server.address().port;
    };
    const ports={ws:await listen(),wss:await listen({key:fs.readFileSync(certs.key),cert:fs.readFileSync(certs.cert)}),
        untrusted:await listen({key:fs.readFileSync(certs.untrustedKey),cert:fs.readFileSync(certs.untrustedCert)})};
    return {identity,ports,async stop(force=false){
        if(force)for(const ws of connections)ws.terminate();
        await Promise.all([...tasks]);
        for(const {server,wss} of servers){await new Promise(resolve=>wss.close(resolve));await new Promise(resolve=>server.close(resolve));}
        return {identity,active:connections.size,records};
    }};
}
