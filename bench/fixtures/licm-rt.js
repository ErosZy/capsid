// Loop-invariant slot copy: the (get_loc src; put_loc dst) pair in the
// loop body copies a value that never changes, so P13' hoists the pair
// into the pre-header. dst is written by the loop on every iteration
// (fresh value), so the hoisted store is observable only through the
// in-loop reads, which all see the same value. acc = 7 x 200000.
export default {
    fetch() {
        const src = 7;
        let dst = 0;
        let acc = 0;
        for (let i = 0; i < 200000; i++) {
            dst = src;
            acc += dst;
        }
        return new Response(String(acc));
    },
};
