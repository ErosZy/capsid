import assert from 'node:assert/strict';
import fs from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { parse } from 'acorn';
import * as prettier from 'prettier';

const root = fileURLToPath(new URL('../', import.meta.url));

function normalized(value) {
  if (typeof value === 'bigint') return { bigintValue: value.toString() };
  if (typeof value === 'number' && !Number.isFinite(value)) return { numberValue: String(value) };
  if (value instanceof RegExp) return { regexValue: value.source, flags: value.flags };
  if (Array.isArray(value)) return value.map(normalized);
  if (value && typeof value === 'object') {
    const result = {};
    for (const [key, item] of Object.entries(value)) {
      if (['start', 'end', 'loc', 'range'].includes(key)) continue;
      if (value.type === 'Literal' && ['raw', 'bigint'].includes(key)) continue;
      if (value.type === 'Literal' && key === 'regex') {
        // Flag order is spelling, but changing the set or pattern is semantic.
        result[key] = { pattern: item.pattern, flags: [...item.flags].sort().join('') };
        continue;
      }
      result[key] = normalized(item);
    }
    return result;
  }
  return value;
}

export function assertEquivalentJS(before, after, filename = 'input.mjs') {
  const options = {
    ecmaVersion: 'latest',
    sourceType: filename.endsWith('.cjs') ? 'script' : 'module',
  };
  assert.deepStrictEqual(
    normalized(parse(after, options)),
    normalized(parse(before, options)),
    `${filename}: formatting changed the JavaScript AST`,
  );
}

async function main() {
  const [mode, ...files] = process.argv.slice(2);
  assert.ok(
    ['--write', '--check'].includes(mode) && files.length,
    'mode and explicit files are required',
  );
  const config = JSON.parse(await fs.readFile(path.join(root, '.prettierrc.json'), 'utf8'));
  let changed = 0;
  for (const filename of files) {
    const original = await fs.readFile(filename, 'utf8');
    const output = await prettier.format(original, { ...config, filepath: filename });
    if (filename.endsWith('.json')) {
      assert.deepStrictEqual(
        JSON.parse(output),
        JSON.parse(original),
        `${filename}: JSON values changed`,
      );
    } else {
      // Prettier's CLI --debug-check JSON serializer rejects BigInt AST values.
      // Keep an independent semantic check rather than dropping that guard.
      assertEquivalentJS(original, output, filename);
    }
    if (output === original) continue;
    changed += 1;
    console.log(filename);
    if (mode === '--write') await fs.writeFile(filename, output, 'utf8');
  }
  console.log(JSON.stringify({ [mode === '--write' ? 'formatted' : 'needsFormatting']: changed }));
  if (mode === '--check' && changed) process.exitCode = 1;
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  await main();
}
