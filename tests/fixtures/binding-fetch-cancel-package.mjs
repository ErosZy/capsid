export default()=>{
    const signals=new WeakSet();let resource;
    let canceled,markReady;const ready=new Promise(resolve=>markReady=resolve);
    const error=reason=>({name:reason.name,message:reason.message,code:reason.code,isDOMException:reason instanceof DOMException});
    const makeReader=(body,route)=>{const Base=ReadableStreamBYOBReader;class Derived extends Base{};return route==='getReader'?body.getReader({mode:'byob'}):route==='direct'?new Base(body):route==='prototype'?new Base.prototype.constructor(body):route==='subclass'?new Derived(body):Reflect.construct(Base,[body],Derived);};

    const context=call=>{const signalNew=!signals.has(call.signal);signals.add(call.signal);return {requestIdType:typeof call.requestId,requestId:String(call.requestId),deadlineType:typeof call.deadline,
        nullPrototype:Object.getPrototypeOf(call)===null,frozen:Object.isFrozen(call),signalType:typeof call.signal,aborted:call.signal.aborted,signalNew};};
    return {
        async wait(input,call){
            if(canceled)throw new Error('one pending canceled read at a time');
            const state=canceled={signal:call.signal,call:context(call),abortEvents:[],readState:'pending',closedState:'pending'};
            call.signal.addEventListener('abort',()=>state.abortEvents.push({aborted:call.signal.aborted,reason:error(call.signal.reason)}));
            const response=await fetch(input.target,{signal:call.signal}),reader=makeReader(response.body,input.route),closed=reader.closed;
            state.response=response;state.reader=reader;state.closedStable=closed===reader.closed;
            const closedResult=closed.then(()=>{state.closedState='fulfilled';return null;},reason=>{state.closedState='rejected';state.closedError=reason;return reason;});
            const buffer=new ArrayBuffer(64);new Uint8Array(buffer).fill(165);const view=input.view==='DataView'?new DataView(buffer,8,32):new globalThis[input.view](buffer,8,32/globalThis[input.view].BYTES_PER_ELEMENT),minimum=input.view==='Float64Array'?2:4;
            const pending=input.method==='least'?reader.readAtLeast(minimum,view):reader.read(view,{min:minimum});state.inputAfter=buffer.byteLength;state.ready=true;markReady();
            state.readError=await pending.then(value=>{state.readState='fulfilled';state.unexpectedValue={done:value.done,length:value.value?.byteLength??null};return null;},reason=>{state.readState='rejected';return reason;});
            await closedResult;state.finished=true;
            // Return successfully after cancellation. The original User receiver
            // must remain rejected; this late successful result is discarded.
            return {lateSuccess:true};
        },
        async ready(input,call){await ready;return {ready:canceled.ready,readState:canceled.readState,closedState:canceled.closedState,inputAfter:canceled.inputAfter,pendingCall:canceled.call,signalAborted:canceled.signal.aborted,closedStable:canceled.closedStable,call:context(call)};},
        async inspect(input,call){
            const state=canceled;if(!state?.ready)throw new Error('pending read required');
            // Both rejection continuations must have run before exposing state.
            await Promise.resolve();
            const data={finished:state.finished===true,ready:state.ready,readState:state.readState,closedState:state.closedState,inputAfter:state.inputAfter,closedStable:state.closedStable,
                signalAborted:state.signal.aborted,signalReason:error(state.signal.reason),abortEvents:state.abortEvents,
                readError:error(state.readError),closedError:error(state.closedError),readSameAsSignal:state.readError===state.signal.reason,closedSameAsSignal:state.closedError===state.signal.reason,closedSameAsRead:state.closedError===state.readError,
                bodyUsed:state.response.bodyUsed,lockedBefore:state.response.body.locked,pendingCall:state.call,call:context(call)};
            state.reader.releaseLock();data.lockedAfter=state.response.body.locked;return data;
        },
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
