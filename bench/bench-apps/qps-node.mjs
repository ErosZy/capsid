// Shared Node.js / Node (--jitless) fixture for the four frozen QPS workloads.
// Uses built-in node:http on a single event loop with HTTP/1.1 keep-alive.
import { createServer } from 'node:http';

const bodies = new Map([
  ['/bench/matrix-json-1k', Buffer.from(JSON.stringify({ data: 'x'.repeat(1024 - 11) }))],
  ['/bench/matrix-json-16k', Buffer.from(JSON.stringify({ data: 'x'.repeat(16384 - 11) }))],
]);
const encoder = new TextEncoder();
const thirds = ['b'.repeat(341), 'c'.repeat(341), 'd'.repeat(342)].map((s) => encoder.encode(s));

function fnv1a32Hex(str) {
  let h = 0x811c9dc5;
  for (let i = 0; i < str.length; i++) {
    h = Math.imul(h ^ str.charCodeAt(i), 0x01000193);
  }
  return (h >>> 0).toString(16).padStart(8, '0');
}

async function buildDynamicApi(url, trace, accept, agent) {
  const sp = url.searchParams;
  const tenant = sp.get('tenant') || 'default';
  const region = sp.get('region') || 'us-east';
  const sort = sp.get('sort') === 'asc' ? 'asc' : 'desc';
  const currency = sp.get('currency') || 'USD';
  const limitRaw = parseInt(sp.get('limit') || '8', 10);
  const offsetRaw = parseInt(sp.get('offset') || '0', 10);
  const limit = Number.isFinite(limitRaw) && limitRaw > 0 && limitRaw <= 32 ? limitRaw : 8;
  const offset = Number.isFinite(offsetRaw) && offsetRaw >= 0 && offsetRaw <= 10000 ? offsetRaw : 0;
  await Promise.resolve();
  const items = new Array(limit);
  let subtotalCents = 0;
  for (let i = 0; i < limit; i++) {
    const rank = sort === 'desc' ? offset + limit - 1 - i : offset + i;
    const qty = (rank % 4) + 1;
    const unitCents = 1250 + ((rank * 137) % 900);
    const lineCents = qty * unitCents;
    subtotalCents += lineCents;
    items[i] = {
      id: `${tenant}-${rank}`,
      sku: `SKU-${1000 + rank}`,
      qty,
      unitCents,
      lineCents,
      tags: [region, rank % 2 === 0 ? 'priority' : 'standard'],
    };
  }
  const taxCents = Math.floor((subtotalCents * 825 + 5000) / 10000);
  const totalCents = subtotalCents + taxCents;
  const checksum = fnv1a32Hex(
    `${tenant}|${region}|${trace}|${accept}|${agent}|${limit}|${offset}|${sort}|${currency}|${totalCents}`,
  );
  return {
    checksum,
    body: JSON.stringify({
      ok: true,
      tenant,
      region,
      currency,
      sort,
      pagination: { limit, offset, count: items.length },
      request: { trace, accept, agent },
      totals: { subtotalCents, taxCents, totalCents, checksum },
      items,
    }),
  };
}

const port = Number(process.argv[2] || 18990);

const server = createServer((req, res) => {
  const url = new URL(req.url || '/', 'http://127.0.0.1');
  const path = url.pathname.replace(/^\/@capsid\/orders(?=\/)/, '');
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
  if (path === '/bench/dynamic-api') {
    const trace = String(req.headers['x-trace'] || '');
    const accept = String(req.headers['accept'] || '');
    const agent = String(req.headers['user-agent'] || '');
    buildDynamicApi(url, trace, accept, agent).then(({ checksum, body: payload }) => {
      const buf = Buffer.from(payload);
      res.writeHead(200, {
        'content-type': 'application/json',
        'content-length': buf.byteLength,
        'cache-control': 'no-store',
        'x-bench-checksum': checksum,
      });
      res.end(buf);
    });
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
