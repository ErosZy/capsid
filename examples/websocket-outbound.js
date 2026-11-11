// Grant the target host/port through capsid.json permissions.fetch.allow
// (and the managed host's fetchTargets). Outbound access defaults to deny.
export default {
    async fetch(request) {
        const target = new URL(request.url).searchParams.get('target');
        if (!target) return new Response('Provide ?target=ws://host/path', { status: 400 });
        try {
            const reply = await new Promise((resolve, reject) => {
                const socket = new WebSocket(target);
                socket.binaryType = 'arraybuffer';
                let received = false;
                const timer = setTimeout(() => { socket.close(); reject(new Error('WebSocket response timed out')); }, 5000);
                socket.onopen = () => socket.send('Hello from Capsid');
                socket.onmessage = event => {
                    received = true;
                    clearTimeout(timer);
                    socket.close(1000, 'done');
                    resolve(event.data);
                };
                socket.onerror = () => { clearTimeout(timer); reject(new Error('WebSocket connection failed')); };
                socket.onclose = () => {
                    if (!received) { clearTimeout(timer); reject(new Error('WebSocket closed before its reply')); }
                };
            });
            return new Response(reply);
        } catch (error) {
            return new Response(error.message, { status: 502 });
        }
    },
};
