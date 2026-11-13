import {waitForWorkerdListen} from './helpers/workerd_listener_control.mjs';
import assert from 'node:assert/strict';
import {randomUUID} from 'node:crypto';
import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import {spawn} from 'node:child_process';
import {pathToFileURL} from 'node:url';
import {setTimeout as delay} from 'node:timers/promises';
import {startCapsid, stopCapsid} from './helpers/ws_host_backend.mjs';
import {responseSession} from './helpers/ws_response_client.mjs';
import {root, readLock, verifyClient, verifyBinary, fileDigest} from '../tools/workerd-reference.mjs';
import {readerIntrinsicInputPaths, readerIntrinsicCases, readerIntrinsicSourceNames, validateReaderIntrinsicReport} from '../tools/workerd-reader-intrinsic-contract.mjs';

const args = new Map();
for (let i = 2; i < process.argv.length; i += 2) {
    assert.ok(['--reference-from-env', '--host', '--worker', '--mode', '--report'].includes(process.argv[i]), 'known reader intrinsic driver option');
    assert.ok(!args.has(process.argv[i]) && process.argv[i + 1], 'unique valued reader intrinsic driver option');
    args.set(process.argv[i], process.argv[i + 1]);
}
const reference = args.has('--reference-from-env') ? process.env.CAPSID_WORKERD_REF : undefined;
if (args.has('--reference-from-env')) {
    assert.equal(args.get('--reference-from-env'), '1'); assert.ok(reference, 'CAPSID_WORKERD_REF is required');
    for (const key of ['--host', '--worker', '--mode']) assert.ok(!args.has(key), 'reference shared cases only');
}
const host = args.get('--host'), worker = args.get('--worker');
const mode = reference ? 'reference' : args.get('--mode');
assert.ok(reference || host && worker && ['single-worker', 'static-pool', 'managed'].includes(mode));
const family = 'shared', requestTimeoutMs = 10000;
const inventory = readerIntrinsicCases;
const lock = readLock(); verifyClient(lock); if (reference) verifyBinary(reference, lock);
const inputs = Object.fromEntries(Object.entries(readerIntrinsicInputPaths).map(([key, name]) => [key, fileDigest(path.join(root, name))]));
const binaries = {host: fileDigest(reference ?? host), worker: reference ? null : fileDigest(worker)};
const fixture = path.join(root, readerIntrinsicInputPaths.fixture), sourceNames = readerIntrinsicSourceNames(pathToFileURL(fixture).href);
const runId = process.env.CAPSID_WS_RUN_ID ?? randomUUID();
const report = {schema: 'capsid-reader-intrinsics-v1', complete: false, runId, mode, family, requestTimeoutMs,
    sourceName: sourceNames[mode], referenceLock: lock, client: {node: process.version, undici: process.versions.undici}, inputs, binaries, cases: []};
const destination = args.get('--report') ?? (process.env.CAPSID_WS_REPORT_DIR ?
    path.join(process.env.CAPSID_WS_REPORT_DIR, 'reader-intrinsics-' + family + '-' + mode + '.json') : undefined);
if (destination) assert.ok(!fs.existsSync(destination), 'preserve existing async evidence');
const save = () => {if (destination) fs.writeFileSync(destination, JSON.stringify(report, null, 2) + '\n');};
save();
async function startReference(directory) {
    const deadline=performance.now()+10000;
    fs.copyFileSync(fixture, path.join(directory, 'main.mjs'));
    const config = path.join(directory, 'config.capnp');
    fs.writeFileSync(config, `using Workerd = import "/workerd/workerd.capnp";
const config :Workerd.Config = (services=[(name="main",worker=(compatibilityDate=${JSON.stringify(lock.compatibility_date)},compatibilityFlags=${JSON.stringify(lock.compatibility_flags)},modules=[(name="main",esModule=embed "main.mjs")]))],sockets=[(name="http",address="127.0.0.1:0",http=(),service="main")]);`);
    const child = spawn(reference, ['serve', config,'--control-fd=3'], {stdio:['ignore','pipe','pipe','pipe']});
    const backend = {child, port:null, closed: new Promise(resolve => child.once('close', resolve)), stderr: () => stderr};
    let stderr = ''; child.stderr.on('data', data => stderr += data);
    
    try{
        backend.port=(await waitForWorkerdListen(child,Math.max(1,deadline-performance.now()))).port;
        const port=backend.port;
        
        for (;;) try {
            const response = await fetch(`http://127.0.0.1:${port}/state`, {signal: AbortSignal.timeout(500)});
            assert.equal(response.status, 200); await response.text(); return backend;
        } catch (error) {
            assert.ok(performance.now() < deadline && child.exitCode === null && child.signalCode === null, `async reference startup: ${error} ${stderr}`);
            await delay(20);
        }
    } catch (error) {await stopReference(backend); throw error;}
}
async function stopReference(backend) {
    const child = backend.child; if (child.exitCode === null && child.signalCode === null) child.kill('SIGTERM');
    let forced = false, timer;
    try {
        await Promise.race([backend.closed, new Promise(resolve => {
            timer = setTimeout(() => {forced = true; child.kill('SIGKILL'); resolve();}, 2000);
        })]); await backend.closed;
    } finally {clearTimeout(timer);}
    return {forced, code: child.exitCode, signal: child.signalCode};
}
for (const kind of inventory) {
    const observed = {id: kind, challenge: randomUUID()}; report.cases.push(observed); save();
    const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'capsid-reader-intrinsics-'));
    let backend, peer, stdout = '';
    try {
        backend = reference ? await startReference(directory) : await startCapsid(host, worker, fixture, directory, mode, {requestTimeoutMs});
        backend.child.stdout.on('data', data => stdout += data);
        const prefix = reference ? '' : '/@capsid/orders';
        const request = target => fetch(`http://127.0.0.1:${backend.port}${prefix}/${target}`, {signal: AbortSignal.timeout(4000)});
        const initial = await request('state'); assert.equal(initial.status, 200); observed.initialState = await initial.json();
        const query = new URLSearchParams({kind, challenge: observed.challenge});
        if (kind.startsWith('http-')) {
            const response = await request('probe?' + query);
            observed.response = {status: response.status, text: await response.text()};
        } else {
            peer = responseSession(backend.port, prefix + '/probe?' + query, {observeCloseReply: true});
            observed.headers = await peer.headers; assert.equal(observed.headers.status, 101);
            const started = performance.now(); peer.send('go');
            try {observed.wire = await peer.complete;} catch (error) {observed.wireFailure = error.message; observed.wire = error.observation;}
            observed.elapsedMs = performance.now() - started;
        }
        const response = await request('state'); observed.stateStatus = response.status;
        observed.stateText = await response.text(); observed.state = JSON.parse(observed.stateText);
    } catch (error) {observed.failure = error.stack;}
    finally {
        peer?.dispose();
        if (backend) {
            try {observed.shutdown = reference ? await stopReference(backend) : await stopCapsid(backend.child);}
            catch (error) {observed.cleanupFailure = error.stack;}
            observed.stderr = backend.stderr(); observed.stdout = stdout;
        }
        save();
    }
}
assert.equal(fileDigest(reference ?? host), binaries.host, 'unchanged async Host binary');
if (worker) assert.equal(fileDigest(worker), binaries.worker, 'unchanged async Worker binary');
for (const [key, name] of Object.entries(readerIntrinsicInputPaths)) assert.equal(fileDigest(path.join(root, name)), inputs[key], 'unchanged async input ' + key);
report.complete = true;
try {validateReaderIntrinsicReport(report, {runId, lock, inputs, sourceNames, host: binaries.host, worker: binaries.worker}, family);}
catch (error) {report.complete = false; report.validationError = error.stack; save(); throw error;}
save(); console.log(`PASS: ${mode}, ${inventory.length} ${family} reader intrinsic cases`);
