// Same-input connection timer/Promise lifecycle cases for pinned workerd and Capsid.
const native = {setTimeout, setInterval, clearInterval, clearTimeout, String};
const states = new Map(), resolvers = new Map(), timerIds = new Map();
const delay = 25;
const tick = state => {state.timerFired++; state.trace.push('timer');};
export default {async fetch(request) {
    const url = new URL(request.url), id = url.searchParams.get('id') ?? 'single';
    if (url.pathname.endsWith('/state')) return Response.json({state: states.get(id) ?? null});
    if(url.pathname.endsWith('/clear')) {
        const state=states.get(id),before=state.timerFired;
        try {native.clearTimeout(timerIds.get(id));state.trace.push('http-clear');}
        catch(error) {state.trace.push('http-clear/'+error.name+': '+error.message);}
        return Response.json({before});
    }
    if(url.pathname.endsWith('/resume')) {resolvers.get(id)?.();return Response.json({resumed:true});}

    const kind = url.searchParams.get('kind') ?? 'live-one-shot';
    const state = {kind, timerFired: 0, trace: [], events: []};
    states.set(id, state);
    if (kind === 'http-await') {
        await new Promise(resolve => native.setTimeout(() => {tick(state); resolve();}, delay));
        return Response.json({accepted: true, state});
    }
    if (kind === 'http-detached') {
        native.setTimeout(() => tick(state), delay);
        return Response.json({accepted: true});
    }
    const pair = new WebSocketPair(), server = pair[1];
    const observe = (type, extra = {}) => state.events.push({type, readyState: server.readyState, ...extra});
    server.accept({allowHalfOpen: kind === 'half-open-close-await'});
    server.addEventListener('error', event => {
        observe('error', {message: event.message, errorName: event.error?.name,
            errorMessage: event.error?.message, trusted: event.isTrusted});
        if (kind === 'error-listener-timer') native.setTimeout(() => tick(state), delay);
    });
    server.addEventListener('close', () => {
        observe('close');
        if (kind === 'half-open-close-await') return new Promise(resolve => native.setTimeout(() => {
            tick(state); server.send('after-peer-close'); server.close(1000, 'done'); resolve();
        }, delay));
    });
    server.addEventListener('message', () => {
        observe('message');
        if(kind==='same-clear' && timerIds.has(id)) {
            native.clearTimeout(timerIds.get(id));state.trace.push('same-clear');server.send('cleared');server.close(1000,'done');return;
        }

        server.send('queued');
        if (kind === 'half-open-close-await') return;

        if(kind==='live-returned-then'||kind==='live-detached-then') {
            const p=new Promise(resolve=>native.setTimeout(resolve,25)).then(()=>{tick(state);server.send('timer');server.close(1000,'done');});
            state.trace.push('handler-return');queueMicrotask(()=>state.trace.push('microtask'));
            return kind==='live-returned-then'?p:undefined;
        }
        if(kind==='close-returned-await'||kind==='close-detached-await') {
            const p=new Promise(resolve=>native.setTimeout(resolve,25)).then(()=>{tick(state);});
            server.close(1000,'done');return kind==='close-returned-await'?p:undefined;
        }
        if(kind==='close-external-returned'||kind==='close-external-detached') {
            const p=new Promise(resolve=>resolvers.set(id,resolve)).then(()=>{
                state.trace.push('resumed');
                try {server.send('resumed');state.trace.push('send-accepted');}
                catch(error){state.trace.push('send/'+error.name+': '+error.message);}
            });
            server.close(1000,'done');return kind==='close-external-returned'?p:undefined;
        }
        if(kind==='cross-clear'||kind==='same-clear') {
            timerIds.set(id,native.setTimeout(()=>{tick(state);server.send('timer');server.close(1000,'done');},100));return;
        }
        if (kind === 'live-one-shot') {
            native.setTimeout(() => {tick(state); server.send('timer'); server.close(1000, 'done');}, delay);
            return;
        }
        if (kind === 'live-interval') {
            const timer = native.setInterval(() => {
                tick(state); server.send('timer-' + state.timerFired);
                if (state.timerFired === 2) {native.clearInterval(timer); server.close(1000, 'done');}
            }, delay);
            return;
        }
        if (kind === 'close-with-ack' || kind === 'close-delayed-ack') {
            native.setTimeout(() => tick(state), delay);
            server.close(1000, 'done');
            return;
        }
        if (kind === 'error-timer-before' || kind === 'error-final-close-timer')
            native.setTimeout(() => tick(state), delay);
        if (kind === 'error-final-close-timer') server.close(1000, 'done');
        queueMicrotask(() => {
            state.trace.push('microtask');
            queueMicrotask(() => {
                state.trace.push('nested-microtask');
                try {server.send('late'); state.trace.push('late-accepted');}
                catch (error) {state.trace.push('late-' + error.name);}
            });
        });
        throw new TypeError('owner-lifecycle/' + kind);
    });
    return new Response(null, {status: 101, webSocket: pair[0]});
}};
