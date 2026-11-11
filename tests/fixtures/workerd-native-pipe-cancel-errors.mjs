const encoder = new TextEncoder();
const delay = () => new Promise(resolve => setTimeout(resolve, 0));
const describe = error => ({name: error?.name ?? null, message: error?.message ?? null});
let active, cancelAllowed = false;
function control(name, kind, preventCancel = false) {
    const marker = new Error('pipe-boundary-' + name);
    const watches = [];
    const state = {name, kind, preventCancel, read: {phase: 'pending'}, closed: {phase: 'pending'},
        pipe: {phase: 'unobserved'}, cancel: {called: false}};
    let controller;
    const transform = new TransformStream({start(value) {controller = value;}, transform() {throw marker;}});
    let source;
    if (kind === 'self-cycle') source = transform.readable;
    else if (kind === 'two-cycle') {
        const other = new TransformStream(); source = other.readable;
        transform.readable.pipeThrough(other);
    } else source = new ReadableStream({start(value) {value.enqueue(new Uint8Array([7]));},
        cancel(reason) {
            state.cancel = {called: true, reason: describe(reason), sameMarker: reason === marker, phase: 'pending'};
            const failure = new RangeError('cancel-boundary-' + name);
            if (name.includes('~cancel-throw')) {state.cancel.phase = 'rejected'; throw failure;}
            return (async () => {
                while (!cancelAllowed) await delay();
                state.cancel.phase = 'rejected'; throw failure;
            })();
        }});
    const watch = (promise, field) => {const watched = promise.then(value => {
        state[field] = {phase: 'fulfilled', ...(field === 'read' ? {done: value.done, bytes: value.value ? [...value.value] : null} : {})};
    }, error => {state[field] = {phase: 'rejected', error: describe(error), sameMarker: error === marker};}); watches.push(watched); return watched;};
    const consume = () => {
        try {
            const reader = transform.readable.getReader(); state.readerAcquired = true;
            watch(reader.read(), 'read'); watch(reader.closed, 'closed');
            return reader;
        } catch (error) {state.readerAcquired = false; state.acquireErrorName = error.name; return null;}
    };
    if (kind === 'preerrored') controller.error(marker);
    if (kind === 'direct') watch(source.pipeTo(transform.writable, {preventCancel}), 'pipe');
    else if (kind === 'mismatched') source.pipeThrough({readable: new ReadableStream({start(value) {value.close();}}), writable: transform.writable}, {preventCancel});
    else source.pipeThrough(transform, {preventCancel});
    if (kind === 'self-cycle' || kind === 'two-cycle') controller.error(marker);
    state.locksImmediatelyAfterConnect = {source: source.locked, writable: transform.writable.locked, readable: transform.readable.locked};
    let reader = consume();
    const snapshot = () => ({...structuredClone(state),
        liveLocks: {source: source.locked, writable: transform.writable.locked, readable: transform.readable.locked}});
    return {snapshot, async finish() {
        while (state.cancel.called && state.cancel.phase === 'pending') await delay();
        await Promise.all(watches);
        if (reader) {reader.releaseLock(); reader = null;}
        for (let i = 0; i < 12; ++i) await Promise.resolve();
    }, async checkpoint() {
        for (let i = 0; i < 12; ++i) await Promise.resolve();
        if (!reader) {reader = consume(); for (let i = 0; i < 12; ++i) await Promise.resolve();}
        await delay();
    }};
}
export default {async fetch(request) {
    const url = new URL(request.url);
    if (url.pathname === '/healthy') return new Response('healthy');
    if (url.pathname === '/guest') {
        const snapshot = {controls: active.controls.map(value => value.snapshot()), prefix: [...active.prefix]};
        active.guestDone = true; cancelAllowed = true;
        return Response.json(snapshot);
    }
    if (url.pathname !== '/owner' || active) throw new Error('one controls owner');
    const target = url.searchParams.get('target');
    const selector = new URL(target).searchParams.get('readerRoute');
    const response = await fetch(target), reader = response.body.getReader();
    const prefix = await reader.read();
    const rest = (async () => {const bytes = [...prefix.value]; for (let i = 0; i < 32; ++i) {
        const value = await reader.read(); if (value.done) {reader.releaseLock(); return bytes;} bytes.push(...value.value);
    } throw new Error('finite source');})();
    const controls = [control(selector, selector.split('~')[0], false)];
    for (const value of controls) await value.checkpoint();
    const state = {controls, prefix: prefix.value, guestDone: false}; active = state;
    let pulling = false;
    return new Response(new ReadableStream({start(value) {value.enqueue(encoder.encode('OWNER_READY\n'));},
        async pull(value) {
            if (pulling) return; pulling = true;
            while (!state.guestDone) await delay();
            value.enqueue(encoder.encode('OWNER_GUEST_DONE\n'));
            const bytes = await rest;
            for (const item of controls) {await item.checkpoint(); await item.finish();}
            value.enqueue(encoder.encode(JSON.stringify({bytes, controls: controls.map(item => item.snapshot())})));
            value.close();
        }}));
}};
