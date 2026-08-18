import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import path from 'node:path';

if (process.argv.length !== 4) {
    throw new Error('expected bundle path and esbuild metafile path');
}

const bundlePath = path.resolve(process.argv[2]);
const metafilePath = path.resolve(process.argv[3]);
const bundle = await readFile(bundlePath, 'utf8');
const metafile = JSON.parse(await readFile(metafilePath, 'utf8'));
const outputs = Object.entries(metafile.outputs);

assert.equal(outputs.length, 1, 'Elysia build must emit exactly one file');
const [ outputPath, output ] = outputs[0];
assert.equal(
    path.resolve(outputPath),
    bundlePath,
    'metafile output must be the loaded worker bundle',
);
assert.deepEqual(
    output.imports,
    [{
        path: 'file-type',
        kind: 'dynamic-import',
        external: true,
    }],
    'Elysia bundle must have no imports except the external file-type ' +
        'dynamic import (runtime-caught by Elysia\'s install fallback)',
);
assert.ok(output.entryPoint, 'Elysia bundle must have one ESM entry point');
assert.ok(
    Buffer.byteLength(bundle) <= 512 * 1024,
    'Elysia reference bundle must remain below the 512 KiB compatibility ' +
        'budget (1.4 embeds the @sinclair/typebox schema system)',
);

const inputPaths = Object.keys(metafile.inputs).map(value => value.replaceAll(
    '\\',
    '/',
));
assert.ok(
    inputPaths.some(value => value.includes('/node_modules/elysia/')),
    'bundle must contain the pinned Elysia package',
);
const allowedInputs = [
    'examples/elysia-reference/src/',
    'examples/elysia-reference/node_modules/elysia/',
    'examples/elysia-reference/node_modules/cookie/',
    'examples/elysia-reference/node_modules/memoirist/',
    'examples/elysia-reference/node_modules/exact-mirror/',
    'examples/elysia-reference/node_modules/fast-decode-uri-component/',
    'examples/elysia-reference/node_modules/@sinclair/typebox/',
];
for (const input of inputPaths) {
    assert.ok(
        allowedInputs.some(prefix => input.includes(prefix)),
        `unexpected package or polyfill in Elysia graph: ${input}`,
    );
}

for (const forbidden of [
    /\bfrom\s*["'][^"']+["']/,
    /\brequire\s*\(/,
    /["']node:[^"']+["']/,
    /["']file:\/\/[^"']*["']/,
    /sourceMappingURL=/,
    /globalThis\.tjs/,
    /globalThis\.(?:process|Buffer|Deno|Bun)\s*=/,
    /Object\.defineProperty\(globalThis,\s*["'](?:process|Buffer|Deno|Bun)["']/,
]) {
    assert.doesNotMatch(bundle, forbidden);
}

console.log(
    `PASS: self-contained Elysia ESM (${output.bytes} bytes, ` +
    `${inputPaths.length} inputs)`,
);
