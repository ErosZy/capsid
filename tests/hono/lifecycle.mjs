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
if (!driverPath || !workerPath || !bundlePath) {
    throw new Error('expected --driver, --worker and --bundle');
}

const encoder = new TextEncoder();
const decoder = new TextDecoder();

const hex = value => {
    let output = '';
    for (const byte of value) {
        output += byte.toString(16).padStart(2, '0');
    }
    return output || '-';
};

const unhex = value => {
    if (value === '-') {
        return new Uint8Array();
    }
    assert.equal(value.length % 2, 0, 'driver hex length');
    const output = new Uint8Array(value.length / 2);
    for (let index = 0; index < output.length; ++index) {
        output[index] = Number.parseInt(
            value.slice(index * 2, index * 2 + 2),
            16,
        );
    }
    return output;
};

const emptyHeaders = '00000000';

const child = spawn(driverPath, [
    workerPath,
    bundlePath,
    '--timeout-ms',
    '500',
], {
    stdio: [ 'pipe', 'pipe', 'inherit' ],
});
const lines = createInterface({ input: child.stdout, crlfDelay: Infinity })[
    Symbol.asyncIterator
]();

const nextLine = async () => {
    const result = await lines.next();
    if (result.done) {
        throw new Error('Hono lifecycle driver exited unexpectedly');
    }
    if (result.value.startsWith('FATAL ')) {
        throw new Error(decoder.decode(unhex(result.value.slice(6))));
    }
    return result.value;
};

const send = command => child.stdin.write(`${command}\n`);

const parseResult = (line, expectedId) => {
    const fields = line.split(' ');
    assert.equal(fields[0], 'RESULT');
    assert.equal(Number(fields[1]), expectedId);
    return {
        status: Number(fields[2]),
        headers: fields[3],
        body: unhex(fields[4]),
        error: decoder.decode(unhex(fields[5])),
    };
};

const request = async (id, path) => {
    send([
        'REQUEST',
        id,
        hex(encoder.encode('GET')),
        hex(encoder.encode(`https://compat.example${path}`)),
        emptyHeaders,
        '-',
        257,
    ].join(' '));
    return parseResult(await nextLine(), id);
};

assert.equal(await nextLine(), 'READY');

send([
    'CONCURRENT',
    1001,
    hex(encoder.encode(
        'https://compat.example/runtime/concurrent?token=slow&delay=40',
    )),
    1002,
    hex(encoder.encode(
        'https://compat.example/runtime/concurrent?token=fast&delay=5',
    )),
].join(' '));
const slow = parseResult(await nextLine(), 1001);
const fast = parseResult(await nextLine(), 1002);
assert.equal(slow.status, 200);
assert.equal(fast.status, 200);
assert.deepEqual(JSON.parse(decoder.decode(slow.body)), {
    token: 'slow',
    delay: 40,
});
assert.deepEqual(JSON.parse(decoder.decode(fast.body)), {
    token: 'fast',
    delay: 5,
});

send([
    'CANCEL',
    1003,
    hex(encoder.encode(
        'https://compat.example/runtime/wait-for-abort',
    )),
    'started',
].join(' '));
assert.equal(await nextLine(), 'CANCELED 1003');
let count = await request(1004, '/runtime/abort-count');
assert.equal(count.status, 200);
assert.deepEqual(JSON.parse(decoder.decode(count.body)), {
    handlers: 1,
    streams: 0,
});

send([
    'CANCEL',
    1005,
    hex(encoder.encode(
        'https://compat.example/runtime/stream-cancel',
    )),
    'body',
].join(' '));
assert.equal(await nextLine(), 'CANCELED 1005');
count = await request(1006, '/runtime/abort-count');
assert.equal(count.status, 200);
assert.deepEqual(JSON.parse(decoder.decode(count.body)), {
    handlers: 1,
    streams: 1,
});

const notFound = await request(1007, '/lifecycle-missing');
assert.equal(notFound.status, 404);
assert.equal(
    decoder.decode(notFound.body),
    'not-found:/lifecycle-missing',
);
assert.equal((await request(1008, '/entry')).status, 200);

const handledError = await request(1009, '/middleware/error-async');
assert.equal(handledError.status, 555);
assert.deepEqual(JSON.parse(decoder.decode(handledError.body)), {
    name: 'RangeError',
    message: 'async-boom',
});
assert.equal((await request(1010, '/entry')).status, 200);

const handlerTimeout = await request(1011, '/runtime/delay?ms=1000');
assert.match(handlerTimeout.error, /^TimeoutError: /);
assert.equal((await request(1012, '/entry')).status, 200);

const middlewareTimeout = await request(
    1013,
    '/runtime/middleware-timeout?ms=1000',
);
assert.match(middlewareTimeout.error, /^TimeoutError: /);
assert.equal((await request(1014, '/entry')).status, 200);

const cpuTimeout = await request(1015, '/runtime/cpu-timeout');
assert.match(
    cpuTimeout.error,
    /^(TimeoutError|RuntimeError): /,
    'synchronous CPU deadline must surface as a runtime timeout/error',
);

child.stdin.end('STOP\n');
const exitCode = await new Promise((resolve, reject) => {
    child.once('error', reject);
    child.once('exit', resolve);
});
assert.equal(exitCode, 0);
console.log('PASS: Hono cancellation, concurrency, timeout and reuse lifecycle');
