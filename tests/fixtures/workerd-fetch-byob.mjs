let realm,current;
const sizes={Int8Array:1,Uint8Array:1,Uint8ClampedArray:1,Int16Array:2,Uint16Array:2,
    Int32Array:4,Uint32Array:4,Float16Array:2,Float32Array:4,Float64Array:8,BigInt64Array:8,BigUint64Array:8,DataView:1};
const describe=value=>({type:typeof value,name:value?.name??null,message:value?.message??null,
    primitive:value===null?'null':typeof value==='object'?null:String(value)});
const measure=read=>{try{return read();}catch(error){return {error:{name:error.name}};}};
const result=value=>({done:value.done,type:value.value?.constructor.name??null,byteOffset:value.value?.byteOffset??null,
    byteLength:value.value?.byteLength??null,bufferBytes:value.value?.buffer.byteLength??null,
    bytes:value.value?Array.from(new Uint8Array(value.value.buffer,value.value.byteOffset,value.value.byteLength)):null,
    backing:value.value?Array.from(new Uint8Array(value.value.buffer)):null});
function construct(stream,route,byob=true){
    const Reader=byob?ReadableStreamBYOBReader:ReadableStreamDefaultReader;
    if(route==='getReader')return byob?stream.getReader({mode:'byob'}):stream.getReader();
    if(route==='direct')return new Reader(stream);
    if(route==='prototype')return new Reader.prototype.constructor(stream);
    if(route==='subclass'){class Derived extends Reader{}return new Derived(stream);}
    if(route==='reflect'){function Derived(){}Derived.prototype=Object.create(Reader.prototype);
        return Reflect.construct(Reader,[stream],Derived);}
    throw new Error('Unknown reader route');
}
const validationScenarios=['capacity','capacity-first-count','zero','negative','signed-max','signed-min','range-high','range-low',
    'range-fraction','string-capacity','object-capacity','primitive-capacity','bigint','symbol','object-bigint','object-symbol',
    'zero-null','zero-nan','zero-infinity','zero-fraction','conversion-error','primitive-error','primitive-noncallable',
    'released-capacity','released-zero','released-object-capacity','released-conversion-error','release-during-conversion',
    'empty-capacity','empty-zero','detached-capacity','detach-during-conversion','invalid-view','invalid-view-conversion-error',
    'wrong-receiver','proxy-receiver','valid-default','valid-minimum','valid-fraction','valid-string'];
async function validationCase(state,server,route,method){
    try{
        const scenarios=[...validationScenarios,...(method==='read'?['options-getter-error','options-null','options-primitive','getter-detaches-then-throws']:[])];
        for(const scenario of scenarios){
            const events=[],row={scenario,events};state.rows.push(row);
            const stream=new ReadableStream({type:'bytes',start(controller){controller.enqueue(Uint8Array.from({length:16},(_,i)=>i+1));controller.close();}});
            let reader=construct(stream,route),released=scenario.startsWith('released-');
            reader.closed.catch(()=>{});
            row.construction={instanceOfReader:reader instanceof ReadableStreamBYOBReader,
                constructorMatches:ReadableStreamBYOBReader.prototype.constructor===ReadableStreamBYOBReader,
                directPrototype:Object.getPrototypeOf(reader)===ReadableStreamBYOBReader.prototype,
                prototypeParent:Object.getPrototypeOf(Object.getPrototypeOf(reader))===ReadableStreamBYOBReader.prototype};
            if(released)reader.releaseLock();
            const sentinel=new Error('validation/'+state.challenge+'/'+scenario),buffer=new ArrayBuffer(32);new Uint8Array(buffer).fill(165);
            let view=scenario.startsWith('invalid-view')?{}:new Float64Array(buffer,8,scenario.startsWith('empty-')?0:2);
            if(scenario==='detached-capacity')buffer.transfer();
            const minimum=()=>{
                if(['conversion-error','released-conversion-error','invalid-view-conversion-error'].includes(scenario))return {valueOf(){events.push('valueOf');throw sentinel;}};
                if(['object-capacity','released-object-capacity','object-bigint','object-symbol','detach-during-conversion','release-during-conversion'].includes(scenario))return {valueOf(){
                    events.push('valueOf');
                    if(scenario==='detach-during-conversion')buffer.transfer();
                    if(scenario==='release-during-conversion'){reader.releaseLock();released=true;}
                    return scenario==='object-bigint'?3n:scenario==='object-symbol'?Symbol('minimum'):3;
                }};
                if(scenario==='primitive-capacity'||scenario==='primitive-error')return {[Symbol.toPrimitive](hint){events.push('primitive:'+hint);return scenario==='primitive-error'?{}:3;}};
                if(scenario==='primitive-noncallable')return {[Symbol.toPrimitive]:3};
                if(scenario==='bigint')return 3n;if(scenario==='symbol')return Symbol('minimum');
                if(scenario==='zero-null')return null;if(scenario==='zero-nan')return NaN;if(scenario==='zero-infinity')return Infinity;
                if(scenario==='valid-default')return undefined;
                if(scenario==='string-capacity')return '3';if(scenario==='valid-string')return '2';
                return ({'capacity-first-count':17,zero:0,negative:-1,'signed-max':2147483647,'signed-min':-2147483648,
                    'range-high':2147483648,'range-low':-2147483649,'range-fraction':2147483647.9,'zero-fraction':0.9,
                    'released-zero':0,'empty-zero':0,'valid-minimum':2,'valid-fraction':2.9})[scenario]??3;
            };
            const options={get min(){
                events.push('get-min');
                if(scenario==='getter-detaches-then-throws')buffer.transfer();
                if(scenario==='options-getter-error'||scenario==='getter-detaches-then-throws')throw sentinel;
                return minimum();
            }};
            const target=scenario==='wrong-receiver'?{}:scenario==='proxy-receiver'?new Proxy(reader,{get(){events.push('proxy-get');throw new Error('unexpected proxy trap');}}):reader;
            const failure=error=>({...describe(error),isTypeError:error instanceof TypeError,isRangeError:error instanceof RangeError,sameSentinel:error===sentinel});
            let pending;
            try{
                pending=method==='least'?ReadableStreamBYOBReader.prototype.readAtLeast.call(target,minimum(),view):
                    ReadableStreamBYOBReader.prototype.read.call(target,view,scenario==='options-null'?null:scenario==='options-primitive'?3:options);
                events.push('returned');
            }catch(error){row.synchronous=failure(error);}
            row.afterCall={bufferBytes:buffer.byteLength,viewBytes:scenario.startsWith('invalid-view')?null:view.byteLength};
            if(pending)try{row.read={value:result(await pending)};}catch(error){row.read={error:failure(error)};}
            row.originalBacking=buffer.byteLength?Array.from(new Uint8Array(buffer)):null;
            if(released){reader=construct(stream,route);reader.closed.catch(()=>{});}
            if(!row.read?.value)row.retry=result(await reader.read(new Uint8Array(16)));
            row.eof=result(await reader.read(new Uint8Array(1)));
            const cancelled=await reader.cancel();row.cancelReturnedUndefined=cancelled===undefined;
            reader.releaseLock();row.lockedAfterRelease=stream.locked;
        }
    }catch(error){state.unexpected=describe(error);}
    server.send('done');server.close(1000,'done');
}
export function backingRangeFilled(buffer,start,end,fill){
    const bytes=new Uint8Array(buffer);let at=start;
    for(;at<end&&at%4;at++)if(bytes[at]!==fill)return false;
    const count=Math.floor((end-at)/4),word=fill*16843009;
    if(count){const words=new Uint32Array(buffer,at,count);
        for(let i=0;i<count;i++)if(words[i]!==word)return false;}
    at+=count*4;for(;at<end;at++)if(bytes[at]!==fill)return false;
    return true;
}
const patternBytes=new Uint8Array(254),patternWords=new Uint32Array(251);
for(let i=0;i<patternBytes.length;i++)patternBytes[i]=(i*29+17)%251;
const patternView=new DataView(patternBytes.buffer),littleEndian=new Uint8Array(new Uint32Array([1]).buffer)[0]===1;
for(let i=0;i<patternWords.length;i++)patternWords[i]=patternView.getUint32(i,littleEndian);
export function patternErrors(view,position){
    let errors=0,at=0;const end=view.byteLength;
    for(;at<end&&(view.byteOffset+at)%4;at++)if(view[at]!==((position+at)*29+17)%251)errors++;
    const count=Math.floor((end-at)/4);
    if(count){const words=new Uint32Array(view.buffer,view.byteOffset+at,count);
        for(let i=0;i<count;i++)if(words[i]!==patternWords[(position+at+i*4)%251]){
            for(let j=0;j<4;j++){const index=at+i*4+j;if(view[index]!==((position+index)*29+17)%251)errors++;}
        }
    }
    at+=count*4;for(;at<end;at++)if(view[at]!==((position+at)*29+17)%251)errors++;
    return errors;
}
async function bodyCase(state,url,server,route,method,phase){
    const length=phase==='empty'?0:phase==='small'?6:1048613,byob=method!=='default';
    let reader;
    try{
        const response=await fetch(url.searchParams.get('target'),{headers:{Upgrade:'websocket','X-Probe':state.challenge}});
        const stream=response.body,Reader=byob?ReadableStreamBYOBReader:ReadableStreamDefaultReader;
        reader=construct(stream,route,byob);
        state.construction={instanceOfReader:reader instanceof Reader,constructorMatches:Reader.prototype.constructor===Reader,
            directPrototype:Object.getPrototypeOf(reader)===Reader.prototype,
            prototypeParent:Object.getPrototypeOf(Object.getPrototypeOf(reader))===Reader.prototype};
        state.response={status:response.status,contentLength:response.headers.get('content-length')};
        state.input=byob?{type:'Uint8Array',byteOffset:16,byteLength:length+32,bufferBytes:length+64,min:length}:null;
        const closed=reader.closed.then(()=>{state.closed={fulfilled:true};},error=>{state.closed={error:describe(error)};});
        const all=new Uint8Array(length);
        // The byte count bounds this loop: every nonterminal read must advance.
        // Keep compact shapes so the observation itself stays below IPC limits.
        for(let index=0;index<=length+1;index++){
            let pending,input;
            if(byob){
                input=new ArrayBuffer(length+64);new Uint8Array(input).fill(165);
                const view=new Uint8Array(input,16,length+32);
                pending=method==='least'?reader.readAtLeast(length,view):reader.read(view,{min:length});
            }else pending=reader.read();
            const detached=input?.byteLength??null,value=await pending,view=value.value;
            let outside=null;
            if(view){
                if(!(view instanceof Uint8Array)||view.constructor.name!=='Uint8Array')state.unexpectedViews++;
                const buffer=new Uint8Array(view.buffer),fill=byob?165:0;
                outside=backingRangeFilled(buffer.buffer,0,view.byteOffset,fill)
                    &&backingRangeFilled(buffer.buffer,view.byteOffset+view.byteLength,buffer.byteLength,fill);
                state.invalidBytes+=patternErrors(view,state.total);
                all.set(view,state.total);state.total+=view.byteLength;
            }
            state.shapes.push([value.done,view?.byteOffset??null,view?.byteLength??null,view?.buffer.byteLength??null,detached,outside]);
            if(value.done)break;
            if(!view?.byteLength)throw new Error('Nonterminal read did not advance');
        }
        state.sha256=Array.from(new Uint8Array(await crypto.subtle.digest('SHA-256',all)),byte=>byte.toString(16).padStart(2,'0')).join('');
        await closed;state.bodyUsed=response.bodyUsed;reader.releaseLock();state.lockedAfterRelease=stream.locked;
    }catch(error){state.unexpected=describe(error);if(reader)try{await reader.cancel();reader.releaseLock();}catch{}}
    server.send('done');server.close(1000,'done');
}
async function readerBrandCase(state,url,server,route,byob){
    let reader;
    try{
        const response=await fetch(url.searchParams.get('target'),{headers:{Upgrade:'websocket','X-Probe':state.challenge}});
        const stream=response.body,Reader=byob?ReadableStreamBYOBReader:ReadableStreamDefaultReader;
        reader=construct(stream,route,byob);
        state.construction={instanceOfReader:reader instanceof Reader,constructorMatches:Reader.prototype.constructor===Reader,
            directPrototype:Object.getPrototypeOf(reader)===Reader.prototype,
            prototypeParent:Object.getPrototypeOf(Object.getPrototypeOf(reader))===Reader.prototype};
        const prime=await (byob?reader.read(new Uint8Array(1)):reader.read());
        state.prime={done:prime.done,type:prime.value?.constructor.name,bytes:Array.from(prime.value)};
        const closed=reader.closed.then(()=>{state.closed={fulfilled:true};},error=>{state.closed={error:describe(error)};});
        const pending=(byob?reader.read(new Uint8Array(16),{min:8}):reader.read()).then(
            value=>{state.read={value:result(value)};},error=>{state.read={error:describe(error)};});
        const wrong=byob?ReadableStreamDefaultReader.prototype:ReadableStreamBYOBReader.prototype;
        state.optionGets=0;state.attempts=[];state.afterReleaseAttempts=[];
        async function attempts(destination){
            const buffer=new ArrayBuffer(16),view=new Uint8Array(buffer),options={get min(){state.optionGets++;return 1;}};
            const operations=[['read',()=>wrong.read.call(reader,view,options)],
                ...(byob?[]:[['readAtLeast',()=>wrong.readAtLeast.call(reader,1,view)]]),
                ['closed',()=>Object.getOwnPropertyDescriptor(wrong,'closed').get.call(reader)],
                ['releaseLock',()=>wrong.releaseLock.call(reader)],['cancel',()=>wrong.cancel.call(reader,'invalid/'+state.challenge)]];
            for(const [name,invoke] of operations){
                const entry={name,synchronous:false};let value;
                try{value=invoke();}catch(error){entry.synchronous=true;entry.error=describe(error);destination.push(entry);continue;}
                try{entry.returnedUndefined=(await value)===undefined;}catch(error){entry.error=describe(error);}
                destination.push(entry);
            }
            return buffer.byteLength;
        }
        state.wrongInputBytes=await attempts(state.attempts);
        state.beforeCancel={readSettled:state.read!==null,closedSettled:state.closed!==null,locked:stream.locked};
        state.cancel={returnedUndefined:(await reader.cancel('valid/'+state.challenge))===undefined};
        await Promise.all([pending,closed]);state.bodyUsed=response.bodyUsed;reader.releaseLock();state.lockedAfterRelease=stream.locked;
        state.releasedWrongInputBytes=await attempts(state.afterReleaseAttempts);
    }catch(error){state.unexpected=describe(error);if(reader)try{await reader.cancel();reader.releaseLock();}catch{}}
    server.send('done');server.close(1000,'done');
}
const describeStandardClose=error=>({name:error?.name??null,message:error?.message??null});
const standardCloseResult=read=>({done:read.done,type:read.value?.constructor.name??null,offset:read.value?.byteOffset??null,
    bytes:read.value?Array.from(new Uint8Array(read.value.buffer,read.value.byteOffset,read.value.byteLength)):null,
    backing:read.value?Array.from(new Uint8Array(read.value.buffer)):null});
async function boundStandardClose(promise){
    let timer;try{return await Promise.race([promise.then(value=>({value}),error=>({error:describeStandardClose(error)})),
        new Promise(resolve=>{timer=setTimeout(()=>resolve({pending:true}),150);})]);}finally{clearTimeout(timer);}
}
async function measureStandardClose(type,phase,length,method,route){
    const row={length,method,trace:[],read:null,eof:null,closed:null,closeThrow:null,cancel:null,unexpected:null};
    let controller,reader,readError,closedError,cancelError;
    const payload=Uint8Array.from({length},(_,i)=>i+1);
    const stream=new ReadableStream({type:'bytes',start(value){controller=value;controller.enqueue(new Uint8Array([0]));}});
    try{
        reader=construct(stream,route);row.prime=standardCloseResult(await reader.read(new Uint8Array(1)));
        if(phase==='queued'){
            if(payload.length)controller.enqueue(payload);
            try{controller.close();row.trace.push('source-close');}catch(error){row.closeThrow=describeStandardClose(error);}
        }
        const buffer=new ArrayBuffer(64);new Uint8Array(buffer).fill(165);
        const view=new globalThis[type](buffer,8,32/globalThis[type].BYTES_PER_ELEMENT);
        const closed=reader.closed.then(()=>{row.trace.push('closed');return {fulfilled:true};},error=>{
            closedError=error;row.trace.push('closed-error');throw error;
        });closed.catch(()=>{});
        const read=(method==='default'?reader.read(view):method==='read'?reader.read(view,{min:2.9}):reader.readAtLeast(2.9,view)).then(value=>{
            row.trace.push('read');return standardCloseResult(value);
        },error=>{readError=error;row.trace.push('read-error');throw error;});read.catch(()=>{});
        row.detached=buffer.byteLength===0;
        if(phase==='pending'){
            if(payload.length)controller.enqueue(payload);
            try{controller.close();row.trace.push('source-close');}catch(error){row.closeThrow=describeStandardClose(error);}
        }
        row.read=await boundStandardClose(read);row.closed=await boundStandardClose(closed);
        if(row.read.value)row.eof=await boundStandardClose(reader.read(new Uint8Array(32)).then(standardCloseResult));
        row.beforeCleanup=structuredClone({read:row.read,closed:row.closed,trace:row.trace});
        row.trace.push('cleanup');
        try{await reader.cancel();row.cancel={returnedUndefined:true};}catch(error){cancelError=error;row.cancel={error:describeStandardClose(error)};}
        row.errorIdentity={readClosed:readError===closedError,readCancel:readError===cancelError};
        reader.releaseLock();row.lockedAfterRelease=stream.locked;
    }catch(error){row.unexpected=describeStandardClose(error);if(reader)try{await reader.cancel();reader.releaseLock();}catch{}}
    return row;
}

export default {fetch(request){
    realm??=crypto.randomUUID();const url=new URL(request.url);
    if(url.pathname.endsWith('/state'))return Response.json({realm,state:current??null});
    const kind=url.searchParams.get('kind'),challenge=url.searchParams.get('challenge');
    const temporal=/^standard-close-(getReader|direct)-(queued|pending)-(Uint8Array|Uint16Array|Int32Array|Float64Array)$/.exec(kind);
    if(temporal){
        const state=current={kind,challenge,rows:[],unexpected:null};
        const pair=new WebSocketPair(),server=pair[1];server.accept();
        server.addEventListener('message',async event=>{
            if(event.data!=='go')return;server.send('started');
            try{for(const length of [0,1,3,4,5,7,8,9,12,15,16])for(const method of ['default','read','least']){
                state.rows.push(await measureStandardClose(temporal[3],temporal[2],length,method,temporal[1]));
            }}catch(error){state.unexpected=describeStandardClose(error);}
            server.send('done');server.close(1000,'done');
        });return new Response(null,{status:101,webSocket:pair[0]});
    }
    const body=/^body-(getReader|direct|prototype|subclass|reflect)-(default|minimum|least)-(empty|small|large)$/.exec(kind);
    if(body){
        const state=current={kind,challenge,shapes:[],total:0,invalidBytes:0,unexpectedViews:0,closed:null,unexpected:null};
        const pair=new WebSocketPair(),server=pair[1];server.accept();
        server.addEventListener('message',async event=>{
            if(event.data!=='go')return;server.send('started');await bodyCase(state,url,server,body[1],body[2],body[3]);
        });return new Response(null,{status:101,webSocket:pair[0]});
    }
    const brand=/^brand-(getReader|direct|prototype|subclass|reflect)-(default|byob)-held$/.exec(kind);
    if(brand){
        const state=current={kind,challenge,read:null,closed:null,cancel:null,unexpected:null};
        const pair=new WebSocketPair(),server=pair[1];server.accept();
        server.addEventListener('message',async event=>{
            if(event.data!=='go')return;server.send('started');await readerBrandCase(state,url,server,brand[1],brand[2]==='byob');
        });return new Response(null,{status:101,webSocket:pair[0]});
    }
    const validation=/^validation-(getReader|direct|prototype|subclass|reflect)-(read|least)$/.exec(kind);
    if(validation){
        const state=current={kind,challenge,rows:[],unexpected:null};const pair=new WebSocketPair(),server=pair[1];server.accept();
        server.addEventListener('message',async event=>{if(event.data!=='go')return;server.send('started');await validationCase(state,server,validation[1],validation[2]);});
        return new Response(null,{status:101,webSocket:pair[0]});
    }
    const match=/^(native|standard)-(getReader|direct|prototype|subclass|reflect)-(Int8Array|Uint8Array|Uint8ClampedArray|Int16Array|Uint16Array|Int32Array|Uint32Array|Float16Array|Float32Array|Float64Array|BigInt64Array|BigUint64Array|DataView)-(default|min2|min3|least2|least3)-(held|eof)$/.exec(kind);
    if(!match)return new Response('Unknown case',{status:400});
    const [,source,route,viewName,method,phase]=match,minimum=method==='default'?1:Number(method.at(-1));
    const state=current={kind,challenge,trace:[],read:null,eofRead:null,closed:null,cancel:null,unexpected:null};
    const pair=new WebSocketPair(),server=pair[1];server.accept();
    server.addEventListener('message',async event=>{
        if(event.data!=='go')return;server.send('started');let reader,readError,closedError,cancelError;
        try{
            let stream,response;
            if(source==='native'){
                response=await fetch(url.searchParams.get('target'),{headers:{Upgrade:'websocket','X-Probe':challenge}});
                stream=response.body;
            }else stream=new ReadableStream({type:'bytes',start(controller){
                controller.enqueue(new Uint8Array([0,102,105,114,115,116]));if(phase==='eof')controller.close();
            }});
            reader=construct(stream,route);
            state.construction={instanceOfReader:reader instanceof ReadableStreamBYOBReader,
                constructorMatches:ReadableStreamBYOBReader.prototype.constructor===ReadableStreamBYOBReader,
                directPrototype:Object.getPrototypeOf(reader)===ReadableStreamBYOBReader.prototype,
                prototypeParent:Object.getPrototypeOf(Object.getPrototypeOf(reader))===ReadableStreamBYOBReader.prototype};
            state.prime=result(await reader.read(new Uint8Array(1)));
            if(state.prime.done||JSON.stringify(state.prime.bytes)!=='[0]')throw new Error('Leading byte was not consumed');
            const buffer=new ArrayBuffer(32);new Uint8Array(buffer).fill(165);
            const view=new globalThis[viewName](buffer,8,16/sizes[viewName]);
            state.input={type:view.constructor.name,byteOffset:view.byteOffset,byteLength:view.byteLength,bufferBytes:buffer.byteLength};
            const closed=reader.closed.then(()=>{state.closed={fulfilled:true};state.trace.push('closed');},
                error=>{closedError=error;state.closed={error:describe(error)};state.trace.push('closed-error');});
            let promise;
            try{promise=method.startsWith('least')?reader.readAtLeast(minimum,view):method==='default'?reader.read(view):reader.read(view,{min:minimum});}
            catch(error){promise=Promise.reject(error);state.synchronousThrow=true;}
            state.afterCall={bufferBytes:buffer.byteLength,viewBytes:measure(()=>view.byteLength)};
            const pending=promise.then(value=>{state.read={value:result(value)};state.trace.push('read');},
                error=>{readError=error;state.read={error:describe(error)};state.trace.push('read-error');});
            const minimumBytes=minimum*sizes[viewName];
            // Positive reads await their actual result. A negative held read
            // starts only after the same reader consumed the leading byte.
            if(minimumBytes<=16&&(phase==='eof'||minimumBytes<=5)){
                let timer;try{await Promise.race([pending,new Promise((_,reject)=>{
                    timer=setTimeout(()=>reject(new Error('Read did not settle after the byte checkpoint')),3000);
                })]);}finally{clearTimeout(timer);}
            }else await new Promise(resolve=>setTimeout(resolve,30));
            state.beforeCancel={readSettled:state.read!==null,closedSettled:state.closed!==null};
            if(phase==='eof'&&state.read?.value){
                state.eofRead=await reader.read(new Uint8Array(16)).then(value=>({value:result(value)}),error=>({error:describe(error)}));
            }
            try{const value=await reader.cancel({challenge});state.cancel={returnedUndefined:value===undefined};state.trace.push('cancel');}
            catch(error){cancelError=error;state.cancel={error:describe(error)};state.trace.push('cancel-error');}
            await Promise.all([pending,closed]);
            if(source==='standard'&&phase==='eof'&&minimumBytes>5&&minimumBytes<=16&&sizes[viewName]<=5){
                state.errorIdentity={readClosed:readError===closedError,readCancel:readError===cancelError};
            }
            state.bodyUsed=response?.bodyUsed??null;reader.releaseLock();state.lockedAfterRelease=stream.locked;
        }catch(error){state.unexpected=describe(error);if(reader)try{await reader.cancel();reader.releaseLock();}catch{}}
        server.send('done');server.close(1000,'done');
    });
    return new Response(null,{status:101,webSocket:pair[0]});
}};
