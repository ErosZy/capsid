import assert from 'node:assert/strict';
const routes=['getReader','direct','prototype','subclass','reflect'];
export const readerScopeCases=Object.freeze(['native','standard'].flatMap(source=>routes.flatMap(route=>['default','byob'].flatMap(reader=>
    ['unread','primed'].flatMap(phase=>['read','cancel','release','closed'].map(action=>[source,route,reader,'foreign_'+action+'_'+phase].join('|')))))));
const message="Cannot perform I/O on behalf of a different request. I/O objects (such as streams, request/response bodies, and others) created in the context of one request handler cannot be accessed from a different request's handler. This is a limitation of Cloudflare Workers which allows us to improve overall performance. (I/O type: Canceler)";
const fulfilled=()=>({settled:true,result:{returnedUndefined:true}}),pending=()=>({settled:false,result:null});
const rejected=(name,message,tag=null)=>({settled:true,result:{error:{name,message,isError:name!==null,tag}}});
const payload=[0,102,105,114,115,116];
export function expectedReaderScopeState(kind,challenge){
    assert.ok(readerScopeCases.includes(kind));const [source,,reader,scenario]=kind.split('|'),[,action,phase]=scenario.split('_');
    const native=source==='native',byob=reader==='byob',primed=phase==='primed';
    const input=()=>({before:byob?32:null,after:byob?0:null});
    const read=(bytes,{done=false,undefinedValue=false}={})=>({settled:true,result:{value:{done,type:undefinedValue?null:'Uint8Array',
        offset:undefinedValue?null:byob?8:0,length:undefinedValue?null:bytes.length,capacity:undefinedValue?null:byob?32:4096,
        bytes:undefinedValue?null:bytes}}});
    const state={kind,challenge,unexpected:null,closedInitialStable:true,
        closed:!native&&action==='release'?rejected('TypeError','This ReadableStream reader has been released.'):fulfilled()};
    if(primed){state.primeInput=input();state.prime=read(byob?payload.slice(0,1):payload);}
    state.lockedBeforeActor=true;
    state.actorSync=native&&action==='release'?{name:'Error',message,isError:true,tag:null}:null;
    if(action==='read'){
        state.actorReadInput=input();state.actorRead=native?rejected('Error',message):primed&&!byob?pending():read(primed?payload.slice(1):payload);
    }else if(action==='cancel')state.actorCancel=native?rejected('Error',message):fulfilled();
    else if(action==='release'&&!native)state.actorRelease=true;
    else if(action==='closed'){state.closedActorIdentity=true;state.closedActorStable=true;state.actorClosed=pending();}
    state.actorLocked=native||action!=='release';state.actorBodyUsed=native?(primed||['read','cancel'].includes(action)):null;
    state.beforeFinish={closed:!native&&['cancel','release'].includes(action),read:action==='read'?(native||!primed||byob):null,
        cancel:action==='cancel'?true:null,actorClosed:action==='closed'?false:null};
    state.ownerReadInput=input();
    const available=primed?(byob?payload.slice(1):[]):payload;
    if(native&&action==='read')state.ownerRead=rejected('TypeError','This ReadableStream only supports a single pending read request at a time.');
    else if(!native&&action==='cancel')state.ownerRead=read([],{done:true,undefinedValue:!byob});
    else if((native||action!=='read')&&available.length)state.ownerRead=read(available);
    else state.ownerRead=native?rejected(null,null,'owner-stop'):read([],{done:true});
    state.ownerWaitedForData=native?(!primed||byob||action==='read'):action!=='read'&&(!primed||byob||action==='cancel');
    if(state.ownerWaitedForData)state.ownerAwaitComplete=true;
    state.ownerReadBeforeCancel=native?(action==='read'||available.length>0):action==='cancel'||action!=='read'&&available.length>0;
    state.ownerCancel=fulfilled();state.watchersSettled=action!=='closed'&&(native||action!=='read'||!primed||byob);
    state.actorReadErrorIsClosed=false;state.bodyUsed=native?true:null;state.locked=false;return state;
}

export function expectedReaderScopeActorState(kind,challenge){
    const final=expectedReaderScopeState(kind,challenge);
    const state={};
    for(const key of ['kind','challenge','unexpected','closedInitialStable','closed','primeInput','prime',
        'lockedBeforeActor','actorSync','actorReadInput','actorRead','actorCancel','actorRelease',
        'closedActorIdentity','closedActorStable','actorClosed','actorLocked','actorBodyUsed','beforeFinish'])
        if(Object.hasOwn(final,key))state[key]=final[key];
    if(!final.beforeFinish.closed)state.closed=pending();
    return state;
}
