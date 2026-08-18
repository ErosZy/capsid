import { Elysia } from 'elysia';

const textDecoder = new TextDecoder();
const textEncoder = new TextEncoder();

const app = new Elysia();

/*
 * Entry / fixed / lifecycle primitives, mirroring the hono reference app so
 * the differential vectors stay comparable across frameworks.
 */
app.get('/entry', () => 'elysia-entry-ok');
app.get('/fixed', () => new Response(
    new Uint8Array(1024).fill(0x78),
    { headers: { 'content-type': 'application/octet-stream' } },
));

/* --- routing: methods, params, wildcard, query --- */
for (const method of [ 'GET', 'POST', 'PUT', 'PATCH', 'DELETE', 'OPTIONS' ]) {
    app[method.toLowerCase()](
        '/routing/method',
        (context) => context.request.method,
    );
}
app.get('/routing/static', () => 'static');
app.get('/routing/users/:id', (context) => ({
    id: context.params.id,
}));
app.get('/routing/assets/*', (context) => context.path);
app.get('/routing/query', (context) => ({
    value: context.query.value ?? null,
}));

/* --- middleware chain --- */
app.onRequest((context) => {
    context.set.headers['x-elysia-on-request'] = 'yes';
});
app.onBeforeHandle((context) => {
    context.store = { ...(context.store ?? {}), before: 'ran' };
});
app.get('/middleware/before', (context) => context.store);
app.get('/middleware/headers', (context) => ({
    onRequest: context.set.headers['x-elysia-on-request'] ?? null,
}));

/* --- body handling --- */
app.post('/body/text', async (context) => ({
    length: (await context.request.text()).length,
}));
app.post('/body/json', async (context) => {
    const value = await context.request.json();
    return { name: value?.name ?? null };
});
app.post('/body/form', async (context) => {
    const form = await context.request.formData();
    return {
        field: form.get('field'),
        files: form.getAll('upload').map(file => ({
            name: file.name,
            size: file.size,
        })),
    };
});

/* --- headers / cookies --- */
app.get('/headers/echo', (context) => ({
    value: context.request.headers.get('x-elysia-send') ?? null,
}));
app.get('/cookie/read', (context) => context.cookie.flavor?.value ?? null);
app.get('/cookie/write', (context) => {
    context.cookie.session.set({
        value: 'abc123',
        httpOnly: true,
        path: '/',
    });
    return 'cookie-set';
});

/* --- responses --- */
app.get('/response/json-auto', () => ({ status: 'ok', app: 'elysia' }));
app.get('/response/binary', () => new Uint8Array(256).fill(0x65));
app.get('/response/status', (context) => {
    context.set.status = 202;
    return 'accepted';
});
// Redirect boundary: Elysia 1.4.29's web-standard adapter never converts
// set.redirect into a Location header (and has no app.redirect API). The
// route pins the ACTUAL behavior — 200 with the body — so reference and
// runtime stay aligned; the differential must not invent a redirect.
app.get('/response/redirect', (context) => {
    context.set.redirect = '/entry';
    return new Response('redirecting');
});

/* --- streaming: 1.4 responds to iterable returns by teeing them ---
 * Chunks are yielded as Uint8Array: the teed body stream is byte-based,
 * and string chunks would make the Fetch API reject the body on read.
 */
app.get('/stream/one', async function* () {
    yield textEncoder.encode('stream-one');
});
app.get('/stream/chunks', async function* () {
    yield textEncoder.encode('a');
    yield textEncoder.encode('b');
    yield textEncoder.encode('c');
});

/* --- errors --- */
app.onError(({ code, set, error }) => {
    if (code === 'NOT_FOUND') {
        return new Response(`elysia-not-found:${error?.message ?? ''}`, {
            status: 404,
        });
    }
    return new Response(JSON.stringify({
        code,
        name: error?.name ?? 'Error',
        message: error?.message ?? String(error),
    }), {
        status: 500,
        headers: { 'content-type': 'application/json' },
    });
});
app.get('/error/throw', () => {
    throw new Error('elysia-exploded');
});
app.get('/error/status', (context) => {
    context.set.status = 418;
    return 'teapot';
});

/* --- runtime probes: identical contracts to the hono reference app --- */
app.get('/runtime/globals', () => {
    const names = new Set(Object.getOwnPropertyNames(globalThis));
    const state = name => names.has(name) ? 'present' : 'undefined';
    return {
        process: state('process'),
        Buffer: state('Buffer'),
        Deno: state('Deno'),
        Bun: state('Bun'),
        caches: state('caches'),
        tjs: state('tjs'),
    };
});

// Abort-aware delay (same contract as the hono fixture): on hard timeout
// the worker fires the request abort, so the timer must be cleared and the
// route promise settled from the abort listener — otherwise the timer
// continuation leaks and the reclaim poisons the worker. Handlers must be
// async functions: Elysia only awaits promises returned from async handlers.
app.get('/runtime/delay', async (context) => {
    const url = new URL(context.request.url);
    const delay = Number(url.searchParams.get('ms') ?? '0');
    const signal = context.request.signal;
    return new Promise((resolve, reject) => {
        const timer = setTimeout(() => {
            signal.removeEventListener('abort', onAbort);
            resolve(new Response(`delay:${delay}`));
        }, delay);
        const onAbort = () => {
            clearTimeout(timer);
            signal.removeEventListener('abort', onAbort);
            reject(signal.reason ?? new Error('request aborted'));
        };
        if (signal.aborted) onAbort();
        else signal.addEventListener('abort', onAbort, { once: true });
    });
});

let abortedHandlers = 0;
let abortedStreams = 0;

app.get('/runtime/wait-for-abort', async (context) => {
    await new Promise(resolve => {
        context.request.signal.addEventListener('abort', () => {
            abortedHandlers += 1;
            resolve();
        }, { once: true });
    });
    return new Response('aborted');
});
app.get('/runtime/abort-count', () => ({
    handlers: abortedHandlers,
    streams: abortedStreams,
}));
// Streaming body whose cancel is counted exactly once, whether the request
// abort or the body-stream cancel fires first.
app.get('/runtime/stream-cancel', (context) => {
    let done = false;
    let chunk = 0;
    const countOnce = () => {
        if (!done) {
            done = true;
            abortedStreams += 1;
        }
    };
    context.request.signal.addEventListener('abort', countOnce, { once: true });
    const body = new ReadableStream({
        async pull(controller) {
            if (done || chunk >= 128) {
                controller.close();
                return;
            }
            controller.enqueue(new Uint8Array(256).fill(65 + (chunk % 26)));
            chunk += 1;
            await new Promise(resolve => setTimeout(resolve, 1));
        },
        cancel: countOnce,
    });
    return new Response(body);
});

app.get('/runtime/concurrent', async (context) => {
    const url = new URL(context.request.url);
    const token = url.searchParams.get('token');
    const delay = Number(url.searchParams.get('delay') ?? '0');
    await new Promise(resolve => setTimeout(resolve, delay));
    return { token, delay };
});

// Abort-aware beforeHandle scoped to the timeout route only; async so Elysia
// awaits its promise.
app.onBeforeHandle(async (context) => {
    const url = new URL(context.request.url);
    if (url.pathname !== '/runtime/middleware-timeout') {
        return;
    }
    const delay = Number(url.searchParams.get('ms') ?? '0');
    const signal = context.request.signal;
    return new Promise((resolve, reject) => {
        const timer = setTimeout(() => {
            signal.removeEventListener('abort', onAbort);
            resolve();
        }, delay);
        const onAbort = () => {
            clearTimeout(timer);
            signal.removeEventListener('abort', onAbort);
            reject(signal.reason ?? new Error('request aborted'));
        };
        if (signal.aborted) onAbort();
        else signal.addEventListener('abort', onAbort, { once: true });
    });
});
app.get('/runtime/middleware-timeout', () => 'too-late');

app.get('/runtime/cpu-timeout', () => {
    for (;;) {
        // Deliberately exercise the runtime interrupt deadline.
    }
});

app.get('/runtime/ownership', async () => {
    console.log('capsid-owner:before');
    await Promise.resolve();
    console.log('capsid-owner:after');
    return 'ownership-ok';
});

app.get('/runtime/ownership-cancel', async () => {
    console.log('capsid-owner:start');
    setTimeout(() => console.log('capsid-owner:after-cancel'), 80);
    await new Promise(() => {});
});

app.get('/runtime/fetch', async (context) => {
    const url = new URL(context.request.url);
    const target = url.searchParams.get('url');
    const response = await fetch(target);
    return {
        status: response.status,
        text: await response.text(),
        header: response.headers.get('x-elysia-upstream'),
    };
});

export { app };
