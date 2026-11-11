// Shared Capsid/workerd fixture for the three frozen QPS workloads.
// The managed Host removes the application prefix; workerd sees it intact.
const bodies = new Map([
  ['/bench/matrix-json-1k', JSON.stringify({data: 'x'.repeat(1024 - 11)})],
  ['/bench/matrix-json-16k', JSON.stringify({data: 'x'.repeat(16384 - 11)})],
]);
const encoder = new TextEncoder();
const thirds = ['b'.repeat(341), 'c'.repeat(341), 'd'.repeat(342)].map(s => encoder.encode(s));
export default {
  fetch(request) {
    const path = new URL(request.url).pathname.replace(/^\/@capsid\/orders(?=\/)/, '');
    if (bodies.has(path)) return new Response(bodies.get(path), {headers: {'content-type': 'application/json'}});
    if (path === '/bench/stream') return new Response(new ReadableStream({
      start(controller) {
        for (const part of thirds) controller.enqueue(part);
        controller.close();
      },
    }), {headers: {'content-type': 'application/octet-stream'}});
    return new Response('unknown workload', {status: 404});
  },
};
