import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import fs from 'node:fs';
import http from 'node:http';
import os from 'node:os';
import path from 'node:path';
import { setTimeout as delay } from 'node:timers/promises';
import { pathToFileURL } from 'node:url';

function parseArgs(argv) {
    const values = new Map();
    for (let index = 0; index < argv.length; index += 2) {
        const key = argv[index];
        const value = argv[index + 1];
        assert.ok(key?.startsWith('--') && value, `invalid argument: ${key}`);
        values.set(key.slice(2), value);
    }
    for (const required of [ 'host', 'worker', 'bundle' ]) {
        assert.ok(values.has(required), `missing --${required}`);
    }
    return values;
}

function readLine(stream, child, timeoutMs, stderrText) {
    return new Promise((resolve, reject) => {
        let buffer = '';
        const timer = setTimeout(() => {
            cleanup();
            reject(new Error(`readiness timeout; stderr=${stderrText()}`));
        }, timeoutMs);
        const cleanup = () => {
            clearTimeout(timer);
            stream.off('data', onData);
            child.off('exit', onExit);
        };
        const onData = (chunk) => {
            buffer += chunk;
            const newline = buffer.indexOf('\n');
            if (newline !== -1) {
                cleanup();
                resolve(buffer.slice(0, newline));
            }
        };
        const onExit = (code, signal) => {
            cleanup();
            reject(new Error(
                `host exited before READY: code=${code} signal=${signal}; ` +
                `stderr=${stderrText()}`,
            ));
        };
        stream.setEncoding('utf8');
        stream.on('data', onData);
        child.on('exit', onExit);
    });
}

function request(port, {
    method = 'GET',
    target,
    headers = {},
    body,
    agent = false,
    timeoutMs = 5000,
}) {
    return new Promise((resolve, reject) => {
        const req = http.request({
            host: '127.0.0.1',
            port,
            method,
            path: target,
            agent,
            headers: {
                connection: agent === false ? 'close' : 'keep-alive',
                host: 'client-controlled.example',
                ...headers,
            },
        });
        req.setTimeout(timeoutMs, () => {
            req.destroy(new Error(`HTTP timeout for ${method} ${target}`));
        });
        req.on('error', reject);
        req.on('response', (response) => {
            const chunks = [];
            const clientPort = response.socket.localPort;
            response.on('data', (chunk) => chunks.push(chunk));
            response.on('aborted', () => {
                // A truncated chunked response (the Host closes the
                // connection mid-stream) must reject, never hang the CTest.
                reject(new Error(
                    `response aborted before completion for ${method} ${target}`,
                ));
            });
            response.on('end', () => resolve({
                status: response.statusCode,
                headers: response.headers,
                rawHeaders: response.rawHeaders,
                body: Buffer.concat(chunks),
                clientPort,
            }));
        });
        if (body) {
            req.end(body);
        } else {
            req.end();
        }
    });
}

function waitForExit(child, timeoutMs) {
    return new Promise((resolve, reject) => {
        if (child.exitCode !== null || child.signalCode !== null) {
            resolve({ code: child.exitCode, signal: child.signalCode });
            return;
        }
        const timer = setTimeout(() => {
            child.kill('SIGKILL');
            reject(new Error('capsid-host did not exit after SIGTERM'));
        }, timeoutMs);
        child.once('exit', (code, signal) => {
            clearTimeout(timer);
            resolve({ code, signal });
        });
    });
}

// The frozen M1A CLI as an argument list, with the scenario knobs exposed
// for the startup-failure regressions.
function hostArgs(args, { requestTimeoutMs, listen = '127.0.0.1:0' } = {}) {
    const bundle = path.resolve(args.get('bundle'));
    return [
        '--mode', 'single-worker',
        '--worker', path.resolve(args.get('worker')),
        '--source-bundle', bundle,
        '--source-name', pathToFileURL(bundle).href,
        '--application', 'orders',
        '--listen', listen,
        '--routing', 'path',
        '--public-scheme', 'http',
        '--public-authority', 'public.example',
        '--request-timeout-ms', String(requestTimeoutMs),
        '--initial-stream-window', '1024',
        '--strict-sandbox', 'off',
        '--ready-fd', '3',
    ];
}

// Starts a capsid-host in single-worker mode and resolves with its READY
// state. requestTimeoutMs selects the scenario: the correctness phase runs
// with a loose deadline (streaming correctness is asserted, not wall-clock
// throughput), and the timeout phase runs the frozen 100ms deadline against
// an independent slow endpoint.
async function startHost(args, { requestTimeoutMs }) {
    const child = spawn(args.get('host'),
        hostArgs(args, { requestTimeoutMs }),
        {
            stdio: [ 'ignore', 'pipe', 'pipe', 'pipe' ],
        });
    let stdout = '';
    let stderr = '';
    child.stdout.setEncoding('utf8');
    child.stderr.setEncoding('utf8');
    child.stdout.on('data', (chunk) => { stdout += chunk; });
    child.stderr.on('data', (chunk) => { stderr += chunk; });
    const readyLine = await readLine(child.stdio[3], child, 10000, () => stderr);
    const ready = JSON.parse(readyLine);
    assert.equal(ready.schema, 'capsid-host-ready-v1');
    assert.equal(ready.app, 'orders');
    assert.equal(ready.address, '127.0.0.1');
    assert.ok(Number.isInteger(ready.port) && ready.port > 0 && ready.port < 65536);
    assert.equal(readyLine, JSON.stringify({
        schema: 'capsid-host-ready-v1',
        app: 'orders',
        address: '127.0.0.1',
        port: ready.port,
    }), 'READY record is not canonical or has uncontracted fields');
    return {
        child,
        port: ready.port,
        stdoutText: () => stdout,
        stderrText: () => stderr,
    };
}

async function stopHost(host) {
    host.child.kill('SIGTERM');
    const exited = await waitForExit(host.child, 3000);
    assert.ok(
        exited.code === 0 || exited.signal === 'SIGTERM',
        `unexpected host exit: ${JSON.stringify(exited)}; stderr=${host.stderrText()}`,
    );
    assert.equal(host.stdoutText(), '', 'capsid-host wrote readiness or logs to stdout');
    // The Host must never report an internal state mismatch: a frame that
    // reached the Runtime ABI after the Runtime forgot its request (late
    // credit/end/body) is a Host-side race, not a worker failure.
    for (const pattern of [
        /grant_response_credit failed/,
        /write_request failed/,
        /end_request failed/,
        /begin_request failed/,
        /invalid IPC frame/,
    ]) {
        assert.doesNotMatch(
            host.stderrText(),
            pattern,
            `host reported an internal state error: ${pattern}`,
        );
    }
}

const args = parseArgs(process.argv.slice(2));

// ---- Startup-failure regressions: CLI-phase validation must fail before
// anything is spawned, and a post-spawn failure (the READY record write)
// must tear the worker down and exit within a bounded time.
{
    const invalidAddress = spawn(args.get('host'),
        hostArgs(args, {
            requestTimeoutMs: 10000,
            listen: '999.999.999.999:0',
        }), {
            stdio: [ 'ignore', 'pipe', 'pipe', 'ignore' ],
        });
    const invalidExit = await waitForExit(invalidAddress, 5000);
    assert.equal(
        invalidExit.code,
        2,
        'invalid listen address must fail the CLI phase before spawn',
    );
}
{
    // fd 3 is a read-only descriptor: the READY record write fails after
    // the worker spawned and became READY, and the Host must exit with
    // code 1 instead of hanging on the worker thread.
    const readOnly = fs.openSync(process.platform === 'win32' ? '\\\\.\\NUL' : '/dev/null', 'r');
    try {
        const brokenReady = spawn(args.get('host'),
            hostArgs(args, { requestTimeoutMs: 10000 }),
            {
                stdio: [ 'ignore', 'pipe', 'pipe', readOnly ],
            });
        const brokenExit = await waitForExit(brokenReady, 5000);
        assert.equal(
            brokenExit.code,
            1,
            'READY-fd write failure must exit with code 1',
        );
    } finally {
        fs.closeSync(readOnly);
    }
}

// ---- Phase A: correctness under a loose worker deadline. The 96 KiB
// stream and the 80 KiB echo must be byte-exact; their wall-clock time is
// not asserted here (the sanitizer builds take ~2.5s on the same credit
// rounds at a 1 KiB window).
const correctness = await startHost(args, { requestTimeoutMs: 10000 });
try {
    const inspect = await request(correctness.port, {
        target: '/@capsid/orders/inspect?x=%2Fcart',
        headers: {
            'capsid-app': [ 'spoof-a', 'spoof-b' ],
            forwarded: 'for=203.0.113.1;proto=https',
            'x-forwarded-proto': 'https',
            'x-trace': 'kept',
        },
    });
    assert.equal(inspect.status, 200);
    const observed = JSON.parse(inspect.body.toString('utf8'));
    assert.deepEqual(observed, {
        url: 'http://public.example/inspect?x=%2Fcart',
        host: null,
        forwarded: null,
        forwardedProto: null,
        capsidApp: null,
        trace: 'kept',
    });

    const fixed = await request(correctness.port, {
        target: '/@capsid/orders/fixed',
    });
    assert.equal(fixed.status, 200);
    assert.equal(fixed.body.length, 1024);
    assert.ok(fixed.body.every((byte) => byte === 0x78));

    const fixedString = await request(correctness.port, {
        target: '/@capsid/orders/fixed-string',
    });
    assert.equal(fixedString.status, 200);
    assert.equal(fixedString.body.length, 1024);
    assert.ok(fixedString.body.every((byte) => byte === 0x78));
    assert.equal(fixedString.headers['content-length'], '1024');
    assert.equal(fixedString.headers['transfer-encoding'], undefined);

    const fixedString16k = await request(correctness.port, {
        target: '/@capsid/orders/fixed-string-16k',
    });
    assert.equal(fixedString16k.status, 200);
    assert.equal(fixedString16k.body.length, 16 * 1024);
    assert.ok(fixedString16k.body.every((byte) => byte === 0x79));
    assert.equal(fixedString16k.headers['content-length'], undefined);
    assert.equal(fixedString16k.headers['transfer-encoding'], 'chunked');

    // The JS-side hint uses UTF-16 code units. Native must still reject the
    // fixed path when exact UTF-8 encoding expands beyond the 4 KiB bound.
    const fixedStringUtf8Overflow = await request(correctness.port, {
        target: '/@capsid/orders/fixed-string-utf8-overflow',
    });
    assert.equal(fixedStringUtf8Overflow.status, 200);
    assert.equal(fixedStringUtf8Overflow.body.length, 3 * 2048);
    assert.equal(fixedStringUtf8Overflow.headers['content-length'], undefined);
    assert.equal(fixedStringUtf8Overflow.headers['transfer-encoding'], 'chunked');

    const parallel = await Promise.all(Array.from({ length: 16 }, () =>
        request(correctness.port, { target: '/@capsid/orders/fixed' })));
    const parallelBad = parallel.filter((response) =>
        response.status !== 200 || response.body.length !== 1024);
    assert.deepEqual(parallelBad, [],
        `parallel /fixed responses failed; host stderr=${correctness.stderrText()}`);

    const keepAliveAgent = new http.Agent({ keepAlive: true, maxSockets: 1 });
    try {
        const keepAliveFirst = await request(correctness.port, {
            target: '/@capsid/orders/fixed',
            agent: keepAliveAgent,
        });
        const keepAliveSecond = await request(correctness.port, {
            target: '/@capsid/orders/fixed',
            agent: keepAliveAgent,
        });
        assert.equal(keepAliveFirst.status, 200);
        assert.equal(keepAliveSecond.status, 200);
        assert.equal(
            keepAliveSecond.clientPort,
            keepAliveFirst.clientPort,
            'HTTP/1 keep-alive opened a second client connection',
        );
    } finally {
        keepAliveAgent.destroy();
    }

    const head = await request(correctness.port, {
        method: 'HEAD',
        target: '/@capsid/orders/fixed-string',
    });
    assert.equal(head.status, 200);
    assert.equal(head.body.length, 0, 'HEAD exposed the worker response body');
    assert.equal(head.headers['content-length'], '1024');

    const routeMiss = await request(correctness.port, { target: '/outside' });
    assert.equal(routeMiss.status, 404);

    const echoedBody = Buffer.alloc(80 * 1024 + 19);
    for (let index = 0; index < echoedBody.length; ++index) {
        echoedBody[index] = index % 251;
    }
    const echoed = await request(correctness.port, {
        method: 'POST',
        target: '/@capsid/orders/echo',
        body: echoedBody,
    });
    assert.equal(echoed.status, 201);
    assert.deepEqual(echoed.body, echoedBody);
    const cookies = [];
    for (let index = 0; index < echoed.rawHeaders.length; index += 2) {
        if (echoed.rawHeaders[index].toLowerCase() === 'set-cookie') {
            cookies.push(echoed.rawHeaders[index + 1]);
        }
    }
    assert.deepEqual(cookies, [ 'first=1; Path=/', 'second=2; Path=/' ]);

    const streamed = await request(correctness.port, {
        target: '/@capsid/orders/stream',
    });
    assert.equal(streamed.status, 200);
    assert.equal(streamed.body.length, 96 * 1024 + 37);
    for (let index = 0; index < streamed.body.length; ++index) {
        assert.equal(streamed.body[index], index % 251);
    }

    // Connection-nominated fields must be stripped regardless of whether
    // they appear before or after the Connection header naming them. (The
    // response does carry a Beast-generated Connection header — the Host's
    // own framing — which is expected.)
    for (const target of [
        '/@capsid/orders/conn-before',
        '/@capsid/orders/conn-after',
    ]) {
        const connTest = await request(correctness.port, { target });
        assert.equal(connTest.status, 200);
        assert.ok(
            !Object.hasOwn(connTest.headers, 'a-nominated'),
            `${target} leaked a Connection-nominated header`,
        );
    }

    // A worker response whose body exceeds its declared Content-Length must
    // fail the connection closed (the Host's response gate, design §8.3),
    // never serve a silently truncated body.
    await assert.rejects(
        request(correctness.port, { target: '/@capsid/orders/bad-cl' }),
        /response aborted before completion/,
        'oversized body was not failed closed',
    );
} finally {
    await stopHost(correctness);
}

// ---- Phase B: the frozen 100ms request deadline against an independent
// slow endpoint. No throughput assertion runs against this deadline.
const timeout = await startHost(args, { requestTimeoutMs: 100 });
try {
    const fixed = await request(timeout.port, {
        target: '/@capsid/orders/fixed',
    });
    assert.equal(fixed.status, 200);
    assert.equal(fixed.body.length, 1024);

    // The deadline must interrupt an async sleep on an independent slow
    // endpoint; the response is a clean 504, not a hang.
    const slow = await request(timeout.port, {
        target: '/@capsid/orders/slow',
        timeoutMs: 3000,
    });
    assert.equal(slow.status, 504);

    // The synchronous interrupt deadline must stop a busy loop. This is the
    // last request on this worker: the interrupt exception propagates out of
    // the fetch dispatch and the Runtime treats it as worker-fatal, so no
    // further request can run afterwards. Either the clean 504 (the timeout
    // event beat the worker death) or the 502 from the exit path (the
    // interrupt killed the worker first) proves the loop was stopped by the
    // deadline.
    const busy = await request(timeout.port, {
        target: '/@capsid/orders/timeout',
        timeoutMs: 3000,
    });
    assert.ok(
        busy.status === 504 || busy.status === 502,
        `unexpected /timeout status ${busy.status}`,
    );
} finally {
    await stopHost(timeout);
}

// ---- Phase C: disconnect-cancel under a 2s worker deadline. The client is
// destroyed mid-stream; the Host must cancel the worker request within
// hundreds of milliseconds (the abort the worker observes fires on the RST
// of the in-flight response write), not wait for the 2s deadline. Comparing
// against the worker's own deadline instead of a 100ms budget keeps startup
// latency from creating a false positive.
const cancelHost = await startHost(args, { requestTimeoutMs: 2000 });
try {
    await new Promise((resolve) => {
        const cancelled = http.request({
            host: '127.0.0.1',
            port: cancelHost.port,
            method: 'GET',
            path: '/@capsid/orders/cancel',
            agent: false,
            headers: { connection: 'close', host: 'ignored.example' },
        });
        cancelled.on('error', resolve);
        cancelled.on('close', resolve);
        cancelled.end();
        setTimeout(() => cancelled.destroy(), 35);
    });

    // Poll /status until the worker reports the cancel. A successful answer
    // also proves the worker survived the cancel and serves new requests.
    let cancelState = null;
    const statusDeadline = Date.now() + 3000;
    while (Date.now() < statusDeadline) {
        try {
            const status = await request(cancelHost.port, {
                target: '/@capsid/orders/status',
                timeoutMs: 500,
            });
            cancelState = JSON.parse(status.body.toString('utf8'));
            if (cancelState.cancelCount >= 1) {
                break;
            }
        } catch {
            // The worker may still be settling after the cancel.
        }
        await delay(20);
    }
    assert.ok(cancelState, 'worker never observed the disconnect cancel');
    assert.equal(cancelState.cancelCount, 1);
    assert.ok(
        cancelState.lastCancelDelayMs < 500,
        `cancel observed at ${cancelState.lastCancelDelayMs}ms ` +
            '— not within hundreds of milliseconds of the disconnect',
    );
} finally {
    await stopHost(cancelHost);
}

// ---- TCP_NODELAY behavior: default is on; only the exact value "0"
// disables it. The runner exports CAPSID_TCP_NODELAY=1 by default.
async function nodelayProbe(envValue) {
    const child = spawn(args.get('host'), hostArgs(args, { requestTimeoutMs: 10000 }), {
        env: {
            ...process.env,
            ...(envValue === null ? {} : { CAPSID_TCP_NODELAY: envValue }),
        },
        stdio: [ 'ignore', 'pipe', 'pipe', 'pipe' ],
    });
    let stderr = '';
    child.stderr.setEncoding('utf8');
    child.stderr.on('data', (chunk) => { stderr += chunk; });
    try {
        const readyLine = await readLine(child.stdio[3], child, 10000, () => stderr);
        const ready = JSON.parse(readyLine);
        await request(ready.port, { target: '/@capsid/orders/fixed', timeoutMs: 3000 });
        await delay(200);
        // Default (no env var): NODELAY enabled.
        // CAPSID_TCP_NODELAY=0: explicitly disabled.
        return !stderr.includes('TCP_NODELAY disabled');
    } finally {
        child.kill('SIGTERM');
        await waitForExit(child, 3000);
    }
}

{
    // Default (no env var): NODELAY must be on.
    assert.equal(
        await nodelayProbe(null),
        true,
        'default (no env var) must have TCP_NODELAY enabled',
    );
    // CAPSID_TCP_NODELAY=0: explicit disable.
    assert.equal(
        await nodelayProbe('0'),
        false,
        'CAPSID_TCP_NODELAY=0 must disable TCP_NODELAY',
    );
}

// ---- Bodyless fusion toggle: on by default, only the exact value "0"
// disables it. The unfused path must still complete bodyless requests
// (worker request-credit event drives an explicit kEndRequest; the request
// must not hang waiting for a body end that never arrives).
async function bodylessProbe(envValue) {
    const child = spawn(args.get('host'), hostArgs(args, { requestTimeoutMs: 10000 }), {
        env: {
            ...process.env,
            ...(envValue === null ? {} : { CAPSID_BODYLESS: envValue }),
        },
        stdio: [ 'ignore', 'pipe', 'pipe', 'pipe' ],
    });
    let stderr = '';
    child.stderr.setEncoding('utf8');
    child.stderr.on('data', (chunk) => { stderr += chunk; });
    try {
        const readyLine = await readLine(child.stdio[3], child, 10000, () => stderr);
        const ready = JSON.parse(readyLine);
        const response = await request(ready.port, {
            target: '/@capsid/orders/fixed',
            timeoutMs: 3000,
        });
        assert.equal(response.status, 200);
        assert.equal(response.body.length, 1024);
        return !stderr.includes('bodyless request fusion disabled');
    } finally {
        child.kill('SIGTERM');
        await waitForExit(child, 3000);
    }
}

{
    // Default (no env var): fusion must be on.
    assert.equal(
        await bodylessProbe(null),
        true,
        'default (no env var) must have bodyless fusion enabled',
    );
    // CAPSID_BODYLESS=0: explicit disable; the unfused path must still serve
    // bodyless requests end-to-end.
    assert.equal(
        await bodylessProbe('0'),
        false,
        'CAPSID_BODYLESS=0 must disable bodyless fusion',
    );
}
