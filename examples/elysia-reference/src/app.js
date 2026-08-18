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
        context => context.request.method,
    );
}
app.get('/routing/static', () => 'static');
app.get('/routing/users/:id', context => ({
    id: context.params.id,
}));
app.get('/routing/assets/*', context => context.path);
app.get('/routing/query', context => ({
    value: context.query.value ?? null,
}));

/* --- middleware chain --- */
app.onRequest(context => {
    context.set.headers['x-elysia-on-request'] = 'yes';
});
app.onBeforeHandle(context => {
    context.store = { ...(context.store ?? {}), before: 'ran' };
});
app.get('/middleware/before', context => context.store);
app.get('/middleware/headers', context => ({
    onRequest: context.set.headers['x-elysia-on-request'] ?? null,
}));

/* --- body handling --- */
app.post('/body/text', async context => ({
    length: (await context.request.text()).length,
}));
app.post('/body/json', async context => {
    const value = await context.request.json();
    return { name: value?.name ?? null };
});
app.post('/body/form', async context => {
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
app.get('/headers/echo', context => ({
    value: context.request.headers.get('x-elysia-send') ?? null,
}));
app.get('/cookie/read', context => context.cookie.flavor?.value ?? null);
app.get('/cookie/write', context => {
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
app.get('/response/status', context => {
    context.set.status = 202;
    return 'accepted';
});
app.get('/response/redirect', context => {
    context.set.redirect = '/entry';
    return 'redirecting';
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
app.get('/error/status', context => {
    context.set.status = 418;
    return 'teapot';
});

export { app };
