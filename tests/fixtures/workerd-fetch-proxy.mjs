let realm;
let state=null,reused,parked,wake,released;
export default {async fetch(request){
    realm??=crypto.randomUUID();
    const url=new URL(request.url),kind=url.searchParams.get('kind');
    if(url.pathname.endsWith('/state'))return Response.json({realm,state});
    if(url.pathname.endsWith('/foreign')){
        state.foreign={};
        for(const operation of ['readyState','accept','send','close'])try{
            const result=operation==='readyState'?released.readyState:operation==='accept'?released.accept():operation==='send'?released.send('foreign-side'):released.close(1000,'foreign');
            state.foreign[operation]={returned:result===undefined?{type:'undefined'}:result};
            if(operation==='close')state.foreign.readyStateAfterClose=released.readyState;
        }catch(error){state.foreign[operation]={error:{name:error.name,message:error.message}};}
        return Response.json({realm,state});
    }
    if(url.pathname.endsWith('/mark')){state.sawHeaderBeforeTimer=state.timerFired===0;return Response.json({realm,state});}
    if(url.pathname.endsWith('/wake')){state.futureRequested=true;const resolve=wake;wake=null;resolve?.();return Response.json({realm,state});}
    if(kind==='cross-origin'){state.reused=true;return parked;}
    if(kind==='reuse'&&reused){state.reused=true;return reused;}
    const target=url.searchParams.get('target');let callTarget=target,init={headers:{Upgrade:'websocket'}},inputConversions=0;const initOrder=[];
    if(kind==='request-info-upgrade-url'||kind==='request-info-upgrade-request'){
        const input=new URL(target);Object.defineProperty(input,'toString',{value(){inputConversions++;initOrder.push('input');return target;}});
        init={get headers(){initOrder.push('headers');return {Upgrade:'websocket'};}};
        if(kind==='request-info-upgrade-request'){callTarget=new Request(input,init);init=undefined;}else callTarget=input;
    }
    const response=await fetch(callTarget,init);
    state={kind,status:response.status,hasSocket:response.webSocket!==null,accepted:false,reused:false,inputConversions,initOrder};
    if(kind==='foreign-methods')released=response.webSocket;
    if(kind==='park'){parked=response;return new Response('parked');}
    if(kind==='accepted'){response.webSocket.accept();state.accepted=true;}
    if(kind==='upstream-first'||kind==='upstream-first-binary')await new Promise(resolve=>setTimeout(resolve,75));
    if(kind==='rewrap')return new Response(null,{status:101,statusText:response.statusText,headers:response.headers,webSocket:response.webSocket});
    if(kind==='intrinsic-traps'){state.traps=0;for(const name of ['send','accept','addEventListener'])Object.defineProperty(WebSocket.prototype,name,{configurable:true,writable:true,value(){state.traps++;throw new Error('application WebSocket '+name+' must not relay native proxy data');}});}
    if(kind==='source-live-sibling'){
        const sibling=await fetch(url.searchParams.get('target')+'&sibling=1',{headers:{Upgrade:'websocket'}});
        const child=sibling.webSocket;child.accept();state.childClosed=0;state.childTimerFired=0;state.afterChildTimerFired=0;state.childEcho=null;state.childFailure=null;
        child.addEventListener('message',event=>{state.childEcho=event.data;child.close(1000,'sibling done');});
        child.addEventListener('close',()=>state.childClosed++);
        setTimeout(()=>{state.childTimerFired++;try{child.send('sibling/'+url.searchParams.get('challenge'));}catch(error){state.childFailure={name:error.name,message:error.message};}},750);
        setTimeout(()=>state.afterChildTimerFired++,1300);
    }
    if(['source-http','source-http-request','source-http-object'].includes(kind)){
        const held=new URL(url.searchParams.get('target'));held.pathname='/held-http';const href=held.href;
        state.sideInputConversions=0;state.sideInitOrder=[];
        Object.defineProperty(held,'toString',{value(){state.sideInputConversions++;state.sideInitOrder.push('input');return href;}});
        let input=held;
        if(kind==='source-http-request')input=new Request(held,{get headers(){state.sideInitOrder.push('headers');return {};}}).clone();
        if(kind==='source-http-object'){
            const sentinel={challenge:url.searchParams.get('challenge')};state.rejection={synchronous:false,identity:false,calls:0,hints:[],returnedPromise:false};let rejected;
            try{rejected=fetch({[Symbol.toPrimitive](hint){state.rejection.calls++;state.rejection.hints.push(hint);throw sentinel;}});state.rejection.returnedPromise=typeof rejected?.then==='function';}
            catch(error){state.rejection.synchronous=true;state.rejection.identity=error===sentinel;}
            if(rejected)try{await rejected;}catch(error){state.rejection.identity=error===sentinel;}
            input={[Symbol.toPrimitive](hint){state.sideInputConversions++;state.sideInitOrder.push(hint);return href;}};
        }
        const pending=await fetch(input);state.heldStatus=pending.status;state.heldBodyFinished=false;state.heldBodyFailure=false;
        pending.text().then(()=>state.heldBodyFinished=true,()=>state.heldBodyFailure=true);
    }
    if(kind==='source-parked-promise'){
        state.future=0;state.futureRequested=false;
        new Promise(resolve=>wake=resolve).then(()=>state.future++);
    }
    if(['source-lifetime','source-live-sibling','source-http','source-parked-promise','source-http-request','source-http-object'].includes(kind)){
        const socket=response.webSocket;state.timerFired=0;state.timerDelayMs=500;state.listenerEvents=[];
        socket.addEventListener('message',()=>state.listenerEvents.push('message'));
        socket.addEventListener('close',()=>state.listenerEvents.push('close'));
        setTimeout(()=>{state.timerFired++;state.afterTransfer={};
            for(const operation of ['readyState','accept','send','close'])try{
                const result=operation==='readyState'?socket.readyState:operation==='accept'?socket.accept():operation==='send'?socket.send('application-side'):socket.close(1000,'released');
                state.afterTransfer[operation]={returned:result===undefined?{type:'undefined'}:result};if(operation==='close')state.afterTransfer.readyStateAfterClose=socket.readyState;
            }catch(error){state.afterTransfer[operation]={error:{name:error.name,message:error.message}};}
        },500);
    }
    if(kind==='reuse')reused=response;
    return response;
}};
