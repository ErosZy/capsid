// Elysia bench app — payloads/routes mirror hono-bench.mjs so both
// frameworks run the same workload matrix through capsid-host. The app
// field is 'elysia' (same length as 'hono', so payload sizes match).
import { Elysia } from 'elysia';

const pad = size => 'x'.repeat(size);
const J1K = JSON.stringify({ status: 'ok', app: 'elysia', pad: pad(1024) });

const json = body => new Response(body, {
    headers: { 'content-type': 'application/json' },
});
const bytes = size => new Response(
    new Uint8Array(size).fill(0x61),
    { headers: { 'content-type': 'application/octet-stream' } },
);

// gcTime: null is required — see examples/elysia-reference/src/app.js:
// without it the inference GC timer holds a request token open and
// poisons the worker after the first inference.
const app = new Elysia({ sucrose: { gcTime: null } });

app.get('/bench/json', () => json(J1K));
app.get('/bench/bytes', () => bytes(1024));

app.get('/fixed', () => new Response(
    new Uint8Array(1024).fill(0x78),
    { headers: { 'content-type': 'application/octet-stream' } },
));

export default app;
