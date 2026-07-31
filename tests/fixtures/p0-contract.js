function assertSetCookieHeaderSemantics() {
    const headers = new Headers([
        [ 'x-z', 'last' ],
        [ 'set-cookie', 'first=1; Path=/' ],
        [ 'x-a', 'first' ],
        [ 'set-cookie', 'second=2; Path=/' ],
    ]);
    const expectedEntries = [
        [ 'set-cookie', 'first=1; Path=/' ],
        [ 'set-cookie', 'second=2; Path=/' ],
        [ 'x-a', 'first' ],
        [ 'x-z', 'last' ],
    ];
    const forEachEntries = [];
    headers.forEach((value, name) => {
        forEachEntries.push([ name, value ]);
    });
    const checks = [
        [ [ ...headers ], expectedEntries ],
        [
            [ ...headers.keys() ],
            expectedEntries.map(([ name ]) => name),
        ],
        [
            [ ...headers.values() ],
            expectedEntries.map(([, value ]) => value),
        ],
        [ forEachEntries, expectedEntries ],
        [
            headers.getSetCookie(),
            [ 'first=1; Path=/', 'second=2; Path=/' ],
        ],
    ];
    for (const [ actual, expected ] of checks) {
        if (JSON.stringify(actual) !== JSON.stringify(expected)) {
            throw new Error(
                `Set-Cookie Headers iteration mismatch: ${JSON.stringify(actual)}`,
            );
        }
    }
    if (
        headers.get('set-cookie') !==
        'first=1; Path=/, second=2; Path=/'
    ) {
        throw new Error('Set-Cookie Headers get() mismatch');
    }
}

assertSetCookieHeaderSemantics();

function streamingResponse() {
    let produced = 0;
    const total = 96 * 1024 + 37;
    const body = new ReadableStream({
        type: 'bytes',
        pull(controller) {
            if (produced === total) {
                controller.close();
                return;
            }
            const size = Math.min(997, total - produced);
            const chunk = new Uint8Array(size);
            for (let index = 0; index < size; ++index) {
                chunk[index] = (produced + index) % 251;
            }
            produced += size;
            controller.enqueue(chunk);
        },
    });
    const headers = new Headers([
        [ 'content-type', 'application/octet-stream' ],
        [ 'set-cookie', 'first=1; Path=/' ],
        [ 'set-cookie', 'second=2; Path=/' ],
        [ 'x-capsid-contract', 'streaming' ],
    ]);
    return new Response(body, { status: 201, headers });
}

export default {
    async fetch(request) {
        const path = new URL(request.url).pathname;
        if (path === '/stream') {
            const body = new Uint8Array(await request.arrayBuffer());
            if (body.byteLength !== 80 * 1024 + 19) {
                return new Response('bad request size', { status: 400 });
            }
            for (let index = 0; index < body.byteLength; ++index) {
                if (body[index] !== index % 251) {
                    return new Response('bad request bytes', { status: 400 });
                }
            }
            return streamingResponse();
        }
        if (path === '/cancel') {
            await new Promise((resolve, reject) => {
                request.signal.addEventListener(
                    'abort',
                    () => reject(request.signal.reason),
                    { once: true },
                );
            });
        }
        if (path === '/cancel-body') {
            await request.arrayBuffer();
            return new Response('body completed');
        }
        if (path === '/backpressure') {
            return new Response(new Uint8Array(8192));
        }
        if (path === '/cancel-fetch') {
            const target = new URL(request.url).searchParams.get('target');
            await fetch(target, { signal: request.signal });
            return new Response('fetch unexpectedly completed');
        }
        if (path === '/timeout') {
            while (true) {
                // The native interrupt handler must terminate this execution.
            }
        }
        if (path === '/async-timeout') {
            await new Promise(() => {});
        }
        if (path === '/large-header') {
            return new Response('unreachable', {
                headers: {
                    'x-oversized': 'x'.repeat(4096),
                },
            });
        }
        if (path === '/large-chunk') {
            return new Response(new Uint8Array(8192));
        }
        if (path === '/formdata-content-type') {
            const clone = request.clone();
            try {
                await clone.formData();
                return new Response('formData accepted application/json', {
                    status: 500,
                });
            } catch (error) {
                return new Response(JSON.stringify({
                    name: error.name,
                    bodyUsed: clone.bodyUsed,
                    contentType: clone.headers.get('content-type'),
                }), {
                    headers: { 'content-type': 'application/json' },
                });
            }
        }
        return new Response('reused', {
            status: 200,
            headers: [
                [ 'set-cookie', 'reuse-a=1' ],
                [ 'set-cookie', 'reuse-b=2' ],
            ],
        });
    },
};
