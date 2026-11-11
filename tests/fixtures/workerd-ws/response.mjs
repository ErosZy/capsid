// Response construction and real Upgrade commit/coupling are distinct stages.
const jobs = new Map();
let prepared;

function constructors() {
  const pair = new WebSocketPair();
  const own = (object, key, value) => {
    Object.defineProperty(object, key, { value });
    return object;
  };
  const params = new URLSearchParams('a=b');
  params.toString = () => '';
  const emptyParams = new URLSearchParams();
  emptyParams.toString = () => 'a=b';
  const detached = new ArrayBuffer(1);
  const detachedView = new Uint8Array(detached);
  const detachedDataView = new DataView(detached);
  structuredClone(detached, { transfer: [detached] });
  const withSocket = { status: 101, webSocket: pair[0] };
  const inputs = [
    ['missing', null, { status: 101 }],
    ['wrong-object', null, { status: 101, webSocket: {} }],
    ['wrong-status', null, { status: 200, webSocket: pair[0] }],
    ['wrong-status-object', null, { status: 200, webSocket: {} }],
    ['wrong-status-100', null, { status: 100, webSocket: pair[0] }],
    ['wrong-status-201', null, { status: 201, webSocket: pair[0] }],
    ['wrong-status-500', null, { status: 500, webSocket: pair[0] }],
    ['default-status', null, { webSocket: pair[0] }],
    ['body-empty', '', withSocket],
    ['body-text', 'x', withSocket],
    ['body-zero', 0, withSocket],
    ['body-bytes-empty', new Uint8Array(), withSocket],
    ['body-buffer-empty', new ArrayBuffer(0), withSocket],
    ['body-blob-empty', new Blob([]), withSocket],
    ['body-blob-empty-type', new Blob([], { type: 'text/x-fixture' }), withSocket],
    ['body-empty-explicit-type', '', { ...withSocket, headers: { 'content-type': 'text/x-explicit' } }],
    ['body-params-empty', new URLSearchParams(), withSocket],
    ['body-boxed-empty', new String(''), withSocket],
    ['body-stream-empty', new ReadableStream({ start(c) { c.close(); } }), withSocket],
    ['body-dataview-empty', new DataView(new ArrayBuffer(0)), withSocket],
    ['body-view-empty-offset', new Uint8Array(new ArrayBuffer(4), 2, 0), withSocket],
    ['body-buffer-spoof-empty', own(new ArrayBuffer(1), 'byteLength', 0), withSocket],
    ['body-view-spoof-empty', own(new Uint8Array([1]), 'byteLength', 0), withSocket],
    ['body-blob-spoof-empty', own(new Blob(['x']), 'size', 0), withSocket],
    ['body-params-spoof-empty', params, withSocket],
    ['body-params-spoof-nonempty', emptyParams, withSocket],
    ['body-detached-buffer', detached, withSocket],
    ['body-detached-view', detachedView, withSocket],
    ['body-detached-dataview', detachedDataView, withSocket],
    ['body-undefined', undefined, withSocket],
    ['unaccepted', null, withSocket],
  ];
  const result = {};
  for (const [name, body, init] of inputs) {
    try {
      const response = new Response(body, init);
      result[name] = { status: response.status, statusText: response.statusText,
        body: response.body === null, bodyUsed: response.bodyUsed,
        contentType: response.headers.get('content-type'), endpoint: response.webSocket === pair[0] };
    } catch (error) {
      result[name] = { error: error.name, message: error.message };
    }
  }
  const response = new Response(null, withSocket);
  try { response.clone(); result.clone = { accepted: true }; }
  catch (error) { result.clone = { error: error.name, message: error.message }; }
  return result;
}

function upgradeResponse(socket) {
  return new Response(null, { status: 101, webSocket: socket,
    headers: { 'x-response-fixture': 'yes', 'sec-websocket-protocol': 'chat' } });
}

export default {
  async fetch(request) {
    const url = new URL(request.url);
    const id = url.searchParams.get('id');
    const kind = url.searchParams.get('kind');
    if (url.pathname.endsWith('/construct')) return Response.json(constructors());
    if (url.pathname.endsWith('/state')) {
      const job = jobs.get(id);
      return Response.json({ accepted: job.accepted, events: job.events });
    }
    if (url.pathname.endsWith('/release')) {
      const job = jobs.get(id);
      const release = job.release;
      job.release = null;
      release();
      return new Response('released');
    }
    if (url.pathname.endsWith('/prepare')) {
      const pair = new WebSocketPair();
      pair[1].accept();
      prepared = upgradeResponse(pair[0]);
      return new Response('prepared');
    }
    if (!url.pathname.endsWith('/upgrade')) return new Response('healthy');
    if (kind === 'prepared') {
      const response = prepared;
      prepared = null;
      return response;
    }
    if (kind === 'outbound-open' || kind === 'outbound-connecting') {
      const socket = new WebSocket(url.searchParams.get('target'), ['chat']);
      if (kind === 'outbound-open') {
        await new Promise((resolve, reject) => {
          socket.onopen = resolve;
          socket.onerror = () => reject(new Error('fixture upstream did not open'));
        });
      }
      return upgradeResponse(socket);
    }
    const pair = new WebSocketPair();
    const server = pair[1];
    // Retain observations, not endpoints: live invalid local pairs would keep
    // the reference's independent local I/O alive after this failed coupling.
    const job = { events: [], accepted: false };
    jobs.set(id, job);
    if (kind === 'delayed' || kind === 'delayed-close') {
      new Promise(resolve => { job.release = resolve; }).then(() => {
        server.accept();
        job.accepted = true;
      });
    } else {
      server.accept();
      job.accepted = true;
    }
    server.onmessage = event => {
      job.events.push({ type: 'message', data: event.data });
      if (kind === 'delayed-close') return;
      server.send('reply:' + event.data);
      if (kind !== 'hold') server.close(1000, 'done');
    };
    server.onclose = event => job.events.push({ type: 'close', code: event.code, clean: event.wasClean });
    if (kind === 'both') pair[0].accept();
    if (kind === 'closed') server.close(1000, 'local');
    return upgradeResponse(['accepted', 'closed'].includes(kind) ? server : pair[0]);
  },
};
