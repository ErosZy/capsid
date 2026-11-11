import assert from 'node:assert/strict';
import {spawn, execFileSync} from 'node:child_process';
import fs from 'node:fs';
import http from 'node:http';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';
import {setTimeout as delay} from 'node:timers/promises';
import {pathToFileURL} from 'node:url';
import {randomUUID, randomBytes, createHash} from 'node:crypto';
import {loadNodeWS} from './helpers/ws_interop_node_sdk.mjs';

const options = new Map();
for (let i = 2; i < process.argv.length; i += 2) options.set(process.argv[i], process.argv[i + 1]);
const host = options.get('--host'), worker = options.get('--worker'), mode = options.get('--mode');
const scenario = options.get('--case') ?? 'idle';
assert.ok(host && worker && ['single-worker', 'static-pool'].includes(mode));
assert.ok(['idle', 'parent-eof', 'repeat-stop', 'active-ws', 'no-close-reply', 'pending-dial',
    'startup-fail', 'invalid-fd', 'ready-alias', 'stdio-fd', 'malformed-fd', 'file-fd', 'signal'].includes(scenario));
const invalid = ['invalid-fd', 'ready-alias', 'stdio-fd', 'malformed-fd', 'file-fd'].includes(scenario);
const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'capsid-parent-stop-'));
const bundle = path.join(dir, 'main.mjs');
const ownedSockets = new Set();
let child, peer, websocket, response, fileFd, primaryError, forced = false;
const within = async (promise, stage, milliseconds = 6000) => {
    let timer;
    try {
        return await Promise.race([promise, new Promise((_, reject) => {
            timer = setTimeout(() => reject(new Error(`${stage} deadline`)), milliseconds);
        })]);
    } finally { clearTimeout(timer); }
};
function getChildren(pid) {
    if (process.platform === 'win32') {
        const command = `ConvertTo-Json -Compress -InputObject @(Get-CimInstance Win32_Process -Filter 'ParentProcessId=${pid}' | Where-Object Name -eq 'capsid-worker.exe' | Select-Object ProcessId)`;
        const data = execFileSync('powershell.exe', ['-NoProfile', '-NonInteractive', '-Command', command], {encoding: 'utf8', windowsHide: true, timeout: 6000});
        return JSON.parse(data.trim() || '[]').map(item => item.ProcessId);
    }
    if (process.platform === 'linux') {
        return fs.readdirSync('/proc').filter(name => /^\d+$/.test(name)).flatMap(name => {
            try {
                const text = fs.readFileSync(`/proc/${name}/stat`, 'utf8');
                const tail = text.slice(text.lastIndexOf(') ') + 2).split(' ');
                return Number(tail[1]) === pid && text.includes('(capsid-worker)') ? [Number(name)] : [];
            } catch (error) { if (error.code === 'ENOENT' || error.code === 'ESRCH') return []; throw error; }
        });
    }
    return execFileSync('ps', ['-o', 'pid=', '-P', String(pid)], {encoding: 'utf8'}).trim().split(/\s+/).filter(Boolean).map(Number);
}
function exists(pid) {
    try { process.kill(pid, 0); return true; }
    catch (error) { if (error.code === 'ESRCH') return false; throw error; }
}
async function reaped(pids) {
    const deadline = performance.now() + 3000;
    while (pids.some(exists) && performance.now() < deadline) await delay(20);
    assert.deepEqual(pids.filter(exists), [], 'all actual Worker processes were reaped');
}
function request(port, target) {
    return new Promise((resolve, reject) => {
        const req = http.get({hostname: '127.0.0.1', port, path: target, agent: false}, resolve);
        req.once('error', reject);
    });
}
try {
    fs.writeFileSync(bundle, scenario === 'startup-fail' ? 'this is not valid javascript @@' : `
export default {fetch(request) {
  const url = new URL(request.url);
  if (url.pathname.endsWith('/ws')) {
    const pair = new WebSocketPair(); pair[1].accept(); pair[1].onmessage = e => pair[1].send(e.data);
    return new Response(null, {status: 101, webSocket: pair[0]});
  }
  if (url.pathname.endsWith('/dial')) {
    const ws = new WebSocket(url.searchParams.get('target'));
    return new Response(new ReadableStream({start(c) {
      c.enqueue(new TextEncoder().encode('constructed\\n'));
      ws.onclose = () => c.close();
    }}));
  }
  return new Response('parent-stop-healthy');
}};`);
    const config = path.join(dir, 'capsid.json');
    fs.writeFileSync(config, JSON.stringify({apiVersion: 'capsid/app-v1', entry: 'main.mjs',
        permissions: {modules: [], fetch: {allow: ['127.0.0.1']}},
        pool: {minReady: mode === 'static-pool' ? 2 : 1, maxWorkers: mode === 'static-pool' ? 2 : 1}}));
    const args = ['--mode', mode, '--worker', worker, '--source-bundle', bundle, '--source-name', pathToFileURL(bundle).href,
        '--application', 'orders', '--listen', '127.0.0.1:0', '--routing', 'path', '--public-scheme', 'http',
        '--public-authority', 'public.example', '--request-timeout-ms', '20000', '--strict-sandbox', 'off', '--ready-fd', '3',
        '--stop-fd', ({'invalid-fd':'99', 'ready-alias':'3', 'stdio-fd':'2', 'malformed-fd':'four'})[scenario] ?? '4', '--capsid-json', config];
    if (mode === 'static-pool') args.push('--workers', '2');
    if (scenario === 'file-fd') fileFd = fs.openSync(bundle, 'r');
    child = spawn(host, args, {stdio: ['ignore', 'pipe', 'pipe', 'pipe', fileFd ?? 'pipe'], env: {...process.env, CAPSID_HOST_IPC_METRICS: '1'}});
    let stderr = '', readyText = '';
    child.stderr.setEncoding('utf8'); child.stderr.on('data', bytes => { stderr += bytes; });
    child.stdio[4]?.on('error', error => { if (error.code !== 'EPIPE' && error.code !== 'ECONNRESET') primaryError ??= error; });
    const exited = new Promise((resolve, reject) => { child.once('error', reject); child.once('close', (code, signal) => resolve({code, signal})); });
    const ready = new Promise((resolve, reject) => {
        child.stdio[3].setEncoding('utf8'); child.stdio[3].on('data', bytes => {
            readyText += bytes;
            if (readyText.includes('\n')) { try { resolve(JSON.parse(readyText.split('\n')[0])); } catch (error) { reject(error); } }
        });
        child.once('exit', (code, signal) => reject(new Error(`Host before READY ${code}/${signal}: ${stderr}`)));
    });
    // Startup-failure cases consume the exit, and retain READY rejection as an
    // observation rather than an unhandled promise that masks their verdict.
    ready.catch(() => {});
    if (scenario === 'startup-fail' || invalid) {
        const result = await within(exited, 'startup failure with parent control still open');
        assert.notEqual(result.code, 0); assert.equal(result.signal, null); assert.equal(readyText, '');
        assert.match(stderr, invalid ? /stop-fd.*(?:descriptor|pipe|integer)/ : /bundle|worker|syntax|Syntax/);
        console.log(JSON.stringify({complete: true, mode, scenario, result}));
    } else {
        const record = await within(ready, 'Host READY', 15000);
        assert.equal(record.schema, 'capsid-host-ready-v1');
        const children = getChildren(child.pid);
        assert.equal(children.length, mode === 'static-pool' ? 2 : 1, 'actual Worker processes after READY');
        const health = await request(record.port, '/@capsid/orders/healthy');
        let text = ''; for await (const chunk of health) text += chunk;
        assert.equal(health.statusCode, 200); assert.equal(text, 'parent-stop-healthy');
        let terminal;
        if (scenario === 'active-ws') {
            const {WebSocket} = loadNodeWS();
            websocket = new WebSocket(`ws://127.0.0.1:${record.port}/@capsid/orders/ws`, {perMessageDeflate: false});
            await within(new Promise((resolve, reject) => { websocket.once('open', resolve); websocket.once('error', reject); }), 'WebSocket open');
            const challenge = randomUUID();
            const echo = new Promise((resolve, reject) => { websocket.once('message', (data, binary) => { try { assert.equal(binary, false); assert.equal(data.toString(), challenge); resolve(); } catch (e) { reject(e); } }); });
            websocket.send(challenge); await within(echo, 'WebSocket progress barrier');
            terminal = new Promise(resolve => websocket.once('close', (code, reason) => resolve({code, reason: reason.toString()})));
        } else if (scenario === 'no-close-reply') {
            // Independent raw client reads Close but deliberately never replies.
            const socket = net.connect(record.port, '127.0.0.1');
            ownedSockets.add(socket);
            socket.on('error', error => { primaryError ??= error; });
            const key = randomBytes(16).toString('base64');
            let bytes = Buffer.alloc(0), opened;
            const upgraded = new Promise(resolve => { opened = resolve; });
            let headerDone = false;
            socket.on('data', chunk => {
                bytes = Buffer.concat([bytes, chunk]);
                const end = bytes.indexOf('\r\n\r\n');
                if (!headerDone && end >= 0) {
                    try {
                        const head = bytes.subarray(0, end).toString();
                        assert.match(head, /^HTTP\/1\.1 101 /);
                        assert.ok(head.includes(createHash('sha1').update(key + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').digest('base64')));
                        bytes = bytes.subarray(end + 4); headerDone = true; opened();
                    } catch (error) { primaryError ??= error; opened(); }
                }
            });
            socket.once('connect', () => socket.write(`GET /@capsid/orders/ws HTTP/1.1\r\nHost: public.example\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: ${key}\r\n\r\n`));
            terminal = new Promise(resolve => socket.once('close', () => { ownedSockets.delete(socket); resolve(); }));
            await within(upgraded, 'raw WebSocket Upgrade');
            if (primaryError) throw primaryError;
            terminal = terminal.then(() => assert.deepEqual(bytes, Buffer.from([0x88, 2, 3, 0xe9]), 'exactly one Going Away Close before bounded teardown'));
        } else if (scenario === 'pending-dial') {
            let accepted;
            const connected = new Promise(resolve => { accepted = resolve; });
            peer = net.createServer(socket => {
                ownedSockets.add(socket); socket.once('close', () => ownedSockets.delete(socket));
                socket.on('error', error => { if (error.code !== 'ECONNRESET') primaryError ??= error; });
                let head = ''; socket.on('data', bytes => { head += bytes.toString(); if (head.includes('\r\n\r\n')) accepted(socket); });
            });
            await new Promise(resolve => peer.listen(0, '127.0.0.1', resolve));
            response = await request(record.port, '/@capsid/orders/dial?' + new URLSearchParams({target: `ws://127.0.0.1:${peer.address().port}/pending`}));
            response.on('error', error => { if (error.code !== 'ECONNRESET') primaryError ??= error; });
            const first = await within(new Promise(resolve => response.once('data', resolve)), 'constructor response body');
            assert.equal(first.toString(), 'constructed\n');
            const socket = await within(connected, 'actual pending Upgrade request');
            terminal = new Promise(resolve => socket.once('close', resolve));
        }
        terminal?.catch(() => {});
        console.log(JSON.stringify({stage: 'stop-request', mode, scenario, hostPID: child.pid, workerPIDs: children}));
        if (scenario === 'parent-eof') child.stdio[4].destroy();
        else if (scenario === 'signal') child.kill('SIGTERM');
        else child.stdio[4].write(Buffer.alloc(scenario === 'repeat-stop' ? 3 : 1, 1));
        const result = await within(exited, 'normal Host exit');
        assert.deepEqual(result, {code: 0, signal: null}, stderr);
        if (terminal) {
            const state = await within(terminal, 'active transport terminal');
            if (scenario === 'active-ws') assert.equal(state.code, 1001, 'Host shutdown sends Going Away Close');
        }
        await reaped(children);
        assert.doesNotMatch(stderr, /ERROR: (?:AddressSanitizer|LeakSanitizer)|runtime error:/);
        if (primaryError) throw primaryError;
        console.log(JSON.stringify({complete: true, mode, scenario, result, workerPIDs: children, workersReaped: true}));
    }
} catch (error) { primaryError = error; }
finally {
    if (child && child.exitCode === null && child.signalCode === null) {
        forced = true; child.kill('SIGKILL'); await new Promise(resolve => child.once('close', resolve));
    }
    response?.destroy(); websocket?.terminate();
    if (fileFd !== undefined) fs.closeSync(fileFd);
    for (const socket of ownedSockets) socket.destroy();
    if (peer?.listening) await new Promise(resolve => peer.close(resolve));
    assert.equal(path.dirname(dir), path.resolve(os.tmpdir()));
    assert.ok(path.basename(dir).startsWith('capsid-parent-stop-')); fs.rmSync(dir, {recursive: true, force: true});
}
if (primaryError) { console.error(primaryError, {forcedCleanup: forced}); process.exitCode = 1; }
