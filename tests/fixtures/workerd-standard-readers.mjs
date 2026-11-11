let realm,current;
function construct(stream,route) {
    const Base=ReadableStreamDefaultReader;
    if(route==='getReader')return stream.getReader();
    if(route==='direct')return new Base(stream);
    if(route==='prototype')return new Base.prototype.constructor(stream);
    class Derived extends Base {}
    return route==='subclass'?new Derived(stream):Reflect.construct(Base,[stream],Derived);
}
function append(list,value) {
    const last=list.at(-1);
    const equal=last&&(typeof value==='object'?Object.keys(value).length===Object.keys(last.value).length&&
        Object.keys(value).every(key=>last.value[key]===value[key]):last.value===value);
    if(equal)last.count++;else list.push({count:1,value});
}
const prefix=buffer=>buffer.byteLength?Array.from(new Uint8Array(buffer).slice(0,20)):[];
const digest=async bytes=>Array.from(new Uint8Array(await crypto.subtle.digest('SHA-256',bytes))).map(v=>v.toString(16).padStart(2,'0')).join('');
function appendShape(list,done,length,offset,capacity,outsideZero,type,sameRealm) {
    const last=list.at(-1),previous=last?.value;
    if(previous&&previous.done===done&&previous.length===length&&previous.offset===offset&&
        previous.capacity===capacity&&previous.outsideZero===outsideZero&&previous.type===type&&previous.sameRealm===sameRealm)last.count++;
    else list.push({count:1,value:{done,length,offset,capacity,outsideZero,type,sameRealm}});
}
function outsideBytesZero(buffer,offset,length) {
    const bytes=new Uint8Array(buffer);let zero=true;
    for(let at=0;at<offset;++at)if(bytes[at]!==0)zero=false;
    for(let at=offset+length;at<bytes.length;++at)if(bytes[at]!==0)zero=false;
    return zero;
}
async function shapes(state,route,phase,allocation,encoded) {
    const lengths=encoded==='empty'?[]:encoded.split(',').map(Number);
    const expectedTotal=lengths.reduce((sum,value)=>sum+value,0);
    let chunk=0,generated=0;
    state.allocations=[];state.enqueued=[];state.shapes=[];state.reads=0;state.total=0;
    const parts=new Array(expectedTotal+1),partLengths=new Array(expectedTotal+1);let partCount=0;
    const emit=controller=>{
        if(chunk===lengths.length){controller.close();return;}
        const view=new Uint8Array(lengths[chunk++]),buffer=view.buffer;
        for(let i=0;i<view.length;++i)view[i]=generated++%251;
        controller.enqueue(view);append(state.enqueued,{bufferBytes:buffer.byteLength,viewBytes:view.byteLength});
        if(chunk===lengths.length)controller.close();
    };
    const source={type:'bytes',
        start(controller){if(phase==='queue'){if(!lengths.length)controller.close();else while(chunk<lengths.length)emit(controller);}},
        pull(controller){append(state.allocations,controller.byobRequest?.view.byteLength??null);emit(controller);}};
    if(allocation!=='none')source.autoAllocateChunkSize=Number(allocation);
    const stream=new ReadableStream(source),reader=construct(stream,route);
    for(;;){
        const result=await reader.read(),view=result.value;
        const buffer=view?.buffer,length=view?.byteLength??null,offset=view?.byteOffset??null,capacity=buffer?.byteLength??null;
        appendShape(state.shapes,result.done,length,offset,capacity,view?outsideBytesZero(buffer,offset,length):null,
            view?.constructor.name??null,view?view instanceof Uint8Array&&Object.getPrototypeOf(view)===Uint8Array.prototype&&buffer instanceof ArrayBuffer:null);
        state.reads++;
        if(view){parts[partCount]=view;partLengths[partCount++]=length;state.total+=length;}
        if(result.done)break;
        if(state.reads>generated+1)throw new Error('No byte progress');
    }
    await reader.closed;
    const joined=new Uint8Array(state.total);let offset=0;
    for(let at=0;at<partCount;++at){
        const part=parts[at],length=partLengths[at];
        if(part.byteLength!==length)throw new Error('Retained view changed length');
        if(length===0)new Uint8Array(part.buffer);
        for(let index=0;index<length;++index)joined[offset+index]=part[index];
        offset+=length;
    }
    state.sha256=await digest(joined);reader.releaseLock();state.locked=stream.locked;
}
async function ownership(state,route,action,allocation) {
    let cached,view;
    const source={type:'bytes',pull(controller){
        view=controller.byobRequest?.view;cached=view?.buffer;state.before=cached?.byteLength??null;view?.fill(165);
        if(action==='close')controller.close();else return new Promise(()=>{});
    },cancel(){state.inCancel=cached?.byteLength??null;}};
    if(allocation!=='none')source.autoAllocateChunkSize=Number(allocation);
    const stream=new ReadableStream(source),reader=construct(stream,route),promise=reader.read();
    if(action==='cancel'){await new Promise(resolve=>setTimeout(resolve,0));await reader.cancel();}
    const result=await promise;
    state.after=cached?.byteLength??null;state.viewAfter=view?.byteLength??null;state.sameBuffer=result.value?.buffer===cached;
    state.read={done:result.done,bytes:result.value?.byteLength??null,capacity:result.value?.buffer.byteLength??null};
    const bytes=new Uint8Array(result.value.buffer);
    state.initialPrefix=Array.from(bytes.slice(0,8));state.initialSHA256=await digest(bytes);
    if(cached.byteLength)new Uint8Array(cached).fill(90);
    state.afterCachedPrefix=Array.from(bytes.slice(0,8));bytes.fill(204);
    state.cachedAfterReturnedFirst=cached.byteLength?new Uint8Array(cached)[0]:null;
    await reader.closed;reader.releaseLock();state.locked=stream.locked;
}
async function transfer(state,route,target,method,length) {
    let source,sourceView;
    const stream=new ReadableStream({type:'bytes',autoAllocateChunkSize:16,
        pull(controller){sourceView=controller.byobRequest.view;source=sourceView.buffer;sourceView.fill(165);controller.close();}});
    const reader=construct(stream,route),result=await reader.read(),returned=result.value.buffer;
    const selected=target==='source'?source:returned,alias=target==='source'?returned:source;
    state.before={done:result.done,valueLength:result.value.byteLength,source:source.byteLength,returned:returned.byteLength,
        sameBuffer:source===returned,sourcePrefix:prefix(source),returnedPrefix:prefix(returned)};
    state.available=typeof selected[method]==='function';state.error=null;let moved;
    if(state.available){try{moved=length==='none'?selected[method]():selected[method](Number(length));}
        catch(error){state.error={name:error.name,message:error.message};}}
    state.after={source:source.byteLength,returned:returned.byteLength,sourceView:sourceView.byteLength,
        valueLength:result.value.byteLength,moved:moved?.byteLength??null,aliasPrefix:prefix(alias),movedPrefix:moved?prefix(moved):null};
    if(moved?.byteLength)new Uint8Array(moved).fill(90);state.aliasAfterMovedWrite=prefix(alias);
    if(alias.byteLength)new Uint8Array(alias).fill(204);state.movedAfterAliasWrite=moved?prefix(moved):null;
    await reader.closed;reader.releaseLock();state.locked=stream.locked;
}
async function exercise(state,server) {
    try {
        const [,route,,operation]=state.kind.split('|'),[kind,a,b,c]=operation.split('_');
        if(kind==='queue'||kind==='pull')await shapes(state,route,kind,a,b);
        else if(kind==='buffer')await ownership(state,route,a,b);
        else if(kind==='transfer')await transfer(state,route,a,b,c);
        else throw new Error('Unknown standard reader operation');
    }catch(error){state.unexpected={name:error.name,message:error.message};}
    server.send('done');server.close(1000,'done');
}
export default {fetch(request){
    realm??=crypto.randomUUID();const url=new URL(request.url);
    if(url.pathname.endsWith('/state'))return Response.json({realm,state:current??null});
    const state=current={kind:url.searchParams.get('kind'),challenge:url.searchParams.get('challenge'),unexpected:null};
    const pair=new WebSocketPair(),server=pair[1];server.accept();
    server.addEventListener('message',event=>{if(event.data==='go'){server.send('started');void exercise(state,server);}});
    return new Response(null,{status:101,webSocket:pair[0]});
}};
