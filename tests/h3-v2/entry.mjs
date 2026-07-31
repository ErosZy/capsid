import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { createInterface } from 'node:readline';

const args = new Map();
for (let index = 2; index < process.argv.length; index += 2) {
    args.set(process.argv[index], process.argv[index + 1]);
}

const driverPath = args.get('--driver');
const workerPath = args.get('--worker');
const bundlePath = args.get('--bundle');
const expectedBody = args.get('--expected-body') ?? 'h3-entry-ok';
const expectedStatus = Number(args.get('--expected-status') ?? 200);
const expectedStatusText = args.get('--expected-status-text');
const requestPath = args.get('--path') ?? '/entry';
if (!driverPath || !workerPath || !bundlePath) {
    throw new Error('expected --driver, --worker and --bundle');
}

const encoder = new TextEncoder();
const decoder = new TextDecoder();
const hex = bytes => {
    let output = '';
    for (const byte of bytes) {
        output += byte.toString(16).padStart(2, '0');
    }
    return output || '-';
};
const unhex = value => {
    if (value === '-') {
        return new Uint8Array();
    }
    assert.match(value, /^(?:[0-9a-f]{2})+$/i, 'driver hex');
    const output = new Uint8Array(value.length / 2);
    for (let index = 0; index < output.length; ++index) {
        output[index] = Number.parseInt(
            value.slice(index * 2, index * 2 + 2),
            16,
        );
    }
    return output;
};

const child = spawn(driverPath, [ workerPath, bundlePath ], {
    stdio: [ 'pipe', 'pipe', 'inherit' ],
});
const lines = createInterface({
    input: child.stdout,
    crlfDelay: Infinity,
})[Symbol.asyncIterator]();
const nextLine = async () => {
    const result = await lines.next();
    if (result.done) {
        throw new Error('H3 worker driver exited unexpectedly');
    }
    if (result.value.startsWith('FATAL ')) {
        throw new Error(decoder.decode(unhex(result.value.slice(6))));
    }
    return result.value;
};
const exitPromise = new Promise((resolve, reject) => {
    child.once('error', reject);
    child.once('exit', resolve);
});

try {
    assert.equal(await nextLine(), 'READY', 'worker readiness');
    child.stdin.write([
        'REQUEST',
        1,
        hex(encoder.encode('GET')),
        hex(encoder.encode(`https://compat.example${requestPath}`)),
        '00000000',
        '-',
        257,
    ].join(' ') + '\n');
    const fields = (await nextLine()).split(' ');
    assert.equal(fields[0], 'RESULT', 'driver result marker');
    assert.equal(Number(fields[1]), 1, 'request id');
    assert.equal(Number(fields[2]), expectedStatus, 'response status');
    assert.equal(
        decoder.decode(unhex(fields[4])),
        expectedBody,
        'response body',
    );
    assert.equal(decoder.decode(unhex(fields[5])), '', 'runtime error');
    if (expectedStatusText !== undefined) {
        assert.equal(
            decoder.decode(unhex(fields[6])),
            expectedStatusText,
            'response statusText',
        );
    }

    child.stdin.end('STOP\n');
    assert.equal(await exitPromise, 0, 'driver exit');
} catch (cause) {
    child.kill('SIGKILL');
    await exitPromise.catch(() => {});
    throw new Error(`H3 entry failure: bundle=${bundlePath}`, { cause });
}

console.log(`PASS: real-worker H3 entry ${expectedBody}`);
