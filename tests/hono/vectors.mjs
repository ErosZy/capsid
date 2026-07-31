const encoder = new TextEncoder();

export const utf8 = value => encoder.encode(value);

const repeatedText = Array.from({ length: 4096 }, (_, index) =>
    String.fromCharCode(65 + (index % 23))).join('');

const multipartBoundary = 'capsid-hono-boundary';
const multipartText = [
    `--${multipartBoundary}\r\n`,
    'Content-Disposition: form-data; name="field"\r\n\r\n',
    'value\r\n',
    `--${multipartBoundary}\r\n`,
    'Content-Disposition: form-data; name="repeated"\r\n\r\n',
    'one\r\n',
    `--${multipartBoundary}\r\n`,
    'Content-Disposition: form-data; name="repeated"\r\n\r\n',
    'two\r\n',
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
 * the same way on both sides -- an upstream Hono regression, or a shared
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
    'routing.base-path': { status: 200 },
    'routing.mount': { status: 200 },
    'request.query-repeated': { status: 200 },
    'request.headers': { status: 200 },
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
    vector('routing.all', 'PROPFIND', '/routing/all'),
    vector('routing.base-path', 'GET', '/routing/base/item/42'),
    vector('routing.route', 'GET', '/routing/child/item/7'),
    vector('routing.mount', 'GET', '/routing/mount/nested?q=1'),
    vector('routing.not-found', 'GET', '/missing/route'),

    vector(
        'request.query-repeated',
        'GET',
        '/request/query?tag=one&tag=two&encoded=capsid%20hono',
    ),
    vector('request.headers', 'GET', '/request/headers', {
        headers: [
            [ 'x-one', 'value' ],
            [ 'x-repeated', 'first' ],
            [ 'x-repeated', 'second' ],
        ],
    }),
    vector('request.json', 'POST', '/request/json', {
        headers: [[ 'content-type', 'application/json' ]],
        body: utf8('{"number":42,"text":"capsid"}'),
    }),
    vector('request.text', 'POST', '/request/text', {
        headers: [[ 'content-type', 'text/plain;charset=UTF-8' ]],
        body: utf8('snowman:☃'),
    }),
    vector('request.urlencoded', 'POST', '/request/urlencoded', {
        headers: [[
            'content-type',
            'application/x-www-form-urlencoded;charset=UTF-8',
        ]],
        body: utf8('alpha=a%20b&repeated=one&repeated=two'),
    }),
    vector('request.multipart', 'POST', '/request/multipart', {
        headers: [[
            'content-type',
            `multipart/form-data; boundary=${multipartBoundary}`,
        ]],
        body: utf8(multipartText),
        requestChunkSize: 73,
    }),
    vector('request.array-buffer', 'POST', '/request/array-buffer', {
        headers: [[ 'content-type', 'application/octet-stream' ]],
        body: new Uint8Array([ 0, 1, 2, 127, 128, 255 ]),
    }),
    vector('request.blob', 'POST', '/request/blob', {
        headers: [[ 'content-type', 'text/custom' ]],
        body: utf8('blob-body'),
    }),
    vector('request.empty', 'POST', '/request/empty'),
    vector('request.stream-over-credit', 'POST', '/request/stream', {
        headers: [[ 'content-type', 'application/octet-stream' ]],
        body: utf8(repeatedText),
        requestChunkSize: 257,
        ignoreBodyJsonFields: [ 'chunks' ],
    }),

    vector('response.text', 'GET', '/response/text'),
    vector('response.json', 'GET', '/response/json'),
    vector('response.html', 'GET', '/response/html'),
    vector('response.binary', 'GET', '/response/binary'),
    vector('response.status', 'POST', '/response/status'),
    vector('response.redirect', 'GET', '/response/redirect'),
    vector('response.headers', 'GET', '/response/headers'),
    vector('response.set-cookie-order', 'GET', '/response/cookies'),
    vector('response.stream-over-credit', 'GET', '/response/stream'),
    vector('response.head', 'HEAD', '/response/head'),
    vector('response.204', 'GET', '/response/no-content'),
    vector('response.304', 'GET', '/response/not-modified'),

    vector('middleware.order', 'GET', '/middleware/order'),
    vector('middleware.early', 'GET', '/middleware/early'),
    vector('middleware.scoped-inside', 'GET', '/middleware/scoped/inside'),
    vector('middleware.scoped-outside', 'GET', '/middleware/outside'),
    vector(
        'middleware.context-first',
        'GET',
        '/middleware/context?value=first',
    ),
    vector(
        'middleware.context-second',
        'GET',
        '/middleware/context?value=second',
    ),
    vector('middleware.response-mutation', 'GET', '/middleware/mutate'),
    vector('middleware.error-sync', 'GET', '/middleware/error-sync'),
    vector('middleware.error-async', 'GET', '/middleware/error-async'),

    vector('builtin.cors-get', 'GET', '/builtin/cors', {
        headers: [[ 'origin', 'https://client.example' ]],
    }),
    vector('builtin.cors-preflight', 'OPTIONS', '/builtin/cors', {
        headers: [
            [ 'origin', 'https://client.example' ],
            [ 'access-control-request-method', 'POST' ],
        ],
    }),
    vector('builtin.secure-headers', 'GET', '/builtin/secure-headers'),
    vector('builtin.body-limit-accept', 'POST', '/builtin/body-limit', {
        body: utf8('12345678'),
    }),
    vector('builtin.body-limit-reject', 'POST', '/builtin/body-limit', {
        body: utf8('123456789'),
    }),
    vector('builtin.basic-auth-deny', 'GET', '/builtin/basic-auth'),
    vector('builtin.basic-auth-allow', 'GET', '/builtin/basic-auth', {
        headers: [[ 'authorization', `Basic ${btoa('capsid:hono')}` ]],
    }),
    vector('builtin.bearer-auth-deny', 'GET', '/builtin/bearer-auth'),
    vector('builtin.bearer-auth-allow', 'GET', '/builtin/bearer-auth', {
        headers: [[ 'authorization', 'Bearer capsid-token' ]],
    }),
    vector('builtin.jwt', 'GET', '/builtin/jwt', {
        headers: [[ 'authorization', 'Bearer $jwt' ]],
    }),
    vector('builtin.request-id-provided', 'GET', '/builtin/request-id', {
        headers: [[ 'x-request-id', 'caller-id' ]],
    }),
    vector('builtin.request-id-generated', 'GET', '/builtin/request-id'),
    vector('builtin.etag', 'GET', '/builtin/etag', {
        captureHeader: [ 'etag', 'etag' ],
    }),
    vector('builtin.etag-conditional', 'GET', '/builtin/etag', {
        headers: [[ 'if-none-match', '$etag' ]],
    }),
    vector('builtin.compress', 'GET', '/builtin/compress', {
        headers: [[ 'accept-encoding', 'deflate' ]],
        decompressBody: 'deflate',
    }),
    vector('builtin.logger', 'GET', '/builtin/logger'),
    vector('builtin.timing', 'GET', '/builtin/timing'),
    vector('builtin.cookie', 'GET', '/builtin/cookie', {
        headers: [[ 'cookie', 'input=cookie-value' ]],
    }),
    vector('builtin.streaming', 'GET', '/builtin/streaming'),
    vector('builtin.factory', 'GET', '/builtin/factory'),

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
