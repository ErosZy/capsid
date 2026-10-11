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

function request(port, headers, target = '/@capsid/orders/') {
    return new Promise((resolve, reject) => {
        const req = http.request({
            host: '127.0.0.1',
            port,
            method: 'GET',
            path: target,
            headers: { host: 'client.example', ...headers },
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

async function startHost(args) {
    const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'capsid-ws-upgrade-'));
    const bundle = path.join(dir, 'fixture.js');
    // Any non-101 worker answer exercises the relay/reject path.
    fs.writeFileSync(bundle, [
        'export default {',
        '  async fetch(request) {',
        '    return new Response("plain", { status: 200 });',
        '  },',
        '};',
        '',
    ].join('\n'));
    const child = spawn(args.get('host'), [
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
        '--strict-sandbox', 'off',
        '--ready-fd', '3',
    ], { stdio: [ 'ignore', 'pipe', 'pipe', 'pipe' ] });
    let stderr = '';
    child.stderr.setEncoding('utf8');
    child.stderr.on('data', (chunk) => { stderr += chunk; });
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

        // 3. valid handshake, non-101 worker answer -> relayed unchanged
        const relayed = await request(port, upgradeHeaders);
        assert.equal(relayed.status, 200);
        assert.equal(relayed.body.toString('utf8'), 'plain');

        // 4. non-upgrade requests are untouched by the gates
        const plain = await request(port, {});
        assert.equal(plain.status, 200);
    } finally {
        child.kill('SIGKILL');
        await delay(50);
        fs.rmSync(dir, { recursive: true, force: true });
    }
    console.log('host ws upgrade gates: PASS');
}

main().catch((error) => {
    console.error(error);
    process.exit(1);
});
