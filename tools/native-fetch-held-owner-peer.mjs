import assert from 'node:assert/strict';
import net from 'node:net';
import {setTimeout as delay} from 'node:timers/promises';
export function createHeldOwnerSource(row,reply,onSource) {
    const sockets=new Set();let source;
    const server=net.createServer({allowHalfOpen:true},socket=>{
        sockets.add(socket);let input=Buffer.alloc(0),handled=false,record;
        socket.on('close',()=>sockets.delete(socket));
        socket.on('end',()=>{if(record){record.peerEnded=true;record.events.push('peer-ended');}});
        socket.on('error',error=>{if(record){if(error.code==='ECONNRESET')record.peerReset=true;else record.error=error.code;}});
        socket.on('data',bytes=>{
            input=Buffer.concat([input,bytes]);if(handled||input.indexOf('\r\n\r\n')<0)return;handled=true;
            const at=input.indexOf('\r\n\r\n'),request=input.subarray(0,at).toString(),first=request.split('\r\n')[0];
            if(first==='GET /hold HTTP/1.1') {
                assert.equal(source,undefined);source=record={request,rawRequest:input.toString('base64'),peerEnded:false,peerReset:false,fixtureEnded:false,fixtureDestroyed:false,events:['request','sent-held-http-chunk'],sent:reply.packet.toString('base64')};
                row.source=source;onSource(source);socket.write(reply.packet);
            } else {
                assert.equal(first,'GET /hold/marker HTTP/1.1');assert.equal(row.marker,undefined);
                row.marker={request,rawRequest:input.toString('base64'),originalBoundMilliseconds:3000};
                (async()=>{
                    const end=performance.now()+3000;
                    while(!source.peerEnded&&performance.now()<end)await delay(5);
                    const state={sourcePeerEnded:source.peerEnded,sourcePeerReset:source.peerReset,fixtureEnded:source.fixtureEnded,fixtureDestroyed:source.fixtureDestroyed};
                    row.marker.sourceBeforeReply=structuredClone(source);row.marker.state=state;
                    const payload=Buffer.from(JSON.stringify(state));
                    socket.end(Buffer.concat([Buffer.from('HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: '+payload.length+'\r\nConnection: close\r\n\r\n'),payload]));
                })().catch(error=>{row.marker.failure=String(error.stack??error);socket.destroy();});
            }
        });
    });
    return {server,sockets};
}
