import assert from 'node:assert/strict';
import crypto from 'node:crypto';
import fs from 'node:fs';
import http from 'node:http';
import path from 'node:path';
import {pathToFileURL, fileURLToPath} from 'node:url';
import {setTimeout as delay} from 'node:timers/promises';

// Keep one managed Host alive across real inbound/outbound messages, pending
// handshakes, generation changes and worker replacement. This is a functional
// resource/lifetime soak; it does not qualify native performance or density.
const args = new Map();
for (let i = 2; i < process.argv.length; i += 2) {
    assert.ok(process.argv[i].startsWith('--') && process.argv[i + 1] && !args.has(process.argv[i]), 'unique option/value pairs');
    assert.ok(['--source-root', '--host', '--worker', '--runtime-dir', '--output', '--seconds', '--min-rounds', '--memory-mode', '--control-retain-channel'].includes(process.argv[i]), 'known option');
    args.set(process.argv[i], process.argv[i + 1]);
}
for (const name of ['--host', '--worker', '--runtime-dir', '--output']) assert.ok(args.get(name), 'required ' + name);
const root = path.resolve(args.get('--source-root') ?? path.join(path.dirname(fileURLToPath(import.meta.url)), '..'));
const host = path.resolve(args.get('--host')), worker = path.resolve(args.get('--worker'));
const directory = path.resolve(args.get('--runtime-dir')), output = path.resolve(args.get('--output'));
const duration = Number(args.get('--seconds') ?? 1800), minimumRounds = Number(args.get('--min-rounds') ?? 200);
const memoryMode = args.get('--memory-mode') ?? 'production';
assert.ok(Number.isFinite(duration) && duration > 0 && Number.isInteger(minimumRounds) && minimumRounds > 0);
assert.ok(['production', 'sanitizer'].includes(memoryMode));
assert.equal(process.platform, 'linux', 'this runner samples Linux /proc; no Windows or formal native-performance claim');
assert.ok(!fs.existsSync(directory), 'fresh runtime directory required');
fs.mkdirSync(directory, {mode: 0o700});
fs.mkdirSync(output, {recursive: true});
assert.ok(!fs.existsSync(path.join(output, 'result.json')), 'fresh evidence files required');
const {startCapsid, stopCapsid} = await import(pathToFileURL(path.join(root, 'tests/helpers/ws_host_backend.mjs')));
const {loadNodeWS} = await import(pathToFileURL(path.join(root, 'tests/helpers/ws_interop_node_sdk.mjs')));
const {WebSocket, WebSocketServer, identity} = loadNodeWS();
const hash = filename => crypto.createHash('sha256').update(fs.readFileSync(filename)).digest('hex');
const inputs = Object.fromEntries([fileURLToPath(import.meta.url), host, worker, path.join(root, 'tests/helpers/ws_host_backend.mjs'), path.join(root, 'tests/helpers/ws_interop_node_sdk.mjs')].map(file => [file, hash(file)]));
const result = {schema: 'capsid-managed-websocket-soak-v1', status: 'running', complete: false,
    durationSecondsRequired: duration, minimumRoundsRequired: minimumRounds, memoryMode,
    hostSHA256: inputs[host], workerSHA256: inputs[worker], inputs, client: identity,
    formalPerformanceEvidence: false, fullV030GoalComplete: false,
    rounds: 0, echoes: 0, outboundEchoes: 0, pendingDialCancellations: 0, deployments: 0,
    workerReplacements: 0, queueRefusals: 0, pausedReaders: 0, elapsedSeconds: 0,
    deliberateRetainedChannelControl: args.get('--control-retain-channel') === '1'};
const save = () => fs.writeFileSync(path.join(output, 'result.json'), JSON.stringify(result, null, 2) + '\n');
const sampleFile = path.join(output, 'samples.jsonl');
const clients = new Set(), peerConnections = new Set(), heldHandshakes = new Set();
let backend, server, peerServer, currentVersion = 'v1', baseline, maximumRss = 0;

async function until(read, condition, label, timeout = 6000) {
    const deadline = Date.now() + timeout; let value;
    do { value = await read(); if (condition(value)) return value; await delay(15); } while (Date.now() < deadline);
    assert.fail(label + ': ' + JSON.stringify(value));
}
async function within(promise, label, milliseconds = 6000) {
    let timer;
    try { return await Promise.race([promise, new Promise((_, reject) => { timer = setTimeout(() => reject(new Error(label)), milliseconds); })]); }
    finally { clearTimeout(timer); }
}
function snapshot() {
    const base = '/proc/' + backend.child.pid;
    const status = fs.readFileSync(base + '/status', 'utf8');
    return {rssKiB: Number(/^VmRSS:\s+(\d+) kB$/m.exec(status)?.[1]),
        threads: fs.readdirSync(base + '/task').length, descriptors: fs.readdirSync(base + '/fd').length};
}
function children() {
    const pids = new Set();
    for (const task of fs.readdirSync('/proc/' + backend.child.pid + '/task')) {
        try { for (const value of fs.readFileSync(`/proc/${backend.child.pid}/task/${task}/children`, 'utf8').trim().split(/\s+/)) if (value) pids.add(Number(value)); }
        catch (error) { if (error.code !== 'ENOENT') throw error; }
    }
    return [...pids];
}
async function metrics() {
    const response = await backend.admin('/metrics'); assert.equal(response.status, 200);
    const lines = response.text.split('\n'); const values = {};
    for (const [key, name] of [['channels', 'channel_reservations'], ['active', 'channel_active'], ['dials', 'channel_pending_dials'], ['queue', 'channel_queue_bytes'], ['http', 'channel_http_inflight']]) {
        const line = lines.find(line => line.startsWith('capsid_' + name + '{scope="service",'));
        assert.ok(line, 'required service metric: ' + name); values[key] = Number(line.slice(line.lastIndexOf(' ') + 1));
    }
    values.drains = lines.filter(line => line.startsWith('capsid_worker_events_total{event="generation_drained",app="orders",'))
        .reduce((sum, line) => sum + Number(line.slice(line.lastIndexOf(' ') + 1)), 0);
    for (const [key, name] of [['queueRejected', 'channel_budget_rejections_total'], ['queueHighWater', 'channel_budget_high_water']]) {
        const matches = lines.filter(line => line.startsWith('capsid_' + name + '{') && line.includes('scope="service"') && line.includes('resource="queue_bytes"'));
        assert.equal(matches.length, 1, 'one service queue budget series');
        values[key] = Number(matches[0].slice(matches[0].lastIndexOf(' ') + 1));
    }
    assert.ok(values.channels <= 4 && values.dials <= 2 && values.queue <= 4096 && values.queueHighWater <= 4096, 'configured service resource bounds');
    return values;
}
async function idle() {
    return until(metrics, value => !value.channels && !value.active && !value.dials && !value.queue && !value.http,
        'all channel resources return');
}
async function request(target) {
    const response = await fetch(`http://127.0.0.1:${backend.port}/@capsid/orders${target}`, {signal: AbortSignal.timeout(6000)});
    assert.equal(response.status, 200, target); return response.text();
}
async function openClient(route = '/ws') {
    const ws = new WebSocket(`ws://127.0.0.1:${backend.port}/@capsid/orders${route}`, {perMessageDeflate: false});
    clients.add(ws); ws.on('close', () => clients.delete(ws)); ws.on('error', () => {});
    const queue = []; ws.on('message', (data, binary) => queue.push({data: Buffer.from(data), binary}));
    const closed = new Promise(resolve => ws.once('close', resolve));
    await within(new Promise((resolve, reject) => { ws.once('open', resolve); ws.once('error', reject); }), 'client opens');
    return {ws, queue, closed, next: () => until(() => queue.shift(), value => value !== undefined, 'client receives'),
        close: async () => { if (ws.readyState < 2) ws.close(1000); await within(closed, 'client closes'); }};
}
function program(version, target, held) {
    return `const version=${JSON.stringify(version)},target=${JSON.stringify(target)},held=${JSON.stringify(held)};
const outbound=new Set();let lastEcho=null;
function own(ws){outbound.add(ws);ws.onclose=()=>outbound.delete(ws);ws.onerror=()=>{};return ws;}
export default {async fetch(request){const url=new URL(request.url);
if(url.pathname.endsWith('/ws') || url.pathname.endsWith('/burst')){
 const pair=new WebSocketPair(),socket=pair[1];socket.accept();
// An accepted incoming endpoint retains its outgoing I/O lifetime until local close.
 socket.onclose=()=>socket.close(1000);
 socket.onmessage=e=>{if(e.data==='__pending__'){own(new WebSocket(held));socket.send(version+':pending');return;}socket.send(url.pathname.endsWith('/burst')?new Uint8Array(131072):version+':'+e.data);};
 return new Response(null,{status:101,webSocket:pair[0]});}
if(url.pathname.endsWith('/out')){const ws=own(new WebSocket(target));
 const token=url.searchParams.get('token');
 return await new Promise(resolve=>{ws.onopen=()=>ws.send(token);ws.onmessage=e=>{lastEcho=e.data;resolve(Response.json({echo:e.data}));};ws.onerror=()=>resolve(new Response('outbound error',{status:500}));});}
if(url.pathname.endsWith('/state'))return Response.json({version,outbound:outbound.size,lastEcho});
return new Response(version);}};\n`;
}
async function operate(target, body) {
    const response = await backend.admin(target, body); assert.equal(response.status, 202, response.text);
    const operation = await until(async () => {
        const row = await backend.admin('/v1/operations/' + response.json.operationId); assert.equal(row.status, 200); return row.json;
    }, row => ['active', 'failed'].includes(row.state), 'admin operation settles', 15000);
    assert.equal(operation.state, 'active', JSON.stringify(operation));
}
function versionFiles(version, source) {
    const dest = path.join(directory, 'apps/orders', version); fs.mkdirSync(dest, {recursive: true, mode: 0o700});
    fs.writeFileSync(path.join(dest, 'bundle.mjs'), source);
    fs.copyFileSync(path.join(directory, 'apps/orders/v1/capsid.json'), path.join(dest, 'capsid.json'));
}
save();
try {
    server = http.createServer(); peerServer = new WebSocketServer({noServer: true, perMessageDeflate: false});
    server.on('upgrade', (request, socket, head) => {
        socket.on('error', () => {});
        if (request.url === '/held') { assert.equal(head.length, 0); heldHandshakes.add(socket); socket.once('close', () => heldHandshakes.delete(socket)); socket.once('end', () => { result.handshakeTransportEofs = (result.handshakeTransportEofs ?? 0) + 1; socket.end(); }); socket.resume(); return; }
        peerServer.handleUpgrade(request, socket, head, ws => {
            peerConnections.add(ws); ws.on('error', () => {}); ws.on('close', () => peerConnections.delete(ws));
            ws.on('message', (bytes, binary) => ws.send(bytes, {binary}));
        });
    });
    await new Promise((resolve, reject) => { server.once('error', reject); server.listen(0, '127.0.0.1', resolve); });
    const port = server.address().port, target = `ws://127.0.0.1:${port}/echo`, held = `ws://127.0.0.1:${port}/held`;
    const bundle = path.join(directory, 'source.mjs'); fs.writeFileSync(bundle, program(currentVersion, target, held));
    backend = await startCapsid(host, worker, bundle, directory, 'managed', {
        permissions: {modules: [], fetch: {allow: [`127.0.0.1:${port}`]}}, fetchTargets: [`127.0.0.1:${port}`],
        requestTimeoutMs: 5000, hostCapacity: {workersTotal: 1, activationSurgeWorkers: 1},
        channelCapacity: {channelsTotal: 4, pendingDialsTotal: 2, channelQueueBytesTotal: '4KiB'}, channels: {dialTimeoutMs: 30000},
        recovery: {restartBackoff: {initial: '20ms', maximum: '100ms', jitter: '0%', stableReset: '60s'}}});
    result.hostPid = backend.child.pid; save();
    const started = Date.now(); let retained;
    while ((Date.now() - started) / 1000 < duration || result.rounds < minimumRounds) {
        const round = result.rounds, client = await openClient();
        for (let i = 0; i < 16; ++i) {
            const token = `${round}/${i}/${crypto.randomBytes(12).toString('hex')}`;
            client.ws.send(token); const message = await client.next(); assert.equal(message.binary, false);
            assert.equal(message.data.toString(), currentVersion + ':' + token, 'inbound echo integrity'); ++result.echoes;
        }
        const token = 'out/' + round + '/' + crypto.randomBytes(24).toString('hex');
        assert.deepEqual(JSON.parse(await request('/out?' + new URLSearchParams({token}))), {echo: token}, 'outbound echo integrity'); ++result.outboundEchoes;
        await until(metrics, value => value.channels === 2 && value.active === 2 && !value.dials, 'both real directions active');
        assert.equal(peerConnections.size, 1); for (const ws of peerConnections) ws.close(1000);
        await until(() => peerConnections.size, value => value === 0, 'outbound peer closes');
        await until(metrics, value => value.channels === 1 && !value.dials, 'outbound resource returns');
        client.ws.send('__pending__'); assert.equal((await client.next()).data.toString(), currentVersion + ':pending');
        await until(() => heldHandshakes.size, value => value === 1, 'real held handshake reached');
        await until(metrics, value => value.channels === 2 && value.dials === 1, 'pending dial owns capacity');
        if (round % 2 === 0) {
            const next = 'v' + (round + 2), drains = (await metrics()).drains;
            versionFiles(next, program(next, target, held)); await operate('/v1/deploy', {app: 'orders', version: next});
            assert.equal(await request('/version'), next, 'new generation routes');
            client.ws.send('old-generation'); assert.equal((await client.next()).data.toString(), currentVersion + ':old-generation');
            // The quiet old generation also owns a pending dial: closing the
            // inbound channel alone cannot retire it; cancel it through its
            // peer transport and verify the generation completes naturally.
            for (const socket of heldHandshakes) socket.destroy();
            await client.close(); await idle();
            await until(metrics, value => value.drains === drains + 1, 'old generation drain acknowledged');
            await until(children, pids => pids.length === 1, 'old worker reaped'); currentVersion = next; ++result.deployments;
        } else {
            const pids = children(); assert.equal(pids.length, 1); const pid = pids[0];
            const stat = fs.readFileSync(`/proc/${pid}/stat`, 'utf8');
            assert.equal(Number(stat.slice(stat.lastIndexOf(')') + 2).split(' ')[1]), backend.child.pid, 'only this host child is terminated');
            process.kill(pid, 'SIGKILL'); await within(client.closed, 'crashed worker channel closes');
            await until(children, rows => rows.length === 1 && rows[0] !== pid, 'one replacement worker');
            await until(async () => { try { return await request('/version'); } catch { return null; } }, value => value === currentVersion, 'replacement serves');
            await idle(); ++result.workerReplacements;
        }
        ++result.pendingDialCancellations;
        await until(() => heldHandshakes.size, value => value === 0, 'pending transport canceled');
        const rejectedBefore = (await metrics()).queueRejected;
        const burst = await openClient('/burst'); burst.ws._socket.pause(); ++result.pausedReaders;
        burst.ws.send('burst'); await delay(30); burst.ws._socket.resume();
        await within(burst.closed, 'oversized queued message is refused');
        assert.equal(burst.queue.length, 0, 'refused payload never delivered');
        const refused = await idle(); assert.ok(refused.queueRejected > rejectedBefore, 'real host queue refusal recorded'); ++result.queueRefusals;
        if (result.deliberateRetainedChannelControl && !retained) retained = await openClient();
        const empty = await idle(), measured = snapshot();
        assert.ok(measured.rssKiB > 0); maximumRss = Math.max(maximumRss, measured.rssKiB);
        if (round === 9) baseline = measured;
        if (baseline) {
            assert.ok(measured.descriptors <= baseline.descriptors + 4, 'host descriptor plateau');
            assert.ok(measured.threads <= baseline.threads + 2, 'host thread plateau');
            if (memoryMode === 'production') assert.ok(measured.rssKiB <= baseline.rssKiB + 65536, 'host production RSS plateau');
        }
        ++result.rounds; result.elapsedSeconds = (Date.now() - started) / 1000;
        fs.appendFileSync(sampleFile, JSON.stringify({round: result.rounds, elapsedSeconds: result.elapsedSeconds, resources: empty, process: measured}) + '\n');
        save(); if (round % 25 === 0) console.log(JSON.stringify({rounds: result.rounds, elapsedSeconds: result.elapsedSeconds}));
    }
    await idle(); assert.ok(result.elapsedSeconds >= duration && result.rounds >= minimumRounds);
    assert.ok(result.deployments > 0 && result.workerReplacements > 0 && result.outboundEchoes === result.rounds);
    result.maximumHostRssKiB = maximumRss; result.finalHostSample = snapshot(); result.finalResources = await metrics();
    result.status = 'passed';
} catch (error) {
    result.status = 'failed'; result.error = String(error.stack ?? error);
    if (backend) {
        result.failureSnapshot = {process: snapshot(), children: children()};

    }
}
finally {
    for (const ws of clients) ws.terminate(); for (const ws of peerConnections) ws.terminate(); for (const socket of heldHandshakes) socket.destroy();
    try {
        if (backend) {
            result.shutdown = await stopCapsid(backend.child); fs.writeFileSync(path.join(output, 'host.stderr.log'), backend.stderr());
            assert.deepEqual(result.shutdown, {code: 0, signal: null, forced: false}, 'host exits naturally');
            assert.doesNotMatch(backend.stderr(), /ERROR: AddressSanitizer|LeakSanitizer|runtime error:/);
        }
        if (peerServer) await new Promise(resolve => peerServer.close(resolve));
        if (server) await new Promise(resolve => server.close(resolve));
        assert.ok(Object.entries(inputs).every(([file, value]) => hash(file) === value), 'input binaries and helpers unchanged');
    } catch (error) { result.status = 'failed'; result.cleanupError = String(error.stack ?? error); }
    result.complete = true; save();
}
console.log(JSON.stringify({status: result.status, rounds: result.rounds, elapsedSeconds: result.elapsedSeconds, error: result.error}));
if (result.status !== 'passed') process.exitCode = 1;
