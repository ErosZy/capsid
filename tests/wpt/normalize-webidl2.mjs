#!/usr/bin/env node

import fs from 'node:fs';

const [ inputPath, outputPath ] = process.argv.slice(2);
if (!inputPath || !outputPath) {
    throw new TypeError('usage: normalize-webidl2.mjs <input> <output>');
}

let source = fs.readFileSync(inputPath, 'utf8');
const factoryStart = '})(globalThis, () => {';
const start = source.indexOf(factoryStart);
if (start < 0) {
    throw new TypeError('unexpected WebIDL2 UMD wrapper');
}
source =
    'globalThis.WebIDL2 = (() => {' +
    source.slice(start + factoryStart.length);
const factoryEnd = '\n});\n//# sourceMappingURL=';
const end = source.lastIndexOf(factoryEnd);
if (end < 0) {
    throw new TypeError('unexpected WebIDL2 UMD footer');
}
source =
    source.slice(0, end) +
    '\n})();\n//# sourceMappingURL=' +
    source.slice(end + factoryEnd.length);
fs.writeFileSync(outputPath, source);
