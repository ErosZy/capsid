const describe=(error,reason)=>({name:error?.name??null,message:error?.message??null,sameReason:error===reason,
    nativePrototype:error instanceof DOMException?Object.getPrototypeOf(error)===DOMException.prototype:Object.getPrototypeOf(error)===globalThis[error?.name]?.prototype});
const meta=response=>({status:response.status,bodyUsed:response.bodyUsed,locked:response.body.locked,headers:[...response.headers]});
export default {async fetch(request){
    const url=new URL(request.url);if(url.pathname==='/healthy')return new Response('healthy');
    const stage=url.searchParams.get('stage'),trigger=url.searchParams.get('trigger'),link=url.searchParams.get('link'),target=url.searchParams.get('target');
    const state={stage,trigger,link,failure:null,trace:[]};let response,reader,first,closed,firstSettled=false,closedSettled=false;
    const signal=trigger==='signal'?AbortSignal.timeout(500):link==='request'?request.signal:null;
    if(signal)signal.addEventListener('abort',()=>{state.trace.push('signal.abort');state.reason=describe(signal.reason,signal.reason);},{once:true});
    const read=label=>{
        const input=stage==='default'?null:new ArrayBuffer(24);if(input)new Uint8Array(input).fill(165);
        const row={label,inputBefore:input?.byteLength??null};
        try{const pending=stage==='default'?reader.read():stage==='least'?reader.readAtLeast(2,new DataView(input,8,8)):reader.read(new Uint8Array(input,8,8));row.returnedPromise=pending instanceof Promise;row.inputAfterIssue=input?.byteLength??null;
            return pending.then(({done,value})=>{if(label==='first')firstSettled=true;state.trace.push(label+'.fulfilled');return {...row,inputAfter:input?.byteLength??null,done,type:value?.constructor.name??null,offset:value?.byteOffset??null,length:value?.byteLength??null,capacity:value?.buffer.byteLength??null,bytes:value?Array.from(new Uint8Array(value.buffer,value.byteOffset,value.byteLength)):null,backing:value?Array.from(new Uint8Array(value.buffer)):null};},error=>{if(label==='first')firstSettled=true;state.trace.push(label+'.rejected');return {...row,inputAfter:input?.byteLength??null,error:describe(error,signal?.reason)};});
        }catch(error){return Promise.resolve({...row,inputAfter:input?.byteLength??null,syncError:describe(error,signal?.reason)});}
    };
    try{
        state.fetchIssued=true;response=await fetch(target,signal?{signal}:undefined);state.before=meta(response);
        if(['default','byob','least'].includes(stage)){
            reader=response.body.getReader(stage==='default'?undefined:{mode:'byob'});const watched=reader.closed;state.closedStable=watched===reader.closed;
            closed=watched.then(()=>{closedSettled=true;state.trace.push('closed.fulfilled');state.closed={fulfilled:true};},error=>{closedSettled=true;state.trace.push('closed.rejected');state.closed={error:describe(error,signal?.reason)};});first=read('first');
        }
        state.preControl={firstPending:first?!firstSettled:null,closedPending:closed?!closedSettled:null,response:meta(response)};
        const control=new URL('/control',target);control.searchParams.set('phase',JSON.stringify({stage,trigger,link,preControl:state.preControl}));const ack=await fetch(control);state.control={status:ack.status,text:await ack.text()};
        if(trigger==='host')console.log('NATIVE_DEADLINE_READY '+JSON.stringify(state));
        if(trigger==='host'){
            if(first)state.first=await first;else await new Promise(()=>{});
            state.unexpectedAfterHostDeadline=true;
        }else{
            if(!first){if(!signal.aborted)await new Promise(resolve=>signal.addEventListener('abort',resolve,{once:true}));reader=response.body.getReader({mode:'byob'});first=read('first');}
            state.first=await first;if(closed)await closed;state.retry=await read('retry');state.aborted=signal.aborted;
        }
    }catch(error){state.failure=describe(error,signal?.reason);state.aborted=signal?.aborted??null;}
    finally{if(reader)try{reader.releaseLock();state.released=true;}catch(error){state.releaseError=describe(error,signal?.reason);}if(response)state.after=meta(response);}
    return Response.json(state);
}};
