import {
    AutoRouter,
    IttyRouter,
    Router,
    StatusError,
    cors,
    createResponse,
    error,
    html,
    jpeg,
    json,
    png,
    status,
    text,
    webp,
    withContent,
    withCookies,
    withParams,
} from 'itty-router';

const delay = milliseconds => new Promise(resolve => {
    setTimeout(resolve, milliseconds);
});

const toHex = value => {
    let output = '';
    for (const byte of new Uint8Array(value)) {
        output += byte.toString(16).padStart(2, '0');
    }
    return output;
};

const requestPath = request => new URL(request.url).pathname;

const requestTrace = request =>
    request.executionTrace ?? request.flowTrace;

const setTraceHeader = (response, request) => {
    const trace = requestTrace(request);
    if (response instanceof Response && trace) {
        response.headers.set('x-execution-trace', trace.join(','));
    }
    return response;
};

const normalizeContent = async content => {
    if (!(content instanceof FormData)) {
        return content ?? null;
    }
    const output = {};
    for (const [ key, value ] of content.entries()) {
        const normalized = value instanceof File ? {
            name: value.name,
            type: value.type,
            size: value.size,
            text: await value.text(),
        } : value;
        if (Object.hasOwn(output, key)) {
            output[key] = Array.isArray(output[key])
                ? [ ...output[key], normalized ]
                : [ output[key], normalized ];
        } else {
            output[key] = normalized;
        }
    }
    return output;
};

const createCorsPolicies = () => {
    const policies = new Map([
        [ '/cors/wildcard', cors() ],
        [ '/cors/fixed', cors({ origin: 'https://fixed.example' }) ],
        [ '/cors/list', cors({
            origin: [ 'https://one.example', 'https://two.example' ],
        }) ],
        [ '/cors/regexp', cors({ origin: /\.trusted\.example$/ }) ],
        [ '/cors/callback', cors({
            origin: origin => origin?.endsWith('.callback.example')
                ? origin
                : undefined,
        }) ],
        [ '/cors/credentials', cors({
            origin: '*',
            credentials: true,
        }) ],
        [ '/cors/options', cors({
            origin: true,
            credentials: true,
            allowMethods: [ 'GET', 'POST', 'PATCH' ],
            allowHeaders: [ 'x-input', 'content-type' ],
            exposeHeaders: [ 'x-output' ],
            maxAge: 7200,
        }) ],
        [ '/cors/error', cors({ origin: true }) ],
        [ '/cors/missing', cors({ origin: true }) ],
    ]);

    const select = request => {
        const path = requestPath(request);
        for (const [ prefix, policy ] of policies) {
            if (path === prefix || path.startsWith(`${prefix}/`)) {
                return policy;
            }
        }
        return undefined;
    };

    return {
        before(request) {
            return select(request)?.preflight(request);
        },
        after(response, request) {
            const policy = select(request);
            return policy ? policy.corsify(response, request) : response;
        },
    };
};

const createApplicationState = () => ({
    abortedHandlers: 0,
    abortedParses: 0,
    abortedStreams: 0,
});

const registerRoutes = (router, variant, state) => {
    router.get('/entry', () => text('itty-entry-ok'));
    router.get('/variant/name', () => ({ variant }));

    for (const method of [ 'get', 'post', 'put', 'patch', 'delete', 'options' ]) {
        router[method](`/methods/${method}`, request => text(request.method));
    }
    router.head('/methods/head', request => new Response(null, {
        headers: { 'x-method': request.method },
    }));
    router.all('/methods/all', request => text(`all:${request.method}`));
    router.purge('/methods/nonstandard', request => text(
        `nonstandard:${request.method}`,
    ));

    router.get('/patterns/fixed', () => ({ pattern: 'fixed' }));
    router.get('/patterns/users/:id', request => ({
        id: request.id,
        params: request.params,
    }));
    router.get('/patterns/multiple/:team/:id', request => ({
        team: request.team,
        id: request.id,
    }));
    router.get('/patterns/optional/:id?', request => ({
        id: request.id ?? null,
    }));
    router.get('/patterns/files/:name.:ext', request => ({
        name: request.name,
        extension: request.ext,
    }));
    router.get('/patterns/wild/*', request => ({
        path: requestPath(request),
        route: request.route,
    }));
    router.get('/patterns/greedy/:path+', request => ({
        path: request.path,
    }));
    router.get('/patterns/order/:value', request => ({
        winner: 'dynamic-first',
        value: request.value,
    }));
    router.get('/patterns/order/fixed', () => ({
        winner: 'unreachable-fixed',
    }));
    router.get('/patterns/fixed-first/fixed', () => ({
        winner: 'fixed-first',
    }));
    router.get('/patterns/fixed-first/:value', request => ({
        winner: 'dynamic-second',
        value: request.value,
    }));
    router.get('/patterns/unicode/:value', request => ({
        encoded: request.value,
        decoded: decodeURIComponent(request.value),
    }));

    const basedRouter = IttyRouter({ base: '/patterns/base' });
    basedRouter.all('*', withParams);
    basedRouter.get('/item/:id', request => ({
        base: true,
        id: request.id,
    }));
    router.all('/patterns/base/*', request => basedRouter.fetch(request));

    router.get('/query/inspect', request => ({
        query: request.query,
        prototypeIsNull: Object.getPrototypeOf(request.query) === null,
    }));

    const flow = (request, event) => {
        request.flowTrace ??= [];
        request.flowTrace.push(event);
    };
    router.get(
        '/flow/continue',
        request => {
            flow(request, 'undefined');
        },
        request => {
            flow(request, 'null');
            return null;
        },
        async request => {
            flow(request, 'async:start');
            await Promise.resolve();
            flow(request, 'async:end');
        },
        request => {
            flow(request, 'terminal');
            return { trace: [ ...request.flowTrace ] };
        },
    );
    const stopRoutes = [
        [ 'response', request => {
            flow(request, 'stop:response');
            return text('response-stop');
        } ],
        [ 'object', request => {
            flow(request, 'stop:object');
            return { kind: 'object', trace: [ ...request.flowTrace ] };
        } ],
        [ 'string', request => {
            flow(request, 'stop:string');
            return 'string-stop';
        } ],
        [ 'zero', request => {
            flow(request, 'stop:zero');
            return 0;
        } ],
        [ 'false', request => {
            flow(request, 'stop:false');
            return false;
        } ],
        [ 'empty-string', request => {
            flow(request, 'stop:empty-string');
            return '';
        } ],
    ];
    for (const [ name, terminal ] of stopRoutes) {
        router.get(
            `/flow/stop/${name}`,
            request => flow(request, 'first'),
            terminal,
            request => {
                flow(request, 'unreachable');
                return { trace: request.flowTrace };
            },
        );
    }
    router.get('/flow/promise', async request => {
        flow(request, 'promise:start');
        const value = await Promise.resolve('awaited');
        flow(request, 'promise:end');
        return { value, trace: request.flowTrace };
    });
    router.get(
        '/flow/mixed',
        request => flow(request, 'sync:one'),
        async request => {
            await Promise.resolve();
            flow(request, 'async:two');
        },
        request => {
            flow(request, 'sync:three');
            return { trace: request.flowTrace };
        },
    );
    router.get('/flow/throw', request => {
        flow(request, 'throw:sync');
        throw new TypeError('sync-flow-boom');
    });
    router.get('/flow/reject', async request => {
        flow(request, 'throw:async');
        await Promise.resolve();
        throw new RangeError('async-flow-boom');
    });

    router.get('/stages/success', request => {
        request.executionTrace.push('route:sync');
        return { beforeFinally: [ ...request.executionTrace ] };
    });
    router.get('/stages/async', async request => {
        await Promise.resolve();
        request.executionTrace.push('route:async');
        return { beforeFinally: [ ...request.executionTrace ] };
    });
    router.get('/stages/before-throw', request => {
        request.executionTrace.push('unreachable');
        return { unreachable: true };
    });
    router.get('/stages/route-throw', request => {
        request.executionTrace.push('route:throw');
        throw new StatusError(422, 'route-stage-boom');
    });
    router.get('/stages/finally-throw', request => {
        request.executionTrace.push('route:before-finally-throw');
        return { route: 'completed' };
    });

    router.get(
        '/context/custom/:id',
        request => {
            request.customValue = request.headers.get('x-custom');
        },
        request => ({
            id: request.id,
            custom: request.customValue,
        }),
    );
    router.post(
        '/context/helpers/:id',
        withContent,
        withCookies,
        async request => ({
            id: request.id,
            content: await normalizeContent(request.content),
            cookies: request.cookies,
        }),
    );
    router.get('/context/isolation', request => {
        const previous = request.localValue ?? null;
        request.localValue = request.query.value ?? 'unset';
        return { previous, current: request.localValue };
    });

    const contentHandler = async request => ({
        content: await normalizeContent(request.content),
    });
    for (const name of [
        'json',
        'malformed-json',
        'text',
        'urlencoded',
        'multipart',
        'empty',
    ]) {
        router.post(`/content/${name}`, withContent, contentHandler);
    }
    router.post('/content/direct-form', async request => {
        const contentType = request.headers.get('content-type');
        try {
            return {
                contentType,
                content: await normalizeContent(await request.formData()),
            };
        } catch (cause) {
            return {
                contentType,
                error: {
                    name: cause.name,
                    message: cause.message,
                },
                bodyUsed: request.bodyUsed,
            };
        }
    });
    router.post('/content/clone-form', async request => {
        const originalContentType = request.headers.get('content-type');
        const jsonClone = request.clone();
        const jsonCloneContentType =
            jsonClone.headers.get('content-type');
        let jsonError;
        try {
            await jsonClone.json();
        } catch (cause) {
            jsonError = cause.name;
        }
        const originalAfterJson =
            request.headers.get('content-type');
        const formClone = request.clone();
        const formCloneContentType =
            formClone.headers.get('content-type');
        try {
            return {
                jsonError,
                originalContentType,
                jsonCloneContentType,
                originalAfterJson,
                formCloneContentType,
                content: await normalizeContent(
                    await formClone.formData(),
                ),
            };
        } catch (cause) {
            return {
                jsonError,
                originalContentType,
                jsonCloneContentType,
                originalAfterJson,
                formCloneContentType,
                formError: {
                    name: cause.name,
                    message: cause.message,
                },
            };
        }
    });
    router.post('/content/stream', async request => {
        const reader = request.body.getReader();
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
        return { chunks, size, checksum };
    });

    const capsidResponse = createResponse(
        'application/x-capsid',
        value => `capsid:${value}`,
    );
    router.get('/responses/json', () => json({ helper: 'json' }));
    router.get('/responses/text', () => text('text-helper'));
    router.get('/responses/html', () => html('<h1>itty</h1>'));
    router.get('/responses/jpeg', () => jpeg(new Uint8Array([ 0xff, 0xd8, 1 ])));
    router.get('/responses/png', () => png(new Uint8Array([
        0x89, 0x50, 0x4e, 0x47,
    ])));
    router.get('/responses/webp', () => webp(new Uint8Array([
        0x52, 0x49, 0x46, 0x46,
    ])));
    router.get('/responses/create', () => capsidResponse('created'));
    router.get('/responses/status', () => status(204, {
        headers: { 'x-status-helper': 'yes' },
    }));
    router.get('/responses/error', () => error(418, 'teapot'));
    router.get('/responses/status-error', () => {
        throw new StatusError(409, 'status-conflict');
    });
    router.get('/responses/custom', () => new Response('custom-response', {
        status: 207,
        headers: { 'x-custom-response': 'yes' },
    }));
    router.get('/responses/blob', () => new Response(
        new Blob([ new Uint8Array([ 0, 1, 127, 128, 255 ]) ], {
            type: 'application/octet-stream',
        }),
        { status: 206, headers: { 'x-blob': 'yes' } },
    ));
    router.get('/responses/headers', () => text('headers', {
        status: 202,
        headers: {
            'x-first': 'one',
            'x-second': 'two',
        },
    }));
    router.get('/responses/not-modified', () => status(304, {
        headers: { etag: '"itty"' },
    }));
    router.get('/responses/stream', () => {
        let index = 0;
        return new Response(new ReadableStream({
            pull(controller) {
                if (index === 20) {
                    controller.close();
                    return;
                }
                controller.enqueue(new Uint8Array(256).fill(65 + index));
                index += 1;
            },
        }), {
            headers: { 'content-type': 'application/octet-stream' },
        });
    });

    for (const name of [
        'wildcard',
        'fixed',
        'list',
        'regexp',
        'callback',
        'credentials',
        'options',
    ]) {
        router.get(`/cors/${name}`, () => ({
            cors: name,
        }));
    }
    router.get('/cors/error', () => {
        throw new StatusError(451, 'cors-error');
    });

    const child = IttyRouter({ base: '/nest' });
    child.all('*', withParams);
    child.get('/item/:id', request => ({
        id: request.id,
        parent: request.parentValue,
    }));
    const grandchild = IttyRouter({ base: '/nest/deep' });
    grandchild.all('*', withParams);
    grandchild.get('/leaf/:id', request => ({
        id: request.id,
        parent: request.parentValue,
        depth: 3,
    }));
    child.all('/deep/*', request => grandchild.fetch(request));
    router.all('/nest/*', request => {
        request.parentValue = 'parent-middleware';
    });
    router.all('/nest/*', request => child.fetch(request));

    const customAuto = AutoRouter({
        base: '/variant/auto',
        before: [ request => {
            request.customAutoBefore = 'custom-before';
        } ],
        catch(cause, request) {
            const response = error(cause instanceof Error
                ? cause
                : new Error(String(cause)));
            response.headers.set(
                'x-custom-auto-catch',
                request.customAutoBefore,
            );
            return response;
        },
        finally: [ response => {
            response.headers.set('x-custom-auto-finally', 'yes');
            return response;
        } ],
        format(value) {
            return value instanceof Response
                ? value
                : json({ customFormat: value });
        },
        missing: request => ({
            missing: requestPath(request),
        }),
    });
    customAuto.get('/item/:id', request => ({
        id: request.id,
        before: request.customAutoBefore,
    }));
    customAuto.get('/throw', () => {
        throw new StatusError(417, 'custom-auto-boom');
    });
    router.all('/variant/auto/*', request => customAuto.fetch(request));

    const defaultAuto = AutoRouter({ base: '/variant/default-auto' });
    defaultAuto.get('/object', () => ({ defaultAuto: true }));
    router.all(
        '/variant/default-auto/*',
        request => defaultAuto.fetch(request),
    );

    router.get('/runtime/globals', () => {
        const names = new Set(Object.getOwnPropertyNames(globalThis));
        const stateOf = name => names.has(name) ? 'present' : 'undefined';
        return {
            process: stateOf('process'),
            Buffer: stateOf('Buffer'),
            Deno: stateOf('Deno'),
            Bun: stateOf('Bun'),
            tjs: stateOf('tjs'),
        };
    });
    router.get('/runtime/delay', async request => {
        const milliseconds = Number(request.query.ms ?? '0');
        await delay(milliseconds);
        return { delay: milliseconds };
    });
    router.get('/runtime/wait-for-abort', request => new Promise(resolve => {
        request.signal.addEventListener('abort', () => {
            state.abortedHandlers += 1;
            resolve(text('aborted'));
        }, { once: true });
    }));
    router.post('/runtime/cancel-parse', async request => {
        request.signal.addEventListener('abort', () => {
            state.abortedParses += 1;
        }, { once: true });
        const reader = request.body.getReader();
        try {
            for (;;) {
                const result = await reader.read();
                if (result.done) {
                    break;
                }
            }
        } catch {
            // The host cancellation owns the response lifecycle.
        }
        return text('parse-finished');
    });
    router.get('/runtime/abort-count', () => ({
        handlers: state.abortedHandlers,
        parses: state.abortedParses,
        streams: state.abortedStreams,
    }));
    router.get('/runtime/stream-cancel', () => {
        let index = 0;
        return new Response(new ReadableStream({
            async pull(controller) {
                controller.enqueue(new Uint8Array(256).fill(
                    65 + (index % 26),
                ));
                index += 1;
                await delay(1);
            },
            cancel() {
                state.abortedStreams += 1;
            },
        }));
    });
    router.get('/runtime/concurrent/:param', async request => {
        const token = request.query.token;
        request.content = { token, source: 'request-content' };
        request.cookies = { token, source: 'request-cookie' };
        const milliseconds = Number(request.query.delay ?? '0');
        await delay(milliseconds);
        return {
            param: request.param,
            query: request.query,
            content: request.content,
            cookies: request.cookies,
        };
    });
    router.get('/runtime/context-probe/:param', request => ({
        param: request.param,
        query: request.query,
        content: request.content ?? null,
        cookies: request.cookies ?? null,
    }));
    router.get('/runtime/cpu-timeout', () => {
        for (;;) {
            // Deliberately exercise the runtime interrupt deadline.
        }
    });
    router.get('/runtime/ownership', async () => {
        console.log('capsid-owner:before');
        await Promise.resolve();
        console.log('capsid-owner:after');
        return { ownership: 'ok' };
    });
    router.get('/runtime/ownership-cancel', () => {
        console.log('capsid-owner:start');
        setTimeout(() => console.log('capsid-owner:after-cancel'), 80);
        return new Promise(() => {});
    });
    router.get('/runtime/fetch', async request => {
        const response = await fetch(request.query.url);
        return {
            status: response.status,
            header: response.headers.get('x-itty-upstream'),
            body: await response.text(),
        };
    });
};

export const createApplication = variant => {
    const state = createApplicationState();
    const corsPolicies = createCorsPolicies();

    const beforeSync = request => {
        if (!requestPath(request).startsWith('/stages/')) {
            return;
        }
        request.executionTrace = [ 'before:sync' ];
        if (requestPath(request) === '/stages/before-throw') {
            throw new TypeError('before-stage-boom');
        }
    };
    const beforeAsync = async request => {
        if (!request.executionTrace) {
            return;
        }
        await Promise.resolve();
        request.executionTrace.push('before:async');
    };
    const catchHandler = (cause, request) => {
        const normalized = cause instanceof Error
            ? cause
            : new Error(String(cause));
        const classification = normalized instanceof StatusError
            ? 'StatusError'
            : normalized.name ?? 'Error';
        requestTrace(request)?.push(`catch:${classification}`);
        const response = error(normalized);
        response.headers.set('x-error-class', classification);
        return setTraceHeader(response, request);
    };
    const missing = request => error(404, {
        error: 'missing',
        path: requestPath(request),
    });
    const ensureResponse = (response, request) =>
        response ?? missing(request);
    const finalizer = async (response, request) => {
        if (request.executionTrace) {
            await Promise.resolve();
            request.executionTrace.push('finally:async');
            if (
                requestPath(request) === '/stages/finally-throw' &&
                !request.finallyThrew
            ) {
                request.finallyThrew = true;
                throw new StatusError(598, 'finally-stage-boom');
            }
        }
        return setTraceHeader(response, request);
    };

    let router;
    let fetchHandler;
    if (variant === 'autorouter') {
        router = AutoRouter({
            before: [ beforeSync, beforeAsync, corsPolicies.before ],
            catch: catchHandler,
            finally: [ finalizer, corsPolicies.after ],
            format: json,
            missing,
        });
        fetchHandler = router.fetch;
    } else if (variant === 'router') {
        router = Router({
            before: [
                withParams,
                beforeSync,
                beforeAsync,
                corsPolicies.before,
            ],
            catch: catchHandler,
            finally: [
                ensureResponse,
                json,
                finalizer,
                corsPolicies.after,
            ],
        });
        fetchHandler = router.fetch;
    } else if (variant === 'itty-router') {
        router = IttyRouter();
        router.all(
            '*',
            withParams,
            beforeSync,
            beforeAsync,
            corsPolicies.before,
        );
        fetchHandler = (request, ...args) => router
            .fetch(request, ...args)
            .then(response => ensureResponse(response, request))
            .then(json)
            .catch(cause => catchHandler(cause, request))
            .then(response => finalizer(response, request))
            .catch(cause => catchHandler(cause, request))
            .then(response => corsPolicies.after(response, request));
    } else {
        throw new TypeError(`unknown itty-router variant: ${variant}`);
    }

    registerRoutes(router, variant, state);
    return { router, fetch: fetchHandler };
};
