let realm, current;
const describe = error => ({name: error?.name ?? null, message: error?.message ?? null});
const metadata = response => ({status: response.status, statusText: response.statusText, url: response.url,
    redirected: response.redirected, bodyUsed: response.bodyUsed, locked: response.body.locked, headers: [...response.headers]});
export default {async fetch(request) {
    realm ??= crypto.randomUUID(); const url = new URL(request.url);
    if (url.pathname.endsWith('/state')) return Response.json({realm, state: current ?? null});
    const state = current = {kind: url.searchParams.get('kind'), challenge: url.searchParams.get('challenge'), failure: null};
    const pair = new WebSocketPair(), server = pair[1]; server.accept();
    server.addEventListener('message', event => {
        if (event.data !== 'go') return;
        server.send('started');
        void (async () => {
            let response, reader, watched;
            try {
                response = await fetch(url.searchParams.get('target')); state.before = metadata(response);
                reader = response.body.getReader(); const closed = reader.closed;
                state.closedStable = closed === reader.closed;
                watched = closed.then(() => {state.closed = {fulfilled: true};}, error => {state.closed = {error: describe(error)};});
                state.reads = [];
                for (let index = 0;; ++index) {
                    if (index > 1000) throw new Error('finite codec probe exceeded read bound');
                    const {done, value} = await reader.read();
                    state.reads.push({done, type: value?.constructor.name ?? null, offset: value?.byteOffset ?? null,
                        length: value?.byteLength ?? null, capacity: value?.buffer.byteLength ?? null,
                        bytes: value ? Array.from(value) : null, backing: value ? Array.from(new Uint8Array(value.buffer)) : null});
                    if (done) break;
                }
                await watched;
            } catch (error) {state.failure = describe(error); if (watched) await watched;}
            finally {
                if (reader) try {reader.releaseLock();} catch (error) {state.releaseError = describe(error);}
                if (response) state.after = metadata(response);
                server.send('done'); server.close(1000, 'done');
            }
        })();
    });
    return new Response(null, {status: 101, webSocket: pair[0]});
}};
