const describe = (error, reason) => ({name: error?.name ?? null, message: error?.message ?? null,
    sameReason: error === reason, nativePrototype: Object.getPrototypeOf(error) === globalThis[error?.name]?.prototype});
const metadata = response => ({status: response.status, bodyUsed: response.bodyUsed, locked: response.body.locked, headers: [...response.headers]});
export default {async fetch(request) {
    const url = new URL(request.url); if (url.pathname === '/healthy') return new Response('healthy');
    const route = url.searchParams.get('route'), api = url.searchParams.get('api'), action = url.searchParams.get('action');
    const target = url.searchParams.get('target'), reason = new Error('native pending stop'), aborter = new AbortController();
    const state = {route, api, action, failure: null, trace: [], tails: []}; let response, reader, firstSettled = false, closedSettled = false, watched;
    const read = label => {
        const buffer = api === 'default' ? null : new ArrayBuffer(24);
        if (buffer) new Uint8Array(buffer).fill(165);
        const row = {label, initialInputBytes: buffer?.byteLength ?? null};
        try {
            const view = !buffer ? null : api.endsWith('DataView') ? new DataView(buffer, 8, 8)
                : api.endsWith('Uint16Array') ? new Uint16Array(buffer, 8, 4) : new Uint8Array(buffer, 8, 8);
            const pending = api === 'default' ? reader.read() : api.startsWith('least-') ? reader.readAtLeast(2, view) : reader.read(view);
            row.returnedPromise = pending instanceof Promise; row.inputAfterIssue = buffer?.byteLength ?? null;
            return pending.then(({done, value}) => {
                state.trace.push(label+'.fulfilled'); if (label === 'first') firstSettled = true;
                return {...row, inputAfter: buffer?.byteLength ?? null, done, type: value?.constructor.name ?? null,
                    offset: value?.byteOffset ?? null, length: value?.byteLength ?? null, capacity: value?.buffer.byteLength ?? null,
                    bytes: value ? Array.from(new Uint8Array(value.buffer, value.byteOffset, value.byteLength)) : null,
                    backing: value ? Array.from(new Uint8Array(value.buffer)) : null};
            }, error => {
                state.trace.push(label+'.rejected'); if (label === 'first') firstSettled = true;
                return {...row, inputAfter: buffer?.byteLength ?? null, error: describe(error, reason)};
            });
        } catch (error) {return Promise.resolve({...row, inputAfter: buffer?.byteLength ?? null, syncError: describe(error, reason)});}
    };
    try {
        response = await fetch(target, {signal: aborter.signal}); state.before = metadata(response);
        const Constructor = api === 'default' ? ReadableStreamDefaultReader : ReadableStreamBYOBReader;
        reader = route === 'getReader' ? response.body.getReader(api === 'default' ? undefined : {mode: 'byob'})
            : route === 'direct' ? new Constructor(response.body)
            : route === 'prototype' ? new Constructor.prototype.constructor(response.body)
            : route === 'subclass' ? new (class extends Constructor {})(response.body)
            : Reflect.construct(Constructor, [response.body], class extends Constructor {});
        state.construction = {instanceOfReader: reader instanceof Constructor, constructorMatches: reader.constructor === Constructor,
            directPrototype: Object.getPrototypeOf(reader) === Constructor.prototype,
            prototypeParent: Object.getPrototypeOf(Object.getPrototypeOf(reader)) === Constructor.prototype};
        const closed = reader.closed; state.closedStable = closed === reader.closed;
        watched = closed.then(() => {closedSettled = true; state.closed = {fulfilled: true}; state.trace.push('closed.fulfilled');},
            error => {closedSettled = true; state.closed = {error: describe(error, reason)}; state.trace.push('closed.rejected');});
        const first = read('first'); state.second = await read('second');
        try {reader.releaseLock(); state.releaseWhilePending = {fulfilled: true};}
        catch (error) {state.releaseWhilePending = {syncError: describe(error, reason)};}
        state.preControl = {firstPending: !firstSettled, closedPending: !closedSettled, response: metadata(response)};
        const control = new URL('/control', target);
        control.searchParams.set('phase', JSON.stringify({action, preControl: state.preControl, second: state.second, release: state.releaseWhilePending}));
        const ack = await fetch(control); state.control = {status: ack.status, text: await ack.text()};
        if (action === 'cancel') {
            await reader.cancel(reason).then(() => {state.cancellation = {fulfilled: true}; state.trace.push('cancel.fulfilled');},
                error => {state.cancellation = {error: describe(error, reason)}; state.trace.push('cancel.rejected');});
        } else if (action === 'abort') {aborter.abort(reason); state.aborted = aborter.signal.aborted; state.trace.push('abort.called');}
        state.first = await first;
        for (let index = 0; index < 8; ++index) {
            const row = await read('tail'+index); state.tails.push(row);
            if (row.done || row.error || row.syncError) break;
            if (index === 7) throw new Error('finite native pending read bound');
        }
        await watched;
    } catch (error) {state.failure = describe(error, reason);}
    finally {
        if (reader) try {reader.releaseLock(); state.released = true;} catch (error) {state.releaseError = describe(error, reason);}
        if (response) state.after = metadata(response);
    }
    return Response.json(state);
}};
