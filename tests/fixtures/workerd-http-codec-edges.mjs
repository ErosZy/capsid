const plans = {"codec-edge-crc-0":{"bytes":0,"operation":"crc"},"codec-edge-crc-97":{"bytes":97,"operation":"crc"},"codec-edge-crc-4095":{"bytes":4095,"operation":"crc"},"codec-edge-crc-4096":{"bytes":4096,"operation":"crc"},"codec-edge-crc-4097":{"bytes":4097,"operation":"crc"},"codec-edge-crc-8192":{"bytes":8192,"operation":"crc"},"codec-edge-crc-8197":{"bytes":8197,"operation":"crc"},"codec-edge-crc-16389":{"bytes":16389,"operation":"crc"},"codec-edge-truncated-0":{"bytes":0,"operation":"truncated"},"codec-edge-truncated-97":{"bytes":97,"operation":"truncated"},"codec-edge-truncated-4096":{"bytes":4096,"operation":"truncated"},"codec-edge-truncated-4097":{"bytes":4097,"operation":"truncated"},"codec-edge-truncated-8192":{"bytes":8192,"operation":"truncated"},"codec-edge-truncated-8197":{"bytes":8197,"operation":"truncated"},"codec-edge-empty":{"bytes":0,"operation":"valid","split":0},"codec-edge-wire-empty":{"bytes":0,"operation":"wire-empty","split":0},"codec-edge-members":{"bytes":97,"operation":"members","split":49},"codec-edge-member-first-empty":{"bytes":97,"operation":"members","split":0},"codec-edge-member-last-empty":{"bytes":97,"operation":"members","split":97},"codec-edge-members-empty":{"bytes":0,"operation":"members","split":0},"codec-edge-member-second-crc":{"bytes":97,"operation":"member-second-crc","split":49},"codec-edge-member-second-truncated":{"bytes":97,"operation":"member-second-truncated","split":49},"codec-edge-trailing-byte":{"bytes":97,"operation":"trailing-byte","split":0},"codec-edge-byob-16-valid":{"bytes":8197,"operation":"valid","view":16},"codec-edge-byob-16-crc":{"bytes":8197,"operation":"crc","view":16},"codec-edge-byob-64-valid":{"bytes":8197,"operation":"valid","view":64},"codec-edge-byob-64-crc":{"bytes":8197,"operation":"crc","view":64},"codec-edge-byob-8192-valid":{"bytes":8197,"operation":"valid","view":8192},"codec-edge-byob-8192-crc":{"bytes":8197,"operation":"crc","view":8192}};
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
                const plan = plans[state.kind];
                reader = response.body.getReader(plan.view ? {mode:'byob'} : undefined); const closed = reader.closed;
                state.closedStable = closed === reader.closed;
                watched = closed.then(() => {state.closed = {fulfilled: true};}, error => {state.closed = {error: describe(error)};});
                state.reads = []; state.closedAfterRead = [];
                for (let index = 0;; ++index) {
                    if (index > 1000) throw new Error('finite codec probe exceeded read bound');
                    const plan = plans[state.kind], input = plan.view ? new ArrayBuffer(plan.view + 16) : null;
                    if(input) new Uint8Array(input).fill(165);
                    const result = plan.view ? reader.read(new Uint8Array(input, 8, plan.view)) : reader.read();
                    const inputAfter = input?.byteLength ?? null;
                    const {done, value} = await result;
                    state.reads.push({inputAfter, done, type: value?.constructor.name ?? null, offset: value?.byteOffset ?? null,
                        length: value?.byteLength ?? null, capacity: value?.buffer.byteLength ?? null,
                        bytes: value ? Array.from(value) : null, backing: value ? Array.from(new Uint8Array(value.buffer)) : null});
                    await Promise.resolve(); state.closedAfterRead.push(state.closed ?? null);
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
