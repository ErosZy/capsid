import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import net from 'node:net';
import os from 'node:os';
import {assertNativeBranchGraphCase, branchGraphKey, branchGraphPlansForGroup, loadBranchGraphPrimaries, nativeBranchGraphFiles, nativeBranchGraphPrograms} from '../tools/native-branch-graph-contract.mjs';
import {spawn} from 'node:child_process';
import {createHash} from 'node:crypto';
import {fileURLToPath} from 'node:url';
import {setTimeout as delay} from 'node:timers/promises';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const hash = file => createHash('sha256').update(fs.readFileSync(file)).digest('hex');
const digest = value => createHash('sha256').update(value).digest('hex');
const args = new Map();
for (let i = 2; i < process.argv.length; i += 2) {
    assert.ok(['--helper', '--worker', '--group'].includes(process.argv[i]) && process.argv[i + 1] && !args.has(process.argv[i]));
    args.set(process.argv[i], process.argv[i + 1]);
}
assert.equal(args.size, 3);
const helper = args.get('--helper'), worker = args.get('--worker'), group = args.get('--group');
const plans = branchGraphPlansForGroup(group), primaries = loadBranchGraphPrimaries(root);
const inputNames = [...Object.values(nativeBranchGraphFiles), ...Object.values(nativeBranchGraphPrograms),
    'tests/test_native_branch_graph.mjs', 'tools/native-branch-graph-contract.mjs',
    'tools/native-reader-context-contract.mjs', 'tests/test_native_reader_context.cc',
    'tests/graceful_worker_exit.h', 'src/worker_exit_status.h'];
const inputHashes = () => Object.fromEntries(inputNames.map(name => [name, hash(path.join(root, name))]));
const inputs = inputHashes(), binaries = {helper: hash(helper), worker: hash(worker)};
let reportFile;
if (process.env.CAPSID_WS_REPORT_DIR) {
    fs.mkdirSync(process.env.CAPSID_WS_REPORT_DIR, {recursive: true});
    reportFile = path.join(process.env.CAPSID_WS_REPORT_DIR, 'native-branch-graph-' + group + '.json');
    assert.equal(fs.existsSync(reportFile), false);
}
const report = {schema: 'capsid-native-branch-graph-v1', complete: false, status: 'running',
    runId: process.env.CAPSID_WS_RUN_ID ?? 'standalone-native-branch-graph',
    platform: process.platform, group, inputs, binaries, exactReferenceProfile: primaries.lock.profile,
    nativeEventDeadlineMilliseconds: 5000, nativeNaturalExitDeadlineMilliseconds: 2500, cases: []};
const save = () => {if (reportFile) fs.writeFileSync(reportFile, JSON.stringify(report, null, 2) + '\n');}; save();
async function within(promise, ms, label) {
    let timer;
    try {return await Promise.race([promise, new Promise((_, reject) => {timer = setTimeout(() => reject(new Error(label)), ms);})]);}
    finally {clearTimeout(timer);}
}
async function listen(server, address, port) {
    await new Promise((resolve, reject) => {server.once('error', reject); server.listen({host: address, port, ipv6Only: address === '::1'}, resolve);});
}
async function packet(bodyKind) {
    const value = primaries.lock.packets[bodyKind];
    const prefix = Buffer.from(value.prefix, 'base64'), suffix = Buffer.from(value.suffix, 'base64');
    return {decoded: Buffer.from(Array.from({length: 17}, (_, i) => i + 1)), prefix, suffix, whole: Buffer.concat([prefix, suffix])};
}
function startChild(binary, args, row, name) {
    row.command = [binary, ...args]; row.stdout = ''; row.stderr = '';
    const child = spawn(binary, args, {stdio: ['pipe', 'pipe', 'pipe']}); child.stdin.on('error', () => {});
    child.stdout.on('data', value => {row.stdout += value;}); child.stderr.on('data', value => {row.stderr += value;});
    const closed = new Promise((resolve, reject) => {child.once('close', (code, signal) => resolve({code, signal})); child.once('error', reject);});
    child.on('error', error => {row.spawnError = String(error);});
    return {child, closed, name};
}
async function nativePair(row, target, copied, releaseSuffix) {
    const {child, closed} = startChild(helper, [worker, copied, target, 'observe'], row, 'native');
    let released = false, finished = false;
    const terminal = () => {if (child.exitCode !== null || child.signalCode !== null) throw new Error('native helper exited early: ' + row.stderr);};
    try {
        const end = performance.now() + 15000;
        while (true) {
            const line = row.stdout.split(/\r?\n/).find(value => value.startsWith('CONTEXT_GUEST '));
            if (line && row.stdout.split(/\r?\n/).includes('CONTEXT_OWNER_READ_READY true')) {row.guest = JSON.parse(line.slice(14)); break;}
            terminal(); assert.ok(performance.now() < end, 'native original5s event helper must reach graph barriers'); await delay(2);
        }
        await releaseSuffix(row); child.stdin.write('continue\n'); released = true;
        const ownerEnd = performance.now() + 5000;
        while (true) {
            const line = row.stdout.split(/\r?\n/).find(value => value.startsWith('CONTEXT_OWNER '));
            if (line) {row.owner = JSON.parse(line.slice(14)); break;}
            terminal(); assert.ok(performance.now() < ownerEnd, 'original5s owner completion budget'); await delay(2);
        }
        await sourceTerminal(row); child.stdin.end('finish\n'); finished = true;
        row.exit = await within(closed, 5000, 'helper process completion'); assert.deepEqual(row.exit, {code: 0, signal: null});
        const jsonLine = prefix => {const lines = row.stdout.split(/\r?\n/).filter(line => line.startsWith(prefix)); assert.equal(lines.length, 1); return JSON.parse(lines[0].slice(prefix.length));};
        row.protocol = jsonLine('CONTEXT_PROTOCOL '); row.workerExit = jsonLine('CONTEXT_EXIT ');
        assert.deepEqual(row.workerExit, {clean: true, processObserved: true, exitStatus: 0, forced: false, errorEvents: 0});
        assert.equal(row.protocol.requests, 3); assert.deepEqual(row.protocol.replies.map(value => value.id), [1, 2, 3]);
        for (const value of row.protocol.replies) {assert.equal(value.heads, 1); assert.equal(value.ends, 1); assert.equal(value.bodyBytes, value.returnedCredit);}
        assert.ok(!/ERROR: AddressSanitizer|LeakSanitizer:|runtime error:|AddressSanitizer:DEADLYSIGNAL/.test(row.stderr));
        row.healthyReuse = true;
    } finally {
        if (!finished && child.exitCode === null && child.signalCode === null) {
            row.helperForced = true; child.kill();
        }
        row.exit ??= await within(closed, 5000, 'native diagnostic stop');
        row.suffixReleased = released;
    }
}
async function sourceTerminal(row) {
    const end = performance.now() + 3000;
    while (!row.sources.every(value => value.peerEnded || value.peerReset) && performance.now() < end) await delay(5);
    row.sourceTerminalBeforeRuntimeStop = row.sources.every(value => value.peerEnded || value.peerReset);
    row.sourceBeforeRuntimeStop = structuredClone(row.sources);
    assert.equal(row.sourceTerminalBeforeRuntimeStop, true);
}
try {
    for (const {category, bodyKind, graph} of plans) {
        const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'capsid-branch-graph-'));
        const program = path.join(root, nativeBranchGraphPrograms[category]), programHash = hash(program);
        const copied = path.join(directory, 'main.mjs'); fs.copyFileSync(program, copied); assert.equal(hash(copied), programHash);
        const encoded = await packet(bodyKind), sockets = new Map(); let currentRow;
        const row = {category, bodyKind, graph, programSHA256: programHash, sourceDecodedBytes: [...encoded.decoded], sourcePacketSHA256: digest(encoded.whole),
            sourcePrefixSHA256: digest(encoded.prefix), sourcePrefixEncodedLength: encoded.prefix.length,
            sourceSuffixEncodedLength: encoded.suffix.length, native: {sources: []}};
        report.cases.push(row);
        const accept = socket => {
            const source = {rawRequest: null, peerEnded: false, peerReset: false, fixtureEnded: false, fixtureDestroyed: false, events: []};
            currentRow.sources.push(source); currentRow.upstream = socket; sockets.set(socket, source);
            let raw = Buffer.alloc(0);
            socket.on('end', () => {source.peerEnded = true; source.events.push('peer-ended');});
            socket.on('error', error => {source.error = error.code; source.peerReset = error.code === 'ECONNRESET';});
            socket.on('close', () => sockets.delete(socket));
            socket.on('data', value => {
                raw = Buffer.concat([raw, value]); if (source.rawRequest !== null || !raw.includes('\r\n\r\n')) return;
                source.rawRequest = raw.toString('base64'); source.events.push('request');
                const headers = Buffer.from('HTTP/1.1 201 Graph\r\nContent-Length: ' + encoded.whole.length + '\r\n' +
                    (bodyKind === 'gzip' ? 'Content-Encoding: gzip\r\n' : '') + 'X-Graph: held\r\nConnection: close\r\n\r\n');
                source.headers = headers.toString('base64'); source.prefix = encoded.prefix.toString('base64');
                source.events.push('headers-and-prefix-sent'); socket.write(Buffer.concat([headers, encoded.prefix]));
            });
        };
        const ipv4 = net.createServer({allowHalfOpen: true}, accept); await listen(ipv4, '127.0.0.1', 0);
        const upstreamPort = ipv4.address().port;
        const ipv6 = net.createServer({allowHalfOpen: true}, accept); await listen(ipv6, '::1', upstreamPort);
        const target = `http://localhost:${upstreamPort}/${bodyKind}-body?readerRoute=${graph}`; row.targetUrl = target;
        const releaseSuffix = async value => {
            assert.equal(value.sources.length, 1); assert.ok(value.upstream);
            value.sourceBeforeGuestBodyRelease = structuredClone(value.sources);
            const source = value.sources[0];
            assert.equal(source.fixtureEnded, false); assert.equal(source.fixtureDestroyed, false);
            assert.equal(source.peerEnded, false); assert.equal(source.peerReset, false);
            source.fixtureEnded = true; source.suffix = encoded.suffix.toString('base64'); source.events.push('suffix-released');
            value.upstream.end(encoded.suffix);
        };
        try {
            currentRow = row.native;
            try {await nativePair(currentRow, target, copied, releaseSuffix);}
            catch (error) {currentRow.collectionFailure = String(error.stack ?? error);}
            finally {delete currentRow.upstream;}
        } finally {
            for (const [socket, source] of sockets) {source.fixtureDestroyed = true; socket.destroy();}
            await Promise.all([ipv4, ipv6].map(server => new Promise(resolve => server.close(resolve))));
            assert.equal(hash(copied), programHash); fs.unlinkSync(copied); fs.rmdirSync(directory);
        }
        save();
        assertNativeBranchGraphCase(row, primaries);
        console.log(JSON.stringify({key: branchGraphKey(row), exactPublicState: true, healthyReuse: true, naturalExit: true}));
    }
    assert.deepEqual(inputHashes(), inputs);
    assert.deepEqual({helper: hash(helper), worker: hash(worker)}, binaries);
    assert.equal(report.cases.length, plans.length);
    report.complete = true; report.status = 'complete'; save();
    console.log(JSON.stringify({complete: true, group, cases: report.cases.length,
        exactPinnedPublicReports: true, actualHeldSourceCreditsAndNaturalExit: true}));
} catch (error) {report.complete = true; report.status = 'runner-error'; report.error = error.stack; save(); throw error;}
