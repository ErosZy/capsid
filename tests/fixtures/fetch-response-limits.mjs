// Actual native fetch body; no synthetic ReadableStream or JavaScript limit override.
const describe=error=>({name:error?.name??null,message:error?.message??null,isTypeError:error instanceof TypeError});
function construct(stream,route,byob){
    if(route==='getReader')return byob?stream.getReader({mode:'byob'}):stream.getReader();
    return byob?new ReadableStreamBYOBReader(stream):new ReadableStreamDefaultReader(stream);
}
export default {async fetch(request){
    const query=new URL(request.url).searchParams;
    const method=query.get('method'),route=query.get('route'),known=query.get('known')==='1',length=Number(query.get('length'));
    const action=query.get('action'),byob=method!=='default';
    const state={method,route,known,length,action,phase:'fetch',reads:[],error:null,closed:null,cancel:null,pending:null};
    let response,reader,readError,closedError;
    const source=await fetch('https://example.test/body',{headers:{Upgrade:'websocket'}});
    try{
        response=source;state.response={status:response.status,contentLength:response.headers.get('content-length')};
        if(query.get('encoding')==='1')state.response.contentEncoding=response.headers.get('content-encoding');
        state.phase='getReader';reader=construct(response.body,route,byob);
        const closed=reader.closed.then(()=>{state.closed={fulfilled:true};},error=>{closedError=error;state.closed={error:describe(error)};});
        const issue=(minimumOverride=undefined)=>{
            const input=byob?new ArrayBuffer(32):null;
            if(input)new Uint8Array(input).fill(165);
            const view=input?new Uint8Array(input,8,16):null;
            const inputBytes=input?.byteLength??null;
            const minimum=minimumOverride??(method==='high'?12:method==='least'?2:1);
            const promise=!byob?reader.read():minimumOverride!==undefined?reader.read(view,{min:minimum}):
                method==='high'||method==='least'?reader.readAtLeast(minimum,view):reader.read(view);
            return {input,promise,inputBytes,inputAfterIssue:input?.byteLength??null};
        };
        const observe=async issued=>{
            try{
                const value=await issued.promise,view=value.value;
                const outside=view?(()=>{
                    const bytes=new Uint8Array(view.buffer),fill=byob?165:0;
                    return bytes.subarray(0,view.byteOffset).every(byte=>byte===fill)&&
                        bytes.subarray(view.byteOffset+view.byteLength).every(byte=>byte===fill);
                })():null;
                state.reads.push({done:value.done,type:view?.constructor.name??null,bytes:view?Array.from(view):null,
                    offset:view?.byteOffset??null,backingBytes:view?.buffer.byteLength??null,outside,
                    inputBytes:issued.inputBytes,inputAfterIssue:issued.inputAfterIssue,inputAfter:issued.input?.byteLength??null});
                return value;
            }catch(error){
                readError=error;state.error=describe(error);
                state.rejectedInput={inputBytes:issued.inputBytes,inputAfterIssue:issued.inputAfterIssue,inputAfter:issued.input?.byteLength??null};
                return null;
            }
        };
        state.phase='read';
        const reason={tag:'consumer-stop'};
        if(action==='full'){
            for(let index=0;index<=length+1;index++){
                const value=await observe(issue());if(value===null||value.done)break;
                if(!value.value?.byteLength)throw new Error('Nonterminal read did not advance');
            }
        }else{
            await observe(issue(1));
            if(action==='pending'){
                await observe(issue(1));
                const issued=issue();let settled=false;
                const pending=issued.promise.then(value=>{settled=true;return {done:value.done,bytes:value.value?Array.from(value.value):null};},
                    error=>{settled=true;return {error:describe(error),sameReason:error===reason,type:typeof error,tag:error?.tag??null,keys:Object.keys(error??{}),plain:Object.getPrototypeOf(error)===Object.prototype};});
                await new Promise(resolve=>setTimeout(resolve,30));
                state.pending={beforeCancel:{readSettled:settled,closedSettled:state.closed!==null},inputBytes:issued.inputBytes,inputAfterIssue:issued.inputAfterIssue};
                state.cancel={returnedUndefined:(await reader.cancel(reason))===undefined};
                state.pending.result=await pending;state.pending.inputAfter=issued.input?.byteLength??null;
            }else state.cancel={returnedUndefined:(await reader.cancel(reason))===undefined};
        }
        await closed;
        if(action==='full'){
            try{state.cancel={returnedUndefined:(await reader.cancel())===undefined};}
            catch(error){state.cancel={error:describe(error),sameRead:error===readError};}
        }
        state.errorIdentity=readError===closedError;
        reader.releaseLock();state.phase='complete';
    }catch(error){state.error=describe(error);}
    state.bodyUsed=response?.bodyUsed??null;state.locked=response?.body?.locked??null;
    return Response.json(state);
}};
