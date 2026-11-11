const native = {setTimeout, setInterval, clearInterval, queueMicrotask, Promise};
let realm;
let current = null, currentSocket = null, lastThrown, pendingOther;
const error = kind => lastThrown = kind === 'microtask-primitive' ? 'primitive/' + kind : new TypeError('async-callback/' + kind);
for (const type of ['error', 'unhandledrejection', 'rejectionhandled']) addEventListener(type, event => {
    if (!current) return;
    current.trace.push('global/' + type);
    const value = type === 'error' ? event.error : event.reason;
    current.globalEvents.push({type, trusted: event.isTrusted, message: event.message ?? null,
        name: value?.name ?? null, errorMessage: value?.message ?? null,
        promise: type === 'error' ? null : event.promise instanceof native.Promise,
        valueType: typeof value, sameValue: value === lastThrown, bubbles: event.bubbles,
        cancelable: event.cancelable, defaultPrevented: event.defaultPrevented,
        filename: event.filename ?? null, lineno: event.lineno ?? null, colno: event.colno ?? null});
    if (type === 'error' && current.kind === 'microtask-error-listener-throw' && !current.observerThrew) {
        current.observerThrew = true; throw new RangeError('observer failure');
    }
    if (type === 'error' && current.kind === 'microtask-error-listener-close' || type === 'unhandledrejection' && current.kind === 'rejection-listener-close') currentSocket.close(1000, 'observer');
    if (type === 'unhandledrejection' && current.kind === 'rejection-listener-send') currentSocket.send('observed:' + current.challenge);
    if (type === 'unhandledrejection' && current.kind === 'rejection-listener-handle-next')
        pendingOther.catch(value => current.trace.push('other-caught/' + value.name));
    if (type === 'rejectionhandled' && current.kind === 'rejection-late-handled-send') currentSocket.send('handled:' + current.challenge);
    if (type === 'rejectionhandled' && current.kind === 'rejection-late-handled-close') currentSocket.close(1000, 'observer');
    if (type === 'unhandledrejection' && current.kind === 'rejection-listener-throw' && !current.observerThrew) {
        current.observerThrew = true; throw new RangeError('observer failure');
    }
});
export default {async fetch(request) {
    realm ??= crypto.randomUUID();
    const url = new URL(request.url);
    if (url.pathname.endsWith('/state')) return Response.json({realm, state: current});
    const kind = url.searchParams.get('kind'), challenge = url.searchParams.get('challenge');
    const state = {kind, challenge, trace: [], globalEvents: [], socketEvents: [], callbacks: 0, sum: 0, observerThrew: false};
    current = state;
    const run = (server = null) => {
        const complete = () => {
            state.trace.push('followup');
            server?.send('after:' + challenge);
            server?.close(1000, 'done');
        };
        const base = kind.replace(/^http-/, '');
        const fail = () => {state.callbacks++; state.trace.push('callback'); throw error(kind);};
        if (base === 'timeout-throw') native.setTimeout(fail, 25);
        else if (base === 'timeout-throw-microtask') native.setTimeout(() => {
            native.queueMicrotask.call(globalThis, () => state.trace.push('nested')); fail();
        }, 25);
        else if (base === 'interval-clear-throw') {
            const id = native.setInterval(() => {native.clearInterval(id); fail();}, 25);
        }
        else if (base === 'interval-throw') {
            const id = native.setInterval(() => {
                state.callbacks++; state.trace.push('interval/' + state.callbacks);
                if (state.callbacks === 1) throw error(kind);
                native.clearInterval(id); complete();
            }, 25);
            return;
        } else if (base.startsWith('rejection-late-handled-')) {
            native.setTimeout(() => {
            state.callbacks++; state.trace.push('callback');
            const rejected = native.Promise.reject(error(kind));
            native.setTimeout(() => {
                state.trace.push('attach/before');
                rejected.catch(value => {
                    state.trace.push('caught/' + value.name);
                    // Followup depends on handler execution, rather than a
                    // competing absolute timer that may close the socket first.
                    native.setTimeout(complete, 180);
                });
                state.trace.push('attach/after');
            }, 20);
            }, 25);
            return;
        }
        else if (base === 'rejection-listener-handle-next') native.setTimeout(() => {
            state.callbacks++; state.trace.push('callback'); native.Promise.reject(error(kind));
            pendingOther = native.Promise.reject(new RangeError('handled-next'));
        }, 25);
        else if (base === 'timeout-reject' || base.startsWith('rejection-listener-')) native.setTimeout(() => {
            state.callbacks++; state.trace.push('callback'); return native.Promise.reject(error(kind));
        }, 25);
        else if (base === 'timeout-async-reject') native.setTimeout(async () => {
            state.callbacks++; state.trace.push('callback'); await native.Promise.resolve();
            state.trace.push('async'); throw error(kind);
        }, 25);
        else if (base === 'microtask-throw' || base === 'microtask-primitive' || base.startsWith('microtask-error-listener-') || base === 'microtask-report-override') {
            if (base === 'microtask-report-override') globalThis.reportError = () => state.trace.push('replacement');
            native.queueMicrotask.call(globalThis, fail);
        }
        else if (base === 'promise-throw') native.Promise.resolve().then(fail);
        else if (base === 'timeout-caught') native.setTimeout(() => {
            try {fail();} catch (error) {state.trace.push('caught/' + error.name);}
        }, 25);
        else if (base === 'rejection-handled') native.setTimeout(() => {
            state.callbacks++; state.trace.push('callback');
            return native.Promise.reject(error(kind)).catch(value => state.trace.push('caught/' + value.name));
        }, 25);
        else if (base === 'timer-budget-loop') native.setTimeout(() => {state.trace.push('loop'); for (;;) {}}, 25);
        else if (base === 'promise-budget-loop') native.setTimeout(() => {
            // Keep the queue continuously runnable without making each
            // reaction adopt the next Promise. An ever-growing adoption
            // chain can exhaust the realm heap before its CPU deadline.
            state.trace.push('loop'); const loop = () => {native.Promise.resolve().then(loop);}; loop();
        }, 25);
        else if (base === 'message-budget-loop') {state.trace.push('loop'); for (;;) {}}
        else if (base === 'timer-budget-fresh') {
            native.setTimeout(() => {
                state.callbacks++; state.trace.push('callback');
                for (let i = 0; i < 10000; i++) state.sum = (state.sum + i) >>> 0;
                complete();
            }, 180); return;
        } else if (base === 'interval-budget-fresh') {
            const id = native.setInterval(() => {
                state.callbacks++; state.trace.push('interval/' + state.callbacks);
                if (state.callbacks === 3) {native.clearInterval(id); complete();}
            }, 90); return;
        } else throw new Error('unknown async callback case: ' + kind);
        // Allow the real Close ACK to reach the server before the late task.
        // The driver observes state again after this timer has become due.
        if (!base.endsWith('budget-loop')) native.setTimeout(complete,
            base.endsWith('listener-close') || base === 'rejection-late-handled-close' ? 180 : 60);
    };
    if (kind.startsWith('http-')) {
        state.trace.push('http'); run();
        await new native.Promise(resolve => native.setTimeout(resolve, 100));
        state.trace.push('response');
        return Response.json({realm, state});
    }
    const pair = new WebSocketPair(), server = pair[1]; currentSocket = server; server.accept();
    server.addEventListener('message', () => {
        state.trace.push('message'); server.send('queued:' + challenge); run(server);
    });
    for (const type of ['error', 'close']) server.addEventListener(type, event => {
        state.trace.push('socket/' + type);
        state.socketEvents.push({type, readyState: server.readyState,
            code: event.code ?? null, reason: event.reason ?? null,
            name: event.error?.name ?? null, message: event.message ?? null, trusted: event.isTrusted});
    });
    return new Response(null, {status: 101, webSocket: pair[0]});
}};
