import assert from 'node:assert/strict';
import fs from 'node:fs';
import net from 'node:net';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import {createHash} from 'node:crypto';
import {spawn} from 'node:child_process';
import {gzipSync} from 'node:zlib';
import {setTimeout as delay} from 'node:timers/promises';

import {unreadBindingCases, unreadBindingInputPaths} from '../tools/binding-fetch-unread-input.mjs';
import {assertUnreadBindingCase, validateUnreadBindingReport} from '../tools/binding-fetch-unread-contract.mjs';
const root = fileURLToPath(new URL('../', import.meta.url));
const args = new Map();
for (let index = 2; index < process.argv.length; index += 2) {
    assert.ok(['--helper', '--worker', '--kind'].includes(process.argv[index]) && process.argv[index + 1] && !args.has(process.argv[index]));
    args.set(process.argv[index], process.argv[index + 1]);
}
assert.equal(args.size, 3);
const helper = args.get('--helper'), worker = args.get('--worker'), kind = args.get('--kind');
const plans = unreadBindingCases(kind), hash = file => createHash('sha256').update(fs.readFileSync(file)).digest('hex');
const inputs = () => Object.fromEntries(Object.entries(unreadBindingInputPaths).map(([name, file]) => [name, hash(path.join(root, file))]));
const binding = path.join(root, unreadBindingInputPaths.binding), user = path.join(root, unreadBindingInputPaths.user);
const expected = {kind, runId: process.env.CAPSID_WS_RUN_ID ?? 'standalone-binding-unread', inputs: inputs(),
    binaries: {helper: hash(helper), worker: hash(worker)}};
const report = {schema: 'capsid-binding-fetch-unread-v1', complete: false, ...expected, cases: []};
let destination;
if (process.env.CAPSID_WS_REPORT_DIR) {
    fs.mkdirSync(process.env.CAPSID_WS_REPORT_DIR, {recursive: true});
    destination = path.join(process.env.CAPSID_WS_REPORT_DIR, `binding-fetch-unread-${kind}.json`);
    assert.equal(fs.existsSync(destination), false, 'fresh actual unread observations');
}
const save = () => { if (destination) fs.writeFileSync(destination, JSON.stringify(report, null, 2) + '\n'); }; save();
for (const plan of plans) {
    const row = {plan, collectionComplete: false}, sources = [], sockets = new Map(); report.cases.push(row);
    const payload = Buffer.from(Array.from({length: 16}, (_, index) => index + 1)), body = plan.kind === 'raw' ? payload : gzipSync(payload);
    if (plan.kind === 'gzip') body[9] = 255;
    const server = net.createServer({allowHalfOpen: true}, socket => {
        socket.setNoDelay(true); let bytes = Buffer.alloc(0);
        const source = {request: null, rawRequest: '', sent: [], events: ['accepted'], peerEnded: false, peerReset: false,
            fixtureEnded: false, fixtureDestroyed: false}; sources.push(source); sockets.set(socket, source);
        socket.on('end', () => { source.peerEnded = true; source.events.push('peer-ended'); });
        socket.on('error', error => { if (error.code === 'ECONNRESET') source.peerReset = true; else source.error = error.code; });
        socket.on('close', () => sockets.delete(socket));
        socket.on('data', chunk => {
            bytes = Buffer.concat([bytes, chunk]); source.rawRequest = bytes.toString('base64');
            if (source.request !== null) return; const at = bytes.indexOf('\r\n\r\n'); if (at < 0) return;
            source.request = bytes.subarray(0, at).toString(); source.events.push('request');
            const target = new URL(source.request.split(' ')[1], 'http://127.0.0.1'); source.slot = Number(target.searchParams.get('slot'));
            const head = Buffer.from('HTTP/1.1 201 Created\r\nConnection: close\r\nContent-Type: application/octet-stream\r\n' +
                (plan.kind === 'gzip' ? 'Content-Encoding: gzip\r\n' : '') +
                `X-Unread-Slot: ${source.slot}\r\nContent-Length: ${body.length}\r\n\r\n`);
            source.sent.push(head.toString('base64')); source.events.push('sent-head'); socket.write(head);
        });
    });
    await new Promise((resolve, reject) => { server.once('error', reject); server.listen(0, '127.0.0.1', resolve); }); row.port = server.address().port;
    const child = spawn(helper, [worker, binding, user, `http://127.0.0.1:${row.port}`, `${plan.stage},${plan.count}`], {stdio: ['pipe', 'pipe', 'pipe']});
    let stdout = '', stderr = '', pending = '', resolvePhase2, rejectPhase2, timer;
    const phase2 = new Promise((resolve, reject) => { resolvePhase2 = resolve; rejectPhase2 = reject; });
    child.stdin.on('error', () => {}); child.stdout.setEncoding('utf8');
    child.stdout.on('data', value => {
        stdout += value; pending += value;
        for (;;) {
            const at = pending.indexOf('\n'); if (at < 0) break;
            const line = pending.slice(0, at).replace(/\r$/, ''); pending = pending.slice(at + 1);
            try {
                if (line.startsWith('BINDING_PHASE 1 ')) {
                    assert.equal(row.request1, undefined); row.request1 = JSON.parse(line.slice(16));
                    row.sourceAfterRequest1 = structuredClone(sources);
                    assert.equal(sources.length, plan.count);
                    assert.ok(sources.every(source => source.sent.length === 1 && !source.peerEnded && !source.peerReset && !source.fixtureEnded));
                    child.stdin.write('continue\n');
                } else if (line.startsWith('BINDING_PHASE 2 ')) {
                    assert.equal(row.request2, undefined); row.request2 = JSON.parse(line.slice(16)); resolvePhase2();
                } else if (line.startsWith('BINDING_EXIT ')) row.workerExit = JSON.parse(line.slice(13));
            } catch (error) { rejectPhase2(error); }
        }
    });
    child.stderr.on('data', value => { stderr += value; });
    const closed = new Promise((resolve, reject) => { child.once('close', (code, signal) => resolve({code, signal})); child.once('error', reject); });
    const bound = new Promise((_, reject) => { timer = setTimeout(() => reject(new Error('private unread observation12s bound')), 12000); });
    try {
        await Promise.race([phase2, closed.then(() => assert.ok(row.request2, 'two actual User requests before exit')), bound]);
        row.sourceBeforeStop = structuredClone(sources);
        assert.equal(sources.length, plan.count); assert.ok(sources.every(source => !source.peerEnded && !source.peerReset && !source.fixtureEnded));
        child.stdin.end('finish\n'); row.helperExit = await Promise.race([closed, bound]);
        const until = performance.now() + 3000;
        while (performance.now() < until && !sources.every(source => source.peerEnded || source.peerReset)) await delay(5);
        row.sourceTerminalAfterWorkerExit = sources.length === plan.count && sources.every(source => source.peerEnded || source.peerReset);
        row.sourceAfterWorkerExit = structuredClone(sources); row.collectionComplete = true;
    } catch (error) { row.failure = String(error.stack ?? error); row.sourceFailure = structuredClone(sources); }
    finally {
        clearTimeout(timer);
        if (child.exitCode === null && child.signalCode === null) { row.helperForced = true; child.kill(); row.helperExit = await closed; }
        row.stdout = stdout; row.stderr = stderr; row.sourceFinalBeforeFixtureCleanup = structuredClone(sources);
        for (const [socket, source] of sockets) { source.fixtureDestroyed = true; socket.destroy(); }
        await new Promise(resolve => server.close(resolve)); save();
        console.log('BINDING_UNREAD_ROW ' + JSON.stringify(row));
    }
    assertUnreadBindingCase(row, plan);
}
assert.equal(hash(helper), report.binaries.helper); assert.equal(hash(worker), report.binaries.worker);
assert.deepEqual(inputs(), expected.inputs);
report.complete = true; save(); validateUnreadBindingReport(report, expected);
console.log(JSON.stringify({complete: true, kind, cases: plans.length, requests: plans.length * 2,
    rpcCalls: plans.length * 2, sourceConnections: plans.reduce((total, plan) => total + plan.count, 0),
    allSourcesLiveAcrossUserRequests: true, allPeersTerminalBeforeFixtureCleanup: true, allNaturalWorkerExits: true}));
