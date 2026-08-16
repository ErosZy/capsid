// Cross-Binding native-handle ownership probe. Both packages share the same
// Binding Runtime global, so JavaScript can pass an object through globalThis;
// the native owner tag must still reject use by the wrong Binding ID.
import mongo from 'capsid:binding/mongo';
import redis from 'capsid:binding/redis';

export default {
    async fetch() {
        const stored = await mongo.store();
        const cross = await redis.use();
        const cleaned = await mongo.cleanup();
        return new Response(`result:${stored}:${cross}:${cleaned}`);
    },
};
