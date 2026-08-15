// Binding v1 §7.6 end-to-end: the App calls a Host Binding through the
// synthetic capsid:binding facade; the response body carries the Binding's
// return value.
import mongo from 'capsid:binding/mongo';

export default {
    async fetch() {
        const rows = await mongo.find({ collection: 'users' });
        return new Response('result:' + rows);
    },
};
