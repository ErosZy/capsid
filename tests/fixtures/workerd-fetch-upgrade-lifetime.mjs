let realm, current;
const describe = (error, reason) => ({name: error?.name ?? null, message: error?.message ?? null, sameReason: error === reason});
export default {async fetch(request) {
    realm ??= crypto.randomUUID(); const url = new URL(request.url);
    if (url.pathname.endsWith('/state')) return Response.json({realm, state: current ?? null});
    const kind = url.searchParams.get('kind'), challenge = url.searchParams.get('challenge'), role = url.searchParams.get('role');
    if (kind==='fetched-disconnect-close') return fetchedDisconnectLifetime(url,kind,challenge);
    if (kind.startsWith('await-')) return awaitLifetime(url,kind,challenge);
    if (role === 'a') current = {kind, challenge, owners: {a: null, b: null}};
    const state = current.owners[role] = {trace: [], headers: null, first: null, second: null, failure: null, socket: null};
    const pair = new WebSocketPair(), server = pair[1]; server.accept();
    const controller = new AbortController(), reason = {name: 'Custom', message: challenge}; let reader;
    function startPendingDescendant() {
        const pending = current.owners.b = {trace: ['go','fetch'], headers: null, first: null, second: null, failure: null, socket: null};
        if (kind.endsWith('-complete')) {pending.timerTicks = 0; setInterval(() => pending.timerTicks++, 10);}
        fetch(url.searchParams.get('secondTarget'), {headers: {Upgrade: 'websocket'}}).then(async response => {
            pending.trace.push('headers'); pending.headers = {status: response.status, url: response.url, bodyNull: response.body === null, bodyUsed: response.bodyUsed, webSocket: response.webSocket != null};
            if (kind.endsWith('-complete')) {pending.first = {value: await response.text(), done: true, bodyUsed: response.bodyUsed}; pending.trace.push('complete');}
        }).catch(error => {pending.trace.push('failure'); pending.failure = describe(error, undefined);});
    }
    server.addEventListener('close', () => state.trace.push('close'));
    server.addEventListener('message', event => {
        if (event.data === 'finish') {state.trace.push('finish'); server.send('ack/finish'); server.close(1000, 'done'); return;}
        if (event.data === 'checkpoint') {state.trace.push('checkpoint'); server.send('ack/checkpoint'); return;}
        if (event.data === 'cancel') {
            state.trace.push('cancel/before');
            reader.cancel(challenge).then(() => {state.trace.push('cancel/after'); server.send('cancelled');}); return;
        }
        if (event.data === 'abort') {state.trace.push('abort'); controller.abort(reason); server.send('ack/abort'); return;}
        if (event.data !== 'go') return;
        state.trace.push('go', 'fetch'); server.send('started');
        if (kind === 'same-origin-parent-pending' || kind === 'same-origin-parent-complete') startPendingDescendant();
        fetch(url.searchParams.get('target'), {headers: {Upgrade: 'websocket'}, signal: controller.signal}).then(async response => {
            state.trace.push('headers'); const socket = response.webSocket;
            state.headers = {status: response.status, url: response.url, bodyNull: response.body === null, bodyUsed: response.bodyUsed, webSocket: socket != null};
            if (socket) {
                const value = state.socket = {before: socket.readyState, accepted: false, messages: [], events: [], closed: null};
                socket.addEventListener('error', event => value.events.push({type: 'error', state: socket.readyState, message: event.message, error: String(event.error),
                    errorType: typeof event.error, errorIsError: event.error instanceof Error, errorName: event.error?.name, errorMessage: event.error?.message}));
                socket.addEventListener('close', event => {value.closed = {code: event.code, reason: event.reason, wasClean: event.wasClean}; value.events.push({type: 'close', state: socket.readyState, ...value.closed});});
                socket.addEventListener('message', event => {
                    value.messages.push(event.data);
                    if (event.data === 'ready/' + challenge + '/' + role) {
                        if (kind === 'same-origin-child-pending' || kind === 'same-origin-child-complete') startPendingDescendant();
                        server.send('socket-ready');
                    }
                    if (event.data === 'after/' + challenge + '/' + role) socket.send('echo/' + challenge + '/' + role);
                    if (event.data === 'echo/' + challenge + '/' + role) socket.close(1000, 'child');
                });
                if (kind === 'close-unaccepted') {server.send('headers'); return;}
                socket.accept(); value.accepted = true; socket.send('ready/' + challenge + '/' + role); return;
            }
            if (kind === 'close-unread') {server.send('headers'); return;}
            reader = response.body.getReader(); let first = '';
            const expected = 'first/' + challenge + '/' + role;
            while (first.length < expected.length) {
                const part = await reader.read(); if (part.done) throw new Error('Body ended before the complete first marker');
                first += new TextDecoder().decode(part.value);
            }
            if (first !== expected) throw new Error('Body marker differs from the actual upstream payload');
            state.first = {value: first, done: false, bodyUsed: response.bodyUsed}; state.trace.push('first'); server.send('reading');
            if (kind === 'cancel-reader') return;
            const second = await reader.read(); state.second = {done: second.done, value: second.value ? new TextDecoder().decode(second.value) : null}; state.trace.push('second');
        }).catch(error => {state.failure = describe(error, controller.signal.reason); state.trace.push('failure'); server.send('failed');});
    });
    return new Response(null, {status: 101, webSocket: pair[0]});
}};

async function awaitLifetime(url,kind,challenge) {
    const deferred=kind.startsWith('await-deferred-');
    const state=current={kind,challenge,trace:[],timerTicks:0,parentClose:null,response:null,failure:null,socket:null};
    const pair=new WebSocketPair(),server=pair[1];server.accept();
    server.addEventListener('close',event=>{state.trace.push('close');state.parentClose={code:event.code,reason:event.reason,wasClean:event.wasClean,before:server.readyState};try{if(!deferred){server.close(1000,'done');state.parentClose.after=server.readyState;}}catch(error){state.parentClose.closeFailure={name:error.name,message:error.message};}});
    server.addEventListener('message',async event=>{
        if(event.data==='finish'){state.trace.push('finish');server.send('ack/finish');server.close(1000,'done');return;}
        if(event.data!=='go')return;
        state.trace.push('go');setInterval(()=>state.timerTicks++,10);
        try {
            if(!kind.endsWith('-last')) {
                const response=await fetch(url.searchParams.get('target'),{headers:{Upgrade:'websocket'}});
                const socket=response.webSocket;state.trace.push('socket');
                const value=state.socket={status:response.status,accepted:false,messages:[],events:[],closed:null};
                let ready;const waiting=new Promise(resolve=>{ready=resolve;});
                socket.addEventListener('error',event=>value.events.push({type:'error',state:socket.readyState,message:event.message,error:String(event.error),errorType:typeof event.error,errorIsError:event.error instanceof Error,errorName:event.error?.name,errorMessage:event.error?.message}));
                socket.addEventListener('close',event=>{value.closed={code:event.code,reason:event.reason,wasClean:event.wasClean};value.events.push({type:'close',...value.closed});});
                socket.addEventListener('message',event=>{
                    value.messages.push(event.data);
                    if(event.data==='ready/'+challenge+'/a'){state.trace.push('ready');server.send('socket-ready');ready();}
                    if(event.data==='after/'+challenge+'/a')socket.send('echo/'+challenge+'/a');
                    if(event.data==='echo/'+challenge+'/a')socket.close(1000,'child');
                });
                socket.accept();value.accepted=true;socket.send('ready/'+challenge+'/a');await waiting;
            }
            state.trace.push('await');const pending=fetch(url.searchParams.get('secondTarget'),{headers:{Upgrade:'websocket'}});server.send('waiting');
            const response=await pending;state.trace.push('headers');
            const text=await response.text();state.response={status:response.status,text,bodyUsed:response.bodyUsed};state.trace.push('resumed');if(deferred){state.parentClose.localBefore=server.readyState;server.close(1000,'done');state.parentClose.localAfter=server.readyState;state.trace.push('local-close');}
        }catch(error){state.failure={name:error?.name??null,message:error?.message??null};state.trace.push('failure');}
    });
    return new Response(null,{status:101,webSocket:pair[0]});
}

async function fetchedDisconnectLifetime(url,kind,challenge) {
    const state=current={kind,challenge,status:null,beforeAccept:null,trace:[],close:null,states:null,errors:[],failure:null};
    const server=new WebSocketPair();server[1].accept();
    server[1].addEventListener('message',async()=>{try{
        const response=await fetch(url.searchParams.get('target'),{headers:{Upgrade:'websocket'}});const socket=response.webSocket;state.status=response.status;state.beforeAccept=socket.readyState;state.trace.push('headers');
        socket.addEventListener('error',event=>state.errors.push({message:event.message,error:String(event.error),state:socket.readyState}));
        socket.addEventListener('message',event=>{state.trace.push(event.data);if(event.data.startsWith('ready/'))server[1].send('socket-ready');});
        socket.addEventListener('close',event=>{state.close={code:event.code,reason:event.reason,wasClean:event.wasClean};
            const states=state.states={before:socket.readyState};try{socket.close(1000,'done');states.after=socket.readyState;}catch(error){states.failure={name:error.name,message:error.message};}
            queueMicrotask(()=>{states.microtask=socket.readyState;setTimeout(()=>{states.timer=socket.readyState;server[1].send('closed');server[1].close(1000,'done');},10);});
        });socket.accept();socket.send('ready/'+url.searchParams.get('challenge')+'/a');
    }catch(error){state.failure={name:error.name,message:error.message};}});
    return new Response(null,{status:101,webSocket:server[0]});
}
