async function measureNativeIntrinsics(target, api) {
    const define = Object.defineProperty, descriptor = Object.getOwnPropertyDescriptor;
    const prototypeOf = Object.getPrototypeOf, ErrorType = Error;
    const original = api === 'none' ? null : descriptor(WeakMap.prototype, api);
    const sentinel = new ErrorType('application WeakMap.' + api);
    const state = {api, failure: null, failurePhase: 'fetch', traps: [], returnedPromise: false,
        bytes: null, metadata: null, cleanupBytes: null, cleanupError: null};
    let response;
    try {
        if (original) define(WeakMap.prototype, api, {...original, value() { state.traps.push('WeakMap.' + api); throw sentinel; }});
        const pending = fetch(target); state.returnedPromise = pending instanceof Promise;
        response = await pending; state.failurePhase = 'headers';
        state.metadata = {status: response.status, statusText: response.statusText, headers: [...response.headers]};
        state.failurePhase = 'body'; state.bytes = Array.from(new Uint8Array(await response.arrayBuffer()));
        state.failurePhase = null;
    } catch (error) {
        state.failure = {name: error?.name ?? null, message: error?.message ?? null,
            sameSentinel: error === sentinel, nativePrototype: prototypeOf(error) === ErrorType.prototype};
    } finally {
        if (original) define(WeakMap.prototype, api, original);
        if (response && !response.bodyUsed) try { state.cleanupBytes = (await response.arrayBuffer()).byteLength; }
        catch (error) { state.cleanupError = {name: error.name, message: error.message}; }
    }
    return state;
}
export default {async fetch(request) {
    const url = new URL(request.url);
    if (url.pathname === '/healthy') return new Response('healthy');
    return Response.json(await measureNativeIntrinsics(url.searchParams.get('target'), url.searchParams.get('api')));
}};
