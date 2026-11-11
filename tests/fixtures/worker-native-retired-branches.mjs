const delay = () => new Promise(resolve => setTimeout(resolve, 0));
const describe = error => ({name: error?.name ?? null, message: error?.message ?? null});
const result = value => ({done: value.done, type: value.value?.constructor.name ?? null,
    offset: value.value?.byteOffset ?? null, capacity: value.value?.buffer.byteLength ?? null,
    bytes: value.value === undefined ? null : Array.from(new Uint8Array(value.value.buffer,
        value.value.byteOffset, value.value.byteLength))});
const metadata = response => ({status: response.status, statusText: response.statusText,
    ok: response.ok, url: response.url, redirected: response.redirected, type: response.type,
    bodyUsed: response.bodyUsed, locked: response.body.locked, headers: [...response.headers]});
let active;

function startJob(stream, graph, index) {
    const job = {index, phase: 'pending', closedPhase: 'not-acquired', chunks: []};
    let markReady;
    const ready = new Promise(resolve => {markReady = resolve;});
    const retained = {job, stream, ready};
    let body = stream;
    if (graph === 'tee-transform' && index === 0) {
        retained.transform = new TransformStream({transform(chunk, controller) {controller.enqueue(chunk);}});
        body = stream.pipeThrough(retained.transform);
    }
    const append = value => {job.chunks.push(result(value)); markReady();};
    let pending;
    if (graph === 'clone-pipe' && index === 0) {
        retained.sink = new WritableStream({
            write(value) {append({done: false, value});}, close() {job.sinkClosed = true;},
            abort(reason) {job.sinkAbort = describe(reason);}
        });
        pending = body.pipeTo(retained.sink);
    } else {
        const byob = graph.endsWith('-byob');
        const reader = byob ? body.getReader({mode: 'byob'}) : body.getReader();
        retained.reader = reader; retained.readBody = body; job.closedPhase = 'pending';
        reader.closed.then(() => {job.closedPhase = 'fulfilled';}, error => {
            job.closedPhase = 'rejected'; job.closedError = describe(error);
        });
        pending = (async () => {
            try {
                for (let i = 0; i < 32; ++i) {
                    let value;
                    if (byob) {
                        const buffer = new ArrayBuffer(80); new Uint8Array(buffer).fill(165);
                        value = await reader.read(new Uint8Array(buffer, 8, 64));
                    } else value = await reader.read();
                    append(value);
                    if (value.done) return;
                }
                throw new Error('finite retired graph read bound');
            } finally {
                try {reader.releaseLock(); job.readerReleased = !body.locked;}
                catch (error) {job.releaseError = describe(error);}
            }
        })();
    }
    retained.pending = pending.then(() => {job.phase = 'fulfilled'; markReady();}, error => {
        job.phase = 'rejected'; job.error = describe(error); markReady();
    });
    return retained;
}

function snapshot(state) {
    return {graph: state.graph, response: metadata(state.response), copies: state.copies.map(metadata),
        leaves: state.leaves.map(stream => ({locked: stream.locked})),
        jobs: structuredClone(state.jobs.map(value => ({...value.job,
            transformLocks: value.transform ? {readable: value.transform.readable.locked,
                writable: value.transform.writable.locked} : null,
            sinkLocked: value.sink?.locked ?? null}))) };
}

export default {async fetch(request) {
    const url = new URL(request.url);
    if (url.pathname === '/healthy') return new Response('healthy');
    if (url.pathname === '/guest') {
        if (!active) throw new Error('retired graph has not been established');
        for (let i = 0; i < 8; ++i) await delay();
        return Response.json(snapshot(active));
    }
    if (url.pathname !== '/owner' || active) throw new Error('one retired graph per fresh runtime');
    const target = url.searchParams.get('target');
    const graph = new URL(target).searchParams.get('readerRoute');
    const response = await fetch(target), copies = [];
    let leaves;
    if (graph === 'nested-tee') {const [a, b] = response.body.tee(); leaves = [a, ...b.tee()];}
    else if (graph === 'clone-tee') {const copy = response.clone(); copies.push(copy); leaves = [response.body, ...copy.body.tee()];}
    else if (graph.startsWith('tee-')) leaves = response.body.tee();
    else if (graph.startsWith('clone-')) {const copy = response.clone(); copies.push(copy); leaves = [response.body, copy.body];}
    else throw new Error('unknown retired graph topology');
    const jobs = leaves.map((stream, index) => startJob(stream, graph, index));
    const state = {graph, response, copies, leaves, jobs}; active = state;
    await Promise.all(jobs.map(value => value.ready));
    await delay();
    // Complete the owner HTTP response with all graph objects retained. A new
    // request observes their state after the parent's actual RESPONSE_END.
    return Response.json(snapshot(state));
}};
