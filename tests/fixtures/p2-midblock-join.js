// Regression: P2 cross-BB lattice must not fold reads on the join state
// of the jump-taken path. The mid-block conditional `if (c) { x = 1; }`
// jumps to the join with the state AT the jump, not with the block-exit
// state the propagation attached to the last live insn; dropping that
// edge let the join adopt the fall-through exit state and fold y on the
// false path, where x kept its old value. False path must yield y = 8
// (7 + 1), never 2. (Also exercises the P16 mid-block liveness merge:
// the false-path read of x keeps the init store live across the jump.)
export default {
    fetch() {
        let x = 7;
        let c = Math.random() < 0;
        if (c) { x = 1; }
        let y = x + 1;
        return new Response(String(y));
    },
};
