// Differential failload fixture: compiles fine (self-contained module,
// no imports), but the worker must reject it at load on BOTH paths —
// source bundle and optimized trusted bytecode — with identical error
// text (no default.fetch / named fetch export).
export const marker = 'no-fetch-export';
