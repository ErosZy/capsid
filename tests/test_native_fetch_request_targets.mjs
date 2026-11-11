import assert from 'node:assert/strict';
import {spawn} from 'node:child_process';
import {createHash, randomUUID} from 'node:crypto';
import fs from 'node:fs';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import {parseArgs} from 'node:util';
import {setTimeout as delay} from 'node:timers/promises';
import {readLock, verifyBinary, verifyClient} from '../tools/workerd-reference.mjs';
import {parseWorkerdListenPort} from '../tools/workerd-fetch-byob-contract.mjs';
import {referenceLockName} from '../tools/workerd-platform.mjs';

const {values: args} = parseArgs({options: Object.fromEntries(['helper', 'worker', 'reference', 'report'].map(k => [k, {type: 'string'}]))});
args.reference ??= process.env.CAPSID_WORKERD_REF;
for (const key of ['helper', 'worker', 'reference']) assert.ok(args[key] && path.isAbsolute(args[key]), key + ' actual executable');
const lock = readLock(); verifyClient(lock); verifyBinary(args.reference, lock);
const hash = file => createHash('sha256').update(fs.readFileSync(file)).digest('hex');
const fixture = fileURLToPath(new URL('./fixtures/native-fetch-request-targets.mjs', import.meta.url));
const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'capsid-native-request-targets-'));
const destination = args.report ?? path.join(process.env.CAPSID_WS_REPORT_DIR ?? directory, 'native-fetch-request-targets.json');
assert.ok(!fs.existsSync(destination), 'fresh request-target observations'); fs.mkdirSync(path.dirname(destination), {recursive: true});
const measured = [args.helper, args.worker, args.reference, fixture, fileURLToPath(import.meta.url),
    fileURLToPath(new URL('../tools/workerd-reference.mjs', import.meta.url)),
    fileURLToPath(new URL('../tools/workerd-fetch-byob-contract.mjs', import.meta.url)),
    fileURLToPath(new URL('../tools/workerd-platform.mjs', import.meta.url)),
    fileURLToPath(new URL('./fixtures/workerd-ws/' + referenceLockName(process.platform, process.arch), import.meta.url))];
const inputs = Object.fromEntries(measured.map(file => [file, hash(file)]));
const plans = [64, 1023, 1024, 1025, 4095, 4096, 4097, 8192].map(bytes => ({id: 'path-' + bytes, bytes}));
plans.push({id: 'query-8192', bytes: 8192, query: true}, {id: 'escaped-query', escaped: true});
const report = {schema: 'capsid-native-request-targets-v1', complete: false, platform: process.platform,
    architecture: process.arch, referenceLock: lock, inputs, plans, shapes: ['inline', 'separate'],
    nativeHelperEventDeadlineMilliseconds: 5000, workerProgramUsesNoTimers: true, cases: []};
const save = () => fs.writeFileSync(destination, JSON.stringify(report, null, 2) + '\n'); save();
async function until(read, milliseconds, message) {
    const end = performance.now() + milliseconds;
    for (;;) { if (await read()) return; assert.ok(performance.now() < end, message); await delay(2); }
}
async function source(plan, token) {
    const sockets = new Set(), records = [];
    const accept = socket => {
        sockets.add(socket); const record = {rawRequest: '', peerEnded: false, reset: false, replied: false}; records.push(record);
        let wire = Buffer.alloc(0);
        socket.on('error', error => {record.reset = error.code;});
        socket.on('end', () => {record.peerEnded = true;}); socket.on('close', () => sockets.delete(socket));
        socket.on('data', chunk => {
            wire = Buffer.concat([wire, chunk]); record.rawRequest = wire.toString('base64');
            if (wire.length > 65536) {record.error = 'oversized real source request'; socket.destroy(); return;}
            if (record.replied || !wire.includes('\r\n\r\n')) return;
            record.replied = true;
            socket.end('HTTP/1.1 200 OK\r\nContent-Length: ' + token.length + '\r\nConnection: close\r\n\r\n' + token);
        });
    };
    const v4 = net.createServer({allowHalfOpen: true}, accept), v6 = net.createServer({allowHalfOpen: true}, accept);
    await new Promise((resolve, reject) => {v4.once('error', reject); v4.listen(0, '127.0.0.1', resolve);});
    const port = v4.address().port;
    try { await new Promise((resolve, reject) => {v6.once('error', reject); v6.listen({port, host: '::1', ipv6Only: true}, resolve);}); }
    catch (error) {await new Promise(resolve => v4.close(resolve)); throw error;}
    const prefix = '/' + token + (plan.query ? '?value=' : '/');
    const target = plan.escaped ? '/' + token + '/a%2Fb?one=%26%3F%00&two=+%2B' : prefix + 'x'.repeat(plan.bytes - prefix.length);
    return {target: 'http://localhost:' + port + target, expected: 'GET ' + target + ' HTTP/1.1', records,
        async close() { for (const socket of sockets) socket.destroy(); await Promise.all([v4, v6].map(server => new Promise(resolve => server.close(resolve)))); }};
}
async function startReference() {
    fs.copyFileSync(fixture, path.join(directory, 'main.mjs'));
    const config = path.join(directory, 'main.capnp');
    fs.writeFileSync(config, `using Workerd = import "/workerd/workerd.capnp"; const config :Workerd.Config =
        (services=[(name="main",worker=(compatibilityDate=${JSON.stringify(lock.compatibility_date)},compatibilityFlags=${JSON.stringify(lock.compatibility_flags)},
        modules=[(name="main",esModule=embed "main.mjs")])),(name="internet",network=(allow=["local"]))],
        sockets=[(name="http",address="127.0.0.1:0",http=(),service="main")]);`);
    const child = spawn(args.reference, ['serve', config, '--control-fd=3'], {stdio: ['ignore', 'pipe', 'pipe', 'pipe']});
    let stderr = ''; child.stdout.resume(); child.stderr.on('data', data => {stderr += data;});
    const backend = {child, stderr: () => stderr, closed: new Promise((resolve, reject) => {child.once('error', reject); child.once('close', (code, signal) => resolve({code, signal}));})};
    try {
        backend.port = await new Promise((resolve, reject) => {
            let buffer = '', finished = false;
            const done = (error, port) => {if (finished) return; finished = true; clearTimeout(timer); error ? reject(error) : resolve(port);};
            const timer = setTimeout(() => done(new Error('locked workerd listener control deadline: ' + stderr)), 10000);
            child.stdio[3].setEncoding('utf8'); child.stdio[3].on('data', data => {
                buffer += data; if (buffer.length > 65536) return done(new Error('bounded workerd listener control'));
                if (buffer.includes('\n')) try {done(null, parseWorkerdListenPort(buffer.split('\n')[0]));} catch (error) {done(error);}
            });
            child.stdio[3].once('end', () => done(new Error('workerd listener control ended')));
            child.stdio[3].once('error', done); child.once('error', done);
            child.once('exit', () => done(new Error('workerd exited before listener control: ' + stderr)));
        });
        return backend;
    } catch (error) {await stopReference(backend); throw error;}
}
async function stopReference(backend) {
    let forced = false;
    const timer = setTimeout(() => {forced = true; backend.child.kill('SIGKILL');}, 2000);
    try {if (backend.child.exitCode === null && backend.child.signalCode === null) backend.child.kill('SIGTERM');
        const result = await backend.closed; return {...result, forced, stderr: backend.stderr()};
    } finally {clearTimeout(timer);}
}
async function native(target, shape, beforeFinish) {
    const child = spawn(args.helper, [args.worker, fixture, encodeURIComponent(target), '0', '8&shape=' + shape], {stdio: ['pipe', 'pipe', 'pipe'], windowsHide: true});
    let stdout = '', stderr = '', forced = false, resolveState, rejectState, state;
    const ready = new Promise((resolve, reject) => {resolveState = resolve; rejectState = reject;});
    child.stdin.on('error', () => {}); child.stdout.on('data', data => {stdout += data;
        const line = stdout.split(/\r?\n/).find(line => line.startsWith('LIMIT_PROBE '));
        if (line) try {state = JSON.parse(line.slice(12)); resolveState();} catch (error) {rejectState(error);}
    }); child.stderr.on('data', data => {stderr += data;});
    const closed = new Promise((resolve, reject) => {child.once('error', reject); child.once('close', (code, signal) => resolve({code, signal}));});
    const timer = setTimeout(() => {forced = true; child.kill('SIGKILL');}, 12000);
    let result;
    try {
        await Promise.race([ready, closed.then(() => {assert.ok(state, 'actual native target response: ' + stderr);})]);
        await beforeFinish();
        child.stdin.end('finish\n'); result = await closed;
        const exits = stdout.split(/\r?\n/).filter(line => line.startsWith('LIMIT_EXIT '));
        assert.equal(exits.length, 1, 'actual worker OS exit observation');
        const workerExit = JSON.parse(exits[0].slice(11));
        assert.deepEqual(result, {code: 0, signal: null}, stderr); assert.equal(forced, false);
        assert.deepEqual(workerExit, {clean: true, processObserved: true, exitStatus: 0, forced: false, errorEvents: 0});
        assert.equal(stderr, ''); return {state, result, workerExit, stdout, stderr, forced};
    } finally {
        if (child.exitCode === null && child.signalCode === null) {
            if (state) child.stdin.end('finish\n');
            else {forced = true; child.kill('SIGTERM');}
            await closed;
        }
        clearTimeout(timer);
    }
}
let backend, failure;
try {
    backend = await startReference();
    for (const shape of report.shapes) for (const plan of plans) {
        const token = randomUUID(), peer = await source(plan, token), row = {plan: plan.id, shape, expectedFirstLine: peer.expected};
        report.cases.push(row); save();
        try {
            for (const role of ['reference', 'native']) {
                const index = peer.records.length;
                if (role === 'reference') {
                    const response = await fetch('http://127.0.0.1:' + backend.port + '/probe?' + new URLSearchParams({target: peer.target, shape}), {signal: AbortSignal.timeout(5000)});
                    assert.equal(response.status, 200); row[role] = {state: await response.json()};
                } else row[role] = await native(peer.target, shape, async () => {
                    await until(() => peer.records.length === index + 1 && peer.records[index].peerEnded, 2000, 'native source FIN before helper finish');
                    row.nativeSourceBeforeFinish = structuredClone(peer.records[index]); save();
                });
                await until(() => peer.records.length === index + 1 && peer.records[index].peerEnded, 2000, role + ' source FIN before teardown');
                const actual = peer.records[index]; row[role].source = structuredClone(actual); save();
                if (role === 'native') assert.deepEqual(actual, row.nativeSourceBeforeFinish, 'helper teardown supplied no source completion');
                assert.equal(actual.replied, true); assert.equal(actual.reset, false);
                assert.equal(Buffer.from(actual.rawRequest, 'base64').toString().split('\r\n')[0], peer.expected, role + ' preserves every request-target byte');
                assert.deepEqual(row[role].state, {status: 200, body: token}, role + ' actual challenge response');
            }
            assert.deepEqual(row.native.state, row.reference.state, 'same program agrees with locked workerd'); row.complete = true; save();
        } finally {row.sourceBeforeFixtureCleanup = structuredClone(peer.records); save(); await peer.close();}
    }
    assert.equal(report.cases.length, plans.length * report.shapes.length);
    assert.deepEqual(Object.fromEntries(measured.map(file => [file, hash(file)])), inputs, 'all measured inputs unchanged');
} catch (error) {failure = error; report.error = String(error.stack ?? error);}
finally {
    if (backend) {report.referenceShutdown = await stopReference(backend);
        if (report.referenceShutdown.forced || !(report.referenceShutdown.code === 0 && report.referenceShutdown.signal === null ||
            report.referenceShutdown.code === null && report.referenceShutdown.signal === 'SIGTERM'))
            failure ??= new Error('reference shutdown must finish normally');}
    report.complete = !failure; save();
}
if (failure) throw failure;
console.log(JSON.stringify({complete: true, cases: report.cases.length, roles: 2, noWorkerTimers: true, report: destination}));
