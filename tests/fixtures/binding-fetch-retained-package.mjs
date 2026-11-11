export default()=>{
    const signals=new WeakSet();let resource;
    const context=call=>{const signalNew=!signals.has(call.signal);signals.add(call.signal);return {requestIdType:typeof call.requestId,requestId:String(call.requestId),deadlineType:typeof call.deadline,
        nullPrototype:Object.getPrototypeOf(call)===null,frozen:Object.isFrozen(call),signalType:typeof call.signal,aborted:call.signal.aborted,signalNew};};
    return {
        async open(input,call){
            if(resource)throw new Error('one retained reader at a time');
            const response=await fetch(input.target),Base=ReadableStreamBYOBReader;class Derived extends Base{}
            const route=input.route,reader=route==='getReader'?response.body.getReader({mode:'byob'}):route==='direct'?new Base(response.body):route==='prototype'?new Base.prototype.constructor(response.body):route==='subclass'?new Derived(response.body):Reflect.construct(Base,[response.body],Derived);
            resource={response,reader,view:input.view,method:input.method,closed:reader.closed,settled:'pending',signal:call.signal};const saved=resource;
            saved.closed.then(()=>saved.settled='fulfilled',()=>saved.settled='rejected');
            return {metadata:{status:response.status,statusText:response.statusText,ok:response.ok,redirected:response.redirected,type:response.type,url:response.url,headers:[...response.headers],bodyUsedBefore:response.bodyUsed,lockedBefore:true},
                brand:reader instanceof Base,closedStable:saved.closed===reader.closed,closedState:saved.settled,call:context(call)};
        },
        async read(input,call){
            if(!resource)throw new Error('retained reader required');const {reader,view,method}=resource,inputBuffer=new ArrayBuffer(64);new Uint8Array(inputBuffer).fill(165);
            const into=view==='DataView'?new DataView(inputBuffer,8,32):new globalThis[view](inputBuffer,8,32/globalThis[view].BYTES_PER_ELEMENT),minimum=view==='Float64Array'?2:4;
            const pending=method==='least'?reader.readAtLeast(minimum,into):reader.read(into,{min:minimum}),inputAfter=inputBuffer.byteLength,result=await pending,value=result.value;
            return {read:{inputAfter,done:result.done,type:value?.constructor.name??null,offset:value?.byteOffset??null,length:value?.byteLength??null,capacity:value?.buffer.byteLength??null,
                bytes:value?Array.from(new Uint8Array(value.buffer,value.byteOffset,value.byteLength)):null,backing:value?Array.from(new Uint8Array(value.buffer)):null},
                closedStable:resource.closed===reader.closed,openSignalAborted:resource.signal.aborted,call:context(call)};
        },
        async close(input,call){
            if(!resource)throw new Error('retained reader required');const saved=resource;await saved.closed;saved.reader.releaseLock();resource=null;
            return {closed:saved.settled,bodyUsed:saved.response.bodyUsed,locked:saved.response.body.locked,openSignalAborted:saved.signal.aborted,call:context(call)};
        },
    };
};
