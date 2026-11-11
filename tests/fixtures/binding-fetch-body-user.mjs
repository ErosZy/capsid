import echo from 'capsid:binding/echo';
const nodes=value=>1+(Array.isArray(value)?value.reduce((sum,item)=>sum+nodes(item),0):value&&typeof value==='object'?Object.values(value).reduce((sum,item)=>sum+nodes(item),0):0);
export default {async fetch(request){
    const url=new URL(request.url),target=url.searchParams.get('target'),views=url.searchParams.get('views').split(',');
    const batchWire=await echo.jsonBatch({target,views}),state={batchWireType:typeof batchWire,batch:JSON.parse(batchWire),structured:[],calls:[],rowWireTypes:[],events:['jsonBatch']};
    for(const route of ['getReader','direct','prototype','subclass','reflect'])for(const method of ['read','least'])for(const view of views){
        const envelope=await echo.row({target,selection:[route,method,view]}),row=envelope.row;state.structured.push(row);state.calls.push(envelope.call);
        state.rowWireTypes.push({nullPrototype:Object.getPrototypeOf(envelope)===null,rowNullPrototype:Object.getPrototypeOf(row)===null,
            callNullPrototype:Object.getPrototypeOf(envelope.call)===null,metadataNullPrototype:Object.getPrototypeOf(row.metadata)===null,
            readNullPrototypes:row.reads.every(value=>Object.getPrototypeOf(value)===null),readsArrayPrototype:Object.getPrototypeOf(row.reads)===Array.prototype,
            byteArrayPrototypes:row.reads.every(value=>Object.getPrototypeOf(value.bytes)===Array.prototype&&Object.getPrototypeOf(value.backing)===Array.prototype),
            headersArrayPrototypes:Object.getPrototypeOf(row.metadata.headers)===Array.prototype&&row.metadata.headers.every(value=>Object.getPrototypeOf(value)===Array.prototype)});state.events.push('row'+(state.structured.length-1));
    }
    state.matrixCloneNodes=nodes(state.batch);state.maxRowCloneNodes=Math.max(...state.structured.map(nodes));
    try{const value=await echo.objectBatch({target});state.oversized={fulfilled:true,valueType:typeof value};state.events.push('oversizedFulfilled');}
    catch(error){state.oversized={fulfilled:false,name:error.name,message:error.message,errorPrototype:Object.getPrototypeOf(error)===Error.prototype};state.events.push('oversizedRejected');}
    const envelope=await echo.row({target,selection:['getReader','read',views[0]]});state.recovery=envelope.row;state.recoveryCall=envelope.call;state.events.push('recovery');
    state.completedCalls=[];
    for(let index=0;index<70;index++){
        const value=await echo.echo({index});
        state.completedCalls.push({value:value.value,inputWasNullPrototype:value.inputWasNullPrototype,outputNullPrototype:Object.getPrototypeOf(value.value)===null,call:value.call});state.events.push('echo'+index);
    }

    return Response.json(state);
}};
