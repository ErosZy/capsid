// Repeated-slot-read fixture: the same local read three times with no
// intervening write. Tier-2 P15 GVN target: get_loc v -> dup of the
// first read's value (bytes shrink; dispatch count changes only in
// operand-fetch cost — the honest P15 measurement). acc = 3 * sum(v)
// where v = 2i+1 over 200000 iterations: 3 * (200000 + 2*199999*200000/2)
// = 3 * 40000000000 = 120000000000.
export default {
    fetch() {
        let acc = 0;
        for (let i = 0; i < 200000; i++) {
            const v = i * 2 + 1;
            acc += v + v + v;
        }
        return new Response(String(acc));
    },
};
