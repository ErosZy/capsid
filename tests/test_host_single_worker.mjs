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
// for the startup-failure regressions. routing knobs mirror the managed
// routing matrix: subdomain carries --routing-suffix, header carries
// --routing-trusted.
function hostArgs(args, {
    requestTimeoutMs,
    listen = '127.0.0.1:0',
    routing = 'path',
    routingSuffix,
    routingTrusted,
} = {}) {
    const bundle = path.resolve(args.get('bundle'));
    return [
        '--mode', 'single-worker',
        '--worker', path.resolve(args.get('worker')),
        '--source-bundle', bundle,
        '--source-name', pathToFileURL(bundle).href,
        '--application', 'orders',
        '--listen', listen,
        '--routing', routing,
        ...(routing === 'subdomain' ? [ '--routing-suffix', routingSuffix ] : []),
        ...(routing === 'header' ? [ '--routing-trusted', routingTrusted ] : []),
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
async function startHost(args, { requestTimeoutMs, ...hostKnobs }) {
    const child = spawn(args.get('host'),
        hostArgs(args, { requestTimeoutMs, ...hostKnobs }),
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
    // Routing-argument validation mirrors the managed routing matrix at
    // the CLI phase: subdomain needs a suffix, header needs a trusted
    // listener, and each knob is rejected outside its mode.
    const bundle = path.resolve(args.get('bundle'));
    const routingCases = [
        [ '--routing', 'subdomain' ],
        [ '--routing', 'header' ],
        [ '--routing', 'path', '--routing-suffix', '.apps.local' ],
        [ '--routing', 'path', '--routing-trusted', 'on' ],
    ];
    for (const extra of routingCases) {
        const child = spawn(args.get('host'), [
            '--mode', 'single-worker',
            ...extra,
            '--worker', path.resolve(args.get('worker')),
            '--source-bundle', bundle,
            '--source-name', pathToFileURL(bundle).href,
            '--application', 'orders',
            '--listen', '127.0.0.1:0',
            '--public-scheme', 'http',
            '--public-authority', 'public.example',
            '--request-timeout-ms', '10000',
            '--strict-sandbox', 'off',
            '--ready-fd', '3',
        ], {
            stdio: [ 'ignore', 'pipe', 'pipe', 'ignore' ],
        });
        const routingExit = await waitForExit(child, 5000);
        assert.equal(
            routingExit.code,
            2,
            `CLI must reject routing arguments: ${extra.join(' ')}`,
        );
    }
    // Strict sandbox is a Linux-only capability: the CLI fails at the
    // argument phase on every other platform (fail fast, matching
    // --mode managed), never after spawn.
    if (process.platform !== 'linux') {
        const strictChild = spawn(args.get('host'), [
            '--mode', 'single-worker',
            '--worker', path.resolve(args.get('worker')),
            '--source-bundle', bundle,
            '--source-name', pathToFileURL(bundle).href,
            '--application', 'orders',
            '--listen', '127.0.0.1:0',
            '--routing', 'path',
            '--public-scheme', 'http',
            '--public-authority', 'public.example',
            '--request-timeout-ms', '10000',
            '--strict-sandbox', 'on',
            '--ready-fd', '3',
        ], {
            stdio: [ 'ignore', 'pipe', 'pipe', 'ignore' ],
        });
        const strictExit = await waitForExit(strictChild, 5000);
        assert.equal(
            strictExit.code,
            2,
            '--strict-sandbox on must fail the CLI phase on non-Linux',
        );
    }
}
{
    // capsid.json `entry` is a plain file name inside the capsid.json
    // directory. Traversal and absolute/drive forms are rejected at the
    // CLI phase, before the bundle path is composed.
    const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'capsid-entry-'));
    fs.writeFileSync(path.join(dir, 'capsid.json'), JSON.stringify({
        apiVersion: 'capsid/app-v1',
        entry: '../outside.mjs',
        pool: { minReady: 1, maxWorkers: 1 },
        request: { timeout: '10s' },
    }));
    const child = spawn(args.get('host'), [
        '--mode', 'single-worker',
        '--worker', path.resolve(args.get('worker')),
        '--capsid-json', path.join(dir, 'capsid.json'),
        '--application', 'orders',
        '--listen', '127.0.0.1:0',
        '--routing', 'path',
        '--public-scheme', 'http',
        '--public-authority', 'public.example',
        '--strict-sandbox', 'off',
        '--ready-fd', '3',
    ], {
        cwd: dir,
        stdio: [ 'ignore', 'pipe', 'pipe', 'ignore' ],
    });
    let stderr = '';
    child.stderr.setEncoding('utf8');
    child.stderr.on('data', (chunk) => { stderr += chunk; });
    const entryExit = await waitForExit(child, 5000);
    assert.equal(entryExit.code, 2, 'traversal entry must fail the CLI phase');
    assert.match(stderr, /entry must be a plain file name/,
        'entry rejection must name the containment rule');
    fs.rmSync(dir, { recursive: true, force: true });
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
        let brokenStderr = '';
        brokenReady.stderr.setEncoding('utf8');
        brokenReady.stderr.on('data', (chunk) => { brokenStderr += chunk; });
        const brokenExit = await waitForExit(brokenReady, 5000);
        assert.equal(
            brokenExit.code,
            1,
            'READY-fd write failure must exit with code 1 (stderr=' + brokenStderr + ')',
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
// ---- Routing matrix alignment: the CLI mirrors the managed listeners'
// subdomain/header extraction. The extracted App must equal --application;
// anything else is 404, malformed control fields are 400.
{
    const subdomain = await startHost(args, {
        requestTimeoutMs: 10000,
        routing: 'subdomain',
        routingSuffix: '.apps.local',
    });
    try {
        const match = await request(subdomain.port, {
            target: '/fixed',
            headers: { host: 'orders.apps.local' },
        });
        assert.equal(match.status, 200);
        assert.equal(match.body.length, 1024);

        const mismatch = await request(subdomain.port, {
            target: '/fixed',
            headers: { host: 'other.apps.local' },
        });
        assert.equal(mismatch.status, 404, 'subdomain app mismatch must be 404');
    } finally {
        await stopHost(subdomain);
    }
}
{
    const header = await startHost(args, {
        requestTimeoutMs: 10000,
        routing: 'header',
        routingTrusted: 'on',
    });
    try {
        const match = await request(header.port, {
            target: '/fixed',
            headers: { 'capsid-app': 'orders' },
        });
        assert.equal(match.status, 200);
        assert.equal(match.body.length, 1024);

        const mismatch = await request(header.port, {
            target: '/fixed',
            headers: { 'capsid-app': 'other' },
        });
        assert.equal(mismatch.status, 404, 'header app mismatch must be 404');

        // Missing Capsid-App routes nowhere (404, same as the managed
        // listener); a duplicate is a malformed control field (400).
        const missing = await request(header.port, { target: '/fixed' });
        assert.equal(missing.status, 404, 'missing Capsid-App must be 404');

        const duplicate = await request(header.port, {
            target: '/fixed',
            headers: { 'capsid-app': [ 'orders', 'orders' ] },
        });
        assert.equal(duplicate.status, 400, 'duplicate Capsid-App must be 400');
    } finally {
        await stopHost(header);
    }
}

// ---- Local capsid.json runtime settings (v0.2.x): a production-shaped
// document supplies entry, worker/request/pool knobs and a healthCheck;
// --source-bundle/--source-name are absent and derived from the entry.
{
    const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'capsid-local-capsid-'));
    const bundleFile = path.join(dir, 'host-single-worker.js');
    fs.copyFileSync(args.get('bundle'), bundleFile);
    // processAddressSpace is unsupported on Darwin (the worker rejects it
    // fail-closed, same as the managed compile path): omit it there.
    const worker = process.platform === 'darwin'
        ? { jsHeap: '64MiB', fileDescriptors: 64 }
        : { jsHeap: '64MiB', processAddressSpace: '256MiB', fileDescriptors: 64 };
    fs.writeFileSync(path.join(dir, 'capsid.json'), JSON.stringify({
        apiVersion: 'capsid/app-v1',
        entry: 'host-single-worker.js',
        pool: {
            minReady: 1,
            maxWorkers: 1,
            queueRequests: 2,
            queueHeaderBytes: '64KiB',
            queueTimeout: '1s',
        },
        worker,
        request: {
            timeout: '10s',
            maxInflightPerWorker: 16,
            maxStreamingInflightPerWorker: 1,
            streamIdleTimeoutMs: 30000,
            writeTimeoutMs: 5000,
        },
        healthCheck: { path: '/health', timeout: '5s' },
    }));
    const child = spawn(args.get('host'), [
        '--mode', 'single-worker',
        '--worker', path.resolve(args.get('worker')),
        '--capsid-json', path.join(dir, 'capsid.json'),
        '--application', 'orders',
        '--listen', '127.0.0.1:0',
        '--routing', 'path',
        '--public-scheme', 'http',
        '--public-authority', 'public.example',
        '--strict-sandbox', 'off',
        '--ready-fd', '3',
    ], {
        cwd: dir,
        stdio: [ 'ignore', 'pipe', 'pipe', 'pipe' ],
    });
    let stderr = '';
    child.stderr.setEncoding('utf8');
    child.stderr.on('data', (chunk) => { stderr += chunk; });
    try {
        const readyLine = await readLine(child.stdio[3], child, 10000, () => stderr);
        const ready = JSON.parse(readyLine);
        assert.equal(ready.app, 'orders', 'entry-derived bundle must serve the app');
        const healthy = await request(ready.port, {
            target: '/@capsid/orders/health',
            timeoutMs: 3000,
        });
        assert.equal(healthy.status, 200, 'healthCheck path must serve 200');
        const fixed = await request(ready.port, {
            target: '/@capsid/orders/fixed',
            timeoutMs: 3000,
        });
        assert.equal(fixed.status, 200);
        assert.equal(fixed.body.length, 1024);
    } finally {
        child.kill('SIGTERM');
        await waitForExit(child, 3000);
        fs.rmSync(dir, { recursive: true, force: true });
    }
}
{
    // A healthCheck whose path the app does not serve fails the startup:
    // no READY record, exit 1, and the failure names the probe.
    const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'capsid-local-health-'));
    fs.copyFileSync(args.get('bundle'), path.join(dir, 'host-single-worker.js'));
    fs.writeFileSync(path.join(dir, 'capsid.json'), JSON.stringify({
        apiVersion: 'capsid/app-v1',
        entry: 'host-single-worker.js',
        pool: { minReady: 1, maxWorkers: 1 },
        request: { timeout: '10s' },
        healthCheck: { path: '/nope', timeout: '3s' },
    }));
    const child = spawn(args.get('host'), [
        '--mode', 'single-worker',
        '--worker', path.resolve(args.get('worker')),
        '--capsid-json', path.join(dir, 'capsid.json'),
        '--application', 'orders',
        '--listen', '127.0.0.1:0',
        '--routing', 'path',
        '--public-scheme', 'http',
        '--public-authority', 'public.example',
        '--strict-sandbox', 'off',
        '--ready-fd', '3',
    ], {
        cwd: dir,
        // fd 3 must be OPEN ('ignore' would close it for fds >= 3):
        // the CLI validates the descriptor before spawn.
        stdio: [ 'ignore', 'pipe', 'pipe', 'pipe' ],
    });
    let stderr = '';
    child.stderr.setEncoding('utf8');
    child.stderr.on('data', (chunk) => { stderr += chunk; });
    const exited = await waitForExit(child, 10000);
    try {
        assert.equal(exited.code, 1, 'failed healthCheck must exit 1');
        assert.match(stderr, /health check failed/,
            'startup failure must name the health probe');
    } finally {
        fs.rmSync(dir, { recursive: true, force: true });
    }
}

// ---- Static-pool healthCheck: the pool-level READY record is gated by
// one real HTTP probe through the pool endpoint (single-worker covers the
// single-listener path; static-pool must not silently skip its probe).
{
    const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'capsid-pool-health-'));
    fs.copyFileSync(args.get('bundle'), path.join(dir, 'host-single-worker.js'));
    const spawnPool = (healthPath) => {
        const capsidJson = path.join(dir, `capsid-${healthPath.replaceAll('/', '-')}.json`);
        fs.writeFileSync(capsidJson, JSON.stringify({
            apiVersion: 'capsid/app-v1',
            entry: 'host-single-worker.js',
            pool: { minReady: 1, maxWorkers: 2 },
            request: { timeout: '10s' },
            healthCheck: { path: healthPath, timeout: '3s' },
        }));
        return spawn(args.get('host'), [
            '--mode', 'static-pool',
            '--workers', '2',
            '--worker', path.resolve(args.get('worker')),
            '--capsid-json', capsidJson,
            '--application', 'orders',
            '--listen', '127.0.0.1:0',
            '--routing', 'path',
            '--public-scheme', 'http',
            '--public-authority', 'public.example',
            '--strict-sandbox', 'off',
            '--ready-fd', '3',
        ], {
            cwd: dir,
            stdio: [ 'ignore', 'pipe', 'pipe', 'pipe' ],
        });
    };
    try {
        const healthy = spawnPool('/health');
        let healthyStderr = '';
        healthy.stderr.setEncoding('utf8');
        healthy.stderr.on('data', (chunk) => { healthyStderr += chunk; });
        try {
            const readyLine = await readLine(
                healthy.stdio[3], healthy, 10000, () => healthyStderr);
            const ready = JSON.parse(readyLine);
            const response = await request(ready.port, {
                target: '/@capsid/orders/health',
                timeoutMs: 3000,
            });
            assert.equal(response.status, 200,
                'static-pool healthCheck must pass through the real endpoint');
        } finally {
            healthy.kill('SIGTERM');
            await waitForExit(healthy, 3000);
        }

        const broken = spawnPool('/nope');
        let brokenStderr = '';
        broken.stderr.setEncoding('utf8');
        broken.stderr.on('data', (chunk) => { brokenStderr += chunk; });
        const brokenExit = await waitForExit(broken, 10000);
        assert.equal(brokenExit.code, 1,
            'static-pool healthCheck failure must exit 1');
        assert.match(brokenStderr, /health check failed/,
            'static-pool healthCheck failure must name the probe');
    } finally {
        fs.rmSync(dir, { recursive: true, force: true });
    }
}

// ---- Local env valueFrom (--secrets-root): one regular file per key id,
// the managed layout; without the root the document is rejected.
{
    const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'capsid-local-secret-'));
    fs.writeFileSync(path.join(dir, 'secret-bundle.js'), `
        import { env } from 'capsid:env';
        export default {
            async fetch(request) {
                const url = new URL(request.url);
                if (url.pathname === '/secret') {
                    return new Response(env.get('DB_URL'), { status: 200 });
                }
                return new Response('no route', { status: 404 });
            },
        };
    `);
    fs.mkdirSync(path.join(dir, 'secrets'));
    fs.writeFileSync(path.join(dir, 'secrets', 'db-url'), 'postgres://secret');
    const capsidJson = path.join(dir, 'capsid.json');
    fs.writeFileSync(capsidJson, JSON.stringify({
        apiVersion: 'capsid/app-v1',
        entry: 'secret-bundle.js',
        pool: { minReady: 1, maxWorkers: 1 },
        request: { timeout: '10s' },
        permissions: {
            modules: [ 'capsid:env' ],
            env: { DB_URL: { valueFrom: 'db-url' } },
        },
    }));
    const spawnSecret = (extraArgs) => spawn(args.get('host'), [
        '--mode', 'single-worker',
        '--worker', path.resolve(args.get('worker')),
        '--capsid-json', capsidJson,
        ...extraArgs,
        '--application', 'orders',
        '--listen', '127.0.0.1:0',
        '--routing', 'path',
        '--public-scheme', 'http',
        '--public-authority', 'public.example',
        '--strict-sandbox', 'off',
        '--ready-fd', '3',
    ], {
        cwd: dir,
        stdio: [ 'ignore', 'pipe', 'pipe', 'pipe' ],
    });
    try {
        // Without --secrets-root the same document fails the CLI phase
        // (exit 2), never a silent empty env value. Runs first: the dir
        // is both the spawn cwd and the artifact source and must survive.
        const rejected = spawnSecret([]);
        const rejectedExit = await waitForExit(rejected, 5000);
        assert.equal(
            rejectedExit.code,
            2,
            'valueFrom without --secrets-root must fail the CLI phase',
        );

        const child = spawnSecret([ '--secrets-root', path.join(dir, 'secrets') ]);
        let stderr = '';
        child.stderr.setEncoding('utf8');
        child.stderr.on('data', (chunk) => { stderr += chunk; });
        try {
            const readyLine = await readLine(child.stdio[3], child, 10000, () => stderr);
            const ready = JSON.parse(readyLine);
            const secret = await request(ready.port, {
                target: '/@capsid/orders/secret',
                timeoutMs: 3000,
            });
            assert.equal(secret.status, 200, 'valueFrom must resolve through --secrets-root');
            assert.equal(
                secret.body.toString('utf8'),
                'postgres://secret',
                'the worker must see the resolved secret value',
            );
        } finally {
            child.kill('SIGTERM');
            await waitForExit(child, 3000);
        }

        // NUL and oversized values must fail the CLI phase with the same
        // value contract as the managed secret provider (no silent c_str
        // truncation, no unlimited operator file into the worker env).
        for (const [label, value] of [
            [ 'nul', 'postgres://\0secret' ],
            [ 'oversized', 'x'.repeat(16385) ],
        ]) {
            fs.writeFileSync(path.join(dir, 'secrets', 'db-url'), value);
            const bad = spawnSecret([ '--secrets-root', path.join(dir, 'secrets') ]);
            let badStderr = '';
            bad.stderr.setEncoding('utf8');
            bad.stderr.on('data', (chunk) => { badStderr += chunk; });
            const badExit = await waitForExit(bad, 5000);
            assert.equal(
                badExit.code,
                2,
                `${label} secret value must fail the CLI phase`,
            );
            assert.match(
                badStderr,
                /NUL-free UTF-8 text/,
                `${label} secret rejection must name the value contract`,
            );
        }
    } finally {
        fs.rmSync(dir, { recursive: true, force: true });
    }
}

// ---- Fetch pre-resolution: hostname targets resolve through the system
// resolver before connect (txiki 0030). "localhost" is not a numeric
// address and no proxy applies here, so a successful roundtrip proves the
// pre-resolve path end to end through the real listener.
{
    const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'capsid-preresolve-'));
    const upstream = http.createServer((req, res) => {
        if (req.url === '/probe') {
            res.writeHead(200, { 'content-type': 'text/plain' });
            res.end('capsid-preresolve-ok');
            return;
        }
        res.writeHead(404);
        res.end();
    });
    await new Promise((resolve) => upstream.listen(0, '127.0.0.1', resolve));
    const upstreamPort = upstream.address().port;

    fs.writeFileSync(path.join(dir, 'preresolve-bundle.js'), `
        export default {
            async fetch(request) {
                const url = new URL(request.url);
                if (url.pathname !== '/probe') {
                    return new Response('no route', { status: 404 });
                }
                try {
                    const upstream = await fetch('http://localhost:${upstreamPort}/probe');
                    return new Response(await upstream.text(), {
                        status: upstream.status,
                    });
                } catch (error) {
                    return new Response(JSON.stringify({
                        errorName: error?.name,
                        errorMessage: error?.message,
                    }), { status: 500 });
                }
            },
        };
    `);
    const capsidJson = path.join(dir, 'capsid.json');
    fs.writeFileSync(capsidJson, JSON.stringify({
        apiVersion: 'capsid/app-v1',
        entry: 'preresolve-bundle.js',
        pool: { minReady: 1, maxWorkers: 1 },
        request: { timeout: '10s' },
        permissions: {
            fetch: { allow: [ `localhost:${upstreamPort}` ] },
        },
    }));

    const child = spawn(args.get('host'), [
        '--mode', 'single-worker',
        '--worker', path.resolve(args.get('worker')),
        '--capsid-json', capsidJson,
        '--application', 'orders',
        '--listen', '127.0.0.1:0',
        '--routing', 'path',
        '--public-scheme', 'http',
        '--public-authority', 'public.example',
        '--strict-sandbox', 'off',
        '--ready-fd', '3',
    ], {
        stdio: [ 'ignore', 'pipe', 'pipe', 'pipe' ],
    });
    let stderr = '';
    child.stderr.setEncoding('utf8');
    child.stderr.on('data', (chunk) => { stderr += chunk; });
    try {
        const readyLine = await readLine(child.stdio[3], child, 10000, () => stderr);
        const ready = JSON.parse(readyLine);
        const response = await request(ready.port, {
            target: '/@capsid/orders/probe',
            timeoutMs: 5000,
        });
        assert.equal(
            response.status,
            200,
            `hostname fetch failed: ${response.body.toString('utf8')}; stderr=${stderr}`,
        );
        assert.equal(
            response.body.toString('utf8'),
            'capsid-preresolve-ok',
            'the worker must reach the upstream through the pre-resolve path',
        );
    } finally {
        child.kill('SIGTERM');
        await waitForExit(child, 3000);
        upstream.close();
        fs.rmSync(dir, { recursive: true, force: true });
    }
}

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
