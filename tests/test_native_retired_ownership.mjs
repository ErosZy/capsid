import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import net from 'node:net';
import {createGzip, constants, gunzipSync} from 'node:zlib';
import {spawn} from 'node:child_process';
import {createHash} from 'node:crypto';
import {isDeepStrictEqual, parseArgs} from 'node:util';
import os from 'node:os';
import {plansForGroup, validateRetiredCase, validateRetiredReport} from '../tools/native-retired-ownership-contract.mjs';
import {fileURLToPath} from 'node:url';
import {setTimeout as delay} from 'node:timers/promises';
import {referenceArchitecture, referenceLockName, referencePlatform} from '../tools/workerd-platform.mjs';
import {waitForWorkerdListen} from './helpers/workerd_listener_control.mjs';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const {values: args} = parseArgs({options: {worker: {type: 'string'}, helper: {type: 'string'},
    reference: {type: 'string'}, group: {type: 'string'}}});
assert.ok(args.worker && args.helper && args.group, '--worker, --helper and --group are required');
const group = args.group, plans = plansForGroup(group), windows = process.platform === 'win32';
const hash = file => createHash('sha256').update(fs.readFileSync(file)).digest('hex');
const digest = value => createHash('sha256').update(value).digest('hex');
const read = file => JSON.parse(fs.readFileSync(file));
const kind = group === 'branches' ? 'branches' : group === 'controls' ? 'controls' : group.startsWith('iterator_') ? 'iterators' : 'promises';
const program = path.join(root, 'tests/fixtures/worker-native-retired-' + kind + '.mjs');
const programHash = hash(program), worker = path.resolve(args.worker), helper = path.resolve(args.helper);
const reference = path.resolve(args.reference ?? process.env.CAPSID_WORKERD_REF ??
    (windows ? path.join(root, 'build-v030-native/reference-windows-v1.20261010.1/workerd.exe') : path.join(root, 'build-v030-native/reference/workerd')));
const referenceLockFile = path.join(root, 'tests/fixtures/workerd-ws', referenceLockName(process.platform, process.arch));
const referenceLock = read(referenceLockFile);
assert.equal(hash(reference), referenceLock.binary_sha256);
assert.equal(referencePlatform(referenceLock), process.platform);
assert.equal(referenceArchitecture(referenceLock), process.arch);
const staticPacketFile = path.join(root, 'tests/fixtures/workerd-native-branch-graph-primary-lock.json');
const staticPackets = read(staticPacketFile).packets;
const measuredFiles = [program, worker, helper, reference, referenceLockFile, staticPacketFile,
    fileURLToPath(import.meta.url), path.join(root, 'tools/native-retired-ownership-contract.mjs'), path.join(root, 'tools/workerd-platform.mjs'),
    path.join(root, 'tests/helpers/workerd_listener_control.mjs'), path.join(root, 'tools/workerd-fetch-byob-contract.mjs')];
const measured = Object.fromEntries(measuredFiles.map(file => [file, hash(file)]));
const output = fs.mkdtempSync(path.join(os.tmpdir(), 'capsid-native-retired-ownership-'));
const reportDir = process.env.CAPSID_WS_REPORT_DIR ?? output; fs.mkdirSync(reportDir, {recursive: true});
const reportFile = path.join(reportDir, 'native-retired-ownership-' + group + '.json');
const report = {complete: false, status: 'running', group, platform: process.platform,
    inputs: measured, artifactDirectory: output, referenceLock,
    eventDeadlineMilliseconds: 5000, naturalNativeExitDeadlineMilliseconds: 2500, cases: []};
const save = () => fs.writeFileSync(reportFile, JSON.stringify(report, null, 2) + '\n'); save();
async function within(promise, ms, label) {
    let timer;
    try {return await Promise.race([promise, new Promise((_, reject) => {timer = setTimeout(() => reject(new Error(label)), ms);})]);}
    finally {clearTimeout(timer);}
}
async function listen(server, address, port) {
    await new Promise((resolve, reject) => {server.once('error', reject); server.listen({host: address, port, ipv6Only: address === '::1'}, resolve);});
}
async function packet(bodyKind) {
    const value = staticPackets[bodyKind], prefix = Buffer.from(value.prefix, 'base64'), suffix = Buffer.from(value.suffix, 'base64');
    return {decoded: Buffer.from(Array.from({length: 17}, (_, i) => i + 1)), prefix, suffix, whole: Buffer.concat([prefix, suffix])};
}
function startChild(binary, args, row, name, control = false) {
    row.command = [binary, ...args]; row.stdout = ''; row.stderr = '';
    const child = spawn(binary, args, {stdio: control ? ['pipe', 'pipe', 'pipe', 'pipe'] : ['pipe', 'pipe', 'pipe']}); child.stdin.on('error', () => {});
    child.stdout.on('data', value => {row.stdout += value;}); child.stderr.on('data', value => {row.stderr += value;});
    const closed = new Promise((resolve, reject) => {child.once('close', (code, signal) => resolve({code, signal})); child.once('error', reject);});
    child.on('error', error => {row.spawnError = String(error);});
    return {child, closed, name};
}
async function waitForOwnerSourceRetirement(row) {
    const end = performance.now() + 3000;
    while (!row.sources.every(value => value.peerEnded || value.peerReset) && performance.now() < end) await delay(5);
    row.sourceTerminalBeforeGuestRequest = row.sources.every(value => value.peerEnded || value.peerReset);
    row.sourceBeforeGuestRequest = structuredClone(row.sources);
}
async function nativePair(row, target, copied, releaseSuffix) {
    const {child, closed} = startChild(helper, [worker, copied, target, 'observe'], row, 'native');
    let finished = false;
    const terminal = () => {if (child.exitCode !== null || child.signalCode !== null) throw new Error('retired graph helper exited early: ' + row.stderr);};
    const line = prefix => row.stdout.split(/\r?\n/).slice(0, -1).find(value => value.startsWith(prefix));
    try {
        const ownerEnd = performance.now() + 15000;
        while (!line('CONTEXT_OWNER_RETIRED true')) {
            terminal(); assert.ok(performance.now() < ownerEnd, 'original5s event helper owner terminal'); await delay(2);
        }
        row.owner = JSON.parse(line('CONTEXT_OWNER ').slice(14)); row.ownerHttpEndedBeforeGuest = true;
        await waitForOwnerSourceRetirement(row); child.stdin.write('guest\n');
        const guestEnd = performance.now() + 5000;
        while (!line('CONTEXT_GUEST ')) {
            terminal(); assert.ok(performance.now() < guestEnd, 'original5s guest completion budget'); await delay(2);
        }
        row.guest = JSON.parse(line('CONTEXT_GUEST ').slice(14));
        row.sourceAfterGuest = structuredClone(row.sources);
        if (!row.sources.every(value => value.peerEnded || value.peerReset)) {await releaseSuffix(row); row.suffixReleased = true;}
        else row.suffixReleased = false;
        await sourceTerminal(row); child.stdin.end('finish\n'); finished = true;
        row.exit = await within(closed, 5000, 'helper process completion'); assert.deepEqual(row.exit, {code: 0, signal: null});
        const jsonLine = prefix => {const lines = row.stdout.split(/\r?\n/).filter(value => value.startsWith(prefix)); assert.equal(lines.length, 1); return JSON.parse(lines[0].slice(prefix.length));};
        row.protocol = jsonLine('CONTEXT_PROTOCOL '); row.workerExit = jsonLine('CONTEXT_EXIT ');
        assert.deepEqual(row.workerExit, {clean: true, processObserved: true, exitStatus: 0, forced: false, errorEvents: 0});
        assert.equal(row.protocol.requests, 3); assert.deepEqual(row.protocol.replies.map(value => value.id), [1, 2, 3]);
        for (const value of row.protocol.replies) {assert.equal(value.heads, 1); assert.equal(value.ends, 1); assert.equal(value.bodyBytes, value.returnedCredit);}
        assert.ok(!/ERROR: AddressSanitizer|LeakSanitizer:|runtime error:|AddressSanitizer:DEADLYSIGNAL/.test(row.stderr));
        row.healthyReuse = true;
    } finally {
        if (!finished && child.exitCode === null && child.signalCode === null) {row.helperForced = true; child.kill();}
        row.exit ??= await within(closed, 5000, 'native retired graph diagnostic stop');
    }
}
async function sourceTerminal(row) {
    const end = performance.now() + 3000;
    while (!row.sources.every(value => value.peerEnded || value.peerReset) && performance.now() < end) await delay(5);
    row.sourceTerminalBeforeRuntimeStop = row.sources.every(value => value.peerEnded || value.peerReset);
    row.sourceBeforeRuntimeStop = structuredClone(row.sources);
    assert.equal(row.sourceTerminalBeforeRuntimeStop, true);
}
async function referencePair(row, target, copied, directory, releaseSuffix) {
    const config = path.join(directory, 'reference.capnp');
    fs.writeFileSync(config, `using Workerd = import "/workerd/workerd.capnp";
const config :Workerd.Config = (services=[(name="main",worker=(compatibilityDate=${JSON.stringify(referenceLock.compatibility_date)},
compatibilityFlags=${JSON.stringify(referenceLock.compatibility_flags)},modules=[(name="main",esModule=embed "main.mjs")])),
(name="internet",network=(allow=["local"]))],sockets=[(name="http",address="127.0.0.1:0",http=(),service="main")]);
`);
    row.configSHA256 = hash(config);
    const {child, closed} = startChild(reference, ['serve', config, '--control-fd=3'], row, 'reference', true);
    try {
        row.listenerControl = await waitForWorkerdListen(child, 5000);
        const {port} = row.listenerControl;
        const owner = await fetch(`http://127.0.0.1:${port}/owner?target=${encodeURIComponent(target)}`, {signal: AbortSignal.timeout(5000)});
        assert.equal(owner.status, 200); row.owner = await owner.json(); row.ownerHttpEndedBeforeGuest = true;
        await waitForOwnerSourceRetirement(row);
        const guest = await fetch(`http://127.0.0.1:${port}/guest?operation=observe`, {signal: AbortSignal.timeout(5000)});
        assert.equal(guest.status, 200); row.guest = await guest.json();
        row.sourceAfterGuest = structuredClone(row.sources);
        if (!row.sources.every(value => value.peerEnded || value.peerReset)) {await releaseSuffix(row); row.suffixReleased = true;}
        else row.suffixReleased = false;
        await sourceTerminal(row);
        const healthy = await fetch(`http://127.0.0.1:${port}/healthy`, {signal: AbortSignal.timeout(5000)});
        assert.equal(await healthy.text(), 'healthy'); row.healthyReuse = true;
    } finally {
        if (child.exitCode === null && child.signalCode === null) child.kill();
        row.referenceStop = await within(closed, 5000, 'reference retired graph diagnostic stop');
        assert.equal(hash(copied), programHash);
    }
}
try {
    for (const {bodyKind, graph} of plans) {
        const directory = path.join(output, bodyKind, graph); fs.mkdirSync(directory, {recursive: true});
        const copied = path.join(directory, 'main.mjs'); fs.copyFileSync(program, copied); assert.equal(hash(copied), programHash);
        const encoded = await packet(bodyKind), sockets = new Map(); let currentRow;
        const row = {bodyKind, graph, sourceDecodedBytes: [...encoded.decoded], sourcePacketSHA256: digest(encoded.whole),
            sourcePrefixSHA256: digest(encoded.prefix), sourcePrefixEncodedLength: encoded.prefix.length,
            sourceSuffixEncodedLength: encoded.suffix.length, reference: {sources: []}, native: {sources: []}};
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
                if (group === 'controls') {
                    source.events.push('headers-sent'); socket.write(headers);
                    setTimeout(() => {if (!socket.destroyed) {source.events.push('delayed-prefix-sent'); socket.write(encoded.prefix);}}, 100);
                } else {source.events.push('headers-and-prefix-sent'); socket.write(Buffer.concat([headers, encoded.prefix]));}
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
            for (const name of ['reference', 'native']) {
                currentRow = row[name];
                try {
                    if (name === 'reference') await referencePair(currentRow, target, copied, directory, releaseSuffix);
                    else await nativePair(currentRow, target, copied, releaseSuffix);
                } catch (error) {currentRow.collectionFailure = String(error.stack ?? error);}
                finally {delete currentRow.upstream;}
                // Reference and native runtimes use the same still-listening
                // upstream port, so complete URL metadata compares exactly.
            }
        } finally {
            for (const [socket, source] of sockets) {source.fixtureDestroyed = true; socket.destroy();}
            await Promise.all([ipv4, ipv6].map(server => new Promise(resolve => server.close(resolve))));
        }
        row.exactPublicMatch = !row.reference.collectionFailure && !row.native.collectionFailure &&
            isDeepStrictEqual({guest: row.reference.guest, owner: row.reference.owner}, {guest: row.native.guest, owner: row.native.owner});
        try {validateRetiredCase(row, {bodyKind, graph}, group); row.contractPassed = true;}
        catch (error) {row.contractPassed = false; row.contractFailure = String(error.stack ?? error);}
        save(); console.log(JSON.stringify({bodyKind, graph, contractPassed: row.contractPassed, exactPublicMatch: row.exactPublicMatch,
            referenceFailure: row.reference.collectionFailure?.split('\n')[0], nativeFailure: row.native.collectionFailure?.split('\n')[0]}));
    }
    for (const [file, expected] of Object.entries(measured)) assert.equal(hash(file), expected);
    report.complete = true; report.measuredInputsUnchanged = true;
    report.status = report.cases.every(row => row.contractPassed) ? 'complete' : 'complete-with-findings';
    report.matches = report.cases.filter(row => row.exactPublicMatch).length;
    report.contractsPassed = report.cases.filter(row => row.contractPassed).length;
    save();
    const validation = validateRetiredReport(report, group);
    console.log(JSON.stringify({complete: true, status: report.status, group, ...validation}));
} catch (error) {
    report.complete = true; report.status = report.status === 'complete-with-findings' ? report.status : 'runner-error';
    report.error = String(error.stack ?? error); save(); throw error;
}
