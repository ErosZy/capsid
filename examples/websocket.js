export default {
    fetch(request) {
        if (request.headers.get('upgrade')?.toLowerCase() !== 'websocket') {
            return new Response('Connect with WebSocket to echo text or binary messages.');
        }
        const [client, server] = new WebSocketPair();
        server.accept();
        server.binaryType = 'arraybuffer';
        server.addEventListener('message', event => {
            server.send(event.data);
        });
        return new Response(null, { status: 101, webSocket: client });
    },
};
