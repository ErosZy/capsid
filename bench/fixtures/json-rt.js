// Object/JSON-dense fixture: builds an object graph, stringifies and
// parses it per iteration. Property access and stringify/parse are all
// opaque — the honest low-ceiling case.
export default {
    fetch() {
        let acc = 0;
        for (let i = 0; i < 500; i++) {
            const obj = { a: i, b: { c: [i, i + 1, i * 2], d: 'x' + i } };
            const text = JSON.stringify(obj);
            const parsed = JSON.parse(text);
            acc = (acc + parsed.a + parsed.b.c[2]) % 1000000007;
        }
        return new Response(String(acc));
    },
};
