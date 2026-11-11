export default {
    fetch(request) {
        const url = new URL(request.url);
        if (url.pathname === '/healthy') return new Response('healthy');
        const length = Number(url.searchParams.get('bytes'));
        const text = Array.from({length}, (_, index) => 'abcdefghijklmnopqrstuvwxyz0123456789'[index % 36]).join('');
        if (url.pathname === '/snapshot') return new Response(text);
        return new Response(new ReadableStream({
            start(controller) {
                controller.enqueue(new TextEncoder().encode(text));
                controller.close();
            }
        }), {headers: {'content-type': 'text/plain;charset=UTF-8'}});
    }
};
