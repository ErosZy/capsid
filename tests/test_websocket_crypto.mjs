import assert from 'node:assert/strict';
import fs from 'node:fs';
import net from 'node:net';
import path from 'node:path';
import os from 'node:os';
import {randomUUID} from 'node:crypto';
import {spawn} from 'node:child_process';
import {setTimeout as delay} from 'node:timers/promises';
import {parseArgs} from 'node:util';
import {startCapsid, stopCapsid} from './helpers/ws_host_backend.mjs';
import {responseSession} from './helpers/ws_response_client.mjs';
import {root, readLock, verifyBinary, verifyClient, fileDigest} from '../tools/workerd-reference.mjs';
import {cryptoCases, cryptoInputPaths, validateCryptoReport} from '../tools/workerd-crypto-contract.mjs';

const {values: args} = parseArgs({options: Object.fromEntries(
    ['reference-from-env', 'host', 'worker', 'mode', 'report'].map(name => [name, {type: 'string'}]))});
const reference = args['reference-from-env'] ? path.resolve(process.env.CAPSID_WORKERD_REF ?? '') : undefined;
if (reference) {
    assert.equal(args['reference-from-env'], '1'); assert.ok(process.env.CAPSID_WORKERD_REF, 'CAPSID_WORKERD_REF required');
    for (const name of ['host', 'worker', 'mode']) assert.equal(args[name], undefined, 'no product alias for reference');
}
const host = args.host ? path.resolve(args.host) : undefined;
const worker = args.worker ? path.resolve(args.worker) : undefined;
const mode = reference ? 'reference' : args.mode;
assert.ok(reference || host && worker && ['single-worker', 'static-pool', 'managed'].includes(mode), 'actual reference or product backend');
const lock = readLock(); verifyClient(lock); if (reference) verifyBinary(reference, lock);
const fixture = path.join(root, cryptoInputPaths.fixture);
const inputs = Object.fromEntries(Object.entries(cryptoInputPaths).map(([key, file]) => [key, fileDigest(path.join(root, file))]));
const binaries = {host: fileDigest(reference ?? host), worker: reference ? null : fileDigest(worker)};
const runId = process.env.CAPSID_WS_RUN_ID ?? randomUUID();
const report = {schema: 'capsid-webcrypto-compat-v1', complete: false, mode, runId, referenceLock: lock,
    client: {node: process.version, undici: process.versions.undici}, inputs, binaries, cases: []};
const destination = path.resolve(args.report ?? (process.env.CAPSID_WS_REPORT_DIR
    ? path.join(process.env.CAPSID_WS_REPORT_DIR, 'crypto-' + mode + '.json')
    : path.join(os.tmpdir(), 'capsid-crypto-' + runId + '-' + mode + '.json')));
assert.ok(!fs.existsSync(destination), 'preserve prior crypto evidence');
fs.mkdirSync(path.dirname(destination), {recursive: true});
const save = () => fs.writeFileSync(destination, JSON.stringify(report, null, 2) + '\n');
save();

async function startReference(directory) {
    const listener = net.createServer();
    await new Promise((resolve, reject) => {listener.once('error', reject); listener.listen(0, '127.0.0.1', resolve);});
    const port = listener.address().port;
    await new Promise(resolve => listener.close(resolve));
    fs.copyFileSync(fixture, path.join(directory, 'main.mjs'));
    const config = path.join(directory, 'config.capnp');
    fs.writeFileSync(config, `using Workerd = import "/workerd/workerd.capnp";
const config :Workerd.Config = (services=[(name="main",worker=(
compatibilityDate=${JSON.stringify(lock.compatibility_date)},compatibilityFlags=${JSON.stringify(lock.compatibility_flags)},
modules=[(name="main",esModule=embed "main.mjs")]))],
sockets=[(name="http",address="127.0.0.1:${port}",http=(),service="main")]);`);
    const child = spawn(reference, ['serve', config], {stdio: ['ignore', 'pipe', 'pipe']});
    const closed = new Promise(resolve => child.once('close', resolve));
    let stderr = ''; child.stderr.on('data', bytes => {stderr += bytes;}); child.stdout.resume();
    const backend = {child, closed, port, stderr: () => stderr};
    try {
        const deadline = performance.now() + 10000;
        for (;;) {
            try {
                const response = await fetch(`http://127.0.0.1:${port}/state`, {signal: AbortSignal.timeout(500)});
                assert.equal(response.status, 200); await response.text(); return backend;
            } catch (error) {
                assert.ok(performance.now() < deadline && child.exitCode === null && child.signalCode === null, stderr + error);
                await delay(20);
            }
        }
    } catch (error) {await stopReference(backend); throw error;}
}
async function stopReference(backend) {
    if (backend.child.exitCode === null && backend.child.signalCode === null) backend.child.kill('SIGTERM');
    let timer, forced = false;
    try {
        await Promise.race([backend.closed, new Promise(resolve => {
            timer = setTimeout(() => {forced = true; backend.child.kill('SIGKILL'); resolve();}, 2000);
        })]);
        await backend.closed;
    } finally {clearTimeout(timer);}
    return {forced, code: backend.child.exitCode, signal: backend.child.signalCode};
}

for (const kind of cryptoCases) {
    const value = {kind, challenge: randomUUID()}; report.cases.push(value); save();
    const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'capsid-crypto-'));
    let backend, peer;
    try {
        backend = reference ? await startReference(directory) : await startCapsid(host, worker, fixture, directory, mode);
        const prefix = reference ? '' : '/@capsid/orders';
        const state = async () => {
            const response = await fetch(`http://127.0.0.1:${backend.port}${prefix}/state`, {signal: AbortSignal.timeout(4000)});
            return {status: response.status, text: await response.text()};
        };
        value.initial = await state();
        const query = new URLSearchParams({kind, challenge: value.challenge});
        peer = responseSession(backend.port, prefix + '/probe?' + query, {observeCloseReply: true, timeoutMs: 10000});
        value.headers = await peer.headers; assert.equal(value.headers.status, 101);
        peer.send('go'); assert.equal(await peer.text(), 'started');
        value.wire = await peer.complete; value.atClose = await state();
        // The post-terminal observation detects late jobs or a replacement realm.
        await delay(750); value.observed = await state();
    } catch (error) {value.failure = error.stack; if (error.observation) value.failureObservation = error.observation;}
    finally {
        peer?.dispose();
        if (backend) {
            try {value.shutdown = reference ? await stopReference(backend) : await stopCapsid(backend.child);}
            catch (error) {value.cleanupFailure = error.stack;}
            value.stderr = backend.stderr();
        }
        save();
    }
}
assert.deepEqual(inputs, Object.fromEntries(Object.entries(cryptoInputPaths).map(([key, file]) => [key, fileDigest(path.join(root, file))])), 'crypto input bytes unchanged');
assert.equal(fileDigest(reference ?? host), binaries.host);
if (!reference) assert.equal(fileDigest(worker), binaries.worker);
report.complete = report.cases.every(value => !value.failure && !value.cleanupFailure && value.shutdown?.forced === false);
save();
validateCryptoReport(report, mode);
console.log(JSON.stringify({complete: true, mode, cases: report.cases.length, report: destination}));
