import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { startCapsid, stopCapsid, withCleanup } from './helpers/ws_host_backend.mjs';
import { cases } from './fixtures/gb18030-observation.mjs';

const fixture = fileURLToPath(new URL('./fixtures/gb18030-observation.mjs', import.meta.url));
// Fixed expectations from the pinned upstream WPT and the WHATWG error rules.
// Keep these separate from the product's generated range table.
const expected = new Map(
  [
    [0, [0x80]],
    [35, [0xa3]],
    [36, [0xa5]],
    [7456, [0x1e3e]],
    [7457, [0xe7c7]],
    [7458, [0x1e40]],
    [19056, [0x9fb3]],
    ...Array.from({ length: 8 }, (_, i) => [19057 + i, [0x9fb4 + i]]),
    [19065, [0x9fbc]],
    [33469, [0xe76c]],
    [39075, [0xfe0f]],
    ...Array.from({ length: 10 }, (_, i) => [39076 + i, [0xfe10 + i]]),
    [39086, [0xfe1a]],
    [39419, [0xffff]],
    [39420, [0xfffd]],
    [188999, [0xfffd]],
    [189000, [0x10000]],
    [1237575, [0x10ffff]],
    [1237576, [0xfffd]],
    [1587599, [0xfffd]],
  ].map(([pointer, points]) => [`p${pointer}`, points]),
);
for (const [id, points] of [
  ['two-byte-2022', [0x9fb4, 0xfe10, 0xfe12, 0xfe11]],
  ['two-byte-pua', [0xe000, 0x3000]],
  ['ascii-adjacent', [65, 0x9fb4, 66, 0xfe10, 67]],
  ['euro', [0x20ac]],
  ['truncated-one', [0xfffd]],
  ['truncated-two', [0xfffd]],
  ['truncated-three', [0xfffd]],
  ['bad-third', [0xfffd, 0x30, 65]],
  ['bad-fourth', [0xfffd, 0x30, 0x4e04]],
  ['bad-two-byte', [0xfffd, 63]],
])
  expected.set(id, points);
const invalid = new Set([
  'p39420',
  'p188999',
  'p1237576',
  'p1587599',
  'truncated-one',
  'truncated-two',
  'truncated-three',
  'bad-third',
  'bad-fourth',
  'bad-two-byte',
]);

export async function verifyVectors(base, output) {
  const response = await fetch(`${base}/vectors`, { signal: AbortSignal.timeout(30000) });
  assert.equal(response.status, 200);
  const observations = await response.json();
  if (output) fs.writeFileSync(output, JSON.stringify(observations, null, 2) + '\n');
  assert.equal(observations.length, expected.size * 4 * 2);
  const seen = new Set();
  let runs = 0;
  for (const observation of observations) {
    const { id, label, fatal } = observation,
      key = `${label}:${id}:${fatal}`;
    assert.ok(['gb18030', 'gbk', 'GB_2312-80', 'x-gbk'].includes(label), key);
    assert.ok(expected.has(id) && typeof fatal === 'boolean' && !seen.has(key), key);
    seen.add(key);
    const length = cases.find((sample) => sample.id === id).bytes.length;
    const cuts = [
      [],
      ...Array.from({ length: length + 1 }, (_, i) => [i]),
      Array.from({ length }, (_, i) => i + 1),
    ];
    assert.deepEqual(
      observation.runs.map((run) => run.cuts),
      cuts,
      `${key}: split coverage`,
    );
    for (const result of observation.runs) {
      ++runs;
      assert.deepEqual(result.reset, { points: [65] }, `${key}: decoder reset`);
      if (fatal && invalid.has(id)) assert.equal(result.error, 'TypeError', key);
      else {
        assert.equal(result.error, null, key);
        assert.equal(result.chunks.length, result.cuts.length + 1, key);
        assert.deepEqual(result.chunks.flat(), expected.get(id), key);
      }
    }
  }
  return { vectors: observations.length, runs };
}

export async function captureSweep(base, directory) {
  fs.mkdirSync(directory, { recursive: true });
  const counts = {};
  for (const [width, limit] of [
    [2, 23940],
    [4, 1587600],
  ]) {
    const fd = fs.openSync(path.join(directory, `width-${width}.u32le`), 'wx');
    try {
      for (let start = 0; start < limit; start += 8192) {
        const count = Math.min(8192, limit - start);
        const response = await fetch(`${base}/sweep?width=${width}&start=${start}&count=${count}`, {
          signal: AbortSignal.timeout(30000),
        });
        assert.equal(response.status, 200, `${width}:${start}`);
        const bytes = Buffer.from(await response.arrayBuffer());
        assert.equal(bytes.length, count * 4);
        assert.equal(fs.writeSync(fd, bytes), bytes.length);
      }
    } finally {
      fs.closeSync(fd);
    }
    counts[width] = limit;
  }
  return counts;
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  const [host, worker, mode = 'single-worker', output] = process.argv.slice(2);
  assert.ok(
    host && worker,
    'usage: test_gb18030_compatibility.mjs <host> <worker> [mode] [capture-directory]',
  );
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'capsid-gb18030-'));
  let runtime, stopped;
  await withCleanup(
    async () => {
      runtime = await startCapsid(host, worker, fixture, directory, mode, {
        workers: mode === 'static-pool' ? 2 : 1,
      });
      const base = `http://127.0.0.1:${runtime.port}/@capsid/orders`;
      if (output) fs.mkdirSync(output, { recursive: true });
      const result = await verifyVectors(base, output && path.join(output, 'vectors.json'));
      if (output) result.sweep = await captureSweep(base, output);
      console.log(JSON.stringify({ mode, ...result }));
    },
    async () => {
      if (runtime) {
        stopped = await stopCapsid(runtime.child);
        if (output)
          fs.writeFileSync(path.join(output, 'stop.json'), JSON.stringify(stopped) + '\n');
        assert.deepEqual(stopped, { code: 0, signal: null, forced: false });
      }
      fs.rmSync(directory, { recursive: true, force: true });
    },
  );
}
