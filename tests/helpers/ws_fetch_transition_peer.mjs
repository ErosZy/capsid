import assert from 'node:assert/strict';
import net from 'node:net';
import tls from 'node:tls';
import {createHash} from 'node:crypto';

export async function boundaryPeer(kind,challenge,certificates,{bodyPrefix=Buffer.from('first')}={}){
    const records=[],tcpRecords=[],sockets=new Set(),listeners=[],sni=[],tlsErrors=[];let targetPort,sourcePort;
    const releases=[];
    const ipv6=kind.startsWith('tls-ipv6-'),redirect=kind.startsWith('redirect-');
    const sourceTLS=kind.startsWith('tls-')||['redirect-https-to-http','redirect-https-to-https','redirect-https-denied','redirect-held-abort'].includes(kind);
    const targetTLS=['redirect-http-to-https','redirect-https-to-https','redirect-http-to-https-wrong-host'].includes(kind);
    const sourceHost=ipv6?'[::1]':sourceTLS?'localhost':'127.0.0.1';
    const targetHost=kind==='redirect-https-denied'?'127.0.0.2':kind==='redirect-http-to-https-wrong-host'?'127.0.0.1':targetTLS?'localhost':'127.0.0.1';
    async function listen(role,host,port=0){
        const isTls=(role==='source'?sourceTLS:targetTLS)&&kind!=='tls-handshake-abort';
        const key=kind==='tls-untrusted'?certificates.untrustedKey:certificates.key;
        const certificate=kind==='tls-ipv6-wrong-host'?certificates.ipCert:certificates.cert;
        const options=isTls?{allowHalfOpen:true,key,cert:certificate,
            SNICallback(servername,done){sni.push({role,servername});done(null,tls.createSecureContext({key,cert:ipv6&&kind!=='tls-ipv6-wrong-host'&&['::1','[::1]'].includes(servername)?certificates.ipv6Cert:certificate}));}}:{allowHalfOpen:true};
        const connect=socket=>{
            sockets.add(socket);socket.on('close',()=>sockets.delete(socket));
            const item={role,localAddress:socket.localAddress,localPort:socket.localPort,remoteAddress:socket.remoteAddress,remotePort:socket.remotePort,
                servername:isTls?socket.servername:null,request:null,rawRequest:null,responseHeader:null,responseStatus:null,
                frames:[],sentFrames:[],receivedChunks:[],sentChunks:[],events:[],peerEnded:false,peerReset:false,fixtureEnded:false,fixtureDestroyed:false,error:null};
            records.push(item);let bytes=Buffer.alloc(0),upgraded=false,partial=null;
            const write=(data,end=false)=>{const bytes=Buffer.from(data);item.sentChunks.push(bytes.toString('base64'));if(end)socket.end(bytes);else socket.write(bytes);};
            socket.on('end',()=>{item.peerEnded=true;item.events.push('peer-ended');socket.end();});
            socket.on('error',error=>{if(error.code==='ECONNRESET')item.peerReset=true;else item.error={code:error.code??null,message:error.message};});
            const send=(opcode,payload)=>{
                const data=Buffer.from(payload);let head;
                if(data.length<126)head=Buffer.from([128|opcode,data.length]);
                else if(data.length<=65535){head=Buffer.alloc(4);head[0]=128|opcode;head[1]=126;head.writeUInt16BE(data.length,2);}
                else{head=Buffer.alloc(10);head[0]=128|opcode;head[1]=127;head.writeBigUInt64BE(BigInt(data.length),2);}
                item.sentFrames.push({opcode,payload:data.toString('base64')});write(Buffer.concat([head,data]));
            };
            const http=(status,reason,fields,body,end=true)=>{
                item.responseStatus=status;item.responseHeader=`HTTP/1.1 ${status} ${reason}\r\n${fields}X-Probe: ${challenge}\r\nConnection: close`;
                write(item.responseHeader+'\r\n\r\n');if(body.length)write(body);
                item.events.push('response');if(end){item.fixtureEnded=true;socket.end();}
            };
            socket.on('data',data=>{item.receivedChunks.push(data.toString('base64'));if(kind==='tls-handshake-abort')return;try{
                bytes=Buffer.concat([bytes,data]);
                if(!upgraded){
                    const end=bytes.indexOf('\r\n\r\n');if(end<0)return;assert.equal(item.request,null,'one actual request');
                    item.request=bytes.subarray(0,end).toString('latin1');item.rawRequest=bytes.subarray(0,end+4).toString('base64');bytes=bytes.subarray(end+4);item.events.push('request');
                    const lines=item.request.split('\r\n'),route=new URL(lines[0].split(' ')[1],'http://fixture.local');
                    const headers=new Map(lines.slice(1).map(line=>{const at=line.indexOf(':');assert.ok(at>0);return [line.slice(0,at).toLowerCase(),line.slice(at+1).trim()];}));
                    assert.equal(lines[0].split(' ')[0],'GET');assert.equal(headers.get('upgrade')?.toLowerCase(),'websocket');
                    assert.ok(headers.get('connection')?.toLowerCase().split(/\s*,\s*/).includes('upgrade'));
                    assert.equal(headers.get('sec-websocket-version'),'13');assert.match(headers.get('sec-websocket-key')??'',/^[A-Za-z0-9+/]{22}==$/);
                    assert.equal(route.searchParams.get('challenge'),challenge);
                    if(redirect&&role==='source'){
                        const hop=Number(route.searchParams.get('hop')??0);
                        const location=`${targetTLS?'https':'http'}://${targetHost}:${targetPort}/land?challenge=${challenge}`;
                        const held=kind==='redirect-held-abort';http(302,'Found',`Location: ${location}\r\nContent-Length: ${held?100:0}\r\n`,held?Buffer.from('first'):Buffer.alloc(0),!held);
                        if(held)releases.push(()=>{if(socket.destroyed||item.peerEnded||item.peerReset){item.releaseAfterPeerEnd=true;return;}item.events.push('release-body');item.fixtureEnded=true;write(Buffer.alloc(95,46),true);});
                        return;
                    }
                    if(kind==='tls-upgrade-abort-pending'){item.events.push('headers-held');return;}
                    if(kind==='tls-body-reader-probe-eof'){http(200,'OK',`Content-Length: ${bodyPrefix.length}\r\n`,bodyPrefix);return;}
                    if(kind.startsWith('tls-body-')){http(409,'Conflict','Content-Length: 100\r\n',bodyPrefix,false);return;}
                    if(kind==='tls-http'||kind==='body-eof'){
                        const body=Buffer.from('body/'+challenge);http(409,'Conflict',kind==='body-eof'?'':`Content-Length: ${body.length}\r\n`,body);return;
                    }
                    if(kind==='body-empty'){http(200,'OK','Content-Length: 0\r\n',Buffer.alloc(0));return;}
                    if(['body-204','body-205','body-304'].includes(kind)){
                        const status=Number(kind.slice(5)),reason={204:'No Content',205:'Reset Content',304:'Not Modified'}[status];
                        http(status,reason,status===204?'':'Content-Length: 0\r\n',Buffer.alloc(0));return;
                    }
                    if(kind.startsWith('body-truncated')||kind.startsWith('body-invalid-chunk')){
                        const chunked=kind.startsWith('body-invalid-chunk');
                        if(kind.endsWith('-coalesced')){
                            item.responseStatus=409;item.responseHeader=`HTTP/1.1 409 Conflict\r\n${chunked?'Transfer-Encoding: chunked':'Content-Length: 20'}\r\nX-Probe: ${challenge}\r\nConnection: close`;
                            item.events.push('response','coalesced-finish');item.fixtureEnded=true;write(item.responseHeader+'\r\n\r\n'+(chunked?'5\r\nfirst\r\nZ\r\nlate\r\n':'first'),true);return;
                        }
                        http(409,'Conflict',chunked?'Transfer-Encoding: chunked\r\n':'Content-Length: 20\r\n',Buffer.from(chunked?'5\r\nfirst\r\n':'first'),false);
                        releases.push(()=>{item.events.push('release-body');item.fixtureEnded=true;write(chunked?'Z\r\nlate\r\n':'',true);});return;
                    }
                    const accept=createHash('sha1').update(headers.get('sec-websocket-key')+'258EAFA5-E914-47DA-95CA-C5AB0DC85B11').digest('base64');
                    item.responseStatus=101;item.responseHeader=`HTTP/1.1 101 Switching Protocols\r\nConnection: Upgrade\r\nUpgrade: websocket\r\nSec-WebSocket-Accept: ${accept}\r\nX-Probe: ${challenge}\r\nSet-Cookie: first=1\r\nSet-Cookie: second=2`;
                    write(item.responseHeader+'\r\n\r\n');upgraded=true;item.events.push('upgrade');
                }
                while(bytes.length>=2){
                    const opcode=bytes[0]&15,fin=!!(bytes[0]&128);assert.equal(bytes[0]&0x70,0);assert.equal(bytes[1]&128,128);
                    let length=bytes[1]&127,offset=2;
                    if(length===126){if(bytes.length<4)return;length=bytes.readUInt16BE(2);offset=4;assert.ok(length>=126);}
                    else if(length===127){if(bytes.length<10)return;const wide=bytes.readBigUInt64BE(2);assert.ok(wide>65535n&&wide<=1048576n);length=Number(wide);offset=10;}
                    if(bytes.length<offset+4+length)return;const mask=bytes.subarray(offset,offset+4),payload=Buffer.from(bytes.subarray(offset+4,offset+4+length));
                    for(let i=0;i<payload.length;i++)payload[i]^=mask[i%4];bytes=bytes.subarray(offset+4+length);
                    item.frames.push({opcode,fin,masked:true,mask:mask.toString('hex'),payload:payload.toString('base64')});
                    if(opcode===8){assert.equal(fin,true);assert.ok(payload.length<=125);send(8,payload);item.fixtureEnded=true;item.events.push('close-reply');socket.end();}
                    else if(opcode===9)send(10,payload);
                    else{if(opcode!==0){assert.ok([1,2].includes(opcode));assert.equal(partial,null);partial={opcode,chunks:[]};}assert.ok(partial);partial.chunks.push(payload);if(fin){send(partial.opcode,Buffer.concat(partial.chunks));partial=null;}}
                }
            }catch(error){item.error={code:null,message:error.stack};item.fixtureDestroyed=true;socket.destroy();}});
        };
        const listener=isTls?tls.createServer(options,connect):net.createServer(options,connect);listeners.push(listener);
        listener.on('connection',socket=>{
            sockets.add(socket);const item={role,localAddress:socket.localAddress,localPort:socket.localPort,remoteAddress:socket.remoteAddress,remotePort:socket.remotePort,ended:false,closed:false,error:null};tcpRecords.push(item);
            socket.on('end',()=>item.ended=true);socket.on('close',()=>{item.closed=true;sockets.delete(socket);});socket.on('error',error=>item.error={code:error.code??null,message:error.message});
        });
        if(isTls)listener.on('tlsClientError',(error,socket)=>{const observed={code:error.code??null,message:error.message,remotePort:socket.remotePort??null};tlsErrors.push(observed);const item=tcpRecords.find(item=>item.remotePort===socket.remotePort);if(item)item.tlsError=observed;});
        await new Promise((resolve,reject)=>{listener.once('error',reject);listener.listen({host,port,ipv6Only:host==='::1'},()=>{listener.removeListener('error',reject);resolve();});});return listener.address().port;
    }
    try{
        sourcePort=await listen('source',ipv6?'::1':'127.0.0.1');
        if(sourceTLS&&!ipv6)await listen('source','::1',sourcePort);
        if(redirect){targetPort=await listen('target',targetTLS?'127.0.0.1':targetHost);if(targetTLS)await listen('target','::1',targetPort);}
    }catch(error){for(const socket of sockets)socket.destroy();await Promise.all(listeners.map(server=>new Promise(resolve=>server.close(resolve))));throw error;}
    return {target:`${sourceTLS?'https':'http'}://${sourceHost}:${sourcePort}/probe?challenge=${challenge}`,
        sourcePort,targetPort,authority:sourceHost+':'+sourcePort,
        allow:[sourceHost+':'+sourcePort,...(redirect&&kind!=='redirect-https-denied'?[targetHost+':'+targetPort]:[])],
        snapshot:()=>structuredClone({records,tcpRecords,sni,tlsErrors}),release:()=>{assert.ok(releases.length);for(const release of releases)release();},
        stop:async()=>{for(const socket of sockets)socket.destroy();await Promise.all(listeners.map(server=>new Promise(resolve=>server.close(resolve))));}};
}
