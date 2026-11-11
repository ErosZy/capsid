let realm, current;
const describe = error => ({name: error?.name ?? null, message: error?.message ?? null});
const digest = async bytes => Array.from(new Uint8Array(await crypto.subtle.digest('SHA-256', bytes)), byte => byte.toString(16).padStart(2, '0')).join('');
function socketState(socket) {
    return Object.fromEntries(['readyState', 'protocol', 'extensions', 'url', 'binaryType', 'bufferedAmount'].map(name => {
        const value = socket[name]; return [name, value === undefined ? {type: 'undefined'} : value];
    }));
}
export default {async fetch(request) {
    realm ??= crypto.randomUUID(); const url = new URL(request.url);
    if (url.pathname.endsWith('/state')) return Response.json({realm, state: current ?? null});
    const kind = url.searchParams.get('kind'), challenge = url.searchParams.get('challenge');
    const state = current = {kind, challenge, trace: [], result: null, socket: null, failure: null, firstBody: null};
    const pair = new WebSocketPair(), server = pair[1]; server.accept(); let abort;
    server.addEventListener('close', () => state.trace.push('close'));
    server.addEventListener('message', async event => {
        if (event.data === 'abort') {state.trace.push('abort'); abort(); server.send('ack/abort'); return;}
        if (event.data !== 'go') return;
        state.trace.push('message', 'fetch'); server.send('started');
        const controller = new AbortController(), reason = {name: 'Custom', message: challenge}; abort = () => controller.abort(reason);
        try {
            const headers = {Upgrade: 'websocket'};
            if (kind === 'upgrade-custom-headers') Object.assign(headers, {'X-Probe': challenge, Origin: 'https://caller.example',
                'User-Agent': 'caller/' + challenge, 'Sec-WebSocket-Protocol': 'chat, super'});
            const init = {headers};
            if (kind === 'upgrade-post') {init.method = 'POST'; init.body = 'body/' + challenge;}
            if (kind.startsWith('upgrade-abort-')) {init.signal = controller.signal; if (kind === 'upgrade-abort-pre') abort();}
            if (kind === 'upgrade-redirect-manual') init.redirect = 'manual';
            const response = await fetch(url.searchParams.get('target'), init), socket = response.webSocket;
            state.result = {status: response.status, statusText: response.statusText, url: response.url, type: response.type,
                redirected: response.redirected, bodyNull: response.body === null, bodyUsed: response.bodyUsed,
                headers: [...response.headers], cookies: response.headers.getSetCookie(), hasWebSocket: socket != null,
                constructor: socket?.constructor?.name ?? null, body: null};
            if (socket) {
                const item = state.socket = {before: socketState(socket), beforeSend: null, after: null, afterClose: null,
                    messages: [], sendMutation: null, closed: null, events: []};
                try {socket.send('early'); item.beforeSend = {returned: 'undefined'};} catch (error) {item.beforeSend = {error: describe(error)};}
                socket.binaryType = 'arraybuffer';
                let resolveClosed;
                const closed = new Promise(resolve => {resolveClosed = resolve;});
                socket.addEventListener('error', event => item.events.push({type: 'error', state: socket.readyState,
                    message: event.message, error: String(event.error), errorType: typeof event.error,
                    errorIsError: event.error instanceof Error, errorName: event.error?.name, errorMessage: event.error?.message}));
                socket.addEventListener('close', event => {
                    item.closed = {code: event.code, reason: event.reason, wasClean: event.wasClean};
                    item.events.push({type: 'close', state: socket.readyState, ...item.closed}); resolveClosed();
                });
                socket.addEventListener('message', async event => {
                    if (typeof event.data === 'string') item.messages.push({type: 'text', value: event.data});
                    else {const bytes = new Uint8Array(event.data); item.messages.push({type: 'binary', bytes: bytes.length, sha256: await digest(bytes)});}
                    if (item.messages.length === 2) {socket.close(1000, 'probe'); item.afterClose = socketState(socket);}
                });
                socket.accept(); item.after = socketState(socket); socket.send('echo/' + challenge);
                const bytes = kind === 'upgrade-large-message' ? Uint8Array.from({length: 180000}, (_, index) => index % 251) : new Uint8Array([1, 2, 3, 4, 5]);
                socket.send(kind === 'upgrade-large-message' ? bytes : bytes.subarray(1, 4));
                try {bytes.fill(255); item.sendMutation = {succeeded: true};} catch (error) {item.sendMutation = {succeeded: false, ...describe(error)};}
                await closed;
            } else {
                let bytes;
                if (kind === 'upgrade-non101-stream') {
                    const reader = response.body.getReader(), chunks = []; let total = 0;
                    for (;;) {
                        const part = await reader.read(); if (part.done) break;
                        chunks.push(part.value); total += part.value.length;
                        if (total === 8192) {
                            const first = new Uint8Array(total); let at = 0;
                            for (const chunk of chunks) {first.set(chunk, at); at += chunk.length;}
                            state.firstBody = {bytes: total, sha256: await digest(first)}; state.trace.push('body-first'); server.send('body-first');
                        }
                    }
                    bytes = new Uint8Array(total); let offset = 0; for (const chunk of chunks) {bytes.set(chunk, offset); offset += chunk.length;}
                } else bytes = new Uint8Array(await response.arrayBuffer());
                state.result.body = {bytes: bytes.length, sha256: await digest(bytes)}; state.result.bodyUsed = response.bodyUsed;
            }
            state.trace.push('complete');
        } catch (error) {state.failure = {...describe(error), sameReason: error === reason}; state.trace.push('failure');}
        server.send('done'); server.close(1000, 'done');
    });
    return new Response(null, {status: 101, webSocket: pair[0]});
}};
