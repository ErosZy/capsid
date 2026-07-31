import { H3, onDispose } from 'h3/generic';
import {
    configurablePlugin,
    constructorPlugin,
} from './plugins/reference-plugins.js';
import { installCoreRoutes } from './routes/core.js';
import {
    installRequestResponseRoutes,
} from './routes/request-response.js';
import { installRuntimeRoutes } from './routes/runtime.js';
import { installUtilityRoutes } from './routes/utilities.js';

export const createApplication = () => {
    const state = {
        pluginOrder: [],
        lazyInitializations: 0,
        duplicateNextHandlers: 0,
        activeContexts: new Set(),
        abortCounts: {
            handlers: 0,
            handlerSignals: 0,
            parses: 0,
            streams: 0,
            timeouts: 0,
        },
        disposeCounts: {
            total: 0,
            success: 0,
            error: 0,
            cancel: 0,
            cancelParse: 0,
            stream: 0,
            timeout: 0,
            abortReasons: 0,
            successReason: null,
            errorReason: null,
        },
    };
    const app = new H3({
        silent: true,
        plugins: [ constructorPlugin(state) ],
        onRequest(event) {
            event.context.hookTrace = [ 'onRequest' ];
        },
        onError(error, event) {
            event.context.hookTrace?.push(`onError:${error.status}`);
            if (event.url.pathname === '/errors/hook-handled') {
                return new Response('global-hook-handled', {
                    status: 454,
                    statusText: 'Hook Handled',
                    headers: { 'x-hook-handled': 'yes' },
                });
            }
            if (event.url.pathname === '/errors/onerror-failure') {
                throw new Error('bounded-onerror-failure');
            }
            return undefined;
        },
        onResponse(response, event) {
            event.context.hookTrace?.push('onResponse');
            response.headers.set(
                'x-h3-hook-trace',
                event.context.hookTrace?.join(',') ?? '',
            );
            response.headers.set(
                'x-h3-global-middleware',
                event.context.globalTrace?.join(',') ?? '',
            );
            if (event.url.pathname === '/errors/onresponse-failure') {
                throw new Error('bounded-onresponse-failure');
            }
        },
    });

    app.use(async (event, next) => {
        const marker = {};
        state.activeContexts.add(marker);
        event.context.globalTrace = [ 'global:before' ];
        onDispose(event, () => {
            state.activeContexts.delete(marker);
            state.disposeCounts.total += 1;
        });
        try {
            return await next();
        } finally {
            event.context.globalTrace.push('global:after');
        }
    });

    app.register(configurablePlugin({
        name: 'registered-one',
        state,
    }));
    app.register(configurablePlugin({
        name: 'registered-two',
        state,
    }));

    app.get('/entry', () => 'h3-entry-ok');
    installCoreRoutes(app, state);
    installRequestResponseRoutes(app);
    installUtilityRoutes(app);
    installRuntimeRoutes(app, state);

    return app;
};

export const createMalformedApplication = () => {
    const app = new H3({
        allowMalformedURL: true,
        silent: true,
    });
    app.all('/**:path', event => ({
        rawUrl: event.req.url,
        pathname: event.url.pathname,
        path: event.context.params?.path,
    }));
    return app;
};

export const createDebugApplication = () => {
    const app = new H3({
        debug: true,
        silent: true,
    });
    app.get('/error', () => {
        throw new Error('debug-visible-message');
    });
    return app;
};
