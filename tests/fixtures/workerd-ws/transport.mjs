export default {
    fetch(request) {
        if (request.headers.get('upgrade')?.toLowerCase() !== 'websocket') {
            return new Response('upgrade required', {status: 400});
        }
        const [client, server] = new WebSocketPair();
        server.accept();
        server.binaryType = 'arraybuffer';
        server.addEventListener('message', event => server.send(event.data));
        const protocol = new URL(request.url).pathname === '/protocol';
        return new Response(null, {status: 101, webSocket: client,
            headers: protocol ? {'sec-websocket-protocol': 'chat'} : undefined});
    },
};
