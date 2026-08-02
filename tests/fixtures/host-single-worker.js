function patternedBytes(size) {
    const bytes = new Uint8Array(size);
    for (let index = 0; index < bytes.byteLength; ++index) {
        bytes[index] = index % 251;
    }
    return bytes;
}

// Cancel observations, exposed through /status so the integration test can
// prove that a client disconnect cancelled the worker request before the
// worker's own request deadline could fire.
const cancelObservations = [];

export default {
    async fetch(request) {
        const url = new URL(request.url);
        if (url.pathname === '/inspect') {
            return Response.json({
                url: request.url,
                host: request.headers.get('host'),
                forwarded: request.headers.get('forwarded'),
                forwardedProto: request.headers.get('x-forwarded-proto'),
                capsidApp: request.headers.get('capsid-app'),
                trace: request.headers.get('x-trace'),
            });
        }
        if (url.pathname === '/headers') {
            // Returns the exact worker-observable input (method, URL,
            // headers) so the benchmark runner can assert both sides deliver
            // identical requests.
            const observed = {};
            for (const [name, value] of request.headers) {
                const lower = name.toLowerCase();
                if (!observed[lower]) {
                    observed[lower] = value;
                }
            }
            return Response.json({
                method: request.method,
                url: request.url,
                headers: observed,
            });
        }
        if (url.pathname === '/fixed') {
            return new Response(new Uint8Array(1024).fill(0x78), {
                headers: { 'content-type': 'application/octet-stream' },
            });
        }
        if (url.pathname === '/echo') {
            const body = new Uint8Array(await request.arrayBuffer());
            return new Response(body, {
                status: 201,
                headers: [
                    [ 'content-type', 'application/octet-stream' ],
                    [ 'set-cookie', 'first=1; Path=/' ],
                    [ 'set-cookie', 'second=2; Path=/' ],
                ],
            });
        }
        if (url.pathname === '/stream') {
            const bytes = patternedBytes(96 * 1024 + 37);
            let offset = 0;
            return new Response(new ReadableStream({
                type: 'bytes',
                pull(controller) {
                    if (offset === bytes.byteLength) {
                        controller.close();
                        return;
                    }
                    const end = Math.min(offset + 997, bytes.byteLength);
                    controller.enqueue(bytes.slice(offset, end));
                    offset = end;
                },
            }), {
                headers: { 'content-type': 'application/octet-stream' },
            });
        }
        if (url.pathname === '/cancel') {
            // Streams body chunks forever so the Host always has a write in
            // flight: a client that resets the connection mid-stream then
            // surfaces immediately as a write error, which the Host maps to
            // cancel. The abort observation is recorded and exposed through
            // /status.
            const startedAt = Date.now();
            let cancelled = false;
            request.signal.addEventListener(
                'abort',
                () => {
                    cancelled = true;
                    cancelObservations.push({
                        startedAt,
                        delayMs: Date.now() - startedAt,
                    });
                },
                { once: true },
            );
            return new Response(new ReadableStream({
                type: 'bytes',
                pull(controller) {
                    if (cancelled) {
                        controller.close();
                        return;
                    }
                    controller.enqueue(patternedBytes(997));
                },
            }), {
                headers: { 'content-type': 'application/octet-stream' },
            });
        }
        if (url.pathname === '/conn-before') {
            // The nominated field appears before the Connection header that
            // names it; the Host's filter must not depend on field order.
            return new Response('ok', {
                headers: [
                    [ 'a-nominated', 'must-not-leak' ],
                    [ 'connection', 'a-nominated' ],
                ],
            });
        }
        if (url.pathname === '/conn-after') {
            // The nominated field appears after the Connection header.
            return new Response('ok', {
                headers: [
                    [ 'connection', 'a-nominated' ],
                    [ 'a-nominated', 'must-not-leak' ],
                ],
            });
        }
        if (url.pathname === '/bad-cl') {
            // Declares a Content-Length smaller than the actual body. The
            // Host's response gate must fail the connection closed instead
            // of silently truncating the body (design §8.3).
            return new Response(new Uint8Array(1024).fill(0x78), {
                headers: { 'content-length': '5' },
            });
        }
        if (url.pathname === '/cpu') {
            // Real CPU/template workload for the M1B benchmark: builds a
            // deterministic HTML fragment so the worker does non-trivial
            // string work instead of echoing a fixed buffer.
            const items = [];
            for (let index = 0; index < 200; index++) {
                items.push(`<li id="item-${index}" class="${index % 2 ? 'odd' : 'even'}">item ${index}</li>`);
            }
            const html = `<!doctype html><html><body><ul>${items.join('')}</ul></body></html>`;
            return new Response(html, {
                headers: { 'content-type': 'text/html' },
            });
        }
        if (url.pathname === '/slow') {
            await new Promise((resolve) => setTimeout(resolve, 300));
            return new Response('slow done');
        }
        if (url.pathname === '/status') {
            const last = cancelObservations.length
                ? cancelObservations[cancelObservations.length - 1]
                : null;
            return Response.json({
                cancelCount: cancelObservations.length,
                lastCancelDelayMs: last ? last.delayMs : null,
            });
        }
        if (url.pathname === '/timeout') {
            while (true) {
                // Runtime's synchronous interrupt deadline must stop this.
            }
        }
        return new Response('not found', { status: 404 });
    },
};
