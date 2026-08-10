// Managed-soak application fixture (WP-09 §13.6). Every dimension of the
// 24h/72h soak drives one endpoint:
//
//   /marker              -> env APP_SOAK_MARKER (secret-rotation evidence)
//   /echo                -> request body echo (data-plane liveness)
//   /slow?ms=N           -> N ms timer then "slow-ok" (cancel/timeout,
//                           queue-fairness target)
//   /sse                 -> text/event-stream, 3 ticks with gaps, then end
//   /big                 -> 4 MiB uniform fill (slow-client dimension)
//
// The marker is read from capsid:env, which the Host populates from the
// secret root (permissions.env.valueFrom in capsid.json); rotating the
// secret file and redeploying must change the served marker.

import { env } from 'capsid:env';

const FILL_BYTE = 0x53;
const FILL_SIZE = 4 * 1024 * 1024;

export default {
    async fetch(request) {
        const url = new URL(request.url);
        switch (url.pathname) {
            case '/marker': {
                const marker = env.get('APP_SOAK_MARKER');
                return new Response(marker === null ? 'no-env' : marker, {
                    headers: { 'content-type': 'text/plain' },
                });
            }
            case '/echo': {
                const body = await request.text();
                return new Response('echo:' + body, {
                    headers: { 'content-type': 'text/plain' },
                });
            }
            case '/slow': {
                const ms = Math.min(Number(url.searchParams.get('ms') || 200), 5000);
                await new Promise((resolve) => setTimeout(resolve, ms));
                return new Response('slow-ok', {
                    headers: { 'content-type': 'text/plain' },
                });
            }
            case '/sse': {
                const stream = new ReadableStream({
                    type: 'bytes',
                    async pull(controller) {
                        for (let i = 0; i < 3; i += 1) {
                            controller.enqueue(
                                new TextEncoder().encode('data: tick-' + i + '\n\n'));
                            await new Promise((resolve) => setTimeout(resolve, 100));
                        }
                        controller.close();
                    },
                });
                return new Response(stream, {
                    headers: {
                        'content-type': 'text/event-stream',
                        'cache-control': 'no-cache',
                    },
                });
            }
            case '/big': {
                return new Response(new Uint8Array(FILL_SIZE).fill(FILL_BYTE), {
                    headers: { 'content-type': 'application/octet-stream' },
                });
            }
            default:
                return new Response('soak-ok', {
                    headers: { 'content-type': 'text/plain' },
                });
        }
    },
};
