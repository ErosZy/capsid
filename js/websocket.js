import core from 'tjs:internal/core';
import { configureByteStreamCompatibility, markNativeByteStream } from './streams.js';
import { ErrorEvent, MessageEvent, CloseEvent } from './events.js';
import { Event, dispatchTrustedEvent } from 'txiki-polyfills/event-target.js';
import { getAbortException } from 'txiki-polyfills/abort-controller.js';

const endpoints = new WeakMap();
const responseSockets = new WeakMap();
const encoder = new TextEncoder();
const decoder = new TextDecoder();
const stringToWellFormed = Function.prototype.call.bind(String.prototype.toWellFormed);
const internalEndpoint = Symbol('WebSocketPair endpoint');
const handlerErrorTypes = {Error, TypeError, RangeError, ReferenceError, SyntaxError, URIError, EvalError, AggregateError};
const handlerErrorString = String;
const handlerErrorHasOwn = Object.hasOwn;
const handlerErrorToString = Function.prototype.call.bind(Error.prototype.toString);
const handlerErrorDefineProperty = Object.defineProperty;
const handlerErrorPromise = Promise;
const createHttpGzipDecoder = core.createHttpGzipDecoder;
const intrinsicLowerCase = Function.prototype.call.bind(String.prototype.toLowerCase);
let fetchReadableStream, fetchNativeGetReader, fetchNativeBYOBRead;
const fetchStreamContexts = new WeakMap();
const ordinaryFetchBodyTag = Symbol('ordinary fetch body');
const fetchReaderContexts = new WeakMap();
const fetchReaderKinds = new WeakMap();
const fetchReleasedReaders = new WeakMap();
const readerClosedPromises = new WeakMap();
const intrinsicApply = Reflect.apply;
const intrinsicConstruct = Reflect.construct;
const intrinsicThen = Promise.prototype.then;
const intrinsicPromiseResolve = Promise.resolve.bind(Promise);
const intrinsicPromiseReject = Function.prototype.call.bind(Promise.reject, Promise);
const intrinsicSet = Set;
const intrinsicWeakGet = Function.prototype.call.bind(WeakMap.prototype.get);
const intrinsicWeakSet = Function.prototype.call.bind(WeakMap.prototype.set);
const intrinsicWeakDelete = Function.prototype.call.bind(WeakMap.prototype.delete);
const intrinsicSetAdd = Function.prototype.call.bind(Set.prototype.add);
const intrinsicSetDelete = Function.prototype.call.bind(Set.prototype.delete);
const intrinsicSetForEach = Function.prototype.call.bind(Set.prototype.forEach);
const intrinsicSetSize = Function.prototype.call.bind(Object.getOwnPropertyDescriptor(Set.prototype, 'size').get);
const intrinsicWeakRef = WeakRef;
const intrinsicDeref = Function.prototype.call.bind(WeakRef.prototype.deref);
const intrinsicFinalizationRegistry = FinalizationRegistry;
const intrinsicRegister = Function.prototype.call.bind(FinalizationRegistry.prototype.register);
const intrinsicUnregister = Function.prototype.call.bind(FinalizationRegistry.prototype.unregister);
const intrinsicGetPrototypeOf = Object.getPrototypeOf;
const intrinsicIsView = ArrayBuffer.isView;
const intrinsicTypedPrototype = intrinsicGetPrototypeOf(Uint8Array.prototype);
const intrinsicTypedTag = Object.getOwnPropertyDescriptor(intrinsicTypedPrototype, Symbol.toStringTag).get;
const intrinsicTypedBuffer = Object.getOwnPropertyDescriptor(intrinsicTypedPrototype, 'buffer').get;
const intrinsicTypedOffset = Object.getOwnPropertyDescriptor(intrinsicTypedPrototype, 'byteOffset').get;
const intrinsicTypedLength = Object.getOwnPropertyDescriptor(intrinsicTypedPrototype, 'byteLength').get;
const intrinsicTypedElements = Object.getOwnPropertyDescriptor(intrinsicTypedPrototype, 'length').get;
const intrinsicByteSet = Function.prototype.call.bind(Uint8Array.prototype.set);
const intrinsicViewBuffer = Object.getOwnPropertyDescriptor(DataView.prototype, 'buffer').get;
const intrinsicViewOffset = Object.getOwnPropertyDescriptor(DataView.prototype, 'byteOffset').get;
const intrinsicViewLength = Object.getOwnPropertyDescriptor(DataView.prototype, 'byteLength').get;
const intrinsicBigInt = BigInt;
const intrinsicToPrimitive = Symbol.toPrimitive;
const intrinsicBufferLength = Object.getOwnPropertyDescriptor(ArrayBuffer.prototype, 'byteLength').get;
const intrinsicBufferTransfer = ArrayBuffer.prototype.transfer;
const byobObject = value => value !== null && (typeof value === 'object' || typeof value === 'function');
function byobMinimum(value) {
    // Convert application values exactly once. Do not catch and replace an
    // exception thrown by a getter, valueOf(), or Symbol.toPrimitive hook.
    if (byobObject(value)) {
        const object = value, exotic = object[intrinsicToPrimitive];
        if (exotic !== undefined && exotic !== null) {
            if (typeof exotic !== 'function') {
                const text = byobObject(exotic) ? '#<Object>' : handlerErrorString(exotic);
                throw new handlerErrorTypes.TypeError(`'${text}' returned for property 'Symbol(Symbol.toPrimitive)' of object '#<Object>' is not a function`);
            }
            value = intrinsicApply(exotic, object, ['number']);
        } else {
            for (const key of ['valueOf', 'toString']) {
                const method = object[key];
                if (typeof method === 'function') {
                    value = intrinsicApply(method, object, []);
                    if (!byobObject(value)) break;
                }
            }
        }
        if (byobObject(value)) throw new handlerErrorTypes.TypeError('Cannot convert object to primitive value');
    }
    if (typeof value === 'bigint') throw new handlerErrorTypes.TypeError('Cannot convert a BigInt value to a number');
    if (typeof value === 'symbol') throw new handlerErrorTypes.TypeError('Cannot convert a Symbol value to a number');
    const number = +value;
    if (number !== number || number === Infinity || number === -Infinity) return 0;
    if (number < -2147483648 || number > 2147483647) throw new handlerErrorTypes.TypeError(
        'Value out of range. Must be between -2147483648 and 2147483647 (inclusive).');
    return number | 0;
}
function fetchReaderContext(reader, byob) {
    // Reader branding outlives releaseLock(). Reject a borrowed method before
    // reading its arguments or touching the other reader kind's I/O state.
    const kind = intrinsicWeakGet(fetchReaderKinds, reader);
    if (kind !== undefined && kind !== byob) throw new handlerErrorTypes.TypeError(
        'Illegal invocation: function called with incorrect `this` reference. '
        + 'See https://developers.cloudflare.com/workers/observability/errors/#illegal-invocation-errors for details.');
    return intrinsicWeakGet(fetchReaderContexts, reader);
}
// Internal event tasks return native promises. Observe them with the captured
// intrinsic so resolving the scheduler cannot call an application replacement
// of Promise.prototype.then while an event handler is still running.
function runChannelEvent(generation, task, onError) {
    const settle = () => core.capsidChannelEventSettled(generation);
    const failed = error => {try {onError(error);} finally {settle();}};
    intrinsicApply(intrinsicThen, intrinsicPromiseResolve(), [() => {
        let result;
        try {result = task();} catch (error) {failed(error); return;}
        intrinsicApply(intrinsicThen, result, [settle, failed]);
    }]);
}
const observeStreamPromise = core.capsidStreamObservePromise;
const adoptNativeRead = promise => observeStreamPromise(new handlerErrorPromise(resolve => resolve(promise)));
let fetchStreamsConfigured = false;
export function configureFetchChannelStreams(constructor) {
    fetchReadableStream = constructor;
    if (fetchStreamsConfigured) return;
    fetchStreamsConfigured = true;
    configureByteStreamCompatibility(detail => internalHandlerError(detail).error, core.capsidStreamBufferAlias, observeStreamPromise);
    const streamPrototype = constructor.prototype;
    const nativeReader = globalThis.ReadableStreamDefaultReader;
    const readerPrototype = nativeReader.prototype;
    const nativeGetReader = streamPrototype.getReader, nativeRead = readerPrototype.read;
    const nativeStreamCancel = streamPrototype.cancel;
    Object.defineProperty(streamPrototype, 'cancel', {...Object.getOwnPropertyDescriptor(streamPrototype, 'cancel'), value: function cancel(...args) {
        const context = intrinsicWeakGet(fetchStreamContexts, this);
        if (context?.branch && !this.locked) {
            this._disturbed = true;
            try {context.checkContext();}
            catch (error) {return intrinsicPromiseReject(error?.message === crossRequestMessage
                ? new handlerErrorTypes.Error(crossRequestBranchSourceMessage) : error);}
        }
        return intrinsicApply(nativeStreamCancel, this, args);
    }});
    fetchNativeGetReader = nativeGetReader;
    const nativeCancel = readerPrototype.cancel, nativeReleaseLock = readerPrototype.releaseLock;
    const closed = Object.getOwnPropertyDescriptor(readerPrototype, 'closed');
    const nativeBYOBReader = globalThis.ReadableStreamBYOBReader, byobPrototype = nativeBYOBReader.prototype;
    const nativeBYOBRead = byobPrototype.read, nativeBYOBCancel = byobPrototype.cancel, nativeBYOBRelease = byobPrototype.releaseLock;
    fetchNativeBYOBRead = nativeBYOBRead;
    const byobClosed = Object.getOwnPropertyDescriptor(byobPrototype, 'closed');
    const checkRelease = info => {
        let pending = false;
        if (info) intrinsicSetForEach(info.pending, record => {if (!record.foreign && core.capsidFetchReadPending(record.promise)) pending = true;});
        if (pending) throw new handlerErrorTypes.TypeError('Cannot call releaseLock() on a reader with outstanding read promises.');
    };
    const readerScopeError = (error, info) => error?.message === crossRequestMessage
        ? new handlerErrorTypes.Error(info?.context.branch ? crossRequestBranchSourceMessage : crossRequestCancelerMessage) : error;
    const foreignRead = (info, error, view) => {
        if (error?.message !== crossRequestMessage) return intrinsicPromiseReject(error);
        info.stream._disturbed = true;
        if (view) intrinsicApply(intrinsicBufferTransfer, intrinsicApply(intrinsicTypedBuffer, view, []), []);
        if (info.context.rejectForeignRead) {
            let pending = false;
            intrinsicSetForEach(info.pending, record => {if (core.capsidFetchReadPending(record.promise)) pending = true;});
            if (pending) return intrinsicPromiseReject(new handlerErrorTypes.TypeError(
                'This ReadableStream only supports a single pending read request at a time.'));
            // The source scope check occupies a native read slot until its
            // rejection settles. A second read sees that outstanding slot.
            const result = intrinsicApply(intrinsicThen, intrinsicPromiseResolve(), [() => {throw info.context.rejectForeignRead(error);}]);
            const record = {promise: result};intrinsicSetAdd(info.pending, record);
            return adoptNativeRead(intrinsicApply(intrinsicThen, result, [undefined, failure => {
                intrinsicSetDelete(info.pending, record);throw failure;
            }]));
        }
        // The reference retains its pending native read slot after the
        // scope rejection, until an authorized cancellation clears it.
        intrinsicSetAdd(info.pending, {promise: new handlerErrorPromise(() => {}), foreign: true});
        return intrinsicPromiseReject(readerScopeError(error));
    };
    const associateReader = (stream, reader, byob = false) => {
        intrinsicWeakSet(fetchReaderKinds, reader, byob);
        const context = intrinsicWeakGet(fetchStreamContexts, stream);
        if (context) {
            const info = {stream, context, pending: new intrinsicSet()};
            if (byob) {
                // A partial native EOF is a nonempty read. The public closed
                // promise settles when an empty EOF read or cancellation is
                // observed, even if the byte controller already had to close
                // to return a buffer containing fewer than min bytes.
                info.byteClosed = false; info.closeWanted = false;
                info.closed = observeStreamPromise(new handlerErrorPromise((resolve, reject) => {info.resolveClosed = resolve; info.rejectClosed = reject;}));
                intrinsicApply(intrinsicThen, info.closed, [undefined, () => {}]);
                intrinsicApply(intrinsicThen, observeStreamPromise(intrinsicApply(byobClosed.get, reader, [])), [() => {
                    info.byteClosed = true; if (info.closeWanted) info.resolveClosed();
                }, error => info.rejectClosed(error)]);
            }
            intrinsicWeakSet(fetchReaderContexts, reader, info);
        }
        return reader;
    };
    // Construction can acquire a reader without calling getReader(). Keep the
    // original constructor's branding and newTarget, including derived readers.
    const readerConstructor = new Proxy(nativeReader, {construct(target, args, newTarget) {
        return associateReader(args[0], intrinsicConstruct(target, args, newTarget));
    }});
    Object.defineProperty(globalThis, 'ReadableStreamDefaultReader', {
        ...Object.getOwnPropertyDescriptor(globalThis, 'ReadableStreamDefaultReader'), value: readerConstructor
    });
    Object.defineProperty(readerPrototype, 'constructor', {
        ...Object.getOwnPropertyDescriptor(readerPrototype, 'constructor'), value: readerConstructor
    });
    Object.defineProperty(streamPrototype, 'getReader', {...Object.getOwnPropertyDescriptor(streamPrototype, 'getReader'), value: function getReader(...args) {
        const reader = intrinsicApply(nativeGetReader, this, args);
        return associateReader(this, reader, intrinsicGetPrototypeOf(reader) === byobPrototype);
    }});
    Object.defineProperty(readerPrototype, 'read', {...Object.getOwnPropertyDescriptor(readerPrototype, 'read'), value: function read(...args) {
        const info = fetchReaderContext(this, false);
        if (info) {
            let pending = false;
            intrinsicSetForEach(info.pending, record => {if (core.capsidFetchReadPending(record.promise)) pending = true;});
            if (pending) return intrinsicPromiseReject(new handlerErrorTypes.TypeError(
                'This ReadableStream only supports a single pending read request at a time.'));
        }
        if (info) {try {info.context.checkContext();} catch (error) {return foreignRead(info, error);}}
        const result = observeStreamPromise(intrinsicApply(nativeRead, this, args));
        if (!info) return observeStreamPromise(result);
        if (!core.capsidFetchReadPending(result)) return adoptNativeRead(result);
        const record = {promise: result, cancelled: false, reason: undefined};
        intrinsicSetAdd(info.pending, record);
        return adoptNativeRead(intrinsicApply(intrinsicThen, result, [value => {
            intrinsicSetDelete(info.pending, record);
            // Native I/O cancellation is observed after closed/cancel have
            // settled. Promise adoption preserves that pinned-reference order.
            return record.cancelled ? intrinsicPromiseReject(record.reason) : value;
        }, error => {intrinsicSetDelete(info.pending, record); throw error;}]));
    }});
    Object.defineProperty(readerPrototype, 'cancel', {...Object.getOwnPropertyDescriptor(readerPrototype, 'cancel'), value: function cancel(...args) {
        const info = fetchReaderContext(this, false);
        if (info) {
            info.stream._disturbed = true;
            try {info.context.checkContext();} catch (error) {return intrinsicPromiseReject(readerScopeError(error, info));}
            intrinsicSetForEach(info.pending, record => {
                if (record.foreign) {intrinsicSetDelete(info.pending, record); return;}
                if (!core.capsidFetchReadPending(record.promise)) return;
                if (!info.context.cancellation) info.context.cancellation = {reason: args[0] === undefined
                    ? new handlerErrorTypes.Error('Stream was cancelled.') : captureNativeException(args[0])};
                record.cancelled = true; record.reason = info.context.cancellation.reason;
            });
        }
        return intrinsicApply(nativeCancel, this, args);
    }});
    Object.defineProperty(readerPrototype, 'releaseLock', {...Object.getOwnPropertyDescriptor(readerPrototype, 'releaseLock'), value: function releaseLock(...args) {
        const info = fetchReaderContext(this, false);
        if (info && !info.context.branch) {try {info.context.checkContext();} catch (error) {throw readerScopeError(error, info);}}
        checkRelease(info);
        const result = intrinsicApply(nativeReleaseLock, this, args);intrinsicWeakDelete(fetchReaderContexts, this);return result;
    }});
    const closedDescriptor = {...closed, get() {
        const info = fetchReaderContext(this, false);
        const original = observeStreamPromise(intrinsicApply(closed.get, this, []));
        if (info) return original;
        const cached = intrinsicWeakGet(readerClosedPromises, this);
        if (cached?.original === original) return observeStreamPromise(cached.value);
        // The reference's standard-stream controller completes queued reads
        // before the public closed reaction. Already settled getters retain
        // their native promise and releaseLock still creates a new identity.
        const value = core.capsidFetchReadPending(original)
            ? intrinsicApply(intrinsicThen, original, [result => result]) : original;
        if (value !== original) intrinsicApply(intrinsicThen, value, [undefined, () => {}]);
        intrinsicWeakSet(readerClosedPromises, this, {original, value});return observeStreamPromise(value);
    }};
    Object.defineProperty(closedDescriptor.get, 'name', {value: closed.get.name, configurable: true});
    Object.defineProperty(readerPrototype, 'closed', closedDescriptor);

    const byobConstructor = new Proxy(nativeBYOBReader, {construct(target, args, newTarget) {
        return associateReader(args[0], intrinsicConstruct(target, args, newTarget), true);
    }});
    Object.defineProperty(globalThis, 'ReadableStreamBYOBReader', {
        ...Object.getOwnPropertyDescriptor(globalThis, 'ReadableStreamBYOBReader'), value: byobConstructor
    });
    Object.defineProperty(byobPrototype, 'constructor', {
        ...Object.getOwnPropertyDescriptor(byobPrototype, 'constructor'), value: byobConstructor
    });
    const readBYOB = (reader, args, least = false) => {
        const info = fetchReaderContext(reader, true);
        if (intrinsicWeakGet(fetchReaderKinds, reader) !== true) throw new handlerErrorTypes.TypeError(
            'Illegal invocation: function called with incorrect `this` reference. '
            + 'See https://developers.cloudflare.com/workers/observability/errors/#illegal-invocation-errors for details.');
        let view, minimum, elements;
        try {
            const input = args[0];
            // readAtLeast converts its first argument before the view;
            // read converts the view before its optional dictionary.
            if (least) elements = byobMinimum(args[1].min);
            if (!intrinsicIsView(input)) throw new handlerErrorTypes.TypeError(
                `Failed to execute '${least ? 'readAtLeast' : 'read'}' on 'ReadableStreamBYOBReader': parameter ${least ? 2 : 1} is not of type 'ArrayBufferView'.`);
            if (!least) {
                const options = args[1];
                if (options !== undefined && options !== null && !byobObject(options)) throw new handlerErrorTypes.TypeError(
                    "Failed to execute 'read' on 'ReadableStreamBYOBReader': parameter 2 is not of type 'ReadableStreamBYOBReaderReadOptions'.");
                const option = options?.min;
                elements = option === undefined ? 1 : byobMinimum(option);
            }
            if (intrinsicWeakGet(fetchReleasedReaders, reader)) throw new handlerErrorTypes.TypeError(
                'This ReadableStream reader has been released.');
            const tag = intrinsicApply(intrinsicTypedTag, input, []), typed = tag !== undefined;
            const buffer = intrinsicApply(typed ? intrinsicTypedBuffer : intrinsicViewBuffer, input, []);
            if (intrinsicApply(intrinsicBufferLength, buffer, []) === 0) throw new handlerErrorTypes.TypeError(
                'You must call read() on a "byob" reader with a positive-sized TypedArray object.');
            const offset = intrinsicApply(typed ? intrinsicTypedOffset : intrinsicViewOffset, input, []);
            const length = intrinsicApply(typed ? intrinsicTypedLength : intrinsicViewLength, input, []);
            if (length === 0) throw new handlerErrorTypes.TypeError(
                'You must call read() on a "byob" reader with a positive-sized TypedArray object.');
            if (elements === 0) throw new handlerErrorTypes.TypeError('Requested invalid minimum number of bytes to read (0).');
            // The pinned interpreter does not wrap negative values through
            // BigInt.asUintN(64, ...). Form this unsigned count arithmetically.
            const count = elements < 0 ? (1n << 64n) + intrinsicBigInt(elements) : elements;
            if (count > length) throw new handlerErrorTypes.TypeError(
                `Minimum bytes to read (${count}) exceeds size of buffer (${length}).`);
            minimum = elements * (typed ? length / intrinsicApply(intrinsicTypedElements, input, []) : 1);
            if (minimum > length) throw new handlerErrorTypes.TypeError(
                `Minimum bytes to read (${minimum}) exceeds size of buffer (${length}).`);
            // The native controller consumes bytes and always returns a byte
            // view. Giving the polyfill a multi-byte view would round away a
            // partial element before the native result could be exposed.
            view = info ? new handlerErrorBytes(buffer, offset, length) : input;

        } catch (error) {return intrinsicPromiseReject(error);}
        if (!info) return observeStreamPromise(intrinsicApply(nativeBYOBRead, reader, [view, {min: elements}]));
        let pending = false;
        intrinsicSetForEach(info.pending, record => {if (core.capsidFetchReadPending(record.promise)) pending = true;});
        if (pending) {
            intrinsicApply(intrinsicBufferTransfer, intrinsicApply(intrinsicTypedBuffer, view, []), []);
            return intrinsicPromiseReject(new handlerErrorTypes.TypeError(
                'This ReadableStream only supports a single pending read request at a time.'));
        }
        try {info.context.checkContext();} catch (error) {return foreignRead(info, error, view);}
        const original = observeStreamPromise(intrinsicApply(nativeBYOBRead, reader, [view, {min: minimum}]));
        // Ordinary native I/O consumes a valid caller backing even when the byte
        // controller is already errored. The generic polyfill returns its
        // rejection before transfer in that state; preserve native ownership.
        const inputBuffer = intrinsicApply(intrinsicTypedBuffer, view, []);
        if (info.context.ordinary === ordinaryFetchBodyTag && intrinsicApply(intrinsicBufferLength, inputBuffer, []) !== 0)
            intrinsicApply(intrinsicBufferTransfer, inputBuffer, []);
        const record = {promise: original, cancelled: false, reason: undefined};
        if (core.capsidFetchReadPending(original)) intrinsicSetAdd(info.pending, record);
        return adoptNativeRead(intrinsicApply(intrinsicThen, original, [value => {
            intrinsicSetDelete(info.pending, record);
            if (record.cancelled) return intrinsicPromiseReject(record.reason);
            if (value.done && value.value?.byteLength) return {value: value.value, done: false};
            if (value.done) {
                info.closeWanted = true; if (info.byteClosed) info.resolveClosed();
                // Native EOF exposes an empty byte view at the start of the
                // transferred backing store, including offset input views.
                if (value.value && intrinsicApply(intrinsicTypedOffset, value.value, []) !== 0)
                    return {value: new handlerErrorBytes(intrinsicApply(intrinsicTypedBuffer, value.value, []), 0, 0), done: true};
            }
            return value;
        }, error => {intrinsicSetDelete(info.pending, record); throw error;}]));
    };
    Object.defineProperty(byobPrototype, 'read', {...Object.getOwnPropertyDescriptor(byobPrototype, 'read'), value: function read(view, ...args) {
        return readBYOB(this, [view, ...args]);
    }});
    Object.defineProperty(byobPrototype, 'readAtLeast', {...Object.getOwnPropertyDescriptor(byobPrototype, 'read'), value: function readAtLeast(minElements, view) {
        return readBYOB(this, [view, {min: minElements}], true);
    }});
    Object.defineProperty(byobPrototype, 'cancel', {...Object.getOwnPropertyDescriptor(byobPrototype, 'cancel'), value: function cancel(...args) {
        const info = fetchReaderContext(this, true);
        if (info) {
            info.stream._disturbed = true;
            try {info.context.checkContext();} catch (error) {return intrinsicPromiseReject(readerScopeError(error, info));}
            intrinsicSetForEach(info.pending, record => {
                if (record.foreign) {intrinsicSetDelete(info.pending, record); return;}
                if (!core.capsidFetchReadPending(record.promise)) return;
                if (!info.context.cancellation) info.context.cancellation = {reason: args[0] === undefined
                    ? new handlerErrorTypes.Error('Stream was cancelled.') : captureNativeException(args[0])};
                record.cancelled = true; record.reason = info.context.cancellation.reason;
            });
            info.closeWanted = true; if (info.byteClosed) info.resolveClosed();
        }
        return intrinsicApply(nativeBYOBCancel, this, args);
    }});
    Object.defineProperty(byobPrototype, 'releaseLock', {...Object.getOwnPropertyDescriptor(byobPrototype, 'releaseLock'), value: function releaseLock(...args) {
        const info = fetchReaderContext(this, true);
        if (info && !info.context.branch) {try {info.context.checkContext();} catch (error) {throw readerScopeError(error, info);}}
        checkRelease(info);
        const result = intrinsicApply(nativeBYOBRelease, this, args);
        if (info?.byteClosed && !info.closeWanted) info.rejectClosed(new handlerErrorTypes.TypeError('This ReadableStream reader has been released.'));
        intrinsicWeakDelete(fetchReaderContexts, this); intrinsicWeakSet(fetchReleasedReaders, this, {closed: info?.closed}); return result;
    }});
    Object.defineProperty(byobPrototype, 'closed', {...byobClosed, get() {
        return observeStreamPromise(fetchReaderContext(this, true)?.closed ?? intrinsicWeakGet(fetchReleasedReaders, this)?.closed ?? intrinsicApply(byobClosed.get, this, []));
    }});
}
// Ordinary HttpClient data is delivered into caller-provided byte buffers.
// Registration reuses the same reader ownership and five constructor paths as
// upgrade fallback bodies. The Binding VM keeps its own native resource owner.
export function createOrdinaryFetchBody(client, maxBodyBytes = 0) {
    const user = core.capsidUserRuntime;
    const origin = user ? core.capsidChannelContext() : undefined;
    const checkContext = () => {if (user) core.capsidChannelCheckContext(origin);};
    const branches = new intrinsicSet();
    // A pending native read owns its destination until the producer settles.
    // Promise dependencies alone do not keep the upstream JS controller alive.
    const pendingBranches = new intrinsicSet();
    // The native source keeps each derived registry alive until its branches
    // retire. The anchor retains only the retired root, not the weak children.
    const identityBridges = new intrinsicSet();
    const forEachBranch = callback => intrinsicSetForEach(branches, reference => {
        const branch = intrinsicDeref(reference);
        if (branch) callback(branch);
        else intrinsicSetDelete(branches, reference);
    });
    const branchFinalizer = new intrinsicFinalizationRegistry(reference => {
        intrinsicSetDelete(branches, reference);
        if (intrinsicSetSize(branches) === 0 && !ended && !cancelled) {cancelled = true; client.abort();}
    });
    const retireBranch = branch => {
        intrinsicSetDelete(pendingBranches, branch);
        intrinsicUnregister(branchFinalizer, branch.reference);
        intrinsicSetDelete(branches, branch.reference);
    };
    let ended = false, cancelled = false, decodeFailure, quotaFailure, compressed = false;
    let receivedBytes = 0, split = false;
    let encodedMode = false;
    let identityMode = false, identityBytes = 0;
    let identityChunks = [];
    let sourceMinimum = 1, sourceCapacity = 4096, sourceBytes = 0;
    let sourceChunks = [];
    let demandLocked = false, demandScheduled = false, demandEpoch = 0, delivering = false;
    const discardChunks = branch => {
        branch.chunks.length = 0; branch.queuedBytes = 0; branch.opportunityBytes = null;
        branch.encodedChunks.length = 0; branch.encodedBytes = 0; branch.decodeInput = null;
        if (branch.decoder) {branch.decoder.dispose(); branch.decoder = null;}
    };
    // Each unread gzip branch retains compressed
    // input and creates its own decoder only when it first receives a read.
    const flushEncoded = branch => {
        const controller = branch.controller;
        if (!controller || branch.cancelled || branch.closed || branch.failed) return;
        let request = controller.byobRequest;
        if (!request) {intrinsicSetDelete(pendingBranches, branch); return;}
        intrinsicSetAdd(pendingBranches, branch);
        if (!branch.decoder) branch.decoder = client.createGzipDecoder();
        try {
            while ((request = controller.byobRequest)) {
                if (!branch.decodeInput) {
                    if (branch.encodedBytes) {
                        const size = Math.min(16384, branch.encodedBytes), input = new handlerErrorBytes(size);
                        let written = 0;
                        while (written < size) {
                            const chunk = branch.encodedChunks[0], length = intrinsicApply(intrinsicTypedLength, chunk, []);
                            const count = Math.min(length, size - written), buffer = intrinsicApply(intrinsicTypedBuffer, chunk, []);
                            const offset = intrinsicApply(intrinsicTypedOffset, chunk, []);
                            intrinsicByteSet(input, new handlerErrorBytes(buffer, offset, count), written);
                            if (count === length) branch.encodedChunks.shift();
                            else branch.encodedChunks[0] = new handlerErrorBytes(buffer, offset + count, length - count);
                            written += count;
                        }
                        branch.encodedBytes -= size; branch.decodeInput = input; branch.decodeOffset = 0;
                    } else {
                        if (decodeFailure) throw decodeFailure;
                        if (!ended) return;
                        branch.decoder.finish(); branch.decoder.dispose(); branch.decoder = null;
                        branch.closed = true; controller.close(); controller.byobRequest?.respond(0);
                        intrinsicSetDelete(pendingBranches, branch); return;
                    }
                }
                const view = request.view, capacity = intrinsicApply(intrinsicTypedLength, view, []);
                let written = 0, boundary = false;
                while (written < capacity) {
                    const input = branch.decodeInput, length = intrinsicApply(intrinsicTypedLength, input, []);
                    const remaining = length - branch.decodeOffset;
                    const step = branch.decoder.step(remaining ? intrinsicApply(intrinsicTypedBuffer, input, []) : null,
                        remaining ? branch.decodeOffset : 0, remaining, Math.min(4096, capacity - written));
                    branch.decodeOffset += step.consumed;
                    const bytes = new handlerErrorBytes(step.data), count = intrinsicApply(intrinsicTypedLength, bytes, []);
                    if (count) {intrinsicByteSet(view, bytes, written); written += count;}
                    // Each completed gzip member is a read opportunity.
                    // A larger minimum continues into the next member.
                    if (step.atMemberEnd && count) {boundary = true; break;}
                    if (!step.consumed && !count) {
                        if (remaining) throw new handlerErrorTypes.Error('Private decoder made no progress');
                        boundary = true; branch.decodeInput = null; branch.decodeOffset = 0; break;
                    }
                }
                if (written) request.respond(written);
                if (!controller.byobRequest) {intrinsicSetDelete(pendingBranches, branch); return;}
                if (!written && !boundary) throw new handlerErrorTypes.Error('Private decoder stalled');
            }
        } catch (error) {fail(branch, error);}
    };
    const fail = (branch, error) => {
        branch.failed = error; discardChunks(branch); branch.controller.error(error);
        // Failed consumers no longer own the upstream connection. Retire them
        // even while their errored response objects remain strongly reachable.
        retireBranch(branch);
        if (intrinsicSetSize(branches) === 0 && !ended && !cancelled) {
            cancelled = true; client.abort();
        }
    };
    const flush = branch => {
        if (encodedMode) {flushEncoded(branch); return;}
        const controller = branch.controller;
        if (!controller || branch.cancelled || branch.closed || branch.failed) return;
        const request = controller.byobRequest;
        if (!request) {intrinsicSetDelete(pendingBranches, branch); return;}
        intrinsicSetAdd(pendingBranches, branch);
        const chunks = branch.chunks;
        if (chunks.length) {
            const view = request.view, available = intrinsicApply(intrinsicTypedLength, view, []);
            // Native branches consume their own already-buffered bytes across
            // producer opportunities. An untouched source retains its original
            // opportunity boundary and read behavior.
            const ready = split ? branch.queuedBytes : branch.opportunityBytes ?? branch.queuedBytes;
            const opportunity = branch.opportunityBytes !== null;
            if (ready < available && !opportunity) {
                if (decodeFailure) {fail(branch, decodeFailure); return;}
                if (!ended) return;
            }
            const wanted = Math.min(ready, available);
            let written = 0;
            while (chunks.length && written < wanted) {
                const item = chunks[0], chunk = item.bytes;
                const length = intrinsicApply(intrinsicTypedLength, chunk, []);
                const consumed = Math.min(length, wanted - written);
                const buffer = intrinsicApply(intrinsicTypedBuffer, chunk, []);
                const offset = intrinsicApply(intrinsicTypedOffset, chunk, []);
                intrinsicByteSet(view, new handlerErrorBytes(buffer, offset, consumed), written);
                if (consumed === length) chunks.shift();
                else item.bytes = new handlerErrorBytes(buffer, offset + consumed, length - consumed);
                written += consumed;
            }
            branch.queuedBytes -= written;
            if (branch.opportunityBytes !== null) {
                branch.opportunityBytes -= written;
                if (branch.opportunityBytes <= 0) {
                    branch.opportunityBytes = null;
                    let prefix = 0;
                    for (let index = 0; index < chunks.length; ++index) {
                        const item = chunks[index];
                        prefix += intrinsicApply(intrinsicTypedLength, item.bytes, []);
                        if (item.opportunity) {branch.opportunityBytes = prefix; break;}
                    }
                }
            }
            request.respond(written);
            if (!controller.byobRequest) intrinsicSetDelete(pendingBranches, branch);
        } else if (decodeFailure) {
            fail(branch, decodeFailure);
        } else if (ended && !identityBytes) {
            branch.closed = true; controller.close(); controller.byobRequest?.respond(0);
            intrinsicSetDelete(pendingBranches, branch);
        }
    };
    const updateDemand = () => {
        if (encodedMode) {
            if (cancelled || ended || delivering) return;
            let waiting = false;
            forEachBranch(branch => {
                if (!branch.cancelled && !branch.closed && !branch.failed && branch.controller.byobRequest) waiting = true;
            });
            client.setReadState(16384, !waiting); return;
        }
        if (cancelled || (ended && !identityBytes) || decodeFailure || delivering || (split && demandLocked)) return;
        let count = 0, minimum = 1, maximum = Infinity, largest = 0;
        forEachBranch(branch => {
            if (branch.cancelled || branch.closed || branch.failed) return;
            const request = branch.controller.byobRequest;
            if (!request) return;
            const size = intrinsicApply(intrinsicTypedLength, request.view, []);
            const pending = branch.controller._pendingPullIntos.peek();
            minimum = Math.max(minimum, pending.minimumFill - pending.bytesFilled);
            maximum = Math.min(maximum, size); largest = Math.max(largest, size); count++;
        });
        // Encoded input is decoded once by this adapter. Keep its decoded
        // window large enough for each independent branch; raw native reads
        // share the smallest window that satisfies the largest minimum.
        const capacity = !split ? largest : compressed && !identityMode ? largest : Math.max(minimum, Math.min(maximum, 16384));
        sourceMinimum = count ? minimum : 1;
        sourceCapacity = count ? capacity : 4096;
        client.setReadState(sourceCapacity, identityMode || count === 0, sourceMinimum);
        if (split && count && !demandScheduled) {
            demandScheduled = true; const epoch = demandEpoch;
            intrinsicApply(intrinsicThen, intrinsicPromiseResolve(), [() => {
                if (epoch !== demandEpoch) return;
                demandScheduled = false; demandLocked = true;
                if (identityMode) drainIdentity();
            }]);
        } else if (split && !count && demandScheduled) {
            demandEpoch++; demandScheduled = false;
        }
    };
    // Private identity-layer bridge: a started decoder remains the source of
    // the derived raw tee. Its unread compressed input is never copied into
    // fresh decoders after decoding has begun.
    const bridgeIdentity = (branch, body) => {
        const reader = intrinsicApply(fetchNativeGetReader, body, [{mode: 'byob'}]);
        let adapter, disposed = false, finished = false, scheduled = false, reading = false;
        let capacity = 4096, minimum = 1, paused = true;
        // A native identity tee retains accepted bytes before an upstream
        // error, including bytes queued for a sibling read after the error.
        const schedule = () => {
            if (disposed || finished || paused || scheduled || reading) return;
            scheduled = true;
            intrinsicApply(intrinsicThen, intrinsicPromiseResolve(), [() => {
                scheduled = false;
                if (disposed || finished || paused || reading) return;
                reading = true;
                let promise;
                try {
                    promise = intrinsicApply(fetchNativeBYOBRead, reader, [new handlerErrorBytes(capacity), {min: minimum}]);
                } catch (error) {
                    reading = false; finished = true; adapter.controller.error(error, true);
                    intrinsicSetDelete(identityBridges, adapter); return;
                }
                intrinsicApply(intrinsicThen, promise, [result => {
                    reading = false;
                    if (disposed) return;
                    // EOF may carry fewer bytes than the requested minimum.
                    // Retain those bytes before closing the identity source.
                    if (result.value && intrinsicApply(intrinsicTypedLength, result.value, []))
                        adapter.controller.enqueue(result.value);
                    if (result.done) {
                        finished = true; adapter.controller.close();
                        intrinsicSetDelete(identityBridges, adapter);
                    }
                    schedule();
                }, error => {
                    reading = false;
                    if (!disposed) {
                        finished = true; adapter.controller.error(error, true);
                        intrinsicSetDelete(identityBridges, adapter);
                    }
                }]);
            }]);
        };
        const proxy = {
            enableEncodedRead() {return false;},
            setReadState(nextCapacity, nextPaused, nextMinimum = 1) {
                capacity = nextCapacity; minimum = nextMinimum; paused = nextPaused; schedule();
            },
            abort() {
                if (disposed) return;
                disposed = true; intrinsicSetDelete(identityBridges, adapter);
                branch.cancelled = true; discardChunks(branch); retireBranch(branch);
                if (intrinsicSetSize(branches) === 0 && !ended && !cancelled) {cancelled = true; client.abort();}
                else updateDemand();
            },
        };
        adapter = createOrdinaryFetchBody(proxy, 0);
        intrinsicSetAdd(identityBridges, adapter);
        return adapter.body;
    };
    const makeBranch = previous => {
        const branch = {chunks: [], queuedBytes: 0, opportunityBytes: null,
            controller: null, cancelled: false, closed: false, failed: undefined, readStarted: false,
            encodedChunks: [], encodedBytes: 0, decoder: null, decodeInput: null, decodeOffset: 0};
        let sourceScopeFailure;
        const rejectForeignRead = error => {
            if (error?.message !== crossRequestMessage) return error;
            if (!sourceScopeFailure) {
                sourceScopeFailure = new handlerErrorTypes.Error(crossRequestBranchSourceMessage);
                fail(branch, sourceScopeFailure);
            }
            return sourceScopeFailure;
        };
        if (previous) {
            for (let index = 0; index < previous.encodedChunks.length; ++index) branch.encodedChunks.push(previous.encodedChunks[index]);
            branch.encodedBytes = previous.encodedBytes;
            for (let index = 0; index < previous.chunks.length; ++index) {
                const item = previous.chunks[index];
                branch.chunks.push({bytes: item.bytes, opportunity: item.opportunity});
            }
            branch.queuedBytes = previous.queuedBytes; branch.opportunityBytes = previous.opportunityBytes;
        }
        const body = new fetchReadableStream({
            type: 'bytes', autoAllocateChunkSize: 4096,
            start(value) {branch.controller = value;},
            pull() {
                try {checkContext();}
                catch (error) {
                    if (previous && error?.message === crossRequestMessage) {rejectForeignRead(error); return;}
                    throw error;
                }
                branch.readStarted = true; flush(branch); updateDemand();
            },
            cancel() {
                checkContext(); branch.cancelled = true; discardChunks(branch); retireBranch(branch);
                if (intrinsicSetSize(branches) === 0) {cancelled = true; client.abort();} else updateDemand();
            },
        });
        branch.reference = new intrinsicWeakRef(branch);
        intrinsicSetAdd(branches, branch.reference);
        intrinsicRegister(branchFinalizer, branch, branch.reference, branch.reference);
        markNativeByteStream(body, !!previous);
        const context = {branch: !!previous, checkContext: previous ? () => {if (!sourceScopeFailure) checkContext();} : checkContext,
            cancellation: null, ordinary: ordinaryFetchBodyTag,
            rejectForeignRead: previous ? rejectForeignRead : undefined,
            tee() {
                if (encodedMode && branch.readStarted) return teeOrdinaryFetchBody(bridgeIdentity(branch, body));
                if (!split && !branch.readStarted && maxBodyBytes === 0 && !ended && client.enableEncodedRead()) {
                    encodedMode = true; compressed = true;
                }
                intrinsicApply(fetchNativeGetReader, body, []); body._disturbed = true;
                if (!split && compressed && branch.readStarted && !ended) {
                    // A started gzip body now tees its existing identity layer.
                    // Its remaining decoded prefix is shared source data, not
                    // already-delivered data belonging to both new branches.
                    identityMode = true; identityChunks = branch.chunks;
                    identityBytes = branch.queuedBytes;
                    branch.chunks = []; branch.queuedBytes = 0; branch.opportunityBytes = null;
                }
                split = true;
                const first = makeBranch(branch), second = makeBranch(branch);
                retireBranch(branch); discardChunks(branch);
                if (!branch.failed && !branch.closed) {branch.closed = true; branch.controller.close();}
                updateDemand();
                return [first, second];
            }};
        intrinsicWeakSet(fetchStreamContexts, body, context);
        if (previous?.closed) {branch.closed = true; branch.controller.close();}
        else if (previous?.failed) fail(branch, previous.failed);
        return body;
    };
    const body = makeBranch();
    client.setReadState(4096, true);
    const queueChunk = (branch, chunk, opportunity) => {
        const size = intrinsicApply(intrinsicTypedLength, chunk, []);
        if (size) {
            branch.chunks.push({bytes: chunk, opportunity}); branch.queuedBytes += size;
            if (opportunity && branch.opportunityBytes === null) branch.opportunityBytes = branch.queuedBytes;
        } else if (branch.chunks.length && opportunity) {
            branch.chunks[branch.chunks.length - 1].opportunity = true;
            if (branch.opportunityBytes === null) branch.opportunityBytes = branch.queuedBytes;
        }
    };
    const publish = (chunks, opportunity) => {
        demandEpoch++; demandLocked = false; demandScheduled = false; delivering = true;
        forEachBranch(branch => {
            if (branch.cancelled || branch.closed || branch.failed) return;
            for (let index = 0; index < chunks.length; ++index)
                queueChunk(branch, chunks[index], opportunity && index === chunks.length - 1);
            flush(branch);
        });
        delivering = false; updateDemand();
    };
    const drainIdentity = () => {
        if (!identityMode || cancelled || !demandLocked) return;
        let opportunity = 0;
        for (let index = 0; index < identityChunks.length; ++index) {
            const item = identityChunks[index];
            opportunity += intrinsicApply(intrinsicTypedLength, item.bytes, []);
            if (item.opportunity) break;
        }
        const available = opportunity >= sourceMinimum ? opportunity : identityBytes;
        if (available < sourceMinimum && !ended && !decodeFailure) {
            client.setReadState(Math.max(1, sourceCapacity - identityBytes), false); return;
        }
        if (!available) {
            demandLocked = false;
            forEachBranch(flush); return;
        }
        const wanted = Math.min(sourceCapacity, available), chunks = [];
        let written = 0;
        while (identityChunks.length && written < wanted) {
            const item = identityChunks[0], bytes = item.bytes;
            const length = intrinsicApply(intrinsicTypedLength, bytes, []);
            const count = Math.min(length, wanted - written);
            const buffer = intrinsicApply(intrinsicTypedBuffer, bytes, []);
            const offset = intrinsicApply(intrinsicTypedOffset, bytes, []);
            chunks.push(new handlerErrorBytes(buffer, offset, count));
            if (count === length) identityChunks.shift();
            else item.bytes = new handlerErrorBytes(buffer, offset + count, length - count);
            written += count;
        }
        identityBytes -= written;
        publish(chunks, true);
    };
    const publishRaw = () => {
        if (!sourceChunks.length) return;
        const chunks = sourceChunks; sourceChunks = []; sourceBytes = 0;
        publish(chunks, true);
    };
    const enqueue = (chunk, flags) => {if (!cancelled && !quotaFailure) {
        if (encodedMode) {
            delivering = true;
            const size = intrinsicApply(intrinsicTypedLength, chunk, []);
            forEachBranch(branch => {
                if (branch.cancelled || branch.closed || branch.failed) return;
                if (size) {branch.encodedChunks.push(chunk); branch.encodedBytes += size;}
                flushEncoded(branch);
            });
            delivering = false; updateDemand(); return;
        }
        if (flags !== undefined) compressed = true;
        const size = intrinsicApply(intrinsicTypedLength, chunk, []);
        if (maxBodyBytes !== 0) {
            receivedBytes += size;
            if (receivedBytes > maxBodyBytes) {
                quotaFailure = new handlerErrorTypes.TypeError(
                    `fetch response body exceeded configured limit of ${maxBodyBytes} bytes`);
                decodeFailure = quotaFailure;
                // Bytes from a preceding incomplete native read are already
                // accepted source data. Expose them before reporting failure.
                // Started decoded tees keep that data in the identity queue.
                if (identityChunks.length) {
                    const chunks = [];
                    for (let index = 0; index < identityChunks.length; ++index)
                        chunks[index] = identityChunks[index].bytes;
                    identityChunks = []; identityBytes = 0;
                    publish(chunks, true);
                }
                publishRaw();
                forEachBranch(branch => {
                    if (branch.chunks.length) {
                        branch.chunks[branch.chunks.length - 1].opportunity = true;
                        if (branch.opportunityBytes === null) branch.opportunityBytes = branch.queuedBytes;
                    }
                    flush(branch);
                });
                client.abort(); return;
            }
        }
        if (identityMode) {
            client.setReadState(sourceCapacity, true);
            if (size) {identityChunks.push({bytes: chunk, opportunity: ((flags ?? 1) & 2) === 0}); identityBytes += size;}
            else if (identityChunks.length && ((flags ?? 1) & 2) === 0) identityChunks[identityChunks.length - 1].opportunity = true;
            drainIdentity();
        } else if (split && flags === undefined) {
            // A native tryRead(min,max) may need several transport callbacks.
            // Keep all branches behind that issued read until min is met;
            // only its remaining capacity is offered to the next callback.
            if (size) {sourceChunks.push(chunk); sourceBytes += size;}
            if (sourceBytes < sourceMinimum) {
                client.setReadState(Math.max(1, sourceCapacity - sourceBytes), false); return;
            }
            publishRaw();
        } else publish([chunk], ((flags ?? 1) & 2) === 0);
    }};
    return {body, controller: {
        enqueue,
        enqueueBatch(chunk, flags, blockSize) {
            if (maxBodyBytes === 0) {enqueue(chunk, flags); return;}
            const buffer = intrinsicApply(intrinsicTypedBuffer, chunk, []);
            const start = intrinsicApply(intrinsicTypedOffset, chunk, []);
            const length = intrinsicApply(intrinsicTypedLength, chunk, []);
            for (let offset = 0; offset < length; offset += blockSize) {
                const size = Math.min(blockSize, length-offset);
                enqueue(new handlerErrorBytes(buffer, start+offset, size), offset+size === length ? flags : 2);
                if (quotaFailure || cancelled) break;
            }
        },
        close() {ended = true; if (identityMode) drainIdentity(); publishRaw(); forEachBranch(branch => {
            intrinsicUnregister(branchFinalizer, branch.reference); flush(branch);
        });},
        error(error, defer = false) {if (!cancelled && !quotaFailure) {
            if (defer) {decodeFailure = error; publishRaw(); forEachBranch(flush);}
            else forEachBranch(branch => fail(branch, error));
        }},
    }};
}

export function teeOrdinaryFetchBody(stream) {
    return intrinsicWeakGet(fetchStreamContexts, stream)?.tee?.();
}

export function isOrdinaryFetchBody(stream) {return intrinsicWeakGet(fetchStreamContexts, stream)?.ordinary === ordinaryFetchBodyTag;}
export function isFetchChannelStream(stream) {return intrinsicWeakGet(fetchStreamContexts, stream) !== undefined;}
export function inheritFetchChannelStream(source, destination) {
    const context = intrinsicWeakGet(fetchStreamContexts, source);
    if (context) {
        markNativeByteStream(destination);
        intrinsicWeakSet(fetchStreamContexts, destination, context);
    }
    return destination;
}
const handlerErrorTimer = core.capsidChannelQueueTask;
const handlerErrorObjectKeys = Object.keys;
const handlerErrorOwnDescriptor = Object.getOwnPropertyDescriptor;
const handlerErrorArray = Array;
const handlerErrorSeen = WeakMap;
const handlerErrorSeenHas = Function.prototype.call.bind(WeakMap.prototype.has);
const handlerErrorSeenGet = Function.prototype.call.bind(WeakMap.prototype.get);
const handlerErrorSeenSet = Function.prototype.call.bind(WeakMap.prototype.set);
const handlerErrorOwnKeys = Reflect.ownKeys;
const handlerErrorDOMException = DOMException;
const handlerErrorDOMName = Function.prototype.call.bind(Object.getOwnPropertyDescriptor(DOMException.prototype, 'name').get);
const handlerErrorDOMMessage = Function.prototype.call.bind(Object.getOwnPropertyDescriptor(DOMException.prototype, 'message').get);
const handlerErrorDOMCode = Function.prototype.call.bind(Object.getOwnPropertyDescriptor(DOMException.prototype, 'code').get);
const handlerErrorBytes = Uint8Array;
const handlerErrorRandom = core.random;
const handlerErrorLog = core.capsidLog;
// Pinned workerd's Native I/O ownership diagnostic (same-request fanout probe).
const crossRequestMessage = 'Cannot perform I/O on behalf of a different request. '
    + 'I/O objects (such as streams, request/response bodies, and others) created in the context '
    + "of one request handler cannot be accessed from a different request's handler. "
    + 'This is a limitation of Cloudflare Workers which allows us to improve overall performance. '
    + '(I/O type: Native)';
const crossRequestCancelerMessage = crossRequestMessage.replace('(I/O type: Native)', '(I/O type: Canceler)');
const crossRequestBranchSourceMessage = crossRequestMessage.replace('(I/O type: Native)', '(I/O type: ReadableStreamSource)');
// Before transfer these queues live in the JS heap, under the same
// worker/per-endpoint ceiling as native sends (including empty messages).
let pendingBytes = 0;
const queueFinalizer = new FinalizationRegistry(reservation => { pendingBytes -= reservation.bytes; });
const limits = core.capsidChannelLimits();

function state(socket) {
    const value = intrinsicWeakGet(endpoints, socket);
    if (!value) throw new TypeError('Illegal invocation');
    return value;
}

// WebIDL USVString: symbols are rejected; this pinned engine's
// String(symbol) does not throw on its own (C02/C04/C11).
function toUSVString(value) {
    if (typeof value === 'symbol') {
        throw new TypeError('Cannot convert a Symbol to a string');
    }
    // The pinned engine replaces lone surrogates in one native pass. Building
    // the result one code unit at a time repeatedly copies large messages.
    // Capture the intrinsic before application code can replace it, and keep
    // the observable value-to-string conversion to exactly one call.
    return stringToWellFormed(handlerErrorString(value));
}

class WebSocket extends EventTarget {
    constructor(url, protocols) {
        super();
        const outbound = url !== internalEndpoint;
        if (outbound && arguments.length === 0) {
            throw new TypeError("Failed to construct 'WebSocket': 1 argument required, but only 0 present");
        }
        let parsed, requested = [];
        if (outbound) {
            url = toUSVString(url);
            try { parsed = new URL(url); }
            catch { throw new DOMException('The WebSocket URL is invalid', 'SyntaxError'); }
            if (!['ws:', 'wss:'].includes(parsed.protocol) || !parsed.hostname || url.includes('#'))
                throw new DOMException('WebSocket requires ws/wss and no fragment', 'SyntaxError');
            // Reference conversion (fix-review §10): undefined -> none;
            // string -> [it]; Array -> mapped; ANY other value (null,
            // numbers, booleans, plain objects) becomes ONE scalar via
            // USVString — so a non-array iterable stringifies to
            // "[object ...]" and fails token validation (SyntaxError).
            if (protocols !== undefined)
                requested = typeof protocols === 'string' ? [protocols]
                    : Array.isArray(protocols)
                        ? protocols.map(toUSVString)
                        : [toUSVString(protocols)];
            if (requested.some(p => !/^[!#$%&'*+\-.^_`|~0-9A-Za-z]+$/.test(p)) || new Set(requested).size !== requested.length)
                throw new DOMException('Invalid or duplicate WebSocket protocols', 'SyntaxError');
        }
        const reservation = { bytes: 0 };
        intrinsicWeakSet(endpoints, this, {
            socket: this, peer: null, accepted: false, transferred: false,
            origin: core.capsidChannelContext(),
            channel: 0, outgoingClosed: false, incomingClosed: false,
            failed: false, disconnected: false, binaryType: 'blob', allowHalfOpen: false,
            queue: [], queuedBytes: 0, reservation, stage: null,
            nextDispatchSeq: 0, nextCommitSeq: 0, pendingCommits: new Map(), nativeClosed: false,
            handlerFailurePending: false,
            handlerError: null, handlerOutgoingAborted: false,
            outbound, connecting: outbound, dial: 0n, url: outbound ? url : '', protocol: '', extensions: '',
        });
        queueFinalizer.register(this, reservation);
        if (outbound) {
            const s = state(this);
            // Userinfo is never copied into transport credentials. Keep the
            // original URL visible to JS, as the pinned reference does.
            parsed.username = ''; parsed.password = '';
            const callback = (generation, type, bytes, text, code, reason, clean, channel) => {
                runChannelEvent(generation, async () => {
                    if (!core.capsidChannelEventStarted(generation)) return;
                    if (s.connecting && s.outgoingClosed && type !== 'failure') return;
                    if (type === 'open') {
                        s.channel = channel; s.connecting = false; s.accepted = true; s.protocol = reason || null; s.extensions = null;
                        await dispatch(s, 'open');
                    } else if (type === 'failure') {
                        s.connecting = false; s.failed = true;
                        try { await dispatch(s, 'error', null, false, 0, reason || 'Failed to establish websocket connection'); }
                        finally { await dispatch(s, 'close', null, false, 1006, 'Failed to establish websocket connection', false); }
                    } else await dispatch(s, type, bytes, text, code, reason, clean);
                }, () => {
                    abortFromHandler(s);
                });
            };
            s.dial = core.capsidChannelOpen(parsed.href, requested, callback);
        }
    }

    accept(options = {}) {
        const s = state(this);
        if (s.fetched && s.origin !== core.capsidChannelContext())
            throw new handlerErrorTypes.Error(crossRequestMessage);
        if (s.outbound && s.connecting) throw new TypeError('WebSockets obtained from the constructor cannot call accept');
        if (s.transferred) throw new TypeError(s.fetched
            ? "Can't accept() WebSocket that was already used in a response."
            : 'WebSocket has already been transferred');
        if (s.accepted) return;
        if (s.fetched) core.capsidFetchUpgradeAccept(s.channel);
        s.allowHalfOpen = Boolean(options?.allowHalfOpen ?? false);
        s.accepted = true;
        if (s.peer) deliverLocal(s.peer);
        // fix-review §11: a transfer may be waiting for this (delayed)
        // accept — the reference upgrades a peer accepted in a later
        // task turn after the 101 Response returned.
        const waiters = s.acceptWaiters;
        if (waiters) {
            s.acceptWaiters = null;
            for (const waiter of waiters) waiter();
        }
    }

    send(value) {
        const s = state(this);
        if (arguments.length === 0) {
            throw new TypeError("Failed to execute 'send' on 'WebSocket': 1 argument required, but only 0 present");
        }
        // JSG converts the argument before entering workerd's state checks,
        // including after user close() and after a handler error.
        if (typeof value === 'symbol') throw new TypeError('Cannot convert a Symbol value to a string');
        const binary = value instanceof ArrayBuffer || ArrayBuffer.isView(value);
        const bytes = binary
            ? value instanceof ArrayBuffer ? new Uint8Array(value)
                : new Uint8Array(value.buffer, value.byteOffset, value.byteLength)
            : core.capsidChannelEncodeText(value);
        if (s.origin !== core.capsidChannelContext()) throw new handlerErrorTypes.Error(crossRequestMessage);
        if (s.fetched && s.transferred) return;
        if (s.outgoingClosed) throw new TypeError("Can't call WebSocket send() after close().");
        if (s.failed && !s.handlerError) throw new TypeError('WebSocket is already in failure state');
        if (!s.accepted || s.transferred) throw new TypeError(s.fetched
            ? 'You must call one of accept() or state.acceptWebSocket() on this WebSocket before sending messages.'
            : 'WebSocket must be accepted before send()');
        // The transport has already gone. The reference still converts the
        // argument, then ignores sends without changing its directional state.
        if (s.disconnected || s.handlerOutgoingAborted) {
            core.capsidChannelCheckContext(s.origin);
            return;
        }
        if (s.channel && s.stage) {
            // Preserve call order until synchronous dispatch finishes. A
            // later handler throw must not discard an accepted send (C17).
            s.stage.push({ bytes: bytes.slice(), text: !binary });
            return;
        }
        if (s.channel) {
            core.capsidChannelSend(s.channel, bytes, !binary);
            return;
        }
        const charge = bytes.byteLength + 1024;
        if (bytes.byteLength > limits.message || charge > limits.queue - s.queuedBytes ||
            charge > limits.send - pendingBytes) throw new RangeError('WebSocket send budget exceeded');
        const copy = bytes.slice();
        s.queue.push({ bytes: copy, text: !binary, charge });
        s.queuedBytes += charge;
        s.reservation.bytes += charge;
        pendingBytes += charge;
        deliverLocal(s);
    }

    close(code = undefined, reason = undefined) {
        const s = state(this);
        // Convert once; Number() would accept boxed BigInt.
        const numeric = code === undefined ? undefined : +code;
        if (numeric !== undefined && (!(numeric >= 1000 && numeric < 5000) ||
            [1004, 1005, 1006, 1015].includes(numeric))) {
            throw new DOMException('Invalid WebSocket close code: ' + numeric + '.', 'InvalidAccessError');
        }
        if (reason !== undefined && code === undefined) {
            throw new DOMException('A close reason requires a code', 'InvalidAccessError');
        }
        const text = reason === undefined ? '' : decoder.decode(encoder.encode(toUSVString(reason)));
        if (encoder.encode(text).byteLength > 123) throw new DOMException('Close reason exceeds 123 bytes', 'SyntaxError');
        if (s.origin !== core.capsidChannelContext()) throw new handlerErrorTypes.Error(crossRequestMessage);
        if (s.fetched && s.transferred) return;
        if (s.outgoingClosed || s.handlerOutgoingAborted) return;
        // reportError stores the cloned exception before dispatching error.
        // During that dispatch close() rethrows it, while send() can still
        // append data. Once dispatch finishes both operations are ignored.
        if (s.handlerError) throw s.handlerError;
        if (s.failed) return;
        if (s.disconnected) {
            core.capsidChannelCheckContext(s.origin);
            // Incoming pair endpoints retain a local outgoing close. The
            // constructor and fetched endpoints ignore close() after a lost
            // transport, as observed independently on both native references.
            if (!s.outbound && !s.fetched) {
                if (s.channel) core.capsidChannelLocalClose(s.channel);
                s.outgoingClosed = true;
            }
            return;
        }
        if (s.connecting) {
            core.capsidChannelCancelDial(s.dial); s.outgoingClosed = true; return;
        }
        if (!s.accepted || s.transferred) throw new TypeError('WebSocket must be accepted before close()');
        if (s.channel && s.stage) {
            // inside a live handler: stage the close in call order with
            // sends (a send-then-close handler must deliver the message
            // first), including when the handler subsequently throws (C18).
            s.stage.push({ close: true, code: numeric ?? 1005, reason: text });
            s.outgoingClosed = true;
            if (!s.outbound && !s.fetched) core.capsidChannelLocalClose(s.channel);
            return;
        }
        if (s.channel) {
            // A close accepted during synchronous exception capture (or its
            // microtasks) belongs to the failing turn and must not wait for
            // a peer reply after the error is reported.
            core.capsidChannelClose(s.channel, numeric ?? 0, text, s.handlerFailurePending);
            s.nativeClosed = true;
        } else s.queue.push({ close: true, code: numeric ?? 1005, reason: text, charge: 0 });
        s.outgoingClosed = true;
        if (s.channel && !s.outbound && !s.fetched) core.capsidChannelLocalClose(s.channel);
        deliverLocal(s);
    }

    get readyState() {
        const s = state(this);
        if (s.origin !== core.capsidChannelContext()) {
            throw new Error(crossRequestMessage);
        }
        // Heap-only module-scope pairs have no request/native owner yet.
        if (s.origin !== 0n) core.capsidChannelCheckContext(s.origin);
        return s.failed || (s.incomingClosed && s.outgoingClosed) ? 3
            : s.incomingClosed || s.outgoingClosed ? 2 : s.connecting ? 0 : 1;
    }
    get binaryType() { return state(this).binaryType; }
    set binaryType(value) {
        const s = state(this);
        const text = toUSVString(value);
        if (text === 'blob' || text === 'arraybuffer') s.binaryType = text;
    }
    get url() { return state(this).url; }
    get protocol() { return state(this).protocol; }
    get extensions() { return state(this).extensions; }

    addEventListener(type, callback, options) {
        state(this);
        super.addEventListener(type, callback, options);
    }
    removeEventListener(type, callback, options) {
        state(this);
        super.removeEventListener(type, callback, options);
    }

}

for (const [name, value] of Object.entries({ CONNECTING: 0, OPEN: 1, CLOSING: 2, CLOSED: 3 })) {
    Object.defineProperty(WebSocket, name, { enumerable: true, value });
    Object.defineProperty(WebSocket.prototype, name, { enumerable: true, value });
}

class WebSocketPair {
    constructor() {
        const first = new WebSocket(internalEndpoint);
        const second = new WebSocket(internalEndpoint);
        state(first).peer = state(second);
        state(second).peer = state(first);
        Object.defineProperties(this, {
            0: { enumerable: true, value: first },
            1: { enumerable: true, value: second },
        });
    }
    *[Symbol.iterator]() { yield this[0]; yield this[1]; }
}

function clearQueue(s) {
    pendingBytes -= s.queuedBytes;
    s.queuedBytes = 0;
    s.reservation.bytes = 0;
    s.queue.length = 0;
}

async function dispatch(s, type, value, text, code, reason, clean) {
    const disconnected = type === 'disconnect';
    if (disconnected) type = 'close';
    if ((s.failed || s.handlerFailurePending) && type === 'message') return;
    let event;
    if (type === 'message') {
        const data = text ? decoder.decode(value)
            : s.binaryType === 'blob' ? new Blob([value]) : value.buffer;
        event = new MessageEvent('message', { data });
    } else if (type === 'close') {
        if (s.incomingClosed || s.disconnected) return;
        if (disconnected) {
            // A remote EOF is reported before the incoming direction closes.
            // Track transport disposal separately from JS failure/readiness.
            s.disconnected = true;
            clearQueue(s);
        } else {
            s.incomingClosed = true;
            if (!clean) { s.failed = true; clearQueue(s); }
        }
        // Reference default auto-replies before dispatching close. With
        // allowHalfOpen the handler may send its last message and close.
        if (clean && !s.allowHalfOpen && !s.outgoingClosed) s.socket.close(code === 1005 ? undefined : code, code === 1005 ? undefined : reason);
        event = new CloseEvent('close', { code, reason, wasClean: clean });
    } else if (type === 'error') {
        const ErrorType = /^Failed to establish the WebSocket connection: expected server to reply with HTTP status code 101 \(switching protocols\), but received \d{3} instead\.$/.test(reason)
            ? TypeError : Error;
        const error = new ErrorType(reason === 'WebSocket connect failed'
            ? 'Network connection lost.' : reason || 'WebSocket connection failed');
        event = new ErrorEvent('error', { error, message: 'Uncaught ' + error });
    } else event = new Event(type);
    const pending = [];
    let failurePromise;
    const stage = [];
    s.stage = stage;
    // Dispatch-entry order is the event order; commits must flush in
    // exactly that order even when handler scheduling delays an
    // append (observed: dispatch:await entered before dispatch:b but
    // appended after). The sequence key makes the flush order
    // structural instead of microtask-dependent.
    const seq = s.nextDispatchSeq;
    s.nextDispatchSeq += 1;
    try {
        dispatchTrustedEvent(s.socket, event, result => {
            if (result?.then) pending.push(Promise.resolve(result));
        });
    } catch (error) {
        stage.abort = true;
        stage.error = error;
        pending.push(Promise.reject(error));
    } finally {
        // Do this before the queued microtask checkpoint, without waiting for
        // listener-returned promises. A clean or resource-failure close keeps
        // its own existing transition and diagnostics.
        if (disconnected) s.incomingClosed = true;
        s.stage = null;
    }
    // Commit at the end of the handler's SYNCHRONOUS portion: a send
    // staged before the first await is delivered once that turn ends.
    // Synchronous throws retain accepted sends and closes before handling
    // the error. A native budget rejection still follows the abort path.
    const commit = () => {
        for (const item of stage) {
            if (item.close) {
                // A synchronous throw commits its accepted Close together
                // with transport termination; it must not wait for a reply.
                core.capsidChannelClose(s.channel, item.code === 1005 ? 0 : item.code, item.reason, !!stage.abort);
                s.nativeClosed = true;
            } else core.capsidChannelSend(s.channel, item.bytes, item.text);
        }
    };
    const guarded = () => {
        try { commit(); }
        catch (error) { failurePromise ??= queueHandlerFailure(s, error); }
    };
    const appendCommit = () => {
        s.pendingCommits.set(seq, guarded);
        while (s.pendingCommits.has(s.nextCommitSeq)) {
            const flush = s.pendingCommits.get(s.nextCommitSeq);
            s.pendingCommits.delete(s.nextCommitSeq);
            s.nextCommitSeq += 1;
            flush();
        }
    };
    if (stage.abort) {
        s.pendingCommits.set(seq, () => {
            guarded();
            failurePromise ??= queueHandlerFailure(s, stage.error);
        });
        while (s.pendingCommits.has(s.nextCommitSeq)) {
            const flush = s.pendingCommits.get(s.nextCommitSeq);
            s.pendingCommits.delete(s.nextCommitSeq);
            s.nextCommitSeq += 1;
            flush();
        }
    } else {
        appendCommit();
    }
    // fix-review §5: an ASYNC rejection of a listener-returned promise is
    // NOT fatal in the reference — the next message still flows and the
    // client closes cleanly. Swallow them; only synchronous throws
    // (stage.abort) and commit failures abort.
    if (pending.length !== 0) await Promise.all(pending).catch(() => {});
    if (failurePromise) await failurePromise;
}

function queueHandlerFailure(s, value) {
    if (s.failed || s.handlerFailurePending) return;
    s.handlerFailurePending = true;
    // Capture the thrown value in the synchronous catch turn. Reference
    // serialization reads user accessors before queued microtasks, while the
    // trusted error event itself is delivered after that checkpoint.
    let captured;
    try { captured = captureHandlerError(value); }
    catch { captured = internalHandlerError(); }
    // workerd finishes the turn's microtasks before reporting the error.
    // The private cleanup task has its own finite execution owner and survives
    // connection timer cancellation, including C18's accepted final Close.
    return new handlerErrorPromise(resolve => handlerErrorTimer(() => {
        s.failed = true;
        s.stage = null;
        clearQueue(s);
        s.handlerError = captured?.error ?? null;
        let delivered = false;
        try {
            if (!captured) throw new handlerErrorTypes.Error('Unable to capture the thrown value');
            const event = new ErrorEvent('error', captured);
            dispatchTrustedEvent(s.socket, event, result => {
                if (result?.then) handlerErrorPromise.resolve(result).catch(() => {});
            });
            delivered = true;
        } catch {
            // The reference keeps C17's transport when its error listener
            // itself throws. C18's accepted final Close already owns teardown.
        }
        if (delivered) {
            // CLOSED readiness reflects the stored exception, independently
            // of user close(). Synchronous listener writes are accepted;
            // later sends still convert but cannot append transport data.
            s.handlerOutgoingAborted = true;
            if (s.channel && !s.nativeClosed) {
                try { core.capsidChannelClose(s.channel, 1006, 'event handler failed'); }
                catch { /* the peer transport has already ended */ }
            }
        }
        resolve();
    }, 0));
}

// Native asynchronous consumers reconstruct an exception from the same
// serialized value representation as a failed connection event handler.
export function captureNativeException(value) {
    return captureHandlerError(value).error;
}

function captureHandlerError(value) {
    const kind = core.capsidChannelValueKind(value);
    // Use the native getter's brand check, including DOMExceptions whose
    // prototype or own name/message has been replaced by the application.
    let domName;
    try { domName = handlerErrorDOMName(value); } catch { /* different native brand */ }
    if (domName !== undefined) {
        // Serialization and metadata extraction still read the public fields.
        // The tunneled DOMException itself uses its private name and message.
        try { value.name; value.message; } catch { /* unavailable serialized detail */ }
        value.name; value.stack;
        const error = new handlerErrorDOMException(handlerErrorDOMMessage(value), domName);
        for (const [key, field] of [['code', handlerErrorDOMCode(value)], ['name', domName],
            ['message', handlerErrorDOMMessage(value)]])
            handlerErrorDefineProperty(error, key, {value: field, enumerable: true, configurable: true});
        setHandlerStack(error, true);
        return {error, message: 'Uncaught '};
    }
    if (kind === 2 || kind === 3 || kind === 4) {
        try {
            const error = cloneHandlerObject(value, new handlerErrorSeen());
            return {error, message: kind === 3 ? 'Uncaught [object Array]' : 'Uncaught #<Object>'};
        } catch {
            // The reference's fallback description does not call application
            // conversion methods. A Proxy must not run even reflective traps.
            const descriptor = kind === 2 ? handlerErrorOwnDescriptor(value, 'toString') : undefined;
            const message = typeof descriptor?.value === 'function' ? '[object Object]' : '#<Object>';
            const error = newHandlerError('Error', message);
            return {error, message: 'Uncaught ' + handlerErrorToString(error)};
        }
    }
    if (kind !== 1) {
        const error = newHandlerError('Error', handlerErrorString(value));
        return {error, message: 'Uncaught ' + handlerErrorToString(error)};
    }
    let name, message = '', error;
    try {
        name = value.name;
        // The serializer converts the name for its tag, but unknown names
        // retain their original primitive value in the reconstructed Error.
        handlerErrorString(name);
        try { message = value.message; } catch { /* unreadable message */ }
        if (typeof message !== 'string') message = '';
        error = cloneHandlerNativeError(value, new handlerErrorSeen(), name, message);
    } catch { /* fall back to the exception metadata when detail is uncloneable */ }
    // Metadata is extracted after the attempted serialization, including when
    // serialization failed. An unreadable metadata getter is an internal
    // capture failure, rather than a reason to lose the error event and EOF.
    const metadataName = value.name;
    value.stack;
    if (!error) error = newHandlerError(typeof metadataName === 'string' ? metadataName : 'Error', message);
    const eventName = error.name;
    return {error, message: 'Uncaught ' + (typeof eventName === 'string'
        ? eventName + (error.message ? ': ' : '') : '') + error.message};
}

function newHandlerError(name, message) {
    const Constructor = typeof name === 'string' && handlerErrorHasOwn(handlerErrorTypes, name)
        ? handlerErrorTypes[name] : handlerErrorTypes.Error;
    const error = Constructor === handlerErrorTypes.AggregateError ? new Constructor([], message) : new Constructor(message);
    if (Constructor === handlerErrorTypes.AggregateError) delete error.errors;
    setHandlerStack(error);
    return error;
}

function setHandlerStack(error, enumerable = false) {
    // Deserialization creates an exception without JS call frames. V8 exposes
    // its freshly formatted first line through a configurable stack accessor.
    let formatted, assigned = false;
    handlerErrorDefineProperty(error, 'stack', {enumerable, configurable: true,
        get() { if (!assigned) { formatted = handlerErrorToString(error); assigned = true; } return formatted; },
        set(value) { formatted = value; assigned = true; }});
}

function internalHandlerError(detail = 'WebSocket exception capture failed') {
    // A real diagnostic reference connects the public fallback to a log
    // record. Never reuse a literal upstream ID or coerce the failed getter.
    const bytes = new handlerErrorBytes(15);
    handlerErrorRandom(bytes.buffer, 0, bytes.length);
    const alphabet = '0123456789abcdefghijklmnopqrstuv';
    let id = '', bits = 0, buffer = 0;
    for (const byte of bytes) {
        buffer = (buffer << 8) | byte; bits += 8;
        while (bits >= 5) { bits -= 5; id += alphabet[(buffer >>> bits) & 31]; }
    }
    const message = 'internal error; reference = ' + id;
    handlerErrorLog('error', detail + '; reference = ' + id);
    const error = newHandlerError('Error', message);
    return {error, message: 'Uncaught Error: ' + message};
}

function cloneHandlerNativeError(value, seen, name, message) {
    // Host error serialization cannot encode an Error's reference back to an
    // active Error. Ordinary object/array cycles remain supported by seen.
    if (handlerErrorSeenHas(seen, value)) throw new handlerErrorTypes.TypeError('Cyclic native error');
    const error = newHandlerError(name, message);
    handlerErrorSeenSet(seen, value, error);
    if (typeof name !== 'string' || !handlerErrorHasOwn(handlerErrorTypes, name)) {
        if (name !== null && (typeof name === 'object' || typeof name === 'symbol' || typeof name === 'function'))
            throw new handlerErrorTypes.TypeError('Uncloneable error name');
        handlerErrorDefineProperty(error, 'name', {value: name, writable: true, enumerable: true, configurable: true});
    }
    const properties = {};
    // Gather all non-index own values first. Symbol getters run, even though
    // the temporary ordinary object's symbol keys are omitted on the wire.
    for (const key of handlerErrorOwnKeys(value)) {
        if (key === 'name' || key === 'message') continue;
        if (typeof key === 'string' && key !== '4294967295' && handlerErrorString(key >>> 0) === key) continue;
        handlerErrorDefineProperty(properties, key, {value: value[key], enumerable: true, configurable: true});
    }
    for (const key of handlerErrorObjectKeys(properties)) {
        const field = cloneHandlerObject(properties[key], seen);
        if (key !== 'stack') handlerErrorDefineProperty(error, key,
            {value: field, writable: true, enumerable: true, configurable: true});
    }
    return error;
}

function cloneHandlerObject(value, seen) {
    const type = typeof value;
    if (type === 'symbol' || type === 'function') throw new handlerErrorTypes.TypeError('Uncloneable thrown value');
    if (value === null || type !== 'object') return value;
    const kind = core.capsidChannelValueKind(value);
    if (kind === 1) {
        const name = value.name;
        handlerErrorString(name);
        let message = value.message;
        if (typeof message !== 'string') message = '';
        return cloneHandlerNativeError(value, seen, name, message);
    }
    if (handlerErrorSeenHas(seen, value)) return handlerErrorSeenGet(seen, value);
    if (kind !== 2 && kind !== 3) throw new handlerErrorTypes.TypeError('Unsupported thrown object');
    const copy = kind === 3 ? new handlerErrorArray(value.length) : {};
    handlerErrorSeenSet(seen, value, copy);
    // String enumerable keys match ordinary structured data. Symbol keys are
    // ignored; accessors execute once in the synchronous capture turn. Define
    // data properties explicitly so a literal __proto__ key remains data.
    for (const key of handlerErrorObjectKeys(value)) {
        if (!handlerErrorOwnDescriptor(value, key)?.enumerable) continue;
        handlerErrorDefineProperty(copy, key, {value: cloneHandlerObject(value[key], seen),
            writable: true, enumerable: true, configurable: true});
    }
    return copy;
}

// After committing accepted operations, a handler throw aborts an open
// transport without adding a Close frame (C17). An already committed
// final close keeps its frame and reason before ending the transport (C18).
// Client event delivery
// can race transport disposal; the wire regression checks the actual bytes.
function abortFromHandler(s) {
    // outgoingClosed may reflect a STAGED close that never reached the
    // transport (fix-review §3): the abort must still produce a
    // terminal, so only a previously EXECUTED abort (failed) or a
    // natively-committed close (nativeClosed) short-circuits.
    if (s.failed || s.nativeClosed) return;
    s.failed = true;
    s.stage = null;
    clearQueue(s);
    if (s.channel) {
        try { core.capsidChannelClose(s.channel, 1006, 'event handler failed'); }
        catch { /* the transport is already gone */ }
    }
    s.outgoingClosed = true;
}

function deliverLocal(s) {
    const peer = s.peer;
    if (!peer?.accepted || s.channel || s.transferred) return;
    while (s.queue.length) {
        const item = s.queue.shift();
        s.queuedBytes -= item.charge;
        s.reservation.bytes -= item.charge;
        pendingBytes -= item.charge;
        Promise.resolve().then(() => item.close
            ? dispatch(peer, 'close', null, false, item.code, item.reason, true)
            : dispatch(peer, 'message', item.bytes, item.text)).catch(() => {});
    }
}

// fix-review §4: constructor-time brand check for Response 101 — the
// value must be a WebSocketPair endpoint; accept/role/readiness stay
// send-time concerns.
export function isWebSocketEndpoint(socket) {
    return socket !== null && typeof socket === 'object' &&
        endpoints.has(socket);
}

export function validateResponseWebSocket(socket) {
    const s = state(socket);
    if (s.outbound && s.connecting)
        throw new TypeError("Can't return WebSocket in a Response if it was created with `new WebSocket()`");
    if (s.accepted)
        throw new TypeError("Can't return WebSocket in a Response after calling accept().");
    if (s.transferred)
        throw new TypeError("Can't return WebSocket that was already used in a response.");
    if (!s.fetched && (s.outbound || s.peer.channel || s.peer.transferred)) {
        throw new TypeError('Response requires an unaccepted endpoint with an accepted peer');
    }
}
export function setResponseWebSocket(response, socket) { intrinsicWeakSet(responseSockets, response, socket); }
export function getResponseWebSocket(response) { return intrinsicWeakGet(responseSockets, response) ?? null; }

// A fetch-created endpoint shares the same native channel and ownership core
// as constructor sockets. Non-101 responses use a credit-controlled HTTP body
// on that owner; no second request or independent transport is created here.
export function openFetchUpgrade(url, headers, redirect, signal) {
    if (signal?.aborted) return handlerErrorPromise.reject(signal.reason);
    const origin = core.capsidChannelContext();
    return new handlerErrorPromise((resolve, reject) => {
        let dial = 0n, channel = 0, socketState, controller, pullReady;
        let settled = false, ended = false, cancelled = false;
        let gzipDecoder;
        const disposeDecoder = () => {
            if (gzipDecoder) {gzipDecoder.dispose(); gzipDecoder = null;}
        };
        const chunks = [];
        const checkContext = () => {
            if (origin !== core.capsidChannelContext()) throw new handlerErrorTypes.Error(crossRequestMessage);
            core.capsidChannelCheckContext(origin);
        };
        const removeAbort = () => signal?.removeEventListener('abort', abort);
        const completePull = () => {const complete = pullReady; pullReady = null; complete?.();};
        const failBody = error => {
            cancelled = true; chunks.length = 0; disposeDecoder();
            controller.error(error); completePull(); removeAbort();
            core.capsidChannelCancelDial(dial);
        };
        // The channel window accounts for encoded transport bytes. Decode only
        // into the outstanding caller buffer and return credit for input that
        // the bounded native decoder actually consumed.
        const flushGzip = () => {
            const request = controller.byobRequest;
            const view = request ? request.view : new handlerErrorBytes(4096);
            const capacity = intrinsicApply(intrinsicTypedLength, view, []);
            let written = 0;
            try {
                while (written < capacity) {
                    const chunk = chunks[0];
                    const length = chunk ? intrinsicApply(intrinsicTypedLength, chunk, []) : 0;
                    const offset = chunk ? intrinsicApply(intrinsicTypedOffset, chunk, []) : 0;
                    const buffer = chunk ? intrinsicApply(intrinsicTypedBuffer, chunk, []) : null;
                    const step = gzipDecoder.step(buffer, offset, length, Math.min(4096, capacity - written));
                    if (step.consumed) {
                        if (step.consumed === length) chunks.shift();
                        else chunks[0] = new handlerErrorBytes(buffer, offset + step.consumed, length - step.consumed);
                        core.capsidFetchBodyConsume(channel, step.consumed);
                    }
                    const bytes = new handlerErrorBytes(step.data);
                    const count = intrinsicApply(intrinsicTypedLength, bytes, []);
                    if (count) {intrinsicByteSet(view, bytes, written); written += count;}
                    if (step.atMemberEnd && written) break;
                    if (!step.consumed && !count) {
                        if (length) throw new handlerErrorTypes.Error('Private decoder made no progress');
                        // Publish an accepted prefix before validating final
                        // EOF. A subsequent read observes a truncated member.
                        if (!written && ended) {
                            gzipDecoder.finish(); disposeDecoder();
                            controller.close(); controller.byobRequest?.respond(0);
                            completePull(); removeAbort(); return;
                        }
                        break;
                    }
                }
                if (written) {
                    if (request) request.respond(written);
                    else controller.enqueue(new handlerErrorBytes(intrinsicApply(intrinsicTypedBuffer, view, []), 0, written));
                    completePull();
                }
            } catch (error) {failBody(error);}
        };
        const flushBody = () => {
            if (!controller || !pullReady || cancelled) return;
            if (gzipDecoder) {flushGzip(); return;}
            if (chunks.length) {
                const chunk = chunks[0], length = intrinsicApply(intrinsicTypedLength, chunk, []);
                const request = controller.byobRequest;
                let consumed = length;
                if (request) {
                    const view = request.view, available = intrinsicApply(intrinsicTypedLength, view, []);
                    if (consumed > available) consumed = available;
                    const buffer = intrinsicApply(intrinsicTypedBuffer, chunk, []);
                    const offset = intrinsicApply(intrinsicTypedOffset, chunk, []);
                    intrinsicByteSet(view, new handlerErrorBytes(buffer, offset, consumed));
                    if (consumed === length) chunks.shift();
                    else chunks[0] = new handlerErrorBytes(buffer, offset + consumed, length - consumed);
                    request.respond(consumed);
                } else {
                    chunks.shift(); controller.enqueue(chunk);
                }
                core.capsidFetchBodyConsume(channel, consumed); completePull();
            } else if (ended) {
                controller.close(); controller.byobRequest?.respond(0); completePull(); removeAbort();
            }
        };
        const abort = () => {
            if (cancelled || ended) return;
            const reason = getAbortException(signal); cancelled = true; chunks.length = 0; disposeDecoder();
            try {if (dial) core.capsidChannelCancelDial(dial);}
            finally {
                if (!settled) {settled = true; reject(reason);}
                if (controller) {controller.error(reason); completePull();}
                removeAbort();
            }
        };
        const callback = (generation, type, bytes, text, code, reason, clean, id, http) => {
            runChannelEvent(generation, async () => {
                if (!core.capsidChannelEventStarted(generation)) return;
                if (cancelled) return;
                if (type === 'fetchresponse') {
                    channel = id;
                    if (http.status === 101) {
                        const socket = new WebSocket(internalEndpoint);
                        socketState = state(socket);
                        Object.assign(socketState, {fetched: true, outbound: true, connecting: false, dial, channel,
                            origin, url: null, protocol: '', extensions: ''});
                        settled = true; removeAbort(); resolve({metadata: http, webSocket: socket, body: null});
                    } else {
                        if (![204, 205, 304].includes(http.status)) {
                            for (const [name, value] of http.headers) {
                                if (intrinsicLowerCase(name) === 'content-encoding' && intrinsicLowerCase(value) === 'gzip') {
                                    gzipDecoder = createHttpGzipDecoder(); break;
                                }
                            }
                        }
                        const body = [204, 205, 304].includes(http.status) ? null : new fetchReadableStream({
                            type: 'bytes', autoAllocateChunkSize: 4096,
                            start(value) {controller = value;},
                            pull() {
                                checkContext();
                                const ready = new handlerErrorPromise(done => {pullReady = done; flushBody();});
                                return {then(resolve, reject) {intrinsicApply(intrinsicThen, ready, [resolve, reject]);}};
                            },
                            cancel() {
                                checkContext(); cancelled = true; chunks.length = 0; disposeDecoder();
                                core.capsidChannelCancelDial(dial); completePull(); removeAbort();
                            },
                        });
                        if (body) {
                            markNativeByteStream(body);
                            intrinsicWeakSet(fetchStreamContexts, body, {checkContext, cancellation: null});
                        }
                        settled = true; resolve({metadata: http, webSocket: null, body});
                    }
                } else if (type === 'failure') {
                    const error = internalHandlerError('HTTP upgrade failed: ' + reason).error;
                    removeAbort();
                    if (!settled) {settled = true; reject(error);}
                    else if (controller) {cancelled = true; chunks.length = 0; disposeDecoder(); controller.error(error); completePull();}
                } else if (socketState) {
                    await dispatch(socketState, type, bytes, text, code, reason, clean);
                } else if (type === 'message') {
                    chunks.push(bytes); flushBody();
                } else if (type === 'bodyend') {
                    ended = true; flushBody(); if (!controller) removeAbort();
                } else if (type === 'bodyerror') {
                    const error = reason === 'HTTP response body parse failed'
                        ? internalHandlerError(reason).error
                        : new handlerErrorTypes.Error('Network connection lost.');
                    cancelled = true; chunks.length = 0; disposeDecoder(); removeAbort();
                    if (controller) {controller.error(error); completePull();}
                    if (!settled) {settled = true; reject(error);}
                }
            }, error => {
                if (socketState) abortFromHandler(socketState);
                else {
                    cancelled = true; chunks.length = 0; disposeDecoder(); removeAbort();
                    if (!settled) {settled = true; reject(error);}
                    if (controller) {controller.error(error); completePull();}
                    try {core.capsidChannelCancelDial(dial);} catch {}
                }
            });
        };
        signal?.addEventListener('abort', abort, {once: true});
        try {
            dial = core.capsidFetchUpgradeOpen(url, headers, redirect, callback);
            if (signal?.aborted) abort();
        } catch (error) {removeAbort(); reject(error);}
    });
}

export async function transferWebSocket(id, response, headers) {
    const socket = getResponseWebSocket(response);
    const client = state(socket);
    const server = client.peer;
    if (client.origin !== core.capsidChannelContext()) {
        throw new TypeError('Cannot perform I/O on behalf of a different request');
    }
    const stage = { mode: 'preparing', transportTerminated: false };
    stage.ready = new Promise(resolve => { stage.resolveReady = resolve; });
    stage.terminal = new Promise(resolve => { stage.resolveTerminal = resolve; });
    const callback = (generation, type, bytes, text, code, reason, clean) => {
        if (client.fetched) {
            // A terminal can cross the committed header before native coupling.
            // Settle that provisional event without exposing a JS relay.
            core.capsidChannelEventStarted(generation);
            core.capsidChannelEventSettled(generation);
            return;
        }
        runChannelEvent(generation, async () => {
            if (!core.capsidChannelEventStarted(generation)) return;
            if (type === 'close' && !clean || type === 'disconnect' || type === 'failure') {
                stage.transportTerminated = true;
                stage.resolveTerminal?.('disconnected');
            }
            if (stage.mode === 'preparing') {
                if (!await stage.ready || stage.mode !== 'ready') return;
            } else if (stage.mode === 'failed') return;
            await dispatch(server, type, bytes, text, code, reason, clean);
        }, () => {
            if (stage.mode === 'ready') abortFromHandler(server);
            else stage.resolveTerminal?.('disconnected');
        });
    };
    let channel = 0, activated = false;
    try {
        // The reference sends 101 before it couples the endpoint. Origin and
        // negotiated-request checks remain before commit; role/readiness errors
        // after commit abort that transport and cannot become a second HTTP 500.
        channel = await core.capsidChannelAttach(id, response.statusText, headers, callback);
        validateResponseWebSocket(socket);
        client.transferred = true;
        if (client.fetched) {
            core.capsidChannelCouple(channel, client.channel);
            activated = true;
            stage.resolveReady(false);
            return;
        }
        server.channel = channel;
        if (!server.accepted) {
            let fallback, waiter;
            try {
                const accepted = new Promise(resolve => {
                    waiter = () => resolve('accepted');
                    (server.acceptWaiters ??= []).push(waiter);
                });
                const timeout = new Promise(resolve => { fallback = setTimeout(() => resolve('timeout'), 10000); });
                const result = await Promise.race([accepted, stage.terminal, timeout]);
                if (result !== 'accepted') throw new TypeError(result === 'timeout'
                    ? 'WebSocket peer was not accepted before the coupling deadline'
                    : 'WebSocket disconnected before peer acceptance');
            } finally {
                clearTimeout(fallback);
                const index = server.acceptWaiters?.indexOf(waiter) ?? -1;
                if (index >= 0) server.acceptWaiters.splice(index, 1);
            }
        }
        core.capsidChannelActivate(channel, server.outgoingClosed);
        activated = true;
        stage.mode = 'ready';
        stage.resolveReady(true);
        // send() may have copied data before the handler returned its 101.
        // Move it into the bounded native queue in call order, then Close.
        for (const item of server.queue) {
            if (item.close) core.capsidChannelClose(server.channel, item.code === 1005 ? 0 : item.code, item.reason);
            else core.capsidChannelSend(server.channel, item.bytes, item.text);
        }
        clearQueue(server);
    } catch (error) {
        if (activated) {
            abortFromHandler(server);
        } else {
            stage.mode = 'failed';
            stage.resolveReady(false);
            if (server && client.transferred) { server.failed = true; clearQueue(server); }
            if (channel) {
                try { core.capsidChannelClose(channel, 1006, 'WebSocket coupling failed'); }
                catch { /* a peer terminal or cancellation already stopped I/O */ }
            }
        }
        if (!channel) throw error;
        try { core.capsidLog('error', String(error)); }
        catch { /* the originating request may already be cancelled */ }
    } finally {
        // The network owner outlives HTTP. It must not retain the HTTP job's
        // pending promises/resolvers after the coupling decision is complete.
        stage.ready = stage.terminal = stage.resolveReady = stage.resolveTerminal = null;
    }
}

Object.defineProperties(globalThis, {
    WebSocket: { configurable: true, writable: true, value: WebSocket },
    WebSocketPair: { configurable: true, writable: true, value: WebSocketPair },
    CloseEvent: { configurable: true, writable: true, value: CloseEvent },
});
