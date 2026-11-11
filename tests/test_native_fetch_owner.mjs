import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import net from 'node:net';
import {spawn} from 'node:child_process';
import {createHash} from 'node:crypto';
import {setTimeout as delay} from 'node:timers/promises';
import {fileURLToPath} from 'node:url';
import {nativeFetchOwnerInputPaths, nativeFetchOwnerCasesForShard, nativeFetchOwnerReply} from '../tools/native-fetch-owner-input.mjs';
import {assertNativeFetchOwnerCase, validateNativeFetchOwnerReport} from '../tools/native-fetch-owner-contract.mjs';

const root = fileURLToPath(new URL('../', import.meta.url));
const hash = file => createHash('sha256').update(fs.readFileSync(file)).digest('hex');
const args = new Map();
for (let i = 2; i < process.argv.length; i += 2) {
    assert.ok(['--helper', '--worker', '--shard'].includes(process.argv[i]) && process.argv[i+1] && !args.has(process.argv[i]));
    args.set(process.argv[i], process.argv[i+1]);
}
assert.equal(args.size, 3);
const helper = args.get('--helper'), worker = args.get('--worker'), shard = args.get('--shard');
const plans = nativeFetchOwnerCasesForShard(shard);
const fixture = fileURLToPath(new URL('./fixtures/native-fetch-owner.mjs', import.meta.url));
const inputs = () => Object.fromEntries(Object.entries(nativeFetchOwnerInputPaths).map(([key, name]) => [key, hash(path.join(root, name))]));
const expected = {shard, runId: process.env.CAPSID_WS_RUN_ID ?? 'standalone-native-owner', inputs: inputs(), binaries: {helper: hash(helper), worker: hash(worker)}};
const report = {schema: 'capsid-native-fetch-owner-v1', complete: false, ...expected, cases: []};
let destination;
if (process.env.CAPSID_WS_REPORT_DIR) {
    fs.mkdirSync(process.env.CAPSID_WS_REPORT_DIR, {recursive: true});
    destination = path.join(process.env.CAPSID_WS_REPORT_DIR, 'native-fetch-owner-'+shard+'.json');
    assert.equal(fs.existsSync(destination), false, 'fresh actual owner observations');
}
const save = () => {if (destination) fs.writeFileSync(destination, JSON.stringify(report, null, 2)+'\n');};
save();
for (const plan of plans) {
    const row = {plan}; report.cases.push(row);
    const reply = nativeFetchOwnerReply(plan), sockets = new Set(); let source;
    const server = net.createServer({allowHalfOpen: true}, socket => {
        sockets.add(socket);
        source = {request: null, rawRequest: '', peerEnded: false, peerReset: false, fixtureEnded: false,
            fixtureDestroyed: false, events: [], sent: ''}; row.source = source;
        socket.on('end', () => {source.peerEnded = true; source.events.push('peer-ended');});
        socket.on('close', () => sockets.delete(socket));
        socket.on('error', error => {if (error.code === 'ECONNRESET') source.peerReset = true; else source.error = error.code;});
        let request = Buffer.alloc(0);
        socket.on('data', chunk => {
            request = Buffer.concat([request, chunk]); source.rawRequest = request.toString('base64');
            if (source.request !== null) return;
            const end = request.indexOf('\r\n\r\n'); if (end < 0) return;
            source.request = request.subarray(0, end).toString(); source.events.push('request');
            const sent = Buffer.from(reply.packet); source.sent = sent.toString('base64');
            source.fixtureEnded = true; source.events.push('sent-complete'); socket.end(sent);
        });
    });
    await new Promise((resolve, reject) => {server.once('error', reject); server.listen(0, '127.0.0.1', resolve);});
    row.port = server.address().port; row.encodedBytes = reply.payload.length;
    const child = spawn(helper, [worker, fixture, 'http://localhost:'+row.port+'/hold', plan.action], {stdio: ['pipe', 'pipe', 'pipe']});
    let stdout = '', stderr = '', ready, acknowledged = false, timer;
    const stateReady = new Promise(resolve => {ready = resolve;});
    child.stdout.on('data', chunk => {
        stdout += chunk;
        const line = stdout.split(/\r?\n/).find(line => line.startsWith('OWNER_PROBE '));
        if (line) try {row.state = JSON.parse(line.slice(12)); ready();} catch {}
    });
    child.stderr.on('data', chunk => {stderr += chunk;}); child.stdin.on('error', () => {});
    const closed = new Promise((resolve, reject) => {child.once('close', (code, signal) => resolve({code, signal})); child.once('error', reject);});
    const bound = new Promise((_, reject) => {timer = setTimeout(() => reject(new Error('native owner peer12s deadline')), 12000);});
    let failure;
    try {
        await Promise.race([stateReady, closed.then(() => {assert.ok(row.state, 'helper produced actual owner state');}), bound]);
        const end = performance.now()+3000;
        while (!source?.peerEnded && !source?.peerReset && performance.now() < end) await delay(5);
        row.sourceTerminalBeforeFinish = source?.peerEnded === true || source?.peerReset === true;
        row.sourceBeforeFinish = source ? structuredClone(source) : null;
        assert.equal(row.sourceTerminalBeforeFinish, true, 'source closes before parent authorizes cleanup');
        child.stdin.end('finish\n'); acknowledged = true;
        row.shutdown = await Promise.race([closed, bound]); row.stdout = stdout; row.stderr = stderr;
        const exits = stdout.split(/\r?\n/).filter(line => line.startsWith('OWNER_EXIT '));
        assert.equal(exits.length, 1); row.workerExit = JSON.parse(exits[0].slice(11));
    } catch (error) {
        failure = error; row.collectionFailure = String(error.stack ?? error);
        if (!acknowledged && !child.stdin.destroyed) child.stdin.end('finish\n');
        if (child.exitCode === null && child.signalCode === null) {row.helperForced = true; child.kill();}
        row.shutdown ??= await Promise.race([closed.catch(() => null), delay(2000).then(() => null)]);
        row.stdout = stdout; row.stderr = stderr;
    } finally {
        clearTimeout(timer);
        for (const socket of sockets) {source.fixtureDestroyed = true; socket.destroy();}
        await new Promise(resolve => server.close(resolve)); save();
        console.log('NATIVE_OWNER_ROW '+JSON.stringify(row));
    }
    if (failure) throw failure;
    assertNativeFetchOwnerCase(row, plan);
}
assert.deepEqual(inputs(), expected.inputs);
assert.deepEqual({helper: hash(helper), worker: hash(worker)}, expected.binaries);
report.complete = true; save(); validateNativeFetchOwnerReport(report, expected);
console.log(JSON.stringify({complete: true, shard, cases: plans.length, actualPeerTerminalBeforeStop: true, actualCleanExit: true}));
