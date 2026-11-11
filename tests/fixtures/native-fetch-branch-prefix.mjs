async function measurePrefixBranches(target, api) {
    const [graph, method, action] = api.split('-');
    const state = {graph, method, action, failure: null, controls: [], branches: []};
    const aborter = new AbortController(), abortReason = new Error('branch transport stop');
    const reasons = [new Error('branch zero stop'), new Error('branch one stop'), new Error('branch two stop')];
    const describe = (error, reason) => ({name: error?.name ?? null, message: error?.message ?? null,
        sameReason: error === reason, nativePrototype: Object.getPrototypeOf(error) === globalThis[error?.name]?.prototype});
    const metadata = response => ({status: response.status, bodyUsed: response.bodyUsed, locked: response.body.locked,
        headers: [...response.headers]});
    const encode = bytes => btoa(Array.from(bytes, value => String.fromCharCode(value)).join(''));
    let response, branches, readers, watches, firsts;
    const snapshot = () => state.branches.map(item => ({slot: item.slot, firstState: item.firstState,
        closedState: item.closedState, cancellationState: item.cancellationState,
        inputAfterIssue: item.inputAfterIssue, locked: branches[item.slot].locked}));
    const control = async (phase, data) => {
        const url = new URL('/control', target);
        url.searchParams.set('phase', phase); url.searchParams.set('data', JSON.stringify(data));
        const result = await fetch(url); state.controls.push({phase, status: result.status, text: await result.text()});
    };
    const read = (slot, label) => {
        const item = state.branches[slot], reader = readers[slot];
        const size = slot === 0 ? 65536 : 1024;
        const backing = item.byob ? new ArrayBuffer(size + 32) : null;
        if (backing) new Uint8Array(backing).fill(165);
        const input = slot === 0 && method === 'wide' ? new Float64Array(backing, 8, size / 8) : new DataView(backing, 8, size);
        const row = {label, inputBefore: backing?.byteLength ?? null};
        try {
            const pending = reader.readAtLeast(slot === 1 ? 8 : method === 'wide' ? 4096 : 65536, input);
            row.returnedPromise = pending instanceof Promise; row.inputAfterIssue = backing?.byteLength ?? null;
            if (label === 'first') item.inputAfterIssue = row.inputAfterIssue;
            return pending.then(result => {
                if (label === 'first') item.firstState = 'fulfilled';
                row.inputAfter = backing?.byteLength ?? null; row.done = result.done;
                const value = result.value;
                row.value = value === undefined ? null : {type: value.constructor.name, offset: value.byteOffset,
                    length: value.byteLength, capacity: value.buffer.byteLength,
                    bytes: encode(new Uint8Array(value.buffer, value.byteOffset, value.byteLength)),
                    backing: encode(new Uint8Array(value.buffer))};
                return row;
            }, error => {
                if (label === 'first') item.firstState = 'rejected';
                row.inputAfter = backing?.byteLength ?? null; row.error = describe(error, aborter.signal.aborted ? abortReason : reasons[slot]);
                return row;
            });
        } catch (error) {row.syncError = describe(error, reasons[slot]); return Promise.resolve(row);}
    };
    try {
        response = await fetch(target, {signal: aborter.signal}); state.before = metadata(response);
        if (graph === 'clone') {
            const copy = response.clone(); branches = [response.body, copy.body]; state.copy = metadata(copy);
        } else if (graph === 'nested') {
            const outer = response.body.tee(); branches = [outer[0], ...outer[1].tee()];
        } else branches = response.body.tee();
        state.afterGraph = metadata(response);
        readers = branches.map((stream, slot) => {
            const byob = true;
            state.branches.push({slot, byob, firstState: 'pending', closedState: 'pending', cancellationState: 'none', tails: []});
            return stream.getReader(byob ? {mode: 'byob'} : undefined);
        });
        watches = readers.map((reader, slot) => {
            const item = state.branches[slot], closed = reader.closed;
            item.closedStable = closed === reader.closed;
            return closed.then(() => {item.closedState = 'fulfilled'; item.closed = {fulfilled: true};}, error => {
                item.closedState = 'rejected'; item.closed = {error: describe(error, aborter.signal.aborted ? abortReason : reasons[slot])};
            });
        });
        firsts = readers.map((_reader, slot) => read(slot, 'first'));
        state.prepared = snapshot(); await control('prepared', state.prepared);
        await control('prefix', {}); state.afterPrefix = snapshot(); await control('observed-prefix', state.afterPrefix);
        const cancellations = [];
        const cancel = slot => {
            const item = state.branches[slot]; item.cancellationState = 'pending';
            const result = readers[slot].cancel(reasons[slot]);
            cancellations.push(result.then(() => {item.cancellationState = 'fulfilled'; item.cancellation = {fulfilled: true};},
                error => {item.cancellationState = 'rejected'; item.cancellation = {error: describe(error, reasons[slot])};}));
        };
        if (action === 'cancel0' || action === 'cancel0abort') cancel(0);
        else if (action === 'cancel1' || action === 'cancel1abort') cancel(1);
        else if (action === 'all') for (let slot = 0; slot < readers.length; slot++) cancel(slot);
        if (action === 'abort' || action.endsWith('abort')) {aborter.abort(abortReason); state.aborted = aborter.signal.aborted;}
        await control('called', {action});
        state.afterAction = snapshot(); await control('after-action', state.afterAction);
        await Promise.all(readers.map(async (_reader, slot) => {
            const item = state.branches[slot]; item.first = await firsts[slot];
            for (let index = 0; index < 128; index++) {
                const row = await read(slot, 'tail' + index); item.tails.push(row);
                if (row.done || row.error || row.syncError) break;
                if (index === 127) throw new Error('bounded held branch failed to end');
            }
            await watches[slot]; readers[slot].releaseLock(); item.released = true; item.lockedAfterRelease = branches[slot].locked;
        }));
        await Promise.all(cancellations); state.final = snapshot(); state.after = metadata(response);
    } catch (error) {state.failure = describe(error, abortReason);}
    return state;
}
export default {async fetch(request) {
    const url = new URL(request.url);
    if (url.pathname === '/healthy') return new Response('healthy');
    return Response.json(await measurePrefixBranches(url.searchParams.get('target'), url.searchParams.get('api')));
}};
