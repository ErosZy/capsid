const encoder = new TextEncoder();
let active;
const errorState = error => ({name: error?.name ?? null, message: error?.message ?? null});
const delay = () => new Promise(resolve => setTimeout(resolve, 0));
function inputState(view) {
    let backing;
    try {backing = Array.from(new Uint8Array(view.buffer));}
    catch (error) {backing = {errorName: error.name};}
    return {capacity: view.buffer.byteLength, offset: view.byteOffset, length: view.byteLength, backing};
}
function resultState(value) {
    return {done: value.done, type: value.value?.constructor.name ?? null,
        offset: value.value?.byteOffset ?? null, length: value.value?.byteLength ?? null,
        capacity: value.value?.buffer.byteLength ?? null,
        bytes: value.value ? Array.from(new Uint8Array(value.value.buffer, value.value.byteOffset, value.value.byteLength)) : null,
        backing: value.value ? Array.from(new Uint8Array(value.value.buffer)) : null};
}
async function guest(operation, retainReader) {
    if (!active) throw new Error('owner not established');
    const state = active; state.retainReader = retainReader;
    const buffer = new ArrayBuffer(24); new Uint8Array(buffer).fill(165);
    let view = new Uint8Array(buffer, 8, 8);
    if (operation === 'invalid-view') view = new Uint8Array(buffer, 8, 0);
    if (operation === 'detached-view') structuredClone(buffer, {transfer: [buffer]});
    const observed = {operation, trace: [], before: inputState(view), result: {phase: 'pending'}};
    state.guest = observed;
    try {
        let pending;
        if (operation === 'read') pending = state.reader.read(view);
        else if (operation === 'readAtLeast') pending = state.reader.readAtLeast(1, view);
        else if (operation === 'invalid-view' || operation === 'detached-view') pending = state.reader.read(view);
        else if (operation === 'wrong-receiver') pending = ReadableStreamBYOBReader.prototype.read.call({}, view);
        else if (operation === 'min-getter-error') pending = state.reader.read(view, {get min() {
            observed.trace.push('min'); throw new RangeError('context-min-sentinel');
        }});
        else if (operation === 'read-prototype') pending = ReadableStreamBYOBReader.prototype.read.call(state.reader, view);
        else if (operation === 'read-reflect') pending = Reflect.apply(ReadableStreamBYOBReader.prototype.read, state.reader, [view]);
        else throw new Error('unknown context operation');
        observed.afterIssue = inputState(view);
        Promise.resolve(pending).then(value => {observed.result = {phase: 'fulfilled', value: resultState(value)};},
            error => {observed.result = {phase: 'rejected', error: errorState(error)};});
    } catch (error) {
        observed.afterIssue = inputState(view);
        observed.result = {phase: 'threw', error: errorState(error)};
    }
    await delay();
    observed.afterCheckpoint = inputState(view);
    const response = Response.json(structuredClone(observed));
    state.guestFinished = true;
    return response;
}
export default {async fetch(request) {
    const url = new URL(request.url);
    if (url.pathname === '/healthy') return new Response('healthy');
    if (url.pathname === '/guest') return guest(url.searchParams.get('operation'), url.searchParams.get('retain') === '1');
    if (url.pathname !== '/owner') throw new Error('unknown context route');
    if (active) throw new Error('only one owner pair per fresh runtime');
    const response = await fetch(url.searchParams.get('target'));
    const reader = response.body.getReader({mode: 'byob'});
    // The owner keeps its own pending timer while the second request runs.
    // Resolving an owner Promise from a completed foreign request would test
    // workerd's separate cross-request Promise policy instead of this reader.
    const state = {reader, response, guestFinished: false, guest: null}; active = state;
    let pulling = false;
    const stream = new ReadableStream({
        start(controller) {controller.enqueue(encoder.encode('OWNER_READY\n'));},
        async pull(controller) {
            if (pulling) return;
            pulling = true;
            const result = {reads: [], failure: null};
            try {
                while (!state.guestFinished) await delay();
                controller.enqueue(encoder.encode('OWNER_GUEST_DONE\n'));
                for (let index = 0; index < 8; ++index) {
                    const input = new ArrayBuffer(80); new Uint8Array(input).fill(165);
                    const returned = await reader.read(new Uint8Array(input, 8, 64));
                    result.reads.push(resultState(returned));
                    if (returned.done) break;
                }
                result.eof = result.reads.at(-1)?.done === true;
                result.bodyUsed = response.bodyUsed;
                result.lockedBeforeRelease = response.body.locked;
                await delay();
                result.finalGuest = state.guest;
            } catch (error) {result.failure = errorState(error); result.finalGuest = state.guest;}
            finally {
                result.releaseAttempted = !state.retainReader;
                if (!state.retainReader) {
                    try {reader.releaseLock();} catch (error) {result.releaseError = errorState(error);}
                }
                result.lockedAfterRelease = response.body.locked;
            }
            controller.enqueue(encoder.encode(JSON.stringify(result)));
            controller.close();
        }
    });
    return new Response(stream, {headers: {'content-type': 'text/plain'}});
}};
