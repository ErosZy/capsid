function descriptor(target, key) {
    const found = Object.getOwnPropertyDescriptor(target, key);
    if (!found) return null;
    return {enumerable: found.enumerable, configurable: found.configurable,
        hasValue: Object.hasOwn(found, 'value'), writable: found.writable ?? null,
        getterType: typeof found.get, getterName: found.get?.name ?? null,
        setterType: typeof found.set, setterName: found.set?.name ?? null};
}
function constructorShape() {
    const Constructor = globalThis.PromiseRejectionEvent;
    const result = {type: typeof Constructor};
    if (typeof Constructor !== 'function') return result;
    result.name = Constructor.name;
    result.length = Constructor.length;
    result.ownNames = Object.getOwnPropertyNames(Constructor).sort();
    result.globalDescriptor = descriptor(globalThis, 'PromiseRejectionEvent');
    result.prototypeDescriptor = descriptor(Constructor, 'prototype');
    result.prototypeNames = Object.getOwnPropertyNames(Constructor.prototype).sort();
    result.prototypeSymbols = Object.getOwnPropertySymbols(Constructor.prototype).map(String).sort();
    result.prototypeConstructor = descriptor(Constructor.prototype, 'constructor');
    result.prototypePromise = descriptor(Constructor.prototype, 'promise');
    result.prototypeReason = descriptor(Constructor.prototype, 'reason');
    result.tagDescriptor = descriptor(Constructor.prototype, Symbol.toStringTag);
    result.tagValue = Constructor.prototype[Symbol.toStringTag];
    result.prototypeInheritsEvent = Object.getPrototypeOf(Constructor.prototype) === Event.prototype;
    result.constructorInheritsEvent = Object.getPrototypeOf(Constructor) === Event;
    result.constructorInheritsFunction = Object.getPrototypeOf(Constructor) === Function.prototype;
    const promise = Promise.resolve(1);
    const init = {promise, reason: 'descriptor-reason'};
    const attempts = [
        ['new-valid', () => new Constructor('probe', init)],
        ['new-empty', () => new Constructor()],
        ['new-null', () => new Constructor(null, null)],
        ['call-valid', () => Constructor('probe', init)],
        ['call-empty', () => Constructor()],
        ['reflect-construct', () => Reflect.construct(Constructor, ['probe', init])],
        ['subclass', () => {class Sub extends Constructor {} return new Sub('probe', init);}],
        ['new-getter', () => new Constructor('probe', {get promise() {throw new RangeError('constructor-getter-sentinel');}})]
    ];
    result.attempts = [];
    for (const [label, attempt] of attempts) {
        try {
            const event = attempt();
            result.attempts.push({label, returned: typeof event, ownNames: Object.getOwnPropertyNames(event).sort(),
                promiseDescriptor: descriptor(event, 'promise'), reasonDescriptor: descriptor(event, 'reason'),
                reason: describe(event.reason), trusted: event.isTrusted});
        } catch (error) {result.attempts.push({label, error: describe(error)});}
    }
    return result;
}
const describe = value => ({type: typeof value, name: value?.name ?? null, message: value?.message ?? null,
    primitive: value === null ? 'null' : ['object', 'function'].includes(typeof value) ? null : String(value)});
const delay = milliseconds => new Promise(resolve => setTimeout(resolve, milliseconds));
function listenerOptions(name, trace) {
    if (name === 'false') return false;
    if (name === 'true') return true;
    if (name === 'undefined') return undefined;
    if (name === 'null') return null;
    if (name === 'capture') return {capture: true};
    if (name === 'passive') return {passive: true};
    if (name === 'both') return {capture: true, passive: true};
    if (name === 'once') return {once: true};
    if (name === 'capture-getter') return {get capture() {trace.push('capture'); return true;},
        get passive() {trace.push('passive'); return false;}, get once() {trace.push('once'); return false;}};
    if (name === 'passive-getter') return {get capture() {trace.push('capture'); return false;},
        get passive() {trace.push('passive'); return true;}, get once() {trace.push('once'); return false;}};
    if (name === 'getter-error') return {get capture() {trace.push('capture'); throw new RangeError('listener-option-sentinel');}};
    throw new Error('unknown option');
}
function listeners() {
    const result = [];
    for (const targetKind of ['event-target', 'message-port', 'global']) {
        for (const method of ['addEventListener', 'removeEventListener']) {
            for (const option of ['false', 'true', 'undefined', 'null', 'capture', 'passive', 'both', 'once',
                'capture-getter', 'passive-getter', 'getter-error']) {
                const row = {targetKind, method, option, trace: []};
                result.push(row);
                let target, channel;
                const callback = () => {};
                try {
                    if (targetKind === 'event-target') target = new EventTarget();
                    else if (targetKind === 'message-port') {channel = new MessageChannel(); target = channel.port1;}
                    else target = globalThis;
                    const type = targetKind === 'global' ? 'unhandledrejection' : 'probe';
                    try {
                        row.returnedUndefined = target[method](type, callback, listenerOptions(option, row.trace)) === undefined;
                    } catch (error) {row.error = {...describe(error), isTypeError: error instanceof TypeError, isRangeError: error instanceof RangeError};}
                    target.removeEventListener(type, callback, false);
                } catch (error) {row.outerError = describe(error);}
                finally {if (channel) {channel.port1.close(); channel.port2.close();}}
            }
        }
    }
    return result;
}
async function promiseEvents() {
    const cases = [];
    for (const kind of ['string', 'error', 'dom-exception']) {
        const reason = kind === 'string' ? 'promise-reason-sentinel' : kind === 'error'
            ? new Error('promise-reason-sentinel') : new DOMException('promise-reason-sentinel', 'AbortError');
        const row = {kind, events: []}; cases.push(row);
        let pending;
        const receive = event => {
            event.preventDefault();
            row.events.push({type: event.type, constructorName: event.constructor?.name ?? null,
                tag: Object.prototype.toString.call(event), isTrusted: event.isTrusted,
                ownNames: Object.getOwnPropertyNames(event).sort(),
                promiseDescriptor: descriptor(event, 'promise'), reasonDescriptor: descriptor(event, 'reason'),
                hasPromiseProperty: 'promise' in event, hasReasonProperty: 'reason' in event,
                promiseMatches: event.promise === pending, reasonMatches: event.reason === reason,
                reason: describe(event.reason),
                instanceOfPromiseRejectionEvent: typeof globalThis.PromiseRejectionEvent === 'function'
                    ? event instanceof globalThis.PromiseRejectionEvent : null});
        };
        addEventListener('unhandledrejection', receive);
        addEventListener('rejectionhandled', receive);
        try {
            pending = Promise.reject(reason);
            await delay(40);
            pending.catch(() => {});
            await delay(100);
        } finally {
            removeEventListener('unhandledrejection', receive);
            removeEventListener('rejectionhandled', receive);
        }
    }
    return cases;
}
export default {async fetch(request) {
    if (new URL(request.url).pathname === '/healthy') return new Response('healthy');
    const listenerResults = listeners();
    const promises = await promiseEvents();
    return Response.json({listenerResults, promises, promiseRejectionEventConstructor: typeof globalThis.PromiseRejectionEvent, constructorShape: constructorShape()});
}};
