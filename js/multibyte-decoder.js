import core from 'tjs:internal/core';
import { encodingGroups } from './encoding-data/encodings.js';
import {
    multibyteIndexCodePoint,
} from './encoding-data/multibyte-indexes.js';

const supportedNames = new Set([
    'GBK',
    'gb18030',
    'Big5',
    'EUC-JP',
    'ISO-2022-JP',
    'Shift_JIS',
    'EUC-KR',
    'x-user-defined',
]);
const definitionsByLabel = new Map();

const gb18030TwoByteOverrides = new Map([
    [ 0xA1AD, '\u2026' ],
    [ 0xA1AB, '\uFF5E' ],
    [ 0xA3A0, '\u3000' ],
    [ 0xA6D9, '\uFE10' ],
    [ 0xA6DA, '\uFE12' ],
    [ 0xA6DB, '\uFE11' ],
    [ 0xA6DC, '\uFE13' ],
    [ 0xA6DD, '\uFE14' ],
    [ 0xA6DE, '\uFE15' ],
    [ 0xA6DF, '\uFE16' ],
    [ 0xA6EC, '\uFE17' ],
    [ 0xA6ED, '\uFE18' ],
    [ 0xA6F3, '\uFE19' ],
    [ 0xA8BC, '\u1E3F' ],
    [ 0xFE59, '\u9FB4' ],
    [ 0xFE61, '\u9FB5' ],
    [ 0xFE66, '\u9FB6' ],
    [ 0xFE67, '\u9FB7' ],
    [ 0xFE6D, '\u9FB8' ],
    [ 0xFE7E, '\u9FB9' ],
    [ 0xFE90, '\u9FBA' ],
    [ 0xFEA0, '\u9FBB' ],
]);

for (const group of encodingGroups) {
    for (const definition of group.encodings) {
        if (!supportedNames.has(definition.name)) {
            continue;
        }
        const normalized = Object.freeze({
            encoding: definition.name.toLowerCase(),
            nativeEncoding: definition.name,
        });
        for (const label of definition.labels) {
            definitionsByLabel.set(label, normalized);
        }
    }
}

function bytesFromBufferSource(input) {
    if (input === undefined) {
        return new Uint8Array(0);
    }
    if (typeof SharedArrayBuffer !== 'undefined' &&
        input instanceof SharedArrayBuffer) {
        return new Uint8Array(new Uint8Array(input));
    }
    if (input instanceof ArrayBuffer) {
        return input.byteLength === 0
            ? new Uint8Array(0)
            : new Uint8Array(input);
    }
    if (ArrayBuffer.isView(input)) {
        if (input.byteLength === 0) {
            return new Uint8Array(0);
        }
        const bytes = new Uint8Array(
            input.buffer, input.byteOffset, input.byteLength);
        return typeof SharedArrayBuffer !== 'undefined' &&
            input.buffer instanceof SharedArrayBuffer
            ? new Uint8Array(bytes)
            : bytes;
    }
    throw new TypeError('Expected an ArrayBuffer or ArrayBufferView');
}

export function multibyteDefinition(label) {
    return definitionsByLabel.get(label);
}

export class MultibyteDecoder {
    #definition;
    #fatal;
    #ignoreBOM;
    #pending = null;
    #isoState = 'ascii';
    #isoOutputState = 'ascii';
    #isoLead = 0;
    #isoOutputFlag = false;

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

    #error(output) {
        if (this.#fatal) {
            this.#pending = null;
            throw new TypeError('decoding error');
        }
        output.push('\uFFFD');
    }

    #decodeGB18030(bytes, stream) {
        const output = [];
        let offset = 0;

        while (offset < bytes.length) {
            const sequenceStart = offset;
            const first = bytes[offset++];
            if (first < 0x80) {
                output.push(String.fromCharCode(first));
                continue;
            }
            if (first === 0x80) {
                output.push('\u20AC');
                continue;
            }
            if (first < 0x81 || first > 0xFE) {
                this.#error(output);
                continue;
            }
            if (offset === bytes.length) {
                if (stream) {
                    this.#pending = bytes.slice(sequenceStart);
                    break;
                }
                this.#error(output);
                continue;
            }

            const secondOffset = offset;
            const second = bytes[offset++];
            const twoByteTrail =
                second >= 0x40 && second <= 0xFE && second !== 0x7F;
            if (twoByteTrail) {
                const override =
                    gb18030TwoByteOverrides.get((first << 8) | second);
                if (override !== undefined) {
                    output.push(override);
                } else {
                    output.push(core.legacyDecode(
                        this.#definition.nativeEncoding,
                        new Uint8Array([ first, second ]),
                        this.#fatal));
                }
                continue;
            }

            if (second < 0x30 || second > 0x39) {
                this.#error(output);
                if (second < 0x80) {
                    offset--;
                }
                continue;
            }
            if (offset === bytes.length) {
                if (stream) {
                    this.#pending = bytes.slice(sequenceStart);
                    break;
                }
                this.#error(output);
                continue;
            }

            const third = bytes[offset++];
            if (third < 0x81 || third > 0xFE) {
                this.#error(output);
                offset = secondOffset;
                continue;
            }
            if (offset === bytes.length) {
                if (stream) {
                    this.#pending = bytes.slice(sequenceStart);
                    break;
                }
                this.#error(output);
                continue;
            }

            const fourth = bytes[offset++];
            if (fourth < 0x30 || fourth > 0x39) {
                this.#error(output);
                offset = secondOffset;
                continue;
            }

            const pointer =
                (first - 0x81) * 12600 +
                (second - 0x30) * 1260 +
                (third - 0x81) * 10 +
                fourth - 0x30;
            if ((pointer >= 39420 && pointer < 189000) ||
                pointer > 1237575) {
                this.#error(output);
                continue;
            }
            if (pointer === 7457) {
                output.push('\uE7C7');
                continue;
            }
            output.push(core.legacyDecode(
                this.#definition.nativeEncoding,
                new Uint8Array([ first, second, third, fourth ]),
                this.#fatal));
        }

        return output.join('');
    }

    #decodeUserDefined(bytes) {
        const output = [];
        for (const byte of bytes) {
            output.push(String.fromCharCode(
                byte < 0x80 ? byte : 0xF780 + byte - 0x80));
        }
        return output.join('');
    }

    #appendNative(output, bytes, override) {
        if (override !== undefined) {
            output.push(override);
            return true;
        }
        try {
            output.push(core.legacyDecode(
                this.#definition.nativeEncoding,
                new Uint8Array(bytes),
                true));
            return true;
        } catch (error) {
            this.#error(output);
            return false;
        }
    }

    #decodeBig5(bytes, stream) {
        const output = [];
        let offset = 0;
        while (offset < bytes.length) {
            const start = offset;
            const lead = bytes[offset++];
            if (lead < 0x80) {
                output.push(String.fromCharCode(lead));
                continue;
            }
            if (lead < 0x81 || lead > 0xFE) {
                this.#error(output);
                continue;
            }
            if (offset === bytes.length) {
                if (stream) {
                    this.#pending = bytes.slice(start);
                    break;
                }
                this.#error(output);
                continue;
            }
            const trail = bytes[offset++];
            const validTrail =
                (trail >= 0x40 && trail <= 0x7E) ||
                (trail >= 0xA1 && trail <= 0xFE);
            if (!validTrail) {
                this.#error(output);
                if (trail < 0x80) {
                    offset--;
                }
                continue;
            }
            const pointer =
                (lead - 0x81) * 157 +
                trail - (trail < 0x7F ? 0x40 : 0x62);
            let outputValue;
            if (pointer === 1133) {
                outputValue = '\u00CA\u0304';
            } else if (pointer === 1135) {
                outputValue = '\u00CA\u030C';
            } else if (pointer === 1164) {
                outputValue = '\u00EA\u0304';
            } else if (pointer === 1166) {
                outputValue = '\u00EA\u030C';
            } else {
                const codePoint =
                    multibyteIndexCodePoint('big5', pointer);
                if (codePoint !== null) {
                    outputValue = String.fromCodePoint(codePoint);
                }
            }
            if (outputValue === undefined) {
                this.#error(output);
                if (trail < 0x80) {
                    offset--;
                }
            } else {
                output.push(outputValue);
            }
        }
        return output.join('');
    }

    #decodeShiftJIS(bytes, stream) {
        const output = [];
        let offset = 0;
        while (offset < bytes.length) {
            const start = offset;
            const lead = bytes[offset++];
            if (lead < 0x80) {
                output.push(String.fromCharCode(lead));
                continue;
            }
            if (lead === 0x80) {
                output.push('\u0080');
                continue;
            }
            if (lead >= 0xA1 && lead <= 0xDF) {
                output.push(String.fromCharCode(0xFF61 + lead - 0xA1));
                continue;
            }
            const validLead =
                (lead >= 0x81 && lead <= 0x9F) ||
                (lead >= 0xE0 && lead <= 0xFC);
            if (!validLead) {
                this.#error(output);
                continue;
            }
            if (offset === bytes.length) {
                if (stream) {
                    this.#pending = bytes.slice(start);
                    break;
                }
                this.#error(output);
                continue;
            }
            const trail = bytes[offset++];
            const validTrail =
                trail >= 0x40 && trail <= 0xFC && trail !== 0x7F;
            if (!validTrail) {
                this.#error(output);
                if (trail < 0x80) {
                    offset--;
                }
                continue;
            }
            const trailOffset = trail < 0x7F ? 0x40 : 0x41;
            const leadOffset = lead < 0xA0 ? 0x81 : 0xC1;
            const pointer =
                (lead - leadOffset) * 188 + trail - trailOffset;
            let codePoint;
            if (pointer >= 8836 && pointer <= 10715) {
                codePoint = 0xE000 + pointer - 8836;
            } else {
                codePoint = multibyteIndexCodePoint('jis0208', pointer);
            }
            if (codePoint === null) {
                this.#error(output);
                if (trail < 0x80) {
                    offset--;
                }
            } else {
                output.push(String.fromCodePoint(codePoint));
            }
        }
        return output.join('');
    }

    #decodeEUCKR(bytes, stream) {
        const output = [];
        let offset = 0;
        while (offset < bytes.length) {
            const start = offset;
            const lead = bytes[offset++];
            if (lead < 0x80) {
                output.push(String.fromCharCode(lead));
                continue;
            }
            if (lead < 0x81 || lead > 0xFE) {
                this.#error(output);
                continue;
            }
            if (offset === bytes.length) {
                if (stream) {
                    this.#pending = bytes.slice(start);
                    break;
                }
                this.#error(output);
                continue;
            }
            const trail = bytes[offset++];
            const validTrail = trail >= 0x41 && trail <= 0xFE;
            if (!validTrail) {
                this.#error(output);
                if (trail < 0x80) {
                    offset--;
                }
                continue;
            }
            const pointer =
                (lead - 0x81) * 190 + trail - 0x41;
            const codePoint =
                multibyteIndexCodePoint('euckr', pointer);
            if (codePoint === null) {
                this.#error(output);
                if (trail < 0x80) {
                    offset--;
                }
            } else {
                output.push(String.fromCodePoint(codePoint));
            }
        }
        return output.join('');
    }

    #decodeEUCJP(bytes, stream) {
        const output = [];
        let offset = 0;
        while (offset < bytes.length) {
            const start = offset;
            const lead = bytes[offset++];
            if (lead < 0x80) {
                output.push(String.fromCharCode(lead));
                continue;
            }
            let length;
            if (lead === 0x8E) {
                length = 2;
            } else if (lead === 0x8F) {
                length = 3;
            } else if (lead >= 0xA1 && lead <= 0xFE) {
                length = 2;
            } else {
                this.#error(output);
                continue;
            }
            if (offset + length - 1 > bytes.length) {
                if (stream) {
                    this.#pending = bytes.slice(start);
                    break;
                }
                this.#error(output);
                offset = bytes.length;
                continue;
            }
            const sequence = Array.from(bytes.slice(start, start + length));
            const second = sequence[1];
            const validSecond = lead === 0x8E
                ? second >= 0xA1 && second <= 0xDF
                : second >= 0xA1 && second <= 0xFE;
            const validThird =
                length !== 3 ||
                (sequence[2] >= 0xA1 && sequence[2] <= 0xFE);
            if (!validSecond || !validThird) {
                this.#error(output);
                if (second < 0x80) {
                    offset = start + 1;
                } else {
                    offset = start + length;
                }
                continue;
            }
            offset = start + length;
            if (lead === 0x8E) {
                output.push(String.fromCodePoint(
                    0xFF61 + second - 0xA1));
                continue;
            }
            const pointerLead = lead === 0x8F ? sequence[1] : lead;
            const pointerTrail = lead === 0x8F ? sequence[2] : second;
            const pointer =
                (pointerLead - 0xA1) * 94 + pointerTrail - 0xA1;
            const codePoint = multibyteIndexCodePoint(
                lead === 0x8F ? 'jis0212' : 'jis0208',
                pointer);
            if (codePoint === null) {
                this.#error(output);
            } else {
                output.push(String.fromCodePoint(codePoint));
            }
        }
        return output.join('');
    }

    #resetISO2022JP() {
        this.#isoState = 'ascii';
        this.#isoOutputState = 'ascii';
        this.#isoLead = 0;
        this.#isoOutputFlag = false;
    }

    #decodeISO2022JP(bytes, stream) {
        const end = -1;
        const queue = Array.from(bytes);
        if (!stream) {
            queue.push(end);
        }
        const output = [];
        let finished = false;

        try {
            while (!finished && queue.length > 0) {
                const byte = queue.shift();
                switch (this.#isoState) {
                    case 'ascii':
                        if (byte === 0x1B) {
                            this.#isoState = 'escape-start';
                        } else if (byte === end) {
                            finished = true;
                        } else if (byte >= 0 && byte <= 0x7F &&
                                   byte !== 0x0E && byte !== 0x0F) {
                            this.#isoOutputFlag = false;
                            output.push(String.fromCharCode(byte));
                        } else {
                            this.#isoOutputFlag = false;
                            this.#error(output);
                        }
                        break;
                    case 'roman':
                        if (byte === 0x1B) {
                            this.#isoState = 'escape-start';
                        } else if (byte === end) {
                            finished = true;
                        } else if (byte === 0x5C) {
                            this.#isoOutputFlag = false;
                            output.push('\u00A5');
                        } else if (byte === 0x7E) {
                            this.#isoOutputFlag = false;
                            output.push('\u203E');
                        } else if (byte >= 0 && byte <= 0x7F &&
                                   byte !== 0x0E && byte !== 0x0F) {
                            this.#isoOutputFlag = false;
                            output.push(String.fromCharCode(byte));
                        } else {
                            this.#isoOutputFlag = false;
                            this.#error(output);
                        }
                        break;
                    case 'katakana':
                        if (byte === 0x1B) {
                            this.#isoState = 'escape-start';
                        } else if (byte === end) {
                            finished = true;
                        } else if (byte >= 0x21 && byte <= 0x5F) {
                            this.#isoOutputFlag = false;
                            output.push(String.fromCharCode(
                                0xFF61 + byte - 0x21));
                        } else {
                            this.#isoOutputFlag = false;
                            this.#error(output);
                        }
                        break;
                    case 'lead':
                        if (byte === 0x1B) {
                            this.#isoState = 'escape-start';
                        } else if (byte === end) {
                            finished = true;
                        } else if (byte >= 0x21 && byte <= 0x7E) {
                            this.#isoOutputFlag = false;
                            this.#isoLead = byte;
                            this.#isoState = 'trail';
                        } else {
                            this.#isoOutputFlag = false;
                            this.#error(output);
                        }
                        break;
                    case 'trail':
                        if (byte === 0x1B) {
                            this.#isoState = 'escape-start';
                            this.#error(output);
                        } else if (byte >= 0x21 && byte <= 0x7E) {
                            const lead = this.#isoLead;
                            this.#isoState = 'lead';
                            const pointer =
                                (lead - 0x21) * 94 + byte - 0x21;
                            const codePoint = multibyteIndexCodePoint(
                                'jis0208', pointer);
                            if (codePoint === null) {
                                this.#error(output);
                            } else {
                                output.push(String.fromCodePoint(codePoint));
                            }
                        } else if (byte === end) {
                            this.#isoState = 'lead';
                            queue.unshift(byte);
                            this.#error(output);
                        } else {
                            this.#isoState = 'lead';
                            this.#error(output);
                        }
                        break;
                    case 'escape-start':
                        if (byte === 0x24 || byte === 0x28) {
                            this.#isoLead = byte;
                            this.#isoState = 'escape';
                        } else {
                            queue.unshift(byte);
                            this.#isoOutputFlag = false;
                            this.#isoState = this.#isoOutputState;
                            this.#error(output);
                        }
                        break;
                    case 'escape': {
                        const lead = this.#isoLead;
                        let state;
                        if (lead === 0x28 && byte === 0x42) {
                            state = 'ascii';
                        } else if (lead === 0x28 && byte === 0x4A) {
                            state = 'roman';
                        } else if (lead === 0x28 && byte === 0x49) {
                            state = 'katakana';
                        } else if (lead === 0x24 &&
                                   (byte === 0x40 || byte === 0x42)) {
                            state = 'lead';
                        }
                        if (state !== undefined) {
                            this.#isoState = state;
                            this.#isoOutputState = state;
                            const previousFlag = this.#isoOutputFlag;
                            this.#isoOutputFlag = true;
                            if (previousFlag) {
                                this.#error(output);
                            }
                        } else {
                            queue.unshift(lead, byte);
                            this.#isoOutputFlag = false;
                            this.#isoState = this.#isoOutputState;
                            this.#error(output);
                        }
                        break;
                    }
                }
            }
        } catch (error) {
            if (!stream) {
                this.#resetISO2022JP();
            }
            throw error;
        }

        if (!stream) {
            this.#resetISO2022JP();
        }
        return output.join('');
    }

    decode(input, options = {}) {
        options = options == null ? {} : Object(options);
        const stream = Boolean(options.stream);
        let bytes = bytesFromBufferSource(input);

        if (this.#pending !== null) {
            const combined = new Uint8Array(
                this.#pending.length + bytes.length);
            combined.set(this.#pending);
            combined.set(bytes, this.#pending.length);
            bytes = combined;
            this.#pending = null;
        }

        if (this.#definition.nativeEncoding === 'x-user-defined') {
            return this.#decodeUserDefined(bytes);
        }
        if (this.#definition.nativeEncoding === 'GBK' ||
            this.#definition.nativeEncoding === 'gb18030') {
            return this.#decodeGB18030(bytes, stream);
        }
        if (this.#definition.nativeEncoding === 'Big5') {
            return this.#decodeBig5(bytes, stream);
        }
        if (this.#definition.nativeEncoding === 'Shift_JIS') {
            return this.#decodeShiftJIS(bytes, stream);
        }
        if (this.#definition.nativeEncoding === 'EUC-KR') {
            return this.#decodeEUCKR(bytes, stream);
        }
        if (this.#definition.nativeEncoding === 'EUC-JP') {
            return this.#decodeEUCJP(bytes, stream);
        }
        if (this.#definition.nativeEncoding === 'ISO-2022-JP') {
            return this.#decodeISO2022JP(bytes, stream);
        }
        if (stream) {
            this.#pending = new Uint8Array(bytes);
            return '';
        }
        return core.legacyDecode(
            this.#definition.nativeEncoding, bytes, this.#fatal);
    }
}
