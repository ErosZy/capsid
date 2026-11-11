async function matrix(target,views=['Uint16Array','Float64Array','DataView'],selection=null){
    const rows=[];
    for(const route of ['getReader','direct','prototype','subclass','reflect'])for(const method of ['read','least'])for(const view of views){
        if(selection&&(route!==selection[0]||method!==selection[1]||view!==selection[2]))continue;
        const row={route,method,view,failure:null,reads:[]};rows.push(row);let reader;
        try{
            const response=await fetch(target),Base=ReadableStreamBYOBReader;class Derived extends Base{}
            row.metadata={status:response.status,statusText:response.statusText,ok:response.ok,redirected:response.redirected,type:response.type,url:response.url,headers:[...response.headers],bodyUsedBefore:response.bodyUsed,lockedBefore:response.body.locked};
            reader=route==='getReader'?response.body.getReader({mode:'byob'}):route==='direct'?new Base(response.body):route==='prototype'?new Base.prototype.constructor(response.body):route==='subclass'?new Derived(response.body):Reflect.construct(Base,[response.body],Derived);
            row.brand=reader instanceof ReadableStreamBYOBReader;const closed=reader.closed;let settled='pending';closed.then(()=>settled='fulfilled',()=>settled='rejected');row.closedStable=closed===reader.closed;
            for(let index=0;;++index){
                if(index>10)throw new Error('bounded finite Binding body diagnostic');
                const input=new ArrayBuffer(64);new Uint8Array(input).fill(165);
                const into=view==='DataView'?new DataView(input,8,32):new globalThis[view](input,8,32/globalThis[view].BYTES_PER_ELEMENT),min=view==='Float64Array'?2:4;
                const promise=method==='least'?reader.readAtLeast(min,into):reader.read(into,{min});
                const inputAfter=input.byteLength,result=await promise,value=result.value;
                row.reads.push({inputAfter,done:result.done,type:value?.constructor.name??null,offset:value?.byteOffset??null,
                    length:value?.byteLength??null,capacity:value?.buffer.byteLength??null,
                    bytes:value?Array.from(new Uint8Array(value.buffer,value.byteOffset,value.byteLength)):null,
                    backing:value?Array.from(new Uint8Array(value.buffer)):null});
                if(result.done)break;
            }
            await closed;row.closed=settled;reader.releaseLock();row.lockedAfter=response.body.locked;row.bodyUsedAfter=response.bodyUsed;
        }catch(error){row.failure={name:error?.name??null,message:error?.message??null};if(reader){try{await reader.cancel();}catch{}try{reader.releaseLock();}catch{}}}
    }
    return rows;
}

export default()=>{
    const signals=new WeakSet();
    const context=call=>{
        const signalNew=!signals.has(call.signal);signals.add(call.signal);
        return {requestIdType:typeof call.requestId,requestId:String(call.requestId),deadlineType:typeof call.deadline,
            nullPrototype:Object.getPrototypeOf(call)===null,frozen:Object.isFrozen(call),signalType:typeof call.signal,aborted:call.signal.aborted,signalNew};
    };
    return {
        async jsonBatch(input){return JSON.stringify(await matrix(input.target,input.views));},
        async row(input,call){return {row:(await matrix(input.target,[input.selection[2]],input.selection))[0],call:context(call)};},
        async objectBatch(input){return matrix(input.target);},
        async echo(input,call){return {value:input,inputWasNullPrototype:Object.getPrototypeOf(input)===null,call:context(call)};},
    };
};
