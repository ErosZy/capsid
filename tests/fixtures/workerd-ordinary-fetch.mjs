// Ordinary HTTP response probes. The WebSocket holds the owning request open.
let realm,current;
const describe=error=>({name:error?.name??null,message:error?.message??null});
const metadata=response=>({status:response.status,statusText:response.statusText,ok:response.ok,url:response.url,
    type:response.type,redirected:response.redirected,bodyNull:response.body===null,bodyUsed:response.bodyUsed,
    locked:response.body?.locked??null,bodyStable:response.body===response.body,headers:[...response.headers]});
async function drain(stream){
    if(stream===null)return {bytes:[],chunks:[],closed:null,lockedAfter:null};
    const reader=stream.getReader(),chunks=[],bytes=[];let closed=null;
    const watched=reader.closed.then(()=>{closed={fulfilled:true};},error=>{closed={error:describe(error)};});
    for(let index=0;;++index){
        if(index>1000)throw new Error('unbounded finite-body probe');
        const result=await reader.read();
        chunks.push({done:result.done,type:result.value?.constructor.name??null,length:result.value?.byteLength??null});
        if(result.value)bytes.push(...new Uint8Array(result.value.buffer,result.value.byteOffset,result.value.byteLength));
        if(result.done)break;
    }
    await watched;reader.releaseLock();return {bytes,chunks,closed,lockedAfter:stream.locked};
}
async function executeCore(url,state,server){
    try{
        const options={redirect:state.kind==='ordinary-meta-redirect-manual'?'manual':'follow'};
        if(state.kind==='ordinary-meta-head'||state.kind==='ordinary-clone-null-head')options.method='HEAD';
        const response=await fetch(url.searchParams.get('target'),options);
        state.before=metadata(response);
        try{response.headers.set('X-Diagnostic-Mutation','yes');state.headerMutation={accepted:true,value:response.headers.get('X-Diagnostic-Mutation')};response.headers.delete('X-Diagnostic-Mutation');}
        catch(error){state.headerMutation={accepted:false,error:describe(error)};}
        if(state.kind.startsWith('ordinary-meta-'))state.result={bytes:Array.from(new Uint8Array(await response.arrayBuffer()))};
        else if(state.kind==='ordinary-clone-both'){
            const copy=response.clone();state.copyBefore=metadata(copy);state.originalAfterClone=metadata(response);
            state.result=await Promise.all([drain(response.body),drain(copy.body)]);state.copyAfter=metadata(copy);
        }else if(state.kind==='ordinary-tee-both'){
            const branches=response.body.tee();state.originalAfterTee=metadata(response);
            state.result=await Promise.all(branches.map(drain));
        }else if(state.kind==='ordinary-tee-cancel-one'){
            const [a,b]=response.body.tee();state.originalAfterTee=metadata(response);let canceled=false;
            const cancel=a.cancel({tag:'cancel-left'}).then(()=>{canceled=true;});
            await Promise.resolve();state.cancelBeforeOtherDrain=canceled;
            state.result=await drain(b);await cancel;state.cancelAfterOtherDrain=canceled;
            state.canceledBranchLocked=a.locked;
        }else if(state.kind.startsWith('ordinary-values-')){
            const preventCancel=state.kind==='ordinary-values-prevent-cancel-break';
            const iterator=response.body.values(preventCancel?{preventCancel:true}:undefined),bytes=[];
            state.lockedAfterValues=response.body.locked;
            for await(const chunk of iterator){bytes.push(...chunk);if(state.kind!=='ordinary-values-full')break;}
            state.afterIterator=metadata(response);state.result={bytes};
            if(preventCancel)state.remaining=await drain(response.body);
        }else if(state.kind==='ordinary-pipeTo'){
            const bytes=[],events=[];
            const sink=new WritableStream({write(chunk){events.push('write');bytes.push(...chunk);},close(){events.push('close');},abort(error){events.push({abort:describe(error)});}});
            const result=await response.body.pipeTo(sink);state.result={bytes,events,returnedUndefined:result===undefined,sinkLocked:sink.locked};
        }else if(state.kind==='ordinary-pipeThrough'){
            const events=[];
            const transform=new TransformStream({transform(chunk,controller){events.push('transform');controller.enqueue(Uint8Array.from(chunk,value=>(value+1)&255));},flush(){events.push('flush');}});
            const result=response.body.pipeThrough(transform);state.afterPipeThrough=metadata(response);
            state.result=await drain(result);state.transform={events,writableLocked:transform.writable.locked,readableLocked:transform.readable.locked};
        }else if(state.kind==='ordinary-clone-after-lock'){
            const reader=response.body.getReader();state.lockedBeforeClone=metadata(response);
            try{response.clone();state.cloneError=null;}catch(error){state.cloneError=describe(error);}
            reader.releaseLock();state.result=await drain(response.body);
        }else if(state.kind==='ordinary-clone-after-read'){
            const reader=response.body.getReader(),first=await reader.read();state.first={done:first.done,bytes:Array.from(first.value??[])};
            try{response.clone();state.cloneError=null;}catch(error){state.cloneError=describe(error);}
            reader.releaseLock();state.result=await drain(response.body);

        }else if(state.kind==='ordinary-clone-used-unlocked'||state.kind==='ordinary-clone-eof-unlocked'){
            if(state.kind==='ordinary-clone-eof-unlocked')state.beforeCloneRead=await drain(response.body);
            else{const reader=response.body.getReader(),first=await reader.read();state.beforeCloneRead={bytes:Array.from(first.value??[]),done:first.done};reader.releaseLock();}
            state.beforeClone=metadata(response);let copy;
            try{copy=response.clone();state.cloneError=null;state.copyBefore=metadata(copy);}catch(error){state.cloneError=describe(error);}
            state.result=await drain(response.body);if(copy)state.copyResult=await drain(copy.body);
        }else if(state.kind==='ordinary-clone-null-head'||state.kind==='ordinary-clone-redirect-follow'){
            const copy=response.clone();state.copyBefore=metadata(copy);state.originalAfterClone=metadata(response);
            state.result=await Promise.all([drain(response.body),drain(copy.body)]);state.copyAfter=metadata(copy);
        }else if(state.kind==='ordinary-headers-copies'){
            const copies=[new Headers(response.headers),new Response(null,{headers:response.headers}).headers];
            state.copies=[];
            for(const headers of copies){
                const item={};
                try{headers.set('X-Copy','yes');headers.append('X-Copy','again');item.value=headers.get('X-Copy');headers.delete('X-Copy');item.afterDelete=headers.get('X-Copy');}
                catch(error){item.error=describe(error);}state.copies.push(item);
            }
            const copy=response.clone();
            try{copy.headers.set('X-Copy','yes');state.cloneHeaderMutation={accepted:true};}
            catch(error){state.cloneHeaderMutation={accepted:false,error:describe(error)};}
            state.result=await Promise.all([drain(response.body),drain(copy.body)]);
        }else if(state.kind==='ordinary-headers-conversions'||state.kind==='ordinary-headers-mutable-conversions'){
            const operations=['append','set','delete'],variants=['plain','missing','name-empty','name-space','name-symbol','value-symbol','name-object','value-object','name-throw','value-throw','value-crlf','receiver','missing-value','undefined','null','name-object-symbol','value-object-symbol','name-primitive-noncallable','name-wide','value-wide','name-lookalike','value-lookalike','receiver-name-object','receiver-name-throw'];
            state.operations=[];
            for(const operation of operations)for(const variant of variants){
                const events=[],marker=variant.endsWith('lookalike')?Object.assign(new TypeError('Cannot convert a Symbol value to a string'),{tag:'header-conversion'}):{tag:'header-conversion'};let name='X-Conversion',value='value',receiver=state.kind==='ordinary-headers-mutable-conversions'?new Headers():response.headers;
                const convert=(label,result,throws=false)=>({[Symbol.toPrimitive](hint){events.push(label+'/'+hint);if(throws)throw marker;return result;}});
                if(variant==='name-empty')name='';if(variant==='name-space')name='bad name';
                if(variant==='name-symbol')name=Symbol('name');if(variant==='value-symbol')value=Symbol('value');
                if(variant==='name-object')name=convert('name','X-Conversion');if(variant==='value-object')value=convert('value','value');
                if(variant==='name-throw')name=convert('name','X-Conversion',true);if(variant==='value-throw')value=convert('value','value',true);
                if(variant==='value-crlf')value='bad\r\nvalue';if(variant==='receiver')receiver={};
                if(variant==='undefined'){name=undefined;value=undefined;}if(variant==='null'){name=null;value=null;}
                if(variant==='name-object-symbol')name=convert('name',Symbol('name'));if(variant==='value-object-symbol')value=convert('value',Symbol('value'));
                if(variant==='name-primitive-noncallable')name={[Symbol.toPrimitive]:42};
                if(variant==='name-wide')name='\u0100';if(variant==='value-wide')value='\u0100';
                if(variant==='name-lookalike')name=convert('name','X-Conversion',true);if(variant==='value-lookalike')value=convert('value','value',true);
                if(variant==='receiver-name-object'){receiver={};name=convert('name','X-Conversion');}
                if(variant==='receiver-name-throw'){receiver={};name=convert('name','X-Conversion',true);}
                const item={operation,variant,events};
                try{const result=Reflect.apply(Headers.prototype[operation],receiver,variant==='missing'?[]:variant==='missing-value'?[name]:[name,value]);item.result={returnedUndefined:result===undefined,headers:[...receiver]};}
                catch(error){item.error={...describe(error),sameMarker:error===marker};}
                state.operations.push(item);
            }
            state.result=await drain(response.body);
        }else if(state.kind==='ordinary-tee-after-lock'){
            const reader=response.body.getReader();
            try{response.body.tee();state.teeError=null;}catch(error){state.teeError=describe(error);}
            reader.releaseLock();state.result=await drain(response.body);

        }else if(state.kind==='ordinary-headers-unicode-names'){
            const chars=['\u0080','\u00c0','\u00df','\u0100','\u0120','\u017f','\u0301','\u0391','\u4e2d','\ud83d\ude42','\ufeff','\ud800','\udc00'];state.names=[];
            for(const character of chars)for(let offset=0;offset<10;++offset){
                const name='AZ'.repeat(offset).slice(0,offset)+character+'AZ',headers=new Headers(),item={name};
                try{headers.append(name,'first');headers.append(name,'second');item.entries=[...headers];item.get=headers.get(name);item.has=headers.has(name);
                    const copy=new Headers(headers);item.copyEntries=[...copy];item.copyGet=copy.get(name);headers.delete(name);item.afterDelete=[...headers];}
                catch(error){item.error=describe(error);}state.names.push(item);
            }
            state.result=await drain(response.body);
        }else if(state.kind==='ordinary-headers-unicode-copies'){
            const names=['\u0100','\u0120','\u0140','\u0160','\u4e2d','\ud83d\ude42','X-A','x-a','set-cookie'],headers=new Headers();
            names.forEach((name,index)=>headers.append(name,'v'+index));headers.append('Set-Cookie','next');
            headers[Symbol.iterator]=function*(){yield ['x-application-iterator','override'];};
            const copies=[headers,new Headers(headers),new Response(null,{headers}).headers,new Request('http://localhost/',{headers}).headers];
            const constructed=new Response(null,{headers});copies.push(constructed.clone().headers);
            state.copies=copies.map(copy=>({entries:Array.from(Headers.prototype.entries.call(copy)),gets:names.map(name=>copy.get(name)),cookies:copy.getSetCookie()}));
            state.result=await drain(response.body);
        }else if(state.kind==='ordinary-headers-value-edges'){
            state.values=[];
            for(const value of ['', '\r\n x \r\n','\t\t','\u000b','\u000c','x\u0000y','x\ry','x\ny','\u0100','\ud800','\udc00','\ufeff','\u00a0']){
                const item={value};try{const headers=new Headers([['x-value',value]]);item.entries=[...headers];item.get=headers.get('x-value');}catch(error){item.error=describe(error);}state.values.push(item);
            }
            for(const operation of ['append','set']){const headers=new Headers();try{headers[operation]('bad name','bad\r\nvalue');state.values.push({operation,accepted:true});}catch(error){state.values.push({operation,error:describe(error)});}}
            state.result=await drain(response.body);
        }else if(state.kind==='ordinary-headers-iteration-snapshot'){
            state.iterators=[];
            for(const kind of ['entries','keys','values']){
                const headers=new Headers([['x-a','a'],['x-b','b']]),iterator=headers[kind]();headers.set('x-a','changed');headers.delete('x-b');headers.set('x-c','c');
                state.iterators.push({kind,values:Array.from(iterator),current:[...headers]});
            }
            const headers=new Headers([['x-a','a'],['x-b','b']]),events=[];
            headers.forEach((value,name,owner)=>{events.push({name,value,sameOwner:owner===headers});headers.delete('x-b');headers.set('x-c','c');});
            state.forEach={events,current:[...headers]};state.result=await drain(response.body);

        }else if(state.kind.startsWith('ordinary-held-')){
            const marker={tag:'held-consumer-stop'},resultOf=value=>({returnedUndefined:value===undefined});
            if(state.kind==='ordinary-held-tee-cancel-both'||state.kind==='ordinary-held-clone-cancel-both'){
                let branches;if(state.kind==='ordinary-held-clone-cancel-both'){const copy=response.clone();branches=[response.body,copy.body];}else branches=response.body.tee();
                state.firstReads=[];
                for(const branch of branches){const reader=branch.getReader(),value=await reader.read();state.firstReads.push({done:value.done,bytes:Array.from(value.value??[])});reader.releaseLock();}
                let leftDone=false;const left=branches[0].cancel(marker).then(value=>{leftDone=true;return resultOf(value);});
                await Promise.resolve();state.firstCancelSettledBeforeSecond=leftDone;
                const right=branches[1].cancel(marker);state.cancelResults=await Promise.all([left,right.then(resultOf)]);
                state.branchLocks=branches.map(branch=>branch.locked);
            }else if(state.kind==='ordinary-held-values-break'){
                state.values=[];for await(const value of response.body){state.values.push(Array.from(value));break;}
            }else if(state.kind==='ordinary-held-pipeTo-abort'){
                const abort=new AbortController(),events=[];
                const sink=new WritableStream({write(value){events.push({write:Array.from(value)});abort.abort(marker);},abort(error){events.push({abort:describe(error),sameMarker:error===marker});}});
                try{await response.body.pipeTo(sink,{signal:abort.signal});state.pipeResult={fulfilled:true};}
                catch(error){state.pipeResult={error:describe(error),sameMarker:error===marker};}
                state.pipeEvents=events;state.sinkLocked=sink.locked;
            }else if(state.kind==='ordinary-held-pipeThrough-cancel'){
                const transformed=response.body.pipeThrough(new TransformStream({transform(value,controller){controller.enqueue(value);}})),reader=transformed.getReader(),first=await reader.read();
                state.first={done:first.done,bytes:Array.from(first.value??[])};state.cancelResult=resultOf(await reader.cancel(marker));reader.releaseLock();state.transformedLocked=transformed.locked;

            }else if(state.kind==='ordinary-held-pipeThrough-prevent-cancel'){
                const transformed=response.body.pipeThrough(new TransformStream({transform(value,controller){controller.enqueue(value);}}),{preventCancel:true}),reader=transformed.getReader(),first=await reader.read();
                state.first={done:first.done,bytes:Array.from(first.value??[])};state.cancelResult=resultOf(await reader.cancel(marker));reader.releaseLock();state.transformedLocked=transformed.locked;
            }else if(state.kind==='ordinary-held-pipeThrough-mixed-pair'){
                const detachedReadable=new ReadableStream({start(controller){controller.enqueue(Uint8Array.from({length:17},(_,index)=>(index*29+17)%251));}}),sink=new WritableStream({write(){}}),abort=new AbortController();
                const transformed=response.body.pipeThrough({readable:detachedReadable,writable:sink},{signal:abort.signal}),reader=transformed.getReader(),first=await reader.read();
                state.first={done:first.done,bytes:Array.from(first.value??[])};state.cancelResult=resultOf(await reader.cancel(marker));reader.releaseLock();
                state.beforeAbort={sameReadable:transformed===detachedReadable,sourceLocked:response.body.locked};abort.abort(marker);
                const until=Date.now()+1000;while(response.body.locked&&Date.now()<until)await new Promise(resolve=>setTimeout(resolve,1));
                state.afterAbortLocked=response.body.locked;
            }else if(state.kind==='ordinary-held-pipeThrough-standard-source'){
                const events=[],source=new ReadableStream({start(controller){controller.enqueue(Uint8Array.from({length:17},(_,index)=>(index*29+17)%251));},cancel(reason){events.push({cancel:true,sameMarker:reason===marker});}});
                const transformed=source.pipeThrough(new TransformStream()),reader=transformed.getReader(),first=await reader.read();
                state.first={done:first.done,bytes:Array.from(first.value??[])};state.cancelResult=resultOf(await reader.cancel(marker));reader.releaseLock();state.standardSource={locked:source.locked,events};
                await response.body.cancel(marker);

            }else if(state.kind==='ordinary-held-pipeThrough-transform-cancel-throw'||state.kind==='ordinary-held-pipeThrough-transform-cancel-reject'){
                const events=[],transform=new TransformStream({transform(value,controller){controller.enqueue(value);},cancel(reason){events.push({sameMarker:reason===marker});if(state.kind.endsWith('-throw'))throw marker;return Promise.reject(marker);}});
                const transformed=response.body.pipeThrough(transform),reader=transformed.getReader(),first=await reader.read();state.first={done:first.done,bytes:Array.from(first.value??[])};
                try{state.cancelResult=resultOf(await reader.cancel(marker));}catch(error){state.cancelError={...describe(error),sameMarker:error===marker};}
                reader.releaseLock();state.transformCancel={events,writableLocked:transform.writable.locked,readableLocked:transformed.locked};
            }else if(state.kind==='ordinary-held-pipeThrough-standard-cancel-reject'){
                const events=[],source=new ReadableStream({start(controller){controller.enqueue(Uint8Array.from({length:17},(_,index)=>(index*29+17)%251));},cancel(reason){events.push({sameMarker:reason===marker});return Promise.reject(marker);}});
                const transform=new TransformStream(),transformed=source.pipeThrough(transform),reader=transformed.getReader(),first=await reader.read();state.first={done:first.done,bytes:Array.from(first.value??[])};
                try{state.cancelResult=resultOf(await reader.cancel(marker));}catch(error){state.cancelError={...describe(error),sameMarker:error===marker};}
                reader.releaseLock();state.standardSource={locked:source.locked,events};await response.body.cancel(marker);
            }else if(state.kind==='ordinary-held-pipeThrough-nested'){
                const firstTransform=new TransformStream(),secondTransform=new TransformStream(),middle=response.body.pipeThrough(firstTransform),transformed=middle.pipeThrough(secondTransform),reader=transformed.getReader(),first=await reader.read();
                state.first={done:first.done,bytes:Array.from(first.value??[])};state.cancelResult=resultOf(await reader.cancel(marker));reader.releaseLock();
                state.pipelineLocks={source:response.body.locked,middle:middle.locked,output:transformed.locked,firstWritable:firstTransform.writable.locked,secondWritable:secondTransform.writable.locked};
            }else if(state.kind==='ordinary-held-pipeThrough-self-feedback'){
                const abort=new AbortController(),transform=new TransformStream({start(controller){controller.enqueue(new Uint8Array([17]));},transform(){abort.abort(marker);}}),source=transform.readable;
                const transformed=source.pipeThrough(transform,{signal:abort.signal});state.selfReadable=transformed===source;
                const until=Date.now()+1000;while(source.locked&&Date.now()<until)await new Promise(resolve=>setTimeout(resolve,1));
                state.selfSourceLocked=source.locked;state.selfWritableLocked=transform.writable.locked;
                state.selfProbeBudgetMs=1000;await response.body.cancel(marker);
            }else{
                const reader=response.body.getReader(),first=await reader.read();state.first={done:first.done,bytes:Array.from(first.value??[])};
                state.cancelResult=resultOf(await reader.cancel(marker));reader.releaseLock();
            }
        }else if(state.kind.startsWith('ordinary-native-shape-')){
            const [, , , route, method]=state.kind.split('-'),byob=route!=='default';
            let reader;
            if(!byob)reader=response.body.getReader();
            else{
                const Base=ReadableStreamBYOBReader;class Derived extends Base{}
                reader=route==='getReader'?response.body.getReader({mode:'byob'}):route==='direct'?new Base(response.body):route==='prototype'?new Base.prototype.constructor(response.body):route==='subclass'?new Derived(response.body):Reflect.construct(Base,[response.body],Derived);
            }
            state.readerBrand={byob:reader instanceof ReadableStreamBYOBReader,default:reader instanceof ReadableStreamDefaultReader};
            state.reads=[];const closed=reader.closed;state.closedStable=closed===reader.closed;let settled=false;closed.then(()=>{settled=true;},()=>{});
            for(let index=0;;++index){
                if(index>1000)throw new Error('unbounded finite body probe');
                const input=byob?new ArrayBuffer(32):null;if(input)new Uint8Array(input).fill(165);
                const promise=byob?(method==='least'?reader.readAtLeast(4,new Uint8Array(input,8,16)):reader.read(new Uint8Array(input,8,16),{min:4})):reader.read();
                const after=input?.byteLength??null,result=await promise,value=result.value;
                state.reads.push({inputAfter:after,done:result.done,type:value?.constructor.name??null,offset:value?.byteOffset??null,
                    length:value?.byteLength??null,capacity:value?.buffer.byteLength??null,bytes:value?Array.from(value):null,
                    backing:value?Array.from(new Uint8Array(value.buffer)):null});
                if(result.done)break;
            }
            await closed;state.closedSettled=settled;reader.releaseLock();state.result={bytes:state.reads.flatMap(value=>value.bytes??[])};
        }else throw new Error('unknown ordinary fetch probe');
        state.after=metadata(response);
    }catch(error){state.failure=describe(error);}
    server.send('done');server.close(1000,'done');
}

async function executeGated(url,state,server){
    try{
        const response=await fetch(url.searchParams.get('target'));
        state.before={status:response.status,url:response.url,redirected:response.redirected,bodyUsed:response.bodyUsed,locked:response.body.locked};
        const byob=state.kind.includes('-byob-'),cancel=state.kind.endsWith('-cancel');
        const reader=response.body.getReader(byob?{mode:'byob'}:undefined);
        state.readerBrand={byob:reader instanceof ReadableStreamBYOBReader,default:reader instanceof ReadableStreamDefaultReader};
        const closed=reader.closed;state.closedStable=closed===reader.closed;
        let closedState='pending';closed.then(()=>closedState='fulfilled',error=>closedState=describe(error));
        state.reads=[];state.result={bytes:[]};server.send('headers');
        for(let index=0;;++index){
            if(index>100)throw new Error('bounded gated-body diagnostic');
            const input=byob?new ArrayBuffer(32):null;if(input)new Uint8Array(input).fill(165);
            const promise=byob?reader.read(new Uint8Array(input,8,16),{min:4}):reader.read();
            const inputAfter=input?.byteLength??null,result=await promise,value=result.value;
            state.reads.push({inputAfter,done:result.done,type:value?.constructor.name??null,offset:value?.byteOffset??null,
                length:value?.byteLength??null,capacity:value?.buffer.byteLength??null,bytes:value?Array.from(value):null,
                backing:value?Array.from(new Uint8Array(value.buffer)):null});
            state.result.bytes.push(...(value??[]));
            if(result.done)break;
            server.send('read/'+index+'/'+value.byteLength);
            if(cancel){state.cancelReturnedUndefined=(await reader.cancel({tag:'gated-cancel'}))===undefined;break;}
        }
        await closed;state.closed=closedState;reader.releaseLock();
        state.after={bodyUsed:response.bodyUsed,locked:response.body.locked};
    }catch(error){state.failure=describe(error);}
    server.send('done');server.close(1000,'done');
}

async function matrix(target,routes=['getReader','direct','prototype','subclass','reflect'],rows=[]){
    for(const [route,method,view]of routes){
        const row={route,method,view,failure:null,reads:[]};rows.push(row);let reader;
        try{
            row.phase='fetch';const response=await fetch(target),Base=ReadableStreamBYOBReader;class Derived extends Base{}
            row.phase='acquire';reader=route==='getReader'?response.body.getReader({mode:'byob'}):route==='direct'?new Base(response.body):route==='prototype'?new Base.prototype.constructor(response.body):route==='subclass'?new Derived(response.body):Reflect.construct(Base,[response.body],Derived);
            row.brand=reader instanceof ReadableStreamBYOBReader;const closed=reader.closed;let settled='pending';closed.then(()=>settled='fulfilled',()=>settled='rejected');row.closedStable=closed===reader.closed;
            for(let index=0;;++index){
                if(index>10)throw new Error('bounded finite Binding body diagnostic');
                row.phase='read/'+index;const input=new ArrayBuffer(64);new Uint8Array(input).fill(165);
                const into=view==='DataView'?new DataView(input,8,32):new globalThis[view](input,8,32/globalThis[view].BYTES_PER_ELEMENT),min=view==='Float64Array'?2:4;
                const promise=method==='least'?reader.readAtLeast(min,into):reader.read(into,{min});
                const inputAfter=input.byteLength,result=await promise,value=result.value;
                row.reads.push({inputAfter,done:result.done,type:value?.constructor.name??null,offset:value?.byteOffset??null,
                    length:value?.byteLength??null,capacity:value?.buffer.byteLength??null,
                    bytes:value?Array.from(new Uint8Array(value.buffer,value.byteOffset,value.byteLength)):null,
                    backing:value?Array.from(new Uint8Array(value.buffer)):null});
                if(result.done)break;
            }
            row.phase='closed';await closed;row.closed=settled;reader.releaseLock();row.lockedAfter=response.body.locked;delete row.phase;
        }catch(error){row.failure={name:error?.name??null,message:error?.message??null};if(reader){try{await reader.cancel();}catch{}try{reader.releaseLock();}catch{}}}
    }
    return rows;
}

async function executeHeadersRead(url, state, server) {
            try {
                const response = await fetch(url.searchParams.get('target'));
                const variants = ['plain', 'absent', 'missing', 'undefined', 'null', 'empty', 'space', 'symbol',
                    'object', 'throw', 'object-symbol', 'primitive-noncallable', 'wide', 'lookalike',
                    'fallback', 'fallback-throw', 'fallback-symbol', 'receiver', 'receiver-name-object', 'receiver-name-throw',
                    'receiver-prototype', 'receiver-proxy', 'receiver-null', 'receiver-undefined',
                    'receiver-missing', 'receiver-prototype-missing', 'receiver-proxy-missing',
                    'receiver-null-missing', 'receiver-undefined-missing', 'receiver-number-missing', 'receiver-number'];
                state.rows = [];
                for (const family of ['fetched', 'copy']) for (const method of ['get', 'has']) for (const variant of variants) {
                    const events = [], headers = family === 'fetched' ? response.headers : new Headers(response.headers);
                    const marker = variant === 'lookalike' ? new TypeError('Cannot convert a Symbol value to a string') : {tag: 'read-conversion'};
                    const convert = (value, throws = false) => ({[Symbol.toPrimitive](hint) {events.push('primitive/' + hint); if (throws) throw marker; return value;}});
                    let name = 'X-Probe', receiver = headers;
                    if (variant === 'absent') name = 'X-Missing';
                    if (variant === 'undefined') name = undefined;
                    if (variant === 'null') name = null;
                    if (variant === 'empty') name = '';
                    if (variant === 'space') name = 'bad name';
                    if (variant === 'symbol') name = Symbol('name');
                    if (variant === 'object') name = convert('X-Probe');
                    if (variant === 'throw' || variant === 'lookalike') name = convert('X-Probe', true);
                    if (variant === 'object-symbol') name = convert(Symbol('name'));
                    if (variant === 'primitive-noncallable') name = {[Symbol.toPrimitive]: 42};
                    if (variant === 'wide') name = '\u0100';
                    if (variant.startsWith('fallback')) name = {toString() {events.push('toString'); return {};}, valueOf() {
                        events.push('valueOf'); if (variant === 'fallback-throw') throw marker; return variant === 'fallback-symbol' ? Symbol('name') : 'X-Probe';}};
                    if (variant === 'receiver' || variant.startsWith('receiver-name-')) receiver = {};
                    if (variant === 'receiver-name-object') name = convert('X-Probe');
                    if (variant === 'receiver-name-throw') name = convert('X-Probe', true);
                    if (variant === 'receiver-prototype') receiver = Headers.prototype;
                    if (variant === 'receiver-proxy') receiver = new Proxy(headers, {});
                    if (variant === 'receiver-null') receiver = null;
                    if (variant === 'receiver-undefined') receiver = undefined;
                    if (variant.endsWith('-missing')) {
                        const base = variant.slice(0, -8);
                        receiver = base === 'receiver' ? {} : base === 'receiver-prototype' ? Headers.prototype :
                            base === 'receiver-proxy' ? new Proxy(headers, {}) : base === 'receiver-null' ? null :
                            base === 'receiver-undefined' ? undefined : 42;
                    }
                    if (variant === 'receiver-number') receiver = 42;
                    const row = {family, method, variant, events};
                    try {const value = Reflect.apply(Headers.prototype[method], receiver, variant === 'missing' || variant.endsWith('-missing') ? [] : [name]);
                        row.result = {value, type: typeof value};}
                    catch (error) {row.error = {...describe(error), sameMarker: error === marker};}
                    state.rows.push(row);
                }
                state.bytes = Array.from(new Uint8Array(await response.arrayBuffer()));
                state.after = {bodyUsed: response.bodyUsed, locked: response.body.locked};
            } catch (error) {state.failure = describe(error);}
    server.send('done'); server.close(1000, 'done');
}

export default {async fetch(request) {
    realm ??= crypto.randomUUID();
    const url = new URL(request.url);
    if (url.pathname.endsWith('/state')) return Response.json({realm, state: current ?? null});
    const state = current = {kind: url.searchParams.get('kind'), challenge: url.searchParams.get('challenge'), failure: null};
    const pair = new WebSocketPair(), server = pair[1]; server.accept();
    server.addEventListener('message', event => {
        if (event.data !== 'go') return;
        server.send('started');
        if (state.kind === 'ordinary-headers-read-conversions-order') void executeHeadersRead(url, state, server);
        else if (state.kind.startsWith('ordinary-gated-')) void executeGated(url, state, server);
        else if (state.kind.startsWith('ordinary-binding-body-matrix-')) void (async () => {
            try {
                state.rows = [];
                await matrix(url.searchParams.get('target'), [state.kind.split('-').slice(-3)], state.rows);
            } catch (error) {state.failure = describe(error);}
            server.send('done'); server.close(1000, 'done');
        })();
        else void executeCore(url, state, server);
    });
    return new Response(null, {status: 101, webSocket: pair[0]});
}};
