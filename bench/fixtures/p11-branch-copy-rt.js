// Regression: P11 must not carry a slot alias out of a conditional block.
function conditionalCopy(condition, source) {
  let target = 7;
  if (condition)
    target = source;
  return target;
}

if (conditionalCopy(false, 9) !== 7 ||
    conditionalCopy(true, 9) !== 9)
  throw new Error("conditional slot copy mismatch");

globalThis.__capsidSuiteOk = true;
