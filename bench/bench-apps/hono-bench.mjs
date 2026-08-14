// Hono bench app — payloads/routes mirror the slim, flask and fastapi
// fixtures (json/bytes/stream at 1k/8k/16k/32k). JSON documents are
// precomputed strings served via new Response (no per-request
// re-serialization); bytes/stream bodies are constructed per request.
import { Hono } from 'hono';

const app = new Hono();

const pad = size => 'x'.repeat(size);
const J1K = JSON.stringify({ status: 'ok', app: 'hono', pad: pad(1024) });
const J8K = JSON.stringify({ status: 'ok', app: 'hono', pad: pad(8192) });
const J16K = JSON.stringify({ status: 'ok', app: 'hono', pad: pad(16384) });
const J32K = JSON.stringify({ status: 'ok', app: 'hono', pad: pad(32768) });

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
app.get('/bench/bytes', () => bytes(1024));
app.get('/bench/bytes8k', () => bytes(8192));
app.get('/bench/bytes16k', () => bytes(16384));
app.get('/bench/bytes32k', () => bytes(32768));
app.get('/bench/stream', () => stream(1024));
app.get('/bench/stream8k', () => stream(8192));
app.get('/bench/stream16k', () => stream(16384));
app.get('/bench/stream32k', () => stream(32768));
app.get('/fixed', () => new Response(
    new Uint8Array(1024).fill(0x78),
    { headers: { 'content-type': 'application/octet-stream' } },
));

export default app;
