// Retain the parent signals in the realm so a forgotten request abort
// listener cannot disappear just because its AbortController becomes garbage.
const controllers = [];

export default {
  async fetch(request) {
    const mode = new URL(request.url).pathname.slice(1);
    if (mode.startsWith('exit-denied-')) {
      const controller = new AbortController();
      controllers.push(controller);
      const init = { signal: controller.signal };
      if (mode !== 'exit-denied-empty') {
        init.method = 'POST';
        init.body = mode === 'exit-denied-known' ? 'payload' : new ReadableStream({
          start(stream) { stream.enqueue(new Uint8Array([1, 2, 3])); stream.close(); },
        });
        if (mode === 'exit-denied-stream') init.duplex = 'half';
      }
      try {
        await fetch('http://not-authorized.invalid:80/', init);
        throw new Error('unexpected fetch success');
      } catch (error) {
        if (!(error instanceof TypeError) || !error.message.includes('is not authorized')) throw error;
      }
      return new Response(mode);
    }
    if (mode === 'exit-gc-cycle') {
      // No reachable native resource: this abandoned Promise cycle should
      // release its captured request context when GC collects the cycle.
      const cycle = {};
      cycle.promise = new Promise(resolve => { cycle.resolve = resolve; });
      cycle.promise.then(() => cycle.promise);
      return new Response(mode);
    }
    // Keep an actual request-owned native resource reachable through GC.
    // Its callback is far outside the bounded process-exit observation.
    const timer = setTimeout(() => { globalThis.retainedTimerRan = true; }, 600000);
    if (mode === 'exit-cleared-timer') clearTimeout(timer);
    await Promise.resolve().then(() => Promise.resolve());
    return new Response(mode);
  },
};
