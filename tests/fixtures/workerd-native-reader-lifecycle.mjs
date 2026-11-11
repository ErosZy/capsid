let realm, current;
const apply = Reflect.apply, then = Promise.prototype.then, define = Object.defineProperty;
const descriptor = Object.getOwnPropertyDescriptor, getPrototype = Object.getPrototypeOf;
const typed = getPrototype(Uint8Array.prototype);
const bufferOf = descriptor(typed, 'buffer').get, offsetOf = descriptor(typed, 'byteOffset').get;
const lengthOf = descriptor(typed, 'byteLength').get, bufferLength = descriptor(ArrayBuffer.prototype, 'byteLength').get;
const keys = Object.keys, ErrorType = Error, TypeErrorType = TypeError;
const delay = milliseconds => new Promise(resolve => setTimeout(resolve, milliseconds));
const describe = value => ({type: typeof value, name: value?.name ?? null, message: value?.message ?? null,
    tag: value?.tag ?? null, keys: value && typeof value === 'object' ? keys(value) : null,
    isTypeError: value instanceof TypeErrorType});
function part(result) {
    const view = result.value, buffer = view ? apply(bufferOf, view, []) : null;
    return {done: result.done, type: view?.constructor.name ?? null,
        offset: view ? apply(offsetOf, view, []) : null, length: view ? apply(lengthOf, view, []) : null,
        backingBytes: buffer ? apply(bufferLength, buffer, []) : null,
        bytes: view ? Array.from(new Uint8Array(buffer, apply(offsetOf, view, []), apply(lengthOf, view, []))) : null};
}
function construct(stream, route, byob) {
    const Base = byob ? ReadableStreamBYOBReader : ReadableStreamDefaultReader;
    if (route === 'getReader') return stream.getReader(byob ? {mode: 'byob'} : undefined);
    if (route === 'direct') return new Base(stream);
    if (route === 'prototype') return new Base.prototype.constructor(stream);
    class Derived extends Base {}
    return route === 'subclass' ? new Derived(stream) : Reflect.construct(Base, [stream], Derived);
}
async function exercise(url, state, server) {
    const [source, route, mode, scenario] = state.kind.split('|'), byob = mode === 'byob';
    let response, stream, reader, restore = () => {};
    const pending = [], rawErrors = {};
    function watch(label, promise, reading = false) {
        const item = state[label] = {settled: false, result: null};
        const handled = apply(then, promise, [value => {
            item.settled = true;
            item.result = reading ? {value: part(value)} : {fulfilled: true, returnedUndefined: value === undefined};
        }, error => {
            rawErrors[label] = error; item.settled = true; item.result = {error: describe(error)};
        }]);
        pending.push(handled); return handled;
    }
    const issue = (label, minimum = 1, capacity = 16) => {
        const input = byob ? new ArrayBuffer(32) : null;
        if (input) new Uint8Array(input).fill(165);
        state[label + 'Input'] = {before: input ? apply(bufferLength, input, []) : null};
        let promise;
        try {promise = byob ? reader.read(new Uint8Array(input, 8, capacity), {min: minimum}) : reader.read();}
        catch (error) {state[label + 'Synchronous'] = describe(error); promise = Promise.reject(error);}
        state[label + 'Input'].after = input ? apply(bufferLength, input, []) : null;
        return watch(label, promise, true);
    };
    const attemptRelease = () => {
        try {state.release = {returnedUndefined: reader.releaseLock() === undefined};}
        catch (error) {state.release = {error: describe(error)};}
        state.lockedAfterReleaseAttempt = stream.locked;
    };
    try {
        response = source === 'native' ? await fetch(url.searchParams.get('target'),
            {headers: {Upgrade: 'websocket', 'X-Probe': state.challenge}}) : null;
        stream = response?.body ?? new ReadableStream({type: 'bytes', start(controller) {
            controller.enqueue(new Uint8Array([0, 102, 105, 114, 115, 116]));
            if (scenario === 'release-eof') controller.close();
        }});
        reader = construct(stream, route, byob);
        const before = reader.closed;
        state.closedBeforeStable = before === reader.closed; watch('closedBefore', before);
        if (scenario === 'release-open') {
            attemptRelease();
        } else if (scenario === 'release-eof') {
            await issue('data'); await issue('eof'); await pending[0];
            attemptRelease();
        } else if (scenario === 'intrinsics') {
            const changed = [], trap = name => function () {state.traps.push(name); throw new ErrorType('application trap ' + name);};
            for (const [object, key, name] of [
                [Reflect, 'apply', 'Reflect.apply'], [Reflect, 'construct', 'Reflect.construct'],
                [ArrayBuffer, 'isView', 'ArrayBuffer.isView'], [Object, 'getPrototypeOf', 'Object.getPrototypeOf'],
                [WeakMap.prototype, 'get', 'WeakMap.get'], [WeakMap.prototype, 'set', 'WeakMap.set'],
                [Set.prototype, 'add', 'Set.add'], [Set.prototype, 'delete', 'Set.delete'],
                [Set.prototype, 'forEach', 'Set.forEach'], [Promise.prototype, 'then', 'Promise.then'],
            ]) {
                const original = descriptor(object, key); changed.push([object, key, original]);
                define(object, key, {...original, value: trap(name)});
            }
            restore = () => {for (const [object, key, original] of changed) define(object, key, original);};
            try {await issue('data'); state.closedDuringStable = before === reader.closed;}
            finally {restore(); restore = () => {};}
        } else {
            await issue('prime', 1, byob ? 1 : 16);
            issue('first', byob ? 16 : 1);
            if (scenario === 'concurrent') issue('second', byob ? 16 : 1);
            await delay(30);
            state.beforeAction = {first: state.first.settled, second: state.second?.settled ?? null, closed: state.closedBefore.settled};
            if (scenario === 'release-pending') attemptRelease();
        }
        if (scenario.startsWith('release-')) {
            const after = reader.closed; state.closedAfterStable = after === reader.closed;
            state.closedIdentityRetained = before === after; watch('closedAfter', after);
        }
        if (!stream.locked) {
            reader = construct(stream, route, byob);
            if (scenario === 'release-open' || scenario === 'release-eof') await issue('retry');
            else {
                issue('retry'); await delay(30); state.retryBeforeCancel = state.retry.settled;
            }
            watch('replacementClosed', reader.closed);
        }
        const reason = {tag: 'consumer-stop'};
        await watch('cancel', reader.cancel(reason));
        await Promise.all(pending);
        state.errorIdentity = {firstClosed: rawErrors.first === rawErrors.closedBefore,
            firstAfter: rawErrors.first === rawErrors.closedAfter};
        reader.releaseLock(); state.locked = stream.locked; state.bodyUsed = response?.bodyUsed ?? null;
    } catch (error) {
        restore(); state.unexpected = describe(error);
        try {if (reader) {await reader.cancel(); reader.releaseLock();} else await stream?.cancel();} catch {}
    }
    server.send('done'); server.close(1000, 'done');
}
export default {fetch(request) {
    realm ??= crypto.randomUUID();
    const url = new URL(request.url);
    if (url.pathname.endsWith('/state')) return Response.json({realm, state: current ?? null});
    const state = current = {kind: url.searchParams.get('kind'), challenge: url.searchParams.get('challenge'),
        traps: [], unexpected: null};
    const pair = new WebSocketPair(), server = pair[1]; server.accept();
    // Return no application promise to the event dispatcher while testing the
    // Promise.prototype.then intrinsic. The task handles and reports its errors.
    server.addEventListener('message', event => {
        if (event.data !== 'go') return;
        server.send('started'); void exercise(url, state, server);
    });
    return new Response(null, {status: 101, webSocket: pair[0]});
}};
