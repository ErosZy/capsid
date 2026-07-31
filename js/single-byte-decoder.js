import { encodingGroups } from './encoding-data/encodings.js';
import { singleByteIndexes } from './encoding-data/single-byte-indexes.js';

const legacySingleByteGroup = encodingGroups.find(
    group => group.heading === 'Legacy single-byte encodings');

if (!legacySingleByteGroup) {
    throw new TypeError('Encoding Standard single-byte data is missing');
}

const definitionsByLabel = new Map();

for (const definition of legacySingleByteGroup.encodings) {
    const indexName = definition.name === 'ISO-8859-8-I'
        ? 'ISO-8859-8'
        : definition.name;
    const index = singleByteIndexes[indexName];
    if (!index || index.length !== 128) {
        throw new TypeError(`Encoding Standard index is missing: ${indexName}`);
    }
    const normalized = Object.freeze({
        encoding: definition.name.toLowerCase(),
        index,
    });
    for (const label of definition.labels) {
        definitionsByLabel.set(label, normalized);
    }
}

export function singleByteDefinition(label) {
    return definitionsByLabel.get(label);
}

export class SingleByteDecoder {
    #definition;
    #fatal;
    #ignoreBOM;

    constructor(definition, options) {
        options = options == null ? {} : Object(options);
        this.#definition = definition;
        this.#fatal = Boolean(options.fatal);
        this.#ignoreBOM = Boolean(options.ignoreBOM);
    }

    get encoding() {
        return this.#definition.encoding;
    }

    get fatal() {
        return this.#fatal;
    }

    get ignoreBOM() {
        return this.#ignoreBOM;
    }

    decode(input, options = {}) {
        options = options == null ? {} : Object(options);
        Boolean(options.stream);
        let bytes;
        if (input === undefined) {
            bytes = new Uint8Array(0);
        } else if (typeof SharedArrayBuffer !== 'undefined' &&
                   input instanceof SharedArrayBuffer) {
            bytes = new Uint8Array(new Uint8Array(input));
        } else if (input instanceof ArrayBuffer) {
            bytes = input.byteLength === 0
                ? new Uint8Array(0)
                : new Uint8Array(input);
        } else if (ArrayBuffer.isView(input)) {
            if (input.byteLength === 0) {
                bytes = new Uint8Array(0);
            } else {
                bytes = new Uint8Array(
                    input.buffer, input.byteOffset, input.byteLength);
                if (typeof SharedArrayBuffer !== 'undefined' &&
                    input.buffer instanceof SharedArrayBuffer) {
                    bytes = new Uint8Array(bytes);
                }
            }
        } else {
            throw new TypeError('Expected an ArrayBuffer or ArrayBufferView');
        }

        const output = [];
        const index = this.#definition.index;
        for (const byte of bytes) {
            if (byte < 0x80) {
                output.push(String.fromCharCode(byte));
                continue;
            }
            const codePoint = index[byte - 0x80];
            if (codePoint === null) {
                if (this.#fatal) {
                    throw new TypeError('decoding error');
                }
                output.push('\uFFFD');
            } else {
                output.push(String.fromCodePoint(codePoint));
            }
        }
        return output.join('');
    }
}
