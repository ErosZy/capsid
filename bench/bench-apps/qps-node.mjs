// Shared Node.js / Node (--jitless) fixture for the three frozen QPS workloads.
// Uses built-in node:http on a single event loop with HTTP/1.1 keep-alive.
import { createServer } from 'node:http';

const bodies = new Map([
  ['/bench/matrix-json-1k', Buffer.from(JSON.stringify({ data: 'x'.repeat(1024 - 11) }))],
  ['/bench/matrix-json-16k', Buffer.from(JSON.stringify({ data: 'x'.repeat(16384 - 11) }))],
]);
const encoder = new TextEncoder();
const thirds = ['b'.repeat(341), 'c'.repeat(341), 'd'.repeat(342)].map((s) => encoder.encode(s));

const port = Number(process.argv[2] || 18990);

const server = createServer((req, res) => {
  const path = new URL(req.url || '/', 'http://127.0.0.1').pathname.replace(
    /^\/@capsid\/orders(?=\/)/,
    '',
  );
  const body = bodies.get(path);
  if (body !== undefined) {
    res.writeHead(200, {
      'content-type': 'application/json',
      'content-length': body.byteLength,
    });
    res.end(body);
    return;
  }
  if (path === '/bench/stream') {
    res.writeHead(200, { 'content-type': 'application/octet-stream' });
    for (const part of thirds) res.write(part);
    res.end();
    return;
  }
  res.writeHead(404, { 'content-type': 'text/plain; charset=utf-8' });
  res.end('unknown workload');
});

server.keepAliveTimeout = 60_000;
server.headersTimeout = 65_000;
server.listen(port, '127.0.0.1', () => {
  console.log('READY');
});
