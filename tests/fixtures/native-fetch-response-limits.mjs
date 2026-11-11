const describe=error=>({name:error?.name??null,message:error?.message??null});
const metadata=response=>({status:response.status,bodyUsed:response.bodyUsed,bodyNull:response.body===null,
    locked:response.body?.locked??null,headers:[...response.headers]});
export default {async fetch(request){
    const url=new URL(request.url);if(url.pathname==='/healthy')return new Response('healthy');
    const view=Number(url.searchParams.get('view'));const state={failure:null};let response,reader,watched;
    try{
        response=await fetch(url.searchParams.get('target'));state.before=metadata(response);
        reader=response.body.getReader(view?{mode:'byob'}:undefined);const closed=reader.closed;state.closedStable=closed===reader.closed;
        watched=closed.then(()=>{state.closed={fulfilled:true};},error=>{state.closed={error:describe(error)};});
        state.reads=[];
        for(let index=0;;++index){
            if(index>1000)throw new Error('finite limit probe exceeded bound');
            const input=view?new ArrayBuffer(view+16):null;if(input)new Uint8Array(input).fill(165);
            const pending=view?reader.read(new Uint8Array(input,8,view)):reader.read();const inputAfter=input?.byteLength??null;
            const {done,value}=await pending;state.reads.push({inputAfter,done,type:value?.constructor.name??null,
                offset:value?.byteOffset??null,length:value?.byteLength??null,capacity:value?.buffer.byteLength??null,
                bytes:value?Array.from(value):null,backing:value?Array.from(new Uint8Array(value.buffer)):null});
            if(done)break;
        }
        await watched;
    }catch(error){state.failure=describe(error);if(watched)await watched;}
    finally{if(reader)try{reader.releaseLock();}catch(error){state.releaseError=describe(error);}if(response)state.after=metadata(response);}
    return Response.json(state);
}};
