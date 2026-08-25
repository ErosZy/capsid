// Arithmetic-dense fixture: straight-line constant arithmetic chains in a
// hot loop. Every body iteration recomputes constants from fresh locals
// (P2/P3 fold targets); only the counter and accumulator are live across
// the backedge. This is the highest-foldability shape the AOT rewriter
// can produce on — G3's static ceiling anchor.
export default {
    fetch() {
        let acc = 0;
        for (let i = 0; i < 300000; i++) {
            let a = 12345;
            let b = a * 3 + 7;
            let c = (b << 2) ^ 0x2a5;
            let d = c + 999 - (b >> 3);
            let e = (d * 5) & 0xffff;
            let f = e ^ (c >> 4);
            let g = (f << 1) + 1;
            let h = g * 7 - 3;
            let k = (h ^ 0x1f) | 0;
            let m = k + (d & 255);
            let n = (m * 3) >> 2;
            let p = n ^ (g & 31);
            let q = p + 17;
            let r = (q << 3) & 0x7fffffff;
            let s = r - (m >> 1);
            let t = s ^ (q << 5);
            acc += t;
        }
        return new Response(String(acc));
    },
};
