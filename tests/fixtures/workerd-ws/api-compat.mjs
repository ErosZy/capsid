// API-surface compatibility probes C01-C17 (workerd differential).
// The /state endpoint reports every outcome; the runner asserts the
// GOLDEN table from the locked workerd reference in BOTH modes.
// Probes are grouped so close()-affecting cases get their own
// connections (c03/c04) and event-order cases (c15/c17) run isolated.
let results = [];
const record = (id, outcome) => results.push({ id, outcome });
let handlerErrorEvents = [];
const handlerErrorIntrinsics = {Error, TypeError, AggregateError, DOMException, String, Promise,
    setTimeout, hasOwn: Object.hasOwn};
const extendedHandlerErrorKinds = ['subclass', 'renamed-type', 'renamed-error', 'number-message',
    'empty-message', 'message-getter', 'message-getter-throws', 'custom-tostring', 'tostring-throws',
    'symbol', 'bigint', 'override-timer', 'override-hasown', 'async-error-listener',
    'microtask-message-mutation', 'microtask-name-mutation',
    'error-send', 'error-send-symbol', 'error-send-object', 'error-send-conversion-throws',
    'error-close', 'error-close-send', 'error-invalid-close', 'error-microtask-send',
    'getter-send', 'getter-close', 'getter-close-send',
    'object', 'object-primitive', 'object-tostring-throws', 'null-prototype',
    'object-data', 'object-cycle', 'object-getter', 'object-symbol-value', 'object-proxy',
    'object-tostringtag', 'object-fake-error',
    'dom-exception', 'dom-custom-name', 'dom-number-message', 'dom-extra', 'dom-name-getter',
    'dom-message-getter', 'dom-null-prototype', 'dom-fake', 'dom-combined-getters',
    'aggregate-error', 'aggregate-data', 'aggregate-cycle', 'aggregate-errors-getter',
    'error-cause', 'error-properties', 'error-cycle', 'error-fixed-stack', 'error-null-prototype',
    'error-name-number', 'error-name-null', 'error-name-undefined', 'error-name-object', 'error-name-symbol',
    'error-own-function', 'error-cycle-plain', 'error-ordered-getters', 'error-symbol-getter',
    'override-string', 'name-getter-throws', 'array', 'function'];

const handlerObjectKinds = ['object', 'object-primitive', 'object-tostring-throws', 'null-prototype',
    'object-data', 'object-cycle', 'object-getter', 'object-symbol-value', 'object-proxy',
    'object-tostringtag', 'object-fake-error'];
const enhancedHandlerKinds = extendedHandlerErrorKinds.slice(-31);
function snapshotHandlerObject(value, seen = new Map(), includeStack = false) {
    if (value === null || typeof value !== 'object') return {type: typeof value, value};
    if (seen.has(value)) return {ref: seen.get(value)};
    const id = seen.size; seen.set(value, id);
    const properties = Object.create(null);
    for (const key of Object.getOwnPropertyNames(value).filter(key => includeStack || key !== 'stack' || !(value instanceof handlerErrorIntrinsics.Error))) {
        const d = Object.getOwnPropertyDescriptor(value, key);
        properties[key] = {writable: d.writable, enumerable: d.enumerable, configurable: d.configurable,
            ...('value' in d ? {value: snapshotHandlerObject(d.value, seen, includeStack)}
                : {get: typeof d.get, set: typeof d.set})};
    }
    return {id, array: Array.isArray(value), prototype: Object.getPrototypeOf(value) === Object.prototype ? 'Object'
        : Object.getPrototypeOf(value) === Array.prototype ? 'Array' : 'other',
        error: value instanceof handlerErrorIntrinsics.Error, symbols: Object.getOwnPropertySymbols(value).length, properties};
}

function handlerErrorProbe(url) {
    const scenario = url.searchParams.get('case');
    const kind = url.searchParams.get('kind');
    const extended = extendedHandlerErrorKinds.includes(kind);
    const conversionCalls = [], overrideCalls = [];
    const constructors = {Error, TypeError, RangeError, ReferenceError, SyntaxError, URIError, EvalError};
    const pair = new WebSocketPair(), server = pair[1];
    const events = [];
    handlerErrorEvents = events;
    let thrown;
    const observe = (type, extra = {}) => events.push({type, readyState: server.readyState, ...extra});
    const attempt = (operation, fn) => {
        try { fn(); observe(operation, {outcome: 'accepted'}); }
        catch (error) { observe(operation, {outcome: 'threw', name: error.name, message: error.message}); }
    };
    const sendValue = (where, throwing = false) => ({toString() {
        observe(where + '-conversion');
        if (throwing) throw new handlerErrorIntrinsics.TypeError('conversion/' + where);
        return where + '-value';
    }});
    const observeMessageRead = () => conversionCalls.push({operation: 'message', readyState: server.readyState,
        after: events.map(event => event.type)});
    server.accept();
    server.addEventListener('error', event => {
        // Restore inside the observed error turn. A restoration timer would
        // add a separate lifetime input to this exception-specific test.
        globalThis.setTimeout = handlerErrorIntrinsics.setTimeout;
        globalThis.String = handlerErrorIntrinsics.String;
        Object.hasOwn = handlerErrorIntrinsics.hasOwn;
        observe('error', {message: event.message, errorName: event.error?.name,
            errorMessage: event.error?.message, errorText: String(event.error),
            errorType: typeof event.error, originalError: event.error === thrown,
            trusted: event.isTrusted, bubbles: event.bubbles, cancelable: event.cancelable,
            ...(extended ? {conversionCalls: conversionCalls.slice(), overrideCalls: overrideCalls.slice(),
                errorIsError: event.error instanceof handlerErrorIntrinsics.Error,
                errorIsTypeError: event.error instanceof handlerErrorIntrinsics.TypeError} : {}),
            ...(handlerObjectKinds.includes(kind) ? {objectGraph: snapshotHandlerObject(event.error)} : {}),
            ...(enhancedHandlerKinds.includes(kind) ? {errorGraph: snapshotHandlerObject(event.error, new Map(), true),
                errorStack: event.error?.stack,
                errorIsAggregateError: event.error instanceof handlerErrorIntrinsics.AggregateError,
                errorIsDOMException: event.error instanceof handlerErrorIntrinsics.DOMException} : {})});
        if (kind === 'error-send') attempt('error-send', () => server.send('from-error'));
        if (kind === 'error-send-symbol') attempt('error-send', () => server.send(Symbol('bad')));
        if (kind === 'error-send-object') attempt('error-send', () => server.send(sendValue('error')));
        if (kind === 'error-send-conversion-throws') attempt('error-send', () => server.send(sendValue('error', true)));
        if (kind === 'error-close' || kind === 'error-close-send') attempt('error-close', () => server.close(1000, 'error-done'));
        if (kind === 'error-close-send') attempt('error-send', () => server.send(sendValue('after-close')));
        if (kind === 'error-invalid-close') attempt('error-close', () => server.close(1006, 'invalid'));
        if (kind === 'error-microtask-send') handlerErrorIntrinsics.Promise.resolve().then(() => {
            attempt('post-error-send', () => server.send(sendValue('post-error')));
            attempt('post-error-close', () => server.close(1000, 'post-error-done'));
            attempt('post-error-send-after-close', () => server.send(sendValue('post-error-after-close')));
        });
        if (url.searchParams.get('rethrow') === '1') throw new Error('secondary error listener failure');
        if (kind === 'async-error-listener')
            return handlerErrorIntrinsics.Promise.reject(new handlerErrorIntrinsics.Error('secondary async failure'));
    });
    server.addEventListener('close', () => observe('close'));
    server.addEventListener('message', () => {
        observe('message');
        server.send(scenario === 'c18' ? 'a-before' : 'queued');
        if (scenario === 'c18') server.close(1000, 'done');
        queueMicrotask(() => {
            observe('microtask');
            Promise.resolve().then(() => {
                observe('nested-microtask');
                try { server.send('late'); observe('microtask-send', {outcome: 'accepted'}); }
                catch (error) { observe('microtask-send', {outcome: 'threw', name: error.name, message: error.message}); }
            });
        });
        thrown = kind === 'string' ? 'thrown-string' : kind === 'undefined' ? undefined :
            kind === 'null' ? null : kind === 'number' ? 42 :
                new (constructors[kind] ?? constructors.TypeError)('handler-error/' + scenario);
        switch (kind) {
            case 'dom-exception': thrown = new DOMException('dom-message', 'AbortError'); break;
            case 'dom-custom-name': thrown = new DOMException('dom-message', 'AbortError'); Object.defineProperty(thrown, 'name', {value: 'VisibleDOM', enumerable: true}); break;
            case 'dom-number-message': thrown = new DOMException('dom-message', 'AbortError'); Object.defineProperty(thrown, 'message', {value: 7, enumerable: true}); break;
            case 'dom-extra': thrown = new DOMException('dom-message', 'AbortError'); thrown.extra = {x: 7}; break;
            case 'dom-name-getter': thrown = new DOMException('dom-message', 'AbortError'); Object.defineProperty(thrown, 'name', {get() {conversionCalls.push('dom-name'); return 'VisibleDOM';}}); break;
            case 'dom-message-getter': thrown = new DOMException('dom-message', 'AbortError'); Object.defineProperty(thrown, 'message', {get() {conversionCalls.push('dom-message'); return 'visible-message';}}); break;
            case 'dom-null-prototype': thrown = new DOMException('dom-message', 'AbortError'); Object.setPrototypeOf(thrown, null); break;
            case 'dom-fake': thrown = Object.create(DOMException.prototype); break;
            case 'dom-combined-getters': thrown = new DOMException('dom-message', 'AbortError'); Object.defineProperties(thrown, {
                name: {get() {conversionCalls.push('name'); return 'VisibleDOM';}},
                message: {get() {conversionCalls.push('message'); return 'visible-message';}}}); break;
            case 'aggregate-error': thrown = new AggregateError([new handlerErrorIntrinsics.Error('child')], 'aggregate-message'); break;
            case 'aggregate-data': {const child = {x: 7}; thrown = new AggregateError([child, 'child', null], 'aggregate-message', {cause: child}); break;}
            case 'aggregate-cycle': thrown = new AggregateError([], 'aggregate-message'); thrown.errors.push(thrown); break;
            case 'aggregate-errors-getter': thrown = new AggregateError([], 'aggregate-message'); Object.defineProperty(thrown, 'errors', {get() {conversionCalls.push('errors'); return [{x: 7}];}}); break;
            case 'error-cause': thrown = new handlerErrorIntrinsics.TypeError('cause-message', {cause: {x: 7}}); break;
            case 'error-properties': Object.defineProperty(thrown, 'hidden', {value: {x: 7}}); thrown.extra = 9; thrown[Symbol('ignored')] = 4; break;
            case 'error-cycle': thrown.self = thrown; break;
            case 'error-fixed-stack': thrown.stack = 'fixed-stack'; break;
            case 'error-null-prototype': Object.setPrototypeOf(thrown, null); break;
            case 'error-name-number': thrown.name = 7; break;
            case 'error-name-null': thrown.name = null; break;
            case 'error-name-undefined': thrown.name = undefined; break;
            case 'error-name-object': thrown.name = {toString() {conversionCalls.push('name-conversion'); return 'TypeError';}}; break;
            case 'error-name-symbol': thrown.name = Symbol('name'); break;
            case 'error-own-function': thrown.extra = () => {}; break;
            case 'error-cycle-plain': thrown.extra = {parent: thrown}; break;
            case 'error-ordered-getters': thrown.stack = 'fixed-stack'; Object.defineProperties(thrown, {
                first: {get() {conversionCalls.push('first'); return {get nested() {conversionCalls.push('nested'); return 7;}};}},
                second: {get() {conversionCalls.push('second'); return 8;}}}); break;
            case 'error-symbol-getter': Object.defineProperty(thrown, Symbol('ignored'), {get() {conversionCalls.push('symbol'); return 7;}, enumerable: true}); break;
            case 'override-string': globalThis.String = () => {overrideCalls.push('String'); return 'replacement-string';}; break;
            case 'name-getter-throws': Object.defineProperty(thrown, 'name', {get() {conversionCalls.push('name'); throw new handlerErrorIntrinsics.Error('name-conversion-failed');}}); break;
            case 'array': thrown = [1, , undefined, 'x']; break;
            case 'function': thrown = function fixtureFunction(){return 7;}; break;
            case 'subclass': {
                class CustomError extends handlerErrorIntrinsics.TypeError {}
                thrown = new CustomError('custom-message'); thrown.name = 'CustomError'; break;
            }
            case 'renamed-type': thrown.name = 'Renamed'; break;
            case 'renamed-error': thrown = new handlerErrorIntrinsics.Error('changed-type'); thrown.name = 'TypeError'; break;
            case 'number-message': thrown.message = 7; break;
            case 'empty-message': thrown.message = ''; break;
            case 'message-getter': Object.defineProperty(thrown, 'message', {get() {
                observeMessageRead(); return 'getter-message';
            }}); break;
            case 'message-getter-throws': Object.defineProperty(thrown, 'message', {get() {
                observeMessageRead(); throw new handlerErrorIntrinsics.Error('message-conversion-failed');
            }}); break;
            case 'custom-tostring': thrown.toString = () => { conversionCalls.push('toString'); return 'CustomText: custom'; }; break;
            case 'tostring-throws': thrown.toString = () => { conversionCalls.push('toString'); throw new handlerErrorIntrinsics.Error('string-conversion-failed'); }; break;
            case 'symbol': thrown = Symbol('handler-symbol'); break;
            case 'bigint': thrown = 12345678901234567890n; break;
            case 'override-timer': globalThis.setTimeout = () => { overrideCalls.push('setTimeout'); return 0; }; break;
            case 'override-hasown': Object.hasOwn = () => { overrideCalls.push('hasOwn'); throw new handlerErrorIntrinsics.Error('replacement-hasOwn'); }; break;
            case 'microtask-message-mutation': queueMicrotask(() => { thrown.message = 'changed-after-throw'; }); break;
            case 'microtask-name-mutation': queueMicrotask(() => { thrown.name = 'ChangedAfterThrow'; }); break;
            case 'object': thrown = {toString() { conversionCalls.push('toString'); return 'object-text'; }}; break;
            case 'object-primitive': thrown = {[Symbol.toPrimitive](hint) { conversionCalls.push('primitive/' + hint); return 'primitive-text'; }}; break;
            case 'object-tostring-throws': thrown = {toString() { conversionCalls.push('toString'); throw new Error('object-conversion-failed'); }}; break;
            case 'null-prototype': thrown = Object.create(null); break;
            case 'object-data': {
                const nested = {x: 7}, items = [1, , undefined, nested];
                items[4] = items; items.note = 'array-note';
                thrown = {message: 'plain-message', nested, items};
                Object.defineProperty(thrown, '__proto__', {value: {marker: 'data'}, enumerable: true});
                Object.defineProperty(thrown, 'hidden', {value: 'ignored'});
                break;
            }
            case 'object-cycle': { const child = {x: 7}; thrown = {left: child, right: child}; thrown.self = thrown; break; }
            case 'object-getter': thrown = {get detail() {
                conversionCalls.push({operation: 'detail', readyState: server.readyState, after: events.map(event => event.type)});
                return {x: 7};
            }}; break;
            case 'object-symbol-value': thrown = {token: Symbol('not-cloneable')}; break;
            case 'object-proxy': thrown = new Proxy({x: 7}, {ownKeys(target) { conversionCalls.push('ownKeys'); return Reflect.ownKeys(target); }}); break;
            case 'object-tostringtag': thrown = {[Symbol.toStringTag]: 'Special'}; break;
            case 'object-fake-error': thrown = Object.create(TypeError.prototype); thrown.message = 'fake-message'; break;
            case 'getter-send': case 'getter-close': case 'getter-close-send':
                Object.defineProperty(thrown, 'message', {get() {
                    observe('message-getter');
                    if (kind === 'getter-send') attempt('getter-send', () => server.send(sendValue('getter')));
                    else attempt('getter-close', () => server.close(1000, 'getter-done'));
                    if (kind === 'getter-close-send') attempt('getter-send', () => server.send(sendValue('getter-after-close')));
                    return 'getter-message';
                }}); break;
        }
        throw thrown;
    });
    return new Response(null, {status: 101, webSocket: pair[0]});
}

function observePublicEvents() {
    const out = {};
    const probe = (name, fn) => {
        try { out[name] = { ok: true, value: fn() }; }
        catch (error) { out[name] = { ok: false, error: error.name }; }
    };
    const defined = value => value === undefined ? { undefined: true } : value;
    const base = event => ({
        type: event.type, bubbles: event.bubbles, cancelable: event.cancelable,
        composed: event.composed, phase: event.eventPhase, trusted: event.isTrusted,
        timeStamp: event.timeStamp, tag: Object.prototype.toString.call(event),
    });
    const shape = event => {
        const result = base(event);
        for (const field of ['message', 'filename', 'lineno', 'colno', 'error',
            'data', 'origin', 'lastEventId', 'source', 'ports', 'code', 'reason', 'wasClean', 'detail']) {
            if (field in event) result[field] = defined(event[field]);
        }
        return result;
    };
    for (const Constructor of [Event, ErrorEvent, MessageEvent, CloseEvent, CustomEvent]) {
        const prefix = Constructor.name;
        probe(prefix + '/missing', () => shape(new Constructor()));
        probe(prefix + '/defaults', () => shape(new Constructor('x')));
        probe(prefix + '/null', () => shape(new Constructor('x', null)));
        probe(prefix + '/symbol-type', () => shape(new Constructor(Symbol('x'))));
        probe(prefix + '/surrogate-type', () => new Constructor('\ud800').type);
        for (const [name, value] of Object.entries({ false: false, zero: 0, string: 'x',
            symbol: Symbol('s'), function: () => {} })) {
            probe(prefix + '/init-' + name, () => base(new Constructor('x', value)));
        }
    }
    probe('Event/snapshot', () => {
        const reads = [];
        const values = { bubbles: 1, cancelable: 1, composed: 1 };
        const event = new Event('x', new Proxy(values, { get(t, k) { reads.push(k); return t[k]; } }));
        values.bubbles = values.cancelable = values.composed = false;
        return { reads, value: base(event) };
    });
    probe('Event/lifecycle', () => {
        const target = new EventTarget();
        const event = new Event('x', { cancelable: true });
        const states = [];
        const capture = () => ({ phase: event.eventPhase, target: event.target === target,
            current: event.currentTarget === target, path: event.composedPath().map(x => x === target),
            canceled: event.defaultPrevented });
        states.push(capture());
        target.addEventListener('x', () => { states.push(capture()); event.preventDefault(); });
        const returns = [target.dispatchEvent(event)]; states.push(capture());
        returns.push(target.dispatchEvent(event)); states.push(capture());
        return { states, returns };
    });
    probe('Event/no-listeners', () => {
        const target = new EventTarget(); const event = new Event('x');
        const result = target.dispatchEvent(event);
        return { result, phase: event.eventPhase, target: event.target === target,
            current: event.currentTarget === target, path: event.composedPath().length };
    });
    probe('Event/reentrant', () => {
        const target = new EventTarget(); const event = new Event('x'); let caught = null;
        target.addEventListener('x', () => {
            try { target.dispatchEvent(event); } catch (error) { caught = error.name; }
        }, { once: true });
        target.dispatchEvent(event); return caught;
    });
    probe('Event/synchronous-dispatch', () => {
        const target = new EventTarget(); const order = [];
        target.addEventListener('x', () => {
            order.push('first');
            Promise.resolve().then(() => order.push('microtask'));
        });
        target.addEventListener('x', () => order.push('second'));
        target.dispatchEvent(new Event('x'));
        order.push('returned');
        return order.slice();
    });
    probe('Event/trust-descriptor', () => {
        const event = new Event('x');
        const descriptor = Object.getOwnPropertyDescriptor(Event.prototype, 'isTrusted');
        let assignment;
        try { event.isTrusted = true; assignment = 'accepted'; } catch (error) { assignment = error.name; }
        return { own: Object.hasOwn(event, 'isTrusted'),
            descriptor: descriptor ? { get: typeof descriptor.get, set: typeof descriptor.set,
                configurable: descriptor.configurable, enumerable: descriptor.enumerable } : null,
            assignment, value: event.isTrusted };
    });
    for (const Constructor of [ErrorEvent, MessageEvent, CloseEvent, CustomEvent]) {
        probe(Constructor.name + '/reads', () => {
            const reads = [];
            const values = { bubbles: true, cancelable: true, composed: true,
                message: 'm', filename: 'f', lineno: 2, colno: 3, error: 4,
                data: 5, origin: 'o', lastEventId: 'i', source: null, ports: [],
                code: 1000, reason: 'r', wasClean: true, detail: 6 };
            const event = new Constructor('x', new Proxy(values, { get(t, k) { reads.push(k); return t[k]; } }));
            return { reads, value: shape(event) };
        });
    }
    for (const [name, value] of Object.entries({ negative: -1, fraction: 1.5, smallNegative: -.5,
        min: -2147483648, under: -2147483649, max: 2147483647, over: 2147483648,
        maxFraction: 2147483647.9, nan: NaN, infinity: Infinity, negativeInfinity: -Infinity,
        bigint: 1n, boxedBigint: Object(1n), null: null, undefined })) {
        probe('ErrorEvent/number-' + name, () => {
            const event = new ErrorEvent('x', { lineno: value, colno: value });
            return [defined(event.lineno), defined(event.colno)];
        });
    }
    probe('ErrorEvent/strings', () => shape(new ErrorEvent('x', { message: 123n, filename: null })));
    probe('ErrorEvent/surrogates', () => shape(new ErrorEvent('x', { message: '\ud800', filename: '\udc00' })));
    probe('ErrorEvent/identity', () => { const error = {}; return new ErrorEvent('x', { error }).error === error; });
    probe('MessageEvent/identity', () => { const data = {}; return new MessageEvent('x', { data }).data === data; });
    probe('MessageEvent/strings', () => shape(new MessageEvent('x', { origin: 123n, lastEventId: null })));
    const channel = new MessageChannel();
    try {
        for (const [name, value] of Object.entries({ object: {}, number: 1, string: 'x', null: null,
            undefined, port: channel.port1, forged: Object.create(MessagePort.prototype) })) {
            probe('MessageEvent/source-' + name, () => {
                const event = new MessageEvent('x', { source: value });
                return { same: event.source === value, null: event.source === null };
            });
        }
        for (const [name, value] of Object.entries({ object: {}, number: 1, string: 'x', null: null,
            undefined, empty: [], invalid: [{}], list: [channel.port1], set: new Set([channel.port1]),
            arrayLike: { 0: channel.port1, length: 1 } })) {
            probe('MessageEvent/ports-' + name, () => {
                const event = new MessageEvent('x', { ports: value });
                const first = event.ports;
                return { length: first.length, frozen: Object.isFrozen(first), same: first === value,
                    stable: first === event.ports, first: first[0] === channel.port1 };
            });
        }
    } finally { channel.port1.close(); channel.port2.close(); }
    for (const [name, value] of Object.entries({ negative: -1, max: 65535, high: 65536,
        maxFraction: 65535.9, int32: 2147483647, over32: 2147483648 })) {
        probe('CloseEvent/number-' + name, () => new CloseEvent('x', { code: value }).code);
    }
    probe('CustomEvent/identity', () => { const detail = {}; return new CustomEvent('x', { detail }).detail === detail; });
    probe('MessagePort/constructor', () => { const port = new MessagePort(); port.close(); return 'constructed'; });
    return out;
}

function observeListenerEvents() {
    const out = {};
    const factories = {
        EventTarget: () => ({ target: new EventTarget(), cleanup() {} }),
        WebSocket: () => ({ target: new WebSocketPair()[0], cleanup() {} }),
        MessagePort: () => {
            const channel = new MessageChannel();
            return { target: channel.port1, cleanup() { channel.port1.close(); channel.port2.close(); } };
        },
    };
    const cases = {
        exception(target) {
            const event = new Event('message', { cancelable: true });
            const error = new TypeError('listener-contract'); const order = []; let sameError = false;
            const onError = event => { order.push('reported'); event.preventDefault(); };
            globalThis.addEventListener('error', onError);
            target.addEventListener('message', event => {
                order.push('first'); event.preventDefault(); throw error;
            }, { once: true });
            target.addEventListener('message', () => order.push('second'));
            try { target.dispatchEvent(event); } catch (caught) { sameError = caught === error; }
            finally { globalThis.removeEventListener('error', onError); }
            const after = { phase: event.eventPhase, current: event.currentTarget === target,
                canceled: event.defaultPrevented, path: event.composedPath().length };
            const first = order.slice(); order.length = 0;
            const returned = target.dispatchEvent(event);
            return { sameError, first, after, second: order, returned, canceled: event.defaultPrevented };
        },
        'object-exception'(target) {
            const error = {}; let sameError = false; let receiver = false; let reported = 0;
            const onError = event => { ++reported; event.preventDefault(); };
            const listener = { handleEvent() { receiver = this === listener; throw error; } };
            target.addEventListener('message', listener, { once: true });
            globalThis.addEventListener('error', onError);
            const event = new Event('message');
            try { target.dispatchEvent(event); } catch (caught) { sameError = caught === error; }
            finally { globalThis.removeEventListener('error', onError); }
            return { sameError, receiver, reported, phase: event.eventPhase };
        },
        'remove-next-once'(target) {
            const order = []; const second = () => order.push('second');
            target.addEventListener('message', () => {
                order.push('first'); target.removeEventListener('message', second);
            }, { once: true });
            target.addEventListener('message', second);
            target.dispatchEvent(new Event('message')); return order;
        },
        'remove-next'(target) {
            const order = []; const second = () => order.push('second');
            target.addEventListener('message', () => {
                order.push('first'); target.removeEventListener('message', second);
            });
            target.addEventListener('message', second);
            target.dispatchEvent(new Event('message')); return order;
        },
        'add-next'(target) {
            const order = []; const added = () => order.push('added');
            target.addEventListener('message', () => {
                order.push('first'); target.addEventListener('message', added);
            });
            target.addEventListener('message', () => order.push('second'));
            target.dispatchEvent(new Event('message')); const first = order.slice(); order.length = 0;
            target.dispatchEvent(new Event('message')); return { first, second: order };
        },
        'remove-readd'(target) {
            const order = []; const second = () => order.push('second');
            target.addEventListener('message', () => {
                order.push('first'); target.removeEventListener('message', second);
                target.addEventListener('message', second);
            }, { once: true });
            target.addEventListener('message', second);
            target.dispatchEvent(new Event('message')); const first = order.slice(); order.length = 0;
            target.dispatchEvent(new Event('message')); return { first, second: order };
        },
        'self-remove-readd'(target) {
            const order = []; let changed = false;
            const first = () => {
                order.push('first');
                if (!changed) {
                    changed = true; target.removeEventListener('message', first);
                    target.addEventListener('message', first);
                }
            };
            target.addEventListener('message', first);
            target.addEventListener('message', () => order.push('second'));
            target.dispatchEvent(new Event('message')); const initial = order.slice(); order.length = 0;
            target.dispatchEvent(new Event('message')); return { initial, next: order };
        },
        'nested-once'(target) {
            const order = []; const outer = new Event('message'); const inner = new Event('message');
            target.addEventListener('message', event => {
                order.push(['first', event === outer, event.eventPhase]);
                target.dispatchEvent(inner); order.push(['resumed', outer.eventPhase, inner.eventPhase]);
            }, { once: true });
            target.addEventListener('message', event => order.push(['second', event === outer, event.eventPhase]));
            target.dispatchEvent(outer); return { order, phases: [outer.eventPhase, inner.eventPhase] };
        },
        'nested-add-remove'(target) {
            const order = []; const outer = new Event('message');
            const second = event => order.push(event === outer ? 'outer-second' : 'inner-second');
            const added = event => order.push(event === outer ? 'outer-added' : 'inner-added');
            target.addEventListener('message', () => {
                order.push('first'); target.removeEventListener('message', second);
                target.addEventListener('message', added); target.dispatchEvent(new Event('message'));
            }, { once: true });
            target.addEventListener('message', second); target.dispatchEvent(outer); return order;
        },
        'duplicate-once'(target) {
            let count = 0; const listener = () => ++count;
            target.addEventListener('message', listener, { once: true });
            target.addEventListener('message', listener, { once: false });
            target.dispatchEvent(new Event('message')); target.dispatchEvent(new Event('message')); return count;
        },
        'stop-immediate-redispatch'(target) {
            const order = []; const event = new Event('message', { cancelable: true });
            target.addEventListener('message', event => {
                order.push('first'); event.stopImmediatePropagation(); event.preventDefault();
            }, { once: true });
            target.addEventListener('message', event => order.push(['second', event.defaultPrevented]));
            const returns = [target.dispatchEvent(event), target.dispatchEvent(event)]; return { order, returns };
        },
        'stop-propagation'(target) {
            const order = [];
            target.addEventListener('message', event => { order.push('first'); event.stopPropagation(); });
            target.addEventListener('message', () => order.push('second'));
            target.dispatchEvent(new Event('message')); return order;
        },
        'passive-cancel'(target) {
            const event = new Event('message', { cancelable: true }); const states = [];
            try {
                target.addEventListener('message', event => {
                    event.preventDefault(); states.push(event.defaultPrevented);
                }, { passive: true });
            } catch (error) { return { stage: 'registration', error: error.name }; }
            target.addEventListener('message', event => states.push(event.defaultPrevented));
            const returned = target.dispatchEvent(event); return { stage: 'dispatched', states, returned };
        },
        'capture-add'(target) {
            const errors = []; let calls = 0;
            for (const options of [true, { capture: true }]) {
                try { target.addEventListener('message', () => ++calls, options); errors.push(null); }
                catch (error) { errors.push(error.name); }
            }
            target.dispatchEvent(new Event('message')); return { errors, calls };
        },
        'capture-remove'(target) {
            const errors = []; let calls = 0; const listener = () => ++calls;
            target.addEventListener('message', listener);
            for (const options of [true, { capture: true }]) {
                try { target.removeEventListener('message', listener, options); errors.push(null); }
                catch (error) { errors.push(error.name); }
            }
            target.dispatchEvent(new Event('message')); return { errors, calls };
        },
        'handleEvent-snapshot'(target) {
            const calls = []; const listener = { handleEvent() { calls.push('original'); } };
            target.addEventListener('message', listener);
            listener.handleEvent = () => calls.push('replacement');
            target.dispatchEvent(new Event('message')); return calls;
        },
        'return-true'(target) {
            const event = new Event('message', { cancelable: true });
            target.addEventListener('message', () => true);
            return { returned: target.dispatchEvent(event), canceled: event.defaultPrevented };
        },
        'abort-before-turn'(target) {
            const controller = new AbortController(); const calls = [];
            target.addEventListener('message', () => { calls.push('first'); controller.abort(); });
            target.addEventListener('message', () => calls.push('second'), { signal: controller.signal });
            target.dispatchEvent(new Event('message')); return calls;
        },
        'already-aborted'(target) {
            const controller = new AbortController(); controller.abort(); let count = 0;
            target.addEventListener('message', () => ++count, { signal: controller.signal });
            target.dispatchEvent(new Event('message')); return count;
        },
        'abort-after-readd'(target) {
            const controller = new AbortController(); let count = 0; const listener = () => ++count;
            target.addEventListener('message', listener, { signal: controller.signal });
            target.removeEventListener('message', listener); target.addEventListener('message', listener);
            controller.abort(); target.dispatchEvent(new Event('message')); return count;
        },
        'abort-stopped'(target) {
            const controller = new AbortController(); let count = 0;
            controller.signal.addEventListener('abort', event => event.stopImmediatePropagation());
            target.addEventListener('message', () => ++count, { signal: controller.signal });
            controller.abort(); target.dispatchEvent(new Event('message')); return count;
        },
        'option-read-order'(target) {
            const reads = []; const listener = () => {};
            const options = new Proxy({ capture: false, once: false, passive: false }, {
                get(value, key) { reads.push(key); return value[key]; },
            });
            target.addEventListener('message', listener, options);
            target.removeEventListener('message', listener, options); return reads;
        },
    };
    for (const [kind, create] of Object.entries(factories)) {
        for (const [name, observe] of Object.entries(cases)) {
            const { target, cleanup } = create();
            try { out[kind + '/' + name] = { ok: true, value: observe(target) }; }
            catch (error) { out[kind + '/' + name] = { ok: false, error: error.name }; }
            finally { cleanup(); }
        }
    }
    return out;
}

async function observeRuntimeEvents() {
    const out = {};
    const channel = new MessageChannel();
    const shape = (event, target) => ({ type: event.type, trusted: event.isTrusted,
        phase: event.eventPhase, target: event.target === target,
        currentTarget: event.currentTarget === target, tag: Object.prototype.toString.call(event) });
    const reason = new Error('runtime-event-probe');
    let reported;
    const onError = event => { reported = event; event.preventDefault(); out.error = {
        ...shape(event, globalThis), identity: event.error === reason }; };
    globalThis.addEventListener('error', onError, { once: true });
    try {
        reportError(reason);
        out.errorAfter = reported ? shape(reported, globalThis) : null;
        let messageEvent;
        const messageOrder = [];
        channel.port2.addEventListener('message', () => {
            messageOrder.push('first');
            Promise.resolve().then(() => messageOrder.push('microtask'));
        });
        let timer;
        try {
            await Promise.race([
                new Promise(resolve => {
                    channel.port2.onmessage = event => {
                        messageEvent = event;
                        messageOrder.push('second');
                        out.message = { ...shape(event, channel.port2), data: event.data,
                            sourceIsSender: event.source === channel.port1,
                            sourceIsReceiver: event.source === channel.port2,
                            sourceIsPort: event.source instanceof MessagePort,
                            ports: event.ports.length };
                        resolve();
                    };
                    channel.port1.postMessage('runtime');
                }),
                new Promise((_, reject) => { timer = setTimeout(() => reject(new Error('message event deadline')), 2000); }),
            ]);
        } finally { clearTimeout(timer); }
        out.messageAfter = shape(messageEvent, channel.port2);
        out.messageOrder = messageOrder.slice();

        let unhandled;
        let handled;
        const onUnhandled = event => {
            if (event.reason !== reason) return;
            event.preventDefault();
            unhandled = event;
            out.unhandled = { ...shape(event, globalThis), identity: event.reason === reason };
        };
        const onHandled = event => {
            if (event.promise !== unhandled?.promise) return;
            handled = event;
            out.handled = { ...shape(event, globalThis), promiseIdentity: event.promise === unhandled.promise,
                reasonIdentity: event.reason === reason, reasonType: typeof event.reason };
        };
        globalThis.addEventListener('unhandledrejection', onUnhandled);
        globalThis.addEventListener('rejectionhandled', onHandled);
        const rejected = Promise.reject(reason);
        try {
            const deadline = Date.now() + 2000;
            while (!unhandled && Date.now() < deadline) await new Promise(resolve => setTimeout(resolve, 1));
            if (!unhandled) throw new Error('unhandledrejection deadline');
            out.unhandledAfter = shape(unhandled, globalThis);
            rejected.catch(() => {});
            while (!handled && Date.now() < deadline) await new Promise(resolve => setTimeout(resolve, 1));
            if (!handled) throw new Error('rejectionhandled deadline');
            out.handledAfter = shape(handled, globalThis);
        } finally {
            rejected.catch(() => {});
            globalThis.removeEventListener('unhandledrejection', onUnhandled);
            globalThis.removeEventListener('rejectionhandled', onHandled);
        }
        return out;
    } finally {
        globalThis.removeEventListener('error', onError);
        channel.port1.close(); channel.port2.close();
    }
}

// Pinned workerd starts delivery for a message listener without start().
// Record the standards deviation independently instead of calling WPT stale.
const messageQueuePause = () => new Promise(resolve => setTimeout(resolve, 30));

async function observeMessageQueue(startMode) {
    const channel = new MessageChannel();
    const received = [];
    let resolveDelivery;
    const delivered = new Promise(resolve => { resolveDelivery = resolve; });
    channel.port2.addEventListener('message', event => {
        received.push(event.data);
        if (received.length === 3) resolveDelivery();
    });
    try {
        channel.port1.postMessage('first');
        channel.port1.postMessage('second');
        await messageQueuePause();
        const beforeStart = received.slice();
        if (startMode === 'explicit') {
            channel.port2.start();
            channel.port2.start();
        } else if (startMode === 'handler') {
            channel.port2.onmessage = () => {};
        } else {
            channel.port2.onmessage = null;
        }
        const immediatelyAfterStart = received.slice();
        channel.port1.postMessage('third');
        let timer;
        try {
            await Promise.race([
                delivered,
                new Promise((_, reject) => {
                    timer = setTimeout(() => reject(new Error('queued messages were lost')), 1000);
                }),
            ]);
        } finally {
            clearTimeout(timer);
        }
        await messageQueuePause(); // Also detect duplicate delivery after repeated start().
        return { beforeStart, immediatelyAfterStart, received };
    } finally {
        channel.port1.close();
        channel.port2.close();
    }
}

function observeEventHandlerAttributes() {
const cases = {
  replace(target,type,trace) { target['on'+type]=()=>trace.push('old'); target.addEventListener(type,()=>trace.push('listener')); target['on'+type]=()=>trace.push('new'); },
  reassign(target,type,trace) { const fn=()=>trace.push('handler'); target['on'+type]=fn; target.addEventListener(type,()=>trace.push('listener')); target['on'+type]=fn; },
  duplicate(target,type,trace) { const fn=()=>trace.push('same'); target.addEventListener(type,fn); target['on'+type]=fn; },
  remove(target,type,trace) { const fn=()=>trace.push('handler'); target['on'+type]=fn; target.removeEventListener(type,fn); },
  clear(target,type,trace) { const fn=()=>trace.push('listener'); target.addEventListener(type,fn); target['on'+type]=fn; target['on'+type]=null; },
  reset(target,type,trace) { target['on'+type]=()=>trace.push('old'); target.addEventListener(type,()=>trace.push('listener')); target['on'+type]=null; target['on'+type]=()=>trace.push('new'); },
  once(target,type,trace) { const fn=()=>trace.push('same'); target.addEventListener(type,fn,{once:true}); target['on'+type]=fn; },
  nonfunction(target,type,trace) { target['on'+type]={handleEvent(){trace.push('object')}}; trace.push(target['on'+type]===null?'null':'object'); },
  initial(target,type,trace) { trace.push({isNull:target['on'+type]===null,kind:typeof target['on'+type]}); },
  ['null-first'](target,type,trace) { target['on'+type]=null; target.addEventListener(type,()=>trace.push('listener')); target['on'+type]=()=>trace.push('handler'); },
  ['object-first'](target,type,trace) { target['on'+type]={}; target.addEventListener(type,()=>trace.push('listener')); target['on'+type]=()=>trace.push('handler'); },
  ['replace-during-dispatch'](target,type,trace) { target.addEventListener(type,()=>{trace.push('listener');target['on'+type]=()=>trace.push('new')}); target['on'+type]=()=>trace.push('old'); },
  ['clear-during-dispatch'](target,type,trace) { target.addEventListener(type,()=>{trace.push('listener');target['on'+type]=null}); target['on'+type]=()=>trace.push('old'); },
  ['reset-during-dispatch'](target,type,trace) { target.addEventListener(type,()=>{trace.push('listener');target['on'+type]=null;target['on'+type]=()=>trace.push('new')}); target['on'+type]=()=>trace.push('old'); },
  ['self-replace'](target,type,trace) { target['on'+type]=()=>{trace.push('old');target['on'+type]=()=>trace.push('new')};target.addEventListener(type,()=>trace.push('listener')); },
  ['receiver'](target,type,trace) { target['on'+type]=function(event){trace.push({self:this===target,undefinedReceiver:this===undefined,globalReceiver:this===globalThis,target:event.target===target,current:event.currentTarget===target,phase:event.eventPhase})}; },
  ['override-add-remove'](target,type,trace) { target.addEventListener=()=>{throw new Error('overridden add')};target.removeEventListener=()=>{throw new Error('overridden remove')};target['on'+type]=()=>trace.push('handler'); },
  ['handler-throw'](target,type,trace) { target['on'+type]=()=>{trace.push('handler');throw new TypeError('handler failure')};target.addEventListener(type,()=>trace.push('listener')); },
  ['returns-true'](target,type,trace) {target['on'+type]=()=>true;const e=new Event(type,{cancelable:true});trace.push({returned:target.dispatchEvent(e),canceled:e.defaultPrevented});target['on'+type]=null;},
  ['returns-false'](target,type,trace) {target['on'+type]=()=>false;const e=new Event(type,{cancelable:true});trace.push({returned:target.dispatchEvent(e),canceled:e.defaultPrevented});target['on'+type]=null;},
  ['own-value'](target,type,trace) { Object.defineProperty(target,'on'+type,{configurable:true,value:()=>trace.push('own')}); },
  ['own-getter'](target,type,trace) { Object.defineProperty(target,'on'+type,{configurable:true,get(){trace.push('get');return ()=>trace.push('own')}}); },
  ['delete-property'](target,type,trace) { target['on'+type]=()=>trace.push('handler');trace.push(delete target['on'+type]);trace.push(typeof target['on'+type]); },
  ['descriptors'](target,type,trace) { const shape=(o)=>{const d=Object.getOwnPropertyDescriptor(o,'on'+type);return d?{enumerable:d.enumerable,configurable:d.configurable,writable:d.writable??null,get:typeof d.get,set:typeof d.set}:null};trace.push(shape(target),shape(Object.getPrototypeOf(target)));target['on'+type]=()=>{};trace.push(shape(target),shape(Object.getPrototypeOf(target))); },
  ['own-value-after-assignment'](target,type,trace) { target['on'+type]=()=>trace.push('registered'); Object.defineProperty(target,'on'+type,{configurable:true,value:()=>trace.push('own')}); },
  ['own-value-after-clear'](target,type,trace) { target['on'+type]=()=>trace.push('registered');target['on'+type]=null; Object.defineProperty(target,'on'+type,{configurable:true,value:()=>trace.push('own')}); },
  ['getter-adds-listener'](target,type,trace) { const fn=()=>trace.push('added');Object.defineProperty(target,'on'+type,{configurable:true,get(){trace.push('get');target.addEventListener(type,fn);return ()=>trace.push('own')}}); },
  ['getter-throws'](target,type,trace) { Object.defineProperty(target,'on'+type,{configurable:true,get(){throw new TypeError('getter failure')}});const e=new Event(type);try {target.dispatchEvent(e)}catch(error){trace.push({error:error.name,phase:e.eventPhase,current:e.currentTarget===target})};delete target['on'+type]; },
};
for (const [name,value] of Object.entries({null:null,undefined:undefined,number:42,false:false,string:'handler',symbol:Symbol('handler'),bigint:1n,object:{},handleEvent:{handleEvent(){throw new Error('must not call object')}}})) {
 cases['assigned-'+name]=(target,type,trace)=>{target['on'+type]=value;const observed=target['on'+type];trace.push({same:Object.is(observed,value),kind:typeof observed,isNull:observed===null});};
}

 const results=[];
 for(const kind of ['WebSocket/open','WebSocket/message','WebSocket/error','WebSocket/close','MessagePort/message','EventTarget/message','AbortSignal/abort']) {
  for(const [name,configure] of Object.entries(cases)) {
   const [brand,type]=kind.split('/'), owner=brand==='WebSocket'?new WebSocketPair():brand==='MessagePort'?new MessageChannel():brand==='AbortSignal'?new AbortController():new EventTarget();
   const target=brand==='WebSocket'?owner[0]:brand==='MessagePort'?owner.port1:brand==='AbortSignal'?owner.signal:owner, trace=[];
   let error=null;
   try { configure(target,type,trace); target.dispatchEvent(new Event(type)); trace.push('|'); target.dispatchEvent(new Event(type)); }
   catch(e){ error=e.name; }
   results.push({name:kind+'/'+name,trace,error});
   if(brand==='MessagePort'){owner.port1.close();owner.port2.close();}
  }
 }
 return results;
}

export default {
  async fetch(request) {
    const url = new URL(request.url);

    if (url.pathname === "/public-events") {
      return new Response(JSON.stringify(observePublicEvents(), (_key, value) =>
        typeof value === 'bigint' ? { bigint: String(value) } :
        typeof value === 'symbol' ? { symbol: String(value) } : value),
        { headers: { 'content-type': 'application/json' } });
    }
    if (url.pathname === "/runtime-events") {
      return Response.json(await observeRuntimeEvents());
    }
    if (url.pathname === "/handler-attributes") {
      return Response.json(observeEventHandlerAttributes());
    }
    if (url.pathname === "/listener-events") {
      return Response.json(observeListenerEvents());
    }

    if (url.pathname === "/message-port-queue") {
      return Response.json({
        explicit: await observeMessageQueue('explicit'),
        handler: await observeMessageQueue('handler'),
        nullHandler: await observeMessageQueue('null'),
      });
    }
    if (url.pathname === "/selftest") {
      return Response.json({ scenario: "api-compat", probes: 17 });
    }
    if (url.pathname === "/state") {
      const out = JSON.stringify(results);
      results = [];
      return new Response(out, { headers: { "content-type": "application/json" } });
    }
    if (url.pathname === '/handler-error') return handlerErrorProbe(url);
    if (url.pathname === '/handler-error-state') return Response.json(handlerErrorEvents);
    if (url.pathname === "/refuse") {
      // C16: outbound dial to a refused port reports the error event's
      // identity and properties. The handler AWAITS the terminal
      // events: the reference cancels unattended async work when the
      // request context ends.
      const target = url.searchParams.get("target");
      let refusedEvent;
      let refusedSocket;
      const settled = new Promise(resolve => {
        let resolved = false;
        const finish = () => { if (!resolved) { resolved = true; resolve(); } };
        try {
          const socket = new WebSocket(target);
          refusedSocket = socket;
          socket.onerror = event => {
            refusedEvent = event;
            record("C16", JSON.stringify({
              ctor: event?.constructor?.name ?? null,
              hasMessage: event !== null && "message" in event,
              hasError: event !== null && "error" in event,
              errorType: typeof event.error,
              errorIsError: event.error instanceof Error,
              errorName: event.error?.name ?? null,
              errorMessage: event.error?.message ?? null,
              message: event.message,
              state: socket.readyState,
              target: event.target === socket,
              currentTarget: event.currentTarget === socket,
              isTrusted: event.isTrusted, phase: event.eventPhase,
              filename: event.filename, lineno: event.lineno, colno: event.colno,
            }));
          };
          socket.onclose = finish;
        } catch (error) {
          record("C16", JSON.stringify({ threw: error.name }));
          finish();
        }
      });
      let fallback;
      try {
        await Promise.race([settled, new Promise(r => { fallback = setTimeout(r, 4000); })]);
      } finally { clearTimeout(fallback); }
      if (refusedEvent) {
        const after = { phase: refusedEvent.eventPhase, isTrusted: refusedEvent.isTrusted,
          currentTarget: refusedEvent.currentTarget === refusedSocket };
        const target = new EventTarget();
        let redispatched;
        target.addEventListener('error', event => { redispatched = {
          phase: event.eventPhase, isTrusted: event.isTrusted, currentTarget: event.currentTarget === target,
        }; });
        target.dispatchEvent(refusedEvent);
        record('C16-lifecycle', JSON.stringify({ after, redispatched }));
      }
      return new Response("refused-probed");
    }
    if (url.pathname === "/c15log") {
      globalThis.__c15log ??= [];
      const out = globalThis.__c15log;
      globalThis.__c15log = [];
      return Response.json(out);
    }
    if (url.pathname === "/dial") {
      // C12 real-header matrix: dial the peer with each protocols input
      // and report the constructor outcome plus the selected protocol.
      const tag = url.searchParams.get("case");
      const target = url.searchParams.get("target");
      const inputs = {
        omitted: undefined,
        undef: undefined,
        null: null,
        num: 123,
        bool: true,
        obj: { toString() { return "chat"; } },
        str: "chat",
        arr: ["chat", "super"],
        iter: { [Symbol.iterator]() { let i = 0; return { next: () => i === 0 ? (i++, { value: "chat", done: false }) : i === 1 ? (i++, { value: "super", done: false }) : { done: true } }; } },
      };
      // The handler awaits the dial's terminal events: the reference
      // cancels unattended async work when the request context ends.
      const settled = new Promise(resolve => {
        let done = false;
        const finish = () => { if (!done) { done = true; resolve(); } };
        try {
          const socket = new WebSocket(target, inputs[tag]);
          socket.onopen = () => {
            record("C12-" + tag,
              JSON.stringify({ constructed: true, state: socket.readyState, protocolAtOpen: socket.protocol }));
            // the peer initiates the close (1000): the reference does
            // not flush a client close(1000) in this probe's context
          };
          // Observe establishment through terminal close. Keep the error
          // handler installed while the completed HTTP context is cleaned up.
          socket.onerror = () => { if (!done) record("C12-" + tag, JSON.stringify({ error: true })); };
          socket.onclose = event => {
            record("C12-" + tag,
              JSON.stringify({ constructed: true, state: 3, protocolAtClose: socket.protocol, code: event.code, clean: event.wasClean }));
            finish();
          };
        } catch (error) {
          record("C12-" + tag, JSON.stringify({ constructed: false, threw: error.name }));
          finish();
        }
      });
      // the fallback timer MUST be cleared when settled wins: a
      // pending continuation on a terminal request token poisons the
      // worker (terminal continuation leak)
      let timer;
      try {
        await Promise.race([settled, new Promise(r => { timer = setTimeout(r, 700); })]);
      } finally { clearTimeout(timer); }
      return new Response("dial-settled");
    }
    if (url.pathname === "/delayed") {
      // C14: return the 101 FIRST, accept the peer 80ms later
      const pair = new WebSocketPair();
      const server = pair[1];
      server.onmessage = event => server.send(event.data);
      const response = new Response(null, { status: 101, webSocket: pair[0] });
      setTimeout(() => server.accept(), 80);
      return response;
    }
    if (["/retransfer", "/precreated", "/fresh"].includes(url.pathname)) {
      const make = () => {
        const pair = new WebSocketPair();
        pair[1].accept();
        pair[1].onmessage = event => pair[1].send(event.data);
        return new Response(null, { status: 101, webSocket: pair[0] });
      };
      if (url.pathname === "/fresh") return make();
      if (url.pathname === "/retransfer") {
        globalThis.__retransfer ??= make();
        return globalThis.__retransfer;
      }
      if (globalThis.__precreated) return globalThis.__precreated;
      globalThis.__precreated = make();
      return make();
    }
    if (url.pathname === "/accepted-endpoint") {
      // §30: an already-accepted endpoint commits 101 before its coupling
      // fails. Both engines then abort the transport and record TypeError.
      const pair = new WebSocketPair();
      pair[1].accept();
      return new Response(null, { status: 101, webSocket: pair[1] });
    }
    if (url.pathname !== "/ws") {
      return new Response("expected /ws", { status: 400 });
    }

    const pair = new WebSocketPair();
    const server = pair[1];
    server.accept();
    server.binaryType = "arraybuffer";
    const probe = url.searchParams.get("case") ?? "main";
    let probeEvents = [];

    if (probe === "handler-attributes") {
      const trace = [];
      function sharedMessage() { trace.push(this === globalThis ? 'attribute' : 'listener'); }
      server.addEventListener('message', sharedMessage);
      server.onmessage = sharedMessage;
      server.addEventListener('message', event => {
        server.send(JSON.stringify({ data: event.data, calls: trace.splice(0) }));
        if (event.data === 'first') server.onmessage = null;
        if (event.data === 'second') server.onmessage = () => trace.push('restored');
      });
      function sharedClose() { record('handler-close', this === globalThis ? 'attribute' : 'listener'); }
      server.addEventListener('close', sharedClose);
      server.onclose = sharedClose;
    } else if (probe === "string-send") {
      server.onmessage = event => {
        if (event.data !== "go") return;
        const reads = [], errors = [];
        for (const value of ['', 'ASCII\0end', '\u00e9\u4e2d', '\ud83d\ude80',
          '\ud800', '\udc00', '\ud800\ud801', '\udc00\ud800',
          '\ud800A\udfffB\udbff\udfff', 123n, null, undefined]) server.send(value);
        server.send({ [Symbol.toPrimitive](hint) { reads.push('primitive:' + hint); return '\ud800object\udc00'; } });
        server.send({ toString() { reads.push('toString'); return '\udc00method'; },
          valueOf() { throw new Error('must not read valueOf'); } });
        for (const value of [Symbol('s'), Object(Symbol('s')),
          { [Symbol.toPrimitive](hint) { reads.push('symbol:' + hint); return Symbol('s'); } }]) {
          try { server.send(value); errors.push('accepted'); }
          catch (error) { errors.push(error.name); }
        }
        const sentinel = new Error('conversion sentinel');
        let sameError = false, replacedCalls = 0;
        try { server.send({ toString() { reads.push('throws'); throw sentinel; } }); }
        catch (error) { sameError = error === sentinel; }
        const descriptor = Object.getOwnPropertyDescriptor(String.prototype, 'toWellFormed');
        Object.defineProperty(String.prototype, 'toWellFormed', {
          configurable: true, writable: true,
          value() { replacedCalls++; throw new Error('application replacement'); },
        });
        try {
          server.send('\ud800captured');
          server.send(JSON.stringify({ reads, errors, sameError, replacedCalls }));
          // close(reason) uses USVString even though send() preserves WTF-8.
          // Keep the replacement installed through both conversions.
          server.close(1000, '\ud800A\udfffB\udbff\udfff');
        }
        finally {
          if (descriptor) Object.defineProperty(String.prototype, 'toWellFormed', descriptor);
          else delete String.prototype.toWellFormed;
        }
      };
    } else if (probe === "main") {
      server.onmessage = event => {
        if (event.data !== "run") return;
        // C01/C02: send argument conversion
        try { server.send(); record("C01", "accepted"); }
        catch (error) { record("C01", error.name); }
        try { server.send(Symbol("s")); record("C02", "accepted"); }
        catch (error) { record("C02", error.name); }
        // C05/C06: CloseEvent conversions
        try { record("C05", String(new CloseEvent("x", { code: 1000.5 }).code)); }
        catch (error) { record("C05", "threw:" + error.name); }
        try { record("C06", String(new CloseEvent("x", null).code)); }
        catch (error) { record("C06", "threw:" + error.name); }
        // C07: binaryType setter converts via ToString
        try {
          server.binaryType = { toString() { return "arraybuffer"; } };
          record("C07", server.binaryType);
        } catch (error) { record("C07", "threw:" + error.name); }
        // C08: addEventListener callback conversion
        try { server.addEventListener("probe", 42); record("C08", "accepted"); }
        catch (error) { record("C08", error.name); }
        // C09: once is captured at registration time
        try {
          let fired = 0;
          const counter = () => { fired += 1; };
          const options = { once: true };
          server.addEventListener("apic9", counter, options);
          options.once = false;
          server.dispatchEvent(new Event("apic9"));
          server.addEventListener("apic9", counter, { once: true });
          server.dispatchEvent(new Event("apic9"));
          record("C09", String(fired));
        } catch (error) { record("C09", "threw:" + error.name); }
        // round5-7 §3: expanded conversion matrix (CloseEvent-only
        // probes here; close() probes get their own connections)
        try { record("E-ce-reason-bigint", new CloseEvent("x", { reason: 123n }).reason); }
        catch (error) { record("E-ce-reason-bigint", error.name); }
        try { record("E-ce-reason-null", new CloseEvent("x", { reason: null }).reason); }
        catch (error) { record("E-ce-reason-null", "threw:" + error.name); }
        try { record("E-ce-boxed-bigint", String(new CloseEvent("x", { code: Object(1000n) }).code)); }
        catch (error) { record("E-ce-boxed-bigint", "threw:" + error.name); }
        try { record("E-ce-code-nan", String(new CloseEvent("x", { code: NaN }).code)); }
        catch (error) { record("E-ce-code-nan", "threw:" + error.name); }
        try { record("E-ce-code-inf", String(new CloseEvent("x", { code: Infinity }).code)); }
        catch (error) { record("E-ce-code-inf", "threw:" + error.name); }
        try { record("E-ce-lone-surrogate", new CloseEvent("x", { reason: "\ud800" }).reason); }
        catch (error) { record("E-ce-lone-surrogate", error.name); }
        try {
          const reads = [];
          const ce = new CloseEvent("x", {
            get code() { reads.push("code"); return 1000; },
            get reason() { reads.push("reason"); return "r"; },
            get wasClean() { reads.push("wasClean"); return true; },
          });
          record("E-getter-order", JSON.stringify({ reads, values: [ce.code, ce.reason, ce.wasClean] }));
        } catch (error) { record("E-getter-order", "threw:" + error.name); }
        // fix-review §2: numeric semantics (close validates the raw
        // number; CloseEvent truncates toward zero without modulo;
        // BigInt is a TypeError in both)
        try { server.close(66536); record("N-close-66536", "accepted"); }
        catch (error) { record("N-close-66536", error.name); }
        try { server.close(-64536); record("N-close-neg", "accepted"); }
        catch (error) { record("N-close-neg", error.name); }
        try { server.close(1000n); record("N-close-bigint", "accepted"); }
        catch (error) { record("N-close-bigint", error.name); }
        try { record("N-ce-65537", String(new CloseEvent("x", { code: 65537 }).code)); }
        catch (error) { record("N-ce-65537", "threw:" + error.name); }
        try { record("N-ce-neg1", String(new CloseEvent("x", { code: -1 }).code)); }
        catch (error) { record("N-ce-neg1", "threw:" + error.name); }
        try { record("N-ce-bigint", String(new CloseEvent("x", { code: 1000n }).code)); }
        catch (error) { record("N-ce-bigint", "threw:" + error.name); }
        try { record("N-ce-frac", String(new CloseEvent("x", { code: 1000.5 }).code)); }
        catch (error) { record("N-ce-frac", "threw:" + error.name); }
        // fix-review §5: Symbol conversions
        try { record("N-ce-reason-symbol", new CloseEvent("x", { reason: Symbol("s") }).reason); }
        catch (error) { record("N-ce-reason-symbol", error.name); }
        try { server.binaryType = Symbol("b"); record("N-bt-symbol", server.binaryType); }
        catch (error) { record("N-bt-symbol", error.name); }
        server.binaryType = "arraybuffer";
        // fix-review §4: Response 101 webSocket type check
        for (const [id, value] of [["R-obj", {}], ["R-bool", true], ["R-num", 1]]) {
          try { new Response(null, { status: 101, webSocket: value }); record(id, "accepted"); }
          catch (error) { record(id, error.name); }
        }
        try { record("E-ce-string-init", String(new CloseEvent("x", "value").code)); }
        catch (error) { record("E-ce-string-init", error.name); }
        try { record("E-ce-boolean-init", String(new CloseEvent("x", true).code)); }
        catch (error) { record("E-ce-boolean-init", error.name); }
        try { record("E-ce-null-type", new CloseEvent(null).type); }
        catch (error) { record("E-ce-null-type", error.name); }
        const local = () => { const p = new WebSocketPair(); p[0].accept(); p[1].accept(); return p[0]; };
        // Promote the independent conversion/listener probes into the
        // mandatory fixture matrix, including cleanup and callback identity.
        const check = (id, callback) => {
          try { record(id, JSON.stringify({value: callback()})); }
          catch (error) { record(id, JSON.stringify({error: error.name})); }
        };
        check('P-close-nan', () => local().close(NaN));
        check('P-close-reason-without-code', () => local().close(undefined, 'why'));
        check('P-local-closing-state', () => { const ws = local(); ws.close(1000); return ws.readyState; });
        check('P-invalid-listener-object', () => local().addEventListener('probe', {}));
        check('P-noncallable-handler', () => local().addEventListener('probe', {handleEvent:42}));
        for (const alreadyAborted of [false, true]) check('P-abort-readd-' + alreadyAborted, () => {
          const ws = local(), controller = new AbortController(); let calls = 0;
          const callback = () => calls++;
          if (alreadyAborted) controller.abort();
          ws.addEventListener('probe', callback, {signal:controller.signal});
          if (!alreadyAborted) controller.abort();
          ws.addEventListener('probe', callback); ws.dispatchEvent(new Event('probe'));
          ws.close(1000); return calls;
        });
        check('P-listener-receiver', () => {
          const ws = local(); let receiver = false;
          ws.addEventListener('probe', function() { receiver = this === ws; });
          ws.dispatchEvent(new Event('probe')); ws.close(1000); return receiver;
        });
        try { local().close(1000, null); record("E-close-reason-null", "accepted"); }
        catch (error) { record("E-close-reason-null", error.name); }
        try {
          const reads = [];
          local().close({valueOf(){reads.push("code"); return 1000;}},
                        {toString(){reads.push("reason"); return "hi";}});
          record("E-close-read-order", JSON.stringify(reads));
        } catch (error) { record("E-close-read-order", error.name); }
        const primitive = { [Symbol.toPrimitive](){return 1000;}, valueOf(){throw Error("must not read valueOf");} };
        try { record("E-ce-primitive", String(new CloseEvent("x", {code:primitive}).code)); }
        catch (error) { record("E-ce-primitive", error.name); }
        try { local().close(primitive); record("E-close-primitive", "accepted"); }
        catch (error) { record("E-close-primitive", error.name); }
        // C10-C12: constructor conversions
        try { new WebSocket(); record("C10", "accepted"); }
        catch (error) { record("C10", error.name); }
        try { new WebSocket(Symbol("s")); record("C11", "accepted"); }
        catch (error) { record("C11", error.name); }
        try {
          const socket = new WebSocket("ws://127.0.0.1:1/echo", null);
          // an unhandled dial failure is fatal in the reference; the
          // probe only asserts CONSTRUCTION, so the failure is observed
          socket.onerror = () => {};
          socket.onclose = () => {};
          record("C12", "constructed:" + socket.readyState);
        } catch (error) { record("C12", error.name); }
        // C13: Response 101 without an endpoint is a forbidden status
        try { new Response(null, { status: 101 }); record("C13", "accepted"); }
        catch (error) { record("C13", error.name); }
        // C14: Response 101 constructs even while the peer endpoint is
        // not yet accepted; commit readiness is a send-time check
        try {
          const probePair = new WebSocketPair();
          new Response(null, { status: 101, webSocket: probePair[0] });
          record("C14", "constructed");
        } catch (error) { record("C14", error.name); }
        server.send("done");
      };
    } else if (probe === "e-close") {
      server.onmessage = event => {
        if (event.data !== "go") return;
        try { server.close(1000, 123n); record("E-close-reason-bigint", "accepted"); }
        catch (error) { record("E-close-reason-bigint", error.name); }
        try { server.close(Object(1000n)); record("E-close-boxed-bigint", "accepted"); }
        catch (error) { record("E-close-boxed-bigint", error.name); }
      };
    } else if (probe === "c03") {
      server.onmessage = event => {
        if (event.data !== "go") return;
        try { server.close(1000.5); record("C03", "accepted"); }
        catch (error) { record("C03", error.name); }
      };
    } else if (probe === "c04") {
      server.onmessage = event => {
        if (event.data !== "go") return;
        try { server.close(1000, Symbol("s")); record("C04", "accepted"); }
        catch (error) { record("C04", error.name); }
        // either way the probe connection ends here
        server.close(1000);
      };
    } else if (probe === "c15") {
      globalThis.__c15log ??= [];
      server.onmessage = async event => {
        const tag = event.data;
        record("C15", "start:" + tag);
        if (tag === "a") {
          await new Promise(resolve => setTimeout(resolve, 80));
        }
        record("C15", "end:" + tag);
      };
    } else if (probe === "c17") {
      server.onmessage = event => {
        if (event.data !== "go") return;
        record('C17-listener', 'first');
        server.send("queued");
        throw new Error("boom");
      };
      server.addEventListener('message', () => record('C17-listener', 'second'));
    } else if (probe === "c18") {
      // Raw TCP on both pinned official assets receives the message and
      // Close(1000, "done") before termination. Undici may discard queued
      // events on reset; its event trace alone cannot establish frame loss.
      server.onmessage = event => {
        if (event.data !== "go") return;
        server.send("a-before");
        server.close(1000, "done");
        throw new Error("expected listener error");
      };
    } else if (probe === "c19") {
      // fix-review §5: an ASYNC rejection is NOT fatal — the next
      // message still flows and the client closes cleanly
      server.onmessage = async event => {
        if (event.data === "a") {
          server.send("a-sent");
          throw new Error("async rejection");
        }
        if (event.data === "await") {
          server.send("await-sent");
          await new Promise(resolve => setTimeout(resolve, 80));
          throw new Error("late rejection");
        }
        server.send("b-after:" + event.data);
      };
    }
    return new Response(null, { status: 101, webSocket: pair[0] });
  }
};
