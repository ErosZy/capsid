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
assert.ok(['reselect-worker-exit', 'reselect-last-workers-exit', 'pre-post-stop',
    'pre-post-drain', 'active-drain'].includes(scenario));
const activeDrain = scenario === 'active-drain';
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
        const command = `ConvertTo-Json -Compress -InputObject @(Get-CimInstance Win32_Process -Filter 'ParentProcessId=${pid}' | Where-Object Name -eq 'capsid-worker.exe' | Select-Object ProcessId,ExecutablePath,@{Name='Created';Expression={$_.CreationDate.ToUniversalTime().Ticks.ToString()}})`;
        return JSON.parse(execFileSync('powershell.exe', ['-NoProfile', '-NonInteractive', '-Command', command],
            {encoding: 'utf8', windowsHide: true, timeout: 6000}).trim() || '[]').map(item => {
            assert.equal(path.resolve(item.ExecutablePath).toLowerCase(), path.resolve(worker).toLowerCase());
            return {pid: item.ProcessId, created: item.Created};
        });
    }
    if (process.platform === 'linux') {
        return fs.readdirSync('/proc').filter(name => /^\d+$/.test(name)).flatMap(name => {
            try {
                const stat = fs.readFileSync(`/proc/${name}/stat`, 'utf8');
                const tail = stat.slice(stat.lastIndexOf(') ') + 2).split(' ');
                if (Number(tail[1]) !== pid || !stat.includes('(capsid-worker)')) return [];
                assert.equal(fs.realpathSync(`/proc/${name}/exe`), fs.realpathSync(worker));
                return [{pid: Number(name), created: tail[19]}];
            } catch (error) { if (error.code === 'ENOENT' || error.code === 'ESRCH') return []; throw error; }
        });
    }
    const ps = args => execFileSync('ps', args,
        {encoding: 'utf8', env: {...process.env, LC_ALL: 'C'}, timeout: 6000}).trim();
    return ps(['-axo', 'pid=,ppid=']).split(/\r?\n/).map(line => line.trim().split(/\s+/).map(Number))
        .filter(([, parent]) => parent === pid).map(([child]) => {
            assert.equal(fs.realpathSync(ps(['-o', 'comm=', '-p', String(child)])), fs.realpathSync(worker));
            return {pid: child}; // the fixture obtains microseconds from XNU
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
    const socket = net.connect({host: '127.0.0.1', port});
    sockets.add(socket);
    const chunks = [], errors = [];
    socket.on('data', chunk => chunks.push(chunk)); socket.on('error', error => errors.push(error.code));
    const terminal = new Promise(resolve => socket.once('close', hadError => {
        sockets.delete(socket); resolve({bytes: Buffer.concat(chunks).length, errors, hadError});
    }));
    await within(new Promise((resolve, reject) => { socket.once('connect', resolve); socket.once('error', reject); }), 'fault peer connected');
    return {socket, chunks, terminal};
}
async function request(port, peer = undefined) {
    const challenge = randomUUID();
    const agent = new http.Agent({keepAlive: false, maxSockets: 1}); agents.add(agent);
    if (peer) agent.createConnection = () => peer.socket;
    let terminal;
    const response = await within(new Promise((resolve, reject) => {
        const call = http.get({hostname: '127.0.0.1', port,
            path: `/@capsid/orders/healthy?challenge=${challenge}`, agent,
            headers: {Host: 'public.example', Connection: 'close'}}, reply => {
            const chunks = [];
            reply.on('data', chunk => chunks.push(chunk));
            reply.once('end', () => resolve({status: reply.statusCode, complete: reply.complete,
                headers: reply.headers, body: Buffer.concat(chunks).toString('utf8')}));
            reply.once('error', reject);
        });
        call.once('socket', socket => {
            if (peer) assert.equal(socket, peer.socket, 'HTTP must use the paused original connection');
            sockets.add(socket);
            terminal = new Promise(resolve => socket.once('close', hadError => { sockets.delete(socket); resolve({hadError}); }));
        });
        call.once('error', reject);
    }), 'HTTP on original adoption peer');
    assert.equal(response.complete, true); assert.equal(response.headers.connection?.toLowerCase(), 'close');
    assert.deepEqual(await within(terminal, 'complete response TCP close'), {hadError: false});
    agent.destroy(); agents.delete(agent);
    return {...response, challenge};
}
async function openWebSocket(port, index) {
    const ws = new WebSocket(`ws://127.0.0.1:${port}/@capsid/orders/ws`, {headers: {Host: 'public.example'}});
    websockets.add(ws);
    const errors = [];
    ws.on('error', error => errors.push(error.message));
    const terminal = new Promise(resolve => ws.once('close', (code, reason) => {
        websockets.delete(ws); resolve({code, reason: reason.toString(), errors});
    }));
    await within(new Promise((resolve, reject) => { ws.once('open', resolve); ws.once('error', reject); }), 'WebSocket open');
    return {ws, index, terminal, frames: 0, bytes: 0};
}
async function echo(client, phase) {
    for (const binary of [false, true]) for (const size of [0, 1, 126, 65536]) {
        const data = Buffer.alloc(size, 65 + client.index);
        const response = new Promise((resolve, reject) => {
            const onClose = () => { client.ws.off('message', onMessage); reject(new Error('close before echo')); };
            const onMessage = (bytes, isBinary) => { client.ws.off('close', onClose); resolve({bytes, isBinary}); };
            client.ws.once('close', onClose); client.ws.once('message', onMessage);
        });
        client.ws.send(data, {binary});
        const result = await within(response, `${phase} echo ${client.index}/${size}/${binary}`);
        assert.equal(result.isBinary, binary); assert.deepEqual(result.bytes, data);
        client.frames++; client.bytes += size;
    }
}
async function closeWebSocket(client) {
    client.ws.close(1000, 'done');
    const result = await within(client.terminal, 'normal WebSocket close');
    assert.deepEqual(result, {code: 1000, reason: 'done', errors: []});
    return {index: client.index, frames: client.frames, bytes: client.bytes, ...result};
}

let actualWorkers = [], initialFailedPid, replacementPid, survivingPid, port, peer;
let firstResponse, afterResponse, peerTerminal, wsObservations = [], faultWebSocket, activePhaseBarrier;
try {
    const ready = await stage('pool-ready');
    assert.equal(ready.active, 3); port = ready.port;
    assert.ok(Number.isInteger(port) && port > 0 && port <= 65535);
    actualWorkers = workersOf(child.pid); assert.equal(actualWorkers.length, 3);
    if (process.platform === 'darwin') {
        command('created-times ' + actualWorkers.map(v => v.pid).join(' '));
        const times = (await stage('worker-times')).workers;
        assert.deepEqual(times.map(v => v.pid).sort((a, b) => a - b), actualWorkers.map(v => v.pid).sort((a, b) => a - b));
        actualWorkers = times;
    }
    actualWorkers.sort((a, b) => BigInt(a.created) < BigInt(b.created) ? -1 : BigInt(a.created) > BigInt(b.created) ? 1 : 0);
    assert.equal(new Set(actualWorkers.map(v => v.pid)).size, 3);
    for (let i = 1; i < 3; ++i) assert.ok(BigInt(actualWorkers[i - 1].created) < BigInt(actualWorkers[i].created),
        'strict OS creation times establish actual shard order');
    const activeClients = [];
    if (activeDrain) {
        // Three sequential upgrades consume shards 0, 1, 2. The next pending
        // accept is again on shard 0; after it exits, shard 1 is reselected.
        for (let i = 0; i < 3; ++i) {
            const client = await openWebSocket(port, i); activeClients.push(client); await echo(client, 'before fault');
        }
    }
    initialFailedPid = actualWorkers[0].pid;
    replacementPid = actualWorkers[1].pid;
    survivingPid = actualWorkers[2].pid;
    process.kill(initialFailedPid, 'SIGKILL'); command('await-first-exit');
    assert.deepEqual(await stage('first-worker-exited'), {stage: 'first-worker-exited', active: 2});
    await reaped([initialFailedPid]);
    if (activeDrain) {
        faultWebSocket = await within(activeClients[0].terminal, 'failed original Worker WebSocket');
        assert.equal(faultWebSocket.code, 1006, 'real first shard lost its Worker');
    }
    command('arm'); await stage('probe-armed');
    peer = await connectPeer(port);
    const paused = await stage('adoption-paused');
    assert.equal(paused.active, 2); assert.ok(paused.allocationBytes > 0);
    assert.equal(paused.allocationBytes, paused.expectedAllocationBytes);
    assert.match(paused.symbol, /capsid/); assert.match(paused.symbol, /adopt_socket/);
    assert.match(paused.address, /^(?:0x)?[0-9a-f]+$/i);
    assert.ok(exists(replacementPid) && exists(survivingPid));
    assert.equal(peer.chunks.length, 0);
    if (scenario.startsWith('reselect-')) {
        process.kill(replacementPid, 'SIGKILL');
        if (scenario === 'reselect-last-workers-exit') process.kill(survivingPid, 'SIGKILL');
        command('worker-exit');
    } else {
        // Bytes arrive only after the real allocation pause. No Session has
        // read them; an active draining shard must not admit this request.
        peer.socket.write('GET /@capsid/orders/healthy?challenge=must-not-run HTTP/1.1\r\nHost: public.example\r\nConnection: close\r\n\r\n');
        command(scenario === 'pre-post-stop' ? 'stop' : 'drain');
    }
    const remaining = scenario === 'reselect-worker-exit' ? 1 : activeDrain ? 2 : 0;
    assert.deepEqual(await stage('transition-observed'), {stage: 'transition-observed', active: remaining});
    if (remaining < 2) await reaped(actualWorkers.slice(0, remaining === 1 ? 2 : 3).map(v => v.pid));
    if (activeDrain) {
        // Actual round trips on both owner reactors follow begin_drain's
        // posted transition. They prove both Workers remain available while
        // the pending adoption is still paused and admission has closed.
        await Promise.all(activeClients.slice(1).map(client => echo(client, 'drain barrier')));
        assert.ok(exists(replacementPid) && exists(survivingPid));
        activePhaseBarrier = {liveWorkers: 2, completedOwnerRoundTrips: 2};
    }
    command('release'); await stage('probe-released');
    if (scenario.startsWith('reselect-')) {
        firstResponse = await request(port, peer);
        if (remaining) {
            assert.equal(firstResponse.status, 200);
            const first = JSON.parse(firstResponse.body);
            assert.equal(first.challenge, firstResponse.challenge); assert.match(first.instance, /^[0-9a-f-]{36}$/);
            const clients = await Promise.all(Array.from({length: 4}, (_, i) => openWebSocket(port, i)));
            await Promise.all(clients.map(client => echo(client, 'surviving replacement')));
            wsObservations = await Promise.all(clients.map(closeWebSocket));
            afterResponse = await request(port);
            assert.equal(afterResponse.status, 200);
            const after = JSON.parse(afterResponse.body);
            assert.equal(after.challenge, afterResponse.challenge); assert.equal(after.instance, first.instance);
            assert.notEqual(firstResponse.challenge, afterResponse.challenge);
        } else {
            assert.equal(firstResponse.status, 503);
            assert.equal(firstResponse.body, 'worker unavailable'); assert.equal(firstResponse.headers['content-length'], '18');
            afterResponse = await request(port);
            assert.equal(afterResponse.status, 503); assert.equal(afterResponse.body, 'worker unavailable');
        }
        peerTerminal = await within(peer.terminal, 'original peer terminal after response');
        assert.equal(peerTerminal.hadError, false); assert.ok(peerTerminal.bytes > 18);
    } else {
        peerTerminal = await within(peer.terminal, 'pending peer closed by stop/drain');
        assert.equal(peerTerminal.bytes, 0, 'pending adoption must not execute after stop/drain');
        if (activeDrain) {
            await Promise.all(activeClients.slice(1).map(client => echo(client, 'draining Worker after refusal')));
            wsObservations = await Promise.all(activeClients.slice(1).map(closeWebSocket));
        }
    }
    command('finish');
    const stopped = await stage('stopped');
    assert.equal(stopped.active, 0); assert.equal(stopped.forcedCancellations, 0);
    // The previously exited shard never completes a drain; pool.finished
    // aggregates every shard, including that earlier failure.
    if (scenario.endsWith('drain')) {
        assert.equal(stopped.draining, true); assert.equal(stopped.finished, false);
    }
    child.stdin.end();
    assert.deepEqual(await within(closed, 'normal fixture close including stdio'), {code: 0, signal: null});
    await reaped(actualWorkers.map(v => v.pid));
    await within(new Promise((resolve, reject) => {
        const socket = net.connect({host: '127.0.0.1', port}); sockets.add(socket);
        socket.once('close', () => sockets.delete(socket));
        socket.once('connect', () => { socket.destroy(); reject(new Error('listener survived pool wait')); });
        socket.once('error', error => error.code === 'ECONNREFUSED' ? resolve() : reject(error));
    }), 'released listener');
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
    try { await reaped(actualWorkers.map(v => v.pid)); }
    catch (error) { primaryError = new AggregateError([primaryError, error].filter(Boolean), 'scenario and Worker cleanup failed'); }
}
console.log(JSON.stringify({complete: !primaryError && !forced, scenario, fixturePid: child.pid,
    actualWorkers, initialFailedPid, replacementPid, survivingPid, stages: [...stages.values()],
    firstResponse, afterResponse, peerTerminal, wsObservations, faultWebSocket, activePhaseBarrier,
    clientIdentity, result: childResult, forced, stderr,
    error: primaryError ? {name: primaryError.name, code: primaryError.code, message: primaryError.message} : undefined}));
if (primaryError) throw primaryError;
assert.equal(forced, false);
