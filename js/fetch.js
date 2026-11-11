import { validateResponseWebSocket, setResponseWebSocket, getResponseWebSocket, isWebSocketEndpoint, captureNativeException, openFetchUpgrade, configureFetchChannelStreams, inheritFetchChannelStream, isFetchChannelStream } from './websocket.js';
import { configureAbortExceptions } from 'txiki-polyfills/abort-controller.js';
import { configureResponseBodyFactory } from 'txiki-polyfills/fetch/fetch.js';
import { configureOwnedHeadersCloner } from 'txiki-polyfills/fetch/headers.js';
import { createOrdinaryFetchBody, isOrdinaryFetchBody, teeOrdinaryFetchBody } from './websocket.js';

configureAbortExceptions(captureNativeException);

const NativeHeaders = globalThis.Headers;
const NativeRequest = globalThis.Request;
const NativeResponse = globalThis.Response;
const nativeFetch = globalThis.fetch;

function normalizeRequestInfo(input) {
    // Preserve native Request bodies and brands (including the base instances
    // returned by clone). Other RequestInfo values use one Web IDL string
    // conversion, before RequestInit getters are observed.
    return input instanceof NativeRequest ? input : `${input}`;
}
const emptyBodyIntrinsics = {
    bufferLength: Object.getOwnPropertyDescriptor(ArrayBuffer.prototype, 'byteLength').get,
    typedLength: Object.getOwnPropertyDescriptor(Object.getPrototypeOf(Uint8Array.prototype), 'byteLength').get,
    viewLength: Object.getOwnPropertyDescriptor(DataView.prototype, 'byteLength').get,
    viewBuffer: Object.getOwnPropertyDescriptor(DataView.prototype, 'buffer').get,
    blobSize: Object.getOwnPropertyDescriptor(Blob.prototype, 'size').get,
    paramsString: URLSearchParams.prototype.toString,
    isView: ArrayBuffer.isView,
};
const bodyTypeError = globalThis.TypeError;
const bodyProperty = Object.getOwnPropertyDescriptor;

function trackBodyUsage(body) {
    let consumed = body.bodyUsed;
    Object.defineProperty(body, 'bodyUsed', {
        configurable: true,
        enumerable: true,
        get() {
            // The pinned streams implementation records disturbance for read,
            // cancel, tee and pipe operations. Pulling into its internal queue
            // does not disturb it, and merely acquiring a reader does not count.
            const stream = this._bodyStream;
            return consumed || (stream !== null && stream !== undefined &&
                bodyProperty(stream, '_disturbed')?.value === true);
        },
        // The native BodyMixin marks its convenience consumers before reading.
        set(value) {consumed = Boolean(value);},
    });
}

let maxFetchRequestBodyBytes = 0;
let maxFetchResponseBodyBytes = 0;

export function configureFetchLimits(requestBytes, responseBytes) {
    const requestLimit = Number(requestBytes);
    const responseLimit = Number(responseBytes);

    if (!Number.isSafeInteger(requestLimit) || requestLimit < 0 ||
        !Number.isSafeInteger(responseLimit) || responseLimit < 0) {
        throw new TypeError('invalid native Fetch body limits');
    }
    maxFetchRequestBodyBytes = requestLimit;
    maxFetchResponseBodyBytes = responseLimit;
    configureFetchChannelStreams(globalThis.ReadableStream);
    configureResponseBodyFactory(client => createOrdinaryFetchBody(client, maxFetchResponseBodyBytes));
}

const headerLists = new WeakMap();
const setCookieLists = new WeakMap();
const iteratorStates = new WeakMap();
const fastResponseBodies = new WeakMap();
const immutableHeaders = new WeakSet();
const headerBuiltinString = String;
const headerLowerAscii = Function.prototype.call.bind(String.prototype.toLowerCase);
const headerCode = Function.prototype.call.bind(String.prototype.charCodeAt);
const headerSubarray = Function.prototype.call.bind(Uint8Array.prototype.subarray);
const headerByteArray = Uint8Array;
const headerBigInt = BigInt;
const headerChar = String.fromCharCode;
// kj::String conversion preserves unpaired UTF-16 code units as WTF-8.
// The reference decodes their invalid byte sequence at the JS return boundary.
function headerEncode(value) {
    const bytes = new headerByteArray(value.length * 3);let at = 0;
    for (let index = 0; index < value.length; ++index) {
        let code = headerCode(value, index);
        if (code >= 0xd800 && code <= 0xdbff && index + 1 < value.length) {
            const low = headerCode(value, index + 1);
            if (low >= 0xdc00 && low <= 0xdfff) {code = 0x10000 + ((code - 0xd800) << 10) + low - 0xdc00; ++index;}
        }
        if (code < 0x80) bytes[at++] = code;
        else if (code < 0x800) {bytes[at++] = 0xc0 | (code >> 6); bytes[at++] = 0x80 | (code & 63);}
        else if (code < 0x10000) {bytes[at++] = 0xe0 | (code >> 12); bytes[at++] = 0x80 | ((code >> 6) & 63); bytes[at++] = 0x80 | (code & 63);}
        else {bytes[at++] = 0xf0 | (code >> 18); bytes[at++] = 0x80 | ((code >> 12) & 63); bytes[at++] = 0x80 | ((code >> 6) & 63); bytes[at++] = 0x80 | (code & 63);}
    }
    return headerSubarray(bytes, 0, at);
}
const headerDecode = Function.prototype.call.bind(TextDecoder.prototype.decode, new TextDecoder('utf-8', {ignoreBOM: true}));
// Keep uncommon names as canonical bytes. The pinned64-bit SWAR conversion
// also has observable non-ASCII results; decoding only at display preserves
// distinct raw keys that happen to render to the same replacement string.
function lowerHeaderKey(name) {
    if (!/[^\x00-\x7f]/.test(name)) return headerLowerAscii(name);
    const bytes = headerEncode(name);
    for (let offset = 0; offset < bytes.length; offset += 8) {
        const count = Math.min(8, bytes.length - offset);let word = 0n;
        for (let index = 0; index < count; ++index) word |= headerBigInt(bytes[offset + index]) << headerBigInt(index * 8);
        word ^= (((word + 0x3f3f3f3f3f3f3f3fn) ^ (word + 0x2525252525252525n)) & 0x8080808080808080n) >> 2n;
        for (let index = 0; index < count; ++index) bytes[offset + index] = Number((word >> headerBigInt(index * 8)) & 255n);
    }
    let raw = '';for (const byte of bytes) raw += headerChar(byte);return raw;
}
function displayHeaderKey(raw) {
    if (!/[^\x00-\x7f]/.test(raw)) return raw;
    const bytes = new headerByteArray(raw.length);
    for (let index = 0; index < raw.length; ++index) bytes[index] = raw.charCodeAt(index);
    return headerDecode(bytes);
}
const headerPrimitiveSymbol = Symbol.toPrimitive;
// Private state must not call application replacements of WeakMap methods.
const headerWeakGet = Function.prototype.call.bind(WeakMap.prototype.get);
const headerWeakSet = Function.prototype.call.bind(WeakMap.prototype.set);
const headerMapGet = Function.prototype.call.bind(Map.prototype.get);
const headerMapHas = Function.prototype.call.bind(Map.prototype.has);
const readableLocked = Object.getOwnPropertyDescriptor(ReadableStream.prototype, 'locked').get;
const headerInvocationMessage = 'Illegal invocation: function called with incorrect `this` reference. See https://developers.cloudflare.com/workers/observability/errors/#illegal-invocation-errors for details.';
const isObjectValue = value => value !== null && (typeof value === 'object' || typeof value === 'function');
function headerMutationString(value) {
    if (isObjectValue(value)) {
        const exotic = value[headerPrimitiveSymbol];
        if (exotic !== undefined && exotic !== null) {
            if (typeof exotic !== 'function') {
                const label = isObjectValue(exotic) ? '#<Object>' : headerBuiltinString(exotic);
                throw new bodyTypeError(`'${label}' returned for property 'Symbol(Symbol.toPrimitive)' of object '#<Object>' is not a function`);
            }
            value = applyIntrinsic(exotic, value, ['string']);
            if (isObjectValue(value)) throw new bodyTypeError('Cannot convert object to primitive value');
        } else {
            let primitive = false;
            for (const name of ['toString', 'valueOf']) {
                const method = value[name];
                if (typeof method !== 'function') continue;
                const result = applyIntrinsic(method, value, []);
                if (!isObjectValue(result)) {value = result; primitive = true; break;}
            }
            if (!primitive) throw new bodyTypeError('Cannot convert object to primitive value');
        }
    }
    if (typeof value === 'symbol') throw new bodyTypeError('Cannot convert a Symbol value to a string');
    return headerBuiltinString(value);
}
function headerReadInput(headers, operation, count, name) {
    const list = headerWeakGet(headerLists, headers);
    if (list === undefined) throw new bodyTypeError(headerInvocationMessage);
    if (count < 1) {
        throw new bodyTypeError(`Failed to execute '${operation}' on 'Headers': parameter 1 is not of type 'string'.`);
    }
    return {list, name: normalizeName(name)};
}
function headerMutationInput(headers, operation, count, name, value) {
    const list = headerWeakGet(headerLists, headers);
    if (list === undefined) throw new bodyTypeError(headerInvocationMessage);
    const missing = index => new bodyTypeError(`Failed to execute '${operation}' on 'Headers': parameter ${index} is not of type 'string'.`);
    if (count < 1) throw missing(1);
    name = headerMutationString(name);
    if (operation !== 'delete') {
        if (count < 2) throw missing(2);
        value = headerMutationString(value);
    }
    requireMutableHeaders(headers);
    const normalizedValue = operation === 'delete' ? undefined : normalizeValue(value);
    return {list, name: normalizeName(name), value: normalizedValue};
}
const ordinaryFetchStreams = new WeakSet();
const weakHas = Function.prototype.call.bind(WeakSet.prototype.has);
const weakAdd = Function.prototype.call.bind(WeakSet.prototype.add);
const applyIntrinsic = Reflect.apply;
function requireMutableHeaders(headers) {
    if (weakHas(immutableHeaders, headers)) throw new bodyTypeError("Can't modify immutable headers.");
}
function finishFetchedResponse(response, url, redirected) {
    response.url = url;
    response.redirected = Boolean(redirected);
    weakAdd(immutableHeaders, response.headers);
    if (response.body !== null) weakAdd(ordinaryFetchStreams, response.body);
    return response;
}
const streamTee = ReadableStream.prototype.tee;
Object.defineProperty(ReadableStream.prototype, 'tee', {
    ...Object.getOwnPropertyDescriptor(ReadableStream.prototype, 'tee'),
    value: function tee() {
        if ((weakHas(ordinaryFetchStreams, this) || isOrdinaryFetchBody(this)) && applyIntrinsic(readableLocked, this, []))
            throw new bodyTypeError('This ReadableStream is currently locked to a reader,');
        const nativeBranches = teeOrdinaryFetchBody(this);
        const branches = nativeBranches ?? applyIntrinsic(streamTee, this, []);
        if (!nativeBranches) for (const branch of branches) inheritFetchChannelStream(this, branch);
        if (weakHas(ordinaryFetchStreams, this)) this._disturbed = true;
        return branches;
    },
});
const incomingRequestBrand = Symbol('capsid incoming request');
const arrayIteratorPrototype =
    Object.getPrototypeOf(Object.getPrototypeOf([][Symbol.iterator]()));

function toByteString(value) {
    const string = headerBuiltinString(value);

    for (let i = 0; i < string.length; i++) {
        if (string.charCodeAt(i) > 0xff) {
            throw new TypeError('Value is not a valid ByteString');
        }
    }

    return string;
}

function normalizeName(value) {
    const name = headerMutationString(value), forbidden = '()<>@,;:\\"/[]?={}';
    for (let index = 0; index < name.length; ++index) {
        const code = name.charCodeAt(index);
        if (code <= 32 || code === 127 || forbidden.includes(name[index]))
            throw new bodyTypeError('Invalid header name.');
    }
    return lowerHeaderKey(name);
}

function normalizeValue(value) {
    const string = headerMutationString(value).replace(/^[\t\r\n ]+|[\t\r\n ]+$/g, '');
    if (/[\0\r\n]/.test(string)) throw new bodyTypeError('Invalid header value.');
    return headerDecode(headerEncode(string));
}

function requireHeaders(value) {
    const list = headerWeakGet(headerLists, value);
    if (list === undefined) throw new bodyTypeError(headerInvocationMessage);
    return list;
}

function sortedEntries(headers) {
    const entries = [];
    const list = requireHeaders(headers);

    for (const name of Array.from(list.keys()).sort()) {
        if (name === 'set-cookie') {
            for (const value of headerWeakGet(setCookieLists, headers)) {
                entries.push([ name, value ]);
            }
        } else {
            entries.push([ displayHeaderKey(name), list.get(name) ]);
        }
    }
    return entries;
}

const headersIteratorPrototypes = Object.create(null);
for (const [kind, label] of [['entry', 'EntryIterator'], ['key', 'KeyIterator'], ['value', 'ValueIterator']]) {
    const prototype = Object.create(arrayIteratorPrototype);
    Object.defineProperties(prototype, {
        next: {
            configurable: true,
            enumerable: true,
            writable: true,
            value: function next() {
                const state = headerWeakGet(iteratorStates, this);
                if (!state || state.kind !== kind) throw new bodyTypeError(headerInvocationMessage);
                if (state.index >= state.entries.length) return {done: true, value: undefined};
                const [name, value] = state.entries[state.index++];
                return {done: false, value: kind === 'key' ? name : kind === 'value' ? value : [name, value]};
            },
        },
        [Symbol.iterator]: {
            configurable: true,
            writable: true,
            // The pinned iterator method is anonymous and checks its kind.
            value: function () {
                const state = headerWeakGet(iteratorStates, this);
                if (!state || state.kind !== kind) throw new bodyTypeError(headerInvocationMessage);
                return this;
            },
        },
        [Symbol.toStringTag]: {configurable: true, value: label},
    });
    Object.defineProperty(prototype[Symbol.iterator], 'name', {value: ''});
    headersIteratorPrototypes[kind] = prototype;
}

function createHeadersIterator(headers, kind) {
    const iterator = Object.create(headersIteratorPrototypes[kind]);
    headerWeakSet(iteratorStates, iterator, {entries: sortedEntries(headers), kind, index: 0});
    return iterator;
}

// Cache the supplied iterator method and let for-of cache next and perform
// IteratorClose if converting an element throws. Conversion happens while
// consuming the sequence, before Headers validates pair lengths/names/values.
function convertHeaderSequence(value, method, convert) {
    const converted = [];
    const iterable = {[Symbol.iterator]() {return applyIntrinsic(method, value, []);}};
    for (const item of iterable) converted.push(convert(item));
    return converted;
}

class Headers {
    constructor(init = undefined) {
        headerWeakSet(headerLists, this, new Map());
        headerWeakSet(setCookieLists, this, []);
        if (init === undefined) return;

        const owned = headerWeakGet(headerLists, init);
        if (owned !== undefined) {
            headerWeakSet(headerLists, this, new Map(owned));
            headerWeakSet(setCookieLists, this, headerWeakGet(setCookieLists, init).slice());
            return;
        }
        if (!isObjectValue(init)) {
            throw new bodyTypeError("Failed to construct 'Headers': constructor parameter 1 is not of type 'Headers or Sequence or object'.");
        }
        if ((init instanceof NativeHeaders || init instanceof Headers) &&
            !Object.prototype.hasOwnProperty.call(init, Symbol.iterator) &&
            typeof init.getSetCookie === 'function') {
            for (const [name, value] of init) {
                if (name !== 'set-cookie') applyIntrinsic(headerAppendMethod, this, [name, value]);
            }
            for (const value of init.getSetCookie()) applyIntrinsic(headerAppendMethod, this, ['set-cookie', value]);
            return;
        }

        const method = init[Symbol.iterator];
        const sequence = typeof method === 'function';
        let entries;
        if (sequence) {
            entries = convertHeaderSequence(init, method, pair => {
                const inner = isObjectValue(pair) ? pair[Symbol.iterator] : undefined;
                if (typeof inner !== 'function') {
                    throw new bodyTypeError("Incorrect type: the provided value is not of type 'Sequence'.");
                }
                return convertHeaderSequence(pair, inner, headerMutationString);
            });
        } else {
            entries = [];
            for (const name of Object.keys(init)) entries.push([name, headerMutationString(init[name])]);
        }
        // Complete the union/sequence/record conversion before native-style
        // validation. A subclass append override is not part of construction.
        for (const pair of entries) {
            if (pair.length !== 2) {
                throw new bodyTypeError('To initialize a Headers object from a sequence, each inner sequence must have exactly two elements.');
            }
            applyIntrinsic(headerAppendMethod, this, pair);
        }
    }

    append(name, value) {
        const input = headerMutationInput(this, 'append', arguments.length, name, value);
        const list = input.list, normalizedName = input.name, normalizedValue = input.value;
        const existing = list.get(normalizedName);

        if (normalizedName === 'set-cookie') {
            headerWeakGet(setCookieLists, this).push(normalizedValue);
        }
        list.set(
            normalizedName,
            existing === undefined ? normalizedValue : `${existing}, ${normalizedValue}`,
        );
    }

    delete(name) {
        const input = headerMutationInput(this, 'delete', arguments.length, name);
        const list = input.list, normalizedName = input.name;
        list.delete(normalizedName);
        if (normalizedName === 'set-cookie') {
            headerWeakSet(setCookieLists, this, []);
        }
    }

    get(name) {
        const input = headerReadInput(this, 'get', arguments.length, name);
        const value = headerMapGet(input.list, input.name);

        return value === undefined ? null : value;
    }

    getSetCookie() {
        requireHeaders(this);

        return headerWeakGet(setCookieLists, this).slice();
    }

    has(name) {
        const input = headerReadInput(this, 'has', arguments.length, name);
        return headerMapHas(input.list, input.name);
    }

    set(name, value) {
        const input = headerMutationInput(this, 'set', arguments.length, name, value);
        const list = input.list, normalizedName = input.name, normalizedValue = input.value;
        list.set(normalizedName, normalizedValue);
        if (normalizedName === 'set-cookie') {
            headerWeakSet(setCookieLists, this, [ normalizedValue ]);
        }
    }

    forEach(callback, thisArg = undefined) {
        requireHeaders(this);
        if (typeof callback !== 'function') {
            throw new bodyTypeError("Failed to execute 'forEach' on 'Headers': parameter 1 is not of type 'function'.");
        }
        // Pinned workerd uses a complete entry snapshot here, and invokes the
        // callback directly without reading this.entries or callback.call.
        const entries = sortedEntries(this);
        for (const [name, value] of entries) applyIntrinsic(callback, thisArg, [value, name, this]);
    }

    keys() {
        return createHeadersIterator(this, 'key');
    }

    values() {
        return createHeadersIterator(this, 'value');
    }

    entries() {
        return createHeadersIterator(this, 'entry');
    }

    [Symbol.iterator]() {
        return this.entries();
    }

    get [Symbol.toStringTag]() {
        return 'Headers';
    }
}

const headerAppendMethod = Headers.prototype.append;

function wrapNormalizedNativeHeaders(nativeHeaders) {
    const headers = Object.create(Headers.prototype);
    const list = new Map();
    const cookies = [];

    headerWeakSet(headerLists, headers, list);
    headerWeakSet(setCookieLists, headers, cookies);
    for (const [ name, value ] of nativeHeaders) {
        if (name !== 'set-cookie') {
            list.set(name, value);
        }
    }
    for (const value of nativeHeaders.getSetCookie()) {
        const existing = list.get('set-cookie');
        cookies.push(value);
        list.set(
            'set-cookie',
            existing === undefined ? value : `${existing}, ${value}`,
        );
    }
    return headers;
}

function normalizeBody(body) {
    if (body === undefined || body === null ||
        typeof body === 'string' ||
        body instanceof ReadableStream ||
        body instanceof Blob ||
        body instanceof FormData ||
        body instanceof URLSearchParams ||
        body instanceof ArrayBuffer ||
        ArrayBuffer.isView(body)) {
        return body;
    }

    return String(body);
}

function isEmptyFixedResponseBody(body) {
    if (typeof body === 'string') return body.length === 0;
    if (body instanceof ArrayBuffer) return emptyBodyIntrinsics.bufferLength.call(body) === 0;
    if (emptyBodyIntrinsics.isView(body)) {
        try { return emptyBodyIntrinsics.typedLength.call(body) === 0; }
        catch (error) {
            if (!(error instanceof bodyTypeError)) throw error;
            // A DataView's JS byteLength getter throws after detachment, while
            // the reference's native BufferSource conversion sees zero bytes.
            emptyBodyIntrinsics.viewBuffer.call(body); // native brand check
            try { return emptyBodyIntrinsics.viewLength.call(body) === 0; }
            catch (viewError) { if (viewError instanceof bodyTypeError) return true; throw viewError; }
        }
    }
    if (body instanceof Blob) return emptyBodyIntrinsics.blobSize.call(body) === 0;
    if (body instanceof URLSearchParams) return emptyBodyIntrinsics.paramsString.call(body) === '';
    return false;
}

// The txiki Fetch polyfill eagerly wraps every string body in a
// ReadableStream. Keep an internal text snapshot for an untouched string
// response so the Runtime can encode and send it with one native call.
// Accessing or replacing `response.body` disables the shortcut:
// application-visible stream consumption must retain the normal Fetch
// semantics.
// Bridge-only header extraction: one pass over the internal list, no
// sort, no iterator objects, no forEach callback, no getSetCookie slice.
// Order is list-insertion order with set-cookie emitted last — the same
// observable sequence the bootstrap's previous forEach+getSetCookie
// construction produced (frozen by test_host_single_worker.mjs).
export function bridgeHeaderPairs(headers) {
    const list = headerWeakGet(headerLists, headers);
    if (list === undefined) {
        // Foreign (native) Headers instance: fall back to the public
        // iterator construction.
        const pairs = [];
        headers.forEach((value, name) => {
            if (name !== 'set-cookie') {
                pairs.push([ name, value ]);
            }
        });
        for (const value of headers.getSetCookie()) {
            pairs.push([ 'set-cookie', value ]);
        }
        return pairs;
    }
    const pairs = [];
    for (const name of list.keys()) {
        if (name !== 'set-cookie') {
            pairs.push([ name, list.get(name) ]);
        }
    }
    for (const value of headerWeakGet(setCookieLists, headers)) {
        pairs.push([ 'set-cookie', value ]);
    }
    return pairs;
}

export function getFastResponseBody(response) {
    const record = headerWeakGet(fastResponseBodies, response);

    if (!record || record.exposed || response.bodyUsed ||
        response._bodyStream !== record.stream) {
        return null;
    }
    const descriptor = Object.getOwnPropertyDescriptor(response, 'body');
    if (!descriptor || descriptor.get !== record.getter) {
        return null;
    }
    return record.text;
}

function materializeFastResponseBody(response, exposed) {
    const record = headerWeakGet(fastResponseBodies, response);

    if (!record) {
        return null;
    }
    if (exposed) {
        record.exposed = true;
    }
    if (record.stream === null) {
        if (record.bytes === null) {
            record.bytes = new TextEncoder().encode(record.text);
        }
        record.stream = new ReadableStream({
            start(controller) {
                controller.enqueue(record.bytes);
                controller.close();
            },
        });
        response._noBody = false;
        response._bodySize = record.bytes.byteLength;
        response._bodyStream = record.stream;
    }
    return record;
}

function consumeFastResponseBody(response, method, args) {
    const record = materializeFastResponseBody(response, true);

    return record.consumers[method].apply(response, args);
}

function toNativeHeaderEntries(init) {
    const headers = init instanceof Headers ? init : new Headers(init);
    const entries = sortedEntries(headers)
        .filter(([ name ]) => name !== 'set-cookie');

    for (const value of headers.getSetCookie()) {
        entries.push([ 'set-cookie', value ]);
    }
    return entries;
}

const redirectValueError = 'Invalid redirect value, must be one of "follow" or "manual" ("error" won\'t be implemented since it does not make sense at the edge; use "manual" and check the response status code).';
function normalizeRedirect(value) {
    if (value === undefined) return undefined;
    if (typeof value === 'symbol')
        throw new bodyTypeError('Cannot convert a Symbol value to a string');
    const redirect = `${value}`;
    if (redirect !== 'follow' && redirect !== 'manual')
        throw new bodyTypeError(redirectValueError);
    return redirect;
}

function normalizeInit(init, requestInit = false) {
    if (init === undefined || init === null) {
        return {};
    }

    // Response's dictionary has no redirect/body member. Do not invoke
    // unrelated getters or conversions while preparing a response.
    const normalized = requestInit ? { ...init } : {
        headers: init.headers, status: init.status, statusText: init.statusText, webSocket: init.webSocket,
    };
    if (normalized.headers !== undefined) {
        normalized.headers = new Headers(normalized.headers);
    }
    if (Object.prototype.hasOwnProperty.call(normalized, 'body')) {
        normalized.body = normalizeBody(normalized.body);
    }
    if (requestInit && normalized.redirect !== undefined) {
        normalized.redirect = normalizeRedirect(normalized.redirect);
    }
    return normalized;
}

function normalizeIncomingInit(init) {
    // The incoming brand means bootstrap owns this init exclusively (it is
    // not caller-visible), so the fields are normalized in place: no spread
    // copy, no intermediate entries array, no per-pair arrays. The public
    // normalizeInit path keeps the spec-required non-mutating copy.
    const source = init.headers;
    if (source !== undefined) {
        // bootstrap owns this array-of-pairs shape. Normalize each field once
        // without constructing an intermediate Headers object whose iterator
        // repeatedly sorts/materializes the whole list; NativeRequest still
        // receives ordinary canonical header entries below.
        for (let i = 0; i < source.length; i++) {
            const pair = source[i];
            pair[0] = normalizeName(pair[0]);
            pair[1] = normalizeValue(pair[1]);
        }
    }
    if (Object.prototype.hasOwnProperty.call(init, 'body')) {
        init.body = normalizeBody(init.body);
    }
    return init;
}

function isNullBodyStatus(status) {
    return status === 101 || status === 204 || status === 205 || status === 304;
}

function bodyLimitError(kind, limit) {
    return new TypeError(
        `fetch ${kind} body exceeded configured limit of ${limit} bytes`);
}

function bodyChunkSize(chunk) {
    if (typeof chunk === 'string') {
        return new TextEncoder().encode(chunk).byteLength;
    }
    if (chunk instanceof ArrayBuffer) {
        return chunk.byteLength;
    }
    if (ArrayBuffer.isView(chunk)) {
        return chunk.byteLength;
    }
    throw new TypeError('Fetch body stream chunk must be bytes');
}

function knownBodySize(body) {
    if (body === undefined || body === null) {
        return 0;
    }
    if (typeof body === 'string') {
        return new TextEncoder().encode(body).byteLength;
    }
    if (body instanceof Blob) {
        return body.size;
    }
    if (body instanceof URLSearchParams) {
        return new TextEncoder().encode(String(body)).byteLength;
    }
    if (body instanceof ArrayBuffer) {
        return body.byteLength;
    }
    if (ArrayBuffer.isView(body)) {
        return body.byteLength;
    }
    return null;
}

function rejectedBodyStream(stream, error) {
    const channel = isFetchChannelStream(stream);
    const rejected = new ReadableStream({
        ...(channel ? {type: 'bytes'} : {}),
        start(controller) {
            Promise.resolve(stream.cancel(error)).catch(() => {});
            controller.error(error);
        },
    });
    // A configured limit errors the native body without changing its reader
    // interface or losing the request context carried by the original stream.
    return channel ? inheritFetchChannelStream(stream, rejected) : rejected;
}

function limitedBodyStream(stream, limit, kind, state = undefined) {
    const reader = stream.getReader();
    let total = 0;
    let finished = false;

    const limited = new ReadableStream({
        ...(kind === 'response' && isFetchChannelStream(stream) ? {type: 'bytes'} : {}),
        async pull(controller) {
            const result = await reader.read();
            if (result.done) {
                finished = true;
                reader.releaseLock();
                controller.close();
                controller.byobRequest?.respond(0);
                return;
            }

            total += bodyChunkSize(result.value);
            if (total > limit) {
                const error = bodyLimitError(kind, limit);
                if (state !== undefined) {
                    state.error = error;
                }
                finished = true;
                try {
                    await reader.cancel(error);
                } catch {
                    // The configured limit remains the observable failure.
                }
                reader.releaseLock();
                controller.error(error);
                return;
            }
            controller.enqueue(result.value);
        },
        async cancel(reason) {
            if (finished) {
                return;
            }
            finished = true;
            try {
                await reader.cancel(reason);
            } finally {
                reader.releaseLock();
            }
        },
    });
    return kind === 'response' ? inheritFetchChannelStream(stream, limited) : limited;
}

function limitedNativeRequest(input, init, limit) {
    if (init !== undefined && init !== null &&
        Object.prototype.hasOwnProperty.call(init, 'body')) {
        const size = knownBodySize(init.body);
        if (size !== null && size > limit) {
            throw bodyLimitError('request', limit);
        }
        if (size !== null) {
            return { input, init, state: null };
        }
    }

    const request = new NativeRequest(input, init);
    if (request.body === null) {
        return { input, init, state: null };
    }
    const state = { error: null };
    return {
        input: new NativeRequest(request, {
            body: limitedBodyStream(
                request.body,
                limit,
                'request',
                state),
            duplex: 'half',
        }),
        init: undefined,
        state,
    };
}

function limitedResponseBody(response, limit) {
    if (response.body === null) {
        return null;
    }
    const contentLength = response.headers.get('content-length');
    const ordinary = isOrdinaryFetchBody(response.body);
    const encodedGzip = (ordinary || isFetchChannelStream(response.body)) &&
        headerLowerAscii(response.headers.get('content-encoding') ?? '') === 'gzip';
    if (!encodedGzip && contentLength !== null &&
        /^[0-9]+$/.test(contentLength) &&
        Number(contentLength) > limit) {
        return rejectedBodyStream(
            response.body,
            bodyLimitError('response', limit));
    }
    // Ordinary native bodies count decoded producer bytes directly, retaining
    // their native reader and demand geometry under an enabled limit.
    if (ordinary) return response.body;
    return limitedBodyStream(response.body, limit, 'response');
}

class Request extends NativeRequest {
    constructor(input, init = undefined, brand = undefined) {
        const normalizedInput = normalizeRequestInfo(input);
        const incoming = brand === incomingRequestBrand;
        const normalized = incoming
            ? normalizeIncomingInit(init)
            : normalizeInit(init, true);

        if (input instanceof Request) {
            const inherited = {
                method: input.method,
                headers: new Headers(input.headers),
                credentials: input.credentials,
                redirect: input.redirect,
                mode: input.mode,
                signal: input.signal,
                ...normalized,
            };
            if (!Object.prototype.hasOwnProperty.call(normalized, 'body') &&
                input.body !== null) {
                inherited.body = input.body;
                inherited.duplex = 'half';
            }
            super(input.url, inherited);
        } else {
            super(normalizedInput, normalized);
        }

        this.headers = incoming
            ? wrapNormalizedNativeHeaders(this.headers)
            : new Headers(this.headers);
        /*
         * BodyMixin is mixed in as own properties by the vendor constructor.
         * Capture its formData before replacing it with our override that does
         * byte-level multipart parsing.
         */
        this._nativeFormData = this.formData;
        this.formData = multipartFormDataOverride;
        trackBodyUsage(this);
    }

    clone() {
        const nativeClone = super.clone();
        const cloned = new Request(nativeClone, {});
        cloned.cache = nativeClone.cache;
        cloned.destination = nativeClone.destination;
        cloned.integrity = nativeClone.integrity;
        cloned.isHistoryNavigation = nativeClone.isHistoryNavigation;
        cloned.isReloadNavigation = nativeClone.isReloadNavigation;
        cloned.keepalive = nativeClone.keepalive;
        cloned.referrerPolicy = nativeClone.referrerPolicy;
        return cloned;
    }
}

export function createIncomingRequest(input, init) {
    return new Request(input, init, incomingRequestBrand);
}

/*
 * Shared byte-level multipart/form-data parser used by both Request and
 * Response.  Must consume the body via arrayBuffer() (which sets bodyUsed)
 * rather than reading the body stream directly.
 *
 * Boundary matching rules (RFC 2046 § 5.1.1):
 *   - The boundary delimiter is "--" + boundary, optionally preceded by CRLF
 *     (or at the start of the body for the first part).
 *   - A bare "--boundary" inside file payload is NOT a delimiter.
 *   - Media type and boundary parameter names are case-insensitive.
 */
const crlfBytes = new Uint8Array([ 0x0d, 0x0a ]);
const doubleCrlfBytes = new Uint8Array([ 0x0d, 0x0a, 0x0d, 0x0a ]);

function extractBoundary(contentType) {
    const match = /boundary\s*=\s*(?:"([^"]+)"|'([^']+)'|([^;]+))/i.exec(contentType);
    if (!match) {
        throw new TypeError('multipart/form-data missing boundary');
    }
    return match[1] || match[2] || match[3];
}

function buildDelimiter(boundary) {
    return new TextEncoder().encode('--' + boundary);
}

function isCrlfBefore(bytes, index) {
    return index >= crlfBytes.byteLength &&
        bytes[index - 2] === crlfBytes[0] &&
        bytes[index - 1] === crlfBytes[1];
}

async function parseMultipartFormData(request) {
    const contentType = request.headers.get('content-type');
    const boundary = extractBoundary(contentType);
    const delimiter = buildDelimiter(boundary);

    // Consume the body via arrayBuffer() so that bodyUsed is set correctly.
    const bodyBuffer = await request.arrayBuffer();
    const bytes = new Uint8Array(bodyBuffer);

    const form = new FormData();
    let offset = 0;

    while (offset < bytes.byteLength) {
        const delimiterIndex = indexOfBytes(bytes, delimiter, offset);
        if (delimiterIndex < 0) {
            break;
        }
        // A delimiter is only valid if it appears at the start of the body or
        // is immediately preceded by CRLF.
        if (delimiterIndex !== 0 && !isCrlfBefore(bytes, delimiterIndex)) {
            offset = delimiterIndex + delimiter.byteLength;
            continue;
        }
        offset = delimiterIndex + delimiter.byteLength;

        // End boundary: delimiter followed by "--"
        if (offset + 1 < bytes.byteLength &&
            bytes[offset] === 0x2d && bytes[offset + 1] === 0x2d) {
            break;
        }

        // Skip CRLF after delimiter
        if (offset + crlfBytes.byteLength <= bytes.byteLength &&
            bytes[offset] === crlfBytes[0] && bytes[offset + 1] === crlfBytes[1]) {
            offset += crlfBytes.byteLength;
        }

        // Find part header/body separator
        const headerEndIndex = indexOfBytes(bytes, doubleCrlfBytes, offset);
        if (headerEndIndex < 0) {
            break;
        }

        // Decode part headers as text
        const headerBytes = bytes.subarray(offset, headerEndIndex);
        const headerText = new TextDecoder().decode(headerBytes);
        const partHeaders = parsePartHeaders(headerText);

        offset = headerEndIndex + doubleCrlfBytes.byteLength;

        // Find NEXT valid delimiter to locate end of this part's body
        let nextDelimiterIndex = -1;
        let bodyEnd = bytes.byteLength;
        let searchOffset = offset;
        while (searchOffset < bytes.byteLength) {
            const candidateIndex = indexOfBytes(bytes, delimiter, searchOffset);
            if (candidateIndex < 0) {
                break;
            }
            // Must be at start of body or preceded by CRLF
            if (candidateIndex === 0 || isCrlfBefore(bytes, candidateIndex)) {
                nextDelimiterIndex = candidateIndex;
                break;
            }
            searchOffset = candidateIndex + delimiter.byteLength;
        }

        if (nextDelimiterIndex >= 0) {
            bodyEnd = nextDelimiterIndex;
            // Strip trailing CRLF before the delimiter
            if (isCrlfBefore(bytes, bodyEnd)) {
                bodyEnd -= crlfBytes.byteLength;
            }
        }

        const partBody = bytes.subarray(offset, bodyEnd);

        const disposition = partHeaders['content-disposition'] || '';
        const nameMatch = /name="([^"]+)"/.exec(disposition);
        const filenameMatch = /filename="([^"]+)"/.exec(disposition);
        const fieldName = nameMatch ? nameMatch[1] : '';

        if (filenameMatch) {
            const filename = filenameMatch[1];
            const partContentType =
                partHeaders['content-type'] || 'application/octet-stream';
            // Preserve raw bytes — do not go through .text().
            const file = new File([partBody.slice()], filename, {
                type: partContentType,
            });
            form.append(fieldName, file);
        } else {
            const value = new TextDecoder().decode(partBody);
            form.append(fieldName, value);
        }

        offset = nextDelimiterIndex >= 0 ?
            nextDelimiterIndex :
            bytes.byteLength;
    }

    return form;
}

function indexOfBytes(haystack, needle, start) {
    const limit = haystack.byteLength - needle.byteLength;
    for (let i = start; i <= limit; i++) {
        let match = true;
        for (let j = 0; j < needle.byteLength; j++) {
            if (haystack[i + j] !== needle[j]) {
                match = false;
                break;
            }
        }
        if (match) {
            return i;
        }
    }
    return -1;
}

function parsePartHeaders(text) {
    const headers = {};
    const lines = text.split('\r\n');
    let currentName = '';
    for (const line of lines) {
        if (line.startsWith(' ') || line.startsWith('\t')) {
            if (currentName) {
                headers[currentName] += ' ' + line.trim();
            }
        } else {
            const colonIndex = line.indexOf(':');
            if (colonIndex > 0) {
                currentName = line.substring(0, colonIndex).trim().toLowerCase();
                headers[currentName] = line.substring(colonIndex + 1).trim();
            }
        }
    }
    return headers;
}

function multipartFormDataOverride() {
    materializeFastResponseBody(this, true);
    const contentType = this.headers.get('content-type');
    if (!contentType ||
        !/^multipart\/form-data/i.test(contentType.trim())) {
        return this._nativeFormData.call(this);
    }
    return parseMultipartFormData(this);
}

class Response extends NativeResponse {
    constructor(body = null, init = undefined) {
        const normalized = normalizeInit(init);
        const status = normalized.status === undefined ? 200 : Number(normalized.status);
        let normalizedBody = normalizeBody(body);
        const socket = normalized.webSocket;
        if (socket !== undefined && socket !== null && !isWebSocketEndpoint(socket))
            throw new TypeError("Incorrect type for the 'webSocket' field on 'InitializerDict': the provided value is not of type 'WebSocket'.");
        if (status === 101) {
            // 101 without an endpoint is the fetch-spec forbidden-status
            // violation (RangeError in the reference, C13); with an
            // endpoint CONSTRUCTION succeeds regardless of accept/role
            // state — those are validated at send time (C14) — but the
            // VALUE must still BE a WebSocket endpoint (fix-review §4:
            // {} / true / 1 are TypeErrors, not silent 101s).
            if (socket === undefined || socket === null)
                throw new RangeError('Responses may only be constructed with status codes in the range 200 to 599, inclusive.');
            // Fixed zero-byte bodies are absent in the locked reference,
            // including detached BufferSources. An empty stream is still a
            // body. Read native lengths, never caller-overridden properties.
            if (isEmptyFixedResponseBody(normalizedBody)) normalizedBody = null;
        } else if (socket !== undefined && socket !== null) {
            throw new RangeError('Responses with a WebSocket must have status code 101.');
        }

        if (normalizedBody !== null && normalizedBody !== undefined && isNullBodyStatus(status)) {
            throw new TypeError(status === 101
                ? 'Response with null body status (101, 204, 205, or 304) cannot have a body.'
                : 'Response with a null-body status cannot have a body');
        }

        const fastText = typeof normalizedBody === 'string' && normalizedBody.length > 0
            ? normalizedBody
            : null;
        super(fastText === null ? normalizedBody : null,
            status === 101 ? { ...normalized, status: 200, statusText: '' } : normalized);
        if (status === 101) {
            this.status = 101;
            this.statusText = normalized.statusText === undefined ? 'Switching Protocols' : String(normalized.statusText);
            this.ok = false;
            setResponseWebSocket(this, socket);
        }
        this.headers = new Headers(this.headers);
        this.redirected = false;
        this._nativeFormData = this.formData;
        this.formData = multipartFormDataOverride;
        trackBodyUsage(this);

        if (fastText !== null) {
            this._noBody = false;
            this._bodyInit = normalizedBody;
            // The untouched response goes straight to the native final
            // bridge, so defer UTF-8 sizing/encoding until application code
            // actually asks for the standards-visible body stream.
            this._bodySize = -1;
            this._bodyStream = null;
            if (!this.headers.has('content-type')) {
                this.headers.set('content-type', 'text/plain;charset=UTF-8');
            }

            const record = {
                text: fastText,
                bytes: null,
                stream: null,
                exposed: false,
                getter: null,
                consumers: {
                    arrayBuffer: this.arrayBuffer,
                    blob: this.blob,
                    json: this.json,
                    text: this.text,
                },
            };
            headerWeakSet(fastResponseBodies, this, record);
            record.getter = () => {
                return materializeFastResponseBody(this, true).stream;
            };
            Object.defineProperty(this, 'body', {
                configurable: true,
                enumerable: true,
                get: record.getter,
                set(value) {
                    record.exposed = true;
                    record.stream = value;
                    this._bodyStream = value;
                },
            });
            delete this.arrayBuffer;
            delete this.blob;
            delete this.json;
            delete this.text;
        }
    }

    arrayBuffer(...args) {
        return consumeFastResponseBody(this, 'arrayBuffer', args);
    }

    blob(...args) {
        return consumeFastResponseBody(this, 'blob', args);
    }

    json(...args) {
        return consumeFastResponseBody(this, 'json', args);
    }

    text(...args) {
        return consumeFastResponseBody(this, 'text', args);
    }

    clone() {
        if (getResponseWebSocket(this)) throw new TypeError('Cannot clone a response to a WebSocket handshake.');
        /*
         * Response.error() has status 0, which is outside [200, 599].
         * The native clone() calls the Response constructor with status 0
         * and throws RangeError. Build the error-response clone by hand.
         */
        if (this.type === 'error') {
            const wrapped = Object.create(Response.prototype);
            wrapped.ok = this.ok;
            wrapped.status = this.status;
            wrapped.statusText = this.statusText;
            wrapped.type = this.type;
            wrapped.url = this.url;
            wrapped.redirected = this.redirected;
            wrapped.headers = new Headers(this.headers);
            wrapped.body = null;
            wrapped.bodyUsed = false;
            wrapped._nativeFormData = this._nativeFormData;
            wrapped.formData = multipartFormDataOverride;
            return wrapped;
        }
        const stream = this.body;
        if (stream?.locked) throw new bodyTypeError('This ReadableStream is currently locked to a reader,');
        const nativeClone = super.clone();
        const cloned = new Response(
            nativeClone.body,
            {
                status: nativeClone.status,
                statusText: nativeClone.statusText,
                headers: nativeClone.headers,
            },
        );
        cloned.ok = nativeClone.ok;
        cloned.redirected = this.redirected;
        cloned.type = nativeClone.type;
        cloned.url = this.url;
        if (weakHas(immutableHeaders, this.headers)) {
            finishFetchedResponse(cloned, this.url, this.redirected);
            if (this.body !== null) weakAdd(ordinaryFetchStreams, this.body);
        }
        return cloned;
    }

    static error() {
        const native = NativeResponse.error();
        const response = new Response(null);

        response.ok = native.ok;
        response.status = native.status;
        response.type = native.type;
        return response;
    }

    get webSocket() { return getResponseWebSocket(this); }

    static json(data, init = undefined) {
        const body = JSON.stringify(data);

        if (body === undefined) {
            throw new TypeError('The data is not JSON serializable');
        }

        const normalized = normalizeInit(init);
        const headers = new Headers(normalized.headers);
        if (!headers.has('content-type')) {
            headers.set('content-type', 'application/json');
        }
        return new Response(body, { ...normalized, headers });
    }

    static redirect(url, status = 302) {
        const redirectStatuses = [ 301, 302, 303, 307, 308 ];
        const normalizedStatus = Number(status);

        if (!redirectStatuses.includes(normalizedStatus)) {
            throw new RangeError('Invalid status code');
        }
        return new Response(null, {
            status: normalizedStatus,
            headers: { location: String(url) },
        });
    }
}

async function fetchUpgradeRequest(request) {
    let redirected = false;
    const visited = [];
    for (let count = 0;; count++) {
        const target = new URL(request.url); target.hash = '';
        visited.push(target.href);
        const result = await openFetchUpgrade(target.href, toNativeHeaderEntries(request.headers),
            request.redirect === 'manual' ? 1 : request.redirect === 'error' ? 2 : 0, request.signal);
        const metadata = result.metadata;
        const responseHeaders = new Headers(metadata.headers);
        const location = responseHeaders.get('location');
        if ([301, 302, 303, 307, 308].includes(metadata.status) && location !== null && request.redirect !== 'manual') {
            if (count >= 20) {
                await result.body?.cancel();
                throw new bodyTypeError('Too many redirects.' + visited.join(', '));
            }
            // Drain a redirect without accumulating its body. Each subsequent
            // transaction re-enters the same native policy/owner checks and
            // generates a fresh WebSocket nonce for its actual destination.
            if (result.body) {
                const reader = result.body.getReader();
                try {while (!(await reader.read()).done) {}}
                finally {reader.releaseLock();}
            }
            const next = new URL(location, target);
            const headers = new Headers(request.headers);
            if (next.origin !== target.origin) headers.delete('authorization');
            request = new NativeRequest(next.href, {method: 'GET', headers: toNativeHeaderEntries(headers), redirect: request.redirect, signal: request.signal});
            redirected = true; continue;
        }
        const body = result.body && maxFetchResponseBodyBytes !== 0
            ? limitedResponseBody({body: result.body, headers: responseHeaders}, maxFetchResponseBodyBytes) : result.body;
        const response = new Response(body, {status: metadata.status, statusText: metadata.statusText,
            headers: metadata.headers, url: metadata.url, ...(result.webSocket ? {webSocket: result.webSocket} : {})});
        response.url = metadata.url;
        response.redirected = redirected || metadata.redirected;
        return response;
    }
}

function fetch(input, init = undefined) {
    let nativeInput;
    try { nativeInput = normalizeRequestInfo(input); }
    catch (error) { return Promise.reject(error); }
    let nativeInit = init;
    let requestLimitState = null;

    try {
        if (input instanceof Request) {
            nativeInput = input.url;
            nativeInit = {
                method: input.method,
                headers: new Headers(input.headers),
                credentials: input.credentials,
                redirect: input.redirect,
                mode: input.mode,
                signal: input.signal,
            };
            if (input.body !== null) {
                nativeInit.body = input.body;
                nativeInit.duplex = 'half';
            }
            if (init !== undefined && init !== null) {
                Object.assign(nativeInit, normalizeInit(init, true));
            }
        } else if (init !== undefined) {
            nativeInit = normalizeInit(init, true);
        }
    } catch (error) {
        return Promise.reject(error);
    }

    // Prepared Headers retain their raw owned keys for copy operations.
    // Read the private list, without application getters or public iteration,
    // when selecting the existing Upgrade transport.
    const fields = nativeInit?.headers;
    const ownedFields = headerWeakGet(headerLists, fields);
    const upgradeValue = ownedFields === undefined ? undefined : headerMapGet(ownedFields, 'upgrade');
    let upgrade = upgradeValue !== undefined && headerLowerAscii(upgradeValue) === 'websocket';
    if (Array.isArray(fields)) {
        for (let index = 0; index < fields.length; index++)
            if (fields[index][0] === 'upgrade' && headerLowerAscii(fields[index][1]) === 'websocket') upgrade = true;
    }
    if (upgrade) {
        try {return fetchUpgradeRequest(new NativeRequest(nativeInput, nativeInit));}
        catch (error) {return Promise.reject(error);}
    }

    if (maxFetchRequestBodyBytes !== 0) {
        try {
            const limited = limitedNativeRequest(
                nativeInput,
                nativeInit,
                maxFetchRequestBodyBytes);
            nativeInput = limited.input;
            nativeInit = limited.init;
            requestLimitState = limited.state;
        } catch (error) {
            return Promise.reject(error);
        }
    }

    return nativeFetch(nativeInput, nativeInit).catch(error => {
        if (requestLimitState?.error) {
            throw requestLimitState.error;
        }
        throw error;
    }).then(response => {
        if (response instanceof Response) {
            return finishFetchedResponse(response, response.url, response.redirected);
        }
        const nullBody = isNullBodyStatus(response.status);
        if (nullBody && response.body !== null) {
            response.body.cancel().catch(() => {});
        }
        const body = nullBody
            ? null
            : maxFetchResponseBodyBytes === 0
                ? response.body
                : limitedResponseBody(
                    response,
                    maxFetchResponseBodyBytes);
        const wrapped = new Response(nullBody ? null : body, {
            status: response.status,
            statusText: response.statusText,
            headers: wrapNormalizedNativeHeaders(response.headers),
        });
        return finishFetchedResponse(wrapped, response.url, response.redirected);
    });
}

// Preserve owned raw keys through vendor Request/Response copies without
// invoking an application replacement of Headers[Symbol.iterator].
configureOwnedHeadersCloner(init => headerWeakGet(headerLists, init) === undefined ? undefined : new Headers(init));

Object.defineProperties(globalThis, {
    Headers: { configurable: true, writable: true, value: Headers },
    Request: { configurable: true, writable: true, value: Request },
    Response: { configurable: true, writable: true, value: Response },
    fetch: { configurable: true, writable: true, value: fetch },
});
