// Cascade fixture (the plan's Step-0 cascade anchor, committed): folded
// constant chains feeding ===/!==/</ comparisons in one conjunction, so
// every constant read happens before any accumulator mutation (a
// mutation barrier-wipes the slots between reads). The chains fold via
// P2 (cross-BB propagation) + P3.1 (binop folds); the comparisons
// themselves stay live (P3.2/P3.6 were trimmed by G4 for <1%
// attribution), which keeps the fixture a stress test of the deployed
// pipeline's core. Both sides of every branch must match the source
// exactly (acc = 55 × 200000 = 11000000).
export default {
    fetch() {
        let acc = 0;
        for (let i = 0; i < 200000; i++) {
            const a = 12345;
            const b = a * 3 + 7;            // 37042
            const c = (b << 2) ^ 0x2a5;     // 147565
            const d = c + 999 - (b >> 3);   // 143934
            const e = (d * 5) & 0xffff;     // 719670 & 65535 = 64310
            if (b === 37042 && c === 147565 && d !== 143935 &&
                e > 50000 && e < 70000 && a + b + c + d === 340886) {
                acc += 55;
            }
        }
        return new Response(String(acc));
    },
};
