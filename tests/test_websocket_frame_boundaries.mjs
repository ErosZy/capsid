import assert from 'node:assert/strict';
import {createHash, randomBytes, randomUUID} from 'node:crypto';
import fs from 'node:fs';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';
import {spawn} from 'node:child_process';
import {setTimeout as delay} from 'node:timers/promises';
import {startCapsid, stopCapsid, withCleanup} from './helpers/ws_host_backend.mjs';
import {root, readLock, verifyBinary, verifyClient, fileDigest} from '../tools/workerd-reference.mjs';
import {frameInputPaths, frameSizes as sizes, frameConnections, validateFrameReport} from '../tools/workerd-frame-contract.mjs';

const args = new Map();
for (let i = 2; i < process.argv.length; i += 2) args.set(process.argv[i], process.argv[i + 1]);
const reference = args.get('--workerd') ?? (args.has('--reference-from-env') ? process.env.CAPSID_WORKERD_REF : undefined);
if (args.has('--reference-from-env') && !reference) throw new Error('CAPSID_WORKERD_REF is required for raw-frame acceptance');
const host = args.get('--host'), worker = args.get('--worker'), mode = reference ? 'reference' : args.get('--mode');
assert.ok(reference || host && worker && ['single-worker', 'static-pool', 'managed'].includes(mode));
const runId = process.env.CAPSID_WS_RUN_ID ?? randomUUID();
const destination = args.get('--report') ?? (process.env.CAPSID_WS_REPORT_DIR ?
    path.join(process.env.CAPSID_WS_REPORT_DIR, `frame-boundaries-${mode}.json`) : undefined);
if (destination) fs.writeFileSync(destination, JSON.stringify({complete: false, status: 'running', mode, runId}) + '\n');
const lock = readLock();
const inputs = Object.fromEntries(Object.entries(frameInputPaths).map(([key, file]) => [key, fileDigest(path.join(root, file))]));
const digest = value => createHash('sha256').update(value).digest('hex');
const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'capsid-ws-frame-boundaries-'));
const sockets = new Set(), observations = [], closes = [];
let backend, report;

async function within(operation, stage) {
    let timer;
    try {
        return await Promise.race([operation, new Promise((_, reject) => {
            timer = setTimeout(() => reject(new Error(`${stage} deadline`)), 8000);
        })]);
    } finally { clearTimeout(timer); }
}

// This reader and frame encoder do not use the product codec or a WebSocket
// SDK. Every client send is one FIN frame, even when TCP writes are split.
function reader(socket) {
    let bytes = Buffer.alloc(0), wake, ended = false, failure;
    const notify = () => { const previous = wake; wake = undefined; previous?.(); };
    socket.on('data', chunk => {
        bytes = Buffer.concat([bytes, chunk]);
        if (bytes.length > 2 * 1024 * 1024) failure = new Error('bounded raw reader exceeded');
        notify();
    });
    socket.once('error', error => { failure = error; notify(); });
    const closed = new Promise(resolve => socket.once('close', () => { ended = true; notify(); resolve(); }));
    const changed = () => new Promise(resolve => { wake = resolve; });
    const available = async predicate => {
        while (!predicate()) {
            if (failure) throw failure;
            if (ended) throw new Error('transport ended before the expected frame');
            await within(changed(), 'actual TCP data');
        }
        if (failure) throw failure;
    };
    const take = async length => {
        await available(() => bytes.length >= length);
        const result = bytes.subarray(0, length); bytes = bytes.subarray(length); return result;
    };
    return {
        async upgrade() {
            await available(() => bytes.includes('\r\n\r\n'));
            const end = bytes.indexOf('\r\n\r\n');
            assert.ok(end <= 8192, 'bounded Upgrade response headers');
            return (await take(end + 4)).toString();
        },
        async frame() {
            const head = await take(2);
            assert.equal(head[0] & 0x70, 0, 'no unrequested extension bits');
            assert.equal(head[0] & 0x80, 0x80, 'each echo is one final frame');
            assert.equal(head[1] & 0x80, 0, 'server frames are unmasked');
            const tag = head[1] & 127;
            let size = tag;
            if (tag === 126) size = (await take(2)).readUInt16BE();
            if (tag === 127) {
                const wide = (await take(8)).readBigUInt64BE();
                assert.ok(wide <= 1024n * 1024n, 'bounded 64-bit frame length'); size = Number(wide);
            }
            assert.equal(tag, size < 126 ? size : size <= 65535 ? 126 : 127, 'canonical length encoding');
            return {opcode: head[0] & 15, tag, fin: Boolean(head[0] & 128), masked: Boolean(head[1] & 128),
                rsv: head[0] & 0x70, payload: await take(size)};
        },
        async terminal() {
            await within(closed, 'TCP close after both Close frames');
            assert.ifError(failure); assert.equal(bytes.length, 0, 'no additional frame after Close');
        },
    };
}

function maskedFrame(opcode, payload) {
    const size = payload.length;
    const prefix = size < 126 ? 2 : size <= 65535 ? 4 : 10;
    const frame = Buffer.alloc(prefix + 4 + size), mask = randomBytes(4);
    frame[0] = 0x80 | opcode;
    frame[1] = 0x80 | (size < 126 ? size : size <= 65535 ? 126 : 127);
    if (prefix === 4) frame.writeUInt16BE(size, 2);
    if (prefix === 10) frame.writeBigUInt64BE(BigInt(size), 2);
    mask.copy(frame, prefix);
    for (let i = 0; i < size; i++) frame[prefix + 4 + i] = payload[i] ^ mask[i % 4];
    return frame;
}
async function write(socket, bytes) {
    await within(new Promise((resolve, reject) => socket.write(bytes, error => error ? reject(error) : resolve())), 'TCP write');
}
function payload(opcode, size, connection) {
    if (!size) return Buffer.alloc(0);
    if (opcode === 1) {
        const text = size >= 4 ? 'a'.repeat(size - 4) + '😀' : 'a'.repeat(size);
        const result = Buffer.from(text);
        if (size >= 64) Buffer.from(`${runId}:${connection}:`).copy(result, 0);
        assert.equal(result.length, size); return result;
    }
    const result = Buffer.from(Array.from({length: size}, (_, i) => (i * 17 + connection + 29) % 251));
    if (size >= 64) Buffer.from(`${runId}:${connection}:`).copy(result, 0);
    return result;
}
async function connection(index) {
    const socket = net.connect(backend.port, '127.0.0.1'); sockets.add(socket);
    socket.once('close', () => sockets.delete(socket));
    const wire = reader(socket), key = randomBytes(16).toString('base64');
    await write(socket, Buffer.from(`GET /@capsid/orders/ws HTTP/1.1\r\nHost: public.example\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: ${key}\r\n\r\n`));
    const response = await wire.upgrade();
    assert.match(response, /^HTTP\/1\.1 101 Switching Protocols\r\n/);
    const expectedAccept = createHash('sha1').update(key + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').digest('base64');
    const accepts = response.split('\r\n').filter(line => /^sec-websocket-accept:/i.test(line));
    assert.equal(accepts.length, 1, 'one actual Upgrade accept header');
    assert.equal(accepts[0].slice(accepts[0].indexOf(':') + 1).trim(), expectedAccept);
    for (const opcode of [1, 2]) for (const size of sizes) {
        const expected = payload(opcode, size, index), frame = maskedFrame(opcode, expected);
        // Split the header and its extended length across writes. The frame
        // itself remains unfragmented; no implicit SDK fragmentation is used.
        await write(socket, frame.subarray(0, 1));
        await write(socket, frame.subarray(1, 3));
        await write(socket, frame.subarray(3, 9));
        await write(socket, frame.subarray(9));
        const actual = await wire.frame();
        assert.equal(actual.opcode, opcode, `${mode}/${index}: ${size}-byte text/binary type preserved`);
        assert.deepEqual(actual.payload, expected, 'exact whole message in exactly one frame');
        assert.equal(actual.tag, size < 126 ? size : size <= 65535 ? 126 : 127);
        observations.push({connection: index, opcode, size, lengthTag: actual.tag, fin: actual.fin,
            masked: actual.masked, rsv: actual.rsv, sha256: digest(actual.payload)});
    }
    await write(socket, maskedFrame(8, Buffer.from([3, 232])));
    const close = await wire.frame();
    assert.equal(close.opcode, 8); assert.deepEqual(close.payload, Buffer.from([3, 232]));
    await wire.terminal();
    closes.push({connection: index, opcode: close.opcode, fin: close.fin, masked: close.masked, rsv: close.rsv,
        lengthTag: close.tag, payloadHex: close.payload.toString('hex'), tcpClosed: true, extraBytes: 0});
}

async function startReference(bundle) {
    const reserve = net.createServer();
    await new Promise((resolve, reject) => { reserve.once('error', reject); reserve.listen(0, '127.0.0.1', resolve); });
    const port = reserve.address().port;
    await new Promise(resolve => reserve.close(resolve));
    fs.copyFileSync(bundle, path.join(directory, 'main.mjs'));
    const config = path.join(directory, 'reference.capnp');
    fs.writeFileSync(config, `using Workerd = import "/workerd/workerd.capnp";
const config :Workerd.Config = (services=[(name="main",worker=(
compatibilityDate=${JSON.stringify(lock.compatibility_date)},compatibilityFlags=${JSON.stringify(lock.compatibility_flags)},
modules=[(name="main",esModule=embed "main.mjs")]))],
sockets=[(name="http",address="127.0.0.1:${port}",http=(),service="main")]);`);
    const child = spawn(reference, ['serve', config], {stdio: ['ignore', 'pipe', 'pipe']});
    let stderr = '', spawnError;
    child.stdout.resume(); child.stderr.setEncoding('utf8'); child.stderr.on('data', chunk => { stderr += chunk; });
    child.once('error', error => { spawnError = error; });
    try {
        const deadline = performance.now() + 10000;
        for (;;) {
            assert.ifError(spawnError);
            assert.ok(performance.now() < deadline && child.exitCode === null && child.signalCode === null, stderr);
            try {
                const response = await fetch(`http://127.0.0.1:${port}/healthy`, {signal: AbortSignal.timeout(1000)});
                if (response.status === 200 && await response.text() === 'frame-boundary-healthy') break;
            } catch {}
            await delay(10);
        }
        return {child, port, stderr: () => stderr};
    } catch (error) { await stopCapsid(child); throw error; }
}

try {
await withCleanup(async () => {
    verifyClient(lock); if (reference) verifyBinary(reference, lock);
    const bundle = path.join(root, frameInputPaths.fixture);
    backend = reference ? await startReference(bundle) : await startCapsid(host, worker, bundle, directory, mode,
        {workers: mode === 'static-pool' ? 4 : 1, permissions: {modules: []}});
    const count = frameConnections;
    for (let i = 0; i < count; i++) await connection(i);
    const healthy = await fetch(`http://127.0.0.1:${backend.port}/@capsid/orders/health`, {signal: AbortSignal.timeout(5000)});
    const health = {status: healthy.status, text: await healthy.text()};
    assert.deepEqual(health, {status: 200, text: 'frame-boundary-healthy'});
    assert.equal(observations.length, count * 2 * sizes.length);
    report = {schema: 'capsid-websocket-frame-boundaries-v2', mode, runId, referenceLock: lock, inputs,
        client: {node: process.version, undici: process.versions.undici}, connections: count, observations, closes,
        binaries: {host: fileDigest(reference ?? host), worker: reference ? null : fileDigest(worker)}, healthy: health};
}, async () => {
    await withCleanup(async () => {
        if (backend) {
            const terminal = await stopCapsid(backend.child);
            if (reference) {
                assert.equal(terminal.forced, false);
                assert.ok(terminal.code === 0 && terminal.signal === null ||
                    terminal.code === null && terminal.signal === 'SIGTERM', JSON.stringify(terminal));
            } else assert.deepEqual(terminal, {code: 0, signal: null, forced: false}, backend.stderr());
            assert.doesNotMatch(backend.stderr(), /ERROR: (?:AddressSanitizer|LeakSanitizer)|runtime error:/);
            if (report) { report.shutdown = terminal; report.backendStderr = backend.stderr(); }
        }
    }, async () => {
        for (const socket of sockets) socket.destroy();
        assert.equal(path.dirname(directory), path.resolve(os.tmpdir()));
        assert.ok(path.basename(directory).startsWith('capsid-ws-frame-boundaries-'));
        fs.rmSync(directory, {recursive: true, force: true});
    });
});
report.complete = true;
validateFrameReport(report, {runId, lock, inputs, host: report.binaries.host, worker: report.binaries.worker});
console.log(JSON.stringify(report));
if (destination) fs.writeFileSync(destination, JSON.stringify(report, null, 2) + '\n');
} catch (error) {
    if (destination) fs.writeFileSync(destination, JSON.stringify({complete: false, status: 'failed', mode, runId, error: error.message}) + '\n');
    throw error;
}
