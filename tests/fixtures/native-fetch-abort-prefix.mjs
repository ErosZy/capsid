const describe = (error, reason) => ({name: error?.name ?? null, message: error?.message ?? null, sameReason: error === reason});
const hex = bytes => Array.from(new Uint8Array(bytes), byte => byte.toString(16).padStart(2, '0')).join('');
async function observe(promise, pending) {
    let timer;
    try {return await Promise.race([promise, new Promise(resolve => {timer = setTimeout(() => resolve(pending()), 50);})]);}
    finally {clearTimeout(timer);}
}
export default {async fetch(request) {
    const url = new URL(request.url); if (url.pathname === '/healthy') return new Response('healthy');
    const capacity = Number(url.searchParams.get('view')), aborter = new AbortController(), reason = new Error('prefix probe stop');
    const state = {failure: null, reads: [], total: 0}; let response, reader, watched;
    const read = async (size, bounded) => {
        const input = new ArrayBuffer(size+16); new Uint8Array(input).fill(165);
        const pending = reader.read(new Uint8Array(input, 8, size)), inputAfterIssue = input.byteLength;
        const outcome = pending.then(result => ({result}), error => ({error}));
        const settled = bounded ? await observe(outcome, () => ({pending: true})) : await outcome;
        if (settled.pending) return {pending: true, inputAfterIssue, inputAfter: input.byteLength, capacity: size+16};
        if (settled.error) return {error: describe(settled.error, reason), inputAfterIssue, inputAfter: input.byteLength, capacity: size+16};
        const {done, value} = settled.result;
        return {done, inputAfterIssue, inputAfter: input.byteLength, type: value.constructor.name, offset: value.byteOffset,
            length: value.byteLength, capacity: value.buffer.byteLength,
            dataHash: hex(await crypto.subtle.digest('SHA-256', value)),
            backingHash: hex(await crypto.subtle.digest('SHA-256', value.buffer))};
    };
    try {
        response = await fetch(url.searchParams.get('target'), {signal: aborter.signal});
        state.headers = [...response.headers]; state.expectedBytes = Number(response.headers.get('x-decoded-bytes'));
        reader = response.body.getReader({mode: 'byob'});
        const closed = reader.closed; state.closedStable = closed === reader.closed;
        watched = closed.then(() => {state.closed = {fulfilled: true};}, error => {state.closed = {error: describe(error, reason)};});
        const first = await read(64, false); state.reads.push(first); state.total += first.length ?? 0;
        aborter.abort(reason); state.aborted = aborter.signal.aborted;
        for (let index = 0;; index++) {
            if (index >= 2048) throw new Error('finite abort-prefix bound exceeded');
            const row = await read(capacity, true); state.reads.push(row); state.total += row.length ?? 0;
            if (row.done || row.error || row.pending) break;
        }
        await observe(watched, () => {state.closed = {pending: true};});
    } catch (error) {state.failure = describe(error, reason); if (watched) await observe(watched, () => {state.closed = {pending: true};});}
    finally {
        if (reader) try {reader.releaseLock();} catch (error) {state.releaseError = describe(error, reason);}
        if (response) state.after = {bodyUsed: response.bodyUsed, locked: response.body.locked};
    }
    return Response.json(state);
}};
