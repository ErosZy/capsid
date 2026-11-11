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
async function argumentCase(input){
    const {route,method,view:viewName}=input;
    const state={kind:input.kind,challenge:input.challenge,rows:[],unexpected:null};
    try{
        const scenarios=input.scenario?[input.scenario]:[...validationScenarios,...(method==='read'?['options-getter-error','options-null','options-primitive','getter-detaches-then-throws']:[])];
        for(const scenario of scenarios){
            const events=[],row={scenario,events};state.rows.push(row);
            const response=await fetch(input.target);
            const stream=response.body;
            row.response={status:response.status,contentLength:response.headers.get('content-length'),used:response.bodyUsed,locked:stream.locked};
            let reader=construct(stream,route),released=scenario.startsWith('released-');
            reader.closed.catch(()=>{});
            row.construction={instanceOfReader:reader instanceof ReadableStreamBYOBReader,
                constructorMatches:ReadableStreamBYOBReader.prototype.constructor===ReadableStreamBYOBReader,
                directPrototype:Object.getPrototypeOf(reader)===ReadableStreamBYOBReader.prototype,
                prototypeParent:Object.getPrototypeOf(Object.getPrototypeOf(reader))===ReadableStreamBYOBReader.prototype};
            if(released)reader.releaseLock();
            const sentinel=new Error('validation/'+state.challenge+'/'+scenario),buffer=new ArrayBuffer(32);new Uint8Array(buffer).fill(165);
            let view=scenario.startsWith('invalid-view')?{}:new globalThis[viewName](buffer,8,scenario.startsWith('empty-')?0:16/sizes[viewName]);
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
            row.afterCall={bufferBytes:buffer.byteLength,viewBytes:scenario.startsWith('invalid-view')?null:measure(()=>view.byteLength),used:response.bodyUsed,locked:stream.locked};
            if(pending)try{row.read={value:result(await pending)};}catch(error){row.read={error:failure(error)};}
            row.originalBacking=buffer.byteLength?Array.from(new Uint8Array(buffer)):null;
            if(released){reader=construct(stream,route);reader.closed.catch(()=>{});}
            if(!row.read?.value)row.retry=result(await reader.read(new Uint8Array(16)));
            row.remaining=[];
            let total=(row.read?.value??row.retry).byteLength;
            while(total<16){const read=result(await reader.read(new Uint8Array(16)));row.remaining.push(read);if(read.done)throw new Error('Retry bytes ended early');total+=read.byteLength;}
            row.eof=result(await reader.read(new Uint8Array(1)));
            const cancelled=await reader.cancel();row.cancelReturnedUndefined=cancelled===undefined;
            reader.releaseLock();row.lockedAfterRelease=stream.locked;row.bodyUsed=response.bodyUsed;
        }
    }catch(error){state.unexpected=describe(error);}
    return state;
}

export default {async fetch(request){
    const url=new URL(request.url);if(url.pathname==='/healthy')return new Response('healthy');
    const [route,view,method]=url.searchParams.get('selection').split(',');
    return Response.json(await argumentCase({route,view,method,kind:'native-arguments-'+route+'-'+view+'-'+method,
        challenge:'binding-arguments',target:url.searchParams.get('target')}));
}};
