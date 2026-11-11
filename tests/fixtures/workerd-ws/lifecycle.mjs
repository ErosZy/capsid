let lateMessages = 0;
export default { fetch(request) {
    const path = new URL(request.url).pathname;
    if (path === '/stats') return Response.json({lateMessages});
    const [client, server] = new WebSocketPair();
    server.accept({allowHalfOpen: path === '/half'});
    if (path === '/half') server.addEventListener('close', event => {
        server.send(JSON.stringify({code:event.code,reason:event.reason,clean:event.wasClean,state:server.readyState}));
        server.close(1000, 'half done');
    });
    if (path === '/sender-close') {
        server.addEventListener('message', () => { lateMessages++; });
        server.close(3000, 'sender closed');
    }
    return new Response(null, {status:101, webSocket:client});
} };
