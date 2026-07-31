const encoder = new TextEncoder();
const bytes = value => typeof value === 'string'
    ? encoder.encode(value)
    : new Uint8Array(value);

const jsonBody = value => bytes(JSON.stringify(value));

const multipartBoundary = 'capsid-h3-boundary';
const multipartBody = bytes([
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
    'Content-Disposition: form-data; name="upload"; ',
    'filename="capsid.txt"\r\n',
    'Content-Type: text/plain\r\n\r\n',
    'file-content\r\n',
    `--${multipartBoundary}--\r\n`,
].join(''));

const largeBody = new Uint8Array(192 * 1024 + 37);
for (let index = 0; index < largeBody.length; ++index) {
    largeBody[index] = index % 251;
}
const largeChecksum = largeBody.reduce(
    (sum, value) => (sum + value) >>> 0,
    0,
);

const v = (
    id,
    path,
    {
        method = 'GET',
        headers = [],
        body = new Uint8Array(),
        chunkSize = 257,
        expect = {},
        ...options
    } = {},
) => ({
    id,
    method,
    url: `https://compat.example${path}`,
    headers,
    body,
    chunkSize,
    expect,
    ...options,
});

export const vectors = [
    v('entry', '/entry', {
        expect: { status: 200, bodyText: 'h3-entry-ok' },
    }),

    v('core-on', '/core/on', {
        expect: { bodyText: 'core:on' },
    }),
    v('core-get', '/core/get', {
        expect: { bodyText: 'core:get' },
    }),
    v('core-post', '/core/post', {
        method: 'POST',
        expect: { bodyText: 'core:post' },
    }),
    v('core-put', '/core/put', {
        method: 'PUT',
        expect: { bodyText: 'core:put' },
    }),
    v('core-patch', '/core/patch', {
        method: 'PATCH',
        expect: { bodyText: 'core:patch' },
    }),
    v('core-delete', '/core/delete', {
        method: 'DELETE',
        expect: { bodyText: 'core:delete' },
    }),
    v('core-head', '/core/head', {
        method: 'HEAD',
        expect: {
            bodyText: '',
            headers: { 'x-h3-method': 'HEAD' },
        },
    }),
    v('core-options', '/core/options', {
        method: 'OPTIONS',
        expect: { bodyText: 'core:options' },
    }),
    v('core-query', '/core/query', {
        method: 'QUERY',
        headers: [ [ 'content-type', 'application/json' ] ],
        expect: {
            bodyText: 'core:query',
            headers: { 'accept-query': 'application/json' },
        },
    }),
    v('core-custom-method', '/core/custom', {
        method: 'PURGE',
        expect: { bodyText: 'core:PURGE' },
    }),
    v('core-all', '/core/all', {
        method: 'REPORT',
        expect: { bodyText: 'core:all:REPORT' },
    }),

    v('routing-fixed', '/routing/fixed', {
        expect: { bodyText: 'routing:fixed' },
    }),
    v('routing-param', '/routing/users/alice', {
        expect: { bodyJson: { id: 'alice' } },
    }),
    v('routing-multiple-params', '/routing/teams/red/users/bob', {
        expect: { bodyJson: { team: 'red', user: 'bob' } },
    }),
    v('routing-wildcard', '/routing/wild/a/b/c', {
        expect: { bodyJson: { path: '/routing/wild/a/b/c' } },
    }),
    v('routing-named-wildcard', '/routing/named/a/b/c', {
        expect: { bodyJson: { rest: 'a/b/c' } },
    }),
    v('routing-static-precedence', '/routing/precedence/fixed', {
        expect: { bodyText: 'fixed-wins' },
    }),
    v('routing-registration-order', '/routing/order/value', {
        expect: {
            bodyJson: { route: 'first', value: 'value' },
        },
    }),
    v('routing-trailing-slash', '/routing/trailing/', {
        expect: { bodyText: 'trailing-slash' },
    }),
    v('routing-trailing-normalized', '/routing/trailing', {
        expect: { bodyText: 'trailing-slash' },
    }),
    v(
        'routing-unicode',
        '/routing/unicode/%E4%B8%AD%E6%96%87',
        {
            expect: {
                bodyJson: {
                    raw: '%E4%B8%AD%E6%96%87',
                    decoded: '中文',
                    pathname:
                        '/routing/unicode/%E4%B8%AD%E6%96%87',
                },
            },
        },
    ),
    v('routing-encoded-separator', '/routing/encoded/a%2Fb', {
        expect: { bodyJson: { raw: 'a%2Fb' } },
    }),

    v('h3event-inspect', '/event/inspect/event-id', {
        expect: {
            status: 207,
            statusText: 'H3 Event',
            headers: { 'x-event-prepared': 'yes' },
            bodyJson: {
                reqMethod: 'GET',
                reqUrl: 'https://compat.example/event/inspect/event-id',
                urlPath: '/event/inspect/event-id',
                contextId: 'event-id',
                appMatches: true,
                matchedMethod: 'GET',
                matchedRoute: '/event/inspect/:id',
                meta: {
                    source: 'handler-meta',
                    stable: true,
                },
                responseStatus: 207,
                responseStatusText: 'H3 Event',
                responseHeader: 'yes',
                requestWaitUntil: 'undefined',
            },
        },
    }),
    v('h3event-middleware-params', '/event/middleware/scope/value', {
        method: 'POST',
        expect: {
            bodyJson: {
                routeParams: {
                    scope: 'scope',
                },
                middlewareParams: {
                    scope: 'scope',
                    _: 'value',
                },
                captured: {
                    scope: 'scope',
                    _: 'value',
                },
            },
        },
    }),
    v(
        'h3event-middleware-method-filter',
        '/event/middleware/scope/value',
        {
            expect: {
                bodyJson: {
                    middlewareSeen: null,
                },
            },
        },
    ),
    v('h3event-middleware-path-filter', '/event/unfiltered/value', {
        method: 'POST',
        expect: {
            bodyJson: {
                middlewareSeen: null,
            },
        },
    }),

    v('middleware-undefined-continues', '/middleware/continue-result', {
        expect: {
            bodyJson: {
                trace: [ 'sync', 'async', 'next' ],
            },
        },
    }),
    v('middleware-wrap-next', '/middleware/wrap', {
        expect: {
            bodyJson: {
                value: { phase: 'handler' },
                trace: [
                    'outer:before',
                    'inner:before',
                    'handler',
                    'inner:after',
                    'outer:after',
                ],
            },
        },
    }),
    v('middleware-return-intercepts', '/middleware/intercept', {
        expect: {
            bodyJson: {
                intercepted: true,
                trace: [ 'intercept' ],
            },
        },
    }),
    v('middleware-null-intercepts', '/middleware/null', {
        expect: { bodyText: '' },
    }),
    v('middleware-response-intercepts', '/middleware/response', {
        expect: {
            status: 202,
            bodyText: 'middleware-response',
            headers: { 'x-middleware-stop': 'response' },
        },
    }),
    v('middleware-next-idempotent', '/middleware/duplicate-next', {
        expect: {
            bodyJson: {
                samePromise: true,
                values: [ 'once', 'once' ],
                handlers: 1,
                trace: [ 'before', 'handler', 'after' ],
            },
        },
    }),
    v('middleware-sync-throw', '/middleware/throw', {
        expect: {
            status: 409,
            bodyJsonIncludes: {
                status: 409,
                message: 'middleware-sync-boom',
            },
        },
    }),
    v('middleware-async-reject', '/middleware/reject', {
        expect: {
            status: 410,
            bodyJsonIncludes: {
                status: 410,
                message: 'middleware-async-boom',
            },
        },
    }),

    v('hooks-global-success', '/core/get', {
        expect: {
            headers: {
                'x-h3-hook-trace': 'onRequest,onResponse',
                'x-h3-global-middleware':
                    'global:before,global:after',
            },
        },
    }),
    v('hooks-factory-success', '/hooks/factory-success', {
        expect: {
            bodyText: 'factory-success',
            headers: {
                'x-h3-factory-trace':
                    'onRequest,handler,onResponse',
            },
        },
    }),
    v('hooks-factory-error', '/hooks/factory-error', {
        expect: {
            status: 419,
            bodyText: 'factory-error-handled',
            headers: {
                'x-h3-factory-trace': 'onError:418',
            },
        },
    }),

    v('request-query', '/request/query?one=1&empty=&encoded=a%20b' +
        '&repeat=first&repeat=second', {
        expect: {
            bodyJson: {
                query: {
                    one: '1',
                    empty: '',
                    encoded: 'a b',
                    repeat: [ 'first', 'second' ],
                },
                prototypeIsNull: false,
            },
        },
    }),
    v('request-params-safe-decode', '/request/params/a%20b/c%2Fd', {
        expect: {
            bodyJson: {
                raw: {
                    first: 'a%20b',
                    second: 'c%2Fd',
                },
                decoded: {
                    first: 'a b',
                    second: 'c%2Fd',
                },
            },
        },
    }),
    v('request-native-json', '/request/json-native', {
        method: 'POST',
        headers: [ [ 'content-type', 'application/json' ] ],
        body: jsonBody({ hello: 'capsid', n: 3 }),
        expect: {
            bodyJson: {
                value: { hello: 'capsid', n: 3 },
            },
        },
    }),
    v('request-read-body-json', '/request/json', {
        method: 'POST',
        headers: [ [ 'content-type', 'application/json' ] ],
        body: jsonBody({ hello: 'h3' }),
        expect: { bodyJson: { value: { hello: 'h3' } } },
    }),
    v('request-malformed-json', '/request/json', {
        method: 'POST',
        headers: [ [ 'content-type', 'application/json' ] ],
        body: bytes('{"broken":'),
        expect: {
            status: 400,
            statusText: 'Bad Request',
            bodyJsonIncludes: {
                status: 400,
                message: 'Invalid JSON body',
            },
        },
    }),
    v('request-text', '/request/text', {
        method: 'POST',
        headers: [ [ 'content-type', 'text/plain' ] ],
        body: bytes('plain request body'),
        expect: {
            bodyJson: { value: 'plain request body' },
        },
    }),
    v('request-urlencoded', '/request/urlencoded', {
        method: 'POST',
        headers: [ [
            'content-type',
            'application/x-www-form-urlencoded',
        ] ],
        body: bytes('alpha=one&repeat=a&repeat=b&empty='),
        expect: {
            bodyJson: {
                value: {
                    alpha: 'one',
                    repeat: [ 'a', 'b' ],
                    empty: '',
                },
            },
        },
    }),
    v('request-multipart-native', '/request/form-data', {
        method: 'POST',
        headers: [ [
            'content-type',
            `multipart/form-data; boundary=${multipartBoundary}`,
        ] ],
        body: multipartBody,
        chunkSize: 17,
        expect: {
            bodyJson: {
                value: {
                    field: 'value',
                    repeated: [ 'one', 'two' ],
                    upload: {
                        name: 'capsid.txt',
                        type: 'text/plain',
                        size: 12,
                        text: 'file-content',
                    },
                },
            },
        },
    }),
    v('request-multipart-read-body', '/request/form-data-read-body', {
        method: 'POST',
        headers: [ [
            'content-type',
            `multipart/form-data; boundary=${multipartBoundary}`,
        ] ],
        body: multipartBody,
        chunkSize: 23,
        expect: {
            bodyJson: {
                value: {
                    field: 'value',
                    repeated: [ 'one', 'two' ],
                    upload: {
                        name: 'capsid.txt',
                        type: 'text/plain',
                        size: 12,
                        text: 'file-content',
                    },
                },
            },
        },
    }),
    v('request-empty', '/request/empty', {
        method: 'POST',
        expect: { bodyJson: { rawText: '' } },
    }),
    v('request-array-buffer', '/request/array-buffer', {
        method: 'POST',
        headers: [ [ 'content-type', 'application/octet-stream' ] ],
        body: bytes([ 0, 1, 2, 127, 128, 255 ]),
        expect: {
            bodyJson: {
                byteLength: 6,
                hex: '0001027f80ff',
            },
        },
    }),
    v('request-stream', '/request/stream', {
        method: 'POST',
        body: bytes('streamed-request-body'),
        chunkSize: 3,
        normalizeBodyJsonFields: [ 'chunks' ],
        expect: {
            bodyJsonIncludes: {
                size: 21,
                checksum: 2150,
            },
        },
    }),
    v('request-validated-body', '/request/validated-body', {
        method: 'POST',
        headers: [ [ 'content-type', 'application/json' ] ],
        body: jsonBody({ name: 'valid' }),
        expect: {
            bodyJson: { value: { name: 'valid' } },
        },
    }),
    v('request-validation-error', '/request/validated-body', {
        method: 'POST',
        headers: [ [ 'content-type', 'application/json' ] ],
        body: jsonBody({ name: '' }),
        expect: {
            status: 400,
            bodyJsonIncludes: {
                status: 400,
                message: 'Validation failed',
            },
        },
    }),
    v('request-validated-query', '/request/validated-query?name=query', {
        expect: {
            bodyJson: { value: { name: 'query' } },
        },
    }),
    v('request-validated-params', '/request/validated-params/a%20b', {
        expect: {
            bodyJson: { value: { name: 'a b' } },
        },
    }),
    v('request-body-size-ok', '/request/body-size', {
        method: 'POST',
        headers: [ [ 'content-length', '5' ] ],
        body: bytes('small'),
        expect: { bodyJson: { value: 'small' } },
    }),
    v('request-body-size-reject', '/request/body-size', {
        method: 'POST',
        headers: [ [ 'content-length', '32' ] ],
        body: bytes('body is larger than sixteen bytes'),
        expect: {
            status: 413,
            statusText: 'Request Entity Too Large',
            bodyJsonIncludes: { status: 413 },
        },
    }),
    v('request-content-type', '/request/content-type', {
        method: 'POST',
        headers: [ [ 'content-type', 'application/json; charset=utf-8' ] ],
        body: bytes('{}'),
        expect: {
            bodyJson: {
                matched: 'application/json',
                value: '{}',
            },
        },
    }),
    v('request-content-type-reject', '/request/content-type', {
        method: 'POST',
        headers: [ [ 'content-type', 'text/plain' ] ],
        body: bytes('text'),
        expect: { status: 415 },
    }),
    v('request-assert-method', '/request/assert-method', {
        method: 'PUT',
        expect: { bodyText: 'method:PUT' },
    }),
    v('request-assert-method-reject', '/request/assert-method', {
        method: 'GET',
        expect: {
            status: 405,
            headers: { allow: 'POST, PUT' },
        },
    }),

    v('response-object', '/response/object', {
        expect: {
            bodyJson: {
                kind: 'object',
                nested: { ok: true },
            },
        },
    }),
    v('response-array', '/response/array', {
        expect: { bodyJson: [ 'a', 2, false ] },
    }),
    v('response-number', '/response/number', {
        expect: { bodyText: '42' },
    }),
    v('response-boolean', '/response/boolean', {
        expect: { bodyText: 'false' },
    }),
    v('response-null', '/response/null', {
        expect: { bodyText: '' },
    }),
    v('response-undefined', '/response/undefined', {
        expect: { bodyText: '' },
    }),
    v('response-string', '/response/string', {
        expect: { bodyText: 'plain-string' },
    }),
    v('response-empty-string', '/response/empty-string', {
        expect: { bodyText: '' },
    }),
    v('response-html', '/response/html', {
        expect: {
            bodyText: '<h1>&lt;Capsid &amp; H3&gt;</h1>',
            headers: { 'content-type': 'text/html; charset=utf-8' },
        },
    }),
    v('response-raw-html', '/response/raw-html', {
        expect: {
            bodyText: '<strong>trusted</strong>',
            headers: { 'content-type': 'text/html; charset=utf-8' },
        },
    }),
    v('response-custom', '/response/response', {
        expect: {
            status: 201,
            statusText: 'Created by H3',
            bodyText: 'custom-response',
            headers: { 'x-custom-response': 'yes' },
        },
    }),
    v('response-array-buffer', '/response/array-buffer', {
        expect: { bodyHex: '0001027f80ff' },
    }),
    v('response-uint8-array', '/response/uint8-array', {
        expect: { bodyHex: 'ff807f020100' },
    }),
    v('response-blob', '/response/blob', {
        expect: {
            bodyText: 'blob-body',
            headers: {
                'content-type': 'application/x-h3-blob',
                'content-length': '9',
            },
        },
    }),
    v('response-file', '/response/file', {
        expect: {
            bodyText: 'file-body',
            headers: {
                'content-type': 'text/x-h3-file',
                'content-length': '9',
                'content-disposition':
                    'filename="capsid%20h3.txt"; ' +
                    "filename*=UTF-8''capsid%20h3.txt",
            },
        },
    }),
    v('response-bigint', '/response/bigint', {
        expect: { bodyText: '9007199254740993' },
    }),
    v('response-promise', '/response/promise', {
        expect: { bodyJson: { promised: true } },
    }),
    v('response-stream', '/response/stream', {
        expect: { bodyText: 'stream:one|stream:two' },
    }),
    v('response-large-stream', '/response/large-stream?size=196645', {
        expect: {
            bodyLength: 196645,
            bodyChecksum: 24572841,
        },
    }),
    v('response-serialization-error', '/response/serialization-error', {
        expect: {
            status: 500,
            bodyJsonIncludes: {
                status: 500,
                message: 'HTTPError',
                unhandled: true,
            },
        },
    }),
    v('response-head-fallback', '/response/head-fallback', {
        method: 'HEAD',
        expect: { bodyText: '' },
    }),
    v('response-no-content', '/response/no-content', {
        expect: {
            status: 204,
            statusText: 'No Content',
            bodyText: '',
        },
    }),
    v('response-not-modified', '/response/not-modified', {
        expect: {
            status: 304,
            statusText: 'Not Modified',
            bodyText: '',
            headers: { etag: '"h3-v2"' },
        },
    }),
    v('response-redirect', '/response/redirect', {
        expect: {
            status: 307,
            statusText: 'Temporary Redirect',
            headers: { location: '/response/string' },
        },
    }),

    v('prepared-plain', '/prepared/plain', {
        expect: {
            status: 202,
            statusText: 'Prepared Plain',
            bodyText: 'prepared-plain',
            headers: { 'x-prepared': 'plain' },
        },
    }),
    v('prepared-status-text', '/prepared/status-text', {
        expect: {
            status: 209,
            statusText: 'H3 Prepared',
            bodyText: 'prepared-status-text',
        },
    }),
    v('prepared-response-merge', '/prepared/response-merge', {
        expect: {
            status: 206,
            statusText: 'Partial Content',
            bodyText: 'response-merge',
            headers: {
                'x-prepared': 'override',
                'x-prepared-only': 'yes',
                'x-response-only': 'yes',
            },
        },
    }),
    v('prepared-error-response', '/prepared/error-response', {
        expect: {
            status: 422,
            statusText: 'Unprocessable Content',
            bodyText: 'prepared-error',
            headers: {
                'x-error-only': 'yes',
                'x-response-error': 'yes',
            },
            absentHeaders: [ 'x-success-only' ],
        },
    }),
    v('prepared-error-throw', '/prepared/error-throw', {
        expect: {
            status: 423,
            statusText: 'Locked',
            headers: {
                'x-error-only': 'yes',
                'x-http-error': 'yes',
            },
            absentHeaders: [ 'x-success-only' ],
        },
    }),
    v('prepared-set-cookie-order', '/prepared/set-cookie', {
        expect: {
            bodyText: 'cookie-order',
            setCookie: [
                'response-first=0; Path=/',
                'prepared-one=1; Path=/',
                'prepared-two=2; Path=/prepared; HttpOnly',
            ],
        },
    }),

    v('error-http', '/errors/http', {
        expect: {
            status: 451,
            statusText: 'Unavailable For Legal Reasons',
            headers: { 'x-http-error': 'public' },
            bodyJsonIncludes: {
                status: 451,
                message: 'public-http-message',
            },
        },
    }),
    v('error-unhandled-hidden', '/errors/unhandled', {
        expect: {
            status: 500,
            bodyJsonIncludes: {
                status: 500,
                message: 'HTTPError',
                unhandled: true,
            },
            bodyExcludes: [ 'sensitive-unhandled-message', 'stack' ],
        },
    }),
    v('error-rejection-hidden', '/errors/rejection', {
        expect: {
            status: 500,
            bodyJsonIncludes: {
                status: 500,
                message: 'HTTPError',
                unhandled: true,
            },
            bodyExcludes: [ 'sensitive-async-message', 'stack' ],
        },
    }),
    v('error-global-hook-handled', '/errors/hook-handled', {
        expect: {
            status: 454,
            statusText: 'Hook Handled',
            bodyText: 'global-hook-handled',
            headers: { 'x-hook-handled': 'yes' },
        },
    }),
    v('error-onerror-failure-bounded', '/errors/onerror-failure', {
        expect: {
            status: 500,
            bodyJsonIncludes: {
                status: 500,
                message: 'HTTPError',
                unhandled: true,
            },
        },
    }),
    v('error-onresponse-failure-bounded', '/errors/onresponse-failure', {
        expect: {
            status: 200,
            bodyText: 'response-survives-hook',
        },
    }),

    v('composition-child', '/composition/child/item/nested', {
        expect: {
            bodyJson: {
                id: 'nested',
                childMiddleware: true,
                childHooks: {
                    request: 0,
                    response: 0,
                    error: 0,
                },
                pathname: '/composition/child/item/nested',
            },
        },
    }),
    v('composition-three-level', '/composition/child/grand/leaf/deep', {
        expect: {
            bodyJson: {
                id: 'deep',
                level: 3,
                pathname: '/composition/child/grand/leaf/deep',
            },
        },
    }),
    v('composition-web-mount', '/composition/web/path?q=1', {
        expect: {
            status: 206,
            bodyText: 'web:/path?q=1',
            headers: { 'x-mounted-web': 'yes' },
        },
    }),
    v('composition-from-web', '/composition/from-web/value', {
        expect: {
            bodyJson: {
                id: 'value',
                pathname: '/composition/from-web/value',
            },
        },
    }),
    v('composition-handler', '/composition/handler/item/value', {
        expect: {
            bodyJson: {
                handler: true,
                id: 'value',
                pathname: '/item/value',
            },
        },
    }),

    v('plugin-constructor', '/plugins/constructor', {
        expect: {
            bodyJson: {
                order: [
                    'constructor',
                    'registered-one',
                    'registered-two',
                ],
                source: 'constructor',
            },
        },
    }),
    v('plugin-register', '/plugins/registered-two', {
        expect: {
            bodyJson: {
                order: [
                    'constructor',
                    'registered-one',
                    'registered-two',
                ],
                source: 'registered-two',
            },
        },
    }),
    v('plugin-defined-route', '/plugins/defined-route/route-id', {
        expect: {
            bodyJson: {
                id: 'route-id',
                meta: { source: 'defineRoute' },
            },
        },
    }),
    v('plugin-lazy', '/plugins/lazy?token=first', {
        expect: {
            bodyJson: {
                initializations: 1,
                token: 'first',
            },
        },
    }),

    v('cookies-read', '/utilities/cookies', {
        headers: [ [ 'cookie', 'alpha=one; empty=' ] ],
        expect: {
            bodyJson: {
                alpha: 'one',
                empty: '',
                missing: null,
            },
        },
    }),
    v('cookies-set-order', '/utilities/cookies/set', {
        expect: {
            bodyText: 'cookies-set',
            setCookie: [
                'first=one; Path=/; HttpOnly; SameSite=Lax',
                'second=two; Max-Age=60; Path=/utilities',
                'obsolete=; Max-Age=0; Path=/',
            ],
        },
    }),
    v('cors-wildcard', '/utilities/cors/wildcard', {
        headers: [ [ 'origin', 'https://site.example' ] ],
        expect: {
            headers: {
                'access-control-allow-origin': '*',
                'access-control-expose-headers': '*',
            },
        },
    }),
    v('cors-preflight-fixed', '/utilities/cors/fixed', {
        method: 'OPTIONS',
        headers: [
            [ 'origin', 'https://fixed.example' ],
            [ 'access-control-request-method', 'QUERY' ],
            [ 'access-control-request-headers', 'x-one,content-type' ],
        ],
        expect: {
            status: 204,
            headers: {
                'access-control-allow-origin':
                    'https://fixed.example',
                'access-control-allow-methods':
                    'GET,POST,QUERY',
                'access-control-allow-headers':
                    'x-one,content-type',
                'access-control-max-age': '1200',
            },
        },
    }),
    v('cors-list', '/utilities/cors/list', {
        headers: [ [ 'origin', 'https://two.example' ] ],
        expect: {
            headers: {
                'access-control-allow-origin': 'https://two.example',
            },
        },
    }),
    v('cors-regexp', '/utilities/cors/regexp', {
        headers: [ [ 'origin', 'https://api.trusted.example' ] ],
        expect: {
            headers: {
                'access-control-allow-origin':
                    'https://api.trusted.example',
            },
        },
    }),
    v('cors-callback', '/utilities/cors/callback', {
        headers: [ [ 'origin', 'https://yes.callback.example' ] ],
        expect: {
            headers: {
                'access-control-allow-origin':
                    'https://yes.callback.example',
            },
        },
    }),
    v('cors-credentials', '/utilities/cors/credentials', {
        headers: [ [ 'origin', 'https://credential.example' ] ],
        expect: {
            headers: {
                'access-control-allow-origin':
                    'https://credential.example',
                'access-control-allow-credentials': 'true',
                'access-control-expose-headers': 'x-visible',
            },
        },
    }),
    v('cors-error', '/utilities/cors-error', {
        headers: [ [ 'origin', 'https://fixed.example' ] ],
        expect: {
            status: 418,
            headers: {
                'access-control-allow-origin':
                    'https://fixed.example',
            },
        },
    }),
    v('basic-auth-middleware', '/utilities/auth/middleware', {
        headers: [ [ 'authorization', 'Basic Y2Fwc2lkOnNlY3JldA==' ] ],
        expect: {
            bodyJson: {
                username: 'capsid',
                realm: 'capsid-runtime',
            },
        },
    }),
    v('basic-auth-required', '/utilities/auth/required', {
        headers: [ [ 'authorization', 'Basic dXNlcjpzZWNyZXQ=' ] ],
        expect: {
            bodyJson: {
                username: 'user',
                realm: 'capsid-required',
            },
        },
    }),
    v('basic-auth-denied', '/utilities/auth/middleware', {
        expect: {
            status: 401,
            statusText: 'Authentication required',
            headers: {
                'www-authenticate':
                    'Basic realm="capsid-runtime", charset="UTF-8"',
            },
        },
    }),
    v('session-create', '/utilities/session', {
        normalizeSetCookieValues: true,
        expect: {
            bodyJson: {
                id: 'capsid-session-id',
                count: 1,
            },
            setCookieCount: 1,
        },
    }),
    v('cache-miss', '/utilities/cache', {
        expect: {
            bodyText: 'cache-body',
            headers: {
                etag: '"h3-cache-v1"',
                'last-modified': 'Tue, 02 Jan 2024 03:04:05 GMT',
                'cache-control':
                    'public, stale-while-revalidate=30, ' +
                    'max-age=60, s-maxage=60',
            },
        },
    }),
    v('cache-hit', '/utilities/cache', {
        headers: [ [ 'if-none-match', '"h3-cache-v1"' ] ],
        expect: {
            status: 304,
            bodyText: '',
            headers: { etag: '"h3-cache-v1"' },
        },
    }),
    v('static-memory', '/utilities/static/hello.txt', {
        expect: {
            bodyText: 'hello from in-memory H3 static',
            headers: {
                'content-type': 'text/plain; charset=utf-8',
                'content-length': '30',
                etag: '"memory-hello-v1"',
                'x-static-source': 'memory',
            },
        },
    }),
    v('static-memory-head', '/utilities/static/hello.txt', {
        method: 'HEAD',
        expect: {
            bodyText: '',
            headers: {
                'content-length': '30',
                etag: '"memory-hello-v1"',
            },
        },
    }),
    v('sse', '/utilities/sse', {
        expect: {
            headers: { 'content-type': 'text/event-stream' },
            bodyText:
                ': capsid-h3\n\n' +
                'id: 1\nevent: message\ndata: first\n\n' +
                'id: 2\nretry: 1500\ndata: second\n\n',
        },
    }),

    v('runtime-abort-signal', '/runtime/abort-signal', {
        expect: {
            bodyJson: {
                isAbortSignal: true,
                aborted: false,
                reason: null,
            },
        },
    }),
    v('runtime-global-surface', '/runtime/globals', {
        runtimeOnly: true,
        expect: {
            bodyJson: {
                process: 'undefined',
                Buffer: 'undefined',
                Deno: 'undefined',
                Bun: 'undefined',
                tjs: 'undefined',
                ExecutionContext: 'undefined',
            },
        },
    }),
    v('runtime-large-request', '/runtime/large-body', {
        method: 'POST',
        body: largeBody,
        chunkSize: 4093,
        normalizeBodyJsonFields: [ 'chunks' ],
        expect: {
            bodyJsonIncludes: {
                size: largeBody.length,
                checksum: largeChecksum,
            },
        },
    }),
    v('runtime-outbound-fetch', '/runtime/outbound', {
        outbound: 'direct',
        expect: {
            bodyJson: {
                status: 203,
                statusText: 'Non-Authoritative Information',
                contentType: 'text/plain',
                upstream: 'direct-h3-fetch',
                body: 'upstream:/h3-upstream',
            },
        },
    }),
    v('utility-proxy-fetch', '/utilities/proxy', {
        outbound: 'proxy',
        expect: {
            status: 203,
            statusText: 'Non-Authoritative Information',
            bodyText: 'upstream:/h3-upstream',
            headers: { 'x-h3-upstream': 'direct-h3-fetch' },
        },
    }),

    v('not-found', '/missing-h3-route', {
        expect: {
            status: 404,
            bodyJsonIncludes: {
                status: 404,
            },
        },
    }),
];

export const fixtureMetadata = {
    multipartBoundary,
    largeBodySize: largeBody.length,
    largeChecksum,
};
