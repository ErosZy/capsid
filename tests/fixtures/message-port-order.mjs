
const NativePromise=Promise, nativeResolve=Promise.resolve.bind(Promise), nativeThen=Promise.prototype.then;
const nativeTimer=setTimeout, nativeMicrotask=queueMicrotask;
const wait=ms=>new NativePromise(resolve=>nativeTimer(resolve,ms));
export default {async fetch() {
    const result={};
    {
        const pair=new MessageChannel(), trace=[];
        pair.port1.onmessage=()=>{trace.push('port');nativeResolve().then(()=>trace.push('port/promise'));};
        nativeResolve().then(()=>trace.push('promise/before'));
        pair.port2.postMessage('value');
        nativeResolve().then(()=>trace.push('promise/after'));
        await wait(30);pair.port1.close();pair.port2.close();result.promiseOrder=trace;
    }
    {
        const first=new MessageChannel(), second=new MessageChannel(), trace=[];
        second.port1.onmessage=()=>trace.push('second');
        first.port1.onmessage=()=>{trace.push('first');second.port2.postMessage('two');nativeResolve().then(()=>trace.push('first/promise'));};
        first.port2.postMessage('one');nativeResolve().then(()=>trace.push('after/post'));
        await wait(30);for(const pair of [first,second]){pair.port1.close();pair.port2.close();}result.nestedOrder=trace;
    }
    {
        const pair=new MessageChannel(), trace=[];
        pair.port1.onmessage=()=>trace.push('delivered');
        const resolve=Promise.resolve, then=Promise.prototype.then;
        let error=null;
        globalThis.setTimeout=()=>{throw new Error('overridden timer called');};
        globalThis.queueMicrotask=()=>{throw new Error('overridden queueMicrotask called');};
        Promise.resolve=()=>{throw new Error('overridden resolve called');};
        Promise.prototype.then=()=>{throw new Error('overridden then called');};
        try {pair.port2.postMessage('value');}catch(value){error={name:value.name,message:value.message};}
        finally {globalThis.setTimeout=nativeTimer;globalThis.queueMicrotask=nativeMicrotask;Promise.resolve=resolve;Promise.prototype.then=then;}
        await wait(30);pair.port1.close();pair.port2.close();result.intrinsic={error,trace};
    }
    {
        const first=new MessageChannel(), second=new MessageChannel(), events=[], trace=[];
        const token={}, rejected=NativePromise.reject(token);
        const unhandled=event=>{if(event.promise===rejected){events.push({type:event.type,reasonIdentity:event.reason===token});event.preventDefault();}};
        const handled=event=>{if(event.promise===rejected)events.push({type:event.type,reasonType:typeof event.reason});};
        addEventListener('unhandledrejection',unhandled);addEventListener('rejectionhandled',handled);
        first.port1.onmessage=()=>{trace.push('first');second.port2.postMessage('two');};
        second.port1.onmessage=()=>{trace.push('second');rejected.catch(value=>trace.push(value===token?'catch/same':'catch/different'));};
        first.port2.postMessage('one');
        await wait(60);removeEventListener('unhandledrejection',unhandled);removeEventListener('rejectionhandled',handled);
        for(const pair of [first,second]){pair.port1.close();pair.port2.close();}result.nestedRejection={events,trace};
    }
    return Response.json(result);
}};
