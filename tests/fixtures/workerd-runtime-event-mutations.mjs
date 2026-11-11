const ownDescriptor = Object.getOwnPropertyDescriptor;
const define = Object.defineProperty;
const ownNames = Object.getOwnPropertyNames;
const prototypeOf = Object.getPrototypeOf;
const reflectSet = Reflect.set;
const reflectDelete = Reflect.deleteProperty;
const tag = Function.prototype.call.bind(Object.prototype.toString);
const preventDefault = Event.prototype.preventDefault;
const originalConstructor = PromiseRejectionEvent;
const originalPrototype = originalConstructor.prototype;
const originalGlobalDescriptor = ownDescriptor(globalThis, 'PromiseRejectionEvent');
const add = globalThis.addEventListener;
const remove = globalThis.removeEventListener;
const delay = ms => new Promise(resolve => setTimeout(resolve, ms));
const choices = ['baseline', 'write-fields', 'delete-fields', 'redefine-fields', 'replace-global-constructor',
    'replace-public-prototype', 'freeze-public-prototype', 'replace-intrinsics', 'poison-inherited-reason',
    'replace-global-dispatch', 'replace-weak-map-set', 'replace-weak-map-get', 'replace-weak-map-delete',
    'poison-descriptor-get', 'poison-descriptor-set', 'poison-event-init',
    'replace-public-prototype-null', 'replace-public-prototype-number'];

function descriptor(object, key) {
    const d = ownDescriptor(object, key);
    return d ? {enumerable: d.enumerable, configurable: d.configurable, writable: d.writable,
        hasValue: 'value' in d, getterType: typeof d.get, setterType: typeof d.set} : null;
}
function install(choice) {
    const restorations = [];
    const replace = (object, key, value) => {
        const previous = ownDescriptor(object, key);
        define(object, key, {value, writable: true, configurable: true});
        restorations.push(() => previous ? define(object, key, previous) : reflectDelete(object, key));
    };
    const fail = () => {throw new RangeError('public-intrinsic-replacement-sentinel');};
    if (choice === 'replace-global-constructor') {
        globalThis.PromiseRejectionEvent = function Replacement() {throw new RangeError('public-constructor-replacement');};
        restorations.push(() => originalGlobalDescriptor ? define(globalThis, 'PromiseRejectionEvent', originalGlobalDescriptor)
            : reflectDelete(globalThis, 'PromiseRejectionEvent'));
    } else if (choice === 'replace-public-prototype') {
        originalConstructor.prototype = {replacementPrototype: true};
        restorations.push(() => {originalConstructor.prototype = originalPrototype;});
    } else if (choice === 'freeze-public-prototype') Object.freeze(originalPrototype);
    else if (choice === 'replace-intrinsics') {
        replace(Reflect, 'construct', fail);
        replace(Object, 'defineProperties', fail);
        replace(Object, 'hasOwn', fail);
    } else if (choice === 'poison-inherited-reason') replace(Object.prototype, 'reason', 'inherited-reason-sentinel');
    else if (choice === 'replace-global-dispatch') replace(globalThis, 'dispatchEvent', fail);
    else if (choice === 'replace-weak-map-set') replace(WeakMap.prototype, 'set', fail);
    else if (choice === 'replace-weak-map-get') replace(WeakMap.prototype, 'get', fail);
    else if (choice === 'replace-weak-map-delete') replace(WeakMap.prototype, 'delete', fail);
    else if (choice === 'poison-descriptor-get') replace(Object.prototype, 'get', fail);
    else if (choice === 'poison-descriptor-set') replace(Object.prototype, 'set', fail);
    else if (choice === 'poison-event-init') {
        replace(Object.prototype, 'bubbles', true);
        replace(Object.prototype, 'cancelable', true);
        replace(Object.prototype, 'composed', true);
    } else if (choice === 'replace-public-prototype-null' || choice === 'replace-public-prototype-number') {
        originalConstructor.prototype = choice === 'replace-public-prototype-null' ? null : 17;
        restorations.push(() => {originalConstructor.prototype = originalPrototype;});
    }
    return () => {for (let i = restorations.length - 1; i >= 0; --i) restorations[i]();};
}
function snapshot(event, pending, reason) {
    return {type: event.type, tag: tag(event), trusted: event.isTrusted,
        bubbles: event.bubbles, cancelable: event.cancelable, composed: event.composed,
        originalPrototype: prototypeOf(event) === originalPrototype,
        originalConstructor: event.constructor === originalConstructor,
        ownNames: ownNames(event).sort(), promiseDescriptor: descriptor(event, 'promise'), reasonDescriptor: descriptor(event, 'reason'),
        promiseMatches: event.promise === pending, reasonMatches: event.reason === reason,
        hasPromise: 'promise' in event, hasReason: 'reason' in event,
        promiseType: typeof event.promise, reasonType: typeof event.reason,
        reasonText: event.reason === undefined ? null : typeof event.reason === 'string' ? event.reason : event.reason.message};
}
function action(choice, event) {
    const result = {};
    if (choice === 'write-fields') {
        result.reflectPromise = reflectSet(event, 'promise', 'replacement');
        result.reflectReason = reflectSet(event, 'reason', 'replacement');
        try {event.promise = 'replacement'; result.strictPromise = 'returned';}
        catch (error) {result.strictPromise = error.name;}
        try {event.reason = 'replacement'; result.strictReason = 'returned';}
        catch (error) {result.strictReason = error.name;}
    } else if (choice === 'delete-fields') {
        result.deletePromise = reflectDelete(event, 'promise');
        result.deleteReason = reflectDelete(event, 'reason');
    } else if (choice === 'redefine-fields') {
        define(event, 'promise', {value: 'replacement-promise'});
        define(event, 'reason', {value: 'replacement-reason'});
        result.redefined = true;
    }
    return result;
}
export default {async fetch(request) {
    const url = new URL(request.url);
    if (url.pathname === '/healthy') return new Response('healthy');
    const choice = url.searchParams.get('scenario') ?? 'baseline';
    if (!choices.includes(choice)) throw new Error('unknown mutation scenario');
    let pending;
    const reason = new Error('runtime-event-reason');
    const events = [];
    const receive = event => {
        preventDefault.call(event);
        const before = snapshot(event, pending, reason);
        const changed = action(choice, event);
        events.push({before, action: changed, after: snapshot(event, pending, reason)});
    };
    add.call(globalThis, 'unhandledrejection', receive);
    add.call(globalThis, 'rejectionhandled', receive);
    let restore;
    try {
        restore = install(choice);
        pending = Promise.reject(reason);
        await delay(40);
        pending.catch(() => {});
        await delay(100);
    } finally {
        restore?.();
        remove.call(globalThis, 'unhandledrejection', receive);
        remove.call(globalThis, 'rejectionhandled', receive);
    }
    return Response.json({scenario: choice, events});
}};
