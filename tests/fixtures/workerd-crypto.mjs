let realm,current;
export default {async fetch(request) {
    realm ??= crypto.randomUUID(); const url=new URL(request.url);
    if(url.pathname.endsWith('/state'))return Response.json({realm,state:current??null});
    const kind=url.searchParams.get('kind'),challenge=url.searchParams.get('challenge');
    const state=current={kind,challenge,trace:[],reads:[],returned:null,result:null,failure:null,mutation:null};
    if(kind.endsWith('-prototype'))state.observations=[];
    const pair=new WebSocketPair(),server=pair[1];server.accept();
    server.addEventListener('close',()=>state.trace.push('close'));
    server.addEventListener('message',async()=>{
        state.trace.push('message');
        const data=kind.endsWith('-empty')?new Uint8Array():new Uint8Array([1,2,3,4]),keyBytes=new Uint8Array(32),sentinel={tag:challenge};
        const hash=kind.endsWith('-sha1')?'SHA-1':kind.endsWith('-sha384')?'SHA-384':kind.endsWith('-sha512')?'SHA-512':'SHA-256';
        const key=/export|sign|verify/.test(kind)||kind==='crypto-key-map-prototype'
            ?await crypto.subtle.importKey('raw',keyBytes,{name:'HMAC',hash},kind!=='crypto-key-map-prototype',['sign','verify']):null;
        const signature=kind.includes('verify')?new Uint8Array(await crypto.subtle.sign('HMAC',key,data)):null;
        const describe=error=>({type:typeof error,name:error?.name??null,message:error?.message??null,sameSentinel:error===sentinel});
        const algorithm=(name,throws=false)=>Object.defineProperty({},'name',{get(){state.reads.push('algorithm.name');if(throws)throw sentinel;return name;}});
        const finish=()=>{if(!kind.endsWith('-close')){server.send('done');server.close(1000,'done');}};
        const microtasks=()=>queueMicrotask(()=>{state.trace.push('microtask/1');queueMicrotask(()=>{state.trace.push('microtask/2');queueMicrotask(()=>state.trace.push('microtask/3'));});});
        let operation,mutation;
        state.trace.push('before');
        try {
            if(kind==='crypto-key-map-prototype') {
                const original=WeakMap.prototype.get;
                try {
                    WeakMap.prototype.get=function(value) {
                        const result=original.call(this,value);
                        if(value===key)state.observations.push({tag:Object.prototype.toString.call(result),bytes:result instanceof Uint8Array?Array.from(result):null});
                        return result;
                    };
                    operation=crypto.subtle.sign('HMAC',key,data);
                } finally {WeakMap.prototype.get=original;}
            } else if(kind==='crypto-subtle-set-prototype') {
                const original=WeakSet.prototype.has;
                try {WeakSet.prototype.has=()=>true;operation=crypto.subtle.digest.call({},'SHA-256',data);}
                finally {WeakSet.prototype.has=original;}
            } else if(kind==='crypto-digest-data-copy') {operation=crypto.subtle.digest('SHA-256',data);mutation=()=>data.fill(9);}
            else if(kind==='crypto-digest-view-copy') {
                const buffer=new Uint8Array([8,1,2,3,4,8]);operation=crypto.subtle.digest('SHA-256',buffer.subarray(1,5));mutation=()=>buffer.fill(9);
            } else if(kind==='crypto-sign-data-copy') {operation=crypto.subtle.sign('HMAC',key,data);mutation=()=>data.fill(9);}
            else if(kind==='crypto-verify-data-copy') {operation=crypto.subtle.verify('HMAC',key,signature,data);mutation=()=>data.fill(9);}
            else if(kind==='crypto-verify-signature-copy') {operation=crypto.subtle.verify('HMAC',key,signature,data);mutation=()=>signature.fill(9);}
            else if(kind==='crypto-import-data-copy') {operation=crypto.subtle.importKey('raw',keyBytes,{name:'HMAC',hash:'SHA-256'},true,['sign']);mutation=()=>keyBytes.fill(9);}
            else if(kind==='crypto-digest-algorithm-getter')operation=crypto.subtle.digest(algorithm('SHA-256'),data);
            else if(kind==='crypto-digest-throwing-getter')operation=crypto.subtle.digest(algorithm('SHA-256',true),data);
            else if(kind==='crypto-digest-invalid-data')operation=crypto.subtle.digest(algorithm('SHA-256'),'invalid');
            else if(kind==='crypto-digest-invalid-name')operation=crypto.subtle.digest({name:'missing'},data);
            else if(kind==='crypto-digest-invalid-receiver')operation=crypto.subtle.digest.call({},'SHA-256',data);
            else if(kind==='crypto-digest-forged-receiver')operation=crypto.subtle.digest.call(Object.create(SubtleCrypto.prototype),'SHA-256',data);
            else if(kind==='crypto-sign-invalid-key')operation=crypto.subtle.sign(algorithm('HMAC'),{},data);
            else if(kind==='crypto-import-invalid-format')operation=crypto.subtle.importKey('missing',keyBytes,algorithm('HMAC'),false,['sign']);
            else if(kind==='crypto-import-empty-usages')operation=crypto.subtle.importKey('raw',keyBytes,{name:'HMAC',hash:'SHA-256'},false,[]);
            else if(kind==='crypto-generate-zero-length')operation=crypto.subtle.generateKey({name:'HMAC',hash:'SHA-256',length:0},false,['sign']);
            else if(kind==='crypto-generate-empty-usages')operation=crypto.subtle.generateKey({name:'HMAC',hash:'SHA-256'},false,[]);
            else if(kind==='crypto-verify-wrong-signature') {
                signature[0]^=1;operation=crypto.subtle.verify('HMAC',key,signature,data);
            } else if(kind==='crypto-verify-short-signature')operation=crypto.subtle.verify('HMAC',key,signature.subarray(1),data);
            else if(kind==='crypto-export-invalid-key')operation=crypto.subtle.exportKey('raw',{});
            else operation=kind.includes('digest')?crypto.subtle.digest(hash,data)
                :kind.includes('generate')?crypto.subtle.generateKey({name:'HMAC',hash:'SHA-256'},true,['sign'])
                :kind.includes('export')?crypto.subtle.exportKey('raw',key)
                :kind.includes('sign')?crypto.subtle.sign('HMAC',key,data)
                :kind.includes('verify')?crypto.subtle.verify('HMAC',key,signature,data)
                :crypto.subtle.importKey('raw',keyBytes,{name:'HMAC',hash:'SHA-256'},false,['sign']);
            state.returned=operation instanceof Promise?'promise':typeof operation;
            if(mutation){
                try{mutation();state.mutation={succeeded:true,error:null};state.trace.push('mutated');}
                catch(error){state.mutation={succeeded:false,error:describe(error)};state.trace.push('mutation/failed');}
            }
            operation.then(async value=>{
                state.trace.push('complete');
                state.result=value instanceof ArrayBuffer?Array.from(new Uint8Array(value)):typeof value==='boolean'?value
                    :{type:value.type,algorithm:value.algorithm,usages:value.usages};
                if(kind==='crypto-import-data-copy')state.result.raw=Array.from(new Uint8Array(await crypto.subtle.exportKey('raw',value)));
                finish();
            },error=>{state.trace.push('failure');state.failure=describe(error);finish();});
        }catch(error){state.returned='throw';state.trace.push('throw');state.failure=describe(error);}
        microtasks();state.trace.push('after');server.send('started');
        if(kind.endsWith('-close'))server.close(1000,'done');
        else if(state.returned==='throw')finish();
    });
    return new Response(null,{status:101,webSocket:pair[0]});
}};
