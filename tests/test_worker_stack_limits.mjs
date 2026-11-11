// Actual Worker recursion limit and recovery, using the public launch config.
import assert from 'node:assert/strict';
import {spawn} from 'node:child_process';
import {once} from 'node:events';
import {createInterface} from 'node:readline';

const args = new Map();
for (let i = 2; i < process.argv.length; i += 2) args.set(process.argv[i], process.argv[i + 1]);
const driver = args.get('--driver'), worker = args.get('--worker'), bundle = args.get('--bundle');
const stack = args.get('--stack-bytes') ?? 'default';
assert.ok(driver && worker && bundle, 'driver, worker and bundle paths are required');
assert.ok(stack === 'default' || /^[1-9][0-9]*$/.test(stack), 'explicit positive stack budget');
const flags = stack === 'default' ? [] : ['--js-stack-bytes', stack];
const child = spawn(driver, [worker, bundle, ...flags], {stdio: ['pipe', 'pipe', 'inherit']});
const closed = once(child, 'close');
const lines = createInterface({input: child.stdout, crlfDelay: Infinity})[Symbol.asyncIterator]();
const deadline = setTimeout(() => {
    console.error('Worker stack-limit test deadline');
    child.kill();
}, 20_000);
const hex = value => Buffer.from(value).toString('hex');
async function nextLine() {
    const result = await lines.next();
    assert.equal(result.done, false, 'driver remains alive');
    if (result.value.startsWith('FATAL ')) {
        assert.fail(Buffer.from(result.value.slice(6), 'hex').toString());
    }
    return result.value;
}
async function request(id, path) {
    child.stdin.write(`REQUEST ${id} ${hex('GET')} ${hex('https://stack.test' + path)} 00000000 - 1\n`);
    const fields = (await nextLine()).split(' ');
    assert.equal(fields[0], 'RESULT');
    assert.equal(Number(fields[1]), id);
    assert.equal(Number(fields[2]), 200, 'recursion is caught by the application');
    assert.equal(fields[5], '-', 'no runtime error outside the application catch');
    return Buffer.from(fields[4], 'hex').toString();
}
try {
    assert.equal(await nextLine(), 'READY');
    for (let cycle = 0; cycle < 2; ++cycle) {
        assert.deepEqual(JSON.parse(await request(cycle * 2 + 1, '/overflow')), {name: 'RangeError'});
        assert.equal(await request(cycle * 2 + 2, '/healthy'), 'worker-still-responsive');
    }
    child.stdin.end('STOP\n');
    const [code, signal] = await closed;
    assert.equal(signal, null, 'driver exits normally');
    assert.equal(code, 0, 'Worker shuts down after recursion recovery');
    console.log(JSON.stringify({stackBytes: stack, overflowCycles: 2, recoveryRequests: 2, status: 'passed'}));
} finally {
    clearTimeout(deadline);
    child.stdin.end();
    if (child.exitCode === null && child.signalCode === null) child.kill();
    await closed;
}
