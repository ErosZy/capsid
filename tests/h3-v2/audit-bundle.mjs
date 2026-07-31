import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

if (process.argv.length !== 4) {
    throw new Error('expected bundle path and esbuild metafile path');
}

const bundlePath = path.resolve(process.argv[2]);
const metafilePath = path.resolve(process.argv[3]);
const referenceRoot = path.resolve(
    path.dirname(fileURLToPath(import.meta.url)),
    '../../examples/h3-v2-reference',
);
const bundle = await readFile(bundlePath, 'utf8');
const metafile = JSON.parse(await readFile(metafilePath, 'utf8'));
const outputs = Object.entries(metafile.outputs);

assert.equal(outputs.length, 1, 'H3 build must emit exactly one file');
const [ outputPath, output ] = outputs[0];
assert.equal(
    path.resolve(referenceRoot, outputPath),
    bundlePath,
    'metafile output must be the loaded worker bundle',
);
assert.deepEqual(output.imports, [], 'H3 bundle must have no external imports');
assert.ok(output.entryPoint, 'H3 bundle must have one ESM entry point');
assert.ok(
    Buffer.byteLength(bundle) <= 192 * 1024,
    'H3 reference bundle must remain below 192 KiB',
);

const inputPaths = Object.keys(metafile.inputs).map(value => value.replaceAll(
    '\\',
    '/',
));
const outputInputs = output.inputs ?? {};
assert.ok(
    inputPaths.some(value =>
        value.includes('/node_modules/h3/') ||
        value.startsWith('node_modules/h3/')),
    'bundle must contain the pinned H3 package',
);
for (const input of inputPaths) {
    assert.ok(
        input.includes('examples/h3-v2-reference/src/') ||
        input.includes('examples/h3-v2-reference/node_modules/h3/') ||
        input.includes('examples/h3-v2-reference/node_modules/rou3/') ||
        input.startsWith('src/') ||
        input.startsWith('node_modules/h3/') ||
        input.startsWith('node_modules/rou3/') ||
        input.startsWith('node_modules/srvx/'),
        `unexpected package, listener or polyfill in H3 graph: ${input}`,
    );
}
for (const [ input, contribution ] of Object.entries(outputInputs)) {
    if (!input.replaceAll('\\', '/').startsWith('node_modules/srvx/')) {
        continue;
    }
    const normalized = input.replaceAll('\\', '/');
    if (normalized === 'node_modules/srvx/dist/adapters/generic.mjs') {
        assert.ok(
            contribution.bytesInOutput <= 64,
            'srvx generic contribution must remain limited to Web aliases',
        );
        continue;
    }
    if (normalized === 'node_modules/srvx/dist/body-limit.mjs') {
        assert.ok(
            contribution.bytesInOutput <= 4 * 1024,
            'srvx body-limit contribution must remain a small Web utility',
        );
        continue;
    }
    assert.fail(`srvx server/listener code contributed to bundle: ${input}`);
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
    /\bsrvx\b/,
    /\bfromNodeHandler\b/,
    /\bdefineNodeListener\b/,
    /\btoNodeListener\b/,
    /\bGenericServer\b/,
]) {
    assert.doesNotMatch(bundle, forbidden);
}

console.log(
    `PASS: self-contained H3 ESM (${output.bytes} bytes, ` +
    `${inputPaths.length} inputs)`,
);
