// Synchronous-response fixture for the worker IPC end-after-response test:
// the fetch handler returns a Response without any await, so the Runtime can
// complete the response (and erase the request) immediately after the request
// head frame, before a separately-flushed request-end frame arrives.
export default {
    fetch() {
        return new Response('sync-ok', {
            headers: { 'content-type': 'text/plain' },
        });
    },
};
