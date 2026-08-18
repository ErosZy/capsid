// Hono bench app — payloads/routes mirror the slim, flask and fastapi
// fixtures. Legacy routes cover 1k/8k/16k/32k/64k; matrix-<kind>-<label>
// routes cover 1k/4k/8k/16k/32k/64k with exact Content-Length semantics.
import { Hono } from 'hono';

const app = new Hono();

const pad = size => 'x'.repeat(size);
const J1K = JSON.stringify({ status: 'ok', app: 'hono', pad: pad(1024) });
const J8K = JSON.stringify({ status: 'ok', app: 'hono', pad: pad(8192) });
const J16K = JSON.stringify({ status: 'ok', app: 'hono', pad: pad(16384) });
const J32K = JSON.stringify({ status: 'ok', app: 'hono', pad: pad(32768) });
const J64K = JSON.stringify({ status: 'ok', app: 'hono', pad: pad(65536) });

const MATRIX_SIZES = { '1k': 1024, '4k': 4096, '8k': 8192, '16k': 16384, '32k': 32768, '64k': 65536 };
const MATRIX_PADS = Object.fromEntries(Object.entries(MATRIX_SIZES)
    .map(([ label, size ]) => [ label, pad(size - 11) ]));

const json = body => new Response(body, {
    headers: { 'content-type': 'application/json' },
});
const bytes = size => new Response(
    new Uint8Array(size).fill(0x61),
    { headers: { 'content-type': 'application/octet-stream' } },
);
const stream = size => new Response(
    new ReadableStream({
        type: 'bytes',
        pull(controller) {
            // Frozen stream contract: b*⌊n/3⌋ c*⌊n/3⌋ d*(rest) — the
            // loadgen verify cases pin these byte positions.
            const third = Math.floor(size / 3);
            controller.enqueue(new Uint8Array(third).fill(0x62));
            controller.enqueue(new Uint8Array(third).fill(0x63));
            controller.enqueue(new Uint8Array(size - 2 * third).fill(0x64));
            controller.close();
        },
    }),
    { headers: { 'content-type': 'application/octet-stream' } },
);

app.get('/bench/json', () => json(J1K));
app.get('/bench/json8k', () => json(J8K));
app.get('/bench/json16k', () => json(J16K));
app.get('/bench/json32k', () => json(J32K));
app.get('/bench/json64k', () => json(J64K));
app.get('/bench/bytes', () => bytes(1024));
app.get('/bench/bytes8k', () => bytes(8192));
app.get('/bench/bytes16k', () => bytes(16384));
app.get('/bench/bytes32k', () => bytes(32768));
app.get('/bench/bytes64k', () => bytes(65536));
app.get('/bench/stream', () => stream(1024));
app.get('/bench/stream8k', () => stream(8192));
app.get('/bench/stream16k', () => stream(16384));
app.get('/bench/stream32k', () => stream(32768));
app.get('/bench/stream64k', () => stream(65536));

const matrixJson = (c, label) => {
    const size = MATRIX_SIZES[label];
    const body = JSON.stringify({ data: MATRIX_PADS[label] });
    return new Response(body, {
        headers: {
            'content-type': 'application/json',
            'content-length': String(size),
        },
    });
};

const matrixBytes = (c, label) => {
    const size = MATRIX_SIZES[label];
    return new Response(new Uint8Array(size).fill(0x62), {
        headers: {
            'content-type': 'application/octet-stream',
            'content-length': String(size),
        },
    });
};

const matrixStream = (c, label) => {
    const size = MATRIX_SIZES[label];
    const chunk = new Uint8Array(4096).fill(0x73);
    let offset = 0;
    const body = new ReadableStream({
        type: 'bytes',
        pull(controller) {
            if (offset >= size) {
                controller.close();
                return;
            }
            const take = Math.min(4096, size - offset);
            offset += take;
            controller.enqueue(chunk.slice(0, take));
        },
    });
    return new Response(body, { headers: { 'content-type': 'application/octet-stream' } });
};

app.get('/bench/matrix-json-1k', c => matrixJson(c, '1k'));
app.get('/bench/matrix-json-4k', c => matrixJson(c, '4k'));
app.get('/bench/matrix-json-8k', c => matrixJson(c, '8k'));
app.get('/bench/matrix-json-16k', c => matrixJson(c, '16k'));
app.get('/bench/matrix-json-32k', c => matrixJson(c, '32k'));
app.get('/bench/matrix-json-64k', c => matrixJson(c, '64k'));
app.get('/bench/matrix-bytes-1k', c => matrixBytes(c, '1k'));
app.get('/bench/matrix-bytes-4k', c => matrixBytes(c, '4k'));
app.get('/bench/matrix-bytes-8k', c => matrixBytes(c, '8k'));
app.get('/bench/matrix-bytes-16k', c => matrixBytes(c, '16k'));
app.get('/bench/matrix-bytes-32k', c => matrixBytes(c, '32k'));
app.get('/bench/matrix-bytes-64k', c => matrixBytes(c, '64k'));
app.get('/bench/matrix-stream-1k', c => matrixStream(c, '1k'));
app.get('/bench/matrix-stream-4k', c => matrixStream(c, '4k'));
app.get('/bench/matrix-stream-8k', c => matrixStream(c, '8k'));
app.get('/bench/matrix-stream-16k', c => matrixStream(c, '16k'));
app.get('/bench/matrix-stream-32k', c => matrixStream(c, '32k'));
app.get('/bench/matrix-stream-64k', c => matrixStream(c, '64k'));

app.get('/fixed', () => new Response(
    new Uint8Array(1024).fill(0x78),
    { headers: { 'content-type': 'application/octet-stream' } },
));

export default app;
