// Shared Capsid/workerd fixture for the four frozen QPS workloads.
// The managed Host removes the application prefix; workerd sees it intact.
const bodies = new Map([
  ['/bench/matrix-json-1k', JSON.stringify({ data: 'x'.repeat(1024 - 11) })],
  ['/bench/matrix-json-16k', JSON.stringify({ data: 'x'.repeat(16384 - 11) })],
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

export default {
  fetch(request) {
    const url = new URL(request.url);
    const path = url.pathname.replace(/^\/@capsid\/orders(?=\/)/, '');
    if (bodies.has(path))
      return new Response(bodies.get(path), { headers: { 'content-type': 'application/json' } });
    if (path === '/bench/stream')
      return new Response(
        new ReadableStream({
          start(controller) {
            for (const part of thirds) controller.enqueue(part);
            controller.close();
          },
        }),
        { headers: { 'content-type': 'application/octet-stream' } },
      );
    if (path === '/bench/dynamic-api') {
      const trace = request.headers.get('x-trace') || '';
      const accept = request.headers.get('accept') || '';
      const agent = request.headers.get('user-agent') || '';
      return buildDynamicApi(url, trace, accept, agent).then(
        ({ checksum, body }) =>
          new Response(body, {
            headers: {
              'content-type': 'application/json',
              'cache-control': 'no-store',
              'x-bench-checksum': checksum,
            },
          }),
      );
    }
    return new Response('unknown workload', { status: 404 });
  },
};
