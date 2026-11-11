// One fixture exercises both storage paths and call classes on V8 and the
// actual Worker. Literal invariants keep the differential fixture accountable.
import assert from 'node:assert/strict';
import {spawn} from 'node:child_process';
import {once} from 'node:events';
import {createInterface} from 'node:readline';
import {pathToFileURL} from 'node:url';
import path from 'node:path';
import crypto from 'node:crypto';

const args = new Map();
for (let index = 2; index < process.argv.length; index += 2) args.set(process.argv[index], process.argv[index + 1]);
const driver = args.get('--driver'), worker = args.get('--worker'), bundle = args.get('--bundle');
assert.ok(driver && worker && bundle, 'real driver, Worker and fixture required');
const fixture = (await import(pathToFileURL(path.resolve(bundle)).href)).default;
const names = ['captures', 'flags', 'reentry', 'overrides', 'calls', 'iterators', 'generators', 'healthy'];
const expected = [];
for (const name of names) {
    const response = await fixture.fetch(new Request(`https://vm.test/${name}`));
    assert.equal(response.status, 200, `V8/${name}`);
    expected.push(await response.json());
}
assert.equal(expected[0].checksum, 64 * (1 + 2 + 7 + 8 + 9 + 17 + 41));
assert.deepEqual(expected[0].matches.map(item => item.groups), [0, 1, 6, 7, 8, 16, 40]);
for (const {groups, match} of expected[0].matches) {
    assert.deepEqual(match.values, ['a'.repeat(groups), ...Array(groups).fill('a')]);
    assert.deepEqual(match.indices, [[0, groups], ...Array.from({length: groups}, (_, i) => [i, i + 1])]);
}
assert.ok(expected[1].every(item => item.missing === null && item.reset === 0 && item.atZero === null && item.stickyReset === 0));
assert.equal(expected[2].calls, 1);
assert.equal(expected[2].inner.values.length, 17);
assert.deepEqual(expected[3], {test: true, match: 'custom', trace: ['first', 'second', 'get-exec'], same: true, proxy: 12, proxyThis: 'receiver'});
assert.deepEqual(expected[4], {bound: 9, list: [1, 2, 3], subclass: true, boxed: 7, failures: Array(5).fill('TypeError')});
assert.deepEqual(expected[5], {first: {value: 2, done: false}, rest: [4, 6], done: {done: true}, invalid: 'TypeError', head: 7, tail: [11], string: ['😀', 'a'], typed: [0, 65535], entries: [[0, 4], [1, 5]]});
assert.deepEqual(expected[6], {first: {value: 5, done: false}, caught: {value: 13, done: false}, done: {done: true}, closed: 1, syncFirst: {value: [[0, 4], [1, 5]], done: false}, syncReturn: {value: 'finish', done: true}, syncClosed: 1});
assert.deepEqual(expected[7], {okay: true, value: 4});

const child = spawn(driver, [worker, bundle, '--collect-events', '1'], {stdio: ['pipe', 'pipe', 'pipe']});
const closed = once(child, 'close');
const reader = createInterface({input: child.stdout, crlfDelay: Infinity});
const lines = reader[Symbol.asyncIterator]();
let stderr = '';
child.stderr.setEncoding('utf8');
child.stderr.on('data', chunk => { stderr += chunk; });
let expired = false;
const deadline = setTimeout(() => { expired = true; child.kill(); }, 30_000);
async function nextLine() {
    const result = await lines.next();
    assert.equal(result.done, false, `driver remains alive: ${stderr}`);
    assert.ok(!result.value.startsWith('FATAL '), result.value.startsWith('FATAL ')
        ? Buffer.from(result.value.slice(6), 'hex').toString() : result.value);
    return result.value;
}
const hex = text => Buffer.from(text).toString('hex');
try {
    assert.equal(await nextLine(), 'READY');
    // Reuse the same Worker after native throws and generator completion;
    // the last healthy response checks recovery after all preceding cases.
    for (const [index, name] of names.entries()) {
        const id = index + 8101;
        child.stdin.write(`REQUEST ${id} ${hex('GET')} ${hex(`https://vm.test/${name}`)} 00000000 - 257\n`);
        const fields = (await nextLine()).split(' ');
        assert.equal(fields[0], 'RESULT');
        assert.equal(Number(fields[1]), id, `${name}: response owner`);
        assert.equal(Number(fields[2]), 200, `${name}: response status`);
        assert.equal(fields[5], '-', `${name}: no runtime error`);
        const body = Buffer.from(fields[4], 'hex');
        assert.deepEqual(JSON.parse(body.toString()), expected[index], `${name}: actual Worker versus V8`);
        assert.equal(await nextLine(), `EVENTS ${id} 0`, `${name}: fixture has no native log/audit events`);
        console.log(JSON.stringify({case: name, passed: true, bodySHA256: crypto.createHash('sha256').update(body).digest('hex')}));
    }
    child.stdin.end('STOP\n');
    const [code, signal] = await closed;
    assert.equal(expired, false, 'bounded driver completion');
    assert.equal(signal, null, `driver normal exit: ${stderr}`);
    assert.equal(code, 0, `driver success: ${stderr}`);
    assert.doesNotMatch(stderr, /ERROR: AddressSanitizer|WARNING: ThreadSanitizer/);
} finally {
    clearTimeout(deadline);
    child.stdin.end();
    if (child.exitCode === null && child.signalCode === null) child.kill();
    await closed;
    reader.close();
}
