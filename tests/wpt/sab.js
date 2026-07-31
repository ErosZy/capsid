// WPT's common/sab.js discovers SharedArrayBuffer through shared Wasm memory.
// This profile exposes ECMAScript SharedArrayBuffer but intentionally excludes
// Wasm threads, so use the same requested buffer type directly.
const createBuffer = (type, length, options) => {
    if (type === 'ArrayBuffer') {
        return new ArrayBuffer(length, options);
    }
    if (type === 'SharedArrayBuffer') {
        return new SharedArrayBuffer(length, options);
    }
    throw new Error('type has to be ArrayBuffer or SharedArrayBuffer');
};
