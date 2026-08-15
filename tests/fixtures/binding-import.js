// Zero-binding regression fixture: the App imports a Binding that no
// LOAD_BINDING frame declared. The worker must fail the module resolution
// at bundle load (kError before READY) and must never lazily create a
// Binding Runtime.
import mongo from 'capsid:binding/mongo';

export default {
    async fetch() {
        return new Response('binding import fixture reached fetch');
    },
};
