import {
    H3,
    HTTPError,
    defineHandler,
    defineLazyEventHandler,
    defineRoute,
    fromWebHandler,
    onError,
    onRequest,
    onResponse,
    withBase,
} from 'h3/generic';
import { delay, traceFor } from '../shared.js';

const localTrace = event => traceFor(event, 'local');

export const installCoreRoutes = (app, state) => {
    app.on('GET', '/core/on', () => 'core:on');
    app.get('/core/get', () => 'core:get');
    app.post('/core/post', () => 'core:post');
    app.put('/core/put', () => 'core:put');
    app.patch('/core/patch', () => 'core:patch');
    app.delete('/core/delete', () => 'core:delete');
    app.head('/core/head', event => {
        event.res.headers.set('x-h3-method', 'HEAD');
        return 'head-body-must-be-removed';
    });
    app.options('/core/options', () => 'core:options');
    app.query('/core/query', event => {
        event.res.headers.set('accept-query', 'application/json');
        return 'core:query';
    });
    app.on('PURGE', '/core/custom', () => 'core:PURGE');
    app.all('/core/all', event => `core:all:${event.req.method}`);

    app.get('/routing/fixed', () => 'routing:fixed');
    app.get('/routing/users/:id', event => ({
        id: event.context.params?.id,
    }));
    app.get('/routing/teams/:team/users/:user', event => ({
        team: event.context.params?.team,
        user: event.context.params?.user,
    }));
    app.get('/routing/wild/**', event => ({
        path: event.url.pathname,
    }));
    app.get('/routing/named/**:rest', event => ({
        rest: event.context.params?.rest,
    }));
    app.get('/routing/precedence/:value', () => 'dynamic-first');
    app.get('/routing/precedence/fixed', () => 'fixed-wins');
    app.get('/routing/order/:first', event => ({
        route: 'first',
        value: event.context.params?.first,
    }));
    app.get('/routing/order/:second', event => ({
        route: 'second',
        value: event.context.params?.second,
    }));
    app.get('/routing/trailing/', () => 'trailing-slash');
    app.get('/routing/unicode/:value', event => ({
        raw: event.context.params?.value,
        decoded: decodeURIComponent(event.context.params?.value ?? ''),
        pathname: event.url.pathname,
    }));
    app.get('/routing/encoded/:value', event => ({
        raw: event.context.params?.value,
    }));

    app.get(
        '/event/inspect/:id',
        defineHandler({
            meta: {
                source: 'handler-meta',
                stable: true,
            },
            handler: event => {
                event.res.status = 207;
                event.res.statusText = 'H3 Event';
                event.res.headers.set('x-event-prepared', 'yes');
                return {
                    reqMethod: event.req.method,
                    reqUrl: event.req.url,
                    urlPath: event.url.pathname,
                    contextId: event.context.params?.id,
                    appMatches: event.app === app,
                    matchedMethod: event.context.matchedRoute?.method,
                    matchedRoute: event.context.matchedRoute?.route,
                    meta: event.context.matchedRoute?.meta,
                    responseStatus: event.res.status,
                    responseStatusText: event.res.statusText,
                    responseHeader:
                        event.res.headers.get('x-event-prepared'),
                    requestWaitUntil:
                        typeof event.req.waitUntil,
                };
            },
        }),
    );

    app.use(
        '/event/middleware/:scope/**',
        (event, next) => {
            event.context.middlewareSeen = {
                ...event.context.middlewareParams,
            };
            return next();
        },
        { method: 'POST' },
    );
    app.post('/event/middleware/:scope/value', event => ({
        routeParams: event.context.params,
        middlewareParams: event.context.middlewareParams,
        captured: event.context.middlewareSeen,
    }));
    app.get('/event/middleware/:scope/value', event => ({
        middlewareSeen: event.context.middlewareSeen ?? null,
    }));
    app.post('/event/unfiltered/value', event => ({
        middlewareSeen: event.context.middlewareSeen ?? null,
    }));

    app.get(
        '/middleware/continue',
        () => undefined,
        {
            middleware: [
                event => {
                    localTrace(event).push('undefined');
                },
                event => {
                    localTrace(event).push('async:start');
                    return Promise.resolve().then(() => {
                        localTrace(event).push('async:end');
                    });
                },
            ],
        },
    );
    app.get(
        '/middleware/continue-result',
        event => ({
            trace: localTrace(event),
        }),
        {
            middleware: [
                event => {
                    localTrace(event).push('sync');
                },
                async event => {
                    await Promise.resolve();
                    localTrace(event).push('async');
                },
                (event, next) => {
                    localTrace(event).push('next');
                    return next();
                },
            ],
        },
    );
    app.get(
        '/middleware/wrap',
        event => {
            localTrace(event).push('handler');
            return { phase: 'handler' };
        },
        {
            middleware: [
                async (event, next) => {
                    localTrace(event).push('outer:before');
                    const value = await next();
                    localTrace(event).push('outer:after');
                    return {
                        value,
                        trace: [ ...localTrace(event) ],
                    };
                },
                async (event, next) => {
                    localTrace(event).push('inner:before');
                    const value = await next();
                    localTrace(event).push('inner:after');
                    return value;
                },
            ],
        },
    );
    app.get(
        '/middleware/intercept',
        () => 'unreachable',
        {
            middleware: [
                event => {
                    localTrace(event).push('intercept');
                    return {
                        intercepted: true,
                        trace: localTrace(event),
                    };
                },
            ],
        },
    );
    app.get(
        '/middleware/null',
        () => 'unreachable',
        { middleware: [ () => null ] },
    );
    app.get(
        '/middleware/response',
        () => 'unreachable',
        {
            middleware: [
                () => new Response('middleware-response', {
                    status: 202,
                    headers: { 'x-middleware-stop': 'response' },
                }),
            ],
        },
    );
    app.get(
        '/middleware/duplicate-next',
        event => {
            state.duplicateNextHandlers += 1;
            localTrace(event).push('handler');
            return 'once';
        },
        {
            middleware: [
                async (event, next) => {
                    localTrace(event).push('before');
                    const first = next();
                    const second = next();
                    const values = await Promise.all([ first, second ]);
                    localTrace(event).push('after');
                    return {
                        samePromise: first === second,
                        values,
                        handlers: state.duplicateNextHandlers,
                        trace: localTrace(event),
                    };
                },
            ],
        },
    );
    app.get(
        '/middleware/throw',
        () => 'unreachable',
        {
            middleware: [ () => {
                throw new HTTPError({
                    status: 409,
                    message: 'middleware-sync-boom',
                });
            } ],
        },
    );
    app.get(
        '/middleware/reject',
        () => 'unreachable',
        {
            middleware: [ async () => {
                await Promise.resolve();
                throw new HTTPError({
                    status: 410,
                    message: 'middleware-async-boom',
                });
            } ],
        },
    );

    app.get(
        '/hooks/factory-success',
        event => {
            traceFor(event, 'factory').push('handler');
            return 'factory-success';
        },
        {
            middleware: [
                onRequest(event => {
                    traceFor(event, 'factory').push('onRequest');
                }),
                onResponse((response, event) => {
                    traceFor(event, 'factory').push('onResponse');
                    response.headers.set(
                        'x-h3-factory-trace',
                        traceFor(event, 'factory').join(','),
                    );
                }),
            ],
        },
    );
    app.get(
        '/hooks/factory-error',
        () => {
            throw new HTTPError({
                status: 418,
                message: 'factory-error',
            });
        },
        {
            middleware: [
                onError((error, event) => {
                    traceFor(event, 'factory').push(
                        `onError:${error.status}`,
                    );
                    return new Response('factory-error-handled', {
                        status: 419,
                        headers: {
                            'x-h3-factory-trace':
                                traceFor(event, 'factory').join(','),
                        },
                    });
                }),
            ],
        },
    );

    const childHookState = {
        request: 0,
        response: 0,
        error: 0,
    };
    const child = new H3({
        onRequest() {
            childHookState.request += 1;
        },
        onResponse() {
            childHookState.response += 1;
        },
        onError() {
            childHookState.error += 1;
        },
    });
    child.use((event, next) => {
        event.context.childMiddleware = true;
        return next();
    });
    child.get('/item/:id', event => ({
        id: event.context.params?.id,
        childMiddleware: event.context.childMiddleware,
        childHooks: { ...childHookState },
        pathname: event.url.pathname,
    }));
    const grandchild = new H3();
    grandchild.get('/leaf/:id', event => ({
        id: event.context.params?.id,
        level: 3,
        pathname: event.url.pathname,
    }));
    child.mount('/grand', grandchild);
    app.mount('/composition/child', child);

    app.mount('/composition/web', {
        fetch(request) {
            const url = new URL(request.url);
            return new Response(`web:${url.pathname}${url.search}`, {
                status: 206,
                headers: { 'x-mounted-web': 'yes' },
            });
        },
    });
    app.get(
        '/composition/from-web/:id',
        fromWebHandler(async (request, context) => new Response(
            JSON.stringify({
                id: context.params?.id,
                pathname: new URL(request.url).pathname,
            }),
            {
                headers: { 'content-type': 'application/json' },
            },
        )),
    );

    const handlerChild = new H3();
    handlerChild.get('/item/:id', event => ({
        handler: true,
        id: event.context.params?.id,
        pathname: event.url.pathname,
    }));
    app.use(
        '/composition/handler/**',
        withBase('/composition/handler', handlerChild.handler),
    );

    app.register(defineRoute({
        method: 'GET',
        route: '/plugins/defined-route/:id',
        meta: { source: 'defineRoute' },
        handler: event => ({
            id: event.context.params?.id,
            meta: event.context.matchedRoute?.meta,
        }),
    }));

    const lazy = defineLazyEventHandler(async () => {
        state.lazyInitializations += 1;
        await delay(20);
        return event => ({
            initializations: state.lazyInitializations,
            token: event.url.searchParams.get('token'),
        });
    });
    app.get('/plugins/lazy', lazy);
};
