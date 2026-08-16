// Binding v1 RPC lifecycle regression fixture.  Each route isolates one
// cross-runtime contract so the native worker test can drive terminal and
// cancellation paths through the real protocol.
import mongo from 'capsid:binding/mongo';

export default {
    async fetch(request) {
        const path = new URL(request.url).pathname;
        if (path === '/inspect') {
            return new Response('result:' + await mongo.inspect());
        }
        if (path === '/throw') {
            try {
                await mongo.throwSync();
                return new Response('unexpected-success');
            } catch (error) {
                return new Response('error:' + error.message);
            }
        }
        if (path === '/detach') {
            for (let index = 0; index < 64; ++index) {
                mongo.hang().catch(() => {});
            }
            return new Response('detached');
        }
        if (path === '/probe') {
            try {
                return new Response('probe:' + await mongo.ping());
            } catch (error) {
                return new Response('probe-error:' + error.message);
            }
        }
        if (path === '/await-many') {
            await Promise.all(
                Array.from({ length: 64 }, () => mongo.hang()));
            return new Response('unexpected-hang-settlement');
        }
        if (path === '/dispatch-count') {
            return new Response('count:' + await mongo.dispatchCount());
        }
        if (path === '/abort-status') {
            return new Response('abort:' + await mongo.abortStatus());
        }
        return new Response('unknown route', { status: 404 });
    },
};
