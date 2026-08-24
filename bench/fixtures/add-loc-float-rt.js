// Correctness and timing anchor for QuickJS OP_add_loc numeric fast paths.
function addMany(initial, value, count) {
  let sum = initial;
  for (let i = 0; i < count; i++)
    sum += value;
  return sum;
}

if (addMany(0.5, 0.25, 6) !== 2 ||
    addMany(2147483647, 1, 2) !== 2147483649 ||
    addMany("a", 2, 2) !== "a22" ||
    addMany(1n, 2n, 3) !== 7n)
  throw new Error("add_loc result mismatch");

let conversions = 0;
const operand = {
  [Symbol.toPrimitive]() {
    conversions++;
    return 0.5;
  }
};
if (addMany(1, operand, 2) !== 2 || conversions !== 2)
  throw new Error("add_loc coercion mismatch");

globalThis.__capsidSuiteOk = true;
