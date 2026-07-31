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

assert.equal(outputs.length, 1, 'itty-router build must emit exactly one file');
const [ outputPath, output ] = outputs[0];
assert.equal(
    path.resolve(outputPath),
    bundlePath,
    'metafile output must be the loaded worker bundle',
);
assert.deepEqual(
    output.imports,
    [],
    'itty-router bundle must have no external imports',
);
assert.ok(output.entryPoint, 'itty-router bundle must have one ESM entry point');
assert.ok(
    Buffer.byteLength(bundle) <= 96 * 1024,
    'itty-router reference bundle must remain below 96 KiB',
);

const inputPaths = Object.keys(metafile.inputs).map(value => value.replaceAll(
    '\\',
    '/',
));
assert.ok(
    inputPaths.some(value =>
        value.includes('/node_modules/itty-router/')),
    'bundle must contain the pinned itty-router package',
);
for (const input of inputPaths) {
    assert.ok(
        input.includes('examples/itty-router-reference/src/') ||
        input.includes(
            'examples/itty-router-reference/node_modules/itty-router/',
        ),
        `unexpected package or polyfill in itty-router graph: ${input}`,
    );
}

for (const forbidden of [
    /\bfrom\s*["'][^"']+["']/,
    /\brequire\s*\(/,
    /\bimport\s*\(/,
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
    `PASS: self-contained itty-router ESM (${output.bytes} bytes, ` +
    `${inputPaths.length} inputs)`,
);
