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
import {crossContextCases, crossContextInputPaths, validateCrossContextReport} from '../tools/workerd-cross-context-contract.mjs';

const args = new Map();
for (let index = 2; index < process.argv.length; index += 2) {
    assert.ok(['--reference-from-env','--host','--worker','--mode','--report'].includes(process.argv[index]), 'known cross-context option');
    assert.ok(!args.has(process.argv[index]) && process.argv[index + 1], 'unique valued cross-context option');
    args.set(process.argv[index], process.argv[index + 1]);
}
const reference = args.has('--reference-from-env') ? process.env.CAPSID_WORKERD_REF : undefined;
if (args.has('--reference-from-env')) {
    assert.equal(args.get('--reference-from-env'), '1'); assert.ok(reference, 'CAPSID_WORKERD_REF required');
    for (const key of ['--host','--worker','--mode']) assert.ok(!args.has(key), 'no product alias for reference');
}
const host = args.get('--host'), worker = args.get('--worker'), mode = reference ? 'reference' : args.get('--mode');
assert.ok(reference || host && worker && ['single-worker','static-pool','managed'].includes(mode));
const lock = readLock(); verifyClient(lock); if (reference) verifyBinary(reference, lock);
const fixture = path.join(root, crossContextInputPaths.fixture);
const inputs = Object.fromEntries(Object.entries(crossContextInputPaths).map(([key, name]) => [key, fileDigest(path.join(root, name))]));
const binaries = {host: fileDigest(reference ?? host), worker: reference ? null : fileDigest(worker)};
const runId = process.env.CAPSID_WS_RUN_ID ?? randomUUID();
const report = {schema: 'capsid-cross-context-v1', complete: false, mode, runId,
    referenceLock: lock, client: {node: process.version, undici: process.versions.undici}, inputs, binaries, cases: []};
const destination = args.get('--report') ?? (process.env.CAPSID_WS_REPORT_DIR ?
    path.join(process.env.CAPSID_WS_REPORT_DIR, 'cross-context-' + mode + '.json') : undefined);
if (destination) assert.ok(!fs.existsSync(destination), 'preserve prior cross-context evidence');
const save = () => {if (destination) fs.writeFileSync(destination, JSON.stringify(report, null, 2) + '\n');}; save();

async function startReference(directory) {
    const listener = net.createServer();
    await new Promise((resolve, reject) => {listener.once('error', reject); listener.listen(0, '127.0.0.1', resolve);});
    const port = listener.address().port; await new Promise(resolve => listener.close(resolve));
    fs.copyFileSync(fixture, path.join(directory, 'main.mjs'));
    const config = path.join(directory, 'config.capnp');
    fs.writeFileSync(config, `using Workerd = import "/workerd/workerd.capnp";
const config :Workerd.Config = (services=[(name="main",worker=(compatibilityDate=${JSON.stringify(lock.compatibility_date)},compatibilityFlags=${JSON.stringify(lock.compatibility_flags)},modules=[(name="main",esModule=embed "main.mjs")]))],sockets=[(name="http",address="127.0.0.1:${port}",http=(),service="main")]);`);
    const child = spawn(reference, ['serve', config], {stdio: ['ignore','pipe','pipe']});
    let stderr = ''; child.stderr.on('data', data => stderr += data); child.stdout.resume();
    const closed = new Promise(resolve => child.once('close', resolve)), backend = {child, port, closed, stderr: () => stderr};
    try {
        const deadline = performance.now() + 10000;
        for (;;) try {
            const response = await fetch(`http://127.0.0.1:${port}/state`, {signal: AbortSignal.timeout(500)});
            assert.equal(response.status, 200); await response.text(); return backend;
        } catch (error) {
            assert.ok(performance.now() < deadline && child.exitCode === null && child.signalCode === null, `cross-context reference startup: ${error} ${stderr}`);
            await delay(20);
        }
    } catch (error) {await stopReference(backend); throw error;}
}
async function stopReference(backend) {
    let forced = false, timer;
    if (backend.child.exitCode === null && backend.child.signalCode === null) backend.child.kill('SIGTERM');
    try {
        await Promise.race([backend.closed, new Promise(resolve => {
            timer = setTimeout(() => {forced = true; backend.child.kill('SIGKILL'); resolve();}, 2000);
        })]); await backend.closed;
    } finally {clearTimeout(timer);}
    return {forced, code: backend.child.exitCode, signal: backend.child.signalCode};
}

for (const {id, commands} of crossContextCases) {
    const value = {id, challenge: randomUUID(), commands: []}; report.cases.push(value); save();
    const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'capsid-cross-context-'));
    let backend; const peers = {}, finished = new Set();
    try {
        backend = reference ? await startReference(directory) : await startCapsid(host, worker, fixture, directory, mode);
        const prefix = reference ? '' : '/@capsid/orders';
        const request = async target => {
            const response = await fetch(`http://127.0.0.1:${backend.port}${prefix}/${target}${target.includes('?') ? '&' : '?'}challenge=${value.challenge}`,
                {signal: AbortSignal.timeout(4000)});
            return {status: response.status, text: await response.text()};
        };
        value.initial = await request('state');
        for (const role of ['a','b']) {
            peers[role] = responseSession(backend.port, prefix + '/probe?role=' + role + '&challenge=' + value.challenge,
                {observeCloseReply: true, timeoutMs: 10000});
            value['upgrade' + role] = await peers[role].headers;
            assert.equal(value['upgrade' + role].status, 101);
        }
        for (const [role, command] of commands) {
            if (role === 'http') value.commands.push({role, command, ...await request('http?command=' + command)});
            else {
                peers[role].send(command); const seen = [];
                for (;;) {
                    const text = await peers[role].text(); seen.push(text);
                    if (text === 'ack/' + role + '/' + command) break;
                }
                value.commands.push({role, command, seen});
                if (command === 'finish') {value['wire' + role] = await peers[role].complete; finished.add(role);}
            }
            // Separate attachment, notification and handling turns; all
            // commands also require their actual HTTP response or peer ACK.
            await delay(60);
        }
        value.state = await request('state');
        for (const role of ['a','b']) {
            if (finished.has(role)) continue;
            peers[role].send('finish'); value['wire' + role] = await peers[role].complete;
        }
        // A second state observation follows both real transport terminals.
        await delay(150); value.finalState = await request('state');
    } catch (error) {value.failure = error.stack; if (error.observation) value.failureObservation = error.observation;}
    finally {
        for (const peer of Object.values(peers)) peer.dispose();
        if (backend) {
            try {value.shutdown = reference ? await stopReference(backend) : await stopCapsid(backend.child);}
            catch (error) {value.cleanupFailure = error.stack;}
            value.stderr = backend.stderr();
        }
        save();
    }
}
for (const [key, name] of Object.entries(crossContextInputPaths)) assert.equal(fileDigest(path.join(root, name)), inputs[key], 'unchanged cross-context input ' + key);
assert.equal(fileDigest(reference ?? host), binaries.host, 'unchanged cross-context Host');
if (worker) assert.equal(fileDigest(worker), binaries.worker, 'unchanged cross-context Worker');
report.complete = true;
try {validateCrossContextReport(report, {runId, lock, inputs, host: binaries.host, worker: binaries.worker});}
catch (error) {report.complete = false; report.validationError = error.stack; save(); throw error;}
save(); console.log(`PASS: ${mode}, ${crossContextCases.length} cross-context Promise cases`);
