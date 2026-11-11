export default()=>{
    const signals=new WeakSet(),held=[],aborted=[],completed=[];let markReady;const ready=new Promise(resolve=>markReady=resolve);
    const context=call=>{const signalNew=!signals.has(call.signal);signals.add(call.signal);return {requestIdType:typeof call.requestId,requestId:String(call.requestId),deadlineType:typeof call.deadline,nullPrototype:Object.getPrototypeOf(call)===null,frozen:Object.isFrozen(call),signalType:typeof call.signal,aborted:call.signal.aborted,signalNew};};
    return {
        hold(input,call){
            if(!Number.isInteger(input.id)||input.id<0||input.id>62)throw new Error('only63 accepted held calls');
            const saved={id:input.id,call:context(call),signal:call.signal};held.push(saved);if(held.length===63)markReady();
            return new Promise(resolve=>{call.signal.addEventListener('abort',event=>{const reason=call.signal.reason;aborted.push({id:input.id,aborted:call.signal.aborted,reason:{name:reason.name,message:reason.message,code:reason.code,isDOMException:reason instanceof DOMException},event:{type:event.type,targetSame:event.target===call.signal,currentTargetSame:event.currentTarget===call.signal}});resolve({lateSuccess:true,id:input.id});});});
        },
        async barrier(input,call){await ready;const proof=context(call);completed.push(call.signal);return {held:held.map(item=>({id:item.id,call:item.call})),heldSignalsAborted:held.map(item=>item.signal.aborted),abortedCount:aborted.length,call:proof};},
        ping(input,call){const proof=context(call);completed.push(call.signal);return {input,heldCount:held.length,abortedCount:aborted.length,call:proof};},
        stats(input,call){return {held:held.map(item=>({id:item.id,call:item.call})),heldSignalsAborted:held.map(item=>item.signal.aborted),aborted,completedSignalsAborted:completed.map(signal=>signal.aborted),call:context(call)};},
    };
};
