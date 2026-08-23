// Property-dense fixture, form (a): a fresh object literal constructed
// inside the hot loop and its fields read immediately (loop-internal
// construction + get_field). Tier-2 P14 target: the literal builds as
// OP_object + push_const(atom) + push + put_field sequences, and the
// slot read get_loc o + get_field x folds to the pushed constant, after
// which the dead construction is removable. acc = 6 x 200000 = 1200000.
export default {
    fetch() {
        let acc = 0;
        for (let i = 0; i < 200000; i++) {
            const o = { x: 1, y: 2, z: 3 };
            acc += o.x + o.y + o.z;
        }
        return new Response(String(acc));
    },
};
