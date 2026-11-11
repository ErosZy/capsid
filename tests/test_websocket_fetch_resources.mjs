import assert from 'node:assert/strict';
import fs from 'node:fs';
import net from 'node:net';
import path from 'node:path';
import os from 'node:os';
import {randomUUID} from 'node:crypto';
import {spawn} from 'node:child_process';
import {setTimeout as delay} from 'node:timers/promises';
import {startCapsid, stopCapsid} from './helpers/ws_host_backend.mjs';
import {responseSession} from './helpers/ws_response_client.mjs';
import {root, readLock, verifyBinary, verifyClient, fileDigest} from '../tools/workerd-reference.mjs';
import {fetchResourceCases, fetchResourceInputPaths, validateFetchResourceReport} from '../tools/workerd-fetch-resource-contract.mjs';

const args = new Map();
for (let index = 2; index < process.argv.length; index += 2) {
    assert.ok(['--reference-from-env', '--host', '--worker', '--mode', '--report'].includes(process.argv[index]), 'known fetch resource option');
    assert.ok(!args.has(process.argv[index]) && process.argv[index + 1], 'unique valued fetch resource option');
    args.set(process.argv[index], process.argv[index + 1]);
}
const referencePath = args.has('--reference-from-env') ? process.env.CAPSID_WORKERD_REF : undefined;
const reference = referencePath ? path.resolve(referencePath) : undefined;
if (args.has('--reference-from-env')) {
    assert.equal(args.get('--reference-from-env'), '1'); assert.ok(reference, 'CAPSID_WORKERD_REF required');
    for (const key of ['--host', '--worker', '--mode']) assert.ok(!args.has(key), 'no product alias for reference');
}
const host = args.has('--host') ? path.resolve(args.get('--host')) : undefined;
const worker = args.has('--worker') ? path.resolve(args.get('--worker')) : undefined;
const mode = reference ? 'reference' : args.get('--mode');
assert.ok(reference || host && worker && ['single-worker', 'static-pool', 'managed'].includes(mode));
const lock = readLock(); verifyClient(lock); if (reference) verifyBinary(reference, lock);
const fixture = path.join(root, fetchResourceInputPaths.fixture);
const inputs = Object.fromEntries(Object.entries(fetchResourceInputPaths).map(([key, name]) => [key, fileDigest(path.join(root, name))]));
const binaries = {host: fileDigest(reference ?? host), worker: reference ? null : fileDigest(worker)};
const runId = process.env.CAPSID_WS_RUN_ID ?? randomUUID();
const report = {schema: 'capsid-fetch-resources-v1', complete: false, mode, runId, referenceLock: lock,
    client: {node: process.version, undici: process.versions.undici}, inputs, binaries, cases: []};
const destination = path.resolve(args.get('--report') ?? (process.env.CAPSID_WS_REPORT_DIR
    ? path.join(process.env.CAPSID_WS_REPORT_DIR, 'fetch-resources-' + mode + '.json')
    : path.join(os.tmpdir(), 'capsid-fetch-resources-' + runId + '-' + mode + '.json')));
assert.ok(!fs.existsSync(destination), 'preserve prior fetch resource evidence');
function save() {fs.writeFileSync(destination, JSON.stringify(report, null, 2) + '\n');}
save();
async function observeUntil(predicate, milliseconds = 3000) {
    const deadline = performance.now() + milliseconds;
    while (!predicate() && performance.now() < deadline) await delay(5);
    return predicate();
}
async function startUpstream(kind, challenge, role) {
    const sockets = new Map(), records = []; let release;
    const listener = net.createServer({allowHalfOpen: true}, socket => {
        const item = {request: null, rawRequest: '', bytes: 0, peerEnded: false, peerReset: false,
            fixtureEnded: false, fixtureDestroyed: false, events: [],
            localAddress: socket.localAddress, localPort: socket.localPort}; records.push(item);
        sockets.set(socket, item);
        let input = Buffer.alloc(0);
        socket.on('error', error => {if (error.code === 'ECONNRESET') item.peerReset = true; else item.error = error.code;});
        socket.on('end', () => {item.peerEnded = true; item.events.push('peer-ended');});
        socket.on('close', () => sockets.delete(socket));
        socket.on('data', chunk => {
            input = Buffer.concat([input, chunk]); item.bytes += chunk.length; item.rawRequest = input.toString('base64');
            if (item.request !== null) return;
            const end = input.indexOf('\r\n\r\n'); if (end < 0) return;
            item.request = input.subarray(0, end).toString(); item.events.push('request');
            const complete = () => {
                const body = 'complete/' + challenge + '/' + role;
                item.fixtureEnded = true; item.events.push('sent-complete');
                socket.end('HTTP/1.1 200 OK\r\nContent-Length: ' + Buffer.byteLength(body) + '\r\nConnection: close\r\n\r\n' + body);
            };
            if (kind === 'fetch-complete' || kind === 'fetch-custom-headers' || kind === 'fetch-disabled-cookie-cache' || kind.startsWith('fetch-sync-context')) complete();
            else if (kind === 'fetch-two-owners' && role === 'b') release = complete;
            else if (kind === 'fetch-close-body' || kind === 'fetch-cancel-reader' || kind.startsWith('fetch-abort-body-')) {
                const body = 'first/' + challenge + '/' + role; item.events.push('sent-first');
                socket.write('HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n' + Buffer.byteLength(body).toString(16) + '\r\n' + body + '\r\n');
            }
        });
    });
    await new Promise((resolve, reject) => {listener.once('error', reject); listener.listen(0, '127.0.0.1', resolve);});
    return {port: listener.address().port, records, release() {assert.ok(release, 'actual foreign request before release'); release();},
        snapshot() {return structuredClone(records);}, async stop() {
            for (const [socket, item] of sockets) {item.fixtureDestroyed = true; socket.destroy();}
            await new Promise(resolve => listener.close(resolve));
        }};
}
async function startReference(directory) {
    const listener = net.createServer(); await new Promise(resolve => listener.listen(0, '127.0.0.1', resolve));
    const port = listener.address().port; await new Promise(resolve => listener.close(resolve));
    fs.copyFileSync(fixture, path.join(directory, 'main.mjs'));
    const config = path.join(directory, 'config.capnp');
    fs.writeFileSync(config, `using Workerd = import "/workerd/workerd.capnp";
const config :Workerd.Config = (services=[(name="main",worker=(compatibilityDate=${JSON.stringify(lock.compatibility_date)},compatibilityFlags=${JSON.stringify(lock.compatibility_flags)},modules=[(name="main",esModule=embed "main.mjs")])),(name="internet",network=(allow=["local"]))],sockets=[(name="http",address="127.0.0.1:${port}",http=(),service="main")]);`);
    const child = spawn(reference, ['serve', config], {stdio: ['ignore', 'pipe', 'pipe']});
    let stderr = ''; child.stderr.on('data', data => stderr += data); child.stdout.resume();
    const closed = new Promise(resolve => child.once('close', resolve)), backend = {child, closed, port, stderr: () => stderr};
    try {
        const deadline = performance.now() + 10000;
        for (;;) try {
            const response = await fetch(`http://127.0.0.1:${port}/state`, {signal: AbortSignal.timeout(500)});
            assert.equal(response.status, 200); await response.text(); return backend;
        } catch (error) {
            assert.ok(performance.now() < deadline && child.exitCode === null && child.signalCode === null, 'fetch resource reference startup: ' + error + stderr);
            await delay(20);
        }
    } catch (error) {await stopReference(backend); throw error;}
}
async function stopReference(backend) {
    let forced = false, timer;
    if (backend.child.exitCode === null && backend.child.signalCode === null) backend.child.kill('SIGTERM');
    try {await Promise.race([backend.closed, new Promise(resolve => {timer = setTimeout(() => {
        forced = true; backend.child.kill('SIGKILL'); resolve();
    }, 2000);})]); await backend.closed;} finally {clearTimeout(timer);}
    return {forced, code: backend.child.exitCode, signal: backend.child.signalCode};
}

for (const id of fetchResourceCases) {
    const value = {id, challenge: randomUUID()}, directory = fs.mkdtempSync(path.join(os.tmpdir(), 'capsid-fetch-resources-'));
    report.cases.push(value); save(); let backend, readState;
    const originalDirectory = process.cwd(), guard = path.join(directory, '.LCK');
    const guardIdentity = () => {const stat = fs.statSync(guard, {bigint: true});
        return {type: stat.isDirectory() ? 'directory' : 'other', dev: String(stat.dev), ino: String(stat.ino)};};
    if (id === 'fetch-disabled-cookie-cache') {
        fs.mkdirSync(guard); value.filesystemGuard = {path: '.LCK', before: guardIdentity()};
        process.chdir(directory);
    }
    const peers = {}, servers = [];
    try {
        servers.push(await startUpstream(id, value.challenge, 'a'));
        if (['fetch-two-owners', 'fetch-abort-live-shared'].includes(id)) servers.push(await startUpstream(id, value.challenge, id === 'fetch-two-owners' ? 'b' : 'a'));
        const targetHost = id === 'fetch-sync-context-numeric' ? '127.0.0.1' : 'localhost';
        value.targets = servers.map(server => ({host: targetHost, port: server.port}));
        const allow = servers.map(server => targetHost + ':' + server.port);
        backend = reference ? await startReference(directory) : await startCapsid(host, worker, fixture, directory, mode,
            {permissions: {modules: [], fetch: {allow}}, fetchTargets: allow});
        const prefix = reference ? '' : '/@capsid/orders';
        const state = async () => {
            const response = await fetch(`http://127.0.0.1:${backend.port}${prefix}/state`, {signal: AbortSignal.timeout(4000)});
            return {status: response.status, text: await response.text()};
        };
        readState = state;
        const snapshot = () => servers.flatMap(server => server.snapshot());
        const open = async (role, server, start = true) => {
            const target = `http://${targetHost}:${server.port}/hold?challenge=${value.challenge}&role=${role}`;
            const secondTarget = servers[1] ? `http://localhost:${servers[1].port}/hold?challenge=${value.challenge}&role=a` : '';
            const query = new URLSearchParams({kind: id, challenge: value.challenge, role, target, secondTarget});
            const peer = peers[role] = responseSession(backend.port, prefix + '/probe?' + query, {observeCloseReply: true, timeoutMs: 12000});
            value['upgrade' + role] = await peer.headers; assert.equal(value['upgrade' + role].status, 101);
            if (start) {peer.send('go'); assert.equal(await peer.text(), 'started');}
            return peer;
        };
        const syncContext = id.startsWith('fetch-sync-context');
        value.initial = await state(); const peer = await open('a', servers[0], !syncContext);
        if (syncContext) {
            await open('b', servers[0]);
            peer.send('go'); assert.equal(await peer.text(), 'started');
        }
        if (!id.startsWith('fetch-abort-pre-')) assert.ok(await observeUntil(() => servers[0].records.some(entry => entry.request)), 'actual owner upstream request');
        if (id === 'fetch-abort-live-shared') assert.ok(await observeUntil(() => servers[1].records.some(entry => entry.request)), 'actual second consumer upstream request');
        if (id === 'fetch-two-owners') {
            const other = await open('b', servers[1]);
            assert.ok(await observeUntil(() => servers[1].records.some(entry => entry.request)), 'actual foreign upstream request');
            value.beforeAction = snapshot(); peer.send('finish'); value.wirea = await peer.complete;
            await observeUntil(() => servers[0].records.every(entry => entry.peerEnded));
            other.send('checkpoint'); assert.equal(await other.text(), 'ack/checkpoint');
            value.checkpoint = await state(); value.checkpointUpstream = snapshot();
            servers[1].release(); value.wireb = await other.complete;
        } else if (syncContext) {
            value.beforeAction = snapshot(); value.wirea = await peer.complete;
            assert.ok(await observeUntil(() => servers[0].records.every(entry => entry.peerEnded)), 'completed native fetch peer EOF');
            const other = peers.b; other.send('checkpoint'); assert.equal(await other.text(), 'ack/checkpoint');
            value.checkpoint = await state(); value.checkpointUpstream = snapshot();
            other.send('finish'); value.wireb = await other.complete;
        } else {
            if (id === 'fetch-close-body' || id.startsWith('fetch-abort-body-')) assert.equal(await peer.text(), 'reading');
            value.beforeAction = snapshot();
            if (id === 'fetch-close-pending' || id === 'fetch-close-body') peer.send('finish');
            else if (id.startsWith('fetch-abort-live-') || id.startsWith('fetch-abort-body-')) peer.send('abort');
            value.wirea = await peer.complete;
        }
        await observeUntil(() => snapshot().every(entry => entry.peerEnded));
        value.observed = await state(); await delay(100); value.finalState = await state(); value.upstream = snapshot();
    } catch (error) {
        value.failure = error.stack; if (error.observation) value.failureObservation = error.observation;
        if (readState) try {value.failureState = await readState();} catch (nested) {value.failureStateError = nested.stack;}
    }
    finally {
        for (const peer of Object.values(peers)) peer.dispose();
        if (backend) {
            try {value.shutdown = reference ? await stopReference(backend) : await stopCapsid(backend.child);} catch (error) {value.cleanupFailure = error.stack;}
            value.stderr = backend.stderr();
        }
        for (const server of servers) await server.stop(); save();
        if (id === 'fetch-disabled-cookie-cache') value.filesystemGuard.after = guardIdentity();
        process.chdir(originalDirectory); save();
    }
}
for (const [key, name] of Object.entries(fetchResourceInputPaths)) assert.equal(fileDigest(path.join(root, name)), inputs[key], 'unchanged fetch resource input');
assert.equal(fileDigest(reference ?? host), binaries.host); if (!reference) assert.equal(fileDigest(worker), binaries.worker);
report.complete = report.cases.every(value => !value.failure && !value.cleanupFailure && !value.shutdown.forced); save();
console.log(JSON.stringify({collectionComplete: report.complete, report: destination}));
validateFetchResourceReport(report, {runId, lock, inputs, host: binaries.host, worker: binaries.worker});
console.log(JSON.stringify({complete: true, mode, cases: fetchResourceCases.length}));
