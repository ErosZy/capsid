const plans = {"codec-supp-upper":{"bytes":97,"encoding":"GZIP"},"codec-supp-alias":{"bytes":97,"encoding":"x-gzip"},"codec-supp-ows":{"bytes":97,"encoding":"gzip "},"codec-supp-double":{"bytes":97,"encoding":"gzip, gzip"},"codec-supp-head":{"bytes":97,"method":"HEAD"},"codec-supp-status-204":{"bytes":97,"status":204},"codec-supp-status-205":{"bytes":97,"status":205},"codec-supp-status-304":{"bytes":97,"status":304},"codec-supp-extra":{"bytes":97,"operation":"extra"},"codec-supp-name":{"bytes":97,"operation":"name"},"codec-supp-comment":{"bytes":97,"operation":"comment"},"codec-supp-header-crc":{"bytes":97,"operation":"header-crc"},"codec-supp-header-crc-invalid":{"bytes":97,"operation":"header-crc-invalid"},"codec-supp-reserved":{"bytes":97,"operation":"reserved"},"codec-supp-isize-invalid":{"bytes":97,"operation":"isize-invalid"},"codec-supp-fragment-header":{"bytes":97,"fragment":"header"},"codec-supp-fragment-trailer":{"bytes":97,"fragment":"trailer"},"codec-supp-fragment-members":{"bytes":97,"fragment":"members"},"codec-supp-fragment-crc-late":{"bytes":97,"fragment":"crc-late"},"codec-supp-fragment-truncated-late":{"bytes":97,"fragment":"truncated-late"},"codec-supp-byob-members-64":{"bytes":97,"fragment":"members","view":64},"codec-supp-byob-members-8192":{"bytes":97,"fragment":"members","view":8192},"codec-supp-entropy":{"bytes":16389,"entropy":true},"codec-supp-entropy-byob":{"bytes":16389,"entropy":true,"view":8192},"codec-supp-raw-byob-4096":{"bytes":16389,"entropy":true,"view":4096,"raw":true,"encoding":"identity"},"codec-supp-gzip-byob-4096":{"bytes":16389,"entropy":true,"view":4096},"codec-supp-raw-byob-8192":{"bytes":16389,"entropy":true,"view":8192,"raw":true,"encoding":"identity"},"codec-supp-raw-byob-16384":{"bytes":16389,"entropy":true,"view":16384,"raw":true,"encoding":"identity"},"codec-supp-gzip-byob-16384":{"bytes":16389,"entropy":true,"view":16384},"codec-supp-raw-byob-65536":{"bytes":16389,"entropy":true,"view":65536,"raw":true,"encoding":"identity"},"codec-supp-gzip-byob-65536":{"bytes":16389,"entropy":true,"view":65536}};
let realm, current;
const describe = error => ({name: error?.name ?? null, message: error?.message ?? null});
const metadata = response => ({status: response.status, statusText: response.statusText, url: response.url,
    redirected: response.redirected, bodyUsed: response.bodyUsed, bodyNull: response.body === null, locked: response.body?.locked ?? null, headers: [...response.headers]});
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
                response = await fetch(url.searchParams.get('target'), {method: plans[state.kind].method ?? 'GET'}); state.before = metadata(response);
                state.headerCopies = {owned: [...new Headers(response.headers)],
                    iterable: [...new Headers([...response.headers])],
                    response: [...new Response(null, {headers: response.headers}).headers]};
                if (response.body === null) {state.reads = []; state.closed = null; return;}
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
