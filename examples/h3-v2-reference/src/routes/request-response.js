import {
    HTTPError,
    assertBodySize,
    assertMethod,
    getQuery,
    getRouterParams,
    getValidatedQuery,
    getValidatedRouterParams,
    html,
    noContent,
    raw,
    readBody,
    readValidatedBody,
    redirect,
    requireContentType,
    setCookie,
} from 'h3/generic';
import {
    bodyChecksum,
    formDataObject,
    requiredFieldSchema,
    toHex,
} from '../shared.js';

export const installRequestResponseRoutes = app => {
    app.get('/request/query', event => ({
        query: getQuery(event),
        prototypeIsNull: Object.getPrototypeOf(getQuery(event)) === null,
    }));
    app.get('/request/params/:first/:second', event => ({
        raw: getRouterParams(event),
        decoded: getRouterParams(event, { decode: true }),
    }));
    app.post('/request/json-native', async event => ({
        value: await event.req.json(),
    }));
    app.post('/request/json', async event => ({
        value: await readBody(event),
    }));
    app.post('/request/text', async event => ({
        value: await readBody(event, { type: 'text' }),
    }));
    app.post('/request/urlencoded', async event => ({
        value: await readBody(event, { type: 'urlencoded' }),
    }));
    app.post('/request/form-data', async event => ({
        value: await formDataObject(await event.req.formData()),
    }));
    app.post('/request/form-data-read-body', async event => {
        const value = await readBody(event, { type: 'formData' });
        const normalized = Object.create(null);
        for (const [ key, entry ] of Object.entries(value ?? {})) {
            const values = Array.isArray(entry) ? entry : [ entry ];
            normalized[key] = await Promise.all(values.map(async item =>
                item instanceof File ? {
                    name: item.name,
                    type: item.type,
                    size: item.size,
                    text: await item.text(),
                } : item));
            if (!Array.isArray(entry)) {
                normalized[key] = normalized[key][0];
            }
        }
        return { value: normalized };
    });
    app.post('/request/empty', async event => ({
        rawText: await event.req.text(),
        readBody: undefined,
    }));
    app.post('/request/array-buffer', async event => {
        const value = await event.req.arrayBuffer();
        return {
            byteLength: value.byteLength,
            hex: toHex(value),
        };
    });
    app.post('/request/stream', async event => {
        const reader = event.req.body?.getReader();
        let chunks = 0;
        let size = 0;
        let checksum = 0;
        if (reader) {
            for (;;) {
                const result = await reader.read();
                if (result.done) {
                    break;
                }
                chunks += 1;
                size += result.value.byteLength;
                checksum = (
                    checksum + bodyChecksum(result.value)
                ) >>> 0;
            }
        }
        return { chunks, size, checksum };
    });
    app.post('/request/validated-body', async event => ({
        value: await readValidatedBody(
            event,
            requiredFieldSchema('name'),
        ),
    }));
    app.get('/request/validated-query', async event => ({
        value: await getValidatedQuery(
            event,
            requiredFieldSchema('name'),
        ),
    }));
    app.get('/request/validated-params/:name', async event => ({
        value: await getValidatedRouterParams(
            event,
            requiredFieldSchema('name'),
            { decode: true },
        ),
    }));
    app.post('/request/body-size', async event => {
        assertBodySize(event, 16);
        return {
            value: await event.req.text(),
        };
    });
    app.post('/request/content-type', async event => ({
        matched: requireContentType(event, [
            'application/json',
            'application/*+json',
        ]),
        value: await event.req.text(),
    }));
    app.all('/request/assert-method', event => {
        assertMethod(event, [ 'POST', 'PUT' ]);
        return `method:${event.req.method}`;
    });

    app.get('/response/object', () => ({
        kind: 'object',
        nested: { ok: true },
    }));
    app.get('/response/array', () => [ 'a', 2, false ]);
    app.get('/response/number', () => 42);
    app.get('/response/boolean', () => false);
    app.get('/response/null', () => null);
    app.get('/response/undefined', () => undefined);
    app.get('/response/string', () => 'plain-string');
    app.get('/response/empty-string', () => '');
    app.get('/response/html', () =>
        html`<h1>${'<Capsid & H3>'}</h1>`);
    app.get('/response/raw-html', () =>
        html(raw('<strong>trusted</strong>')));
    app.get('/response/response', () => new Response('custom-response', {
        status: 201,
        statusText: 'Created by H3',
        headers: { 'x-custom-response': 'yes' },
    }));
    app.get('/response/array-buffer', () =>
        new Uint8Array([ 0, 1, 2, 127, 128, 255 ]).buffer);
    app.get('/response/uint8-array', () =>
        new Uint8Array([ 255, 128, 127, 2, 1, 0 ]));
    app.get('/response/blob', () => new Blob(
        [ 'blob-body' ],
        { type: 'application/x-h3-blob' },
    ));
    app.get('/response/file', () => new File(
        [ 'file-body' ],
        'capsid h3.txt',
        { type: 'text/x-h3-file' },
    ));
    app.get('/response/bigint', () => 9007199254740993n);
    app.get('/response/promise', () =>
        Promise.resolve({ promised: true }));
    app.get('/response/stream', () => new ReadableStream({
        start(controller) {
            controller.enqueue(new TextEncoder().encode('stream:one|'));
            controller.enqueue(new TextEncoder().encode('stream:two'));
            controller.close();
        },
    }));
    app.get('/response/large-stream', event => {
        const size = Number(event.url.searchParams.get('size') ?? 262144);
        let emitted = 0;
        return new ReadableStream({
            pull(controller) {
                if (emitted >= size) {
                    controller.close();
                    return;
                }
                const chunkSize = Math.min(4093, size - emitted);
                const chunk = new Uint8Array(chunkSize);
                for (let index = 0; index < chunk.length; ++index) {
                    chunk[index] = (emitted + index) % 251;
                }
                emitted += chunk.length;
                controller.enqueue(chunk);
            },
        });
    });
    app.get('/response/serialization-error', () => {
        const circular = {};
        circular.self = circular;
        return circular;
    });
    app.get('/response/head-fallback', () => 'head-body');
    app.get('/response/no-content', () => noContent());
    app.get('/response/not-modified', () =>
        new Response(null, {
            status: 304,
            statusText: 'Not Modified',
            headers: { etag: '"h3-v2"' },
        }));
    app.get('/response/redirect', () =>
        redirect('/response/string', 307, 'Temporary Redirect'));

    app.get('/prepared/status-text', event => {
        event.res.status = 209;
        event.res.statusText = 'H3 Prepared';
        return 'prepared-status-text';
    });
    app.get('/prepared/plain', event => {
        event.res.status = 202;
        event.res.statusText = 'Prepared Plain';
        event.res.headers.set('x-prepared', 'plain');
        return 'prepared-plain';
    });
    app.get('/prepared/response-merge', event => {
        event.res.status = 299;
        event.res.statusText = 'Ignored for Response';
        event.res.headers.set('x-prepared', 'override');
        event.res.headers.set('x-prepared-only', 'yes');
        return new Response('response-merge', {
            status: 206,
            statusText: 'Partial Content',
            headers: {
                'x-prepared': 'original',
                'x-response-only': 'yes',
            },
        });
    });
    app.get('/prepared/error-response', event => {
        event.res.headers.set('x-success-only', 'must-not-appear');
        event.res.errHeaders.set('x-error-only', 'yes');
        return new Response('prepared-error', {
            status: 422,
            statusText: 'Unprocessable Content',
            headers: { 'x-response-error': 'yes' },
        });
    });
    app.get('/prepared/error-throw', event => {
        event.res.headers.set('x-success-only', 'must-not-appear');
        event.res.errHeaders.set('x-error-only', 'yes');
        throw new HTTPError({
            status: 423,
            statusText: 'Locked',
            message: 'prepared-error-throw',
            headers: { 'x-http-error': 'yes' },
        });
    });
    app.get('/prepared/set-cookie', event => {
        setCookie(event, 'prepared-one', '1', { path: '/' });
        setCookie(event, 'prepared-two', '2', {
            httpOnly: true,
            path: '/prepared',
        });
        return new Response('cookie-order', {
            headers: [
                [ 'set-cookie', 'response-first=0; Path=/' ],
                [ 'x-cookie-order', 'response-then-prepared' ],
            ],
        });
    });

    app.get('/errors/http', () => {
        throw new HTTPError({
            status: 451,
            statusText: 'Unavailable For Legal Reasons',
            message: 'public-http-message',
            data: { classification: 'policy' },
            headers: { 'x-http-error': 'public' },
        });
    });
    app.get('/errors/unhandled', () => {
        throw new Error('sensitive-unhandled-message');
    });
    app.get('/errors/rejection', async () => {
        await Promise.resolve();
        throw new TypeError('sensitive-async-message');
    });
    app.get('/errors/hook-handled', () => {
        throw new HTTPError({
            status: 452,
            message: 'global-hook-handled',
        });
    });
    app.get('/errors/onerror-failure', () => {
        throw new HTTPError({
            status: 453,
            message: 'original-onerror-failure',
        });
    });
    app.get('/errors/onresponse-failure', () => 'response-survives-hook');
    app.get('/errors/stream', () => new ReadableStream({
        start(controller) {
            controller.enqueue(new TextEncoder().encode('before-error'));
            controller.error(new Error('stream-failure'));
        },
    }));
};
