const describe = error => ({name: error?.name ?? null, message: error?.message ?? null});
const hex = bytes => Array.from(new Uint8Array(bytes), value => value.toString(16).padStart(2, '0')).join('');
export default {async fetch(request) {
    const url = new URL(request.url);
    if (url.pathname === '/healthy') return new Response('healthy');
    const capacity = Number(url.searchParams.get('view'));
    const state = {failure: null, reads: [], total: 0};
    let response, reader, watched;
    try {
        response = await fetch(url.searchParams.get('target'));
        state.headers = [...response.headers];
        state.expectedBytes = Number(response.headers.get('x-decoded-bytes'));
        reader = response.body.getReader({mode: 'byob'});
        const closed = reader.closed;
        state.closedStable = closed === reader.closed;
        watched = closed.then(() => {state.closed = {fulfilled: true};}, error => {state.closed = {error: describe(error)};});
        let backing = new ArrayBuffer(capacity+16);
        for (let index = 0;; index++) {
            if (index >= 5000) throw new Error('large-body finite read bound exceeded');
            new Uint8Array(backing).fill(165);
            const input = backing;
            const pending = reader.read(new Uint8Array(input, 8, capacity));
            const inputAfterIssue = input.byteLength;
            const {done, value} = await pending;
            const dataHash = hex(await crypto.subtle.digest('SHA-256', value));
            const backingHash = hex(await crypto.subtle.digest('SHA-256', value.buffer));
            state.reads.push({done, inputAfterIssue, inputAfter: input.byteLength,
                type: value.constructor.name, offset: value.byteOffset, length: value.byteLength,
                capacity: value.buffer.byteLength, dataHash, backingHash});
            state.total += value.byteLength;
            backing = value.buffer; // Reuse one transferred destination for the entire response.
            if (done) break;
            await new Promise(resolve => setTimeout(resolve, 1));
        }
        await watched;
    } catch (error) {
        state.failure = describe(error);
        if (watched) await watched;
    } finally {
        if (reader) try {reader.releaseLock();} catch (error) {state.releaseError = describe(error);}
        if (response) state.after = {bodyUsed: response.bodyUsed, locked: response.body.locked};
    }
    return Response.json(state);
}};
