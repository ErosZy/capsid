const delay = () => new Promise(resolve => setTimeout(resolve, 0));
const describe = error => ({name: error?.name ?? null, message: error?.message ?? null});
const bytes = value => value === undefined ? null : Array.from(new Uint8Array(value.buffer, value.byteOffset, value.byteLength));
const record = value => ({done: value.done, bytes: bytes(value.value),
    offset: value.value?.byteOffset ?? null, capacity: value.value?.buffer.byteLength ?? null});
let active;
function snapshot() {
    return {graph: active.graph, first: active.first, phase: active.phase,
        next: active.next ?? null, error: active.error ?? null,
        outerThenRan: active.outerThenRan ?? false, finallyRan: active.finallyRan ?? false,
        bodyLocked: active.response.body.locked, leaves: active.leaves.map(stream => ({locked: stream.locked}))};
}
export default {async fetch(request) {
    const url = new URL(request.url);
    if (url.pathname === '/healthy') return new Response('healthy');
    if (url.pathname === '/guest') {
        for (let i = 0; i < 8; ++i) await delay();
        return Response.json(snapshot());
    }
    if (url.pathname !== '/owner' || active) throw new Error('one combinator graph per fresh runtime');
    const target = url.searchParams.get('target');
    const graph = new URL(target).searchParams.get('readerRoute');
    const [topology, ...action] = graph.split('-'), mode = action.join('-');
    const response = await fetch(target);
    let leaves;
    if (topology === 'plain') leaves = [response.body];
    else if (topology === 'tee') leaves = response.body.tee();
    else if (topology === 'clone') {const copy = response.clone(); leaves = [response.body, copy.body];}
    else throw new Error('unknown combinator topology');
    const reader = leaves[0].getReader();
    const first = record(await reader.read());
    const state = {graph, response, leaves, reader, first, phase: 'pending'}; active = state;
    const fulfilled = value => {state.phase = 'fulfilled'; state.next = record(value);};
    const rejected = error => {state.phase = 'rejected'; state.error = describe(error);};
    const read = () => reader.read();
    const finishAsync = pending => pending.then(() => {state.outerThenRan = true;});
    if (mode === 'direct') state.pending = read().then(fulfilled, rejected);
    else if (mode === 'await' || mode === 'await-then' || mode === 'await-prelude') {
        state.pending = (async () => {
            try {
                if (mode === 'await-prelude') await Promise.resolve();
                fulfilled(await read());
            } catch (error) {rejected(error);}
        })();
        if (mode !== 'await') state.pending = finishAsync(state.pending);
    } else if (mode === 'await-nested') {
        const inner = async () => await read();
        state.pending = finishAsync((async () => {
            try {fulfilled(await inner());} catch (error) {rejected(error);}
        })());
    } else if (mode === 'new-promise-adopt') state.pending = new Promise(resolve => resolve(read())).then(fulfilled, rejected);
    else if (mode === 'resolve-adopt') state.pending = Promise.resolve(read()).then(fulfilled, rejected);
    else if (mode === 'all-one') state.pending = Promise.all([read()]).then(values => fulfilled(values[0]), rejected);
    else if (mode === 'all-resolved') state.pending = Promise.all([Promise.resolve('ready'), read()]).then(values => fulfilled(values[1]), rejected);
    else if (mode === 'race-one') state.pending = Promise.race([read()]).then(fulfilled, rejected);
    else if (mode === 'any-one') state.pending = Promise.any([read()]).then(fulfilled, rejected);
    else if (mode === 'all-settled') state.pending = Promise.allSettled([read()]).then(values => {
        const value = values[0]; if (value.status === 'fulfilled') fulfilled(value.value); else rejected(value.reason);
    }, rejected);
    else if (mode === 'finally') state.pending = read().finally(() => {state.finallyRan = true;}).then(fulfilled, rejected);
    else if (mode === 'catch') state.pending = read().catch(error => {throw error;}).then(fulfilled, rejected);
    else if (mode === 'then-adopt') state.pending = Promise.resolve().then(read).then(fulfilled, rejected);
    else if (mode === 'async-return') state.pending = (async () => read())().then(fulfilled, rejected);
    else throw new Error('unknown combinator observation mode');
    return Response.json(snapshot());
}};
