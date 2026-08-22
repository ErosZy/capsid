// P2 slot-lattice regression fixture: a re-read of an earlier slot after
// a later slot's write, in a loop with a live accumulator. The dataflow
// must keep every slot distinct — a bug where loc indexes were read from
// the push-immediate field aliased all slots to slot 0, folding the
// accumulator read in `acc += b + c` to the chain constant and returning
// a wrong digest (arith-rt returned 152898696 instead of
// 7074999600000). Source vs optimized bytecode must agree byte-for-byte.
export default {
    fetch() {
        let acc = 0;
        for (let i = 0; i < 1000; i++) {
            let a = 12345;
            let b = a * 3 + 7;   // writes b's slot after reading a
            let c = a ^ 0x5a5;   // re-reads a after b's write
            acc += b + c;        // reads acc after c's write
        }
        return new Response(String(acc));
    },
};
