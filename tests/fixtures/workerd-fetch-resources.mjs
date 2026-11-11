let realm, current, sockets;

function describe(value, reason) {
    return {type: typeof value, name: value?.name ?? null, message: value?.message ?? null,
        constructor: value?.constructor?.name ?? null, sameReason: value === reason,
        primitive: value === null || ['string', 'number', 'boolean', 'undefined'].includes(typeof value) ? String(value) : null};
}

export default {async fetch(request) {
    realm ??= crypto.randomUUID();
    const url = new URL(request.url);
    if (url.pathname.endsWith('/state')) return Response.json({realm, state: current ?? null});
    const kind = url.searchParams.get('kind'), challenge = url.searchParams.get('challenge');
    const role = url.searchParams.get('role'), target = url.searchParams.get('target'), secondTarget = url.searchParams.get('secondTarget');
    if (role === 'a') {
        current = {kind, challenge, trace: [], result: null, reason: null, failure: null, clones: null, other: null};
        sockets = {};
    }
    const state = role === 'b'
        ? current.other = {trace: [], result: null}
        : current;
    const pair = new WebSocketPair(), server = pair[1]; server.accept();
    sockets[role] = server;
    const syncContext = kind.startsWith('fetch-sync-context');
    function writeContexts(stage) {
        for (const name of ['a', 'b']) {
            try {sockets[name].send(stage + '/' + name); state.trace.push(stage + '/' + name + '/ok');}
            catch (error) {state.trace.push(stage + '/' + name + '/' + error.name + ': ' + error.message);}
        }
    }
    let controller;
    function abort() {
        const choice = kind.split('-').at(-1);
        let reason;
        if (choice === 'mutation' || choice === 'shared') {
            const nested = {value: 'original'};
            reason = {name: 'Custom', message: challenge, left: nested, right: nested};
        } else if (choice !== 'default') {
            reason = choice === 'error' ? new Error(challenge) : choice === 'dom' ? new DOMException(challenge, 'AbortError')
                : choice === 'null' ? null : choice === 'number' ? 23 : choice === 'object' ? {name: 'Custom', message: challenge} : challenge;
        }
        controller.abort(reason);
        if (choice === 'mutation') {reason.message = 'changed'; reason.left.value = 'changed';}
        state.reason = describe(controller.signal.reason, controller.signal.reason);
    }
    server.addEventListener('close', () => state.trace.push('close'));
    server.addEventListener('message', event => {
        if (event.data === 'finish') {
            state.trace.push('finish'); server.send('ack/finish'); server.close(1000, 'done'); return;
        }
        if (event.data === 'checkpoint') {
            state.trace.push('checkpoint'); server.send('ack/checkpoint'); return;
        }
        if (event.data === 'abort') {
            state.trace.push('abort'); abort(); server.send('ack/abort'); return;
        }
        state.trace.push('message'); controller = new AbortController();
        if (syncContext) {
            server.send('started');
            if (role === 'b') return;
            writeContexts('before');
        }
        if (kind.startsWith('fetch-abort-pre-')) abort();
        state.trace.push('fetch'); if (!syncContext) server.send('started');
        const errors = [];
        const count = kind === 'fetch-abort-live-shared' ? 2 : 1;
        for (let index = 0; index < count; ++index) {
            const options = {signal: controller.signal};
            if (kind === 'fetch-custom-headers') options.headers = {
                'User-Agent': 'caller/' + challenge, 'Accept-Encoding': 'identity', Origin: 'https://caller.example',
                'Cache-Control': 'max-age=17', Pragma: 'caller', 'X-Capsid-Header': challenge,
            };
            fetch((index === 1 ? secondTarget : target) + '&index=' + index, options).then(async response => {
                state.trace.push('headers');
                if (syncContext) writeContexts('callback');
                if (kind === 'fetch-complete' || kind === 'fetch-custom-headers' || kind === 'fetch-disabled-cookie-cache' || syncContext || role === 'b') {
                    state.result = await response.text(); state.trace.push('complete');
                    server.send('done'); server.close(1000, 'done'); return;
                }
                const reader = response.body.getReader(), first = await reader.read();
                state.trace.push('first'); state.result = {first: new TextDecoder().decode(first.value), done: first.done};
                if (kind === 'fetch-cancel-reader') {
                    state.trace.push('cancel/before'); await reader.cancel(challenge); state.trace.push('cancel/after');
                    server.send('done'); server.close(1000, 'done'); return;
                }
                server.send('reading');
                const second = await reader.read(); state.trace.push('second/' + second.done);
            }).catch(error => {
                state.trace.push(count === 1 ? 'failure' : 'failure/' + index);
                errors[index] = error;
                if (index === 0) state.failure = describe(error, controller.signal.reason);
                if (count === 2 && errors.filter(value => value !== undefined).length === 2) {
                    state.clones = {sameErrors: errors[0] === errors[1], sameReason0: errors[0] === controller.signal.reason,
                        sameReason1: errors[1] === controller.signal.reason, sameNested: errors[0].left === errors[1].left,
                        sharedAlias0: errors[0].left === errors[0].right, sharedAlias1: errors[1].left === errors[1].right,
                        firstNested: errors[0].left?.value, secondNested: errors[1].left?.value};
                }
                if (kind.startsWith('fetch-abort-') && (count === 1 || state.clones)) {
                    server.send('done'); server.close(1000, 'done');
                }
            });
        }
        if (syncContext) writeContexts('after');
    });
    return new Response(null, {status: 101, webSocket: pair[0]});
}};
