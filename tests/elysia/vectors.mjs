const encoder = new TextEncoder();

export const utf8 = value => encoder.encode(value);

const multipartBoundary = 'capsid-elysia-boundary';
const multipartText = [
    `--${multipartBoundary}\r\n`,
    'Content-Disposition: form-data; name="field"\r\n\r\n',
    'value\r\n',
    `--${multipartBoundary}\r\n`,
    'Content-Disposition: form-data; name="upload"; filename="capsid.txt"\r\n',
    'Content-Type: text/plain\r\n\r\n',
    'file-body\r\n',
    `--${multipartBoundary}--\r\n`,
].join('');

const vector = (id, method, path, options = {}) => ({
    id,
    method,
    url: `https://compat.example${path}`,
    headers: [],
    body: new Uint8Array(),
    requestChunkSize: 257,
    ...options,
});

/*
 * Absolute expectations, checked against BOTH environments independently of the
 * runtime-vs-reference comparison.
 *
 * The differential comparison alone cannot catch a behaviour that is wrong in
 * the same way on both sides -- an upstream Elysia regression, or a shared
 * misreading of the vector. These anchors pin a handful of load-bearing
 * responses to values derived from the HTTP and Fetch specifications rather than
 * from either implementation, so that "reference and runtime agree" is never the
 * only evidence a vector produces.
 *
 * Keyed by vector id; every id must exist in `vectors`, which
 * differential.mjs asserts at startup.
 */
export const absoluteExpectations = {
    'entry.default': { status: 200 },
    'routing.method.get': { status: 200 },
    'routing.method.post': { status: 200 },
    'routing.method.delete': { status: 200 },
    // An unmatched route must be 404, not a 200 with an error body.
    'routing.not-found': { status: 404 },
    'routing.params': { status: 200 },
    'routing.wildcard': { status: 200 },
    'response.status': { status: 202 },
    // set.status 418 must survive mapping, not degrade to 200.
    'error.status': { status: 418 },
    // An uncaught handler error must surface as 500, not 200.
    'error.throw': { status: 500 },
};

export const smokeVector = vector('entry.default', 'GET', '/entry');

export const vectors = [
    smokeVector,
    vector('routing.method.get', 'GET', '/routing/method'),
    vector('routing.method.post', 'POST', '/routing/method'),
    vector('routing.method.put', 'PUT', '/routing/method'),
    vector('routing.method.patch', 'PATCH', '/routing/method'),
    vector('routing.method.delete', 'DELETE', '/routing/method'),
    vector('routing.method.options', 'OPTIONS', '/routing/method'),
    vector('routing.static', 'GET', '/routing/static'),
    vector('routing.params', 'GET', '/routing/users/alice%20capsid'),
    vector('routing.wildcard', 'GET', '/routing/assets/a/b/c.txt'),
    vector('routing.query', 'GET', '/routing/query?value=hello%20world'),
    vector('routing.not-found', 'GET', '/missing/route'),

    vector('middleware.before', 'GET', '/middleware/before'),
    vector('middleware.headers', 'GET', '/middleware/headers'),

    vector('body.text', 'POST', '/body/text', {
        headers: [[ 'content-type', 'text/plain;charset=UTF-8' ]],
        body: utf8('snowman:☃'),
    }),
    vector('body.json', 'POST', '/body/json', {
        headers: [[ 'content-type', 'application/json' ]],
        body: utf8('{"number":42,"name":"capsid"}'),
    }),
    vector('body.form', 'POST', '/body/form', {
        headers: [[
            'content-type',
            `multipart/form-data; boundary=${multipartBoundary}`,
        ]],
        body: utf8(multipartText),
        requestChunkSize: 73,
    }),

    vector('headers.echo', 'GET', '/headers/echo', {
        headers: [[ 'x-elysia-send', 'echo-me' ]],
    }),
    vector('cookie.read', 'GET', '/cookie/read', {
        headers: [[ 'cookie', 'flavor=vanilla' ]],
    }),
    vector('cookie.write', 'GET', '/cookie/write'),

    vector('response.json-auto', 'GET', '/response/json-auto'),
    vector('response.binary', 'GET', '/response/binary'),
    vector('response.status', 'GET', '/response/status'),
    // 1.4.29's web-standard adapter ignores set.redirect: the vector pins the
    // ACTUAL shared behaviour (200 + body, no Location) rather than a redirect.
    vector('response.redirect', 'GET', '/response/redirect'),
    vector('stream.one', 'GET', '/stream/one'),
    vector('stream.chunks', 'GET', '/stream/chunks'),

    vector('error.throw', 'GET', '/error/throw'),
    vector('error.status', 'GET', '/error/status'),

    vector('runtime.globals-absent', 'GET', '/runtime/globals', {
        runtimeJsonExpected: {
            process: 'undefined',
            Buffer: 'undefined',
            Deno: 'undefined',
            Bun: 'undefined',
            caches: 'undefined',
            tjs: 'undefined',
        },
    }),
    vector('runtime.outbound-fetch', 'GET', '/runtime/fetch', {
        outboundFetch: true,
    }),
];
