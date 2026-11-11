import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import net from 'node:net';
import {spawn} from 'node:child_process';
import {randomUUID} from 'node:crypto';
import {setTimeout as delay} from 'node:timers/promises';
import {root, readLock, verifyClient, verifyBinary, fileDigest} from '../tools/workerd-reference.mjs';
import {fetchUpgradeCases, fetchUpgradeInputPaths, validateFetchUpgradeReport} from '../tools/workerd-fetch-upgrade-contract.mjs';
import {fetchUpgradePeer} from './helpers/ws_fetch_upgrade_peer.mjs';
import {startCapsid, stopCapsid} from './helpers/ws_host_backend.mjs';
import {responseSession} from './helpers/ws_response_client.mjs';

const options = new Map(), allowed = new Set(['--host', '--worker', '--mode', '--report', '--reference-from-env']);
for (let index = 2; index < process.argv.length; index += 2) {
    const key = process.argv[index], value = process.argv[index + 1];
    assert.ok(allowed.has(key) && value && !options.has(key), 'known unique option/value pairs'); options.set(key, value);
}
const fromEnv = options.get('--reference-from-env'); assert.ok(fromEnv === undefined || fromEnv === '1');
const mode = fromEnv ? 'reference' : options.get('--mode');
assert.ok(['reference', 'single-worker', 'static-pool', 'managed'].includes(mode));
assert.ok(!fromEnv || !options.has('--mode') && !options.has('--host') && !options.has('--worker'));
assert.ok(fromEnv || options.get('--host') && options.get('--worker'));
const binary = fromEnv ? process.env.CAPSID_WORKERD_REF : options.get('--host'), worker = options.get('--worker');
if (!binary && fromEnv && process.env.CAPSID_WORKERD_REQUIRED !== '1') {console.log('SKIP: locked native workerd reference is unavailable'); process.exit(77);}
assert.ok(binary, 'CAPSID_WORKERD_REF required');
const lock = readLock(); verifyClient(lock); if (fromEnv) verifyBinary(binary, lock);
const inputs = Object.fromEntries(Object.entries(fetchUpgradeInputPaths).map(([name, file]) => [name, fileDigest(path.join(root, file))]));
const binaries = {host: fileDigest(binary), worker: fromEnv ? null : fileDigest(worker)};
const destination = options.get('--report') ?? (process.env.CAPSID_WS_REPORT_DIR ? path.join(process.env.CAPSID_WS_REPORT_DIR, 'fetch-upgrade-' + mode + '.json') : null);
if (destination) {assert.equal(fs.existsSync(destination), false); fs.mkdirSync(path.dirname(destination), {recursive: true});}
const report = {schema: 'capsid-fetch-upgrade-v1', complete: false, mode, runId: process.env.CAPSID_WS_RUN_ID ?? randomUUID(),
    referenceLock: lock, client: {node: process.version, undici: process.versions.undici}, inputs, binaries, cases: []};
const save = () => {if (destination) fs.writeFileSync(destination, JSON.stringify(report, null, 2) + '\n');}; save();
const fixture = path.join(root, fetchUpgradeInputPaths.fixture);
async function stopReference(backend) {
    let timer, forced = false;
    if (backend.child.exitCode === null && backend.child.signalCode === null) backend.child.kill('SIGTERM');
    try {await Promise.race([backend.closed, new Promise(resolve => {timer = setTimeout(() => {forced = true; backend.child.kill('SIGKILL'); resolve();}, 2000);})]); await backend.closed;}
    finally {clearTimeout(timer);}
    return {code: backend.child.exitCode, signal: backend.child.signalCode, forced};
}
async function reference(directory) {
    const listener = net.createServer(); await new Promise(resolve => listener.listen(0, '127.0.0.1', resolve));
    const port = listener.address().port; await new Promise(resolve => listener.close(resolve));
    fs.copyFileSync(fixture, path.join(directory, 'main.mjs')); const config = path.join(directory, 'config.capnp');
    fs.writeFileSync(config, `using Workerd = import "/workerd/workerd.capnp"; const config :Workerd.Config = (services=[(name="main",worker=(compatibilityDate=${JSON.stringify(lock.compatibility_date)},compatibilityFlags=${JSON.stringify(lock.compatibility_flags)},modules=[(name="main",esModule=embed "main.mjs")])),(name="internet",network=(allow=["local"]))],sockets=[(name="http",address="127.0.0.1:${port}",http=(),service="main")]);`);
    const child = spawn(binary, ['serve', config], {stdio: ['ignore', 'pipe', 'pipe']}); let stderr = '';
    child.stdout.resume(); child.stderr.on('data', value => {stderr += value;});
    const backend = {child, port, closed: new Promise(resolve => child.once('close', resolve)), stderr: () => stderr};
    try {
        const deadline = performance.now() + 10000;
        for (;;) try {const value = await fetch(`http://127.0.0.1:${port}/state`, {signal: AbortSignal.timeout(500)}); assert.equal(value.status, 200); await value.text(); return backend;}
        catch (error) {assert.ok(performance.now() < deadline && child.exitCode === null && child.signalCode === null, stderr + error); await delay(20);}
    } catch (error) {await stopReference(backend); throw error;}
}
async function waitFor(predicate, label) {
    const deadline = performance.now() + 5000;
    while (!predicate()) {assert.ok(performance.now() < deadline, label); await delay(5);}
}
for (const kind of fetchUpgradeCases) {
    const value = {kind, challenge: randomUUID()}, directory = fs.mkdtempSync(path.join(os.tmpdir(), 'capsid-fetch-upgrade-'));
    report.cases.push(value); save(); let backend, peer, upstream;
    let state;
    try {
        upstream = await fetchUpgradePeer(kind, value.challenge); value.target = {host: 'localhost', port: upstream.port};
        const allow = ['localhost:' + upstream.port];
        backend = fromEnv ? await reference(directory) : await startCapsid(binary, worker, fixture, directory, mode,
            {permissions: {modules: [], fetch: {allow}}, fetchTargets: allow});
        const prefix = fromEnv ? '' : '/@capsid/orders';
        state = async () => {const response = await fetch(`http://127.0.0.1:${backend.port}${prefix}/state`, {signal: AbortSignal.timeout(4000)}); return {status: response.status, text: await response.text()};};
        value.initial = await state();
        const query = new URLSearchParams({kind, challenge: value.challenge, target: `http://localhost:${upstream.port}/hold?challenge=${value.challenge}`});
        peer = responseSession(backend.port, prefix + '/probe?' + query, {observeCloseReply: true, timeoutMs: 15000});
        value.headers = await peer.headers; assert.equal(value.headers.status, 101); peer.send('go'); assert.equal(await peer.text(), 'started');
        value.checkpoint = null; value.checkpointUpstream = [];
        if (kind === 'upgrade-non101-stream') {
            assert.equal(await peer.text(), 'body-first'); value.checkpoint = await state(); value.checkpointUpstream = upstream.snapshot(); upstream.release();
        } else if (kind === 'upgrade-abort-pending') {
            await waitFor(() => upstream.snapshot().some(entry => entry.events.includes('response-held')), 'actual pending handshake');
            value.checkpoint = await state(); value.checkpointUpstream = upstream.snapshot(); peer.send('abort'); assert.equal(await peer.text(), 'ack/abort');
        }
        value.wire = await peer.complete; value.atClose = await state(); value.observed = await state();
        await waitFor(() => upstream.snapshot().every(entry => entry.peerEnded), 'actual upstream EOF before disposal');
        value.upstream = upstream.snapshot();
    } catch (error) {
        value.failure = error.stack; if (error.observation) value.failureObservation = error.observation;
        if (upstream) value.upstream = upstream.snapshot();
        if (state) try {value.observed = await state();} catch (stateError) {value.stateFailure = stateError.stack;}
    } finally {
        peer?.dispose();
        if (backend) {
            try {value.shutdown = fromEnv ? await stopReference(backend) : await stopCapsid(backend.child);} catch (error) {value.cleanupFailure = error.stack;}
            value.stderr = backend.stderr();
        }
        if (upstream) await upstream.stop(); save();
    }
}
assert.deepEqual(inputs, Object.fromEntries(Object.entries(fetchUpgradeInputPaths).map(([name, file]) => [name, fileDigest(path.join(root, file))])));
assert.deepEqual(binaries, {host: fileDigest(binary), worker: fromEnv ? null : fileDigest(worker)});
report.complete = report.cases.every(value => !value.failure && !value.cleanupFailure); save();
validateFetchUpgradeReport(report, {runId: report.runId, lock, inputs, host: binaries.host, worker: binaries.worker});
console.log(JSON.stringify({complete: true, mode, cases: report.cases.length}));
