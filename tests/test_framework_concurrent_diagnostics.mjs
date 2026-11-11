// Real request failures must retain their identity and cause through the
// shared framework driver. The healthy control also rejects a driver that
// simply emits a plausible FATAL for every concurrent command.
import assert from 'node:assert/strict';
import {spawn} from 'node:child_process';
import {once} from 'node:events';
import {createInterface} from 'node:readline';

const args = new Map();
for (let index = 2; index < process.argv.length; index += 2) {
    args.set(process.argv[index], process.argv[index + 1]);
}
const driver = args.get('--driver');
const worker = args.get('--worker');
const bundle = args.get('--bundle');
assert.ok(driver && worker && bundle, 'driver, Worker and fixture are required');
const hex = value => Buffer.from(value).toString('hex');
const cases = [
    {name: 'healthy', paths: ['/left', '/right']},
    {name: 'first-error', paths: ['/throw', '/right'], failedIndex: 0, event: 'ERROR'},
    {name: 'second-error', paths: ['/left', '/throw'], failedIndex: 1, event: 'ERROR'},
    {name: 'first-timeout', paths: ['/timeout', '/right'], failedIndex: 0, event: 'REQUEST_TIMEOUT'},
    {name: 'second-timeout', paths: ['/left', '/timeout'], failedIndex: 1, event: 'REQUEST_TIMEOUT'},
];

for (const scenario of cases) {
    const ids = [7101, 7202];
    const child = spawn(driver, [worker, bundle, '--timeout-ms', '500', '--collect-events', '1'], {
        stdio: ['pipe', 'pipe', 'pipe'],
    });
    const closed = once(child, 'close');
    const lines = [];
    let stderr = '';
    let expired = false;
    child.stderr.setEncoding('utf8');
    child.stderr.on('data', chunk => { stderr += chunk; });
    const reader = createInterface({input: child.stdout, crlfDelay: Infinity});
    reader.on('line', line => {
        lines.push(line);
        if (line === 'READY') {
            child.stdin.end(`CONCURRENT ${ids[0]} ${hex('https://concurrent.test' + scenario.paths[0])} ` +
                `${ids[1]} ${hex('https://concurrent.test' + scenario.paths[1])}\nSTOP\n`);
        }
    });
    const deadline = setTimeout(() => {
        expired = true;
        child.kill();
    }, 15_000);
    try {
        const [code, signal] = await closed;
        assert.equal(expired, false, `${scenario.name}: bounded driver completion\n${stderr}`);
        assert.equal(signal, null, `${scenario.name}: driver exits normally\n${stderr}`);
        assert.equal(lines[0], 'READY', `${scenario.name}: real Worker readiness\n${stderr}`);
        assert.doesNotMatch(stderr, /ERROR: AddressSanitizer|WARNING: ThreadSanitizer/, scenario.name);
        if (scenario.event) {
            assert.equal(code, 1, `${scenario.name}: failed request fails the driver`);
            assert.equal(lines.length, 2, `${scenario.name}: no fabricated successful results`);
            assert.match(lines[1], /^FATAL [0-9a-f]+$/, `${scenario.name}: driver failure record`);
            const fatal = Buffer.from(lines[1].slice(6), 'hex').toString('utf8');
            const flags = scenario.event === 'ERROR' ? 0 : 1;
            const prefix = `concurrent framework request failed: request_id=${ids[scenario.failedIndex]} ` +
                `event=${scenario.event} flags=${flags} payload=`;
            assert.ok(fatal.startsWith(prefix), `${scenario.name}: exact event identity and kind: ${fatal}`);
            const payload = fatal.slice(prefix.length);
            if (scenario.event === 'ERROR') {
                assert.match(payload, /^Error: framework-concurrent-fault:\/throw(?:\n|$)/,
                    `${scenario.name}: original application error`);
                assert.match(payload, /\bat fetch\b/, `${scenario.name}: application stack retained`);
            } else {
                assert.equal(payload, 'request deadline exceeded', `${scenario.name}: actual Worker deadline cause`);
            }
        } else {
            assert.equal(code, 0, 'healthy requests preserve success and driver shutdown');
            assert.equal(lines.length, 5, 'two results and two event blocks');
            for (const [index, id] of ids.entries()) {
                const fields = lines[index + 1].split(' ');
                assert.equal(fields[0], 'RESULT');
                assert.equal(Number(fields[1]), id, 'response owner');
                assert.equal(Number(fields[2]), 200, 'actual response status');
                assert.equal(Buffer.from(fields[4], 'hex').toString(), `ok:${scenario.paths[index]}`, 'complete body');
                assert.equal(fields[5], '-', 'no error');
                assert.equal(lines[index + 3], `EVENTS ${id} 0`, 'no dropped fixture logs');
            }
        }
        console.log(JSON.stringify({case: scenario.name, passed: true}));
    } finally {
        clearTimeout(deadline);
        child.stdin.end();
        if (child.exitCode === null && child.signalCode === null) child.kill();
        await closed;
        reader.close();
    }
}
