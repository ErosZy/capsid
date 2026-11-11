import assert from 'node:assert/strict';
import {execFileSync, spawn} from 'node:child_process';
import {randomUUID} from 'node:crypto';
import fs from 'node:fs';
import http from 'node:http';
import net from 'node:net';
import path from 'node:path';
import {setTimeout as delay} from 'node:timers/promises';
import {loadNodeWS} from './helpers/ws_interop_node_sdk.mjs';

const args = new Map();
for (let i = 2; i < process.argv.length; i += 2) {
    assert.ok(process.argv[i].startsWith('--') && process.argv[i + 1]);
    assert.ok(!args.has(process.argv[i]));
    args.set(process.argv[i], process.argv[i + 1]);
}
const fixture = args.get('--fixture'), worker = args.get('--worker'), scenario = args.get('--case');
assert.ok(fixture && worker);
assert.ok(['drain-before-submit', 'drain-during-prepare', 'queued-before-drain',
    'owner-drain-before-submit', 'healthy-control'].includes(scenario));
const draining = scenario !== 'healthy-control';
const {WebSocket, identity: clientIdentity} = loadNodeWS(args.get('--node-package'));
const within = async (promise, label, milliseconds = 8000) => {
    let timer;
    try {
        return await Promise.race([promise, new Promise((_, reject) => {
            timer = setTimeout(() => reject(new Error(`${label} deadline`)), milliseconds);
        })]);
    } finally { clearTimeout(timer); }
};
function workersOf(pid) {
    if (process.platform === 'win32') {
        const command = `ConvertTo-Json -Compress -InputObject @(Get-CimInstance Win32_Process -Filter 'ParentProcessId=${pid}' | Where-Object Name -eq 'capsid-worker.exe' | Select-Object ProcessId,ExecutablePath)`;
        return JSON.parse(execFileSync('powershell.exe', ['-NoProfile', '-NonInteractive', '-Command', command],
            {encoding: 'utf8', windowsHide: true, timeout: 6000}).trim() || '[]').map(item => {
            assert.equal(path.resolve(item.ExecutablePath).toLowerCase(), path.resolve(worker).toLowerCase());
            return {pid: item.ProcessId};
        });
    }
    if (process.platform === 'linux') {
        return fs.readdirSync('/proc').filter(name => /^\d+$/.test(name)).flatMap(name => {
            try {
                const stat = fs.readFileSync(`/proc/${name}/stat`, 'utf8');
                const tail = stat.slice(stat.lastIndexOf(') ') + 2).split(' ');
                if (Number(tail[1]) !== pid || !stat.includes('(capsid-worker)')) return [];
                assert.equal(fs.realpathSync(`/proc/${name}/exe`), fs.realpathSync(worker));
                return [{pid: Number(name)}];
            } catch (error) { if (error.code === 'ENOENT' || error.code === 'ESRCH') return []; throw error; }
        });
    }
    const ps = values => execFileSync('ps', values,
        {encoding: 'utf8', env: {...process.env, LC_ALL: 'C'}, timeout: 6000}).trim();
    return ps(['-axo', 'pid=,ppid=']).split(/\r?\n/).map(line => line.trim().split(/\s+/).map(Number))
        .filter(([, parent]) => parent === pid).map(([child]) => {
            assert.equal(fs.realpathSync(ps(['-o', 'comm=', '-p', String(child)])), fs.realpathSync(worker));
            return {pid: child};
        });
}
function exists(pid) {
    try { process.kill(pid, 0); return true; }
    catch (error) { if (error.code === 'ESRCH') return false; throw error; }
}
async function reaped(pids) {
    const deadline = performance.now() + 6000;
    while (pids.some(exists) && performance.now() < deadline) await delay(10);
    assert.deepEqual(pids.filter(exists), [], 'actual Worker PIDs reaped');
}

const child = spawn(fixture, [worker, scenario], {stdio: ['pipe', 'pipe', 'pipe'], windowsHide: true});
let stderr = '', pending = '', childResult, primaryError, inputError, forced = false;
const stages = new Map(), waiters = new Map(), sockets = new Set(), websockets = new Set(), agents = new Set();
child.stderr.setEncoding('utf8'); child.stderr.on('data', value => { stderr += value; });
child.stdin.on('error', error => { inputError ??= error; });
const rejectWaiters = error => { for (const {reject} of waiters.values()) reject(error); waiters.clear(); };
child.stdout.setEncoding('utf8'); child.stdout.on('data', value => {
    pending += value;
    while (pending.includes('\n')) {
        const end = pending.indexOf('\n'), line = pending.slice(0, end).trim();
        pending = pending.slice(end + 1);
        try {
            const record = JSON.parse(line);
            assert.equal(typeof record.stage, 'string'); assert.ok(!stages.has(record.stage));
            stages.set(record.stage, record);
            waiters.get(record.stage)?.resolve(record); waiters.delete(record.stage);
        } catch (error) { primaryError ??= error; rejectWaiters(error); }
    }
});
const closed = new Promise((resolve, reject) => {
    child.once('error', error => { rejectWaiters(error); reject(error); });
    child.once('close', (code, signal) => {
        childResult = {code, signal};
        rejectWaiters(new Error(`fixture closed ${code}/${signal}: ${stderr}`)); resolve(childResult);
    });
});
closed.catch(() => {});
const stage = name => within(new Promise((resolve, reject) => {
    if (primaryError) reject(primaryError);
    else if (stages.has(name)) resolve(stages.get(name));
    else if (childResult) reject(new Error(`fixture closed before ${name}: ${stderr}`));
    else waiters.set(name, {resolve, reject});
}), name, name === 'selected-ready' ? 20000 : 8000);
const command = value => {
    if (inputError) throw inputError;
    assert.ok(!childResult && child.stdin.writable); child.stdin.write(value + '\n');
};
async function connectPeer(port) {
    const socket = net.connect({host: '127.0.0.1', port}); sockets.add(socket);
    const chunks = [], errors = [];
    socket.on('data', chunk => chunks.push(chunk)); socket.on('error', error => errors.push(error.code));
    const terminal = new Promise(resolve => socket.once('close', hadError => {
        sockets.delete(socket); resolve({bytes: Buffer.concat(chunks).length, errors, hadError});
    }));
    await within(new Promise((resolve, reject) => { socket.once('connect', resolve); socket.once('error', reject); }), 'pending peer connected');
    return {socket, chunks, terminal};
}
async function request(port, peer = undefined) {
    const challenge = randomUUID();
    const agent = new http.Agent({keepAlive: false, maxSockets: 1}); agents.add(agent);
    if (peer) agent.createConnection = () => peer.socket;
    let terminal;
    // A refused admission has no HTTP response. Capture that actual outcome
    // without rejecting before the test has released the blocked owner.
    const outcome = await within(new Promise(resolve => {
        const failure = error => resolve({error: {code: error.code, message: error.message}});
        const call = http.get({hostname: '127.0.0.1', port,
            path: `/@capsid/orders/healthy?challenge=${challenge}`, agent,
            headers: {Host: 'public.example', Connection: 'close'}}, reply => {
            const chunks = [];
            reply.on('data', chunk => chunks.push(chunk));
            reply.once('end', () => resolve({response: {status: reply.statusCode, complete: reply.complete,
                headers: reply.headers, body: Buffer.concat(chunks).toString('utf8')}}));
            reply.once('error', failure);
        });
        call.once('socket', socket => {
            if (peer) assert.equal(socket, peer.socket, 'HTTP must use the original pending connection');
            sockets.add(socket);
            terminal = new Promise(resolve => socket.once('close', hadError => { sockets.delete(socket); resolve({hadError}); }));
        });
        call.once('error', failure);
    }), 'HTTP outcome on original peer');
    const tcpClose = await within(terminal, 'HTTP TCP close');
    agent.destroy(); agents.delete(agent);
    return {...outcome, challenge, tcpClose};
}
function healthyResponse(value) {
    assert.equal(value.error, undefined); assert.equal(value.response.status, 200);
    assert.equal(value.response.complete, true);
    assert.equal(value.response.headers.connection?.toLowerCase(), 'close');
    assert.deepEqual(value.tcpClose, {hadError: false});
    const body = JSON.parse(value.response.body);
    assert.equal(body.challenge, value.challenge); assert.match(body.instance, /^[0-9a-f-]{36}$/);
    return body.instance;
}
async function openWebSocket(port) {
    const ws = new WebSocket(`ws://127.0.0.1:${port}/@capsid/orders/ws`, {headers: {Host: 'public.example'}});
    websockets.add(ws);
    const errors = [];
    ws.on('error', error => errors.push(error.message));
    const terminal = new Promise(resolve => ws.once('close', (code, reason) => {
        websockets.delete(ws); resolve({code, reason: reason.toString(), errors});
    }));
    await within(new Promise((resolve, reject) => { ws.once('open', resolve); ws.once('error', reject); }), 'WebSocket open');
    return {ws, terminal, frames: 0, bytes: 0};
}
async function echo(client, phase) {
    for (const binary of [false, true]) for (const size of [0, 1, 126, 65536]) {
        const data = Buffer.alloc(size, 65);
        const response = new Promise((resolve, reject) => {
            const onClose = () => { client.ws.off('message', onMessage); reject(new Error('close before echo')); };
            const onMessage = (bytes, isBinary) => { client.ws.off('close', onClose); resolve({bytes, isBinary}); };
            client.ws.once('close', onClose); client.ws.once('message', onMessage);
        });
        client.ws.send(data, {binary});
        const result = await within(response, `${phase} echo ${size}/${binary}`);
        assert.equal(result.isBinary, binary); assert.deepEqual(result.bytes, data);
        client.frames++; client.bytes += size;
    }
}
async function closeWebSocket(client) {
    client.ws.close(1000, 'done');
    const result = await within(client.terminal, 'normal WebSocket close');
    assert.deepEqual(result, {code: 1000, reason: 'done', errors: []});
    return {frames: client.frames, bytes: client.bytes, ...result};
}
async function released(port) {
    await within(new Promise((resolve, reject) => {
        const socket = net.connect({host: '127.0.0.1', port}); sockets.add(socket);
        socket.once('close', () => sockets.delete(socket));
        socket.once('connect', () => { socket.destroy(); reject(new Error('listener survived server wait')); });
        socket.once('error', error => error.code === 'ECONNREFUSED' ? resolve() : reject(error));
    }), 'released listener');
}

let actualWorkers = [], selectedPid, healthyPid, port, healthyPort, adoption;
let healthyBefore, healthyAfter, pendingRequest, peerTerminal, wsObservations;
try {
    await stage('selected-ready');
    actualWorkers = workersOf(child.pid); assert.equal(actualWorkers.length, 1);
    selectedPid = actualWorkers[0].pid;
    command('arm');
    ({port, healthyPort} = await stage('listeners-ready'));
    for (const value of [port, healthyPort]) assert.ok(Number.isInteger(value) && value > 0 && value <= 65535);
    assert.notEqual(port, healthyPort);
    actualWorkers = workersOf(child.pid); assert.equal(actualWorkers.length, 2);
    assert.equal(actualWorkers.filter(item => item.pid === selectedPid).length, 1);
    healthyPid = actualWorkers.find(item => item.pid !== selectedPid).pid;
    const client = await openWebSocket(port); await echo(client, 'before drain');
    healthyBefore = await request(healthyPort); const healthyInstance = healthyResponse(healthyBefore);
    command('warm');
    assert.deepEqual(await stage('owner-positioned'), {stage: 'owner-positioned',
        blocked: scenario !== 'owner-drain-before-submit', workerAvailable: true});
    command('prepare'); await stage('pending-armed');
    if (scenario === 'drain-before-submit' || scenario === 'owner-drain-before-submit')
        assert.deepEqual(await stage('drain-requested'), {stage: 'drain-requested', draining: true, finished: false, workerAvailable: true});
    if (scenario === 'owner-drain-before-submit')
        assert.deepEqual(await stage('owner-drain-processed'), {stage: 'owner-drain-processed', workerAvailable: true, draining: true});
    const peer = await connectPeer(port);
    const pendingOutcome = request(port, peer);
    // Attach a rejection handler immediately even if a fixture barrier fails.
    pendingOutcome.catch(() => {});
    if (scenario === 'drain-during-prepare') {
        const prepared = await stage('adoption-prepared');
        assert.equal(prepared.workerAvailable, true); assert.equal(prepared.draining, false);
        assert.ok(prepared.allocationBytes > 0); assert.equal(prepared.allocationBytes, prepared.expectedAllocationBytes);
        command('drain');
        assert.deepEqual(await stage('drain-requested'), {stage: 'drain-requested', draining: true, finished: false, workerAvailable: true});
        command('release-allocation');
    }
    adoption = await stage('adoption-returned');
    assert.equal(adoption.workerAvailable, true);
    if (scenario === 'queued-before-drain') {
        assert.equal(adoption.accepted, true); assert.equal(adoption.draining, false);
        command('drain');
        assert.deepEqual(await stage('drain-requested'), {stage: 'drain-requested', draining: true, finished: false, workerAvailable: true});
    }
    command('release-owner');
    assert.deepEqual(await stage('owner-released'), {stage: 'owner-released', workerAvailable: true, draining});
    pendingRequest = await pendingOutcome;
    peerTerminal = await within(peer.terminal, 'original pending peer terminal');
    // Exercise both live Workers after the transition before checking the
    // refusal contract, so a broken admission is not confused with a dead host.
    await echo(client, 'after owner resumed');
    healthyAfter = await request(healthyPort); assert.equal(healthyResponse(healthyAfter), healthyInstance);
    assert.notEqual(healthyBefore.challenge, healthyAfter.challenge);
    await echo(client, 'after healthy request'); wsObservations = await closeWebSocket(client);
    assert.equal(wsObservations.frames, 24); assert.equal(wsObservations.bytes, 393978);
    if (draining) {
        assert.equal(peerTerminal.bytes, 0, 'draining owner must not execute pending HTTP');
        assert.equal(pendingRequest.response, undefined, 'refused admission has no HTTP response');
        assert.equal(pendingRequest.error?.code, 'ECONNRESET');
        if (scenario === 'queued-before-drain') {
            assert.equal(adoption.accepted, true); assert.equal(adoption.retained, false); assert.equal(adoption.openAfter, false);
        } else {
            assert.equal(adoption.accepted, false, 'drain call must synchronously reject new admission');
            assert.equal(adoption.retained, true, 'refusal must retain original native handle and executor');
            assert.equal(adoption.openAfter, true); assert.equal(adoption.draining, true);
        }
    } else {
        assert.notEqual(healthyResponse(pendingRequest), healthyInstance);
        assert.ok(peerTerminal.bytes > pendingRequest.response.body.length);
        assert.equal(peerTerminal.hadError, false); assert.deepEqual(peerTerminal.errors, []);
        assert.equal(adoption.accepted, true); assert.equal(adoption.retained, false);
        assert.equal(adoption.openAfter, false); assert.equal(adoption.draining, false);
    }
    command('finish');
    const stopped = await stage('stopped');
    assert.deepEqual(stopped, {stage: 'stopped', selectedAvailable: false, healthyAvailable: false,
        warmAccepted: 1, pendingCalls: 1, healthyAccepted: 2, draining, finished: draining, forcedCancellations: 0});
    child.stdin.end();
    assert.deepEqual(await within(closed, 'normal fixture close including stdio'), {code: 0, signal: null});
    await reaped(actualWorkers.map(value => value.pid));
    await released(port); await released(healthyPort);
    if (primaryError) throw primaryError;
    if (inputError) throw inputError;
    assert.equal(pending, '');
} catch (error) { primaryError ??= error; }
finally {
    for (const ws of websockets) ws.terminate();
    for (const agent of agents) agent.destroy();
    for (const socket of sockets) socket.destroy();
    if (!childResult) {
        child.stdin.end();
        try { await within(closed, 'fixture cleanup'); }
        catch (error) {
            forced = true; child.kill('SIGKILL');
            try { await within(closed, 'forced fixture close'); } catch { /* preserve cleanup failure */ }
            primaryError = new AggregateError([primaryError, error].filter(Boolean), 'scenario and fixture cleanup failed');
        }
    }
    try { await reaped(actualWorkers.map(value => value.pid)); }
    catch (error) { primaryError = new AggregateError([primaryError, error].filter(Boolean), 'scenario and Worker cleanup failed'); }
}
console.log(JSON.stringify({complete: !primaryError && !forced, scenario, fixturePid: child.pid,
    actualWorkers, selectedPid, healthyPid, stages: [...stages.values()], adoption,
    pendingRequest, peerTerminal, healthyBefore, healthyAfter, wsObservations,
    clientIdentity, result: childResult, forced, stderr,
    error: primaryError ? {name: primaryError.name, code: primaryError.code, message: primaryError.message} : undefined}));
if (primaryError) throw primaryError;
assert.equal(forced, false);
