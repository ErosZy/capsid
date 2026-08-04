// Queue-saturation activity fix fixture.
//
// Every response body is a uniform byte pattern so the C ABI test can
// verify content byte-for-byte. Paths:
//   /chunk-20000   -> 20000 bytes of 0x53
//   /chunk-8193    -> 8193 bytes of 0x51
//   /chunk-65537   -> 65537 bytes of 0x52
//   /chunk-65536   -> 65536 bytes of 0x52
//   /small         -> "small-ok" (8 bytes)
//   /error-after-write -> writes 20000 bytes then throws; the runtime
//                         must still deliver a terminal promptly
//   /error-immediate -> throws before any body write
//
// The byte pattern is size-dependent per path so cross-contamination
// between concurrent requests is detectable.

const fill = (size, byte) => new Uint8Array(size).fill(byte);

// Counts cancel-bridge deliveries to the hang request; the runtime must
// cancel exactly once even while the output queue stays saturated.
let hangAbortCount = 0;

export default {
    async fetch(request) {
        const url = new URL(request.url);
        switch (url.pathname) {
            case '/chunk-20000':
                return new Response(fill(20000, 0x53));
            case '/chunk-8193':
                return new Response(fill(8193, 0x51));
            case '/chunk-65537':
                return new Response(fill(65537, 0x52));
            case '/chunk-65536':
                return new Response(fill(65536, 0x52));
            case '/small':
                return new Response(new Uint8Array([
                    115, 109, 97, 108, 108, 45, 111, 107,
                ]));
            case '/error-after-write':
                return new Response(new ReadableStream({
                    type: 'bytes',
                    pull(controller) {
                        controller.enqueue(fill(20000, 0x54));
                        controller.error(new Error('boom-after-write'));
                    },
                }));
            case '/error-immediate':
                throw new Error('boom-immediate');
            case '/error-huge-message':
                // > 4 KiB error payload: must still deliver a terminal
                // in a 4 KiB queue (truncated, never wedged).
                throw new Error('E'.repeat(8192));
            case '/error-after-write-real':
                // Writes a large chunk, then the stream errors: the
                // request must still receive exactly one terminal.
                return new Response(new ReadableStream({
                    type: 'bytes',
                    pull(controller) {
                        controller.enqueue(fill(20000, 0x54));
                        controller.error(new Error('boom-after-write'));
                    },
                }));
            case '/hang':
                // Never resolves until the deadline's cancel bridge
                // aborts the request signal; counts deliveries.
                await new Promise(resolve => {
                    request.signal.addEventListener('abort', () => {
                        hangAbortCount += 1;
                        resolve();
                    });
                });
                break;
            case '/hang-count':
                return new Response(fill(4, 0x30 + (hangAbortCount % 10)));
            case '/mutate-after-write': {
                // Plain (non-bytes) stream: the chunk stays writable
                // after enqueue, so the application can mutate the
                // array after the write call. The response must carry
                // the bytes as they were at the write call (0x55) —
                // call-time snapshot semantics, never a later mutation.
                const bytes = fill(20000, 0x55);
                return new Response(new ReadableStream({
                    pull(controller) {
                        controller.enqueue(bytes);
                        setTimeout(() => {
                            bytes.fill(0xaa);
                        }, 0);
                        controller.enqueue(fill(20000, 0x66));
                        controller.close();
                    },
                }));
            }
            default:
                return new Response('unknown', { status: 404 });
        }
    },
};
