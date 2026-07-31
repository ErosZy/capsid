const encoder = new TextEncoder();
const bytes = value => typeof value === 'string'
    ? encoder.encode(value)
    : new Uint8Array(value ?? []);
const url = path => `https://compat.example${path}`;

const makeVector = (
    id,
    path,
    {
        method = 'GET',
        headers = [],
        body = new Uint8Array(),
        requestChunkSize = 257,
        expect = {},
        ...extra
    } = {},
) => ({
    id,
    method,
    url: url(path),
    headers,
    body: bytes(body),
    requestChunkSize,
    expect,
    ...extra,
});

const jsonExpectation = (bodyJson, extra = {}) => ({
    status: 200,
    bodyJson,
    ...extra,
});

const textExpectation = (bodyText, extra = {}) => ({
    status: 200,
    bodyText,
    ...extra,
});

const traceHeader = trace => ({
    'x-execution-trace': trace.join(','),
});

const largeRequestBody = new Uint8Array(8192);
let largeRequestChecksum = 0;
for (let index = 0; index < largeRequestBody.length; ++index) {
    largeRequestBody[index] = (index * 31) % 251;
    largeRequestChecksum =
        (largeRequestChecksum + largeRequestBody[index]) >>> 0;
}

const multipartBoundary = 'capsid-itty-boundary';
const multipartBody = [
    `--${multipartBoundary}\r\n`,
    'Content-Disposition: form-data; name="field"\r\n\r\n',
    'value\r\n',
    `--${multipartBoundary}\r\n`,
    'Content-Disposition: form-data; name="repeated"\r\n\r\n',
    'first\r\n',
    `--${multipartBoundary}\r\n`,
    'Content-Disposition: form-data; name="repeated"\r\n\r\n',
    'second\r\n',
    `--${multipartBoundary}\r\n`,
    'Content-Disposition: form-data; name="upload"; filename="capsid.txt"\r\n',
    'Content-Type: text/plain\r\n\r\n',
    'itty-file\r\n',
    `--${multipartBoundary}--\r\n`,
].join('');

export const vectors = [
    makeVector('entry', '/entry', {
        expect: textExpectation('itty-entry-ok'),
    }),
    makeVector('variant-name', '/variant/name', {
        expect: {
            status: 200,
            bodyJson: variant => ({ variant }),
        },
    }),
    makeVector('variant-auto-custom', '/variant/auto/item/7', {
        expect: jsonExpectation({
            customFormat: { id: '7', before: 'custom-before' },
        }, {
            headers: { 'x-custom-auto-finally': 'yes' },
        }),
    }),
    makeVector('variant-auto-custom-missing', '/variant/auto/missing', {
        expect: jsonExpectation({
            customFormat: {
                missing: '/variant/auto/missing',
            },
        }, {
            headers: { 'x-custom-auto-finally': 'yes' },
        }),
    }),
    makeVector('variant-auto-custom-catch', '/variant/auto/throw', {
        expect: {
            status: 417,
            bodyJson: { status: 417, error: 'custom-auto-boom' },
            headers: {
                'x-custom-auto-catch': 'custom-before',
                'x-custom-auto-finally': 'yes',
            },
        },
    }),
    makeVector('variant-auto-default-format', '/variant/default-auto/object', {
        expect: jsonExpectation({ defaultAuto: true }),
    }),
    makeVector('variant-auto-default-missing', '/variant/default-auto/missing', {
        expect: {
            status: 404,
            bodyJson: { status: 404, error: 'Not Found' },
        },
    }),

    ...[ 'get', 'post', 'put', 'patch', 'delete', 'options' ].map(method =>
        makeVector(`method-${method}`, `/methods/${method}`, {
            method: method.toUpperCase(),
            expect: textExpectation(method.toUpperCase()),
        })),
    makeVector('method-head', '/methods/head', {
        method: 'HEAD',
        expect: {
            status: 200,
            bodyText: '',
            headers: { 'x-method': 'HEAD' },
        },
    }),
    makeVector('method-all', '/methods/all', {
        method: 'POST',
        expect: textExpectation('all:POST'),
    }),
    makeVector('method-nonstandard', '/methods/nonstandard', {
        method: 'PURGE',
        expect: textExpectation('nonstandard:PURGE'),
    }),

    makeVector('pattern-fixed', '/patterns/fixed', {
        expect: jsonExpectation({ pattern: 'fixed' }),
    }),
    makeVector('pattern-param', '/patterns/users/42', {
        expect: jsonExpectation({ id: '42', params: { id: '42' } }),
    }),
    makeVector('pattern-multiple', '/patterns/multiple/capsid/17', {
        expect: jsonExpectation({ team: 'capsid', id: '17' }),
    }),
    makeVector('pattern-optional-absent', '/patterns/optional', {
        expect: jsonExpectation({ id: null }),
    }),
    makeVector('pattern-optional-present', '/patterns/optional/value', {
        expect: jsonExpectation({ id: 'value' }),
    }),
    makeVector('pattern-file-extension', '/patterns/files/archive.tar', {
        expect: jsonExpectation({ name: 'archive', extension: 'tar' }),
    }),
    makeVector('pattern-wildcard', '/patterns/wild/a/b/c', {
        expect: jsonExpectation({
            path: '/patterns/wild/a/b/c',
            route: '/patterns/wild/*',
        }),
    }),
    makeVector('pattern-greedy', '/patterns/greedy/a/b/c', {
        expect: jsonExpectation({ path: 'a/b/c' }),
    }),
    makeVector('pattern-base', '/patterns/base/item/91', {
        expect: jsonExpectation({ base: true, id: '91' }),
    }),
    makeVector('pattern-order-dynamic-first', '/patterns/order/fixed', {
        expect: jsonExpectation({
            winner: 'dynamic-first',
            value: 'fixed',
        }),
    }),
    makeVector(
        'pattern-order-fixed-first',
        '/patterns/fixed-first/fixed',
        { expect: jsonExpectation({ winner: 'fixed-first' }) },
    ),
    makeVector('pattern-unicode', '/patterns/unicode/雪', {
        expect: jsonExpectation({
            encoded: '%E9%9B%AA',
            decoded: '雪',
        }),
    }),
    makeVector('pattern-encoded', '/patterns/unicode/a%2Fb%20c', {
        expect: jsonExpectation({
            encoded: 'a%2Fb%20c',
            decoded: 'a/b c',
        }),
    }),

    makeVector('query-single', '/query/inspect?name=capsid', {
        expect: jsonExpectation({
            query: { name: 'capsid' },
            prototypeIsNull: true,
        }),
    }),
    makeVector('query-empty', '/query/inspect?empty=', {
        expect: jsonExpectation({
            query: { empty: '' },
            prototypeIsNull: true,
        }),
    }),
    makeVector('query-encoded', '/query/inspect?value=a%2Fb+snow', {
        expect: jsonExpectation({
            query: { value: 'a/b snow' },
            prototypeIsNull: true,
        }),
    }),
    makeVector('query-repeated', '/query/inspect?tag=a&tag=b&tag=c', {
        expect: jsonExpectation({
            query: { tag: [ 'a', 'b', 'c' ] },
            prototypeIsNull: true,
        }),
    }),
    makeVector('query-no-leak-first', '/query/inspect?only=first', {
        expect: jsonExpectation({
            query: { only: 'first' },
            prototypeIsNull: true,
        }),
    }),
    makeVector('query-no-leak-second', '/query/inspect?only=second', {
        expect: jsonExpectation({
            query: { only: 'second' },
            prototypeIsNull: true,
        }),
    }),

    makeVector('flow-continue', '/flow/continue', {
        expect: jsonExpectation({
            trace: [
                'undefined',
                'null',
                'async:start',
                'async:end',
                'terminal',
            ],
        }, {
            headers: traceHeader([
                'undefined',
                'null',
                'async:start',
                'async:end',
                'terminal',
            ]),
        }),
    }),
    makeVector('flow-stop-response', '/flow/stop/response', {
        expect: textExpectation('response-stop', {
            headers: traceHeader([ 'first', 'stop:response' ]),
        }),
    }),
    makeVector('flow-stop-object', '/flow/stop/object', {
        expect: jsonExpectation({
            kind: 'object',
            trace: [ 'first', 'stop:object' ],
        }, {
            headers: traceHeader([ 'first', 'stop:object' ]),
        }),
    }),
    makeVector('flow-stop-string', '/flow/stop/string', {
        expect: textExpectation('"string-stop"', {
            headers: traceHeader([ 'first', 'stop:string' ]),
        }),
    }),
    makeVector('flow-stop-zero', '/flow/stop/zero', {
        expect: textExpectation('0', {
            headers: traceHeader([ 'first', 'stop:zero' ]),
        }),
    }),
    makeVector('flow-stop-false', '/flow/stop/false', {
        expect: textExpectation('false', {
            headers: traceHeader([ 'first', 'stop:false' ]),
        }),
    }),
    makeVector('flow-stop-empty-string', '/flow/stop/empty-string', {
        expect: textExpectation('""', {
            headers: traceHeader([ 'first', 'stop:empty-string' ]),
        }),
    }),
    makeVector('flow-promise-awaited', '/flow/promise', {
        expect: jsonExpectation({
            value: 'awaited',
            trace: [ 'promise:start', 'promise:end' ],
        }, {
            headers: traceHeader([ 'promise:start', 'promise:end' ]),
        }),
    }),
    makeVector('flow-sync-async-order', '/flow/mixed', {
        expect: jsonExpectation({
            trace: [ 'sync:one', 'async:two', 'sync:three' ],
        }, {
            headers: traceHeader([ 'sync:one', 'async:two', 'sync:three' ]),
        }),
    }),
    makeVector('flow-sync-throw', '/flow/throw', {
        expect: {
            status: 500,
            bodyJson: { status: 500, error: 'sync-flow-boom' },
            headers: {
                'x-error-class': 'TypeError',
                ...traceHeader([ 'throw:sync', 'catch:TypeError' ]),
            },
        },
    }),
    makeVector('flow-async-reject', '/flow/reject', {
        expect: {
            status: 500,
            bodyJson: { status: 500, error: 'async-flow-boom' },
            headers: {
                'x-error-class': 'RangeError',
                ...traceHeader([ 'throw:async', 'catch:RangeError' ]),
            },
        },
    }),

    makeVector('stage-success', '/stages/success', {
        expect: jsonExpectation({
            beforeFinally: [ 'before:sync', 'before:async', 'route:sync' ],
        }, {
            headers: traceHeader([
                'before:sync',
                'before:async',
                'route:sync',
                'finally:async',
            ]),
        }),
    }),
    makeVector('stage-async', '/stages/async', {
        expect: jsonExpectation({
            beforeFinally: [ 'before:sync', 'before:async', 'route:async' ],
        }, {
            headers: traceHeader([
                'before:sync',
                'before:async',
                'route:async',
                'finally:async',
            ]),
        }),
    }),
    makeVector('stage-before-throw', '/stages/before-throw', {
        expect: {
            status: 500,
            bodyJson: { status: 500, error: 'before-stage-boom' },
            headers: {
                'x-error-class': 'TypeError',
                ...traceHeader([
                    'before:sync',
                    'catch:TypeError',
                    'finally:async',
                ]),
            },
        },
    }),
    makeVector('stage-route-throw', '/stages/route-throw', {
        expect: {
            status: 422,
            bodyJson: { status: 422, error: 'route-stage-boom' },
            headers: {
                'x-error-class': 'StatusError',
                ...traceHeader([
                    'before:sync',
                    'before:async',
                    'route:throw',
                    'catch:StatusError',
                    'finally:async',
                ]),
            },
        },
    }),
    makeVector('stage-finally-throw', '/stages/finally-throw', {
        expect: {
            status: 598,
            bodyJson: { status: 598, error: 'finally-stage-boom' },
            headers: {
                'x-error-class': 'StatusError',
                ...traceHeader([
                    'before:sync',
                    'before:async',
                    'route:before-finally-throw',
                    'finally:async',
                    'catch:StatusError',
                ]),
            },
        },
    }),

    makeVector('context-custom', '/context/custom/context-id', {
        headers: [ [ 'x-custom', 'visible-downstream' ] ],
        expect: jsonExpectation({
            id: 'context-id',
            custom: 'visible-downstream',
        }),
    }),
    makeVector('context-helpers', '/context/helpers/helper-id', {
        method: 'POST',
        headers: [
            [ 'content-type', 'application/json' ],
            [ 'cookie', 'one=1; encoded=a%20b' ],
        ],
        body: '{"hello":"capsid"}',
        expect: jsonExpectation({
            id: 'helper-id',
            content: { hello: 'capsid' },
            cookies: { one: '1', encoded: 'a%20b' },
        }),
    }),
    makeVector('context-isolation-first', '/context/isolation?value=first', {
        expect: jsonExpectation({ previous: null, current: 'first' }),
    }),
    makeVector('context-isolation-second', '/context/isolation?value=second', {
        expect: jsonExpectation({ previous: null, current: 'second' }),
    }),

    makeVector('content-json', '/content/json', {
        method: 'POST',
        headers: [ [ 'content-type', 'application/json' ] ],
        body: '{"answer":42,"nested":{"ok":true}}',
        expect: jsonExpectation({
            content: { answer: 42, nested: { ok: true } },
        }),
    }),
    makeVector('content-malformed-json', '/content/malformed-json', {
        method: 'POST',
        headers: [ [ 'content-type', 'application/json' ] ],
        body: '{"broken":',
        expect: jsonExpectation({ content: '{"broken":' }),
    }),
    makeVector('content-text', '/content/text', {
        method: 'POST',
        headers: [ [ 'content-type', 'text/plain' ] ],
        body: 'capsid text',
        expect: jsonExpectation({ content: 'capsid text' }),
    }),
    makeVector('content-direct-form', '/content/direct-form', {
        method: 'POST',
        headers: [
            [ 'content-type', 'application/x-www-form-urlencoded' ],
        ],
        body: 'alpha=one&repeated=a&repeated=b',
        expect: jsonExpectation({
            contentType: 'application/x-www-form-urlencoded',
            content: { alpha: 'one', repeated: [ 'a', 'b' ] },
        }),
    }),
    makeVector('content-cloned-form', '/content/clone-form', {
        method: 'POST',
        headers: [
            [ 'content-type', 'application/x-www-form-urlencoded' ],
        ],
        body: 'alpha=one&repeated=a&repeated=b',
        expect: jsonExpectation({
            jsonError: 'SyntaxError',
            originalContentType: 'application/x-www-form-urlencoded',
            jsonCloneContentType: 'application/x-www-form-urlencoded',
            originalAfterJson: 'application/x-www-form-urlencoded',
            formCloneContentType: 'application/x-www-form-urlencoded',
            content: { alpha: 'one', repeated: [ 'a', 'b' ] },
        }),
    }),
    makeVector('content-urlencoded', '/content/urlencoded', {
        method: 'POST',
        headers: [
            [ 'content-type', 'application/x-www-form-urlencoded' ],
        ],
        body: 'alpha=one&repeated=a&repeated=b',
        expect: jsonExpectation({
            content: { alpha: 'one', repeated: [ 'a', 'b' ] },
        }),
    }),
    makeVector('content-multipart', '/content/multipart', {
        method: 'POST',
        headers: [ [
            'content-type',
            `multipart/form-data; boundary=${multipartBoundary}`,
        ] ],
        body: multipartBody,
        requestChunkSize: 113,
        expect: jsonExpectation({
            content: {
                field: 'value',
                repeated: [ 'first', 'second' ],
                upload: {
                    name: 'capsid.txt',
                    type: 'text/plain',
                    size: 9,
                    text: 'itty-file',
                },
            },
        }),
    }),
    makeVector('content-empty', '/content/empty', {
        method: 'POST',
        expect: jsonExpectation({ content: '' }),
    }),
    makeVector('content-stream-credit', '/content/stream', {
        method: 'POST',
        headers: [ [ 'content-type', 'application/octet-stream' ] ],
        body: largeRequestBody,
        requestChunkSize: 333,
        ignoreBodyJsonFields: [ 'chunks' ],
        expect: jsonExpectation({
            chunks: '<normalized>',
            size: largeRequestBody.length,
            checksum: largeRequestChecksum,
        }),
    }),

    makeVector('response-json', '/responses/json', {
        expect: jsonExpectation({ helper: 'json' }, {
            headers: {
                'content-type': 'application/json; charset=utf-8',
            },
        }),
    }),
    makeVector('response-text', '/responses/text', {
        expect: textExpectation('text-helper', {
            headers: {
                'content-type': 'text/plain; charset=utf-8',
            },
        }),
    }),
    makeVector('response-html', '/responses/html', {
        expect: textExpectation('<h1>itty</h1>', {
            headers: { 'content-type': 'text/html' },
        }),
    }),
    makeVector('response-jpeg', '/responses/jpeg', {
        expect: {
            status: 200,
            bodyHex: 'ffd801',
            headers: { 'content-type': 'image/jpeg' },
        },
    }),
    makeVector('response-png', '/responses/png', {
        expect: {
            status: 200,
            bodyHex: '89504e47',
            headers: { 'content-type': 'image/png' },
        },
    }),
    makeVector('response-webp', '/responses/webp', {
        expect: {
            status: 200,
            bodyHex: '52494646',
            headers: { 'content-type': 'image/webp' },
        },
    }),
    makeVector('response-create', '/responses/create', {
        expect: textExpectation('capsid:created', {
            headers: { 'content-type': 'application/x-capsid' },
        }),
    }),
    makeVector('response-status', '/responses/status', {
        expect: {
            status: 204,
            bodyText: '',
            headers: { 'x-status-helper': 'yes' },
        },
    }),
    makeVector('response-error', '/responses/error', {
        expect: {
            status: 418,
            bodyJson: { status: 418, error: 'teapot' },
        },
    }),
    makeVector('response-status-error', '/responses/status-error', {
        expect: {
            status: 409,
            bodyJson: { status: 409, error: 'status-conflict' },
            headers: { 'x-error-class': 'StatusError' },
        },
    }),
    makeVector('response-custom', '/responses/custom', {
        expect: {
            status: 207,
            bodyText: 'custom-response',
            headers: { 'x-custom-response': 'yes' },
        },
    }),
    makeVector('response-blob', '/responses/blob', {
        expect: {
            status: 206,
            bodyHex: '00017f80ff',
            headers: {
                'content-type': 'application/octet-stream',
                'x-blob': 'yes',
            },
        },
    }),
    makeVector('response-custom-status-headers', '/responses/headers', {
        expect: {
            status: 202,
            bodyText: 'headers',
            headers: { 'x-first': 'one', 'x-second': 'two' },
        },
    }),
    makeVector('response-not-modified', '/responses/not-modified', {
        expect: {
            status: 304,
            bodyText: '',
            headers: { etag: '"itty"' },
        },
    }),
    makeVector('response-stream-credit', '/responses/stream', {
        expect: {
            status: 200,
            bodyLength: 5120,
            bodyChecksum: 381440,
            headers: { 'content-type': 'application/octet-stream' },
        },
    }),

    makeVector('cors-wildcard', '/cors/wildcard', {
        headers: [ [ 'origin', 'https://client.example' ] ],
        expect: jsonExpectation({ cors: 'wildcard' }, {
            headers: { 'access-control-allow-origin': '*' },
        }),
    }),
    makeVector('cors-fixed', '/cors/fixed', {
        headers: [ [ 'origin', 'https://other.example' ] ],
        expect: jsonExpectation({ cors: 'fixed' }, {
            headers: {
                'access-control-allow-origin': 'https://fixed.example',
            },
        }),
    }),
    makeVector('cors-list-match', '/cors/list', {
        headers: [ [ 'origin', 'https://two.example' ] ],
        expect: jsonExpectation({ cors: 'list' }, {
            headers: {
                'access-control-allow-origin': 'https://two.example',
            },
        }),
    }),
    makeVector('cors-list-miss', '/cors/list', {
        headers: [ [ 'origin', 'https://miss.example' ] ],
        expect: jsonExpectation({ cors: 'list' }, {
            absentHeaders: [ 'access-control-allow-origin' ],
        }),
    }),
    makeVector('cors-regexp', '/cors/regexp', {
        headers: [ [ 'origin', 'https://api.trusted.example' ] ],
        expect: jsonExpectation({ cors: 'regexp' }, {
            headers: {
                'access-control-allow-origin':
                    'https://api.trusted.example',
            },
        }),
    }),
    makeVector('cors-callback', '/cors/callback', {
        headers: [ [ 'origin', 'https://api.callback.example' ] ],
        expect: jsonExpectation({ cors: 'callback' }, {
            headers: {
                'access-control-allow-origin':
                    'https://api.callback.example',
            },
        }),
    }),
    makeVector('cors-credentials', '/cors/credentials', {
        headers: [ [ 'origin', 'https://credentials.example' ] ],
        expect: jsonExpectation({ cors: 'credentials' }, {
            headers: {
                'access-control-allow-origin':
                    'https://credentials.example',
                'access-control-allow-credentials': 'true',
            },
        }),
    }),
    makeVector('cors-options-get', '/cors/options', {
        headers: [ [ 'origin', 'https://options.example' ] ],
        expect: jsonExpectation({ cors: 'options' }, {
            headers: {
                'access-control-allow-origin': 'https://options.example',
                'access-control-allow-credentials': 'true',
            },
        }),
    }),
    makeVector('cors-preflight', '/cors/options', {
        method: 'OPTIONS',
        headers: [
            [ 'origin', 'https://options.example' ],
            [ 'access-control-request-method', 'PATCH' ],
            [ 'access-control-request-headers', 'x-input, content-type' ],
        ],
        expect: {
            status: 204,
            bodyText: '',
            headers: {
                'access-control-allow-origin': 'https://options.example',
                'access-control-allow-credentials': 'true',
                'access-control-allow-methods': 'GET,POST,PATCH',
                'access-control-allow-headers': 'x-input,content-type',
                'access-control-expose-headers': 'x-output',
                'access-control-max-age': '7200',
            },
        },
    }),
    makeVector('cors-error-response', '/cors/error', {
        headers: [ [ 'origin', 'https://error.example' ] ],
        expect: {
            status: 451,
            bodyJson: { status: 451, error: 'cors-error' },
            headers: {
                'x-error-class': 'StatusError',
                'access-control-allow-origin': 'https://error.example',
            },
        },
    }),
    makeVector('cors-404-response', '/cors/missing/route', {
        headers: [ [ 'origin', 'https://missing.example' ] ],
        expect: {
            status: 404,
            bodyJson: {
                status: 404,
                error: 'missing',
                path: '/cors/missing/route',
            },
            headers: {
                'access-control-allow-origin': 'https://missing.example',
            },
        },
    }),

    makeVector('nest-child', '/nest/item/child-id', {
        expect: jsonExpectation({
            id: 'child-id',
            parent: 'parent-middleware',
        }),
    }),
    makeVector('nest-three-levels', '/nest/deep/leaf/deep-id', {
        expect: jsonExpectation({
            id: 'deep-id',
            parent: 'parent-middleware',
            depth: 3,
        }),
    }),
    makeVector('nest-child-404', '/nest/unknown/path', {
        expect: {
            status: 404,
            bodyJson: {
                status: 404,
                error: 'missing',
                path: '/nest/unknown/path',
            },
        },
    }),

    makeVector('runtime-globals', '/runtime/globals', {
        runtimeJsonExpected: {
            process: 'undefined',
            Buffer: 'undefined',
            Deno: 'undefined',
            Bun: 'undefined',
            tjs: 'undefined',
        },
        expect: { status: 200 },
    }),
    makeVector('runtime-outbound-fetch', '/runtime/fetch', {
        outboundFetch: true,
        expect: jsonExpectation({
            status: 203,
            header: 'direct-itty-fetch',
            body: 'upstream:/itty-upstream',
        }),
    }),

    makeVector('missing-root', '/definitely-missing', {
        expect: {
            status: 404,
            bodyJson: {
                status: 404,
                error: 'missing',
                path: '/definitely-missing',
            },
        },
    }),
];

export const smokeVector = vectors[0];
