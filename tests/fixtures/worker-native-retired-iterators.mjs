const delay = () => new Promise(resolve => setTimeout(resolve, 0));
const describe = error => ({name: error?.name ?? null, message: error?.message ?? null});
const record = result => ({done: result.done, bytes: result.value === undefined ? null :
    Array.from(new Uint8Array(result.value.buffer, result.value.byteOffset, result.value.byteLength)),
    offset: result.value?.byteOffset ?? null, capacity: result.value?.buffer.byteLength ?? null});
let active;
function snapshot() {
    return {graph: active.graph, phase: active.phase, chunks: active.chunks,
        error: active.error ?? null, outerThenRan: active.outerThenRan,
        bodyLocked: active.response.body.locked,
        leaves: active.leaves.map(stream => ({locked: stream.locked}))};
}
export default {async fetch(request) {
    const url = new URL(request.url);
    if (url.pathname === '/healthy') return new Response('healthy');
    if (url.pathname === '/guest') {
        for (let i = 0; i < 8; ++i) await delay();
        return Response.json(snapshot());
    }
    if (url.pathname !== '/owner' || active) throw new Error('one iterator graph per fresh runtime');
    const target = url.searchParams.get('target'), response = await fetch(target);
    const graph = new URL(target).searchParams.get('readerRoute');
    const [topology, ...action] = graph.split('-'), mode = action.join('-');
    let leaves;
    if (topology === 'plain') leaves = [response.body];
    else if (topology === 'tee') leaves = response.body.tee();
    else if (topology === 'clone') {const copy = response.clone(); leaves = [response.body, copy.body];}
    else throw new Error('unknown iterator topology');
    const state = {graph, response, leaves, phase: 'pending', chunks: [], outerThenRan: false};
    active = state;
    let ready;
    const prefix = new Promise(resolve => {ready = resolve;});
    const append = value => {state.chunks.push(record(value)); ready();};
    if (mode === 'next' || mode === 'prevent') {
        state.iterator = leaves[0].values({preventCancel: mode === 'prevent'});
        append(await state.iterator.next());
        state.pending = state.iterator.next().then(value => {
            append(value); state.phase = 'fulfilled';
        }, error => {state.phase = 'rejected'; state.error = describe(error);});
    } else if (mode === 'loop' || mode === 'prelude') {
        state.pending = (async () => {
            if (mode === 'prelude') await Promise.resolve();
            try {
                let count = 0;
                for await (const value of leaves[0]) {
                    append({done: false, value});
                    if (++count > 32) throw new Error('finite iterator bound');
                }
                state.phase = 'fulfilled';
            } catch (error) {state.phase = 'rejected'; state.error = describe(error); ready();}
        })();
    } else throw new Error('unknown iterator observation mode');
    state.pending = state.pending.then(() => {state.outerThenRan = true;});
    await prefix;
    await delay();
    return Response.json(snapshot());
}};
