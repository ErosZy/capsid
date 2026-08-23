// Regression: P11 copy propagation must not rename a read inside a loop
// when the copied slot is stored again in the loop body. The loop-back
// edge re-executes the body's stores before the next iteration's read,
// so the linear alias window (store -> read, no store between) does not
// cover the execution order; renaming the read corrupts the value.
// loopCopy(8) must return 13 (y grows by 1 per iteration), never 6 (the
// renamed read pins y to x's initial value and the loop's y-stores
// become dead).
export default {
    fetch() {
        return new Response(String(loopCopy(8)));
    },
};
function loopCopy(n) {
    let x = 5;
    let y = x;      // candidate copy: y = x (get_loc x; put_loc y)
    let i = 0;
    while (i < n) {
        let z = y + 1;  // reads y — must not be renamed to x
        y = z;          // stores y again: later store inside the loop
        i = i + 1;
    }
    return y;       // 5 + n
}
