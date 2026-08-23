// Constant-branch fixture: a loop-invariant variable guards a
// conditional inside the hot loop. P2 proves N constant and P3.1 folds
// the comparison to push_true, but nothing folds the conditional itself
// (P3.6 was G4-trimmed); tier-2 P10 SCCP dead edges must turn the
// if_true into a goto and delete the dead else block. acc = 7 x 200000
// = 1400000.
export default {
    fetch() {
        let acc = 0;
        const N = 1000;
        for (let i = 0; i < 200000; i++) {
            if (N > 500) {
                acc += 7;
            } else {
                acc += 3;
            }
        }
        return new Response(String(acc));
    },
};
