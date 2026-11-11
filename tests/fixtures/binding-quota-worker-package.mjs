export default()=>{
    const groups=new Map(),seen=new WeakSet(),completed=[];let active=0,peak=0,total=0;
    const context=call=>{const fresh=!seen.has(call.signal);seen.add(call.signal);return {requestIdType:typeof call.requestId,requestId:String(call.requestId),deadlineType:typeof call.deadline,nullPrototype:Object.getPrototypeOf(call)===null,frozen:Object.isFrozen(call),signalNew:fresh,signalAborted:call.signal.aborted};};
    const reason=signal=>({name:signal.reason.name,message:signal.reason.message,code:signal.reason.code,isDOMException:signal.reason instanceof DOMException});
    return {
        hold(input,call){
            if(!Number.isInteger(input.slot)||input.slot<0||input.slot>63)throw new Error('bounded hold slot required');
            const key=String(call.requestId);if(!groups.has(key))groups.set(key,[]);const group=groups.get(key),item={slot:input.slot,call:context(call),signal:call.signal,abortEvents:[]};group.push(item);active++;total++;peak=Math.max(peak,active);
            if(group.length===64)console.log('quota-ready:'+key+':64');
            return new Promise(resolve=>call.signal.addEventListener('abort',event=>{
                item.abortEvents.push({type:event.type,targetSame:event.target===call.signal,currentTargetSame:event.currentTarget===call.signal});active--;
                if(group.every(value=>value.signal.aborted))console.log('quota-aborted:'+key+':64');
                resolve({lateSuccess:true,request:key,slot:input.slot});
            }));
        },
        summary(input,call){const proof=context(call);completed.push(call.signal);return {active,peak,total,groups:[...groups].map(([request,items])=>({request,started:items.length,aborted:items.filter(item=>item.signal.aborted).length})),completedSignalsAborted:completed.map(signal=>signal.aborted),call:proof};},
        slice(input,call){const items=groups.get(input.request);if(!items)throw new Error('known quota request');const proof=context(call);completed.push(call.signal);return {request:input.request,items:items.map(item=>({slot:item.slot,call:item.call,aborted:item.signal.aborted,reason:item.signal.aborted?reason(item.signal):null,abortEvents:item.abortEvents})),completedSignalsAborted:completed.map(signal=>signal.aborted),call:proof};},
    };
};
