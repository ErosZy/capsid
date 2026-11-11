const describe = (error, reason) => ({name: error?.name ?? null, message: error?.message ?? null, sameReason: error === reason});
const metadata = response => ({status: response.status, bodyUsed: response.bodyUsed, locked: response.body.locked, headers: [...response.headers]});
export default {async fetch(request) {
    const url = new URL(request.url);
    if (url.pathname === '/healthy') return new Response('healthy');
    const action = url.searchParams.get('action'), view = Number(url.searchParams.get('view'));
    const state = {action, failure: null, reads: []};
    const aborter = new AbortController(), reason = new Error('owner probe stop');
    let response, reader, watched;
    const read = async () => {
        const input = new ArrayBuffer(view+16); new Uint8Array(input).fill(165);
        try {
            const pending = reader.read(new Uint8Array(input, 8, view)); const inputAfterIssue = input.byteLength;
            try {
                const {done, value} = await pending;
                return {done, inputAfterIssue, inputAfter: input.byteLength, type: value?.constructor.name ?? null,
                    offset: value?.byteOffset ?? null, length: value?.byteLength ?? null, capacity: value?.buffer.byteLength ?? null,
                    bytes: value ? Array.from(value) : null, backing: value ? Array.from(new Uint8Array(value.buffer)) : null};
            } catch (error) {return {error: describe(error, reason), inputAfterIssue, inputAfter: input.byteLength};}
        } catch (error) {return {syncError: describe(error, reason), inputAfter: input.byteLength};}
    };
    try {
        response = await fetch(url.searchParams.get('target'), {signal: aborter.signal}); state.before = metadata(response);
        if (action === 'retain') {globalThis.retainedResponse = response; state.retained = true;}
        else if (action === 'stream-cancel') {await response.body.cancel(reason); state.cancel = {fulfilled: true};}
        else {
            reader = response.body.getReader({mode: 'byob'});
            const closed = reader.closed; state.closedStable = closed === reader.closed;
            watched = closed.then(() => {state.closed = {fulfilled: true};}, error => {state.closed = {error: describe(error, reason)};});
            if (action === 'read-cancel') state.reads.push(await read());
            if (action === 'abort') {aborter.abort(reason); state.aborted = aborter.signal.aborted;}
            else {await reader.cancel(reason); state.cancel = {fulfilled: true};}
            state.reads.push(await read()); await watched;
        }
    } catch (error) {state.failure = describe(error, reason); if (watched) await watched;}
    finally {
        if (reader) try {reader.releaseLock();} catch (error) {state.releaseError = describe(error, reason);}
        if (response) state.after = metadata(response);
    }
    return Response.json(state);
}};
