// Synthetic ceiling for the runtime-only monomorphic get_field IC. The
// receiver lives for the module lifetime, so every request reads the exact
// same object and shape. This is deliberately paired with prop-hoist-rt
// (fresh receiver per request) and the Hono request mix; a win here alone is
// not sufficient to enable the IC.
const receiver = { x: 1, y: 2, z: 3 };

export default {
    fetch() {
        let acc = 0;
        for (let i = 0; i < 200000; i++) {
            acc += receiver.x + receiver.y + receiver.z;
        }
        return new Response(String(acc));
    },
};
