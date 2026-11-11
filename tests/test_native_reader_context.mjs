import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import net from 'node:net';
import {spawn} from 'node:child_process';
import {fileURLToPath} from 'node:url';
import {createHash} from 'node:crypto';
import os from 'node:os';
import {assertNativeReaderContextCase, loadNativeReaderContextPrimaries, nativeReaderContextFiles, nativeReaderContextOperations} from '../tools/native-reader-context-contract.mjs';
import {setTimeout as delay} from 'node:timers/promises';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const hash = file => createHash('sha256').update(fs.readFileSync(file)).digest('hex');
const args = new Map();
for (let index = 2; index < process.argv.length; index += 2) {
    const key = process.argv[index], value = process.argv[index + 1];
    assert.ok(['--helper', '--worker', '--shard'].includes(key) && value && !args.has(key));
    args.set(key, value);
}
assert.equal(args.size, 3);
const helper = args.get('--helper'), binary = args.get('--worker'), shard = args.get('--shard');
assert.ok(['released', 'retained'].includes(shard));
const primaries = loadNativeReaderContextPrimaries(root);
const program = path.join(root, nativeReaderContextFiles.fixture);
const inputNames = [...Object.values(nativeReaderContextFiles), 'tests/test_native_reader_context.cc',
    'tests/test_native_reader_context.mjs', 'tools/native-reader-context-contract.mjs',
    'tests/graceful_worker_exit.h', 'src/worker_exit_status.h'];
const inputHashes = () => Object.fromEntries(inputNames.map(name => [name, hash(path.join(root, name))]));
const inputs = inputHashes();
const binaries = {helper: hash(helper), worker: hash(binary)};
let reportFile;
if (process.env.CAPSID_WS_REPORT_DIR) {
    fs.mkdirSync(process.env.CAPSID_WS_REPORT_DIR, {recursive: true});
    reportFile = path.join(process.env.CAPSID_WS_REPORT_DIR, 'native-reader-context-' + shard + '.json');
    assert.equal(fs.existsSync(reportFile), false);
}
const report = {schema: 'capsid-native-reader-context-v1', complete: false,
    runId: process.env.CAPSID_WS_RUN_ID ?? 'standalone-native-reader-context',
    platform: process.platform, shard, inputs, binaries,
    nativeEventDeadlineMilliseconds: 5000, nativeNaturalExitDeadlineMilliseconds: 2500,
    exactLockedReferenceProfile: primaries.lock.profile, cases: []};
const save = () => {if (reportFile) fs.writeFileSync(reportFile, JSON.stringify(report, null, 2) + '\n');};
save();
const operations = nativeReaderContextOperations.filter(value => value.includes('&retain=1') === (shard === 'retained'));
assert.equal(operations.length, 8);
async function listen(server, address, port) {
    await new Promise((resolve, reject) => {server.once('error', reject); server.listen({host: address, port, ipv6Only: address === '::1'}, resolve);});
}
async function within(promise, milliseconds, message) {
    let timer;
    try {return await Promise.race([promise, new Promise((_, reject) => {timer = setTimeout(() => reject(new Error(message)), milliseconds);})]);}
    finally {clearTimeout(timer);}
}
try {
    for (const operation of operations) {
        const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'capsid-reader-context-'));
        const row = {operation, sources: [], nativeStdout: '', nativeStderr: ''}; report.cases.push(row);
        const sockets = new Map(); let upstream;
        const accept = socket => {
            const source = {rawRequest: null, peerEnded: false, peerReset: false, fixtureEnded: false, fixtureDestroyed: false, events: []};
            row.sources.push(source); sockets.set(socket, source);
            let raw = Buffer.alloc(0);
            socket.on('end', () => {source.peerEnded = true; source.events.push('peer-ended');});
            socket.on('error', error => {source.peerReset = error.code === 'ECONNRESET'; source.error = error.code;});
            socket.on('close', () => sockets.delete(socket));
            socket.on('data', chunk => {
                raw = Buffer.concat([raw, chunk]);
                if (source.rawRequest !== null || !raw.includes('\r\n\r\n')) return;
                assert.equal(upstream, undefined, 'one exact upstream request');
                upstream = socket; source.rawRequest = raw.toString('base64'); source.events.push('request');
                const headers = Buffer.from('HTTP/1.1 201 Context\r\nContent-Length: 17\r\nContent-Type: application/octet-stream\r\nConnection: close\r\n\r\n');
                source.headers = headers.toString('base64'); socket.write(headers); source.events.push('headers-sent');
            });
        };
        const ipv4 = net.createServer({allowHalfOpen: true}, accept); await listen(ipv4, '127.0.0.1', 0);
        const upstreamPort = ipv4.address().port;
        const ipv6 = net.createServer({allowHalfOpen: true}, accept); await listen(ipv6, '::1', upstreamPort);
        const copied = path.join(directory, 'main.mjs'); fs.copyFileSync(program, copied); assert.equal(hash(copied), inputs[nativeReaderContextFiles.fixture]);
        const targetUrl = `http://localhost:${upstreamPort}/body`;
        row.nativeCommand = [helper, binary, copied, targetUrl, operation];
        const child = spawn(helper, row.nativeCommand.slice(1), {stdio: ['pipe', 'pipe', 'pipe']});
        child.stdin.on('error', () => {});
        let releaseGuest, releaseOwner;
        const guestReady = new Promise(resolve => {releaseGuest = resolve;});
        const ownerReady = new Promise(resolve => {releaseOwner = resolve;});
        const inspect = () => {
            const lines = row.nativeStdout.split(/\r?\n/);
            const guest = lines.find(line => line.startsWith('CONTEXT_GUEST '));
            const gate = lines.includes('CONTEXT_OWNER_READ_READY true');
            if (guest && gate) {
                try {row.guest = JSON.parse(guest.slice(14)); row.ownerEnteredReadBeforeSourceRelease = true; releaseGuest();} catch {}
            }
            const owner = lines.find(line => line.startsWith('CONTEXT_OWNER '));
            if (owner) try {row.owner = JSON.parse(owner.slice(14)); releaseOwner();} catch {}
        };
        child.stdout.on('data', chunk => {row.nativeStdout += chunk; inspect();});
        child.stderr.on('data', chunk => {row.nativeStderr += chunk;});
        const closed = new Promise((resolve, reject) => {child.once('close', (code, signal) => resolve({code, signal})); child.once('error', reject);});
        try {
            await within(Promise.race([guestReady, closed.then(() => {throw new Error('native helper exited before guest/barrier: ' + row.nativeStderr);})]), 15000, 'native pair entry deadline');
            row.ownerReadyBeforeGuest = true;
            row.sourceBeforeGuestBodyRelease = structuredClone(row.sources);
            assert.equal(row.sources.length, 1); assert.ok(upstream);
            const payload = Buffer.from(Array.from({length: 17}, (_, i) => i + 1));
            row.sources[0].fixtureEnded = true; row.sources[0].payload = payload.toString('base64'); row.sources[0].events.push('body-released');
            upstream.end(payload); child.stdin.write('continue\n');
            await within(Promise.race([ownerReady, closed.then(() => {throw new Error('native helper exited before owner result: ' + row.nativeStderr);})]), 5000, 'native owner response deadline');
            const end = performance.now() + 3000;
            while (!row.sources.every(source => source.peerEnded || source.peerReset) && performance.now() < end) await delay(5);
            row.sourceTerminalBeforeNativeStop = row.sources.every(source => source.peerEnded || source.peerReset);
            row.sourceBeforeNativeStop = structuredClone(row.sources);
            assert.equal(row.sourceTerminalBeforeNativeStop, true, 'actual source terminal before native cleanup');
            child.stdin.end('finish\n'); row.nativeExit = await within(closed, 5000, 'native cleanup deadline');
            assert.deepEqual(row.nativeExit, {code: 0, signal: null});
            const lines = row.nativeStdout.split(/\r?\n/);
            const protocols = lines.filter(line => line.startsWith('CONTEXT_PROTOCOL '));
            const exits = lines.filter(line => line.startsWith('CONTEXT_EXIT '));
            assert.equal(protocols.length, 1); assert.equal(exits.length, 1);
            row.protocol = JSON.parse(protocols[0].slice(17)); row.workerExit = JSON.parse(exits[0].slice(13));
            assert.equal(row.protocol.requests, 3); assert.deepEqual(row.protocol.replies.map(reply => reply.id), [1, 2, 3]);
            for (const reply of row.protocol.replies) {assert.equal(reply.heads, 1); assert.equal(reply.ends, 1); assert.equal(reply.bodyBytes, reply.returnedCredit);}
            assert.deepEqual(row.workerExit, {clean: true, processObserved: true, exitStatus: 0, forced: false, errorEvents: 0});
            row.healthyReuse = true;
        } catch (error) {row.collectionFailure = String(error.stack ?? error);}
        finally {
            if (child.exitCode === null && child.signalCode === null) {row.helperForced = true; child.kill();}
            row.nativeExit ??= await within(closed, 5000, 'native helper stop deadline');
            for (const [socket, source] of sockets) {source.fixtureDestroyed = true; socket.destroy();}
            await Promise.all([ipv4, ipv6].map(server => new Promise(resolve => server.close(resolve))));
            assert.equal(hash(copied), inputs[nativeReaderContextFiles.fixture]);
            fs.unlinkSync(copied); fs.rmdirSync(directory); save();
        }
        assertNativeReaderContextCase(row, primaries);
        console.log(JSON.stringify({operation, exactPublicState: true, healthyReuse: true, cleanNaturalExit: true}));
    }
    assert.deepEqual(inputHashes(), inputs);
    assert.deepEqual({helper: hash(helper), worker: hash(binary)}, binaries);
    report.complete = true; report.status = 'complete'; save();
    console.log(JSON.stringify({complete: true, shard, cases: report.cases.length,
        sameWorkerTwoRequestOverlap: true, actualHeldUpstreamBeforeGuest: true,
        exactPublicStateAndNaturalExit: true}));
} catch (error) {
    report.complete = true; report.status = 'runner-error'; report.error = error.stack; save(); throw error;
}
