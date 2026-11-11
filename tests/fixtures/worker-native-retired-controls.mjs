const delay = () => new Promise(resolve => setTimeout(resolve, 0));
const record = result => ({done: result.done, bytes: result.value === undefined ? null : Array.from(result.value)});
let active;
const snapshot = () => ({graph: active.graph, phase: active.phase, first: active.first ?? null,
    getterCalls: active.getterCalls, customThenCalls: active.customThenCalls,
    outerThenRan: active.outerThenRan, bodyLocked: active.response.body.locked});
export default {async fetch(request) {
    const url = new URL(request.url);
    if (url.pathname === '/healthy') return new Response('healthy');
    if (url.pathname === '/guest') {
        for (let i = 0; i < 8; ++i) await delay();
        return Response.json(snapshot());
    }
    if (url.pathname !== '/owner' || active) throw new Error('one followup control per runtime');
    const target = url.searchParams.get('target'), response = await fetch(target);
    const graph = new URL(target).searchParams.get('readerRoute');
    const reader = response.body.getReader();
    const state = {graph, response, reader, phase: 'read', getterCalls: 0, customThenCalls: 0, outerThenRan: false};
    active = state;
    const read = reader.read(); state.read = read;
    const nativeThen = Promise.prototype.then;
    if (graph === 'mixed-all' || graph === 'override-then') {
        nativeThen.call(read, value => {state.first = record(value);});
        if (graph === 'override-then') Object.defineProperty(read, 'then', {get() {
            ++state.getterCalls;
            return function (...handlers) {++state.customThenCalls; state.customHandlers = handlers;};
        }});
        if (graph === 'mixed-all') state.ordinary = new Promise(() => {});
        state.aggregate = graph === 'mixed-all' ? Promise.all([read, state.ordinary]) : Promise.all([read]);
        state.pending = state.aggregate.then(() => {state.outerThenRan = true;});
        state.phase = graph;
    } else {
        state.pending = (async () => {
            state.first = record(await read);
            state.phase = 'followup';
            if (graph === 'timer') await new Promise(resolve => setTimeout(resolve, 60000));
            else if (graph === 'ordinary') {state.ordinary = new Promise(() => {}); await state.ordinary;}
            else if (graph === 'timer-complete') await new Promise(resolve => setTimeout(resolve, 1));
            else if (graph === 'ordinary-complete') await new Promise(resolve => resolve());
            else throw new Error('unknown followup control');
            state.phase = 'fulfilled';
        })().then(() => {state.outerThenRan = true;});
    }
    while (!state.first) await delay();
    if (graph.endsWith('-complete')) await state.pending;
    return Response.json(snapshot());
}};
