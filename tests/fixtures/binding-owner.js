// Cross-Binding isolation probe. Each Binding package owns a separate
// QuickJS runtime/context, so values stashed by mongo on its globalThis
// (native handles, the factory log object, global/module-cache markers)
// must be invisible to redis; mongo must still see its own state untouched.
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
