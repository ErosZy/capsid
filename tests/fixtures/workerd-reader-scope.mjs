let realm,current,active;
const delay=ms=>new Promise(resolve=>setTimeout(resolve,ms));
const describe=error=>({name:error?.name??null,message:error?.message??null,isError:error instanceof Error,tag:error?.tag??null});
const part=result=>({done:result.done,type:result.value?.constructor.name??null,offset:result.value?.byteOffset??null,
    length:result.value?.byteLength??null,capacity:result.value?.buffer.byteLength??null,bytes:result.value?Array.from(result.value):null});
function construct(stream,route,byob){
    const Base=byob?ReadableStreamBYOBReader:ReadableStreamDefaultReader;
    if(route==='getReader')return stream.getReader(byob?{mode:'byob'}:undefined);
    if(route==='direct')return new Base(stream);
    if(route==='prototype')return new Base.prototype.constructor(stream);
    class Derived extends Base{}return route==='subclass'?new Derived(stream):Reflect.construct(Base,[stream],Derived);
}
async function setup(url,state,server){
    try{
        const [source,route,mode,operation]=state.kind.split('|'),[,action,phase]=operation.split('_'),byob=mode==='byob';
        const response=source==='native'?await fetch(url.searchParams.get('target'),{headers:{Upgrade:'websocket','X-Probe':state.challenge}}):null;
        const stream=response?.body??new ReadableStream({type:'bytes',start(controller){controller.enqueue(new Uint8Array([0,102,105,114,115,116]));}});
        let reader=construct(stream,route,byob);const pending=[],labels=[],raw={};
        const watch=(label,promise,reading=false)=>{
            labels.push(label);const item=state[label]={settled:false,result:null};
            const handled=Promise.resolve(promise).then(value=>{item.settled=true;item.result=reading?{value:part(value)}:{returnedUndefined:value===undefined};},
                error=>{raw[label]=error;item.settled=true;item.result={error:describe(error)};});pending.push(handled);return handled;
        };
        const issue=(label,prime=false)=>{
            const buffer=byob?new ArrayBuffer(32):null;if(buffer)new Uint8Array(buffer).fill(165);
            state[label+'Input']={before:buffer?.byteLength??null};let promise;
            try{promise=byob?reader.read(new Uint8Array(buffer,8,prime?1:16)):reader.read();}
            catch(error){state[label+'Sync']=describe(error);promise=Promise.reject(error);}
            state[label+'Input'].after=buffer?.byteLength??null;return watch(label,promise,true);
        };
        const before=reader.closed;state.closedInitialStable=before===reader.closed;const closedWork=watch('closed',before);
        if(phase==='primed')await issue('prime',true);
        state.lockedBeforeActor=stream.locked;
        active={state,stream,actor:async()=>{
            state.actorSync=null;let actorWork;
            try{
                if(action==='read'){const readWork=issue('actorRead');if(source==='native'||phase==='unread'||byob)actorWork=readWork;}
                else if(action==='cancel'){const cancelWork=watch('actorCancel',reader.cancel({tag:'foreign-stop'}));actorWork=source==='native'?cancelWork:Promise.all([cancelWork,closedWork]);}
                else if(action==='release'){state.actorRelease=reader.releaseLock()===undefined;if(source==='standard')actorWork=closedWork;}
                else{const promise=reader.closed;state.closedActorIdentity=promise===before;state.closedActorStable=promise===reader.closed;watch('actorClosed',promise);}
            }catch(error){state.actorSync=describe(error);}
            if(actorWork)await actorWork;
            await delay(20);state.actorLocked=stream.locked;state.actorBodyUsed=response?.bodyUsed??null;
            state.beforeFinish={closed:state.closed.settled,read:state.actorRead?.settled??null,cancel:state.actorCancel?.settled??null,actorClosed:state.actorClosed?.settled??null};
        },finish:async()=>{
            try{
                if(!stream.locked)reader=construct(stream,route,byob);
                const ownerRead=issue('ownerRead');
                const dataExpected=source==='native'?(phase==='unread'||byob||action==='read'):
                    action!=='read'&&(phase==='unread'||byob||action==='cancel');
                state.ownerWaitedForData=dataExpected;
                if(dataExpected){let settled=false;ownerRead.then(()=>settled=true);await Promise.race([ownerRead,delay(500)]);state.ownerAwaitComplete=settled;}
                else await delay(20);
                state.ownerReadBeforeCancel=state.ownerRead.settled;
                watch('ownerCancel',reader.cancel({tag:'owner-stop'}));await delay(30);
                await delay(10);state.watchersSettled=labels.every(label=>state[label].settled);
                state.actorReadErrorIsClosed=raw.actorRead!==undefined&&raw.actorRead===raw.closed;
                state.bodyUsed=response?.bodyUsed??null;reader.releaseLock();state.locked=stream.locked;
            }catch(error){state.finishError=describe(error);}
            server.send('done');server.close(1000,'done');
        }};
        server.send('primed');
    }catch(error){state.unexpected=describe(error);server.send('done');server.close(1000,'done');}
}
export default{async fetch(request){
    realm??=crypto.randomUUID();const url=new URL(request.url);
    if(url.pathname.endsWith('/state'))return Response.json({realm,state:current??null});
    if(url.pathname.endsWith('/act')){if(!active||active.state.challenge!==url.searchParams.get('challenge'))return new Response('wrong challenge',{status:409});await active.actor();return Response.json({realm,state:current});}
    const state=current={kind:url.searchParams.get('kind'),challenge:url.searchParams.get('challenge'),unexpected:null};
    const pair=new WebSocketPair(),server=pair[1];server.accept();
    server.addEventListener('message',event=>{if(event.data==='go'){server.send('started');void setup(url,state,server);}else if(event.data==='finish')void active.finish();});
    return new Response(null,{status:101,webSocket:pair[0]});
}};
