const describe = error => ({name: error?.name ?? null, message: error?.message ?? null});
const hex = bytes => Array.from(new Uint8Array(bytes), value => value.toString(16).padStart(2, '0')).join('');
const digest = async bytes => hex(await crypto.subtle.digest('SHA-256', bytes));

async function consume(response, capacity) {
    const state = {failure: null, reads: [], total: 0, status: response.status,
        statusText: response.statusText, headers: [...response.headers]};
    let reader, watched;
    try {
        reader = response.body.getReader({mode: 'byob'});
        const closed = reader.closed;
        state.closedStable = closed === reader.closed;
        watched = closed.then(() => {state.closed = {fulfilled: true};}, error => {state.closed = {error: describe(error)};});
        let backing = new ArrayBuffer(capacity + 16);
        for (let index = 0;; index++) {
            if (index >= 5000) throw new Error('large-body finite read bound exceeded');
            new Uint8Array(backing).fill(165);
            const input = backing;
            const pending = reader.read(new Uint8Array(input, 8, capacity));
            const inputAfterIssue = input.byteLength;
            const {done, value} = await pending;
            state.reads.push({done, inputAfterIssue, inputAfter: input.byteLength,
                type: value.constructor.name, offset: value.byteOffset, length: value.byteLength,
                capacity: value.buffer.byteLength, dataHash: await digest(value), backingHash: await digest(value.buffer)});
            state.total += value.byteLength;
            backing = value.buffer;
            if (done) break;
            await new Promise(resolve => setTimeout(resolve, 1));
        }
        await watched;
    } catch (error) {
        state.failure = describe(error);
        if (watched) await watched;
    } finally {
        if (reader) try {reader.releaseLock();} catch (error) {state.releaseError = describe(error);}
        state.after = {bodyUsed: response.bodyUsed, locked: response.body.locked};
    }
    return state;
}

export default {async fetch(request) {
    const url = new URL(request.url);
    if (url.pathname.endsWith('/healthy')) return new Response('healthy');
    const [size, style] = url.searchParams.get('view').split('|');
    const state = {style, prefix: null, branches: [], failure: null};
    let retained=[],owner;
    try {
        const original = await fetch(url.searchParams.get('target'));owner=original;
        let response = original;
        if (style === 'started-child') {
            const outer = original.body.tee();
            response = new Response(outer[0], original);
            outer[1].cancel().catch(() => {});
            const first = response.body.getReader({mode: 'byob'});
            const input = new Uint8Array(32);
            const pending = first.read(input, {min: 32});
            const inputAfterIssue = input.byteLength;
            const result = await pending;
            state.prefix = {done: result.done, bytes: Array.from(result.value),
                offset: result.value.byteOffset, capacity: result.value.buffer.byteLength,
                inputAfterIssue, inputAfter: input.byteLength};
            first.releaseLock();
        } else if (style !== 'unstarted' && style !== 'cancel-control') throw new Error('Unknown branch style');
        const pair = response.body.tee();
        const responses = pair.map(body => new Response(body, response));
        state.prepared = responses.map(item => ({bodyUsed: item.bodyUsed, locked: item.body.locked}));
        retained=responses;
        if(style==='cancel-control')state.cancel=await Promise.all(responses.map(item=>item.body.cancel()));
        else for (const item of responses) state.branches.push(await consume(item, Number(size)));
        state.outer = {bodyUsed: original.bodyUsed, locked: original.body.locked};
    } catch (error) {
        state.failure = describe(error);
    }
    try {state.ownerGate=await (await fetch(url.searchParams.get('target')+'/marker')).json();}
    catch(error){state.ownerGateFailure=describe(error);}
    state.retainedAfterGate=retained.map(item=>({bodyUsed:item.bodyUsed,locked:item.body.locked}));
    state.originalAfterGate=owner?{bodyUsed:owner.bodyUsed,locked:owner.body.locked}:null;
    return Response.json(state);
}};
