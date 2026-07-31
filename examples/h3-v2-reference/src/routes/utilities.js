import {
    HTTPError,
    basicAuth,
    clearSession,
    createEventStream,
    deleteCookie,
    getCookie,
    handleCacheHeaders,
    handleCors,
    proxy,
    requireBasicAuth,
    serveStatic,
    setCookie,
    useSession,
} from 'h3/generic';
import { memoryStaticOptions } from '../assets/memory-assets.js';

const corsPolicies = {
    wildcard: {
        origin: '*',
        methods: '*',
        allowHeaders: '*',
        exposeHeaders: '*',
        maxAge: '600',
    },
    fixed: {
        origin: [ 'https://fixed.example' ],
        methods: [ 'GET', 'POST', 'QUERY' ],
        allowHeaders: [ 'x-one', 'content-type' ],
        exposeHeaders: [ 'x-visible' ],
        maxAge: '1200',
    },
    list: {
        origin: [ 'https://one.example', 'https://two.example' ],
    },
    regexp: {
        origin: [ /^https:\/\/[a-z]+\.trusted\.example$/ ],
    },
    callback: {
        origin: origin => origin.endsWith('.callback.example'),
    },
    credentials: {
        origin: [ 'https://credential.example' ],
        methods: [ 'GET', 'POST' ],
        allowHeaders: [ 'authorization', 'content-type' ],
        exposeHeaders: [ 'x-visible' ],
        credentials: true,
        maxAge: '60',
    },
};

const sessionConfig = {
    name: 'capsid-session',
    password: 'capsid-runtime-h3-v2-session-password-0001',
    generateId: () => 'capsid-session-id',
    cookie: {
        secure: false,
        httpOnly: true,
        sameSite: 'lax',
        path: '/',
    },
};

export const installUtilityRoutes = app => {
    app.get('/utilities/cookies', event => ({
        alpha: getCookie(event, 'alpha') ?? null,
        empty: getCookie(event, 'empty') ?? null,
        missing: getCookie(event, 'missing') ?? null,
    }));
    app.get('/utilities/cookies/set', event => {
        setCookie(event, 'first', 'one', {
            httpOnly: true,
            path: '/',
            sameSite: 'lax',
        });
        setCookie(event, 'second', 'two', {
            maxAge: 60,
            path: '/utilities',
        });
        deleteCookie(event, 'obsolete', { path: '/' });
        return 'cookies-set';
    });

    app.all('/utilities/cors/:policy', event => {
        const policy = corsPolicies[event.context.params?.policy];
        if (!policy) {
            throw new HTTPError({
                status: 404,
                message: 'unknown-cors-policy',
            });
        }
        const handled = handleCors(event, policy);
        if (handled !== false) {
            return handled;
        }
        event.res.headers.set('x-visible', 'cors-visible');
        return {
            policy: event.context.params?.policy,
            method: event.req.method,
        };
    });
    app.get('/utilities/cors-error', event => {
        handleCors(event, corsPolicies.fixed);
        throw new HTTPError({
            status: 418,
            message: 'cors-error',
        });
    });

    app.get(
        '/utilities/auth/middleware',
        event => ({
            username: event.context.basicAuth?.username,
            realm: event.context.basicAuth?.realm,
        }),
        {
            middleware: [ basicAuth({
                username: 'capsid',
                password: 'secret',
                realm: 'capsid-runtime',
            }) ],
        },
    );
    app.get('/utilities/auth/required', async event => {
        await requireBasicAuth(event, {
            password: 'secret',
            realm: 'capsid-required',
        });
        return {
            username: event.context.basicAuth?.username,
            realm: event.context.basicAuth?.realm,
        };
    });

    app.get('/utilities/session', async event => {
        const session = await useSession(event, sessionConfig);
        const count = Number(session.data.count ?? 0) + 1;
        await session.update({ count });
        return {
            id: session.id,
            count: session.data.count,
        };
    });
    app.get('/utilities/session/clear', async event => {
        await clearSession(event, sessionConfig);
        return 'session-cleared';
    });

    app.get('/utilities/cache', event => {
        const matched = handleCacheHeaders(event, {
            etag: '"h3-cache-v1"',
            modifiedTime: '2024-01-02T03:04:05.000Z',
            maxAge: 60,
            cacheControls: [ 'stale-while-revalidate=30' ],
        });
        return matched ? undefined : 'cache-body';
    });

    app.get('/utilities/static/**', event =>
        serveStatic(event, memoryStaticOptions));
    app.head('/utilities/static/**', event =>
        serveStatic(event, memoryStaticOptions));

    app.all('/utilities/proxy', event => {
        const target = event.url.searchParams.get('url');
        if (!target) {
            throw new HTTPError({
                status: 400,
                message: 'missing-proxy-target',
            });
        }
        return proxy(event, target, {
            headers: { 'x-h3-proxy': 'yes' },
        });
    });

    app.get('/utilities/sse', event => {
        const stream = createEventStream(event);
        queueMicrotask(async () => {
            await stream.pushComment('capsid-h3');
            await stream.push({
                id: '1',
                event: 'message',
                data: 'first',
            });
            await stream.push({
                id: '2',
                data: 'second',
                retry: 1500,
            });
            await stream.close();
        });
        return stream.send();
    });
};
