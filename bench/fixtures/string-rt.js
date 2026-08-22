// String-dense fixture: concatenation loop plus a djb2 hash over the
// result. String building and charCodeAt are call/atom paths — the
// honest low-ceiling case (strings do not participate in the int
// lattice).
export default {
    fetch() {
        let s = '';
        for (let i = 0; i < 2000; i++) {
            s += String.fromCharCode(97 + (i % 26));
        }
        let h = 5381;
        for (let i = 0; i < s.length; i++) {
            h = ((h * 33) ^ s.charCodeAt(i)) >>> 0;
        }
        return new Response(String(h));
    },
};
