// Bun fixture for the three frozen QPS workloads using built-in Bun.serve.
const bodies = new Map([
  ['/bench/matrix-json-1k', JSON.stringify({ data: 'x'.repeat(1024 - 11) })],
  ['/bench/matrix-json-16k', JSON.stringify({ data: 'x'.repeat(16384 - 11) })],
]);
const encoder = new TextEncoder();
const thirds = ['b'.repeat(341), 'c'.repeat(341), 'd'.repeat(342)].map((s) => encoder.encode(s));

const port = Number(process.argv[2] || 18990);

Bun.serve({
  hostname: '127.0.0.1',
  port,
  fetch(request) {
    const path = new URL(request.url).pathname.replace(/^\/@capsid\/orders(?=\/)/, '');
    if (bodies.has(path)) {
      return new Response(bodies.get(path), { headers: { 'content-type': 'application/json' } });
    }
    if (path === '/bench/stream') {
      return new Response(
        new ReadableStream({
          start(controller) {
            for (const part of thirds) controller.enqueue(part);
            controller.close();
          },
        }),
        { headers: { 'content-type': 'application/octet-stream' } },
      );
    }
    return new Response('unknown workload', { status: 404 });
  },
});

console.log('READY');
