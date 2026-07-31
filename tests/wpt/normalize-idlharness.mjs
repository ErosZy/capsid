#!/usr/bin/env node

import fs from 'node:fs';

const [ inputPath, outputPath ] = process.argv.slice(2);
if (!inputPath || !outputPath) {
    throw new TypeError('usage: normalize-idlharness.mjs <input> <output>');
}

const source = fs.readFileSync(inputPath, 'utf8');
const proxyProbe =
    'new (new Proxy(o, {construct: () => ({})}));';
const reflectProbe =
    'Reflect.construct(function() {}, [], o);';
const first = source.indexOf(proxyProbe);
if (first < 0 || source.indexOf(proxyProbe, first + 1) >= 0) {
    throw new TypeError('unexpected idlharness IsConstructor probe');
}

// QuickJS currently rejects the Proxy-based probe even for Object. Use the
// equivalent Reflect.construct test so idlharness still verifies the actual
// interface objects without weakening the assertion.
fs.writeFileSync(
    outputPath,
    source.slice(0, first) + reflectProbe +
        source.slice(first + proxyProbe.length),
);
