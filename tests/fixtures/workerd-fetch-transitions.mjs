let realm,current,resolveScopeForeign;
const describe=error=>({type:typeof error,name:error?.name??null,message:error?.message??null,primitive:error===null?'null':typeof error==='object'?null:String(error)});
const digest=async bytes=>Array.from(new Uint8Array(await crypto.subtle.digest('SHA-256',bytes)),value=>value.toString(16).padStart(2,'0')).join('');
const result=response=>({status:response.status,statusText:response.statusText,url:response.url,redirected:response.redirected,
    type:response.type,bodyNull:response.body===null,bodyUsed:response.bodyUsed,headers:[...response.headers],
    cookies:response.headers.getSetCookie(),hasWebSocket:response.webSocket!==null,body:null});
export default {async fetch(request){
    realm??=crypto.randomUUID();const url=new URL(request.url);
    if(url.pathname.endsWith('/state'))return Response.json({realm,state:current??null});
    if(url.pathname.endsWith('/resolve')){resolveScopeForeign?.();return new Response('resolved');}
    const kind=url.searchParams.get('kind'),challenge=url.searchParams.get('challenge');
    if(["http-created-promise", "event-created-promise", "http-created-timer", "http-created-promise-after-close", "http-created-timer-after-close"].includes(kind)){
        const state=current={kind,challenge,timerFired:false,resolverCalled:false,future:false,parentClosed:false};return scopeResponse(url,state);
    }
    if(kind.startsWith('tls-body-reader-probe-')||kind.startsWith('standard-reader-probe')){
        const state=current={kind,challenge,first:null,pending:null,second:null,closed:null,cancel:null,after:null,again:null,trace:[],bodyUsed:null,locked:null,unexpected:null};return readerResponse(url,state);
    }
    const state=current={kind,challenge,trace:[],result:null,firstBody:null,failure:null,sameReason:false,
        socket:null,signal:{aborted:false,reasonIsSentinel:false},action:null,future:false,bodyUsedAtFirst:null,bodyUsedAtEnd:null};
    const sentinel={challenge},controller=new AbortController(),init={headers:{Upgrade:'websocket','X-Probe':challenge},signal:controller.signal};
    if(kind.startsWith('redirect-'))Object.assign(init.headers,{Authorization:'auth/'+challenge,'Proxy-Authorization':'proxy/'+challenge,Cookie:'cookie='+challenge});
    if(kind==='tls-ipv6-proxy'){
        try{const response=await fetch(url.searchParams.get('target'),init);state.result=result(response);state.trace.push('returned');return response;}
        catch(error){state.failure=describe(error);return Response.json({realm,state},{status:500});}
    }
    const pair=new WebSocketPair(),server=pair[1];server.accept();let reader,resolveAction;
    const actionDone=new Promise(resolve=>resolveAction=resolve);
    server.addEventListener('close',()=>state.trace.push('parent-close'));
    server.addEventListener('message',async event=>{
        if(event.data==='abort'){
            state.trace.push('abort');state.action={name:'abort',returned:false,error:null};
            try{controller.abort(sentinel);state.action.returned=true;}catch(error){state.action.error=describe(error);}
            state.signal={aborted:controller.signal.aborted,reasonIsSentinel:controller.signal.reason===sentinel};resolveAction();return;
        }
        if(event.data==='cancel'){
            state.trace.push('cancel');state.action={name:'cancel',returned:false,error:null};
            try{await reader.cancel(sentinel);state.action.returned=true;}catch(error){state.action.error=describe(error);}
            resolveAction();return;
        }
        if(event.data!=='go')return;state.trace.push('fetch');server.send('started');let response;
        try{
            response=await fetch(url.searchParams.get('target'),init);state.result=result(response);state.trace.push('headers');
            if(response.webSocket){
                const socket=response.webSocket;socket.binaryType='arraybuffer';
                const observation=state.socket={before:socket.readyState,messages:[],events:[],closed:null};let resolve;
                const closed=new Promise(done=>resolve=done);
                socket.addEventListener('error',event=>observation.events.push({type:'error',message:event.message,error:String(event.error)}));
                socket.addEventListener('close',event=>{observation.closed={code:event.code,reason:event.reason,wasClean:event.wasClean};observation.events.push({type:'close',...observation.closed});resolve();});
                socket.addEventListener('message',async event=>{
                    if(typeof event.data==='string')observation.messages.push({type:'text',value:event.data});
                    else observation.messages.push({type:'binary',bytes:event.data.byteLength,sha256:await digest(event.data)});
                    if(observation.messages.length===2)socket.close(1000,'child');
                });
                socket.accept();socket.send('echo/'+challenge);socket.send(new Uint8Array([1,2,3,4,5]));await closed;
            }else{
                if(kind==='tls-body-abort-before-read'){
                    server.send('headers-ready');await actionDone;state.result.body={bytes:(await response.arrayBuffer()).byteLength};
                }else{
                    reader=response.body.getReader();const first=await reader.read();
                    state.firstBody={done:first.done,bytes:first.value?.length??0,sha256:await digest(first.value??new Uint8Array())};
                    state.bodyUsedAtFirst=response.bodyUsed;state.trace.push('body-first');server.send('body-first');
                    let total=first.value?.length??0;for(;;){const part=await reader.read();if(part.done)break;total+=part.value.length;}
                    state.result.body={bytes:total};
                }
            }
            state.trace.push('complete');
        }catch(error){state.failure=describe(error);state.sameReason=error===sentinel;state.trace.push('failure');}
        if(kind.includes('abort')||kind==='tls-body-reader-cancel')await actionDone;
        if(response)state.bodyUsedAtEnd=response.bodyUsed;state.future=true;server.send('done');server.close(1000,'done');
    });
    return new Response(null,{status:101,webSocket:pair[0]});
}};

function scopeResponse(url,state){
    const {kind,challenge}=state;
    let resolveInitial;const initial=new Promise(resolve=>resolveInitial=resolve);resolveScopeForeign=()=>{resolveInitial(challenge);state.resolverCalled=true;};
    const pair=new WebSocketPair(),server=pair[1];server.accept();
    server.addEventListener('close',()=>state.parentClosed=true);
    if(kind.startsWith('http-created-timer'))setTimeout(()=>{state.timerFired=true;resolveInitial(challenge);state.resolverCalled=true;},300);
    server.addEventListener('message',async event=>{
        if(event.data!=='go')return;server.send('started');
        let resolve=resolveInitial,waiter=initial;
        if(kind==='event-created-promise')waiter=new Promise(done=>resolve=done);
        if(kind==='http-created-promise'||kind==='event-created-promise')setTimeout(()=>{state.timerFired=true;resolve(challenge);state.resolverCalled=true;},10);
        await waiter;state.future=true;server.send('done');server.close(1000,'done');
    });
    return new Response(null,{status:101,webSocket:pair[0]});
}

function readerResponse(url,state){
 const {kind,challenge}=state;
 const pair=new WebSocketPair(),server=pair[1];server.accept();
 server.addEventListener('message',async event=>{
  if(event.data!=='go')return;server.send('started');
  let reason=kind.endsWith('-default')?undefined:kind.endsWith('-string')?'cancel/'+challenge:kind.endsWith('-null')?null:kind.endsWith('-error')?new Error('cancel/'+challenge):{challenge};
  const describe=value=>({type:typeof value,name:value?.name??null,message:value?.message??null,primitive:value===null?'null':typeof value==='object'?null:String(value),sameReason:value===reason});
  const part=result=>({done:result.done,bytes:result.value?.byteLength??0});
  try{
   const response=kind.startsWith('standard-reader-probe')?new Response(new ReadableStream({start(c){c.enqueue(new Uint8Array([102,105,114,115,116]));}}),{status:409}):await fetch(url.searchParams.get('target'),{headers:{Upgrade:'websocket','X-Probe':challenge}});
   const form=/(?:reader-probe)-(direct|prototype|subclass|reflect)-/.exec(kind)?.[1];
   let reader;
   if(!form)reader=response.body.getReader();
   else{
    const Base=ReadableStreamDefaultReader;let Expected=Base;
    if(form==='direct')reader=new Base(response.body);
    else if(form==='prototype')reader=new Base.prototype.constructor(response.body);
    else{
     class ProbeReader extends Base{}Expected=ProbeReader;
     reader=form==='subclass'?new ProbeReader(response.body):Reflect.construct(Base,[response.body],ProbeReader);
    }
    state.construction={mode:form,instanceOfBase:reader instanceof Base,expectedPrototype:Object.getPrototypeOf(reader)===Expected.prototype,
     expectedConstructor:reader.constructor===Expected,prototypeConstructorIsBase:Base.prototype.constructor===Base};
   }
   const closed=reader.closed.then(()=>{state.closed={fulfilled:true};state.trace.push('closed');},e=>{state.closed={error:describe(e)};state.trace.push('closed-error');});
   if(!kind.endsWith('-before-first')){const first=await reader.read();state.first={...part(first),sha256:await digest(first.value??new Uint8Array())};}
   let pending;
   if(!kind.endsWith('-no-pending')&&!kind.endsWith('-before-first')&&!kind.endsWith('-eof'))pending=reader.read().then(v=>{state.pending={value:part(v)};state.trace.push('read');},e=>{state.pending={error:describe(e)};state.trace.push('read-error');});
   let second;if(kind.endsWith('-pending-two'))second=reader.read().then(v=>{state.second={value:part(v)};state.trace.push('second');},e=>{state.second={error:describe(e)};state.trace.push('second-error');});
   await new Promise(resolve=>setTimeout(resolve,20));
   try{const result=await reader.cancel(reason);state.cancel={fulfilled:true,returnedUndefined:result===undefined};state.trace.push('cancel');}catch(e){state.cancel={error:describe(e)};state.trace.push('cancel-error');}
   await Promise.all([closed,pending,second]);
   try{state.after={value:part(await reader.read())};}catch(e){state.after={error:describe(e)};}
   try{await reader.cancel('again');state.again={fulfilled:true};}catch(e){state.again={error:describe(e)};}
   state.bodyUsed=response.bodyUsed;state.locked=response.body.locked;reader.releaseLock();state.lockedAfterRelease=response.body.locked;
  }catch(e){state.unexpected=describe(e);}
  server.send('done');server.close(1000,'done');
 });return new Response(null,{status:101,webSocket:pair[0]});
}
