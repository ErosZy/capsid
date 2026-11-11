let realm,current;
const describe=error=>({name:error?.name??null,message:error?.message??null});
const digest=async bytes=>Array.from(new Uint8Array(await crypto.subtle.digest('SHA-256',bytes)),value=>value.toString(16).padStart(2,'0')).join('');
function result(response){return {status:response.status,statusText:response.statusText,url:response.url,
    redirected:response.redirected,type:response.type,bodyNull:response.body===null,bodyUsed:response.bodyUsed,
    headers:[...response.headers],cookies:response.headers.getSetCookie(),hasWebSocket:response.webSocket!==null,body:null};}
export default {async fetch(request){
    realm??=crypto.randomUUID();const url=new URL(request.url);
    if(url.pathname.endsWith('/state'))return Response.json({realm,state:current??null});
    const kind=url.searchParams.get('kind'),challenge=url.searchParams.get('challenge');
    if(kind==='request-redirect-values'){
        const state=current={kind,challenge,constructors:[],fetches:[],responses:[]};return parameterResponse(url,state);
    }
    const state=current={kind,challenge,trace:[],result:null,firstBody:null,failure:null,socket:null};
    const init={headers:{Upgrade:'websocket','X-Probe':challenge}};
    if(kind.startsWith('redirect-'))Object.assign(init.headers,{Authorization:'auth/'+challenge,'Proxy-Authorization':'proxy/'+challenge,Cookie:'cookie='+challenge});
    if(kind==='redirect-error'){init.redirect='error';try{new Request(url.searchParams.get('target'),{redirect:'error'});state.requestProbe={returned:true};}catch(error){state.requestProbe={error:describe(error)};}}
    if(kind==='tls-proxy'){
        try{const response=await fetch(url.searchParams.get('target'),init);state.result=result(response);state.trace.push('returned');return response;}
        catch(error){state.failure=describe(error);return Response.json({realm,state},{status:500});}
    }
    const pair=new WebSocketPair(),server=pair[1];server.accept();
    server.addEventListener('close',()=>state.trace.push('parent-close'));
    server.addEventListener('message',async event=>{
        if(event.data!=='go')return;
        state.trace.push('fetch');server.send('started');
        try{
            const response=await fetch(url.searchParams.get('target'),init);state.result=result(response);state.trace.push('headers');
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
            }else if(kind.startsWith('body-truncated')||kind.startsWith('body-invalid-chunk')){
                const reader=response.body.getReader();const first=await reader.read();
                state.firstBody={done:first.done,bytes:first.value?.length??0,sha256:await digest(first.value??new Uint8Array())};
                state.trace.push('body-first');server.send('body-first');
                let total=first.value?.length??0;for(;;){const part=await reader.read();if(part.done)break;total+=part.value.length;}
                state.result.body={bytes:total};state.result.bodyUsed=response.bodyUsed;
            }else{
                const bytes=await response.arrayBuffer();state.result.body={bytes:bytes.byteLength,sha256:await digest(bytes)};state.result.bodyUsed=response.bodyUsed;
            }
            state.trace.push('complete');
        }catch(error){state.failure=describe(error);state.trace.push('failure');}
        server.send('done');server.close(1000,'done');
    });
    return new Response(null,{status:101,webSocket:pair[0]});
}};

function parameterResponse(url,state){
    const pair=new WebSocketPair(),server=pair[1];server.accept();
    server.addEventListener('message',async event=>{
        if(event.data!=='go')return;server.send('started');const target=url.searchParams.get('target'),sentinel={challenge:url.searchParams.get('challenge')};
        for(const label of ['follow','manual','undefined','null','empty','error','bogus','zero','symbol','object-error','object-manual','throwing-object','throwing-getter']){
            const item={label,conversions:0,hints:[],getterCalls:0,redirect:null,error:null,sameThrown:false};state.constructors.push(item);
            let value={follow:'follow',manual:'manual',undefined:undefined,null:null,empty:'',error:'error',bogus:'bogus',zero:0,symbol:Symbol('redirect')}[label];
            if(label.startsWith('object-'))value={[Symbol.toPrimitive](hint){item.conversions++;item.hints.push(hint);return label==='object-error'?'error':'manual';}};
            if(label==='throwing-object')value={[Symbol.toPrimitive](hint){item.conversions++;item.hints.push(hint);throw sentinel;}};
            const init=label==='throwing-getter'?{get redirect(){item.getterCalls++;throw sentinel;}}:{redirect:value};
            try{item.redirect=new Request(target,init).redirect;}catch(error){item.error=describe(error);item.sameThrown=error===sentinel;}
        }
        for(const method of ['constructor','json'])for(const label of ['error','bogus','symbol','object','getter']){
            const item={method,label,status:null,conversions:0,getterCalls:0,error:null};state.responses.push(item);
            const redirect=label==='symbol'?Symbol('unused'):label==='object'?{[Symbol.toPrimitive](){item.conversions++;throw sentinel;}}:label;
            const init=label==='getter'?{get redirect(){item.getterCalls++;throw sentinel;}}:{redirect};
            try{item.status=(method==='constructor'?new Response(null,init):Response.json({ok:true},init)).status;}catch(error){item.error=describe(error);}
        }
        for(const label of ['error','bogus','throwing-getter']){
            const item={label,synchronous:false,returnedPromise:false,resolved:false,error:null,sameThrown:false,getterCalls:0};state.fetches.push(item);let promise;
            const init=label==='throwing-getter'?{headers:{Upgrade:'websocket'},get redirect(){item.getterCalls++;throw sentinel;}}:{headers:{Upgrade:'websocket'},redirect:label};
            try{promise=fetch(target,init);item.returnedPromise=typeof promise?.then==='function';}
            catch(error){item.synchronous=true;item.error=describe(error);item.sameThrown=error===sentinel;}
            if(promise)try{
                const response=await promise;item.resolved=true;
                if(response.webSocket){const child=response.webSocket;let done;const closed=new Promise(resolve=>done=resolve);child.addEventListener('close',()=>done());child.accept();child.close(1000,'parameter cleanup');await closed;}
                else await response.arrayBuffer();
            }catch(error){item.error=describe(error);item.sameThrown=error===sentinel;}
        }
        server.send('done');server.close(1000,'done');
    });
    return new Response(null,{status:101,webSocket:pair[0]});
}
