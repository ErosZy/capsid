#!/usr/bin/env node
import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';

const [wptRoot, output, check] = process.argv.slice(2);
assert.ok(wptRoot && output && (check === undefined || check === '--check'),
    'usage: generate-gb18030-ranges.mjs <wpt-root> <output> [--check]');
const manifest = JSON.parse(fs.readFileSync(new URL('../tests/wpt/manifest.json', import.meta.url)));
const relative = 'encoding/legacy-mb-schinese/gb18030/resources/ranges.js';
const source = fs.readFileSync(path.join(wptRoot, relative));
assert.equal(execFileSync('git', ['-C', wptRoot, 'rev-parse', 'HEAD'], {encoding: 'utf8'}).trim(), manifest.commit);
assert.deepEqual(source, execFileSync('git', ['-C', wptRoot, 'show', `${manifest.commit}:${relative}`]));
const rows = [...source.toString('utf8').matchAll(/\[(\d+),\s*"\\u(?:\{([\da-f]+)\}|([\da-f]{4}))"\]/gi)]
    .map(([, pointer, braced, plain]) => [Number(pointer), parseInt(braced ?? plain, 16)]);
assert.equal(rows.length, 207);
assert.deepEqual(rows[0], [0, 0x80]);
assert.deepEqual(rows.at(-1), [189000, 0x10000]);
for (let i = 1; i < rows.length; ++i) assert.ok(rows[i][0] > rows[i - 1][0]);
const text = [
    '// Generated from the WHATWG ranges in the manifest-pinned WPT revision.',
    '// Regenerate with tools/generate-gb18030-ranges.mjs.',
    `// WPT: ${manifest.commit}; ${relative}`,
    'const ranges = [',
    ...rows.map(([pointer, cp]) => `    ${pointer}, 0x${cp.toString(16).toUpperCase()},`),
    '];',
    '',
    '// WHATWG "index gb18030 ranges code point", including the 2005 exception.',
    'export function gb18030RangesCodePoint(pointer) {',
    '    if ((pointer > 39419 && pointer < 189000) || pointer > 1237575) return null;',
    '    if (pointer === 7457) return 0xE7C7;',
    '    let low = 0;',
    '    let high = ranges.length / 2;',
    '    while (low + 1 < high) {',
    '        const middle = (low + high) >>> 1;',
    '        if (ranges[middle * 2] <= pointer) low = middle;',
    '        else high = middle;',
    '    }',
    '    return ranges[low * 2 + 1] + pointer - ranges[low * 2];',
    '}',
    '',
].join('\n');
if (check) assert.equal(fs.readFileSync(output, 'utf8'), text, `stale GB18030 ranges: ${output}`);
else fs.writeFileSync(output, text);
console.log(`${check ? 'Verified' : 'Generated'} ${rows.length} GB18030 ranges: ${path.resolve(output)}`);
