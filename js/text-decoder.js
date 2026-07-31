import {
    SingleByteDecoder,
    singleByteDefinition,
} from './single-byte-decoder.js';
import {
    MultibyteDecoder,
    multibyteDefinition,
} from './multibyte-decoder.js';

const NativeTextDecoder = globalThis.TextDecoder;
const NativeTextEncoder = globalThis.TextEncoder;

const UTF8_LABELS = new Set([
    'unicode-1-1-utf-8',
    'unicode11utf8',
    'unicode20utf8',
    'utf-8',
    'utf8',
    'x-unicode20utf8',
]);

const UTF16_LABELS = new Map([
    [ 'csunicode', 'utf-16le' ],
    [ 'iso-10646-ucs-2', 'utf-16le' ],
    [ 'ucs-2', 'utf-16le' ],
    [ 'unicode', 'utf-16le' ],
    [ 'unicodefeff', 'utf-16le' ],
    [ 'unicodefffe', 'utf-16be' ],
    [ 'utf-16', 'utf-16le' ],
    [ 'utf-16le', 'utf-16le' ],
    [ 'utf-16be', 'utf-16be' ],
]);

function isSharedArrayBuffer(value) {
    return typeof SharedArrayBuffer !== 'undefined' &&
        value instanceof SharedArrayBuffer;
}

function bytesFromBufferSource(input) {
    if (input === undefined) {
        return new Uint8Array(0);
    }
    if (isSharedArrayBuffer(input)) {
        return new Uint8Array(new Uint8Array(input));
    }
    if (input instanceof ArrayBuffer) {
        if (input.byteLength === 0) {
            return new Uint8Array(0);
        }
        return new Uint8Array(input);
    }
    if (ArrayBuffer.isView(input)) {
        if (input.byteLength === 0) {
            return new Uint8Array(0);
        }
        const bytes = new Uint8Array(
            input.buffer, input.byteOffset, input.byteLength);
        return isSharedArrayBuffer(input.buffer)
            ? new Uint8Array(bytes)
            : bytes;
    }
    throw new TypeError('Expected an ArrayBuffer or ArrayBufferView');
}

function normalizeNativeBufferSource(input) {
    if (input === undefined) {
        return undefined;
    }
    if (isSharedArrayBuffer(input)) {
        return new Uint8Array(new Uint8Array(input));
    }
    if (input instanceof ArrayBuffer) {
        return input.byteLength === 0 ? new Uint8Array(0) : input;
    }
    if (ArrayBuffer.isView(input)) {
        if (input.byteLength === 0) {
            return new Uint8Array(0);
        }
        if (isSharedArrayBuffer(input.buffer)) {
            return new Uint8Array(new Uint8Array(
                input.buffer, input.byteOffset, input.byteLength));
        }
        return input;
    }
    return input;
}

function normalizeEncodingLabel(label) {
    const input = String(label);
    const trimmed = input.replace(
        /^[\u0009\u000A\u000C\u000D\u0020]+|[\u0009\u000A\u000C\u000D\u0020]+$/g,
        '');
    return trimmed.replace(/[A-Z]/g, character =>
        String.fromCharCode(character.charCodeAt(0) + 0x20));
}

class UTF8Decoder {
    #decoder;
    #fatal;
    #ignoreBOM;
    #bomSeen = false;

    constructor(options) {
        options = options == null ? {} : Object(options);
        this.#fatal = Boolean(options.fatal);
        this.#ignoreBOM = Boolean(options.ignoreBOM);
        this.#decoder = new NativeTextDecoder('utf-8', {
            fatal: this.#fatal,
            ignoreBOM: true,
        });
    }

    get encoding() {
        return 'utf-8';
    }

    get fatal() {
        return this.#fatal;
    }

    get ignoreBOM() {
        return this.#ignoreBOM;
    }

    decode(input, options = {}) {
        options = options == null ? {} : Object(options);
        const stream = Boolean(options.stream);
        input = normalizeNativeBufferSource(input);
        let output;
        try {
            output = this.#decoder.decode(input, { stream });
        } catch (error) {
            this.#bomSeen = false;
            throw error;
        }
        if (output.length > 0 && !this.#bomSeen) {
            this.#bomSeen = true;
            if (!this.#ignoreBOM && output.charCodeAt(0) === 0xFEFF) {
                output = output.slice(1);
            }
        }
        if (!stream) {
            this.#bomSeen = false;
        }
        return output;
    }
}

class UTF16Decoder {
    #encoding;
    #fatal;
    #ignoreBOM;
    #pendingByte = null;
    #pendingHigh = null;
    #bomSeen = false;

    constructor(encoding, options) {
        options = options == null ? {} : Object(options);
        this.#encoding = encoding;
        this.#fatal = Boolean(options.fatal);
        this.#ignoreBOM = Boolean(options.ignoreBOM);
    }

    get encoding() {
        return this.#encoding;
    }

    get fatal() {
        return this.#fatal;
    }

    get ignoreBOM() {
        return this.#ignoreBOM;
    }

    #reset() {
        this.#pendingByte = null;
        this.#pendingHigh = null;
        this.#bomSeen = false;
    }

    #error(output) {
        if (this.#fatal) {
            this.#reset();
            throw new TypeError('decoding error');
        }
        output.push('\uFFFD');
    }

    decode(input, options = {}) {
        options = options == null ? {} : Object(options);
        const stream = Boolean(options.stream);
        let bytes = bytesFromBufferSource(input);
        const output = [];

        if (this.#pendingByte !== null) {
            const combined = new Uint8Array(bytes.length + 1);
            combined[0] = this.#pendingByte;
            combined.set(bytes, 1);
            bytes = combined;
            this.#pendingByte = null;
        }

        const littleEndian = this.#encoding === 'utf-16le';
        const emit = codeUnit => {
            if (!this.#bomSeen) {
                this.#bomSeen = true;
                if (!this.#ignoreBOM && codeUnit === 0xFEFF) {
                    return;
                }
            }

            if (this.#pendingHigh !== null) {
                if (codeUnit >= 0xDC00 && codeUnit <= 0xDFFF) {
                    output.push(String.fromCharCode(
                        this.#pendingHigh, codeUnit));
                    this.#pendingHigh = null;
                    return;
                }
                this.#error(output);
                this.#pendingHigh = null;
            }

            if (codeUnit >= 0xD800 && codeUnit <= 0xDBFF) {
                this.#pendingHigh = codeUnit;
            } else if (codeUnit >= 0xDC00 && codeUnit <= 0xDFFF) {
                this.#error(output);
            } else {
                output.push(String.fromCharCode(codeUnit));
            }
        };

        let offset = 0;
        for (; offset + 1 < bytes.length; offset += 2) {
            const codeUnit = littleEndian
                ? bytes[offset] | (bytes[offset + 1] << 8)
                : (bytes[offset] << 8) | bytes[offset + 1];
            emit(codeUnit);
        }

        if (offset < bytes.length) {
            if (stream) {
                this.#pendingByte = bytes[offset];
            } else {
                if (this.#pendingHigh !== null) {
                    this.#error(output);
                    this.#pendingHigh = null;
                } else {
                    this.#error(output);
                }
            }
        }

        if (!stream) {
            if (this.#pendingHigh !== null) {
                this.#error(output);
            }
            this.#reset();
        }

        return output.join('');
    }
}

class TextDecoder {
    #implementation;

    constructor(label = 'utf-8', options = {}) {
        const normalizedLabel = normalizeEncodingLabel(label);
        const utf16Encoding = UTF16_LABELS.get(normalizedLabel);
        const singleByteEncoding =
            singleByteDefinition(normalizedLabel);
        const multibyteEncoding =
            multibyteDefinition(normalizedLabel);

        if (UTF8_LABELS.has(normalizedLabel)) {
            this.#implementation = new UTF8Decoder(options);
        } else if (utf16Encoding !== undefined) {
            this.#implementation =
                new UTF16Decoder(utf16Encoding, options);
        } else if (singleByteEncoding !== undefined) {
            this.#implementation =
                new SingleByteDecoder(singleByteEncoding, options);
        } else if (multibyteEncoding !== undefined) {
            this.#implementation =
                new MultibyteDecoder(multibyteEncoding, options);
        } else {
            throw new RangeError(`Unsupported encoding: ${String(label)}`);
        }
    }

    get encoding() {
        return this.#implementation.encoding;
    }

    get fatal() {
        return this.#implementation.fatal;
    }

    get ignoreBOM() {
        return this.#implementation.ignoreBOM;
    }

    decode(input = undefined, options = undefined) {
        return this.#implementation.decode(input, options);
    }
}

class TextEncoder {
    #encoder = new NativeTextEncoder();

    constructor() {}

    get encoding() {
        // Web IDL attributes must reject incompatible receivers.
        this.#encoder;
        return 'utf-8';
    }

    encode(input = '') {
        return this.#encoder.encode(input);
    }

    encodeInto(source, destination) {
        source = String(source);
        if (!(destination instanceof Uint8Array)) {
            throw new TypeError(
                'TextEncoder.encodeInto destination must be a Uint8Array');
        }
        if (destination.byteLength === 0) {
            return { read: 0, written: 0 };
        }
        return this.#encoder.encodeInto(source, destination);
    }
}

function makeAccessorsEnumerable(prototype, names) {
    for (const name of names) {
        const descriptor =
            Object.getOwnPropertyDescriptor(prototype, name);
        Object.defineProperty(prototype, name, {
            ...descriptor,
            enumerable: true,
        });
    }
}

class TextEncoderStream {
    #transform;

    constructor() {
        const encoder = new TextEncoder();
        let pendingHighSurrogate;
        this.#transform = new TransformStream({
            transform(chunk, controller) {
                chunk = String(chunk);
                if (pendingHighSurrogate !== undefined) {
                    chunk = pendingHighSurrogate + chunk;
                    pendingHighSurrogate = undefined;
                }
                const finalCodeUnit = chunk.charCodeAt(chunk.length - 1);
                if (finalCodeUnit >= 0xD800 &&
                    finalCodeUnit <= 0xDBFF) {
                    pendingHighSurrogate =
                        chunk.slice(chunk.length - 1);
                    chunk = chunk.slice(0, -1);
                }
                const encoded = encoder.encode(chunk);
                if (encoded.length !== 0) {
                    controller.enqueue(encoded);
                }
            },
            flush(controller) {
                if (pendingHighSurrogate !== undefined) {
                    controller.enqueue(
                        encoder.encode(pendingHighSurrogate));
                }
            },
        });
    }

    get encoding() {
        // Web IDL attributes must reject incompatible receivers.
        this.#transform;
        return 'utf-8';
    }

    get readable() {
        return this.#transform.readable;
    }

    get writable() {
        return this.#transform.writable;
    }
}

class TextDecoderStream {
    #decoder;
    #readable;
    #writable;

    constructor(label = 'utf-8', options = {}) {
        this.#decoder = new TextDecoder(label, options);
        const decoder = this.#decoder;
        const transform = new TransformStream({
            transform(chunk, controller) {
                const isBufferSource =
                    chunk instanceof ArrayBuffer ||
                    isSharedArrayBuffer(chunk) ||
                    ArrayBuffer.isView(chunk);
                if (!isBufferSource) {
                    throw new TypeError(
                        'TextDecoderStream chunks must be BufferSource');
                }
                const decoded = decoder.decode(chunk, { stream: true });
                if (decoded !== '') {
                    controller.enqueue(decoded);
                }
            },
            flush(controller) {
                const decoded = decoder.decode();
                if (decoded !== '') {
                    controller.enqueue(decoded);
                }
            },
        });
        const writer = transform.writable.getWriter();
        this.#readable = transform.readable;
        this.#writable = new WritableStream({
            write(chunk) {
                if (chunk === undefined) {
                    throw new TypeError(
                        'TextDecoderStream chunks must be BufferSource');
                }
                return writer.write(chunk);
            },
            close() {
                return writer.close();
            },
            abort(reason) {
                return writer.abort(reason);
            },
        });
        const writable = this.#writable;
        const getWriter = writable.getWriter;
        Object.defineProperty(writable, 'getWriter', {
            configurable: true,
            writable: true,
            value() {
                const streamWriter = getWriter.call(this);
                const write = streamWriter.write;
                Object.defineProperty(streamWriter, 'write', {
                    configurable: true,
                    writable: true,
                    value(chunk) {
                        if (chunk === undefined) {
                            // The txiki streams layer treats an undefined
                            // write as an omitted chunk. Route a guaranteed
                            // invalid value through the transform so both
                            // sides enter the required errored state.
                            return write.call(this, null);
                        }
                        return write.call(this, chunk);
                    },
                });
                return streamWriter;
            },
        });
    }

    get encoding() {
        return this.#decoder.encoding;
    }

    get fatal() {
        return this.#decoder.fatal;
    }

    get ignoreBOM() {
        return this.#decoder.ignoreBOM;
    }

    get readable() {
        return this.#readable;
    }

    get writable() {
        return this.#writable;
    }
}

makeAccessorsEnumerable(
    TextDecoder.prototype,
    [ 'encoding', 'fatal', 'ignoreBOM', 'decode' ]);
makeAccessorsEnumerable(
    TextEncoder.prototype,
    [ 'encoding', 'encode', 'encodeInto' ]);
makeAccessorsEnumerable(
    TextEncoderStream.prototype,
    [ 'encoding', 'readable', 'writable' ]);
makeAccessorsEnumerable(
    TextDecoderStream.prototype,
    [ 'encoding', 'fatal', 'ignoreBOM', 'readable', 'writable' ]);
Object.defineProperty(
    TextDecoder.prototype,
    Symbol.toStringTag,
    { configurable: true, value: 'TextDecoder' });
Object.defineProperty(
    TextEncoder.prototype,
    Symbol.toStringTag,
    { configurable: true, value: 'TextEncoder' });
Object.defineProperty(
    TextEncoderStream.prototype,
    Symbol.toStringTag,
    { configurable: true, value: 'TextEncoderStream' });
Object.defineProperty(
    TextDecoderStream.prototype,
    Symbol.toStringTag,
    { configurable: true, value: 'TextDecoderStream' });

Object.defineProperty(globalThis, 'TextDecoder', {
    configurable: true,
    enumerable: false,
    writable: true,
    value: TextDecoder,
});

Object.defineProperty(globalThis, 'TextEncoder', {
    configurable: true,
    enumerable: false,
    writable: true,
    value: TextEncoder,
});

Object.defineProperty(globalThis, 'TextEncoderStream', {
    configurable: true,
    enumerable: false,
    writable: true,
    value: TextEncoderStream,
});

Object.defineProperty(globalThis, 'TextDecoderStream', {
    configurable: true,
    enumerable: false,
    writable: true,
    value: TextDecoderStream,
});
