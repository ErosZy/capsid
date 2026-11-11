import net from 'node:net';
import {setTimeout as delay} from 'node:timers/promises';
import {nativeFetchPendingReply} from './native-fetch-pending-input.mjs';
export async function openPendingSource(plan) {
    const data = {plan, records: [], controlBeforeBody: false, bodyReleased: false, failure: null};
    const bytes=nativeFetchPendingReply(plan),sockets=new Map();let source;
    const accept=socket=>{
        socket.setNoDelay(true);const record={role:null,rawRequest:'',request:null,peerEnded:false,peerReset:false,fixtureEnded:false,fixtureDestroyed:false,sent:[],events:['accepted']};data.records.push(record);sockets.set(socket,record);let request=Buffer.alloc(0);
        socket.on('end',()=>{record.peerEnded=true;record.events.push('peer-ended');});socket.on('close',()=>sockets.delete(socket));
        socket.on('error',error=>{if(error.code==='ECONNRESET')record.peerReset=true;else record.error=error.code;});
        socket.on('data',chunk=>{
            request=Buffer.concat([request,chunk]);record.rawRequest=request.toString('base64');if(record.request!==null)return;const end=request.indexOf('\r\n\r\n');if(end<0)return;
            record.request=request.subarray(0,end).toString();record.events.push('request');const requestUrl=new URL(record.request.split(' ')[1],'http://localhost');
            const send=(value,end=false)=>{record.sent.push(value.toString('base64'));record.events.push(end?'sent-end':'sent-head');if(end){record.fixtureEnded=true;socket.end(value);}else socket.write(value);};
            if(requestUrl.pathname==='/hold'){
                record.role='source';if(source){data.failure='duplicate source';socket.destroy();return;}source={record,send};send(bytes.head);
            }else if(requestUrl.pathname==='/control'){
                record.role='control';data.controlBeforeBody=Boolean(source)&&!data.bodyReleased&&source.record.sent.length===1&&!source.record.fixtureEnded;
                try{data.phase=JSON.parse(requestUrl.searchParams.get('phase'));}catch(error){data.failure=String(error);}
                send(Buffer.from('HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok'),true);
                if(plan.action==='overlap'&&source){data.bodyReleased=true;source.send(bytes.encoded,true);}
            }else{data.failure='unknown request path';socket.destroy();}
        });
    };
    const server=net.createServer({allowHalfOpen:true},accept),server6=net.createServer({allowHalfOpen:true},accept);
    await new Promise((resolve,reject)=>{server.once('error',reject);server.listen(0,'127.0.0.1',resolve);});data.port=server.address().port;
    await new Promise((resolve,reject)=>{server6.once('error',reject);server6.listen({port:data.port,host:'::1',ipv6Only:true},resolve);});
    return {data,target:'http://localhost:'+data.port+'/hold',async terminal(){
        const end=performance.now()+3000;while((data.records.length!==2||data.records.some(r=>!r.peerEnded&&!r.peerReset))&&performance.now()<end)await delay(5);
        return data.records.length===2&&data.records.every(r=>r.peerEnded||r.peerReset);
    },async close(){for(const [socket,record] of sockets){record.fixtureDestroyed=true;socket.destroy();}await Promise.all([server,server6].map(listener=>new Promise(resolve=>listener.close(resolve))));}};
}
