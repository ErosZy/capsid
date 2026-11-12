import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import net from 'node:net';
import {spawn} from 'node:child_process';
import {createHash} from 'node:crypto';
import {setTimeout as delay} from 'node:timers/promises';
import {fileURLToPath} from 'node:url';
import {nativeFetchAbortPrefixInputPaths, nativeFetchAbortPrefixPlans, nativeFetchAbortPrefixReply} from '../tools/native-fetch-abort-prefix-input.mjs';
import {assertNativeFetchAbortPrefixCase, validateNativeFetchAbortPrefixReport} from '../tools/native-fetch-abort-prefix-contract.mjs';

const root = fileURLToPath(new URL('../', import.meta.url));
const hash = file => createHash('sha256').update(fs.readFileSync(file)).digest('hex');
const args = new Map();
for (let index = 2; index < process.argv.length; index += 2) {
    const name = process.argv[index], value = process.argv[index+1];
    assert.ok(['--helper', '--worker'].includes(name) && value && !args.has(name));
    args.set(name, value);
}
assert.equal(args.size, 2, 'actual C ABI helper and worker');
const helper = args.get('--helper'), worker = args.get('--worker');
const plans = nativeFetchAbortPrefixPlans;
const fixture = fileURLToPath(new URL('./fixtures/native-fetch-abort-prefix.mjs', import.meta.url));
const expected = {runId: process.env.CAPSID_WS_RUN_ID ?? 'standalone-native-abort-prefix',
    inputs: Object.fromEntries(Object.entries(nativeFetchAbortPrefixInputPaths).map(([key, name]) => [key, hash(path.join(root, name))])),
    binaries: {helper: hash(helper), worker: hash(worker)}};
const report = {schema: 'capsid-native-fetch-abort-prefix-v1', complete: false, runId: expected.runId,
    inputs: expected.inputs, binaries: expected.binaries, cases: []};
let destination;
if (process.env.CAPSID_WS_REPORT_DIR) {
    fs.mkdirSync(process.env.CAPSID_WS_REPORT_DIR, {recursive: true});
    destination = path.join(process.env.CAPSID_WS_REPORT_DIR, 'native-fetch-abort-prefix.json');
    assert.equal(fs.existsSync(destination), false, 'fresh observations only');
}
const save = () => {if (destination) fs.writeFileSync(destination, JSON.stringify(report, null, 2)+'\n');};
save();
for (const plan of plans) {
    const row = {plan}; report.cases.push(row);
    const sockets = new Set(); let record;
    const reply = nativeFetchAbortPrefixReply(plan);
    const server = net.createServer({allowHalfOpen: true}, socket => {
        sockets.add(socket);
        record = {request: null, rawRequest: '', peerEnded: false, peerReset: false, peerClosed: false, fixtureEnded: false,
            fixtureDestroyed: false, events: [], packetSHA256: createHash('sha256').update(reply.packet).digest('hex')};
        row.source = record;
        socket.on('end', () => {record.peerEnded = true; record.events.push('peer-ended');});
        socket.on('close', () => {record.peerClosed = true; sockets.delete(socket);});
        socket.on('error', error => {if (error.code === 'ECONNRESET') record.peerReset = true; else record.error = error.code;});
        let request = Buffer.alloc(0);
        socket.on('data', data => {
            request = Buffer.concat([request, data]); record.rawRequest = request.toString('base64');
            if (record.request !== null) return;
            const end = request.indexOf('\r\n\r\n'); if (end < 0) return;
            record.request = request.subarray(0, end).toString(); record.events.push('request');
            record.fixtureEnded = true; record.events.push('sent-complete'); socket.end(reply.packet);
        });
    });
    await new Promise((resolve, reject) => {server.once('error', reject); server.listen(0, '127.0.0.1', resolve);});
    row.port = server.address().port; row.encodedBytes = reply.payload.length;
    row.payloadSHA256 = createHash('sha256').update(reply.payload).digest('hex');
    row.packetSHA256 = createHash('sha256').update(reply.packet).digest('hex');
    const child = spawn(helper, [worker, fixture, 'http://localhost:'+row.port+'/hold', '0', String(plan.view)], {stdio: ['pipe', 'pipe', 'pipe']});
    let stdout = '', stderr = '', resolveState;
    const stateReady = new Promise(resolve => {resolveState = resolve;});
    child.stdout.on('data', data => {
        stdout += data;
        const line = stdout.split(/\r?\n/).find(line => line.startsWith('LIMIT_PROBE '));
        if (line) try {row.state = JSON.parse(line.slice(12)); resolveState();} catch {}
    });
    child.stderr.on('data', data => {stderr += data;});
    const closed = new Promise((resolve, reject) => {child.once('close', (code, signal) => resolve({code, signal})); child.once('error', reject);});
    const deadline = AbortSignal.timeout(12000);
    const bounded = promise => Promise.race([promise, new Promise((_, reject) => {
        if (deadline.aborted) reject(deadline.reason);
        else deadline.addEventListener('abort', () => reject(deadline.reason), {once: true});
    })]);
    let failure, acknowledged = false;
    try {
        await bounded(Promise.race([stateReady, closed.then(() => {assert.ok(row.state, 'helper produced actual state');})]));
        const end = performance.now()+3000;
        while (!record?.peerClosed && performance.now() < end) await delay(5);
        row.sourceEndedBeforeFinish = record?.peerEnded === true;
        row.sourceTerminalBeforeFinish = record?.peerClosed === true && (record.peerEnded || record.peerReset || record.error === 'EPIPE');
        row.sourceBeforeFinish = record ? structuredClone(record) : null;
        child.stdin.end('finish\n'); acknowledged = true;
        row.shutdown = await bounded(closed);
        row.stdout = stdout; row.stderr = stderr;
        const exit = stdout.split(/\r?\n/).filter(line => line.startsWith('LIMIT_EXIT '));
        assert.equal(exit.length, 1, 'one actual OS exit observation');
        row.workerExit = JSON.parse(exit[0].slice(11));
    } catch (error) {
        failure = error; row.collectionFailure = String(error.stack ?? error);
        if (!acknowledged) child.stdin.end('finish\n');
        if (child.exitCode === null && child.signalCode === null) child.kill();
        await Promise.race([closed.catch(() => {}), delay(2000)]);
        row.stdout = stdout; row.stderr = stderr;
    } finally {
        for (const socket of sockets) {record.fixtureDestroyed = true; socket.destroy();}
        await new Promise(resolve => server.close(resolve));
        save();
        console.log('NATIVE_ABORT_PREFIX_ROW '+JSON.stringify(row));
    }
    if (failure) throw failure;
    assertNativeFetchAbortPrefixCase(row, plan);
}
assert.equal(hash(helper), expected.binaries.helper);
assert.equal(hash(worker), expected.binaries.worker);
assert.deepEqual(Object.fromEntries(Object.entries(nativeFetchAbortPrefixInputPaths).map(([key, name]) => [key, hash(path.join(root, name))])), expected.inputs);
report.complete = true; save();
validateNativeFetchAbortPrefixReport(report, expected);
console.log(JSON.stringify({complete: true, cases: plans.length, actualAbortTerminal: true, actualCleanExit: true}));
