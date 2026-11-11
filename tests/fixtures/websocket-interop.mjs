function describe(data) {
    if(typeof data==='string')return {type:'text',text:data};
    const bytes=new Uint8Array(data);
    return {type:'binary',length:bytes.length,first:[...bytes.subarray(0,8)],last:[...bytes.subarray(-8)]};
}
function outbound(target) {
    return new Promise(resolve=>{
        const ws=new WebSocket(target,['chat','super']);ws.binaryType='arraybuffer';
        const events=[{type:'construct',state:ws.readyState}];
        ws.onopen=()=>events.push({type:'open',state:ws.readyState,protocol:ws.protocol});
        ws.onmessage=event=>{events.push({type:'message',data:describe(event.data)});ws.send(event.data);};
        ws.onerror=event=>events.push({type:'error',state:ws.readyState,errorName:event.error?.name,isError:event.error instanceof Error});
        ws.onclose=event=>{events.push({type:'close',state:ws.readyState,code:event.code,reason:event.reason,clean:event.wasClean});resolve(events);};
    });
}
export default {async fetch(request) {
    const url=new URL(request.url);
    if(url.pathname.endsWith('/outbound'))return Response.json({target:url.searchParams.get('target'),events:await outbound(url.searchParams.get('target'))});
    if(!url.pathname.endsWith('/inbound'))return new Response('healthy');
    const pair=new WebSocketPair(),server=pair[1];server.accept();server.binaryType='arraybuffer';
    server.onmessage=event=>{
        if(event.data==='__close_server__')server.close(3001,'server\0终');
        else server.send(event.data);
    };
    return new Response(null,{status:101,webSocket:pair[0],headers:{'sec-websocket-protocol':'chat','x-interop':'yes'}});
}};
