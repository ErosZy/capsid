// Array-dense fixture: 40x40 matrix multiply (three loops, row/column
// indexing). get/put_array_el are opaque to the slot lattice, so almost
// nothing folds here — the honest mid-ceiling case (P6 shrink + loop
// peepholes only).
export default {
    fetch() {
        const N = 40;
        const A = [];
        const B = [];
        for (let i = 0; i < N; i++) {
            const rowA = [];
            const rowB = [];
            for (let j = 0; j < N; j++) {
                rowA.push((i * 7919 + j * 104729) % 100000);
                rowB.push((i * 314159 + j * 271828) % 100000);
            }
            A.push(rowA);
            B.push(rowB);
        }
        const C = [];
        for (let i = 0; i < N; i++) {
            const row = [];
            for (let j = 0; j < N; j++) {
                let s = 0;
                for (let k = 0; k < N; k++) {
                    s = (s + A[i][k] * B[k][j]) % 1000000007;
                }
                row.push(s);
            }
            C.push(row);
        }
        let sum = 0;
        for (let i = 0; i < N; i++) {
            for (let j = 0; j < N; j++) {
                sum = (sum + C[i][j]) % 1000000007;
            }
        }
        return new Response(String(sum));
    },
};
