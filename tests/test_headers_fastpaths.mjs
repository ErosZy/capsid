import assert from 'node:assert/strict';
import fs from 'node:fs';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = fileURLToPath(new URL('../', import.meta.url));
const options = new Map();
for (let index = 2; index < process.argv.length; index += 2) {
  const key = process.argv[index],
    value = process.argv[index + 1];
  assert.ok(['--helper', '--worker'].includes(key) && value && !options.has(key));
  options.set(key, value);
}
assert.equal(options.size, 2);
const report = {
  complete: false,
  observation: null,
  child: null,
  workerExit: null,
  stdout: '',
  stderr: '',
};
let destination;
if (process.env.CAPSID_WS_REPORT_DIR) {
  fs.mkdirSync(process.env.CAPSID_WS_REPORT_DIR, { recursive: true });
  destination = process.env.CAPSID_WS_REPORT_DIR + '/headers-fastpaths.json';
  assert.equal(fs.existsSync(destination), false);
}
const save = () => {
  if (destination) fs.writeFileSync(destination, JSON.stringify(report, null, 2) + '\n');
};
save();
const child = spawn(
  options.get('--helper'),
  [
    options.get('--worker'),
    root + 'tests/fixtures/binding-headers-package.mjs',
    root + 'tests/fixtures/headers-fastpaths.mjs',
    'http://127.0.0.1:1',
    'headers',
  ],
  { stdio: ['pipe', 'pipe', 'pipe'] },
);
let pending = '',
  resolveBody,
  rejectBody,
  timer;
const body = new Promise((resolve, reject) => {
  resolveBody = resolve;
  rejectBody = reject;
});
child.stdin.on('error', rejectBody);
child.stdout.setEncoding('utf8');
child.stdout.on('data', (chunk) => {
  report.stdout += chunk;
  pending += chunk;
  for (;;) {
    const end = pending.indexOf('\n');
    if (end < 0) break;
    const line = pending.slice(0, end).replace(/\r$/, '');
    pending = pending.slice(end + 1);
    try {
      if (line.startsWith('BINDING_BODY ')) {
        assert.equal(report.observation, null);
        report.observation = JSON.parse(line.slice('BINDING_BODY '.length));
        resolveBody();
      } else if (line.startsWith('BINDING_EXIT '))
        report.workerExit = JSON.parse(line.slice('BINDING_EXIT '.length));
    } catch (error) {
      rejectBody(error);
    }
  }
});
child.stderr.on('data', (chunk) => (report.stderr += chunk));
const closed = new Promise((resolve, reject) => {
  child.once('error', reject);
  child.once('close', (code, signal) => resolve({ code, signal }));
});
const deadline = new Promise((_, reject) => {
  timer = setTimeout(() => reject(new Error('original45s Binding helper deadline')), 45000);
});
try {
  await Promise.race([
    body,
    closed.then(() => assert.ok(report.observation, 'actual body before helper exit')),
    deadline,
  ]);
  const actual = report.observation;
  assert.equal(actual.names.length, 128);
  assert.equal(actual.values.length, 128);
  // The pinned API accepts empty/non-ASCII names. Here only ASCII token
  // boundaries are enumerated; the separate99-case VM differential stays intact.
  const separators = new Set('()<>@,;:\\"/[]?={}'.split(''));
  for (let code = 0; code < 128; ++code) {
    const char = String.fromCharCode(code);
    const invalidName = code <= 32 || code === 127 || separators.has(char);
    assert.deepEqual(
      actual.names[code],
      invalidName ? { ok: false, error: 'TypeError' } : { ok: true, value: 'value' },
      'name code ' + code,
    );
    assert.equal(actual.values[code].length, 3);
    for (const row of actual.values[code]) {
      assert.deepEqual(
        row,
        [0, 10, 13].includes(code)
          ? { ok: false, error: 'TypeError' }
          : { ok: true, value: 'a' + char + 'b' },
        'value code ' + code,
      );
    }
  }
  assert.deepEqual(
    actual.unicode,
    [
      'é',
      '中文',
      '💡',
      '\ufeff',
      '\ufffd'.repeat(3),
      '\ufffd'.repeat(3),
      '\ud800\udc00',
      '\ufffd'.repeat(3) + 'a',
      '\ufffd'.repeat(6),
    ].map((value) => ({ ok: true, value })),
  );
  assert.deepEqual(actual.trace, ['name:string', 'value:string']);
  assert.equal(actual.converted, 'ascii');
  assert.equal(actual.whitespace, 'ascii');
  assert.equal(actual.emptyName, 'value');
  assert.equal(actual.longAscii, 'a'.repeat(16384));
  child.stdin.end('finish\n');
  report.child = await Promise.race([closed, deadline]);
  assert.deepEqual(report.child, { code: 0, signal: null });
  assert.deepEqual(report.workerExit, { reaped: true, channelClosed: true, code: 0 });
  report.complete = true;
} finally {
  clearTimeout(timer);
  if (child.exitCode === null && child.signalCode === null) {
    child.kill();
    await closed;
  }
  save();
}
console.log(
  JSON.stringify({
    complete: true,
    asciiNames: 128,
    asciiValueOperations: 384,
    unicode: 9,
    naturalWorkerExit: 0,
  }),
);
