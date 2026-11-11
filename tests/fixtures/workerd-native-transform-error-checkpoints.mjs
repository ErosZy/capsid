const encoder = new TextEncoder();
const delay = () => new Promise(resolve => setTimeout(resolve, 0));
const describe = error => ({name: error?.name ?? null, message: error?.message ?? null});
const bytes = value => value === undefined ? null : Array.from(new Uint8Array(value.buffer, value.byteOffset, value.byteLength));
const metadata = response => ({status: response.status, statusText: response.statusText,
    ok: response.ok, url: response.url, redirected: response.redirected, type: response.type,
    bodyUsed: response.bodyUsed, locked: response.body.locked, headers: [...response.headers]});
let active;

function startJob(stream, action, index) {
    const job = {index, action, phase: 'pending', ready: false, chunks: [], error: null};
    const marker = new Error('branch-graph-sentinel');
    const result = value => ({done: value.done, type: value.value?.constructor.name ?? null,
        offset: value.value?.byteOffset ?? null, capacity: value.value?.buffer.byteLength ?? null,
        bytes: bytes(value.value)});
    const append = value => {job.chunks.push(result(value)); if (!value.done) job.ready = true;};
    const drain = async (body, byob = false) => {
        const reader = byob ? body.getReader({mode: 'byob'}) : body.getReader();
        job.readerLockedAtStart = body.locked;
        try {
            for (let i = 0; i < 32; ++i) {
                let value;
                if (byob) {
                    const buffer = new ArrayBuffer(80); new Uint8Array(buffer).fill(165);
                    const view = new Uint8Array(buffer, 8, 64);
                    const pending = reader.read(view);
                    job.lastInputCapacityAfterIssue = buffer.byteLength;
                    value = await pending;
                } else value = await reader.read();
                append(value);
                if (value.done) return;
            }
            throw new Error('finite graph read bound');
        } finally {
            reader.releaseLock(); job.readerReleased = !body.locked;
        }
    };
    const pending = (async () => {
        if (action === 'cancel') {
            job.ready = true;
            await stream.cancel('graph-left-cancel');
            job.cancelReturnedUndefined = true;
        } else if (action === 'values-break' || action === 'values-prevent-cancel') {
            const iterator = stream.values({preventCancel: action === 'values-prevent-cancel'});
            job.iteratorLockedAtStart = stream.locked;
            const value = await iterator.next(); job.chunks.push(result(value));
            const returned = await iterator.return();
            job.iteratorReturn = result(returned); job.lockedAfterReturn = stream.locked;
            job.ready = true;
            if (action === 'values-prevent-cancel') await drain(stream);
        } else if (action === 'pipe-reject') {
            const sink = new WritableStream({
                write(chunk) {job.chunks.push(result({done: false, value: chunk})); throw marker;},
                close() {job.sinkClose = true;}, abort(reason) {job.sinkAbort = describe(reason);}
            });
            try {await stream.pipeTo(sink);}
            finally {job.sinkLockedAfter = sink.locked;}
        } else if (action === 'transform-reject') {
            const transform = new TransformStream({transform(chunk) {
                job.transformInput = result({done: false, value: chunk}); throw marker;
            }});
            try {await drain(stream.pipeThrough(transform));}
            finally {
                job.transformLocksAfter = {writable: transform.writable.locked, readable: transform.readable.locked};
                const acquire = () => {
                    try {const writer = transform.writable.getWriter(); writer.releaseLock(); return {acquired: true};}
                    catch (error) {return {acquired: false, error: describe(error)};}
                };
                job.writerImmediatelyAfterReadError = acquire();
                job.sourceLockImmediatelyAfterReadError = stream.locked;
                job.microtaskLocks = [];
                for (let i = 0; i < 8; ++i) {
                    await Promise.resolve();
                    job.microtaskLocks.push({writable: transform.writable.locked, source: stream.locked});
                }
                await delay();
                job.transformLocksAfterTurn = {writable: transform.writable.locked, readable: transform.readable.locked};
                job.writerAfterTurn = acquire();
                job.sourceLockAfterTurn = stream.locked;
            }
        } else await drain(stream, action === 'drain-byob');
    })().then(() => {job.phase = 'fulfilled'; job.ready = true;}, error => {
        job.phase = 'rejected'; job.error = {...describe(error), sameMarker: error === marker}; job.ready = true;
    });
    return {job, pending};
}

export default {async fetch(request) {
    const url = new URL(request.url);
    if (url.pathname === '/healthy') return new Response('healthy');
    if (url.pathname === '/guest') {
        const state = active;
        if (!state) throw new Error('owner graph not established');
        await delay();
        const result = {graph: state.graph, before: state.before,
            current: metadata(state.response), copies: state.copies.map(metadata),
            leaves: state.leaves.map(stream => ({locked: stream.locked})),
            jobs: structuredClone(state.jobs.map(value => value.job))};
        state.guestDone = true;
        return Response.json(result);
    }
    if (url.pathname !== '/owner' || active) throw new Error('one graph per fresh runtime');
    const target = url.searchParams.get('target');
    const graph = new URL(target).searchParams.get('readerRoute');
    const response = await fetch(target);
    const before = metadata(response), copies = [];
    let leaves;
    if (graph === 'nested-tee') {
        const [a, b] = response.body.tee(); leaves = [a, ...b.tee()];
    } else if (graph === 'clone-tee') {
        const copy = response.clone(); copies.push(copy); leaves = [response.body, ...copy.body.tee()];
    } else if (graph.startsWith('tee-')) leaves = response.body.tee();
    else if (graph.startsWith('clone-')) {
        const copy = response.clone(); copies.push(copy); leaves = [response.body, copy.body];
    } else throw new Error('unknown graph topology');
    let action = graph.replace(/^(tee|clone)-/, '');
    if (graph === 'nested-tee' || graph === 'clone-tee') action = 'drain-default';
    const jobs = leaves.map((stream, index) => startJob(stream,
        index === 0 ? (action === 'cancel-first' ? 'cancel' : action) :
            action === 'drain-byob' ? 'drain-byob' : 'drain-default', index));
    const state = {graph, response, before, copies, leaves, jobs, guestDone: false}; active = state;
    // A real source prefix has reached every consuming leaf before the guest
    // observes it; the source suffix stays held by the parent until both HTTP
    // request barriers. Cancellation must preserve the other live leaves.
    while (!jobs.every(value => value.job.ready)) await delay();
    await delay();
    let pulling = false;
    return new Response(new ReadableStream({
        start(controller) {controller.enqueue(encoder.encode('OWNER_READY\n'));},
        async pull(controller) {
            if (pulling) return; pulling = true;
            while (!state.guestDone) await delay();
            controller.enqueue(encoder.encode('OWNER_GUEST_DONE\n'));
            await Promise.all(jobs.map(value => value.pending));
            const result = {graph, before, after: metadata(response), copies: copies.map(metadata),
                leaves: leaves.map(stream => ({locked: stream.locked})), jobs: jobs.map(value => value.job)};
            controller.enqueue(encoder.encode(JSON.stringify(result))); controller.close();
        }
    }), {headers: {'content-type': 'text/plain'}});
}};
