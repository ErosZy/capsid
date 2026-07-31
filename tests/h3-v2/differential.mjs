import assert from 'node:assert/strict';
import http from 'node:http';
import path from 'node:path';
import { pathToFileURL } from 'node:url';
import {
    FrameworkWorker,
    decoder,
    hex,
} from './protocol.mjs';
import { vectors } from './vectors.mjs';

const args = new Map();
for (let index = 2; index < process.argv.length; index += 2) {
    args.set(process.argv[index], process.argv[index + 1]);
}

const driverPath = args.get('--driver');
const workerPath = args.get('--worker');
const bundlePath = args.get('--bundle');
const selectedId = args.get('--id');
const smoke = args.has('--smoke');
if (!driverPath || !workerPath || !bundlePath) {
    throw new Error('expected --driver, --worker and --bundle');
}

const checksum = bytes => {
    let output = 0;
    for (const byte of bytes) {
        output = (output + byte) >>> 0;
    }
    return output;
};

const stableValue = value => {
    if (Array.isArray(value)) {
        return value.map(stableValue);
    }
    if (value && typeof value === 'object') {
        return Object.fromEntries(
            Object.keys(value).sort().map(key => [
                key,
                stableValue(value[key]),
            ]),
        );
    }
    return value;
};

const normalizeCookie = value =>
    value.replace(/^([^=]+)=([^;]*)/, '$1=<normalized>');

const normalizeResult = (result, vector) => {
    const regularHeaders = [];
    const setCookie = [];
    for (const [ rawName, rawValue ] of result.headers) {
        const name = rawName.toLowerCase();
        if (name === 'set-cookie') {
            setCookie.push(
                vector.normalizeSetCookieValues
                    ? normalizeCookie(rawValue)
                    : rawValue,
            );
            continue;
        }
        regularHeaders.push([
            name,
            name === 'date' ? '<date>' : rawValue,
        ]);
    }
    regularHeaders.sort(([ firstName, firstValue ], [
        secondName,
        secondValue,
    ]) => firstName.localeCompare(secondName) ||
        firstValue.localeCompare(secondValue));

    let body = result.body;
    if (vector.normalizeBodyJsonFields?.length) {
        const value = JSON.parse(decoder.decode(body));
        for (const field of vector.normalizeBodyJsonFields) {
            if (Object.hasOwn(value, field)) {
                value[field] = '<normalized>';
            }
        }
        body = new TextEncoder().encode(JSON.stringify(stableValue(value)));
    }

    return {
        status: result.status,
        statusText: result.statusText,
        headers: regularHeaders,
        setCookie,
        bodyHex: hex(body),
        error: result.error,
    };
};

const responseHeaders = response => {
    const output = [];
    response.headers.forEach((value, name) => {
        if (name.toLowerCase() !== 'set-cookie') {
            output.push([ name, value ]);
        }
    });
    for (const value of response.headers.getSetCookie()) {
        output.push([ 'set-cookie', value ]);
    }
    return output;
};

const requestInit = vector => {
    const init = {
        method: vector.method,
        headers: vector.headers,
    };
    if (vector.method !== 'GET' && vector.method !== 'HEAD') {
        init.body = vector.body;
    }
    return init;
};

const referenceRequest = async (fetchHandler, vector, url) => {
    try {
        const response = await fetchHandler(
            new Request(url, requestInit(vector)),
        );
        return {
            status: response.status,
            statusText: response.statusText,
            headers: responseHeaders(response),
            body: new Uint8Array(await response.arrayBuffer()),
            error: '',
        };
    } catch (error) {
        return {
            status: 0,
            statusText: '',
            headers: [],
            body: new Uint8Array(),
            error: `${error?.name ?? 'Error'}: ${error?.message ?? error}`,
        };
    }
};

const headerValues = (result, name) => result.headers
    .filter(([ headerName ]) => headerName.toLowerCase() === name)
    .map(([, value ]) => value);

const deepIncludes = (actual, expected, path = '$') => {
    if (!expected || typeof expected !== 'object') {
        assert.deepEqual(actual, expected, path);
        return;
    }
    if (Array.isArray(expected)) {
        assert.ok(Array.isArray(actual), `${path} must be an array`);
        assert.equal(actual.length, expected.length, `${path}.length`);
        expected.forEach((value, index) => {
            deepIncludes(actual[index], value, `${path}[${index}]`);
        });
        return;
    }
    assert.ok(
        actual && typeof actual === 'object' && !Array.isArray(actual),
        `${path} must be an object`,
    );
    for (const [ key, value ] of Object.entries(expected)) {
        assert.ok(Object.hasOwn(actual, key), `${path}.${key} is missing`);
        deepIncludes(actual[key], value, `${path}.${key}`);
    }
};

const assertExpectations = (result, vector, environment) => {
    const { expect } = vector;
    const prefix = `${environment}:${vector.id}`;
    if (expect.status !== undefined) {
        assert.equal(result.status, expect.status, `${prefix}: status`);
    }
    if (expect.statusText !== undefined) {
        assert.equal(
            result.statusText,
            expect.statusText,
            `${prefix}: statusText`,
        );
    }
    assert.equal(result.error, '', `${prefix}: transport/runtime error`);

    const bodyText = decoder.decode(result.body);
    if (expect.bodyText !== undefined) {
        assert.equal(bodyText, expect.bodyText, `${prefix}: body text`);
    }
    if (expect.bodyHex !== undefined) {
        assert.equal(hex(result.body), expect.bodyHex, `${prefix}: body hex`);
    }
    if (expect.bodyLength !== undefined) {
        assert.equal(
            result.body.byteLength,
            expect.bodyLength,
            `${prefix}: body length`,
        );
    }
    if (expect.bodyChecksum !== undefined) {
        assert.equal(
            checksum(result.body),
            expect.bodyChecksum,
            `${prefix}: body checksum`,
        );
    }
    if (expect.bodyJson !== undefined) {
        assert.deepEqual(
            JSON.parse(bodyText),
            expect.bodyJson,
            `${prefix}: JSON body`,
        );
    }
    if (expect.bodyJsonIncludes !== undefined) {
        deepIncludes(
            JSON.parse(bodyText),
            expect.bodyJsonIncludes,
            `${prefix}: JSON body`,
        );
    }
    for (const excluded of expect.bodyExcludes ?? []) {
        assert.equal(
            bodyText.includes(excluded),
            false,
            `${prefix}: body must exclude ${JSON.stringify(excluded)}`,
        );
    }
    for (const [ name, value ] of Object.entries(expect.headers ?? {})) {
        assert.deepEqual(
            headerValues(result, name.toLowerCase()),
            [ value ],
            `${prefix}: header ${name}`,
        );
    }
    for (const name of expect.absentHeaders ?? []) {
        assert.deepEqual(
            headerValues(result, name.toLowerCase()),
            [],
            `${prefix}: absent header ${name}`,
        );
    }
    if (expect.setCookie !== undefined) {
        assert.deepEqual(
            headerValues(result, 'set-cookie'),
            expect.setCookie,
            `${prefix}: ordered Set-Cookie`,
        );
    }
    if (expect.setCookieCount !== undefined) {
        assert.equal(
            headerValues(result, 'set-cookie').length,
            expect.setCookieCount,
            `${prefix}: Set-Cookie count`,
        );
    }
};

const resultSummary = result => ({
    status: result.status,
    statusText: result.statusText,
    headers: result.headers,
    body: {
        byteLength: result.body.byteLength,
        checksum: checksum(result.body),
        textPrefix: decoder.decode(result.body.subarray(0, 512)),
        hexPrefix: hex(result.body.subarray(0, 128)),
    },
    error: result.error,
});

const traceFrom = (result, name) =>
    headerValues(result, name).join(' | ') || '<absent>';

const diagnostic = ({
    vector,
    url,
    reference,
    runtime,
    worker,
    cause,
}) => JSON.stringify({
    failure: cause?.message ?? String(cause),
    route: new URL(url).pathname,
    requestVector: {
        id: vector.id,
        method: vector.method,
        url,
        headers: vector.headers,
        bodyLength: vector.body.byteLength,
        bodyChecksum: checksum(vector.body),
    },
    referenceResult: reference ? resultSummary(reference) : '<runtime-only>',
    runtimeResult: runtime ? resultSummary(runtime) : '<unavailable>',
    middlewareTrace: {
        reference: reference
            ? traceFrom(reference, 'x-h3-global-middleware')
            : '<runtime-only>',
        runtime: runtime
            ? traceFrom(runtime, 'x-h3-global-middleware')
            : '<unavailable>',
    },
    hookTrace: {
        reference: reference
            ? traceFrom(reference, 'x-h3-hook-trace')
            : '<runtime-only>',
        runtime: runtime
            ? traceFrom(runtime, 'x-h3-hook-trace')
            : '<unavailable>',
    },
    workerLifecycleState: worker.lifecycleState,
}, null, 2);

const startUpstream = async () => {
    const server = http.createServer((request, response) => {
        response.statusCode = 203;
        response.statusMessage = 'Non-Authoritative Information';
        response.setHeader('content-type', 'text/plain');
        response.setHeader('x-h3-upstream', 'direct-h3-fetch');
        response.end(`upstream:${request.url}`);
    });
    await new Promise((resolve, reject) => {
        server.once('error', reject);
        server.listen(0, '127.0.0.1', resolve);
    });
    const address = server.address();
    assert.ok(address && typeof address === 'object');
    return {
        server,
        url: `http://127.0.0.1:${address.port}/h3-upstream`,
        port: address.port,
    };
};

const closeServer = server => new Promise((resolve, reject) => {
    server.close(error => error ? reject(error) : resolve());
});

const selectedVectors = vectors.filter(vector => {
    if (selectedId) {
        return vector.id === selectedId;
    }
    return !smoke || vector.id === 'entry';
});
if (selectedVectors.length === 0) {
    throw new Error(`no H3 vector selected: ${selectedId}`);
}

const referenceModule = await import(pathToFileURL(
    path.resolve(bundlePath),
).href);
const referenceTarget = referenceModule.default;
const fetchHandler = typeof referenceModule.fetch === 'function'
    ? referenceModule.fetch
    : typeof referenceTarget?.fetch === 'function'
        ? referenceTarget.fetch.bind(referenceTarget)
        : undefined;
if (!fetchHandler) {
    throw new Error('H3 reference bundle does not export a fetch handler');
}
const upstream = await startUpstream();
const worker = new FrameworkWorker({
    driverPath,
    workerPath,
    bundlePath,
    flags: [ '--loopback-port', String(upstream.port) ],
});

let passed = 0;
let absoluteChecks = 0;
try {
    await worker.start();
    for (let index = 0; index < selectedVectors.length; ++index) {
        const vector = selectedVectors[index];
        const urlObject = new URL(vector.url);
        if (vector.outbound) {
            urlObject.searchParams.set('url', upstream.url);
        }
        const url = urlObject.href;
        let reference;
        let runtime;
        try {
            if (!vector.runtimeOnly) {
                reference = await referenceRequest(fetchHandler, vector, url);
                assertExpectations(reference, vector, 'reference');
            }
            runtime = await worker.request({
                id: index + 1,
                method: vector.method,
                url,
                headers: vector.headers,
                body: vector.body,
                chunkSize: vector.chunkSize,
            });
            assertExpectations(runtime, vector, 'runtime');
            if (Object.keys(vector.expect).length > 0) {
                absoluteChecks += 1;
            }
            if (reference) {
                assert.deepEqual(
                    normalizeResult(runtime, vector),
                    normalizeResult(reference, vector),
                    `differential:${vector.id}`,
                );
            }
            passed += 1;
        } catch (cause) {
            throw new Error(diagnostic({
                vector,
                url,
                reference,
                runtime,
                worker,
                cause,
            }), { cause });
        }
    }
} finally {
    await worker.stop().catch(async () => worker.kill());
    await closeServer(upstream.server);
}

assert.ok(
    absoluteChecks > 0,
    'no H3 absolute expectation was exercised',
);
console.log(
    `PASS: H3 v2 differential ${passed}/${selectedVectors.length} vectors ` +
    `(${absoluteChecks} with independent absolute assertions)`,
);
