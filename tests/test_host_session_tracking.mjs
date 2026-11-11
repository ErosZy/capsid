import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import http from 'node:http';
import { setTimeout as delay } from 'node:timers/promises';
import { startCapsid, stopCapsid } from './helpers/ws_host_backend.mjs';
import { fileDigest } from '../tools/workerd-reference.mjs';

const args = process.argv.slice(2);
const option = name => args[args.indexOf(name) + 1];
const host = option('--host'), worker = option('--worker'), mode = option('--mode');
assert.ok(host && worker && ['single-worker', 'static-pool', 'managed'].includes(mode));
const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'capsid-session-tracking-'));
const agents = [];
let backend, shutdown;
async function open() {
    const agent = new http.Agent({ keepAlive: true, maxSockets: 1, maxFreeSockets: 1 });
    const client = { agent, socket: null };
    agents.push(agent);
    await request(client);
    return client;
}
async function request(client) {
    return new Promise((resolve, reject) => {
        const req = http.get({ host: '127.0.0.1', port: backend.port,
            path: '/@capsid/orders/', headers: { host: 'public.example' }, agent: client.agent }, response => {
            let body = '';
            response.on('data', bytes => body += bytes);
            response.on('error', reject);
            response.on('end', () => { assert.equal(response.statusCode, 200); assert.equal(body, 'ok'); resolve(); });
        });
        req.on('socket', socket => { client.socket = socket; });
        req.on('error', reject);
        req.setTimeout(3000, () => req.destroy(new Error('response deadline')));
    });
}
try {
    const bundle = path.join(dir, 'main.mjs');
    fs.writeFileSync(bundle, "export default {fetch(){return new Response('ok')}};");
    backend = await startCapsid(host, worker, bundle, dir, mode);
    const first = await open(), second = await open(), third = await open();
    assert.equal(new Set([first.socket, second.socket, third.socket]).size, 3);
    const closed = new Promise(resolve => first.socket.once('close', resolve));
    first.agent.destroy(); await closed; await delay(50);
    // A round trip on a surviving connection is a listener progress barrier.
    await request(second);
    const fourth = await open();
    assert.notEqual(fourth.socket, third.socket);
    // Keep every surviving client open. The listener must still own and
    // close the third connection after the first connection left a hole.
    shutdown = await stopCapsid(backend.child);
    assert.equal(shutdown.forced, false, backend.stderr());
    assert.equal(shutdown.code, 0, backend.stderr());
    console.log(JSON.stringify({ complete: true, mode, shutdown,
        hostSHA256: fileDigest(host), workerSHA256: fileDigest(worker),
        sourceSHA256: fileDigest(new URL(import.meta.url)) }));
} finally {
    for (const agent of agents) agent.destroy();
    if (backend && !shutdown) await stopCapsid(backend.child);
    fs.rmSync(dir, { recursive: true, force: true });
}
