import { HTTPError, onDispose } from 'h3/generic';
import { bodyChecksum, delay } from '../shared.js';

export const installRuntimeRoutes = (app, state) => {
    app.get('/runtime/globals', () => {
        const typeOfGlobal = name =>
            typeof Reflect.get(globalThis, name);
        return {
            process: typeOfGlobal('process'),
            Buffer: typeOfGlobal('Buffer'),
            Deno: typeOfGlobal('Deno'),
            Bun: typeOfGlobal('Bun'),
            tjs: typeOfGlobal('tjs'),
            ExecutionContext: typeOfGlobal('ExecutionContext'),
        };
    });
    app.get('/runtime/abort-signal', event => ({
        isAbortSignal: event.req.signal instanceof AbortSignal,
        aborted: event.req.signal.aborted,
        reason: event.req.signal.reason ?? null,
    }));
    app.get('/runtime/outbound', async event => {
        const target = event.url.searchParams.get('url');
        if (!target) {
            throw new HTTPError({
                status: 400,
                message: 'missing-outbound-target',
            });
        }
        const response = await fetch(target, {
            headers: { 'x-h3-outbound': 'direct' },
            signal: event.req.signal,
        });
        return {
            status: response.status,
            statusText: response.statusText,
            contentType: response.headers.get('content-type'),
            upstream: response.headers.get('x-h3-upstream'),
            body: await response.text(),
        };
    });
    app.get('/runtime/outbound-denied', async event => {
        const target = event.url.searchParams.get('url');
        if (!target) {
            throw new HTTPError({
                status: 400,
                message: 'missing-outbound-target',
            });
        }
        try {
            const response = await fetch(target, {
                signal: event.req.signal,
            });
            return {
                allowed: true,
                status: response.status,
            };
        } catch (error) {
            return {
                allowed: false,
                name: error?.name ?? null,
                message: String(error?.message ?? error),
            };
        }
    });
    app.post('/runtime/large-body', async event => {
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
    app.get('/runtime/concurrent/:id', async event => {
        const token = event.url.searchParams.get('token');
        const wait = Number(event.url.searchParams.get('delay') ?? 0);
        event.context.concurrent = {
            id: event.context.params?.id,
            token,
        };
        await delay(wait);
        return {
            id: event.context.params?.id,
            token,
            context: event.context.concurrent,
            query: Object.fromEntries(event.url.searchParams.entries()),
        };
    });
    app.get('/runtime/context-probe/:phase', event => ({
        phase: event.context.params?.phase,
        concurrent: event.context.concurrent ?? null,
        middleware: event.context.middlewareSeen ?? null,
        activeContexts: state.activeContexts.size,
    }));

    app.get('/runtime/wait-for-abort', event => {
        state.abortCounts.handlers += 1;
        onDispose(event, reason => {
            state.disposeCounts.cancel += 1;
            if (reason?.name === 'AbortError') {
                state.disposeCounts.abortReasons += 1;
            }
        });
        return new Promise(resolve => {
            if (event.req.signal.aborted) {
                resolve('already-aborted');
                return;
            }
            event.req.signal.addEventListener('abort', () => {
                state.abortCounts.handlerSignals += 1;
                resolve('handler-aborted');
            }, { once: true });
        });
    });
    app.post('/runtime/cancel-parse', async event => {
        onDispose(event, () => {
            state.disposeCounts.cancelParse += 1;
        });
        try {
            await event.req.text();
            return 'parse-complete';
        } catch (error) {
            if (
                event.req.signal.aborted ||
                error?.name === 'AbortError'
            ) {
                state.abortCounts.parses += 1;
                return 'parse-aborted';
            }
            throw error;
        }
    });
    app.get('/runtime/stream-cancel', event => {
        onDispose(event, reason => {
            state.disposeCounts.stream += 1;
            if (reason?.name === 'AbortError') {
                state.disposeCounts.abortReasons += 1;
            }
        });
        let pendingPull;
        return new ReadableStream({
            start(controller) {
                controller.enqueue(
                    new TextEncoder().encode('stream-start'),
                );
            },
            pull() {
                return new Promise(resolve => {
                    pendingPull = resolve;
                });
            },
            cancel() {
                state.abortCounts.streams += 1;
                pendingPull?.();
            },
        });
    });
    app.get('/runtime/abort-counts', () => ({
        ...state.abortCounts,
        dispose: { ...state.disposeCounts },
    }));

    app.get('/runtime/dispose/success', event => {
        onDispose(event, reason => {
            state.disposeCounts.success += 1;
            state.disposeCounts.successReason =
                reason?.name ?? null;
        });
        return 'dispose-success';
    });
    app.get('/runtime/dispose/error', event => {
        onDispose(event, reason => {
            state.disposeCounts.error += 1;
            state.disposeCounts.errorReason =
                reason?.name ?? null;
        });
        throw new HTTPError({
            status: 432,
            message: 'dispose-error',
        });
    });
    app.get('/runtime/dispose/counts', () => ({
        ...state.disposeCounts,
        activeContexts: state.activeContexts.size,
    }));

    app.get('/runtime/delay', event => new Promise((resolve, reject) => {
        const signal = event.req.signal;
        const milliseconds = Number(
            event.url.searchParams.get('ms') ?? 0,
        );
        onDispose(event, () => {
            state.disposeCounts.timeout += 1;
        });
        const onAbort = () => {
            state.abortCounts.timeouts += 1;
            clearTimeout(timer);
            signal.removeEventListener('abort', onAbort);
            reject(signal.reason);
        };
        const timer = setTimeout(() => {
            signal.removeEventListener('abort', onAbort);
            resolve('delay-complete');
        }, milliseconds);
        if (signal.aborted) {
            onAbort();
        } else {
            signal.addEventListener('abort', onAbort, { once: true });
        }
    }));
    app.get('/runtime/cpu-timeout', () => {
        for (;;) {
            // Exercised under the host's synchronous CPU deadline.
        }
    });
    app.get('/runtime/ownership', async () => {
        console.log('capsid-owner:before');
        await Promise.resolve();
        console.log('capsid-owner:after');
        return 'ownership-ok';
    });
    app.get('/runtime/ownership-cancel', event => {
        console.log('capsid-owner:start');
        setTimeout(() => console.log('capsid-owner:after-cancel'), 80);
        return new Promise(() => {});
    });
};
