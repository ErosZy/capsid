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
    // CORS preflight is answered by the plugin with 204, not routed.
    'cors.preflight': { status: 204 },
    // A guard beforeHandle returning a Response must short-circuit with 403.
    'guard.deny': { status: 403 },
    // A typebox body mismatch must surface as 422, not 500 or 200.
    'schema.body-invalid': { status: 422 },
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

    /* --- @elysiajs plugins --- */
    vector('cors.echo', 'GET', '/cors/echo', {
        headers: [[ 'origin', 'https://capsid.test' ]],
    }),
    vector('cors.preflight', 'OPTIONS', '/cors/echo', {
        headers: [
            [ 'origin', 'https://capsid.test' ],
            [ 'access-control-request-method', 'POST' ],
        ],
    }),
    // A non-matching origin must not be echoed by the pinned origin policy.
    vector('cors.rejected-origin', 'GET', '/cors/echo', {
        headers: [[ 'origin', 'https://evil.test' ]],
    }),
    vector('bearer.header', 'GET', '/bearer/echo', {
        headers: [[ 'authorization', 'Bearer tok-123' ]],
    }),
    vector('bearer.query', 'GET', '/bearer/echo?access_token=qtok'),
    vector('bearer.missing', 'GET', '/bearer/echo'),
    // sign() stamps iat, so the token text is nondeterministic across the two
    // environments; the field is normalized and the structure is still
    // compared. Deterministic signing is covered by verify-good's fixed
    // pre-signed token (HS256, sub=capsid-test-user, no iat).
    vector('jwt.sign', 'GET', '/jwt/sign', {
        ignoreBodyJsonFields: [ 'token' ],
    }),
    vector('jwt.verify-good', 'GET', '/jwt/verify', {
        headers: [[
            'authorization',
            'Bearer eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.' +
                'eyJzdWIiOiJjYXBzaWQtdGVzdC11c2VyIn0.' +
                'KgkH3EVFzVV7jcbFj2Hl0UaUi6E_kuyebHsXqIntPPY',
        ]],
    }),
    vector('jwt.verify-bad', 'GET', '/jwt/verify', {
        headers: [[ 'authorization', 'Bearer junk.token.here' ]],
    }),
    // Stream.send assigns a random nanoid id per frame; only the data lines
    // participate in the differential.
    vector('stream.sse', 'GET', '/stream/sse', {
        bodyTextReplacements: [[ /id: [A-Za-z0-9_-]{21}\n/g, 'id: <id>\n' ]],
    }),
    vector('stream.sse-generator', 'GET', '/stream/sse-generator'),

    /* --- core capabilities --- */
    vector('guard.pass', 'GET', '/guard/protected', {
        headers: [[ 'x-guard-pass', '1' ]],
    }),
    vector('guard.deny', 'GET', '/guard/protected'),
    vector('derive.pass', 'GET', '/derive/echo', {
        headers: [[ 'x-derived', 'from-here' ]],
    }),
    vector('derive.none', 'GET', '/derive/echo'),
    vector('resolve.path', 'GET', '/resolve/echo'),
    vector('schema.body-ok', 'POST', '/schema/body', {
        headers: [[ 'content-type', 'application/json' ]],
        body: utf8('{"name":"alice","age":30}'),
    }),
    vector('schema.body-invalid', 'POST', '/schema/body', {
        headers: [[ 'content-type', 'application/json' ]],
        body: utf8('{"name":42}'),
    }),
    vector('schema.body-optional', 'POST', '/schema/body', {
        headers: [[ 'content-type', 'application/json' ]],
        body: utf8('{"name":"alice"}'),
    }),
    vector('schema.params-ok', 'GET', '/schema/params/abc-123'),

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
