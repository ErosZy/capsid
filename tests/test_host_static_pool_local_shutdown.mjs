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
assert.ok(['stop', 'drain'].includes(scenario));
const draining = scenario === 'drain';
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
}), name, name === 'pool-ready' ? 20000 : 8000);
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
async function request(port, peer = undefined, configuration = {}) {
    const challenge = randomUUID();
    const agent = new http.Agent({keepAlive: false, maxSockets: 1}); agents.add(agent);
    if (peer) agent.createConnection = () => peer.socket;
    let terminal;
    // A refused admission has no HTTP response. Capture that actual outcome
    // without rejecting before the test has released the blocked owner.
    const outcome = await within(new Promise(resolve => {
        const failure = error => resolve({error: {code: error.code, message: error.message}});
        const call = http.request({hostname: '127.0.0.1', port,
            method: configuration.method ?? 'GET', path: `/@capsid/orders/healthy?challenge=${challenge}`, agent,
            headers: {Host: 'public.example', Connection: 'close', ...configuration.headers}}, reply => {
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
        call.once('error', failure); call.end();
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

function localResponse(value, status, method = 'GET') {
    assert.equal(value.error, undefined); assert.equal(value.response.status, status);
    assert.equal(value.response.complete, true);
    assert.equal(value.response.headers.connection?.toLowerCase(), 'close');
    assert.deepEqual(value.tcpClose, {hadError: false});
    if (status === 503) {
        assert.equal(value.response.headers['content-length'], '18');
        assert.equal(value.response.body, method === 'HEAD' ? '' : 'worker unavailable');
        assert.equal(value.response.headers['access-control-allow-origin'], 'https://client.example');
    } else if (status === 204) {
        assert.equal(value.response.body, '');
        assert.equal(value.response.headers['access-control-allow-origin'], 'https://client.example');
        assert.ok(value.response.headers['access-control-allow-methods'].split(/,\s*/).includes('GET'));
    } else {
        assert.equal(value.response.headers['access-control-allow-origin'], undefined);
        assert.ok(value.response.body.length > 0);
    }
}

let actualWorkers = [], port, warmResponses = [], wsObservations, localResponses = [], barrierResponse;
const peers = [];
let pendingBarrier, peerTerminals, peerAfterCleanup;
try {
    const ready = await stage('pool-ready'); assert.equal(ready.active, 2); port = ready.port;
    assert.ok(Number.isInteger(port) && port > 0 && port <= 65535);
    actualWorkers = workersOf(child.pid); assert.equal(actualWorkers.length, 2);
    assert.equal(new Set(actualWorkers.map(value => value.pid)).size, 2);
    for (let i = 0; i < 2; ++i) warmResponses.push(await request(port));
    assert.notEqual(healthyResponse(warmResponses[0]), healthyResponse(warmResponses[1]));
    assert.notEqual(warmResponses[0].challenge, warmResponses[1].challenge);
    const client = await openWebSocket(port); await echo(client, 'live full pool');
    for (const worker of actualWorkers) process.kill(worker.pid, 'SIGKILL');
    command('await-all-exits');
    assert.deepEqual(await stage('no-workers-left'), {stage: 'no-workers-left', active: 0});
    await reaped(actualWorkers.map(value => value.pid));
    const faultClose = await within(client.terminal, 'actual Worker exit WebSocket');
    assert.deepEqual(faultClose, {code: 1006, reason: '', errors: []});
    wsObservations = {frames: client.frames, bytes: client.bytes, ...faultClose};
    assert.equal(client.frames, 8); assert.equal(client.bytes, 131326);
    for (const [method, extra, status] of [
        ['GET', {}, 503], ['HEAD', {}, 503],
        ['OPTIONS', {'Access-Control-Request-Method': 'GET', 'Access-Control-Request-Headers': 'x-challenge'}, 204],
        ['OPTIONS', {'Access-Control-Request-Method': 'POST'}, 403],
    ]) {
        const value = await request(port, undefined, {method, headers: {Origin: 'https://client.example', ...extra}});
        localResponse(value, status, method); localResponses.push({method, ...value});
    }
    const fragments = [
        'GET /@capsid/orders/pending HTTP/1.1\r\nHost: public.example\r\n',
        'HEAD /@capsid/orders/pending HTTP/1.1\r\nHost: public.example\r\n',
        'OPTIONS /@capsid/orders/pending HTTP/1.1\r\nHost: public.example\r\nOrigin: https://client.example\r\nAccess-Control-Request-Method: GET\r\n',
    ];
    for (const fragment of fragments) {
        const peer = await connectPeer(port); peers.push(peer);
        await within(new Promise((resolve, reject) => peer.socket.write(fragment, error => error ? reject(error) : resolve())), 'pending head written');
    }
    // The single accept loop starts each earlier read before rearming accept.
    // A complete fourth response proves all three preceding peers reached it.
    barrierResponse = await request(port, undefined, {headers: {Origin: 'https://client.example'}});
    localResponse(barrierResponse, 503);
    assert.ok(peers.every(peer => !peer.socket.destroyed && peer.chunks.length === 0));
    pendingBarrier = {acceptedBeforeFourthResponse: 3, responseBytes: 0};
    command('shutdown');
    assert.deepEqual(await stage('shutdown-requested'), {stage: 'shutdown-requested', active: 0, calls: 3, draining});
    peerTerminals = await within(Promise.all(peers.map(peer => peer.terminal)), 'pending local peers shutdown', 2000);
    for (const terminal of peerTerminals) {
        assert.equal(terminal.bytes, 0); assert.ok(terminal.errors.length === 0 || terminal.errors.every(value => value === 'ECONNRESET'));
    }
    command('finish');
    assert.deepEqual(await stage('stopped'), {stage: 'stopped', active: 0, waiters: 2, draining,
        finished: false, forcedCancellations: 0});
    child.stdin.end();
    assert.deepEqual(await within(closed, 'normal pool fixture close'), {code: 0, signal: null});
    await reaped(actualWorkers.map(value => value.pid)); await released(port);
    if (primaryError) throw primaryError;
    if (inputError) throw inputError;
    assert.equal(pending, ''); assert.equal(stderr, '');
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
    try {
        await reaped(actualWorkers.map(value => value.pid));
        peerAfterCleanup = await Promise.all(peers.map(peer => peer.terminal));
        if (port && childResult?.code === 0) await released(port);
    } catch (error) { primaryError = new AggregateError([primaryError, error].filter(Boolean), 'pool cleanup failed'); }
}
console.log(JSON.stringify({complete: !primaryError && !forced, scenario, fixturePid: child.pid,
    actualWorkers, stages: [...stages.values()], warmResponses, wsObservations, localResponses,
    barrierResponse, pendingBarrier, peerTerminals, peerAfterCleanup, clientIdentity, result: childResult,
    forced, stderr, error: primaryError ? {name: primaryError.name, code: primaryError.code, message: primaryError.message} : undefined}));
if (primaryError) throw primaryError;
assert.equal(forced, false);
