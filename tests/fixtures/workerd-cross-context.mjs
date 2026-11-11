// Commands are driven by independent native-client tests. Each scenario starts
// a new realm, keeps two connections open, and reads state through another HTTP
// request so ownership cannot be inferred from a single callback's arguments.
const sockets = new Map(), trace = [], events = [];
let realm, challenge, promise, resolve, reject, reason, moduleResolve;
const modulePromise = new Promise(yes => moduleResolve = yes);
function sends(where) {
    for (const [role, socket] of sockets) {
        try {socket.send(where + '/' + role); trace.push(where + '/' + role + '/ok');}
        catch (error) {trace.push(where + '/' + role + '/' + error.name + ': ' + error.message);}
    }
}
for (const type of ['unhandledrejection', 'rejectionhandled']) addEventListener(type, event => {
    events.push({type, samePromise: event.promise === promise, sameReason: event.reason === reason,
        reasonType: typeof event.reason, trusted: event.isTrusted, cancelable: event.cancelable,
        bubbles: event.bubbles, defaultPrevented: event.defaultPrevented});
    trace.push('event/' + type); sends(type);
});
function action(command, role) {
    trace.push(role + '/' + command);
    if (command === 'create') {
        reason = new TypeError('cross-context/' + role);
        promise = new Promise((yes, no) => {resolve = yes; reject = no;});
    } else if (command === 'use-module') {promise = modulePromise; resolve = moduleResolve;}
    else if (command === 'attach') {
        promise.then(() => {trace.push('reaction/' + role); sends('reaction');});
    } else if (command === 'attach-throw') {
        promise.then(() => {trace.push('reaction/' + role); throw reason;});
    } else if (command === 'await' || command === 'all') {
        (async () => {await (command === 'all' ? Promise.all([promise]) : promise);
            trace.push(command + '/' + role); sends(command);})();
    } else if (command === 'resolve-order' || command === 'reject-order') {
        queueMicrotask(() => {trace.push('micro/before'); queueMicrotask(() => trace.push('micro/nested-before'));});
        if (command === 'resolve-order') resolve('ok'); else reject(reason);
        trace.push('settle/after');
        queueMicrotask(() => {trace.push('micro/after'); queueMicrotask(() => trace.push('micro/nested-after'));});
        setTimeout(() => trace.push('timer/settler'), 0);
    } else if (command === 'resolve-thenable') resolve({get then() {
        trace.push('getter/' + role); sends('getter');
        return yes => {trace.push('thenable/' + role); sends('thenable'); yes('ok');};
    }});
    else if (command === 'resolve') resolve('ok');
    else if (command === 'reject') reject(reason);
    else if (command === 'catch') {
        trace.push('catch/before');
        promise.catch(error => {trace.push('catch/' + role + '/' + (error === reason)); sends('catch');});
        trace.push('catch/after');
    } else if (command === 'finish') sockets.get(role).close(1000, 'done');
    else throw new Error('unknown command ' + command);
}
export default {async fetch(request) {
    realm ??= crypto.randomUUID();
    const url = new URL(request.url), role = url.searchParams.get('role');
    challenge ??= url.searchParams.get('challenge') ?? undefined;
    if (url.pathname.endsWith('/state')) return Response.json({realm, challenge, trace, events});
    if (url.pathname.endsWith('/http')) {action(url.searchParams.get('command'), 'http'); return Response.json({realm, challenge});}
    const pair = new WebSocketPair(), server = pair[1]; server.accept(); sockets.set(role, server);
    server.addEventListener('message', event => {
        if (event.data === 'finish') {server.send('ack/' + role + '/finish'); action('finish', role); return;}
        action(event.data, role); server.send('ack/' + role + '/' + event.data);
    });
    server.addEventListener('close', () => trace.push('close/' + role));
    return new Response(null, {status: 101, webSocket: pair[0]});
}};
