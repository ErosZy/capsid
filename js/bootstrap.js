// Import order follows the dependencies between Web Platform APIs.
import 'txiki-polyfills/self.js';
import 'txiki-polyfills/timers.js';
import 'txiki-polyfills/event-target-polyfill.js';
import './dom-exception.js';
import 'txiki-polyfills/structured-clone.js';
import './message-channel.js';
import 'txiki-polyfills/text-encoding.js';
import 'txiki-polyfills/text-encode-transform.js';
import './text-decoder.js';
import 'txiki-polyfills/url.js';
import './url-search-params.js';
import './url.js';
import 'txiki-polyfills/blob.js';
import 'txiki-polyfills/file.js';
import './file.js';
import 'txiki-polyfills/form-data.js';
import 'txiki-polyfills/abort-controller.js';
import 'txiki-polyfills/fetch/polyfill.js';
import { configureFetchLimits, getFastResponseBody } from './fetch.js';
import 'txiki-polyfills/crypto/crypto.js';
import { CryptoKey } from 'txiki-polyfills/crypto/crypto-key.js';
import { SubtleCrypto } from 'txiki-polyfills/crypto/subtle.js';
import '../vendor/txiki.js/node_modules/web-streams-polyfill/dist/polyfill.js';
import 'txiki-polyfills/compression-streams.js';
import 'txiki-polyfills/wasm.js';
import './wasm.js';

import core from 'tjs:internal/core';
import { profileGlobalNames } from './profile-manifest.js';

configureFetchLimits(
    core.capsidFetchRequestBodyLimit(),
    core.capsidFetchResponseBodyLimit(),
);

const nativeCrypto = globalThis.crypto;
let cryptoObject;
class Crypto {
    constructor() {
        throw new TypeError('Illegal constructor');
    }

    get subtle() {
        if (this !== cryptoObject) {
            throw new TypeError('Illegal invocation');
        }
        return nativeCrypto.subtle;
    }

    getRandomValues(value) {
        if (this !== cryptoObject) {
            throw new TypeError('Illegal invocation');
        }
        const integerTypedArray =
            value instanceof Int8Array ||
            value instanceof Uint8Array ||
            value instanceof Uint8ClampedArray ||
            value instanceof Int16Array ||
            value instanceof Uint16Array ||
            value instanceof Int32Array ||
            value instanceof Uint32Array ||
            value instanceof BigInt64Array ||
            value instanceof BigUint64Array;
        if (!integerTypedArray) {
            throw new DOMException(
                'The provided ArrayBufferView is not an integer typed array',
                'TypeMismatchError');
        }
        if (value.byteLength > 65536) {
            throw new DOMException(
                'The requested length exceeds 65536 bytes',
                'QuotaExceededError');
        }
        return nativeCrypto.getRandomValues(value);
    }

    randomUUID() {
        if (this !== cryptoObject) {
            throw new TypeError('Illegal invocation');
        }
        return nativeCrypto.randomUUID();
    }
}
cryptoObject = Object.freeze(Object.create(Crypto.prototype));
Object.defineProperties(globalThis, {
    Crypto: {
        configurable: true,
        writable: true,
        value: Crypto,
    },
    CryptoKey: {
        configurable: true,
        writable: true,
        value: CryptoKey,
    },
    SubtleCrypto: {
        configurable: true,
        writable: true,
        value: SubtleCrypto,
    },
    crypto: {
        configurable: true,
        enumerable: true,
        writable: true,
        value: cryptoObject,
    },
});

const nativePerformance = globalThis.performance;
let performanceObject;
class Performance {
    constructor() {
        throw new TypeError('Illegal constructor');
    }

    get timeOrigin() {
        if (this !== performanceObject) {
            throw new TypeError('Illegal invocation');
        }
        return nativePerformance.timeOrigin;
    }

    now() {
        if (this !== performanceObject) {
            throw new TypeError('Illegal invocation');
        }
        return nativePerformance.now();
    }

    toJSON() {
        return {
            timeOrigin: this.timeOrigin,
        };
    }
}
Object.setPrototypeOf(Performance.prototype, EventTarget.prototype);
performanceObject = Object.freeze(Object.create(Performance.prototype));
EventTarget.prototype.__init.call(performanceObject);
Object.defineProperties(globalThis, {
    Performance: {
        configurable: true,
        writable: true,
        value: Performance,
    },
    performance: {
        configurable: true,
        enumerable: true,
        writable: true,
        value: performanceObject,
    },
});

const formatLogValue = value => {
    if (typeof value === 'string') {
        return value;
    }
    try {
        return JSON.stringify(value);
    } catch {
        return String(value);
    }
};

const consolePrototype = Object.create(Object.prototype);
const consoleObject = Object.create(consolePrototype);
const consoleCounts = new Map();
const consoleTimers = new Map();
let consoleGroupDepth = 0;

const emitConsole = (level, values) => {
    const prefix = '  '.repeat(consoleGroupDepth);
    core.capsidLog(level, prefix + values.map(formatLogValue).join(' '));
};

for (const level of [ 'debug', 'error', 'info', 'log', 'warn' ]) {
    Object.defineProperty(consoleObject, level, {
        enumerable: true,
        configurable: false,
        writable: false,
        value(...values) {
            emitConsole(level, values);
        },
    });
}

const defineConsoleMethod = (name, implementation) => {
    Object.defineProperty(consoleObject, name, {
        enumerable: true,
        configurable: false,
        writable: false,
        value: implementation,
    });
};

defineConsoleMethod('assert', (condition = false, ...values) => {
    if (!condition) {
        const prefix = values.length > 0 && typeof values[0] === 'string'
            ? `Assertion failed: ${values.shift()}`
            : 'Assertion failed';
        emitConsole('error', [ prefix, ...values ]);
    }
});
defineConsoleMethod('clear', () => {
    consoleGroupDepth = 0;
    emitConsole('clear', []);
});
defineConsoleMethod('count', (label = 'default') => {
    label = String(label);
    const count = (consoleCounts.get(label) ?? 0) + 1;
    consoleCounts.set(label, count);
    emitConsole('log', [ `${label}: ${count}` ]);
});
defineConsoleMethod('countReset', (label = 'default') => {
    label = String(label);
    if (!consoleCounts.delete(label)) {
        emitConsole('warn', [ `countReset: No counter named ${label}` ]);
    }
});
defineConsoleMethod('dir', (item, _options) => {
    emitConsole('log', [ item ]);
});
defineConsoleMethod('dirxml', (...values) => {
    emitConsole('log', values);
});
defineConsoleMethod('group', (...values) => {
    if (values.length > 0) {
        emitConsole('log', values);
    }
    consoleGroupDepth += 1;
});
defineConsoleMethod('groupCollapsed', (...values) => {
    consoleObject.group(...values);
});
defineConsoleMethod('groupEnd', () => {
    consoleGroupDepth = Math.max(0, consoleGroupDepth - 1);
});
defineConsoleMethod('table', (data, properties) => {
    if (properties !== undefined && !Array.isArray(properties)) {
        throw new TypeError('console.table properties must be an array');
    }
    emitConsole('log', [ data ]);
});
defineConsoleMethod('time', (label = 'default') => {
    label = String(label);
    if (consoleTimers.has(label)) {
        emitConsole('warn', [ `Timer ${label} already exists` ]);
        return;
    }
    consoleTimers.set(label, performance.now());
});
defineConsoleMethod('timeLog', (label = 'default', ...values) => {
    label = String(label);
    const start = consoleTimers.get(label);
    if (start === undefined) {
        emitConsole('warn', [ `timeLog: No such timer: ${label}` ]);
        return;
    }
    emitConsole('log', [ `${label}: ${performance.now() - start}ms`, ...values ]);
});
defineConsoleMethod('timeEnd', (label = 'default') => {
    label = String(label);
    const start = consoleTimers.get(label);
    if (start === undefined) {
        emitConsole('warn', [ `timeEnd: No such timer: ${label}` ]);
        return;
    }
    consoleTimers.delete(label);
    emitConsole('log', [ `${label}: ${performance.now() - start}ms` ]);
});
defineConsoleMethod('trace', (...values) => {
    const stack = new Error().stack;
    emitConsole('error', [
        `Trace: ${values.map(formatLogValue).join(' ')}`,
        stack,
    ]);
});

Object.defineProperty(globalThis, 'console', {
    enumerable: false,
    configurable: true,
    writable: true,
    value: Object.freeze(consoleObject),
});

Object.defineProperty(globalThis, 'navigator', {
    enumerable: true,
    configurable: false,
    writable: false,
    value: Object.freeze({ userAgent: 'capsid-runtime' }),
});

Object.defineProperty(globalThis, 'reportError', {
    enumerable: true,
    configurable: true,
    writable: true,
    value(error) {
        if (arguments.length === 0) {
            throw new TypeError('reportError requires an argument');
        }
        const stack = new Error().stack ?? '';
        let filename = '';
        let lineno = 0;
        let colno = 0;
        for (const line of stack.split('\n')) {
            if (line.includes('tjs:internal/capsid-bootstrap')) {
                continue;
            }
            const match =
                /\((.+):(\d+):(\d+)\)$/.exec(line) ??
                /^\s*at (.+):(\d+):(\d+)$/.exec(line);
            if (match) {
                filename = match[1];
                lineno = Number(match[2]);
                colno = Number(match[3]);
                break;
            }
        }

        let message;
        if (error instanceof Error) {
            const ownMessage =
                Object.getOwnPropertyDescriptor(error, 'message')?.value;
            const constructor =
                Object.getOwnPropertyDescriptor(
                    Object.getPrototypeOf(error), 'constructor')?.value;
            const errorName =
                Object.getOwnPropertyDescriptor(constructor ?? {}, 'name')
                    ?.value || 'Error';
            message = `Uncaught ${errorName}` +
                (typeof ownMessage === 'string' && ownMessage
                    ? `: ${ownMessage}`
                    : '');
        } else {
            message = `Uncaught ${String(error)}`;
        }
        const event = new ErrorEvent(error);
        Object.defineProperty(event, 'cancelable', {
            configurable: true, enumerable: true, value: true, writable: true,
        });
        Object.defineProperty(event, 'message', {
            configurable: true, enumerable: true, value: message,
        });
        if (filename) {
            Object.defineProperty(event, 'filename', {
                configurable: true, enumerable: true, value: filename,
            });
        }
        if (lineno) {
            Object.defineProperty(event, 'lineno', {
                configurable: true, enumerable: true, value: lineno,
            });
        }
        if (colno) {
            Object.defineProperty(event, 'colno', {
                configurable: true, enumerable: true, value: colno,
            });
        }
        if (globalThis.dispatchEvent(event)) {
            consoleObject.error(message);
        }
    },
});

core.defineEventAttribute(Object.getPrototypeOf(globalThis), 'error');

for (const name of [ 'CloseEvent', 'ProgressEvent' ]) {
    delete globalThis[name];
}

const requests = new Map();

const formatRuntimeError = error => {
    let summary = '';
    let stack = '';
    try {
        summary = String(error);
    } catch {
        summary = 'Error';
    }
    try {
        stack = typeof error?.stack === 'string' ? error.stack : '';
    } catch {
        // A hostile stack getter must not hide the original failure.
    }
    if (!stack) {
        return summary;
    }
    return summary && !stack.includes(summary)
        ? `${summary}\n${stack}`
        : stack;
};

function pullRequestBody(state) {
    if (state.chunks.length > 0) {
        const chunk = state.chunks.shift();
        const byteLength = chunk.byteLength;
        state.waiting = false;
        state.controller.enqueue(chunk);
        core.capsidRequestCredit(state.id, byteLength);
    } else if (state.ended) {
        state.waiting = false;
        state.controller.close();
    } else {
        state.waiting = true;
    }
}

const beginRequest = (handler, handlerThis, id, method, url, headerEntries) => {
    if (requests.has(id)) {
        throw new TypeError(`duplicate request id ${id}`);
    }

    const abortController = new AbortController();
    const hasBody = method !== 'GET' && method !== 'HEAD';
    const state = {
        id,
        abortController,
        chunks: [],
        controller: null,
        ended: !hasBody,
        waiting: false,
        cancelled: false,
        cancelReason: null,
        responseReader: null,
    };
    requests.set(id, state);

    let body;
    if (hasBody) {
        body = new ReadableStream({
            type: 'bytes',
            start(controller) {
                state.controller = controller;
            },
            pull() {
                pullRequestBody(state);
            },
            cancel() {
                state.ended = true;
                state.chunks.length = 0;
            },
        });
    }

    const request = new Request(url, {
        method,
        headers: headerEntries,
        body,
        duplex: hasBody ? 'half' : undefined,
        signal: abortController.signal,
    });

    // WP-02 §6.2: request identity rides the job-context token captured at
    // enqueue; no JS-side enter/leave is needed (or trusted) anymore.
    Promise.resolve()
        .then(() => handler.call(handlerThis, request))
        .then(async response => {
            if (state.cancelled) {
                if (response instanceof Response && response.body) {
                    try {
                        await response.body.cancel(state.cancelReason);
                    } catch {
                        // Cancellation cleanup must not revive the request.
                    }
                }
                return;
            }
            if (!(response instanceof Response)) {
                throw new TypeError('application fetch handler must return a Response');
            }

            const headers = [];
            response.headers.forEach((value, name) => {
                if (name !== 'set-cookie') {
                    headers.push([ name, value ]);
                }
            });
            for (const value of response.headers.getSetCookie()) {
                headers.push([ 'set-cookie', value ]);
            }

            // Fast path: a non-streamed body completes the whole
            // response (head + body + end) in one native call, avoiding
            // two JS round-trips per request. Streamed bodies keep the
            // incremental path below.
            const fastBody = getFastResponseBody(response);
            if (fastBody !== null) {
                core.capsidResponseFinal(
                    id,
                    response.status,
                    response.statusText,
                    headers,
                    fastBody,
                );
                return;
            }

            const body = response.body;
            if (body && !(body instanceof ReadableStream)) {
                let bytes = null;
                if (body instanceof Uint8Array) {
                    bytes = body;
                } else if (typeof body === 'string') {
                    bytes = body;
                } else if (body instanceof ArrayBuffer) {
                    bytes = new Uint8Array(body);
                } else if (body instanceof Blob) {
                    bytes = new Uint8Array(await body.arrayBuffer());
                } else {
                    bytes = null;
                }
                if (bytes !== null) {
                    core.capsidResponseFinal(
                        id,
                        response.status,
                        response.statusText,
                        headers,
                        bytes,
                    );
                    return;
                }
            }

            core.capsidResponseHead(
                id,
                response.status,
                response.statusText,
                headers,
            );

            if (response.body) {
                const reader = response.body.getReader();
                state.responseReader = reader;
                try {
                    while (true) {
                        const result = await reader.read();
                        if (result.done || state.cancelled) {
                            break;
                        }
                        await core.capsidResponseWrite(id, result.value);
                    }
                } finally {
                    if (state.responseReader === reader) {
                        state.responseReader = null;
                    }
                }
            }
            if (!state.cancelled) {
                core.capsidResponseEnd(id);
            }
        })
        .catch(error => {
            if (!state.cancelled) {
                core.capsidResponseError(id, formatRuntimeError(error));
            }
        })
        .finally(() => {
            if (requests.get(id) === state) {
                requests.delete(id);
                // WP-02 §6.4: settled signal — the last lifecycle bridge.
                // Native marks the token terminal; the post-drain reclaim
                // drops it (or poisons the worker on surviving refs).
                core.capsidRequestSettled(id);
            }
        });
};

const requestChunk = (id, chunk) => {
    const state = requests.get(id);
    if (!state || state.ended || !state.controller) {
        throw new TypeError(`request ${id} does not accept body data`);
    }
    if (state.waiting) {
        const byteLength = chunk.byteLength;
        state.waiting = false;
        state.controller.enqueue(chunk);
        core.capsidRequestCredit(id, byteLength);
    } else {
        state.chunks.push(chunk);
    }
};

const requestEnd = id => {
    const state = requests.get(id);
    if (!state || state.ended) {
        return;
    }
    state.ended = true;
    if (state.waiting && state.chunks.length === 0) {
        state.waiting = false;
        state.controller.close();
    }
};

const cancelRequest = id => {
    const state = requests.get(id);
    if (!state) {
        return;
    }
    requests.delete(id);
    state.cancelled = true;
    state.ended = true;
    state.chunks.length = 0;
    const reason = new DOMException('Request canceled', 'AbortError');
    state.cancelReason = reason;
    state.abortController.abort(reason);
    if (state.responseReader) {
        const reader = state.responseReader;
        state.responseReader = null;
        Promise.resolve(reader.cancel(reason)).catch(() => {});
    }
    if (state.controller) {
        try {
            state.controller.error(reason);
        } catch {
            // The stream may already be closed by the application.
        }
    }
};

core.capsidInstallBridge(beginRequest, requestChunk, requestEnd, cancelRequest);

const allowedGlobals = new Set(profileGlobalNames);
for (const name of Object.getOwnPropertyNames(globalThis)) {
    if (allowedGlobals.has(name)) {
        continue;
    }
    const descriptor = Object.getOwnPropertyDescriptor(globalThis, name);
    if (!descriptor?.configurable || !delete globalThis[name]) {
        throw new TypeError(`unexpected non-removable global: ${name}`);
    }
}
for (const name of profileGlobalNames) {
    if (!Object.prototype.hasOwnProperty.call(globalThis, name)) {
        throw new TypeError(`required profile global is missing: ${name}`);
    }
}
