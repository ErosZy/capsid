export default {
    fetch(request) {
        if (!new URL(request.url).pathname.endsWith('/ws'))
            return new Response('frame-boundary-healthy');
        const pair = new WebSocketPair();
        pair[1].accept();
        pair[1].binaryType = 'arraybuffer';
        pair[1].onmessage = event => pair[1].send(event.data);
        return new Response(null, {status: 101, webSocket: pair[0]});
    },
};
