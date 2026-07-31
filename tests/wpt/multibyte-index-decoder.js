// Worker adaptation for the legacy multibyte WPT index fixtures. The original
// tests use an iframe containing encoded HTML; this profile validates the same
// locked Encoding Standard indexes directly through TextDecoder.
const replacement = '\uFFFD';

const expectedCodePoint = codePoint =>
    codePoint == null ? replacement : String.fromCodePoint(codePoint);

const checkPointers = (name, count, bytesForPointer, expectedForPointer) => {
    const chunkSize = 512;
    for (let start = 0; start < count; start += chunkSize) {
        const end = Math.min(start + chunkSize, count);
        test(() => {
            const decoder = new TextDecoder(globalThis.__wptEncoding);
            for (let pointer = start; pointer < end; ++pointer) {
                const actual = decoder.decode(
                    new Uint8Array(bytesForPointer(pointer)));
                assert_equals(
                    actual,
                    expectedForPointer(pointer),
                    `pointer ${pointer}`);
            }
        }, `${name} pointers ${start}-${end - 1}`);
    }
};

switch (globalThis.__wptEncoding) {
    case 'big5':
        checkPointers(
            'Big5',
            big5.length,
            pointer => {
                const lead = Math.floor(pointer / 157) + 0x81;
                const trailPointer = pointer % 157;
                const trail = trailPointer +
                    (trailPointer < 0x3F ? 0x40 : 0x62);
                return [ lead, trail ];
            },
            pointer => {
                switch (pointer) {
                    case 1133: return '\u00CA\u0304';
                    case 1135: return '\u00CA\u030C';
                    case 1164: return '\u00EA\u0304';
                    case 1166: return '\u00EA\u030C';
                    default: {
                        if (big5[pointer] != null) {
                            return String.fromCodePoint(big5[pointer]);
                        }
                        const trailPointer = pointer % 157;
                        const trail = trailPointer +
                            (trailPointer < 0x3F ? 0x40 : 0x62);
                        return replacement +
                            (trail < 0x80
                                ? String.fromCharCode(trail)
                                : '');
                    }
                }
            });
        break;

    case 'euc-jp':
        checkPointers(
            'EUC-JP JIS0208',
            94 * 94,
            pointer => [
                Math.floor(pointer / 94) + 0xA1,
                pointer % 94 + 0xA1,
            ],
            pointer => expectedCodePoint(jis0208[pointer]));
        checkPointers(
            'EUC-JP JIS0212',
            94 * 94,
            pointer => [
                0x8F,
                Math.floor(pointer / 94) + 0xA1,
                pointer % 94 + 0xA1,
            ],
            pointer => expectedCodePoint(jis0212[pointer]));
        break;

    case 'shift_jis':
        checkPointers(
            'Shift_JIS',
            10716,
            pointer => {
                const leadPointer = Math.floor(pointer / 188);
                const lead = leadPointer +
                    (leadPointer < 0x1F ? 0x81 : 0xC1);
                const trailPointer = pointer % 188;
                const trail = trailPointer +
                    (trailPointer < 0x3F ? 0x40 : 0x41);
                return [ lead, trail ];
            },
            pointer => {
                if (pointer >= 8836) {
                    return String.fromCodePoint(0xE000 + pointer - 8836);
                }
                if (jis0208[pointer] != null) {
                    return String.fromCodePoint(jis0208[pointer]);
                }
                const trailPointer = pointer % 188;
                const trail = trailPointer +
                    (trailPointer < 0x3F ? 0x40 : 0x41);
                return replacement +
                    (trail < 0x80 ? String.fromCharCode(trail) : '');
            });
        break;

    case 'euc-kr':
        checkPointers(
            'EUC-KR',
            euckr.length,
            pointer => [
                Math.floor(pointer / 190) + 0x81,
                pointer % 190 + 0x41,
            ],
            pointer => {
                if (euckr[pointer] != null) {
                    return String.fromCodePoint(euckr[pointer]);
                }
                const trail = pointer % 190 + 0x41;
                return replacement +
                    (trail < 0x80 ? String.fromCharCode(trail) : '');
            });
        break;

    default:
        throw new TypeError(
            `unsupported multibyte index fixture: ${globalThis.__wptEncoding}`);
}
