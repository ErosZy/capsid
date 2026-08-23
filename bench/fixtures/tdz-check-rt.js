// A3 negative control for the density lattice: real TDZ exposure that
// the analysis must NOT call reducible. `let x;` stores undefined at the
// declaration point, so reads AFTER the declaration are always safe —
// genuine TDZ exposure is a read/write BEFORE the binding's init store:
// the binding is hoisted (set_loc_uninitialized at function top) while
// the access sits earlier in the instruction stream. Both accesses below
// throw ReferenceError at runtime; their density verdicts must stay
// below 100%, which is what proves the slot-init analysis is not
// vacuously accepting everything. Compile-only fixture: it throws.
export default {
    fetch() {
        let s = 0;
        s += x;         // get_loc_check x: x is still UNINIT — throws
        let x;          // init store (undefined) comes after the read
        y = 7;          // put_loc_check y: y is still UNINIT — throws
        let y;
        let z = s + 1;  // control site: z's checks are genuinely reducible
        z += 1;
        return new Response(String(z));
    },
};
