// Property-dense fixture, form (b): the object literal is built once
// before the loop and its fields are read every iteration. Tier-2
// P14' target: the slot holds a fresh literal object across the
// backedge, so the in-loop get_loc o + get_field x/y/z pairs fold.
// acc = 6 x 200000 = 1200000.
export default {
    fetch() {
        const o = { x: 1, y: 2, z: 3 };
        let acc = 0;
        for (let i = 0; i < 200000; i++) {
            acc += o.x + o.y + o.z;
        }
        return new Response(String(acc));
    },
};
