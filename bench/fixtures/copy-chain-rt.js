// Copy-chain fixture: a loop-carried (non-constant) value aliased
// through intermediate slots. Tier-2 P11 target: get_loc w -> get_loc v
// renames make the intermediate [get_loc; put_loc] pairs dead, so the
// dead stores vanish. acc = sum(i*2) = 2 * (199999*200000/2) = 39999800000.
export default {
    fetch() {
        let acc = 0;
        for (let i = 0; i < 200000; i++) {
            const v = i * 2;
            const w = v;
            const u = w;
            acc += u;
        }
        return new Response(String(acc));
    },
};
