const apply = Reflect.apply, then = Promise.prototype.then;
const descriptor = Object.getOwnPropertyDescriptor, define = Object.defineProperty;
const typed = Object.getPrototypeOf(Uint8Array.prototype);
const bufferOf = descriptor(typed, 'buffer').get, offsetOf = descriptor(typed, 'byteOffset').get;
const lengthOf = descriptor(typed, 'byteLength').get;
const bufferLength = descriptor(ArrayBuffer.prototype, 'byteLength').get;
const ErrorType = Error;
let realm, current = null;

function acquire(stream, route) {
    const Base = ReadableStreamBYOBReader;
    if (route === 'getReader') return stream.getReader({mode: 'byob'});
    if (route === 'direct') return new Base(stream);
    if (route === 'prototype') return new Base.prototype.constructor(stream);
    class Derived extends Base {}
    return route === 'subclass' ? new Derived(stream) : Reflect.construct(Base, [stream], Derived);
}
async function exercise(state, server) {
    const [route, selection, phase] = state.kind.split('|');
    const stream = new ReadableStream({type: 'bytes', start(controller) {
        controller.enqueue(new Uint8Array([0, 102, 105, 114, 115, 116]));
    }});
    const reader = acquire(stream, route), closed = reader.closed;
    const observedClosed = apply(then, closed, [() => {state.closed = 'fulfilled';},
        error => {state.closed = {name: error.name, message: error.message};}]);
    state.closedStable = closed === reader.closed;
    const input = new ArrayBuffer(32); new Uint8Array(input).fill(165);
    state.inputBefore = apply(bufferLength, input, []);
    const changed = [];
    try {
        if (phase === 'deferred') await Promise.resolve();
        for (const [object, key, name] of [
            [Reflect, 'apply', 'Reflect.apply'], [Reflect, 'construct', 'Reflect.construct'],
            [ArrayBuffer, 'isView', 'ArrayBuffer.isView'], [Object, 'getPrototypeOf', 'Object.getPrototypeOf'],
            [WeakMap.prototype, 'get', 'WeakMap.get'], [WeakMap.prototype, 'set', 'WeakMap.set'],
            [Set.prototype, 'add', 'Set.add'], [Set.prototype, 'delete', 'Set.delete'],
            [Set.prototype, 'forEach', 'Set.forEach'], [Promise.prototype, 'then', 'Promise.then'],
        ]) {
            if (selection !== 'all' && !selection.split('+').includes(name)) continue;
            const original = descriptor(object, key); changed.push([object, key, original]);
            define(object, key, {...original, value() {
                state.traps.push(name); throw new ErrorType('application trap ' + name);
            }});
        }
        state.trace.push('mutated');
        const result = reader.read(new Uint8Array(input, 8, 16));
        state.inputAfter = apply(bufferLength, input, []);
        const value = await result, buffer = apply(bufferOf, value.value, []);
        state.read = {done: value.done, type: value.value.constructor.name,
            offset: apply(offsetOf, value.value, []), length: apply(lengthOf, value.value, []),
            backing: Array.from(new Uint8Array(buffer))};
        state.closedDuringStable = closed === reader.closed;
        state.trace.push('read');
    } catch (error) {state.readError = {name: error.name, message: error.message};}
    finally {
        for (const [object, key, original] of changed) define(object, key, original);
        state.trace.push('restored');
    }
    try {
        state.cancelReturnedUndefined = await reader.cancel({tag: 'consumer-stop'}) === undefined;
        await observedClosed; state.trace.push('cancelled');
        reader.releaseLock(); state.locked = stream.locked; state.trace.push('released');
    } catch (error) {state.unexpected = {name: error.name, message: error.message};}
    server.send('done:' + state.challenge); server.close(1000, 'done');
}
export default {fetch(request) {
    realm ??= crypto.randomUUID();
    const url = new URL(request.url);
    if (url.pathname.endsWith('/state')) return Response.json({realm, state: current});
    const state = current = {kind: url.searchParams.get('kind'), challenge: url.searchParams.get('challenge'),
        trace: [], traps: [], readError: null, unexpected: null};
    const pair = new WebSocketPair(), server = pair[1]; server.accept();
    server.addEventListener('message', event => {
        if (event.data !== 'go') return;
        state.trace.push('listener'); server.send('started:' + state.challenge);
        void exercise(state, server);
    });
    server.addEventListener('message', event => {
        if (event.data === 'go') state.trace.push('second-listener');
    });
    return new Response(null, {status: 101, webSocket: pair[0]});
}};
