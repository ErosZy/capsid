import {waitForWorkerdListen} from './helpers/workerd_listener_control.mjs';
import assert from 'node:assert/strict';
import {randomUUID} from 'node:crypto';
import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import {spawn} from 'node:child_process';
import {setTimeout as delay} from 'node:timers/promises';
import {startCapsid, stopCapsid} from './helpers/ws_host_backend.mjs';
import {responseSession} from './helpers/ws_response_client.mjs';
import {root, readLock, verifyClient, verifyBinary, fileDigest} from '../tools/workerd-reference.mjs';
import {ownerInputPaths, ownerKinds, validateOwnerReport} from '../tools/workerd-owner-contract.mjs';

const args = new Map();
for (let i = 2; i < process.argv.length; i += 2) {
    assert.ok(['--reference-from-env', '--host', '--worker', '--mode', '--report'].includes(process.argv[i]), 'known owner driver option');
    assert.ok(!args.has(process.argv[i]) && process.argv[i + 1], 'unique valued owner driver option');
    args.set(process.argv[i], process.argv[i + 1]);
}
const reference = args.has('--reference-from-env') ? process.env.CAPSID_WORKERD_REF : undefined;
if (args.has('--reference-from-env')) {
    assert.equal(args.get('--reference-from-env'), '1');
    assert.ok(reference, 'CAPSID_WORKERD_REF is required for connection owner acceptance');
    for (const key of ['--host', '--worker', '--mode']) assert.ok(!args.has(key), 'reference has no product alias');
}
const host = args.get('--host'), worker = args.get('--worker');
const mode = reference ? 'reference' : args.get('--mode');
assert.ok(reference || host && worker && ['single-worker', 'static-pool', 'managed'].includes(mode));
const lock = readLock(); verifyClient(lock); if (reference) verifyBinary(reference, lock);
const inputs = Object.fromEntries(Object.entries(ownerInputPaths).map(([key, name]) => [key, fileDigest(path.join(root, name))]));
const binaries = {host: fileDigest(reference ?? host), worker: reference ? null : fileDigest(worker)};
const runId = process.env.CAPSID_WS_RUN_ID ?? randomUUID();
const report = {schema: 'capsid-websocket-owner-lifecycle-v1', complete: false, runId, mode,
    referenceLock: lock, client: {node: process.version, undici: process.versions.undici}, inputs, binaries, cases: []};
const destination = args.get('--report') ?? (process.env.CAPSID_WS_REPORT_DIR ?
    path.join(process.env.CAPSID_WS_REPORT_DIR, 'owner-lifecycle-' + mode + '.json') : undefined);
const save = () => {if (destination) fs.writeFileSync(destination, JSON.stringify(report, null, 2) + '\n');};
save();
const fixture = path.join(root, ownerInputPaths.fixture);

async function startReference(directory) {
    const deadline=performance.now()+10000;
    fs.copyFileSync(fixture, path.join(directory, 'main.mjs'));
    const config = path.join(directory, 'config.capnp');
    fs.writeFileSync(config, `using Workerd = import "/workerd/workerd.capnp";
const config :Workerd.Config = (services=[(name="main",worker=(compatibilityDate=${JSON.stringify(lock.compatibility_date)},compatibilityFlags=${JSON.stringify(lock.compatibility_flags)},modules=[(name="main",esModule=embed "main.mjs")]))],sockets=[(name="http",address="127.0.0.1:0",http=(),service="main")]);`);
    const child = spawn(reference, ['serve', config,'--control-fd=3'], {stdio:['ignore','pipe','pipe','pipe']});
    const closed = new Promise(resolve => child.once('close', resolve));
    let stderr = ''; child.stderr.on('data', data => stderr += data); child.stdout.resume();
    const backend = {child, port:null, closed, stderr: () => stderr};
    
    try{
        backend.port=(await waitForWorkerdListen(child,Math.max(1,deadline-performance.now()))).port;
        const port=backend.port;
        
        for (;;) {
            try {
                const response = await fetch(`http://127.0.0.1:${port}/state`, {signal: AbortSignal.timeout(500)});
                assert.equal(response.status, 200); await response.text(); return backend;
            } catch (error) {
                assert.ok(performance.now() < deadline && child.exitCode === null && child.signalCode === null,
                    `reference owner startup: ${stderr} ${error}`);
                await delay(20);
            }
        }
    } catch (error) {await stopReference(backend); throw error;}
}
async function stopReference(backend) {
    const child = backend.child; if (child.exitCode === null && child.signalCode === null) child.kill('SIGTERM');
    let forced = false, timer;
    try {
        await Promise.race([backend.closed, new Promise(resolve => {
            timer = setTimeout(() => {forced = true; child.kill('SIGKILL'); resolve();}, 2000);
        })]);
        await backend.closed;
    } finally {clearTimeout(timer);}
    return {forced, code: child.exitCode, signal: child.signalCode};
}
for (const kind of ownerKinds) {
    const observed = {id: kind}; report.cases.push(observed); save();
    const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'capsid-owner-lifecycle-'));
    let backend, peer;
    try {
        backend = reference ? await startReference(directory) : await startCapsid(host, worker, fixture, directory, mode);
        const prefix = reference ? '' : '/@capsid/orders';
        const query = new URLSearchParams({kind, id: kind});
        const request = target => fetch(`http://127.0.0.1:${backend.port}${prefix}/${target}?${query}`,
            {signal: AbortSignal.timeout(4000)});
        if (kind === 'http-await') {
            const response = await request('probe');
            observed.initialHTTP = {status: response.status, body: await response.text()};
        } else {
            peer = responseSession(backend.port, prefix + '/probe?' + query,
                {autoCloseReply: kind !== 'close-delayed-ack', observeCloseReply: true});
            observed.headers = await peer.headers; assert.equal(observed.headers.status, 101); peer.send('go');
            if (kind === 'cross-clear') {
                assert.equal(await peer.text(), 'queued'); const response = await request('clear');
                observed.clearResponse = {status: response.status, text: await response.text()};
            }
            if (kind === 'same-clear') {assert.equal(await peer.text(), 'queued'); peer.send('stop');}
            if (kind === 'half-open-close-await') {
                assert.equal(await peer.text(), 'queued'); peer.close(); observed.manualCloseSent = true;
            }
            if (kind === 'close-delayed-ack') {
                await delay(60); observed.beforeManualAck = peer.snapshot(); const response = await request('state');
                observed.beforeAckHTTP = {status: response.status, body: await response.text()};
                peer.close(); observed.manualCloseSent = true;
            }
            observed.wire = await peer.complete;
        }
        if (kind.startsWith('close-external-')) {
            const response = await request('resume');
            observed.resumeResponse = {status: response.status, text: await response.text()};
        }
        // Observe after every input timer has become due. A queued callback
        // may change plain JS state without touching any native API.
        await delay(150);
        const response = await request('state');
        observed.stateStatus = response.status; observed.stateText = await response.text();
        observed.state = JSON.parse(observed.stateText);
    } catch (error) {observed.failure = error.stack;}
    finally {
        peer?.dispose();
        if (backend) {
            try {observed.shutdown = reference ? await stopReference(backend) : await stopCapsid(backend.child);}
            catch (error) {observed.cleanupFailure = error.stack;}
            observed.stderr = backend.stderr();
        }
        save();
    }
}
assert.equal(fileDigest(reference ?? host), binaries.host, 'unchanged owner Host binary');
if (worker) assert.equal(fileDigest(worker), binaries.worker, 'unchanged owner Worker binary');
for (const [key, name] of Object.entries(ownerInputPaths)) assert.equal(fileDigest(path.join(root, name)), inputs[key], 'unchanged owner input ' + key);
report.complete = true;
try {validateOwnerReport(report, {runId, lock, inputs, host: binaries.host, worker: binaries.worker});}
catch (error) {report.complete = false; report.validationError = error.stack; save(); throw error;}
save(); console.log(`PASS: ${mode}, ${ownerKinds.length} connection owner lifecycle cases`);
