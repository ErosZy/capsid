import { Hono } from 'hono';
import { HTTPException } from 'hono/http-exception';
import { basicAuth } from 'hono/basic-auth';
import { bearerAuth } from 'hono/bearer-auth';
import { bodyLimit } from 'hono/body-limit';
import { compress } from 'hono/compress';
import { cors } from 'hono/cors';
import { etag } from 'hono/etag';
import { createFactory } from 'hono/factory';
import { getCookie, setCookie } from 'hono/cookie';
import { jwt, sign } from 'hono/jwt';
import { logger } from 'hono/logger';
import { requestId } from 'hono/request-id';
import { secureHeaders } from 'hono/secure-headers';
import { stream } from 'hono/streaming';
import { setMetric, timing } from 'hono/timing';

const textDecoder = new TextDecoder();

const toHex = value => {
    let output = '';
    for (const byte of new Uint8Array(value)) {
        output += byte.toString(16).padStart(2, '0');
    }
    return output;
};

const app = new Hono();

app.onError((error, context) => {
    if (error instanceof HTTPException) {
        return error.getResponse();
    }
    return context.json({
        name: error?.name ?? 'Error',
        message: error?.message ?? String(error),
    }, 555);
});

app.notFound(context => context.text(
    `not-found:${new URL(context.req.url).pathname}`,
    404,
));

app.get('/entry', context => context.text('hono-entry-ok'));

app.on(
    [ 'GET', 'POST', 'PUT', 'PATCH', 'DELETE', 'OPTIONS' ],
    '/routing/method',
    context => context.text(context.req.method),
);
app.get('/routing/static', context => context.text('static'));
app.get('/routing/users/:id', context => context.json({
    id: context.req.param('id'),
}));
app.get('/routing/assets/*', context => context.text(
    new URL(context.req.url).pathname,
));
app.all('/routing/all', context => context.text(`all:${context.req.method}`));

// Single-process benchmark routes (hono vs slim comparison):
// /bench/json  -> a fixed JSON document
// /bench/bytes -> 1024 bytes of binary payload
// /bench/stream -> a streamed response (3 chunks)
app.get('/bench/json', context => context.json({
    status: 'ok',
    app: 'hono',
    item: 'benchmark',
    value: 42,
}));
app.get('/bench/bytes', context => new Response(
    new Uint8Array(1024).fill(0x61),
    { headers: { 'content-type': 'application/octet-stream' } },
));
app.get('/bench/stream', context => new Response(
    new ReadableStream({
        type: 'bytes',
        pull(controller) {
            controller.enqueue(new Uint8Array(341).fill(0x62));
            controller.enqueue(new Uint8Array(341).fill(0x63));
            controller.enqueue(new Uint8Array(342).fill(0x64));
            controller.close();
        },
    }),
    { headers: { 'content-type': 'application/octet-stream' } },
));

const based = new Hono().basePath('/routing/base');
based.get('/item/:id', context => context.text(
    `base:${context.req.param('id')}`,
));
app.route('/', based);

const child = new Hono();
child.get('/item/:id', context => context.text(
    `route:${context.req.param('id')}`,
));
app.route('/routing/child', child);

app.mount('/routing/mount', request => new Response(
    `mount:${new URL(request.url).pathname}`,
    { headers: { 'content-type': 'text/plain; charset=UTF-8' } },
));

app.get('/request/query', context => context.json({
    first: context.req.query('tag'),
    all: context.req.queries('tag'),
    encoded: context.req.query('encoded'),
}));
app.get('/request/headers', context => context.json({
    one: context.req.header('x-one'),
    repeated: context.req.header('x-repeated'),
}));
app.post('/request/json', async context => context.json({
    parsed: await context.req.json(),
}));
app.post('/request/text', async context => context.text(
    `text:${await context.req.text()}`,
));
app.post('/request/urlencoded', async context => {
    const form = await context.req.parseBody({ all: true });
    return context.json({
        alpha: form.alpha,
        repeated: form.repeated,
    });
});
app.post('/request/multipart', async context => {
    const form = await context.req.formData();
    const file = form.get('upload');
    return context.json({
        field: form.get('field'),
        repeated: form.getAll('repeated'),
        file: file instanceof File ? {
            name: file.name,
            type: file.type,
            size: file.size,
            text: await file.text(),
        } : null,
    });
});
app.post('/request/array-buffer', async context => {
    const body = await context.req.arrayBuffer();
    return context.json({ byteLength: body.byteLength, hex: toHex(body) });
});
app.post('/request/blob', async context => {
    const blob = await context.req.blob();
    return context.json({
        size: blob.size,
        type: blob.type,
        text: await blob.text(),
    });
});
app.post('/request/empty', async context => context.json({
    text: await context.req.text(),
}));
app.post('/request/stream', async context => {
    const reader = context.req.raw.body.getReader();
    let chunks = 0;
    let size = 0;
    let checksum = 0;
    for (;;) {
        const result = await reader.read();
        if (result.done) {
            break;
        }
        chunks += 1;
        size += result.value.byteLength;
        for (const byte of result.value) {
            checksum = (checksum + byte) >>> 0;
        }
    }
    return context.json({ chunks, size, checksum });
});

app.get('/response/text', context => context.text('plain-text'));
app.get('/response/json', context => context.json({
    framework: 'hono',
    runtime: 'capsid',
}));
app.get('/response/html', context => context.html(
    '<!doctype html><h1>Capsid Hono</h1>',
));
app.get('/response/binary', context => context.body(
    new Uint8Array([ 0, 1, 2, 127, 128, 255 ]),
    200,
    { 'content-type': 'application/octet-stream' },
));
app.post('/response/status', context => context.text('created', 201));
app.get('/response/redirect', context => context.redirect('/response/text', 307));
app.get('/response/headers', context => {
    context.header('x-capsid', 'one');
    context.header('x-repeat', 'first', { append: true });
    context.header('x-repeat', 'second', { append: true });
    return context.text('headers');
});
app.get('/response/cookies', context => {
    setCookie(context, 'first', 'one', {
        httpOnly: true,
        path: '/',
        sameSite: 'Lax',
    });
    setCookie(context, 'second', 'two', {
        maxAge: 60,
        path: '/cookies',
    });
    return context.text('cookies');
});
app.get('/response/stream', context => stream(context, async output => {
    for (let index = 0; index < 16; ++index) {
        await output.write(new Uint8Array(256).fill(65 + index));
    }
}));
app.get('/response/no-content', context => context.body(null, 204));
app.get('/response/not-modified', context => context.body(null, 304, {
    etag: '"capsid"',
}));
app.get('/response/head', context => context.text('head-body', 200, {
    'x-head': 'present',
}));

app.use('/middleware/order', async (context, next) => {
    context.set('trace', [ 'outer:before' ]);
    await next();
    context.get('trace').push('outer:after');
    context.res.headers.set('x-order', context.get('trace').join(','));
});
app.use('/middleware/order', async (context, next) => {
    context.get('trace').push('inner:before');
    await next();
    context.get('trace').push('inner:after');
});
app.get('/middleware/order', context => {
    context.get('trace').push('handler');
    return context.json({ during: [ ...context.get('trace') ] });
});
app.use('/middleware/early', context => context.text('early', 202));
app.get('/middleware/early', context => context.text('unreachable'));
app.use('/middleware/scoped/*', async (context, next) => {
    context.set('scope', 'applied');
    await next();
});
app.get('/middleware/scoped/inside', context => context.text(
    context.get('scope') ?? 'missing',
));
app.get('/middleware/outside', context => context.text(
    context.get('scope') ?? 'absent',
));
app.get('/middleware/context', context => {
    const before = context.get('request-value') ?? null;
    const value = context.req.query('value') ?? 'unset';
    context.set('request-value', value);
    return context.json({ before, after: context.get('request-value') });
});
app.use('/middleware/mutate', async (context, next) => {
    await next();
    const original = await context.res.text();
    context.res = new Response(`${original}:mutated`, context.res);
    context.res.headers.set('x-mutated', 'true');
});
app.get('/middleware/mutate', context => context.text('original'));
app.get('/middleware/error-sync', () => {
    throw new TypeError('sync-boom');
});
app.get('/middleware/error-async', async () => {
    await Promise.resolve();
    throw new RangeError('async-boom');
});

app.use('/builtin/cors', cors({
    origin: 'https://client.example',
    allowMethods: [ 'GET', 'POST' ],
    exposeHeaders: [ 'x-capsid' ],
    maxAge: 600,
}));
app.get('/builtin/cors', context => context.text('cors'));

app.use('/builtin/secure-headers', secureHeaders());
app.get('/builtin/secure-headers', context => context.text('secure'));

app.use('/builtin/body-limit', bodyLimit({ maxSize: 8 }));
app.post('/builtin/body-limit', async context => context.text(
    `accepted:${await context.req.text()}`,
));

app.use('/builtin/basic-auth', basicAuth({
    username: 'capsid',
    password: 'hono',
}));
app.get('/builtin/basic-auth', context => context.text('basic-ok'));

app.use('/builtin/bearer-auth', bearerAuth({ token: 'capsid-token' }));
app.get('/builtin/bearer-auth', context => context.text('bearer-ok'));

app.use('/builtin/jwt', jwt({
    secret: 'capsid-secret',
    alg: 'HS256',
}));
app.get('/builtin/jwt', context => context.json({
    payload: context.get('jwtPayload'),
}));

app.use('/builtin/request-id', requestId({
    generator: () => 'capsid-generated-id',
}));
app.get('/builtin/request-id', context => context.text(
    context.get('requestId'),
));

app.use('/builtin/etag', etag());
app.get('/builtin/etag', context => context.text('etag-content'));

app.use('/builtin/compress', compress({
    encoding: 'deflate',
    threshold: 1,
}));
app.get('/builtin/compress', context => context.text(
    'compressible-capsid-hono-content',
));

const loggerEvents = [];
app.use('/builtin/logger', logger(message => loggerEvents.push(message)));
app.get('/builtin/logger', context => context.json({
    incoming: loggerEvents.length,
}));

app.use('/builtin/timing', timing({ crossOrigin: '*' }));
app.get('/builtin/timing', context => {
    setMetric(context, 'fixed', 12.5, 'Fixed Metric', 1);
    return context.text('timing');
});

app.get('/builtin/cookie', context => {
    setCookie(context, 'seen', getCookie(context, 'input') ?? 'missing', {
        path: '/',
    });
    return context.text(getCookie(context, 'input') ?? 'missing');
});

app.get('/builtin/streaming', context => stream(context, async output => {
    await output.write(textDecoder.decode(new Uint8Array([
        115, 116, 114, 101, 97, 109, 45,
    ])));
    await output.write('helper');
}));

const factory = createFactory();
const factoryMiddleware = factory.createMiddleware(async (context, next) => {
    context.set('factory', 'factory-value');
    await next();
});
app.get(
    '/builtin/factory',
    factoryMiddleware,
    ...factory.createHandlers(context => context.text(context.get('factory'))),
);

app.get('/runtime/globals', context => {
    const names = new Set(Object.getOwnPropertyNames(globalThis));
    const state = name => names.has(name) ? 'present' : 'undefined';
    return context.json({
        process: state('process'),
        Buffer: state('Buffer'),
        Deno: state('Deno'),
        Bun: state('Bun'),
        caches: state('caches'),
        tjs: state('tjs'),
    });
});

app.get('/runtime/delay', async context => {
    const delay = Number(context.req.query('ms') ?? '0');
    await new Promise(resolve => setTimeout(resolve, delay));
    return context.text(`delay:${delay}`);
});

let abortedHandlers = 0;
let abortedStreams = 0;

app.get('/runtime/wait-for-abort', context => new Promise(resolve => {
    context.req.raw.signal.addEventListener('abort', () => {
        abortedHandlers += 1;
        resolve(context.text('aborted'));
    }, { once: true });
}));
app.get('/runtime/abort-count', context => context.json({
    handlers: abortedHandlers,
    streams: abortedStreams,
}));
app.get('/runtime/stream-cancel', context => stream(context, async output => {
    output.onAbort(() => {
        abortedStreams += 1;
    });
    for (let index = 0; index < 128 && !output.aborted; ++index) {
        await output.write(new Uint8Array(256).fill(65 + (index % 26)));
        await output.sleep(1);
    }
}));

app.use('/runtime/concurrent', async (context, next) => {
    const token = context.req.query('token');
    context.set('concurrent-token', token);
    await next();
    context.res.headers.set(
        'x-context-token',
        context.get('concurrent-token'),
    );
});
app.get('/runtime/concurrent', async context => {
    const delay = Number(context.req.query('delay') ?? '0');
    await new Promise(resolve => setTimeout(resolve, delay));
    return context.json({
        token: context.get('concurrent-token'),
        delay,
    });
});

app.use('/runtime/middleware-timeout', async (context, next) => {
    const delay = Number(context.req.query('ms') ?? '0');
    await new Promise(resolve => setTimeout(resolve, delay));
    await next();
});
app.get('/runtime/middleware-timeout', context => context.text('too-late'));

app.get('/runtime/cpu-timeout', () => {
    for (;;) {
        // Deliberately exercise the runtime interrupt deadline.
    }
});

app.get('/runtime/fetch', async context => {
    const target = context.req.query('url');
    const response = await fetch(target);
    return context.json({
        status: response.status,
        header: response.headers.get('x-hono-upstream'),
        body: await response.text(),
    });
});

export { app };
export const createReferenceJwt = () => sign(
    { sub: 'capsid-user', role: 'tester' },
    'capsid-secret',
    'HS256',
);
export default app;
