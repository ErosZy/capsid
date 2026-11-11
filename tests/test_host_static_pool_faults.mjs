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
assert.ok(['first-worker-exit', 'last-worker-exit', 'first-worker-exit-websocket',
    'all-workers-exit', 'last-ready-worker-exit', 'all-workers-exit-websocket'].includes(scenario));
const unavailable = ['all-workers-exit', 'last-ready-worker-exit', 'all-workers-exit-websocket'].includes(scenario);
const {WebSocket, identity: clientIdentity} = loadNodeWS(args.get('--node-package'));
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
    // Darwin ps has no -P parent filter. Select the actual PPID column;
    // retain its executable check before injecting any process failure.
    const ps = args => execFileSync('ps', args,
        {encoding: 'utf8', env: {...process.env, LC_ALL: 'C'}, timeout: 6000}).trim();
    return ps(['-axo', 'pid=,ppid=']).split(/\r?\n/)
        .map(line => line.trim().split(/\s+/).map(Number))
        .filter(([, parent]) => parent === pid).map(([child]) => {
            assert.ok(Number.isSafeInteger(child) && child > 0);
            const executable = ps(['-o', 'comm=', '-p', String(child)]);
            assert.equal(fs.realpathSync(executable), fs.realpathSync(worker));
            const started = Date.parse(ps(['-o', 'lstart=', '-p', String(child)]));
            assert.ok(Number.isFinite(started));
            return {pid: child, created: String(started)};
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

const child = spawn(fixture, [worker], {stdio: ['pipe', 'pipe', 'pipe'], windowsHide: true});
let stderr = '', pending = '', childResult, primaryError, forced = false, inputError;
const stages = new Map(), waiters = new Map(), sockets = new Set(), websockets = new Set();
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
            assert.ok(!stages.has(record.stage), 'unique fixture stage');
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
    assert.ok(!childResult && child.stdin.writable);
    child.stdin.write(value + '\n');
};
async function challengeRequest(port, label) {
    const challenge = randomUUID();
    let terminal, response;
    const reply = await within(new Promise((resolve, reject) => {
        const request = http.get({hostname: '127.0.0.1', port,
            path: `/@capsid/orders/healthy?challenge=${challenge}`, agent: false,
            headers: {Host: 'public.example', Connection: 'close'}}, message => {
            const chunks = [];
            message.on('data', chunk => chunks.push(chunk));
            message.once('end', () => resolve({status: message.statusCode, complete: message.complete,
                connection: message.headers.connection, body: Buffer.concat(chunks).toString('utf8')}));
            message.once('error', reject);
        });
        request.once('socket', socket => {
            sockets.add(socket);
            terminal = new Promise(resolveClose => socket.once('close', hadError => {
                sockets.delete(socket); resolveClose({hadError});
            }));
        });
        request.once('error', reject);
    }), label);
    assert.equal(reply.status, 200, `${label}: surviving Worker serves the first connection`);
    assert.equal(reply.complete, true); assert.equal(reply.connection?.toLowerCase(), 'close');
    response = JSON.parse(reply.body);
    assert.equal(response.challenge, challenge, 'real dynamic challenge response');
    assert.match(response.instance, /^[0-9a-f-]{36}$/);
    assert.deepEqual(await within(terminal, `${label}: terminal TCP event`), {hadError: false});
    return response;
}
async function echoClient(port, index) {
    const ws = new WebSocket(`ws://127.0.0.1:${port}/@capsid/orders/ws`, {headers: {Host: 'public.example'}});
    websockets.add(ws);
    let socketError;
    ws.on('error', error => { socketError ??= error; });
    const terminal = new Promise(resolve => ws.once('close', (code, reason) => {
        websockets.delete(ws); resolve({code, reason: reason.toString()});
    }));
    await within(new Promise((resolve, reject) => { ws.once('open', resolve); ws.once('error', reject); }), 'WebSocket open');
    let frames = 0, bytes = 0;
    for (const binary of [false, true]) for (const size of [0, 1, 125, 126, 65535, 65536]) {
        const payload = Buffer.alloc(size, 65 + index);
        const echoed = new Promise((resolve, reject) => {
            const onClose = () => { ws.off('message', onMessage); reject(new Error('WebSocket closed before echo')); };
            const onMessage = (data, isBinary) => { ws.off('close', onClose); resolve({data, isBinary}); };
            ws.once('close', onClose); ws.once('message', onMessage);
        });
        ws.send(payload, {binary});
        const result = await within(echoed, `WebSocket ${index} echo ${binary}/${size}`);
        assert.equal(result.isBinary, binary); assert.deepEqual(result.data, payload);
        ++frames; bytes += size;
    }
    ws.close(1000, 'done');
    assert.deepEqual(await within(terminal, 'WebSocket normal close'), {code: 1000, reason: 'done'});
    if (socketError) throw socketError;
    return {index, frames, bytes, closeCode: 1000};
}

async function unavailableRequest(port, method = 'GET', upgrade = false) {
    let terminal;
    const reply = await within(new Promise((resolve, reject) => {
        const request = http.request({hostname: '127.0.0.1', port, method,
            path: '/@capsid/orders/unavailable', agent: false,
            headers: {Host: 'public.example', Connection: upgrade ? 'Upgrade' : 'keep-alive',
                Origin: 'https://client.example', ...(upgrade ? {Upgrade: 'websocket',
                    'Sec-WebSocket-Version': '13', 'Sec-WebSocket-Key': 'dGhlIHNhbXBsZSBub25jZQ=='} : {})}}, message => {
            const chunks = [];
            message.on('data', chunk => chunks.push(chunk));
            message.once('end', () => resolve({status: message.statusCode, complete: message.complete,
                headers: message.headers, body: Buffer.concat(chunks).toString('utf8')}));
            message.once('error', reject);
        });
        request.once('socket', socket => {
            sockets.add(socket);
            terminal = new Promise(resolve => socket.once('close', hadError => {
                sockets.delete(socket); resolve({hadError});
            }));
        });
        request.once('upgrade', (_response, socket) => {
            socket.destroy(); reject(new Error('unavailable pool accepted a WebSocket upgrade'));
        });
        request.once('error', reject); request.end();
    }), `${method} unavailable response`);
    assert.equal(reply.status, 503, 'zero READY Workers must produce an HTTP 503');
    assert.equal(reply.complete, true);
    assert.equal(reply.headers.connection?.toLowerCase(), 'close');
    assert.equal(reply.headers['content-length'], String(Buffer.byteLength('worker unavailable')));
    assert.equal(reply.body, method === 'HEAD' ? '' : 'worker unavailable');
    assert.equal(reply.headers['access-control-allow-origin'], 'https://client.example');
    assert.equal(reply.headers['sec-websocket-accept'], undefined);
    assert.deepEqual(await within(terminal, 'unavailable response TCP close'), {hadError: false});
    return {method, upgrade, ...reply, tcpClosed: true};
}

async function pendingConnection(port) {
    const socket = net.connect({host: '127.0.0.1', port});
    sockets.add(socket);
    const chunks = [];
    let error;
    socket.on('data', chunk => chunks.push(chunk));
    socket.on('error', value => { error ??= value; });
    const closed = new Promise(resolve => socket.once('close', hadError => {
        sockets.delete(socket); resolve({hadError, error: error?.code, bytes: Buffer.concat(chunks)});
    }));
    await within(new Promise((resolve, reject) => {
        socket.once('connect', resolve); socket.once('error', reject);
    }), 'unavailable TCP connect');
    return {socket, chunks, closed};
}

async function fragmentedUnavailable(port) {
    const pending = await pendingConnection(port);
    pending.socket.write('GET /@capsid/orders/unavailable HTTP/1.1\r\nHost: public.example\r\nX-Probe: ');
    await delay(100);
    assert.equal(pending.socket.destroyed, false, 'partial head remains pending');
    assert.equal(pending.chunks.length, 0, 'no response before a complete HTTP head');
    pending.socket.write('complete\r\n\r\n');
    const result = await within(pending.closed, 'fragmented 503 complete TCP close');
    assert.equal(result.hadError, false);
    const wire = result.bytes.toString('utf8'), split = wire.indexOf('\r\n\r\n');
    assert.ok(split > 0); assert.match(wire.slice(0, split), /^HTTP\/1\.1 503 /);
    assert.match(wire.slice(0, split), /\r\nConnection: close/i);
    assert.equal(wire.slice(split + 4), 'worker unavailable');
    return {bytes: result.bytes.length, tcpClosed: true, noResponseBeforeHead: true};
}

async function unavailableLimit(port) {
    const held = await Promise.all(Array.from({length: 128}, () => pendingConnection(port)));
    for (const pending of held) pending.socket.write(
        'GET /@capsid/orders/unavailable HTTP/1.1\r\nHost: public.example\r\n');
    await delay(100);
    for (const pending of held) {
        assert.equal(pending.socket.destroyed, false, 'admitted incomplete head remains live');
        assert.equal(pending.chunks.length, 0);
    }
    const excess = await pendingConnection(port);
    const rejected = await within(excess.closed, 'unavailable exchange capacity is bounded');
    assert.equal(rejected.bytes.length, 0);
    assert.ok(!rejected.error || rejected.error === 'ECONNRESET');
    for (const pending of held) pending.socket.write('\r\n');
    for (const pending of held) {
        const terminal = await within(pending.closed, 'admitted exchange finishes after capacity rejection');
        assert.equal(terminal.hadError, false);
        const wire = terminal.bytes.toString('utf8'), split = wire.indexOf('\r\n\r\n');
        assert.ok(split > 0); assert.match(wire.slice(0, split), /^HTTP\/1\.1 503 /);
        assert.equal(wire.slice(split + 4), 'worker unavailable');
    }
    await unavailableRequest(port);
    return {admitted: held.length, rejected: 1, rejectedBytes: 0,
        completed503: held.length, capacityReused: true, excessError: rejected.error};
}

let actualWorkers = [], failedPid, healthyPid, port, firstResponse, afterResponse, echoes;
let unavailableResponses, fragmented, idleExpiry, pendingAtStop, pendingStopResult, capacityLimit;
try {
    const ready = await stage('pool-ready');
    assert.equal(ready.active, 2); port = ready.port;
    assert.ok(Number.isInteger(port) && port > 0 && port <= 65535);
    actualWorkers = workersOf(child.pid).sort((a, b) => BigInt(a.created) < BigInt(b.created) ? -1 :
        BigInt(a.created) > BigInt(b.created) ? 1 : a.pid - b.pid);
    assert.equal(actualWorkers.length, 2);
    assert.notEqual(actualWorkers[0].pid, actualWorkers[1].pid);
    // Windows creates shards serially and arms the initial accept on shard 0.
    // Strict OS creation times identify that target without a product hook,
    // a preliminary HTTP request, or a guessed process enumeration order.
    if (process.platform === 'win32') assert.ok(BigInt(actualWorkers[0].created) < BigInt(actualWorkers[1].created));
    if (unavailable && scenario !== 'last-ready-worker-exit') {
        for (const item of actualWorkers) process.kill(item.pid, 'SIGKILL');
    } else {
        failedPid = actualWorkers[scenario === 'last-worker-exit' ? 1 : 0].pid;
        healthyPid = actualWorkers.find(item => item.pid !== failedPid).pid;
        assert.ok(exists(failedPid) && exists(healthyPid));
        process.kill(failedPid, 'SIGKILL');
        command('await-one-exit');
        assert.deepEqual(await stage('one-worker-left'), {stage: 'one-worker-left', active: 1});
        await reaped([failedPid]); assert.ok(exists(healthyPid));
        // This is the first TCP client since activation: the initial pending
        // accept completes only after the selected Worker exit was processed.
        if (scenario === 'first-worker-exit-websocket') {
            echoes = await Promise.all(Array.from({length: 4}, (_, i) => echoClient(port, i)));
            firstResponse = await challengeRequest(port, 'HTTP after first post-exit WebSocket');
        } else {
            firstResponse = await challengeRequest(port, 'first post-exit HTTP request');
            echoes = await Promise.all(Array.from({length: 4}, (_, i) => echoClient(port, i)));
        }
        afterResponse = await challengeRequest(port, 'post-WebSocket HTTP request');
        assert.equal(afterResponse.instance, firstResponse.instance, 'same surviving Worker before and after concurrent IO');
        assert.ok(exists(healthyPid));
        if (scenario === 'last-ready-worker-exit') process.kill(healthyPid, 'SIGKILL');
    }
    if (unavailable) {
        command('await-all-exits');
        assert.deepEqual(await stage('no-workers-left'), {stage: 'no-workers-left', active: 0});
        await reaped(actualWorkers.map(item => item.pid));
        // For all-workers-exit* these are the first public connections since
        // READY, including the accept originally armed on the first shard.
        unavailableResponses = [await unavailableRequest(port, 'GET', scenario === 'all-workers-exit-websocket')];
        unavailableResponses.push(await unavailableRequest(port, 'HEAD'));
        unavailableResponses.push(...await Promise.all(Array.from({length: 8}, () => unavailableRequest(port))));
        fragmented = await fragmentedUnavailable(port);
        if (scenario === 'all-workers-exit-websocket') capacityLimit = await unavailableLimit(port);
        if (scenario === 'all-workers-exit') {
            const idle = await pendingConnection(port);
            const terminal = await within(idle.closed, 'bounded idle rejection lifetime');
            assert.equal(terminal.bytes.length, 0);
            // Beast's timeout closes the socket; Windows can report RST for
            // this idle connection. Completed HTTP replies above still
            // require a clean close and an intact body on every platform.
            assert.ok(!terminal.error || terminal.error === 'ECONNRESET');
            idleExpiry = {tcpClosed: true, bytes: 0, hadError: terminal.hadError, error: terminal.error};
        }
        pendingAtStop = await pendingConnection(port);
        pendingAtStop.socket.write('GET /@capsid/orders/unavailable HTTP/1.1\r\nHost: public.example\r\n');
        await delay(100);
        assert.equal(pendingAtStop.socket.destroyed, false, 'incomplete head is live before stop');
        assert.equal(pendingAtStop.chunks.length, 0);
    }
    command('stop');
    assert.deepEqual(await stage('stopped'), {stage: 'stopped', active: 0});
    child.stdin.end();
    assert.deepEqual(await within(closed, 'fixture close including stdio'), {code: 0, signal: null});
    await reaped(actualWorkers.map(item => item.pid));
    if (pendingAtStop) {
        const terminal = await within(pendingAtStop.closed, 'pool stop releases incomplete HTTP');
        assert.equal(terminal.bytes.length, 0);
        pendingStopResult = {tcpClosed: true, bytes: 0, hadError: terminal.hadError};
    }
    await within(new Promise((resolve, reject) => {
        const probe = net.connect({host: '127.0.0.1', port});
        sockets.add(probe); probe.once('close', () => sockets.delete(probe));
        probe.once('connect', () => { probe.destroy(); reject(new Error('listener survived pool wait')); });
        probe.once('error', error => error.code === 'ECONNREFUSED' ? resolve() : reject(error));
    }), 'released listener');
    if (primaryError) throw primaryError;
    if (inputError) throw inputError;
    assert.equal(pending, '');
} catch (error) { primaryError ??= error; }
finally {
    for (const ws of websockets) ws.terminate();
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
    try { await reaped(actualWorkers.map(item => item.pid)); }
    catch (error) { primaryError = new AggregateError([primaryError, error].filter(Boolean), 'scenario and Worker cleanup failed'); }
}
console.log(JSON.stringify({complete: !primaryError && !forced, scenario,
    firstConnection: scenario.endsWith('-websocket') ? 'websocket' : 'http', fixturePid: child.pid,
    actualWorkers, failedPid, healthyPid, stages: [...stages.values()], firstResponse, afterResponse,
    echoes, unavailableResponses, fragmented, idleExpiry, pendingStopResult, capacityLimit,
    clientIdentity, result: childResult, forced, stderr}));
if (primaryError) throw primaryError;
assert.equal(forced, false);
