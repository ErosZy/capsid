// Correctness anchor for the interpreter's mixed int/float OP_mul fast path.
function multiply(left, right) {
  return left * right;
}

if (multiply(3, 0.5) !== 1.5 ||
    multiply(0.5, 3) !== 1.5 ||
    !Object.is(multiply(-1, 0.0), -0) ||
    !Number.isNaN(multiply(NaN, 2)) ||
    multiply(Infinity, 2) !== Infinity ||
    multiply(3n, 4n) !== 12n)
  throw new Error("mixed multiply result mismatch");

const order = [];
const left = {
  valueOf() {
    order.push("left");
    return 3;
  }
};
const right = {
  valueOf() {
    order.push("right");
    return 0.5;
  }
};
if (multiply(left, right) !== 1.5 || order.join(",") !== "left,right")
  throw new Error("mixed multiply coercion mismatch");

let threw = false;
try {
  multiply(Symbol("x"), 2);
} catch (error) {
  threw = error instanceof TypeError;
}
if (!threw)
  throw new Error("mixed multiply exception mismatch");

globalThis.__capsidSuiteOk = true;
