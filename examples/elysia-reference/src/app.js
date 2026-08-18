import { Elysia, t } from 'elysia';
import { bearer } from '@elysiajs/bearer';
import { cors } from '@elysiajs/cors';
import { jwt } from '@elysiajs/jwt';
import { Stream } from '@elysiajs/stream';

const textDecoder = new TextDecoder();
const textEncoder = new TextEncoder();

// Elysia 1.4's route inference (sucrose) schedules a ~5-minute module-global
// GC timer on cache misses and reschedules it on every inference. Under
// capsid's async-context hooks a timer created inside a request captures that
// request's token, so the last inference's timer holds the final token open
// and the worker is poisoned as a terminal continuation leak. gcTime: null
// makes clearSucroseCache skip the timer entirely (the cache still flushes on
// the next inference; only the lazy GC is disabled).
const app = new Elysia({ sucrose: { gcTime: null } });

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
// Multipart parsing lives in a module-scope helper, not the route handler.
// Elysia 1.4's sucrose inference scans handler.toString() with
// /\w\((?:.*?)?<param>(?:.*?)?\)/ and reads a `<param>,`/`<param>)` hit as
// "context passed to a function", which enables body pre-parse. Under esbuild
// minification the handler param becomes `e`, and a closure like
// `map(r => ({ name: r.name, size: r.size }))` puts an `e` inside parens
// right before `)` — the exactParameter regex fires, body:true is inferred,
// compose pre-parses the multipart body, and the handler's own formData()
// then throws "Already read" (Node reproduces the 500 on the minified
// bundle; it is not capsid-specific). Keeping the parsing in a helper leaves
// the handler as `e => parseForm(e.request)`, which no longer matches, so
// the multipart body is read exactly once.
const parseForm = async (request) => {
    const form = await request.formData();
    const files = [];
    for (const file of form.getAll('upload')) {
        files.push({ name: file.name, size: file.size });
    }
    return { field: form.get('field'), files };
};
app.post('/body/form', (context) => parseForm(context.request));

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
    // Schema validation failures must keep their 422 status instead of
    // falling through to the generic 500 handler below.
    if (code === 'VALIDATION') {
        return new Response(JSON.stringify({
            code,
            message: error?.message ?? String(error),
        }), {
            status: 422,
            headers: { 'content-type': 'application/json' },
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

/* ================= @elysiajs plugin coverage ================= */

/*
 * CORS — plugin hooks onRequest to answer preflight and stamp
 * access-control-* headers. The origin is pinned so only matching
 * origins get allow-origin back (origin: true would echo anything).
 */
app.use(cors({
    origin: 'https://capsid.test',
    methods: [ 'GET', 'POST' ],
    allowedHeaders: [ 'x-elysia-send', 'content-type' ],
}));
app.get('/cors/echo', () => 'cors-ok');

/*
 * Bearer — plugin derives a lazy `bearer` getter (query access_token or
 * Authorization header). Header value is stripped of the scheme prefix.
 *
 * The route declares an optional access_token query schema. Without it,
 * AOT compose skips query parsing entirely (hasQuery is inference-driven)
 * and the derive's query access would read undefined — the dynamic-handle
 * path always parses query, so Node would not catch the difference.
 */
app.use(bearer());
app.get('/bearer/echo', (context) => ({
    bearer: context.bearer ?? null,
}), {
    query: t.Object({
        access_token: t.Optional(t.String()),
    }),
});

/*
 * JWT — plugin decorates `context.jwt` with sign/verify backed by jose's
 * webapi entry (crypto.subtle only, no Node primitives). The payload is
 * echoed back on verify so differential can pin both directions.
 */
app.use(jwt({
    name: 'jwt',
    secret: 'capsid-elysia-jwt-secret',
}));
app.get('/jwt/sign', async (context) => ({
    token: await context.jwt.sign({ sub: 'capsid-test-user' }),
}));
app.get('/jwt/verify', async (context) => {
    const header = context.request.headers.get('authorization') ?? '';
    const token = header.startsWith('Bearer ') ? header.slice(7) : header;
    const payload = await context.jwt.verify(token);
    return { valid: payload !== false, payload: payload === false ? null : payload };
});

/*
 * SSE — the plugin's Stream class formats id/data frames and the response
 * is served with an explicit text/event-stream content-type. Stream.send
 * assigns a random nanoid id per frame, so differentials assert the data
 * lines only. (Stream is deprecated in favor of generator returns; both
 * shapes are covered — generator route below.)
 */
app.get('/stream/sse', (context) => {
    const stream = new Stream((stream) => {
        stream.send('sse-one');
        stream.send({ count: 2 });
        stream.close();
    });
    return new Response(stream.toResponse(), {
        headers: { 'content-type': 'text/event-stream' },
    });
});
app.get('/stream/sse-generator', async function* (context) {
    context.set.headers['content-type'] = 'text/event-stream';
    yield textEncoder.encode('data: gen-one\n\n');
    yield textEncoder.encode('data: gen-two\n\n');
});

/* ================= core capabilities ================= */

/*
 * guard — scoped hooks: routes registered through the guard inherit its
 * beforeHandle; a returned Response short-circuits the chain.
 */
app.guard({
    beforeHandle: (context) => {
        if (context.headers['x-guard-pass'] !== '1') {
            return new Response('guard-denied', { status: 403 });
        }
    },
// The guard callback must return the scoped instance (expression body),
// not a bare block — Elysia merges the callback's return value back.
}, (guard) => guard.get('/guard/protected', () => 'guard-ok'));

/* derive — synchronous per-request augmentation injected into context. */
app.derive((context) => ({
    derivedFrom: context.request.headers.get('x-derived') ?? 'none',
}));
app.get('/derive/echo', (context) => ({
    derivedFrom: context.derivedFrom,
}));

/* resolve — async per-request augmentation (awaited before the handler). */
app.resolve(async (context) => ({
    resolvedPath: new URL(context.request.url).pathname,
}));
app.get('/resolve/echo', (context) => ({
    resolvedPath: context.resolvedPath,
}));

/*
 * schema validation — typebox body/params contracts; failures surface as
 * VALIDATION with 422 via the onError branch above. Note the 1.4 signature
 * is route(path, handler, hook): the schema is the THIRD argument — the
 * second position is the handler itself, so a schema there gets treated as
 * a plain object return value.
 */
app.post('/schema/body', (context) => ({
    name: context.body.name,
    age: context.body.age ?? null,
}), {
    body: t.Object({
        name: t.String(),
        age: t.Optional(t.Number()),
    }),
});
app.get('/schema/params/:id', (context) => ({ id: context.params.id }), {
    params: t.Object({ id: t.String() }),
});

export { app };
