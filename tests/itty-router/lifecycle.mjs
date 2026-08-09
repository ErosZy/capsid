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
const variant = args.get('--variant');
if (
    !driverPath ||
    !workerPath ||
    !bundlePath ||
    ![ 'autorouter', 'router', 'itty-router' ].includes(variant)
) {
    throw new Error(
        'expected --driver, --worker, --bundle and a valid --variant',
    );
}

const encoder = new TextEncoder();
const decoder = new TextDecoder();
const emptyHeaders = '00000000';

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

const child = spawn(driverPath, [
    workerPath,
    bundlePath,
    '--timeout-ms',
    '500',
    '--collect-events',
    '1',
], {
    stdio: [ 'pipe', 'pipe', 'inherit' ],
});
const lines = createInterface({
    input: child.stdout,
    crlfDelay: Infinity,
})[Symbol.asyncIterator]();

const nextLine = async () => {
    const result = await lines.next();
    if (result.done) {
        throw new Error('lifecycle driver exited unexpectedly');
    }
    if (result.value.startsWith('FATAL ')) {
        throw new Error(decoder.decode(unhex(result.value.slice(6))));
    }
    return result.value;
};

const send = command => {
    child.stdin.write(`${command}\n`);
};

const parseResult = (line, expectedId) => {
    const fields = line.split(' ');
    assert.equal(fields[0], 'RESULT', 'driver result marker');
    assert.equal(Number(fields[1]), expectedId, 'driver request id');
    return {
        status: Number(fields[2]),
        body: unhex(fields[4]),
        error: decoder.decode(unhex(fields[5])),
    };
};

// Consumes the EVENTS block that follows every RESULT/CANCELED line when
// --collect-events is on. Returns structured native events.
const consumeEvents = async expectedId => {
    const line = await nextLine();
    const fields = line.split(' ');
    assert.equal(fields[0], 'EVENTS', `expected events block for ${expectedId}`);
    assert.equal(Number(fields[1]), expectedId);
    const count = Number(fields[2]);
    const events = [];
    for (let index = 0; index < count; ++index) {
        const parts = (await nextLine()).split(' ');
        if (parts[0] === 'LOG') {
            events.push({ kind: 'LOG', requestId: parts[1], text: parts[2] });
        } else {
            assert.equal(parts[0], 'AUDIT');
            events.push({
                kind: 'AUDIT',
                requestId: parts[1],
                recordId: parts[2],
                text: parts[3],
                resource: parts[4],
            });
        }
    }
    return events;
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
    const result = parseResult(await nextLine(), id);
    result.events = await consumeEvents(id);
    return result;
};

const jsonBody = result => JSON.parse(decoder.decode(result.body));

const assertReusable = async (id, phase) => {
    const entry = await request(id, '/entry');
    assert.equal(entry.status, 200, `${phase}: entry status`);
    assert.equal(
        decoder.decode(entry.body),
        'itty-entry-ok',
        `${phase}: entry body`,
    );
};

const assertCleanContext = async (id, phase) => {
    const result = await request(
        id,
        `/runtime/context-probe/${phase}?phase=${phase}`,
    );
    assert.equal(result.status, 200, `${phase}: probe status`);
    assert.deepEqual(jsonBody(result), {
        param: phase,
        query: { phase },
        content: null,
        cookies: null,
    }, `${phase}: request-local context`);
};

const exitPromise = new Promise((resolve, reject) => {
    child.once('error', reject);
    child.once('exit', resolve);
});

try {
    assert.equal(await nextLine(), 'READY', `${variant}: worker readiness`);

    send([
        'CONCURRENT',
        1001,
        hex(encoder.encode(
            'https://compat.example/runtime/concurrent/slow-param' +
            '?token=slow&delay=40&repeat=a&repeat=b',
        )),
        1002,
        hex(encoder.encode(
            'https://compat.example/runtime/concurrent/fast-param' +
            '?token=fast&delay=5&repeat=x&repeat=y',
        )),
    ].join(' '));
    const slow = parseResult(await nextLine(), 1001);
    const fast = parseResult(await nextLine(), 1002);
    await consumeEvents(1001);
    await consumeEvents(1002);
    assert.equal(slow.status, 200, 'slow concurrent status');
    assert.equal(fast.status, 200, 'fast concurrent status');
    assert.deepEqual(jsonBody(slow), {
        param: 'slow-param',
        query: {
            token: 'slow',
            delay: '40',
            repeat: [ 'a', 'b' ],
        },
        content: {
            token: 'slow',
            source: 'request-content',
        },
        cookies: {
            token: 'slow',
            source: 'request-cookie',
        },
    });
    assert.deepEqual(jsonBody(fast), {
        param: 'fast-param',
        query: {
            token: 'fast',
            delay: '5',
            repeat: [ 'x', 'y' ],
        },
        content: {
            token: 'fast',
            source: 'request-content',
        },
        cookies: {
            token: 'fast',
            source: 'request-cookie',
        },
    });
    await assertCleanContext(1003, 'after-concurrent');

    send([
        'CANCEL',
        1004,
        hex(encoder.encode(
            'https://compat.example/runtime/wait-for-abort',
        )),
        'started',
    ].join(' '));
    assert.equal(await nextLine(), 'CANCELED 1004');
    await consumeEvents(1004);
    let counts = await request(1005, '/runtime/abort-count');
    assert.deepEqual(jsonBody(counts), {
        handlers: 1,
        parses: 0,
        streams: 0,
    });
    await assertCleanContext(1006, 'after-handler-cancel');

    send([
        'CANCEL_UPLOAD',
        1007,
        hex(encoder.encode(
            'https://compat.example/runtime/cancel-parse',
        )),
    ].join(' '));
    assert.equal(await nextLine(), 'CANCELED_UPLOAD 1007');
    await consumeEvents(1007);
    counts = await request(1008, '/runtime/abort-count');
    assert.deepEqual(jsonBody(counts), {
        handlers: 1,
        parses: 1,
        streams: 0,
    });
    await assertCleanContext(1009, 'after-parse-cancel');

    send([
        'CANCEL',
        1010,
        hex(encoder.encode(
            'https://compat.example/runtime/stream-cancel',
        )),
        'body',
    ].join(' '));
    assert.equal(await nextLine(), 'CANCELED 1010');
    await consumeEvents(1010);
    counts = await request(1011, '/runtime/abort-count');
    assert.deepEqual(jsonBody(counts), {
        handlers: 1,
        parses: 1,
        streams: 1,
    });
    await assertCleanContext(1012, 'after-stream-cancel');

    const notFound = await request(1013, '/lifecycle-missing');
    assert.equal(notFound.status, 404);
    assert.deepEqual(jsonBody(notFound), {
        status: 404,
        error: 'missing',
        path: '/lifecycle-missing',
    });
    await assertReusable(1014, '404');

    const caughtError = await request(1015, '/flow/reject');
    assert.equal(caughtError.status, 500);
    assert.deepEqual(jsonBody(caughtError), {
        status: 500,
        error: 'async-flow-boom',
    });
    await assertReusable(1016, 'caught error');

    const asyncTimeout = await request(1017, '/runtime/delay?ms=1000');
    assert.match(asyncTimeout.error, /^TimeoutError: /);
    await assertReusable(1018, 'async timeout');
    await assertCleanContext(1019, 'after-async-timeout');

    // Native event ownership: the LOG events before and after the await
    // must carry the request id (pre-RequestToken bridge reports 0).
    const ownership = await request(1021, '/runtime/ownership');
    assert.equal(ownership.status, 200);
    assert.deepEqual(jsonBody(ownership), { ownership: 'ok' });
    const ownershipLogs = ownership.events.filter(
        event => event.kind === 'LOG',
    );
    const beforeLog = ownershipLogs.find(event =>
        decoder.decode(unhex(event.text)) === 'capsid-owner:before');
    const afterLog = ownershipLogs.find(event =>
        decoder.decode(unhex(event.text)) === 'capsid-owner:after');
    assert.ok(beforeLog, 'before-await LOG must be emitted');
    assert.ok(afterLog, 'after-await LOG must be emitted');
    assert.equal(
        Number(beforeLog.requestId),
        1021,
        'before-await LOG must carry the request id',
    );
    assert.equal(
        Number(afterLog.requestId),
        1021,
        'after-await LOG must carry the request id',
    );

    // The synchronous CPU deadline is interrupt-based and must leave the
    // worker reusable, so it runs before the cancel segment below poisons
    // the realm (a poisoned worker exits; nothing can run after it).
    const cpuTimeout = await request(1023, '/runtime/cpu-timeout');
    assert.match(
        cpuTimeout.error,
        /^(TimeoutError|RuntimeError): /,
        'synchronous CPU deadline must surface as a runtime timeout/error',
    );
    await assertReusable(1024, 'after-cpu-timeout');

    // Cancellation must end the realm: the detached continuation may never
    // run, and the worker must poison itself.
    send([
        'CANCEL_CONTINUATION',
        1022,
        hex(encoder.encode(
            'https://compat.example/runtime/ownership-cancel',
        )),
        hex(encoder.encode('capsid-owner:after-cancel')),
    ].join(' '));
    assert.equal(await nextLine(), 'CANCELED 1022');
    assert.equal(
        await nextLine(),
        'EXITED 1022',
        'canceled request must end the worker (poison)',
    );
    const cancelEvents = await consumeEvents(1022);
    assert.ok(
        !cancelEvents.some(event =>
            event.kind === 'LOG' &&
            decoder.decode(unhex(event.text)) === 'capsid-owner:after-cancel'),
        'after-cancel continuation must never run',
    );

    child.stdin.end('STOP\n');
    assert.equal(await exitPromise, 0, `${variant}: driver exit`);
} catch (cause) {
    child.kill('SIGKILL');
    await exitPromise.catch(() => {});
    throw new Error(`itty-router lifecycle failure: variant=${variant}`, {
        cause,
    });
}

console.log(
    `PASS: itty-router ${variant} cancellation, concurrency, timeout and reuse`,
);
