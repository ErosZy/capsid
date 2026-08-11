const NativeHeaders = globalThis.Headers;
const NativeRequest = globalThis.Request;
const NativeResponse = globalThis.Response;
const nativeFetch = globalThis.fetch;

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
}

const headerLists = new WeakMap();
const setCookieLists = new WeakMap();
const iteratorStates = new WeakMap();
const fastResponseBodies = new WeakMap();
const incomingRequestBrand = Symbol('capsid incoming request');
const arrayIteratorPrototype =
    Object.getPrototypeOf(Object.getPrototypeOf([][Symbol.iterator]()));

function toByteString(value) {
    const string = String(value);

    for (let i = 0; i < string.length; i++) {
        if (string.charCodeAt(i) > 0xff) {
            throw new TypeError('Value is not a valid ByteString');
        }
    }

    return string;
}

function normalizeName(value) {
    const name = toByteString(value);

    if (name === '' || /[^!#$%&'*+.^_`|~0-9A-Za-z-]/.test(name)) {
        throw new TypeError(`Invalid character in header field name: "${name}"`);
    }

    return name.toLowerCase();
}

function normalizeValue(value) {
    const string = toByteString(value);

    if (/[\0\r\n]/.test(string)) {
        throw new TypeError('Invalid character in header field value');
    }

    return string.replace(/^[\t ]+|[\t ]+$/g, '');
}

function requireHeaders(value) {
    const list = headerLists.get(value);

    if (!list) {
        throw new TypeError('Illegal invocation');
    }

    return list;
}

function sortedEntries(headers) {
    const entries = [];
    const list = requireHeaders(headers);

    for (const name of Array.from(list.keys()).sort()) {
        if (name === 'set-cookie') {
            for (const value of setCookieLists.get(headers)) {
                entries.push([ name, value ]);
            }
        } else {
            entries.push([ name, list.get(name) ]);
        }
    }
    return entries;
}

const headersIteratorPrototype = Object.create(arrayIteratorPrototype);
Object.defineProperties(headersIteratorPrototype, {
    next: {
        configurable: true,
        enumerable: true,
        writable: true,
        value() {
            const state = iteratorStates.get(this);

            if (!state) {
                throw new TypeError('Illegal invocation');
            }

            const entries = sortedEntries(state.headers);
            if (state.index >= entries.length) {
                return { value: undefined, done: true };
            }

            const [ name, value ] = entries[state.index++];

            if (state.kind === 'key') {
                return { value: name, done: false };
            }
            if (state.kind === 'value') {
                return { value, done: false };
            }
            return { value: [ name, value ], done: false };
        },
    },
    [Symbol.iterator]: {
        configurable: true,
        writable: true,
        value() {
            return this;
        },
    },
    [Symbol.toStringTag]: {
        configurable: true,
        value: 'Headers Iterator',
    },
});

function createHeadersIterator(headers, kind) {
    const iterator = Object.create(headersIteratorPrototype);

    iteratorStates.set(iterator, { headers, kind, index: 0 });
    return iterator;
}

class Headers {
    constructor(init = undefined) {
        headerLists.set(this, new Map());
        setCookieLists.set(this, []);

        if (init === undefined) {
            return;
        }
        if ((typeof init !== 'object' && typeof init !== 'function') ||
            init === null) {
            throw new TypeError('Headers init must be a sequence or record');
        }

        if ((init instanceof NativeHeaders || init instanceof Headers) &&
            !Object.prototype.hasOwnProperty.call(init, Symbol.iterator) &&
            typeof init.getSetCookie === 'function') {
            for (const [ name, value ] of init) {
                if (name !== 'set-cookie') {
                    this.append(name, value);
                }
            }
            for (const value of init.getSetCookie()) {
                this.append('set-cookie', value);
            }
            return;
        }

        const iterator = init[Symbol.iterator];
        if (iterator !== undefined) {
            if (typeof iterator !== 'function') {
                throw new TypeError('Headers init iterator is not callable');
            }
            for (const pair of init) {
                if ((typeof pair !== 'object' && typeof pair !== 'function') ||
                    pair === null) {
                    throw new TypeError('Header entry is not an object');
                }
                const values = Array.from(pair);
                if (values.length !== 2) {
                    throw new TypeError(
                        `Expected name/value pair to be length 2, found ${values.length}`);
                }
                this.append(values[0], values[1]);
            }
            return;
        }

        for (const name of Object.keys(init)) {
            this.append(name, init[name]);
        }
    }

    append(name, value) {
        const list = requireHeaders(this);
        const normalizedName = normalizeName(name);
        const normalizedValue = normalizeValue(value);
        const existing = list.get(normalizedName);

        if (normalizedName === 'set-cookie') {
            setCookieLists.get(this).push(normalizedValue);
        }
        list.set(
            normalizedName,
            existing === undefined ? normalizedValue : `${existing}, ${normalizedValue}`,
        );
    }

    delete(name) {
        const normalizedName = normalizeName(name);

        requireHeaders(this).delete(normalizedName);
        if (normalizedName === 'set-cookie') {
            setCookieLists.set(this, []);
        }
    }

    get(name) {
        const value = requireHeaders(this).get(normalizeName(name));

        return value === undefined ? null : value;
    }

    getSetCookie() {
        requireHeaders(this);

        return setCookieLists.get(this).slice();
    }

    has(name) {
        return requireHeaders(this).has(normalizeName(name));
    }

    set(name, value) {
        const normalizedName = normalizeName(name);
        const normalizedValue = normalizeValue(value);

        requireHeaders(this).set(normalizedName, normalizedValue);
        if (normalizedName === 'set-cookie') {
            setCookieLists.set(this, [ normalizedValue ]);
        }
    }

    forEach(callback, thisArg = undefined) {
        if (typeof callback !== 'function') {
            throw new TypeError('Headers forEach callback must be callable');
        }

        const iterator = this.entries();
        for (let result = iterator.next(); !result.done; result = iterator.next()) {
            callback.call(thisArg, result.value[1], result.value[0], this);
        }
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

function wrapNormalizedNativeHeaders(nativeHeaders) {
    const headers = Object.create(Headers.prototype);
    const list = new Map();
    const cookies = [];

    headerLists.set(headers, list);
    setCookieLists.set(headers, cookies);
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

// The txiki Fetch polyfill eagerly wraps every string body in a
// ReadableStream. Keep an internal text snapshot for an untouched string
// response so the Runtime can encode and send it with one native call.
// Accessing or replacing `response.body` disables the shortcut:
// application-visible stream consumption must retain the normal Fetch
// semantics.
export function getFastResponseBody(response) {
    const record = fastResponseBodies.get(response);

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
    const record = fastResponseBodies.get(response);

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
    const entries = Array.from(headers)
        .filter(([ name ]) => name !== 'set-cookie');

    for (const value of headers.getSetCookie()) {
        entries.push([ 'set-cookie', value ]);
    }
    return entries;
}

function normalizeInit(init) {
    if (init === undefined || init === null) {
        return {};
    }

    const normalized = { ...init };
    if (normalized.headers !== undefined) {
        normalized.headers = toNativeHeaderEntries(normalized.headers);
    }
    if (Object.prototype.hasOwnProperty.call(normalized, 'body')) {
        normalized.body = normalizeBody(normalized.body);
    }
    return normalized;
}

function normalizeIncomingInit(init) {
    const normalized = { ...init };
    const source = normalized.headers;

    if (source !== undefined) {
        // bootstrap owns this array-of-pairs shape. Normalize each field once
        // without constructing an intermediate Headers object whose iterator
        // repeatedly sorts/materializes the whole list; NativeRequest still
        // receives ordinary canonical header entries below.
        const entries = new Array(source.length);
        for (let i = 0; i < source.length; i++) {
            const pair = source[i];
            entries[i] = [
                normalizeName(pair[0]),
                normalizeValue(pair[1]),
            ];
        }
        normalized.headers = entries;
    }
    if (Object.prototype.hasOwnProperty.call(normalized, 'body')) {
        normalized.body = normalizeBody(normalized.body);
    }
    return normalized;
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
    return new ReadableStream({
        start(controller) {
            Promise.resolve(stream.cancel(error)).catch(() => {});
            controller.error(error);
        },
    });
}

function limitedBodyStream(stream, limit, kind, state = undefined) {
    const reader = stream.getReader();
    let total = 0;
    let finished = false;

    return new ReadableStream({
        async pull(controller) {
            const result = await reader.read();
            if (result.done) {
                finished = true;
                reader.releaseLock();
                controller.close();
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
    if (contentLength !== null &&
        /^[0-9]+$/.test(contentLength) &&
        Number(contentLength) > limit) {
        return rejectedBodyStream(
            response.body,
            bodyLimitError('response', limit));
    }
    return limitedBodyStream(response.body, limit, 'response');
}

class Request extends NativeRequest {
    constructor(input, init = undefined, brand = undefined) {
        const incoming = brand === incomingRequestBrand;
        const normalized = incoming
            ? normalizeIncomingInit(init)
            : normalizeInit(init);

        if (input instanceof Request) {
            const inherited = {
                method: input.method,
                headers: toNativeHeaderEntries(input.headers),
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
            super(input, normalized);
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
        const normalizedBody = normalizeBody(body);

        if (body !== null && body !== undefined && isNullBodyStatus(status)) {
            throw new TypeError('Response with a null-body status cannot have a body');
        }

        const fastText = typeof normalizedBody === 'string' && normalizedBody.length > 0
            ? normalizedBody
            : null;
        super(fastText === null ? normalizedBody : null, normalized);
        this.headers = new Headers(this.headers);
        this._nativeFormData = this.formData;
        this.formData = multipartFormDataOverride;

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
            fastResponseBodies.set(this, record);
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
        cloned.redirected = nativeClone.redirected;
        cloned.type = nativeClone.type;
        cloned.url = nativeClone.url;
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

function fetch(input, init = undefined) {
    let nativeInput = input;
    let nativeInit = init;
    let requestLimitState = null;

    if (input instanceof Request) {
        nativeInput = input.url;
        nativeInit = {
            method: input.method,
            headers: toNativeHeaderEntries(input.headers),
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
            Object.assign(nativeInit, normalizeInit(init));
        }
    } else if (init !== undefined) {
        nativeInit = normalizeInit(init);
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
            return response;
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
        return new Response(
            nullBody ? null : body,
            {
                status: response.status,
                statusText: response.statusText,
                headers: response.headers,
                url: response.url,
            });
    });
}

Object.defineProperties(globalThis, {
    Headers: { configurable: true, writable: true, value: Headers },
    Request: { configurable: true, writable: true, value: Request },
    Response: { configurable: true, writable: true, value: Response },
    fetch: { configurable: true, writable: true, value: fetch },
});
