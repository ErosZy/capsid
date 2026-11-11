// Host-side WebSocket upgrade validation (FetchRPC v4 WS-C3, TDD).
//
// Contract: docs/fetchrpc-v4.md §2 — the host owns the handshake gates
// (frozen decision D1). A request that presents itself as a WebSocket
// upgrade is validated per RFC 6455 §4.2.1 BEFORE any worker
// involvement:
//   - malformed or missing Sec-WebSocket-Key        -> 400
//   - Sec-WebSocket-Version other than 13           -> 426 + supported
//   - a valid upgrade whose worker answers non-101  -> the response is
//     relayed unchanged (the reject path; the fixture answers 200)
// The 101-accept path lands with the worker v4 surface and is covered
// by tests/test_workerd_ws_fixtures.mjs.
import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import fs from 'node:fs';
import http from 'node:http';
import net from 'node:net';
import { once } from 'node:events';
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
    for (const required of [ 'host', 'worker' ]) {
        assert.ok(values.has(required), `missing --${required}`);
    }
    return values;
}

function request(port, headers, target = '/@capsid/orders/', method = 'GET') {
    return new Promise((resolve, reject) => {
        const req = http.request({
            host: '127.0.0.1',
            port,
            method,
            path: target,
            headers: { host: 'client.example', ...headers },
            agent: false,
        });
        req.setTimeout(5000, () => {
            req.destroy(new Error('timeout'));
        });
        req.on('error', reject);
        req.on('response', (response) => {
            const chunks = [];
            response.on('data', (chunk) => chunks.push(chunk));
            response.on('aborted', () => reject(new Error('aborted')));
            response.on('end', () => resolve({
                status: response.statusCode,
                headers: response.headers,
                body: Buffer.concat(chunks),
            }));
        });
        req.end();
    });
}

function rawHttp(port, text) {
    return new Promise((resolve, reject) => {
        const socket = net.connect(port, '127.0.0.1');
        const chunks = [];
        socket.setTimeout(5000, () => socket.destroy(new Error('raw HTTP timeout')));
        socket.on('connect', () => socket.write(text));
        socket.on('data', (chunk) => chunks.push(chunk));
        socket.on('error', reject);
        socket.on('end', () => {
            const response = Buffer.concat(chunks).toString('latin1');
            resolve({ status: Number(response.split(' ')[1]), text: response });
        });
    });
}

async function unusedPort() {
    const server = net.createServer();
    await new Promise((resolve, reject) => {
        server.once('error', reject);
        server.listen(0, '127.0.0.1', resolve);
    });
    const port = server.address().port;
    await new Promise((resolve) => server.close(resolve));
    return port;
}

async function admin(socketPath, target, body) {
    return new Promise((resolve, reject) => {
        const req = http.request({ socketPath, path: target,
            method: body ? 'POST' : 'GET',
            headers: body ? { 'content-type': 'application/json' } : {} }, (response) => {
            const chunks = [];
            response.on('data', (chunk) => chunks.push(chunk));
            response.on('end', () => {
                try { resolve({ status: response.statusCode,
                    json: JSON.parse(Buffer.concat(chunks)) }); }
                catch (error) { reject(error); }
            });
        });
        req.setTimeout(5000, () => req.destroy(new Error('admin request timeout')));
        req.on('error', reject);
        req.end(body ? JSON.stringify(body) : undefined);
    });
}

async function stopHost(child) {
    if (child.exitCode !== null || child.signalCode !== null) return;
    const exited = once(child, 'exit');
    child.kill('SIGTERM');
    const timer = setTimeout(() => child.kill('SIGKILL'), 5000);
    try { await exited; } finally { clearTimeout(timer); }
}

async function startHost(args) {
    const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'capsid-ws-upgrade-'));
    const bundle = path.join(dir, 'fixture.js');
    // A marker distinguishes worker responses from host handshake rejections.
    fs.writeFileSync(bundle, [
        'const workerId = String(Math.random()); let admissions = 0;',
        'export default {',
        '  async fetch(request) {',
        '    admissions++;',
        '    const rejected = new URL(request.url).pathname === "/upstream-reject";',
        '    return new Response(rejected ? "worker-rejected" : "plain", {',
        '      status: rejected ? 403 : 200, headers: { "x-worker-hit": "yes", "x-worker-id": workerId, "x-worker-admissions": String(admissions) }',
        '    });',
        '  },',
        '};',
        '',
    ].join('\n'));
    const mode = args.get('mode') ?? 'single-worker';
    let hostArgs = [
        '--mode', mode,
        '--worker', path.resolve(args.get('worker')),
        '--source-bundle', bundle,
        '--source-name', pathToFileURL(bundle).href,
        '--application', 'orders',
        '--listen', '127.0.0.1:0',
        '--routing', 'path',
        '--public-scheme', 'http',
        '--public-authority', 'public.example',
        '--request-timeout-ms', '10000',
        '--strict-sandbox', 'off',
        '--ready-fd', '3',
    ];
    let port;
    let socketPath;
    if (mode === 'static-pool') hostArgs.push('--workers', '2');
    if (mode === 'managed') {
        port = await unusedPort();
        const applicationsRoot = path.join(dir, 'applications');
        const stateRoot = path.join(dir, 'state');
        const secrets = path.join(dir, 'secrets');
        const runRoot = path.join(dir, 'run');
        for (const directory of [applicationsRoot, stateRoot, secrets, runRoot]) {
            fs.mkdirSync(directory, { recursive: true, mode: 0o700 });
        }
        const versionRoot = path.join(applicationsRoot, 'orders', 'v1');
        fs.mkdirSync(versionRoot, { recursive: true, mode: 0o700 });
        fs.copyFileSync(bundle, path.join(versionRoot, 'bundle.mjs'));
        fs.writeFileSync(path.join(versionRoot, 'capsid.json'), JSON.stringify({
            apiVersion: 'capsid/app-v1', entry: 'bundle.mjs',
            permissions: { modules: ['capsid:env'] }, pool: { minReady: 1, maxWorkers: 1 },
        }));
        socketPath = path.join(runRoot, 'admin.sock');
        const config = path.join(dir, 'host.json');
        fs.writeFileSync(config, JSON.stringify({
            apiVersion: 'capsid/host-v1', applicationsRoot, stateRoot,
            secretRootTemplate: path.join(secrets, '{application}'),
            admin: { unix: socketPath, mode: '0600' },
            permissions: { modules: ['capsid:env'], environmentNames: [],
                fsReadRoots: [], fetchTargets: [], storageNamespaces: [], stdioStreams: [] },
            isolation: { mode: 'strict', required: [] },
            capacity: { workersTotal: 1, startupsConcurrent: 1 },
            listeners: [{ name: 'public', tcp: `127.0.0.1:${port}`,
                publicScheme: 'http', publicAuthority: 'public.example',
                trusted: false, routing: { mode: 'path' } }],
        }));
        hostArgs = ['--mode', 'managed', '--host-config', config,
            '--worker', path.resolve(args.get('worker'))];
    }
    const child = spawn(args.get('host'), hostArgs,
        { stdio: [ 'ignore', 'pipe', 'pipe', 'pipe' ] });
    let stderr = '';
    child.stderr.setEncoding('utf8');
    child.stderr.on('data', (chunk) => { stderr += chunk; });
    if (mode === 'managed') {
        try {
            const deadline = Date.now() + 20000;
            while (!fs.existsSync(socketPath)) {
                assert.ok(Date.now() < deadline && child.exitCode === null && child.signalCode === null,
                    `managed admin startup failed: ${stderr}`);
                await delay(25);
            }
            const deployment = await admin(socketPath, '/v1/deploy', { app: 'orders', version: 'v1' });
            assert.equal(deployment.status, 202, JSON.stringify(deployment.json));
            for (;;) {
                const operation = await admin(socketPath, `/v1/operations/${deployment.json.operationId}`);
                assert.notEqual(operation.json.state, 'failed', `${JSON.stringify(operation.json)} ${stderr}`);
                if (operation.json.state === 'active') break;
                assert.ok(Date.now() < deadline, `managed deploy timed out: ${stderr}`);
                await delay(25);
            }
            return { child, port, dir };
        } catch (error) {
            await stopHost(child);
            fs.rmSync(dir, { recursive: true, force: true });
            throw error;
        }
    }
    try {
        const readyLine = await new Promise((resolve, reject) => {
            let buffer = '';
            const timer = setTimeout(
                () => reject(new Error(`ready timeout; stderr=${stderr}`)), 10000);
            child.stdio[3].setEncoding('utf8');
            child.stdio[3].on('data', (chunk) => {
                buffer += chunk;
                const newline = buffer.indexOf('\n');
                if (newline !== -1) {
                    clearTimeout(timer);
                    resolve(buffer.slice(0, newline));
                }
            });
            child.on('exit', () => {
                clearTimeout(timer);
                reject(new Error(`host exited; stderr=${stderr}`));
            });
        });
        const ready = JSON.parse(readyLine);
        assert.equal(ready.schema, 'capsid-host-ready-v1');
        return { child, port: ready.port, dir };
    } catch (error) {
        await stopHost(child);
        fs.rmSync(dir, { recursive: true, force: true });
        throw error;
    }
}

const upgradeHeaders = {
    connection: 'Upgrade',
    upgrade: 'websocket',
    'sec-websocket-key': 'dGhlIHNhbXBsZSBub25jZQ==',
    'sec-websocket-version': '13',
};

async function main() {
    const args = parseArgs(process.argv.slice(2));
    const { child, port, dir } = await startHost(args);
    try {
        const ordinary = await request(port, { 'sec-websocket-key': ['one', 'two'] });
        assert.equal(ordinary.status, 200, 'ordinary HTTP ignores incidental duplicate WS keys');
        assert.equal(ordinary.body.toString(), 'plain');

        const counts = new Map();
        const observeAdmission = (response) => {
            assert.equal(response.headers['x-worker-hit'], 'yes');
            const id = response.headers['x-worker-id'];
            const count = Number(response.headers['x-worker-admissions']);
            assert.ok(id && Number.isSafeInteger(count) && count > 0);
            if (counts.has(id)) assert.equal(count, counts.get(id) + 1,
                'worker admission counter advanced only for forwarded HTTP requests');
            counts.set(id, count);
        };
        observeAdmission(ordinary);
        for (let i = 0; i < 32; ++i) observeAdmission(await request(port, {}));
        assert.equal(counts.size, args.get('mode') === 'static-pool' ? 2 : 1,
            'all serving workers supplied admission counters');

        // 1. malformed key -> 400, host-synthesized, no worker round trip
        const badKey = await request(
            port, { ...upgradeHeaders, 'sec-websocket-key': 'short' });
        assert.equal(badKey.status, 400);

        // missing key entirely -> 400
        const noKey = await request(port, {
            connection: 'Upgrade',
            upgrade: 'websocket',
            'sec-websocket-version': '13',
        });
        assert.equal(noKey.status, 400);

        // 2. unsupported version -> 426 naming the supported version
        const oldVersion = await request(
            port, { ...upgradeHeaders, 'sec-websocket-version': '8' });
        assert.equal(oldVersion.status, 426);
        assert.equal(oldVersion.headers['sec-websocket-version'], '13');

        const duplicateVersion = await request(port, { ...upgradeHeaders,
            'sec-websocket-version': ['13', '13'] });
        assert.equal(duplicateVersion.status, 426);
        assert.equal(duplicateVersion.headers['sec-websocket-version'], '13');
        const duplicateKey = await request(port, { ...upgradeHeaders,
            'sec-websocket-key': [upgradeHeaders['sec-websocket-key'], upgradeHeaders['sec-websocket-key']] });
        assert.equal(duplicateKey.status, 400);
        const noConnection = await request(port, { ...upgradeHeaders, connection: 'keep-alive' });
        assert.equal(noConnection.status, 400);
        const postUpgrade = await request(port, upgradeHeaders, undefined, 'POST');
        assert.equal(postUpgrade.status, 400);
        const oldHttp = await rawHttp(port,
            'GET /@capsid/orders/ HTTP/1.0\r\nHost: client.example\r\n' +
            'Connection: Upgrade\r\nUpgrade: websocket\r\n' +
            'Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n' +
            'Sec-WebSocket-Version: 13\r\n\r\n');
        assert.equal(oldHttp.status, 400);
        assert.ok(!oldHttp.text.toLowerCase().includes('x-worker-hit'));
        const bodyfulUpgrade=await rawHttp(port,
            'GET /@capsid/orders/ HTTP/1.1\r\nHost: client.example\r\nConnection: Upgrade\r\nUpgrade: websocket\r\n'+
            'Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\nContent-Length: 1\r\n\r\nx');
        assert.equal(bodyfulUpgrade.status,400);
        assert.ok(!bodyfulUpgrade.text.toLowerCase().includes('x-worker-hit'),'bodyful upgrade rejected before worker admission');
        const badProtocols = await request(port, { ...upgradeHeaders, 'sec-websocket-protocol': 'chat, chat' });
        assert.equal(badProtocols.status, 400);
        const quotedProtocol = await request(port, { ...upgradeHeaders, 'sec-websocket-protocol': '"chat"' });
        assert.equal(quotedProtocol.status, 400);
        const malformedTokens = await request(port, { ...upgradeHeaders, connection: 'Upgrade,, keep-alive' });
        assert.equal(malformedTokens.status, 400);
        for (const rejected of [badKey, noKey, oldVersion, duplicateVersion, duplicateKey,
            noConnection, postUpgrade, badProtocols, quotedProtocol, malformedTokens]) {
            assert.equal(rejected.headers['x-worker-hit'], undefined, 'gate never invokes the worker');
        }

        // 3. valid handshake, non-101 worker answer -> relayed unchanged
        const relayed = await request(port, upgradeHeaders);
        assert.equal(relayed.status, 200);
        assert.equal(relayed.body.toString('utf8'), 'plain');
        observeAdmission(relayed);

        const combined = await request(port, { ...upgradeHeaders,
            connection: ['keep-alive', 'Upgrade'], upgrade: ['h2c', 'WebSocket'],
            'sec-websocket-protocol': ['chat', 'other'] });
        assert.equal(combined.status, 200, 'list-valued headers combine across duplicate fields');
        observeAdmission(combined);
        const upstreamReject = await request(port, upgradeHeaders, '/@capsid/orders/upstream-reject');
        assert.equal(upstreamReject.status, 403);
        assert.equal(upstreamReject.body.toString(), 'worker-rejected');
        observeAdmission(upstreamReject);

        // 4. non-upgrade requests are untouched by the gates
        const plain = await request(port, {});
        assert.equal(plain.status, 200);
        observeAdmission(plain);
        for (let i = 0; i < 32; ++i) observeAdmission(await request(port, {}));
    } finally {
        await stopHost(child);
        fs.rmSync(dir, { recursive: true, force: true });
    }
    console.log(`host ws upgrade gates (${args.get('mode') ?? 'single-worker'}): PASS`);
}

main().catch((error) => {
    console.error(error);
    process.exit(1);
});
