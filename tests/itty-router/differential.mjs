import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { createServer } from 'node:http';
import { createInterface } from 'node:readline';
import { createApplication } from '../../examples/itty-router-reference/src/shared-handlers.js';
import { vectors } from './vectors.mjs';

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

const hex = bytes => {
    let result = '';
    for (const byte of bytes) {
        result += byte.toString(16).padStart(2, '0');
    }
    return result || '-';
};

const unhex = text => {
    if (text === '-') {
        return new Uint8Array();
    }
    if (text.length % 2 !== 0 || /[^0-9a-f]/i.test(text)) {
        throw new Error(`invalid driver hex: ${text}`);
    }
    const output = new Uint8Array(text.length / 2);
    for (let index = 0; index < output.length; ++index) {
        output[index] = Number.parseInt(
            text.slice(index * 2, index * 2 + 2),
            16,
        );
    }
    return output;
};

const encodeU32 = value => [
    value & 0xff,
    (value >>> 8) & 0xff,
    (value >>> 16) & 0xff,
    (value >>> 24) & 0xff,
];

const encodeHeaders = headers => {
    const chunks = [ encodeU32(headers.length) ];
    for (const [ name, value ] of headers) {
        const nameBytes = encoder.encode(name);
        const valueBytes = encoder.encode(value);
        chunks.push(
            encodeU32(nameBytes.length),
            [ ...nameBytes ],
            encodeU32(valueBytes.length),
            [ ...valueBytes ],
        );
    }
    return hex(new Uint8Array(chunks.flat()));
};

const decodeU32 = (bytes, offset) => {
    if (offset + 4 > bytes.length) {
        throw new Error('truncated driver header blob');
    }
    return (
        bytes[offset] |
        (bytes[offset + 1] << 8) |
        (bytes[offset + 2] << 16) |
        (bytes[offset + 3] << 24)
    ) >>> 0;
};

const decodeHeaders = encoded => {
    const bytes = unhex(encoded);
    let offset = 0;
    const count = decodeU32(bytes, offset);
    offset += 4;
    const output = [];
    for (let index = 0; index < count; ++index) {
        const nameSize = decodeU32(bytes, offset);
        offset += 4;
        const name = decoder.decode(
            bytes.subarray(offset, offset + nameSize),
        );
        offset += nameSize;
        const valueSize = decodeU32(bytes, offset);
        offset += 4;
        const value = decoder.decode(
            bytes.subarray(offset, offset + valueSize),
        );
        offset += valueSize;
        output.push([ name, value ]);
    }
    if (offset !== bytes.length) {
        throw new Error('trailing data in driver header blob');
    }
    return output;
};

const bodyChecksum = body => {
    let checksum = 0;
    for (const byte of body) {
        checksum = (checksum + byte) >>> 0;
    }
    return checksum;
};

const normalizeJsonBody = (body, vector) => {
    const parsed = JSON.parse(decoder.decode(body));
    for (const field of vector.ignoreBodyJsonFields ?? []) {
        parsed[field] = '<normalized>';
    }
    return parsed;
};

const normalize = (status, headerEntries, body, vector, error = '') => {
    const headers = headerEntries.map(([ originalName, originalValue ]) => {
        const name = originalName.toLowerCase();
        const value = name === 'date' ? '<date>' : originalValue;
        return [ name, value ];
    });
    headers.sort((left, right) =>
        left[0].localeCompare(right[0]) ||
        left[1].localeCompare(right[1]));

    let normalizedBody = vector.runtimeJsonExpected
        ? '<runtime-only-negative-assertion>'
        : hex(body);
    if (vector.ignoreBodyJsonFields?.length) {
        normalizedBody = JSON.stringify(normalizeJsonBody(body, vector));
    }
    return { status, headers, body: normalizedBody, error };
};

const headersToMap = entries => {
    const result = new Map();
    for (const [ name, value ] of entries) {
        result.set(name.toLowerCase(), value);
    }
    return result;
};

const resolveExpected = expected =>
    typeof expected === 'function' ? expected(variant) : expected;

const assertExpectation = (result, vector) => {
    const expected = vector.expect;
    if (Object.hasOwn(expected, 'status')) {
        assert.equal(result.status, expected.status, 'status');
    }
    if (Object.hasOwn(expected, 'bodyText')) {
        assert.equal(decoder.decode(result.body), expected.bodyText, 'body text');
    }
    if (Object.hasOwn(expected, 'bodyHex')) {
        assert.equal(hex(result.body), expected.bodyHex || '-', 'body hex');
    }
    if (Object.hasOwn(expected, 'bodyJson')) {
        const actualJson = normalizeJsonBody(result.body, vector);
        assert.deepEqual(
            actualJson,
            resolveExpected(expected.bodyJson),
            'body JSON',
        );
    }
    if (Object.hasOwn(expected, 'bodyLength')) {
        assert.equal(result.body.length, expected.bodyLength, 'body length');
    }
    if (Object.hasOwn(expected, 'bodyChecksum')) {
        assert.equal(
            bodyChecksum(result.body),
            expected.bodyChecksum,
            'body checksum',
        );
    }
    const headerMap = headersToMap(result.headers);
    for (const [ name, value ] of Object.entries(expected.headers ?? {})) {
        assert.equal(
            headerMap.get(name.toLowerCase()),
            value,
            `header ${name}`,
        );
    }
    for (const name of expected.absentHeaders ?? []) {
        assert.equal(
            headerMap.has(name.toLowerCase()),
            false,
            `absent header ${name}`,
        );
    }
};

const referenceApplication = createApplication(variant);
const referenceRequest = async (vector, activeUrl) => {
    const init = {
        method: vector.method,
        headers: vector.headers,
    };
    if (vector.method !== 'GET' && vector.method !== 'HEAD') {
        init.body = vector.body;
    }
    const response = await referenceApplication.fetch(
        new Request(activeUrl, init),
    );
    assert.ok(
        response instanceof Response,
        `${variant}/${vector.id}: reference did not return a Response`,
    );
    const headers = [];
    response.headers.forEach((value, name) => {
        headers.push([ name, value ]);
    });
    const body = new Uint8Array(await response.arrayBuffer());
    return {
        status: response.status,
        headers,
        body,
        error: '',
        normalized: normalize(response.status, headers, body, vector),
    };
};

const upstream = createServer((request, response) => {
    response.writeHead(203, {
        'content-type': 'text/plain',
        'x-itty-upstream': 'direct-itty-fetch',
    });
    response.end(`upstream:${request.url}`);
});
await new Promise((resolve, reject) => {
    upstream.once('error', reject);
    upstream.listen(0, '127.0.0.1', resolve);
});
upstream.unref();
const upstreamPort = upstream.address().port;
const upstreamUrl = `http://127.0.0.1:${upstreamPort}/itty-upstream`;

const child = spawn(driverPath, [
    workerPath,
    bundlePath,
    '--loopback-port',
    String(upstreamPort),
], {
    stdio: [ 'pipe', 'pipe', 'inherit' ],
});
const lines = createInterface({ input: child.stdout, crlfDelay: Infinity })[
    Symbol.asyncIterator
]();

const nextLine = async () => {
    const result = await lines.next();
    if (result.done) {
        throw new Error('itty-router worker driver exited unexpectedly');
    }
    if (result.value.startsWith('FATAL ')) {
        throw new Error(decoder.decode(unhex(result.value.slice(6))));
    }
    return result.value;
};

assert.equal(await nextLine(), 'READY', 'driver must reach worker READY');

const diagnostic = (vector, reference, runtime, cause) => {
    const path = new URL(vector.url).pathname;
    const referenceTrace = reference
        ? headersToMap(reference.headers).get('x-execution-trace') ?? null
        : null;
    const runtimeTrace = runtime
        ? headersToMap(runtime.headers).get('x-execution-trace') ?? null
        : null;
    return new Error([
        'itty-router differential failure',
        `variant=${variant}`,
        `vector=${vector.id}`,
        `route=${vector.method} ${path}`,
        `reference=${reference ? JSON.stringify(reference.normalized) : '<unavailable>'}`,
        `runtime=${runtime ? JSON.stringify(runtime.normalized) : '<unavailable>'}`,
        `executionTrace=${JSON.stringify({
            reference: referenceTrace,
            runtime: runtimeTrace,
        })}`,
        cause.message,
    ].join('\n'), { cause });
};

let requestId = 1;
let absoluteChecks = 0;
for (const vector of vectors) {
    const activeUrl = vector.outboundFetch
        ? `${vector.url}?url=${encodeURIComponent(upstreamUrl)}`
        : vector.url;
    let reference;
    let runtime;
    try {
        reference = await referenceRequest(vector, activeUrl);

        const command = [
            'REQUEST',
            requestId,
            hex(encoder.encode(vector.method)),
            hex(encoder.encode(activeUrl)),
            encodeHeaders(vector.headers),
            hex(vector.body),
            vector.requestChunkSize,
        ].join(' ');
        child.stdin.write(`${command}\n`);

        const line = await nextLine();
        const fields = line.split(' ');
        if (fields[0] !== 'RESULT' || Number(fields[1]) !== requestId) {
            throw new Error(`invalid driver result: ${line}`);
        }
        const runtimeBody = unhex(fields[4]);
        const runtimeHeaders = decodeHeaders(fields[3]);
        const runtimeError = decoder.decode(unhex(fields[5]));
        runtime = {
            status: Number(fields[2]),
            headers: runtimeHeaders,
            body: runtimeBody,
            error: runtimeError,
            normalized: normalize(
                Number(fields[2]),
                runtimeHeaders,
                runtimeBody,
                vector,
                runtimeError,
            ),
        };

        assertExpectation(reference, vector);
        assertExpectation(runtime, vector);
        if (Object.keys(vector.expect).length > 0) {
            absoluteChecks += 1;
        }
        if (vector.runtimeJsonExpected) {
            assert.deepEqual(
                JSON.parse(decoder.decode(runtimeBody)),
                vector.runtimeJsonExpected,
                'forbidden globals must be absent in Capsid',
            );
        }
        assert.deepEqual(runtime.normalized, reference.normalized);
    } catch (cause) {
        throw diagnostic(vector, reference, runtime, cause);
    }
    requestId += 1;
}

child.stdin.end('STOP\n');
const exitCode = await new Promise((resolve, reject) => {
    child.once('error', reject);
    child.once('exit', resolve);
});
assert.equal(exitCode, 0, 'itty-router worker driver exit status');
await new Promise(resolve => upstream.close(resolve));
assert.ok(
    absoluteChecks > 0,
    'no itty-router absolute expectation was exercised',
);
console.log(
    `PASS: ${variant} ${vectors.length} itty-router vectors ` +
    `(${absoluteChecks} with independent absolute assertions)`,
);
