import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { createServer } from 'node:http';
import { createInterface } from 'node:readline';
import { inflateSync } from 'node:zlib';
import { app } from '../../examples/elysia-reference/src/app.js';
import { absoluteExpectations, smokeVector, vectors } from './vectors.mjs';

/*
 * Check a result against spec-derived absolute values, independently of the
 * runtime-vs-reference diff. Applied to both environments so that a behaviour
 * which is wrong identically on both sides still fails.
 */
const assertAbsolute = (normalized, vector, environment) => {
    const expected = absoluteExpectations[vector.id];
    if (!expected) {
        return;
    }
    if (Object.hasOwn(expected, 'status')) {
        assert.equal(
            normalized.status,
            expected.status,
            `${vector.id}: ${environment} status must be ${expected.status} ` +
                'per absolute expectation (not merely equal to the other ' +
                'environment)',
        );
    }
};

const args = new Map();
for (let index = 2; index < process.argv.length; index += 2) {
    args.set(process.argv[index], process.argv[index + 1]);
}

const driverPath = args.get('--driver');
const workerPath = args.get('--worker');
const bundlePath = args.get('--bundle');
const smokeOnly = args.get('--smoke') === 'true';
if (!driverPath || !workerPath || !bundlePath) {
    throw new Error('expected --driver, --worker and --bundle');
}

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
        output[index] = Number.parseInt(text.slice(index * 2, index * 2 + 2), 16);
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
        const nameBytes = new TextEncoder().encode(name);
        const valueBytes = new TextEncoder().encode(value);
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
        const name = new TextDecoder().decode(
            bytes.subarray(offset, offset + nameSize),
        );
        offset += nameSize;
        const valueSize = decodeU32(bytes, offset);
        offset += 4;
        const value = new TextDecoder().decode(
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

const normalizeHeaderValue = (name, value) => {
    if (name === 'date') {
        return '<date>';
    }
    if (name === 'server-timing') {
        return value.replace(/dur=[0-9.]+/g, 'dur=<duration>');
    }
    return value;
};

const normalize = (status, headerEntries, body, vector) => {
    const headers = [];
    const setCookie = [];
    for (const [ originalName, originalValue ] of headerEntries) {
        const name = originalName.toLowerCase();
        if (name === 'set-cookie') {
            setCookie.push(originalValue);
        } else {
            headers.push([ name, normalizeHeaderValue(name, originalValue) ]);
        }
    }
    headers.sort((left, right) =>
        left[0].localeCompare(right[0]) || left[1].localeCompare(right[1]));

    let normalizedBody = vector.runtimeJsonExpected
        ? '<runtime-only-negative-assertion>'
        : hex(vector.decompressBody === 'deflate' ? inflateSync(body) : body);
    // Text-level normalization for bodies whose framing is nondeterministic
    // across environments (e.g. SSE id lines carrying a random nanoid).
    if (vector.bodyTextReplacements?.length) {
        let text = new TextDecoder().decode(body);
        for (const [ pattern, replacement ] of vector.bodyTextReplacements) {
            text = text.replace(pattern, replacement);
        }
        normalizedBody = hex(new TextEncoder().encode(text));
    }
    if (vector.ignoreBodyJsonFields?.length) {
        const parsed = JSON.parse(new TextDecoder().decode(body));
        for (const field of vector.ignoreBodyJsonFields) {
            // Only normalize fields that are actually present: error bodies
            // must not gain phantom keys from the normalization itself.
            if (field in parsed) {
                parsed[field] = '<normalized>';
            }
        }
        normalizedBody = JSON.stringify(parsed);
    }
    return { status, headers, setCookie, body: normalizedBody };
};

// Elysia 1.4 has no app.request: the handler is app.fetch, called directly
// with a standard Request.
const referenceRequest = async (vector, headers) => {
    const init = { method: vector.method, headers };
    if (vector.method !== 'GET' && vector.method !== 'HEAD') {
        init.body = vector.body;
    }
    const response = await app.fetch(new Request(vector.url, init));
    const entries = [];
    response.headers.forEach((value, name) => {
        if (name !== 'set-cookie') {
            entries.push([ name, value ]);
        }
    });
    for (const value of response.headers.getSetCookie()) {
        entries.push([ 'set-cookie', value ]);
    }
    return {
        response,
        normalized: normalize(
            response.status,
            entries,
            new Uint8Array(await response.arrayBuffer()),
            vector,
        ),
    };
};

const upstream = createServer((request, response) => {
    response.writeHead(203, {
        'content-type': 'text/plain',
        'x-elysia-upstream': 'direct-txiki-fetch',
    });
    response.end(`upstream:${request.url}`);
});
await new Promise((resolve, reject) => {
    upstream.once('error', reject);
    upstream.listen(0, '127.0.0.1', resolve);
});
upstream.unref();
const upstreamAddress = upstream.address();
const upstreamPort = upstreamAddress.port;
const upstreamUrl = `http://127.0.0.1:${upstreamPort}/elysia-upstream`;

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
        throw new Error('Elysia worker driver exited unexpectedly');
    }
    if (result.value.startsWith('FATAL ')) {
        throw new Error(new TextDecoder().decode(unhex(result.value.slice(6))));
    }
    return result.value;
};

assert.equal(await nextLine(), 'READY', 'driver must reach worker READY');

const selectedVectors = smokeOnly ? [ smokeVector ] : vectors;

// Guard against absoluteExpectations drifting away from the vector list: a key
// that matches no vector would silently never be checked.
{
    const knownIds = new Set(vectors.map(item => item.id));
    const orphans = Object.keys(absoluteExpectations).filter(
        id => !knownIds.has(id));
    assert.deepEqual(
        orphans,
        [],
        'absoluteExpectations keys must all name a known vector',
    );
    assert.ok(
        Object.keys(absoluteExpectations).length > 0,
        'absoluteExpectations must not be empty',
    );
}

let requestId = 1;
let absoluteChecks = 0;
for (const vector of selectedVectors) {
    const activeVector = vector.outboundFetch ? {
        ...vector,
        url: `${vector.url}?url=${encodeURIComponent(upstreamUrl)}`,
    } : vector;
    const reference = await referenceRequest(activeVector, vector.headers);
    assertAbsolute(reference.normalized, activeVector, 'reference');

    const command = [
        'REQUEST',
        requestId,
        hex(new TextEncoder().encode(activeVector.method)),
        hex(new TextEncoder().encode(activeVector.url)),
        encodeHeaders(vector.headers),
        hex(activeVector.body),
        activeVector.requestChunkSize,
    ].join(' ');
    child.stdin.write(`${command}\n`);

    const line = await nextLine();
    const fields = line.split(' ');
    if (fields[0] !== 'RESULT' || Number(fields[1]) !== requestId) {
        throw new Error(`${vector.id}: invalid driver result: ${line}`);
    }
    const error = new TextDecoder().decode(unhex(fields[5]));
    if (error) {
        throw new Error(
            `${activeVector.id} ${new URL(activeVector.url).pathname}: runtime ${error}`,
        );
    }
    const runtimeBody = unhex(fields[4]);
    if (activeVector.runtimeJsonExpected) {
        assert.deepEqual(
            JSON.parse(new TextDecoder().decode(runtimeBody)),
            activeVector.runtimeJsonExpected,
            `${activeVector.id}: forbidden globals must be absent in Capsid`,
        );
    }
    const runtime = normalize(
        Number(fields[2]),
        decodeHeaders(fields[3]),
        runtimeBody,
        activeVector,
    );
    assertAbsolute(runtime, activeVector, 'runtime');
    if (absoluteExpectations[activeVector.id]) {
        absoluteChecks += 1;
    }
    try {
        assert.deepEqual(runtime, reference.normalized);
    } catch (error_) {
        throw new Error([
            `Elysia differential mismatch`,
            `vector=${activeVector.id}`,
            `route=${activeVector.method} ${new URL(activeVector.url).pathname}`,
            `reference=${JSON.stringify(reference.normalized)}`,
            `runtime=${JSON.stringify(runtime)}`,
            error_.message,
        ].join('\n'), { cause: error_ });
    }
    requestId += 1;
}

child.stdin.end('STOP\n');
const exitCode = await new Promise((resolve, reject) => {
    child.once('error', reject);
    child.once('exit', resolve);
});
assert.equal(exitCode, 0, 'Elysia worker driver exit status');
await new Promise(resolve => upstream.close(resolve));
// A full run must have exercised the absolute anchors; if none fired, the
// independent layer silently contributed nothing.
if (!smokeOnly) {
    assert.ok(
        absoluteChecks > 0,
        'no absolute expectation was exercised; the independent assertion ' +
            'layer contributed no coverage',
    );
}
console.log(
    `PASS: ${selectedVectors.length} Elysia differential vectors ` +
        `(${absoluteChecks} with independent absolute assertions)`,
);
