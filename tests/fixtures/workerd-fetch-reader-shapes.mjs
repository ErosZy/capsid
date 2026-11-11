let realm, current;
const describe = error => ({name: error.name, message: error.message});
const digest = async bytes => Array.from(new Uint8Array(await crypto.subtle.digest('SHA-256', bytes)), value => value.toString(16).padStart(2, '0')).join('');
const shape = async result => ({done: result.done, type: result.value?.constructor.name ?? null,
    byteOffset: result.value?.byteOffset ?? null, byteLength: result.value?.byteLength ?? null,
    bufferBytes: result.value?.buffer.byteLength ?? null,
    head: result.value ? Array.from(result.value.subarray(0, 16)) : null,
    tail: result.value ? Array.from(result.value.subarray(-16)) : null,
    sha256: result.value ? await digest(result.value) : null,
    backingSHA256: result.value ? await digest(result.value.buffer) : null});
export default {fetch(request) {
    realm ??= crypto.randomUUID();
    const url = new URL(request.url);
    if (url.pathname.endsWith('/state')) return Response.json({realm, state: current ?? null});
    const kind = url.searchParams.get('kind'), challenge = url.searchParams.get('challenge');
    const match = /^chunks-(default|byob|least)-(\d+)-(\d+)$/.exec(kind);
    if (!match) return new Response('Unknown case', {status: 400});
    const [, readerKind, lengthText, minimumText] = match, length = Number(lengthText), minimum = Number(minimumText);
    const state = current = {kind, challenge, unexpected: null, reads: [], total: 0, invalidBytes: 0, closed: null};
    const pair = new WebSocketPair(), server = pair[1]; server.accept();
    server.addEventListener('message', async event => {
        if (event.data !== 'go') return;
        server.send('started'); let reader;
        try {
            const response = await fetch(url.searchParams.get('target'), {headers: {Upgrade: 'websocket', 'X-Probe': challenge}});
            state.response = {status: response.status, contentLength: response.headers.get('content-length')};
            reader = readerKind === 'default' ? response.body.getReader() : response.body.getReader({mode: 'byob'});
            const closed = reader.closed.then(() => {state.closed = {fulfilled: true};}, error => {state.closed = {error: describe(error)};});
            const all = new Uint8Array(length);
            for (let index = 0; index <= length + 1; ++index) {
                let pending, input;
                if (readerKind === 'default') pending = reader.read();
                else {
                    input = new ArrayBuffer(length + 64); new Uint8Array(input).fill(165);
                    const view = new Uint8Array(input, 16, length + 32);
                    pending = readerKind === 'least' ? reader.readAtLeast(minimum, view) : reader.read(view, {min: minimum});
                }
                const afterCall = input?.byteLength ?? null, value = await pending;
                const row = await shape(value); row.inputBytesAfterCall = afterCall; row.closedAfterRead = state.closed;
                state.reads.push(row);
                if (value.value) {
                    const bytes = value.value;
                    for (let i = 0; i < bytes.length; ++i) if (bytes[i] !== ((state.total + i) * 29 + 17) % 251) state.invalidBytes++;
                    all.set(bytes, state.total); state.total += bytes.length;
                }
                if (value.done) break;
                if (!value.value?.byteLength) throw new Error('Nonterminal read did not advance');
            }
            state.sha256 = await digest(all); await closed;
            state.bodyUsed = response.bodyUsed;
            reader.releaseLock(); state.lockedAfterRelease = response.body.locked;
        } catch (error) {
            state.unexpected = describe(error);
            if (reader) try {await reader.cancel(); reader.releaseLock();} catch {}
        }
        server.send('done'); server.close(1000, 'done');
    });
    return new Response(null, {status: 101, webSocket: pair[0]});
}};
