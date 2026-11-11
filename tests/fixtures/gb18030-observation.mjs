function fourBytes(pointer) {
    return [Math.floor(pointer / 12600) + 0x81,
        Math.floor(pointer / 1260) % 10 + 0x30,
        Math.floor(pointer / 10) % 126 + 0x81, pointer % 10 + 0x30];
}

// Inputs only. Expected values live in the external driver, never in the Worker.
export const cases = [
    ...[0, 35, 36, 7456, 7457, 7458, 19056, 19057, 19058, 19059,
        19060, 19061, 19062, 19063, 19064, 19065, 33469, 39075,
        39076, 39077, 39078, 39079, 39080, 39081, 39082, 39083,
        39084, 39085, 39086, 39419, 39420, 188999, 189000,
        1237575, 1237576, 1587599].map(pointer => ({id: `p${pointer}`, bytes: fourBytes(pointer)})),
    {id: 'two-byte-2022', bytes: [0xFE, 0x59, 0xA6, 0xD9, 0xA6, 0xDA, 0xA6, 0xDB]},
    {id: 'two-byte-pua', bytes: [0xAA, 0xA1, 0xA3, 0xA0]},
    {id: 'ascii-adjacent', bytes: [65, ...fourBytes(19057), 66, ...fourBytes(39076), 67]},
    {id: 'euro', bytes: [0x80]},
    {id: 'truncated-one', bytes: [0x81]},
    {id: 'truncated-two', bytes: [0x81, 0x30]},
    {id: 'truncated-three', bytes: [0x81, 0x30, 0x81]},
    {id: 'bad-third', bytes: [0x81, 0x30, 65]},
    {id: 'bad-fourth', bytes: [0x81, 0x30, 0x81, 65]},
    {id: 'bad-two-byte', bytes: [0x81, 65 - 2]},
];

const points = text => Array.from(text, char => char.codePointAt(0));
function observe(label, bytes, fatal, cuts) {
    const decoder = new TextDecoder(label, {fatal});
    const chunks = [];
    let offset = 0, error = null;
    try {
        for (const cut of cuts) {
            chunks.push(points(decoder.decode(bytes.subarray(offset, cut), {stream: true})));
            offset = cut;
        }
        chunks.push(points(decoder.decode(bytes.subarray(offset))));
    } catch (caught) { error = caught.name; }
    // A fatal failure must not poison the next independent decode call.
    let reset;
    try { reset = {points: points(decoder.decode(new Uint8Array([65])))}; }
    catch (caught) { reset = {error: caught.name}; }
    return {chunks, error, reset};
}

export default {
    fetch(request) {
        const url = new URL(request.url);
        if (url.pathname.endsWith('/vectors')) {
            const observations = [];
            for (const label of ['gb18030', 'gbk', 'GB_2312-80', 'x-gbk']) {
                for (const {id, bytes: input} of cases) {
                    const bytes = new Uint8Array(input);
                    for (const fatal of [false, true]) {
                        const cuts = [[], ...Array.from({length: bytes.length + 1}, (_, i) => [i]),
                            Array.from({length: bytes.length}, (_, i) => i + 1)];
                        observations.push({label, id, fatal, runs: cuts.map(cut => ({cuts: cut, ...observe(label, bytes, fatal, cut)}))});
                    }
                }
            }
            return new Response(JSON.stringify(observations), {headers: {'content-type': 'application/json'}});
        }
        if (url.pathname.endsWith('/sweep')) {
            const width = Number(url.searchParams.get('width'));
            const start = Number(url.searchParams.get('start'));
            const count = Number(url.searchParams.get('count'));
            const limit = width === 4 ? 1587600 : width === 2 ? 23940 : 0;
            if (!Number.isInteger(start) || !Number.isInteger(count) || start < 0 ||
                count < 1 || count > 8192 || start + count > limit) return new Response('bad range', {status: 400});
            const bytes = new Uint8Array(count * width);
            for (let i = 0; i < count; ++i) {
                const pointer = start + i;
                if (width === 4) bytes.set(fourBytes(pointer), i * 4);
                else {
                    const trail = pointer % 190;
                    bytes[i * 2] = Math.floor(pointer / 190) + 0x81;
                    bytes[i * 2 + 1] = trail + (trail < 63 ? 0x40 : 0x41);
                }
            }
            const codePoints = points(new TextDecoder('gb18030').decode(bytes));
            if (codePoints.length !== count) return new Response(`wrong output length: ${codePoints.length}`, {status: 500});
            const buffer = new ArrayBuffer(count * 4), view = new DataView(buffer);
            for (let i = 0; i < count; ++i) view.setUint32(i * 4, codePoints[i], true);
            return new Response(buffer, {headers: {'content-type': 'application/octet-stream'}});
        }
        return new Response('unknown observation', {status: 404});
    },
};
