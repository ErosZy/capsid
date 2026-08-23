// Regression: P16 slot liveness must merge the jump-taken edge of a
// mid-block conditional. The true-tail store `x = 1` (after the
// mid-block jump) kills slot x in the linear backward walk, so the
// marker and init store look dead — but the false path reads x at the
// join and needs the 7. choose(false) must return 7, never 1. Also
// exercises the arg/loc index separation: get_arg0 is a different frame
// store from loc0 and must never fold to a local's value.
export default {
    fetch() {
        return new Response(String(choose(false)));
    },
};
function choose(c) {
    let x = 7;
    if (c) { x = 1; }
    return x;
}
