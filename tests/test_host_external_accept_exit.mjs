import assert from 'node:assert/strict';
import {execFileSync, spawn} from 'node:child_process';
import {randomUUID} from 'node:crypto';
import fs from 'node:fs';
import http from 'node:http';
import net from 'node:net';
import path from 'node:path';
import {setTimeout as delay} from 'node:timers/promises';
import {posixWorkerChildren} from './helpers/posix_worker_processes.mjs';

const args = new Map();
for (let i = 2; i < process.argv.length; i += 2) {
    assert.ok(process.argv[i].startsWith('--') && process.argv[i + 1], 'complete argument pair');
    assert.ok(!args.has(process.argv[i]), 'unique option');
    args.set(process.argv[i], process.argv[i + 1]);
}
const fixture = args.get('--fixture'), worker = args.get('--worker'), scenario = args.get('--case');
assert.ok(fixture && worker);
assert.ok(['before-completion', 'before-adoption', 'cancel-pending', 'queued-before-exit',
    'idle-after-adoption', 'keepalive-after-adoption', 'pre-post-worker-exit', 'pre-post-stop', 'pre-post-drain'].includes(scenario));
const prePost = scenario.startsWith('pre-post-');
const queued = scenario === 'queued-before-exit';
const keepalive = scenario === 'keepalive-after-adoption';
const adopted = queued || keepalive || scenario === 'idle-after-adoption';
const within = async (promise, stage, milliseconds = 8000) => {
    let timer;
    try {
        return await Promise.race([promise, new Promise((_, reject) => {
            timer = setTimeout(() => reject(new Error(`${stage} deadline`)), milliseconds);
        })]);
    } finally { clearTimeout(timer); }
};
function workersOf(pid) {
    if (process.platform === 'win32') {
        const command = `ConvertTo-Json -Compress -InputObject @(Get-CimInstance Win32_Process -Filter 'ParentProcessId=${pid}' | Where-Object Name -eq 'capsid-worker.exe' | Select-Object ProcessId,ExecutablePath)`;
        return JSON.parse(execFileSync('powershell.exe', ['-NoProfile', '-NonInteractive', '-Command', command],
            {encoding: 'utf8', windowsHide: true, timeout: 6000}).trim() || '[]').map(item => {
            assert.equal(path.resolve(item.ExecutablePath).toLowerCase(), path.resolve(worker).toLowerCase(),
                'fault injection targets the requested Worker executable');
            return item.ProcessId;
        });
    }
    if (process.platform === 'linux') {
        return fs.readdirSync('/proc').filter(name => /^\d+$/.test(name)).flatMap(name => {
            try {
                const stat = fs.readFileSync(`/proc/${name}/stat`, 'utf8');
                const tail = stat.slice(stat.lastIndexOf(') ') + 2).split(' ');
                if (Number(tail[1]) !== pid || !stat.includes('(capsid-worker)')) return [];
                assert.equal(fs.realpathSync(`/proc/${name}/exe`), fs.realpathSync(worker),
                    'fault injection targets the requested Worker executable');
                return [Number(name)];
            } catch (error) { if (error.code === 'ENOENT' || error.code === 'ESRCH') return []; throw error; }
        });
    }
    return posixWorkerChildren(pid, worker);
}
function exists(pid) {
    try { process.kill(pid, 0); return true; }
    catch (error) { if (error.code === 'ESRCH') return false; throw error; }
}
async function reaped(pids) {
    const deadline = performance.now() + 6000;
    while (pids.some(exists) && performance.now() < deadline) await delay(10);
    assert.deepEqual(pids.filter(exists), [], 'actual Worker PIDs have been reaped');
}

const child = spawn(fixture, [worker, scenario], {stdio: ['pipe', 'pipe', 'pipe'], windowsHide: true});
let stderr = '', pending = '', childResult, primaryError, forced = false, inputError;
const stages = new Map(), waiters = new Map(), sockets = new Set();
child.stderr.setEncoding('utf8'); child.stderr.on('data', text => { stderr += text; });
child.stdin.on('error', error => { inputError ??= error; });
const rejectWaiters = error => { for (const {reject} of waiters.values()) reject(error); waiters.clear(); };
child.stdout.setEncoding('utf8'); child.stdout.on('data', text => {
    pending += text;
    while (pending.includes('\n')) {
        const end = pending.indexOf('\n'), line = pending.slice(0, end).trim();
        pending = pending.slice(end + 1);
        try {
            const record = JSON.parse(line);
            assert.equal(typeof record.stage, 'string');
            assert.ok(!stages.has(record.stage), `stage occurs once: ${record.stage}`);
            assert.notEqual(record.stage, 'fixture-error', `fixture assertion failed: ${stderr}`);
            stages.set(record.stage, record);
            waiters.get(record.stage)?.resolve(record); waiters.delete(record.stage);
        } catch (error) { primaryError ??= error; rejectWaiters(error); }
    }
});
const closed = new Promise((resolve, reject) => {
    child.once('error', error => { rejectWaiters(error); reject(error); });
    child.once('close', (code, signal) => {
        childResult = {code, signal};
        rejectWaiters(new Error(`fixture closed ${code}/${signal}: ${stderr}`));
        resolve(childResult);
    });
});
closed.catch(() => {});
const stage = name => within(new Promise((resolve, reject) => {
    if (primaryError) reject(primaryError);
    else if (stages.has(name)) resolve(stages.get(name));
    else if (childResult) reject(new Error(`fixture closed before ${name}: ${stderr}`));
    else waiters.set(name, {resolve, reject});
}), name);
const command = value => {
    if (inputError) throw inputError;
    assert.ok(!childResult && child.stdin.writable, `fixture input open for ${value}`);
    child.stdin.write(value + '\n');
};
let allWorkers = [], selectedPid, healthyPid, port, peer, peerTerminal, initialResponse, beforeExitBytes = 0;
const agents = new Set();
function observePeer(socket) {
    sockets.add(socket);
    let connected = false, bytes = 0;
    const errors = [];
    const ready = new Promise((resolve, reject) => {
        socket.once('connect', () => { connected = true; resolve(); });
        socket.once('error', reject);
    });
    const terminal = new Promise(resolve => {
        socket.on('data', chunk => { bytes += chunk.length; });
        socket.on('error', error => errors.push(error.code));
        socket.once('close', () => { sockets.delete(socket); resolve({connected, bytes, errors}); });
    });
    return {socket, ready, terminal, byteCount: () => bytes};
}
function connectPeer() { return observePeer(net.connect({host: '127.0.0.1', port})); }
try {
    await stage('selected-ready');
    const initial = workersOf(child.pid);
    assert.equal(initial.length, 1, 'one actual selected Worker before accept is armed');
    [selectedPid] = initial; allWorkers = initial;
    command('arm');
    ({port} = await stage('accept-armed'));
    assert.ok(Number.isInteger(port) && port > 0 && port <= 65535);
    allWorkers = workersOf(child.pid);
    assert.equal(allWorkers.length, 2); assert.ok(allWorkers.includes(selectedPid));
    [healthyPid] = allWorkers.filter(pid => pid !== selectedPid);
    if (scenario === 'before-adoption' || adopted || prePost) {
        if (keepalive) {
            const agent = new http.Agent({keepAlive: true, maxSockets: 1}); agents.add(agent);
            const freed = new Promise(resolve => agent.once('free', resolve));
            const challenge = randomUUID();
            initialResponse = await within(new Promise((resolve, reject) => {
                const request = http.get({hostname: '127.0.0.1', port,
                    path: `/@capsid/orders/healthy?challenge=${challenge}`, agent,
                    headers: {Host: 'public.example'}}, reply => {
                    let body = ''; reply.setEncoding('utf8'); reply.on('data', text => { body += text; });
                    reply.once('end', () => resolve({status: reply.statusCode, body,
                        httpVersion: reply.httpVersion, connection: reply.headers.connection ?? null}));
                    reply.once('error', reject);
                });
                request.once('socket', socket => { peer = observePeer(socket); peer.ready.catch(() => {}); });
                request.once('error', reject);
            }), 'actual selected Worker keep-alive response');
            assert.equal(initialResponse.status, 200); assert.equal(initialResponse.body, `healthy:${challenge}`);
            assert.equal(initialResponse.httpVersion, '1.1');
            assert.ok(!(initialResponse.connection ?? '').toLowerCase().split(',').map(value => value.trim()).includes('close'));
            assert.equal(await within(freed, 'keep-alive socket returned to agent'), peer.socket);
            beforeExitBytes = peer.byteCount();
            assert.ok(beforeExitBytes > Buffer.byteLength(initialResponse.body));
        } else peer = connectPeer();
        await within(peer.ready, 'TCP connected before Worker exit');
        if (prePost) {
            const paused = await stage('adoption-pre-post-paused');
            assert.equal(paused.workerAvailable, true);
            assert.ok(Number.isSafeInteger(paused.allocationBytes) && paused.allocationBytes > 0);
            assert.equal(paused.allocationBytes, paused.expectedAllocationBytes);
        } else await stage(adopted ? 'adoption-queued' : 'peer-held');
        if (adopted && !queued) {
            assert.deepEqual(await stage('adoption-handler-drained'), {stage: 'adoption-handler-drained', workerAvailable: true});
        }
        assert.equal(peer.socket.destroyed, false, 'accepted peer held at the explicit adoption barrier');
    }
    assert.ok(exists(selectedPid) && exists(healthyPid), 'both actual Workers alive at failure barrier');
    if (!prePost || scenario === 'pre-post-worker-exit') process.kill(selectedPid, 'SIGKILL');
    if (queued) await reaped([selectedPid]);
    command('worker-exit');
    if (queued) {
        await stage('worker-exit-pending');
        command('release');
        assert.deepEqual(await stage('adoption-handler-drained'),
            {stage: 'adoption-handler-drained', workerAvailable: true}, 'real adoption ran before owner observed exit');
    }
    await stage('selected-unavailable');
    if (prePost) {
        assert.deepEqual(await stage('selected-joined-before-post'),
            {stage: 'selected-joined-before-post', workerAvailable: false, reactorStopped: true});
        await reaped([selectedPid]);
        assert.equal(peer.socket.destroyed, false, 'paused producer still owns the actual accepted TCP peer');
        command('release');
        const returned = await stage('adoption-returned');
        assert.equal(typeof returned.accepted, 'boolean');
    }
    await reaped([selectedPid]);
    assert.ok(exists(healthyPid), 'healthy Worker survives selected Worker exit');
    if (scenario !== 'cancel-pending') {
        if (scenario === 'before-completion') {
            peer = connectPeer();
            await within(peer.ready, 'TCP connected after Worker exit');
        } else if (!adopted && !prePost) command('adopt');
        if (!adopted && !prePost) await stage('peer-refused');
        peerTerminal = await within(peer.terminal,
            prePost ? 'pre-post adoption peer terminal TCP event' : adopted ? 'adopted peer terminal TCP event' : 'refused peer terminal TCP event');
        if (prePost) assert.equal((await stage('adoption-returned')).accepted, false,
            'producer must refuse instead of posting to a reactor that already joined');
        assert.equal(peerTerminal.connected, true); assert.equal(peerTerminal.bytes, beforeExitBytes,
            'no extra response bytes after Worker exit');
        assert.ok(peerTerminal.errors.every(code => code === 'ECONNRESET'), 'only EOF or peer reset is accepted');
        const challenge = randomUUID();
        const response = await within(new Promise((resolve, reject) => {
            const request = http.get({hostname: '127.0.0.1', port,
                path: `/@capsid/orders/healthy?challenge=${challenge}`, agent: false,
                headers: {Host: 'public.example'}}, reply => {
                let body = ''; reply.setEncoding('utf8'); reply.on('data', text => { body += text; });
                reply.once('end', () => resolve({status: reply.statusCode, body})); reply.once('error', reject);
            });
            request.once('error', reject);
            request.once('socket', socket => { sockets.add(socket); socket.once('close', () => sockets.delete(socket)); });
        }), 'healthy successor real HTTP response');
        assert.deepEqual(response, {status: 200, body: `healthy:${challenge}`});
        await stage('healthy-adopted');
    }
    command('stop');
    if (scenario === 'cancel-pending') await stage('accept-cancelled');
    const stopped = await stage('stopped');
    assert.deepEqual(stopped, {stage: 'stopped', selectedCompletions: 1,
        cancelled: scenario === 'cancel-pending' ? 1 : 0,
        refused: scenario === 'cancel-pending' || adopted ? 0 : 1,
        queuedAdoptions: adopted ? 1 : 0,
        healthyAccepted: scenario === 'cancel-pending' ? 0 : 1});
    child.stdin.end();
    assert.deepEqual(await within(closed, 'fixture close including stdio'), {code: 0, signal: null});
    await reaped(allWorkers);
    await within(new Promise((resolve, reject) => {
        const probe = net.connect({host: '127.0.0.1', port});
        sockets.add(probe); probe.once('close', () => sockets.delete(probe));
        probe.once('connect', () => { probe.destroy(); reject(new Error('listener still open after stop')); });
        probe.once('error', error => error.code === 'ECONNREFUSED' ? resolve() : reject(error));
    }), 'released listener');
    if (primaryError) throw primaryError;
    if (inputError) throw inputError;
    assert.equal(pending, '', 'no partial terminal fixture record');
} catch (error) { primaryError ??= error; }
finally {
    for (const agent of agents) agent.destroy();
    for (const socket of sockets) socket.destroy();
    if (!childResult) {
        // EOF makes the fixture unwind and join both servers even when the
        // driver fails before the next command. Forced cleanup is a failure.
        child.stdin.end();
        try { await within(closed, 'fixture cleanup after driver failure'); }
        catch (error) {
            forced = true; child.kill('SIGKILL');
            try { await within(closed, 'forced fixture close'); } catch { /* retain primary and cleanup below */ }
            primaryError = new AggregateError([primaryError, error].filter(Boolean), 'scenario and fixture cleanup failed');
        }
    }
    try { await reaped(allWorkers); }
    catch (error) { primaryError = new AggregateError([primaryError, error].filter(Boolean), 'scenario and Worker cleanup failed'); }
}
const evidence = {complete: !primaryError && !forced, scenario, fixturePid: child.pid, selectedPid, healthyPid,
    actualWorkers: allWorkers, stages: [...stages.values()], initialResponse, beforeExitBytes,
    peerTerminal, result: childResult, forced, stderr};
console.log(JSON.stringify(evidence));
if (primaryError) throw primaryError;
assert.equal(forced, false, 'fixture must exit without forced termination');
