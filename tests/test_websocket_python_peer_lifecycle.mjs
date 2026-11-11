import assert from 'node:assert/strict';
import {spawn, execFileSync} from 'node:child_process';
import {createHash, randomUUID} from 'node:crypto';
import fs from 'node:fs';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import {loadNodeWS} from './helpers/ws_interop_node_sdk.mjs';

const args = new Map();
for (let i = 2; i < process.argv.length; i += 2) args.set(process.argv[i], process.argv[i + 1]);
const scenario = args.get('--case');
assert.ok(['idle', 'exchange', 'tls-reset', 'reset-session', 'close-reset'].includes(scenario));
const python = args.get('--python') ?? process.env.CAPSID_WS_INTEROP_PYTHON ?? 'python3';
if (args.has('--node-package')) process.env.CAPSID_WS_INTEROP_NODE_PACKAGE = args.get('--node-package');
const {WebSocket, identity: nodeIdentity} = loadNodeWS();
const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const program = path.join(root, 'tests/helpers/ws_interop_python.py');
const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'capsid-python-peer-lifecycle-'));
const digest = data => createHash('sha256').update(data).digest('hex');
const describe = (data, binary) => ({type: binary ? 'binary' : 'text', length: data.length, sha256: digest(data)});
const within = async (promise, stage, milliseconds = 5000) => {
    let timer;
    try {
        return await Promise.race([promise, new Promise((_, reject) => {
            timer = setTimeout(() => reject(new Error(`${stage} deadline`)), milliseconds);
        })]);
    } finally { clearTimeout(timer); }
};
function certificates() {
    const run = parameters => execFileSync(args.get('--openssl') ?? 'openssl', parameters, {stdio: 'pipe'});
    const certs = Object.fromEntries(['ca', 'caKey', 'key', 'cert', 'untrustedKey', 'untrustedCert']
        .map(name => [name, path.join(dir, name + '.pem')]));
    run(['req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-keyout', certs.caKey, '-out', certs.ca,
        '-days', '1', '-subj', '/CN=Capsid peer lifecycle CA']);
    const csr = path.join(dir, 'server.csr');
    run(['req', '-newkey', 'rsa:2048', '-nodes', '-keyout', certs.key, '-out', csr, '-subj', '/CN=localhost']);
    const ext = path.join(dir, 'extensions');
    fs.writeFileSync(ext, 'subjectAltName=DNS:localhost\nextendedKeyUsage=serverAuth\n');
    run(['x509', '-req', '-in', csr, '-CA', certs.ca, '-CAkey', certs.caKey, '-CAcreateserial',
        '-out', certs.cert, '-days', '1', '-extfile', ext]);
    run(['req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-keyout', certs.untrustedKey, '-out', certs.untrustedCert,
        '-days', '1', '-subj', '/CN=localhost', '-addext', 'subjectAltName=DNS:localhost']);
    return certs;
}
function startPeer(certs) {
    const child = spawn(python, ['-I', program, 'peer', '--cert', certs.cert, '--key', certs.key,
        '--untrusted-cert', certs.untrustedCert, '--untrusted-key', certs.untrustedKey], {stdio: ['pipe', 'pipe', 'pipe']});
    const queue = [], pending = [];
    let buffer = '', stderr = '', failure;
    const fail = error => { failure ??= error; for (const waiter of pending.splice(0)) waiter.reject(error); };
    const exited = new Promise(resolve => child.once('close', (code, signal) => {
        fail(new Error(`peer exited ${code}/${signal}: ${stderr}`));
        resolve({code, signal});
    }));
    child.once('error', fail);
    child.stdin.on('error', fail);
    child.stderr.setEncoding('utf8'); child.stderr.on('data', chunk => { stderr += chunk; });
    child.stdout.setEncoding('utf8'); child.stdout.on('data', chunk => {
        buffer += chunk;
        try {
            while (buffer.includes('\n')) {
                const end = buffer.indexOf('\n');
                const value = JSON.parse(buffer.slice(0, end)); buffer = buffer.slice(end + 1);
                const waiter = pending.shift();
                if (waiter) waiter.resolve(value); else queue.push(value);
            }
        } catch (error) { fail(error); }
    });
    return {child, exited, stderr: () => stderr,
        send: value => child.stdin.write(JSON.stringify(value) + '\n'),
        next: stage => within(queue.length ? Promise.resolve(queue.shift()) : failure ? Promise.reject(failure) :
            new Promise((resolve, reject) => pending.push({resolve, reject})), stage)};
}
function expectedMessages(challenge) {
    return [['challenge:' + challenge, false], ['hello\0界😀', false], [Buffer.alloc(0), true],
        [Buffer.from(Array.from({length: 180000}, (_, i) => (i * 17 + 29) % 251)), true],
        ['分片\0😀', false], [Buffer.from(Array.from({length: 60240}, (_, i) => i % 251)), true]]
        .map(([value, binary]) => describe(Buffer.from(value), binary));
}
async function exchange(uri, certs, name, challenge, resetOnClose = false) {
    let transport, reset = false;
    const ws = new WebSocket(uri + '/echo?' + new URLSearchParams({case: name, challenge}), ['chat'],
        {ca: fs.readFileSync(certs.ca), perMessageDeflate: false,
            ...(resetOnClose ? {createConnection: () => {
                transport = net.connect(Number(new URL(uri).port), '127.0.0.1'); return transport;
            }} : {})});
    const messages = [];
    try {
        await within(new Promise((resolve, reject) => {
            if (resetOnClose) ws.once('open', () => {
                // The SDK invokes close() after parsing the actual peer Close.
                // This fault client deliberately resets instead of replying;
                // observing CLOSING on raw data raced its automatic Close write.
                ws.close = (code, reason) => {
                    try {
                        assert.equal(code, 1000); assert.equal(reason.toString(), 'remote\0终');
                        assert.equal(reset, false); reset = true;
                        transport.resetAndDestroy();
                    } catch (error) { reject(error); }
                };
            });
            ws.on('message', (data, binary) => {
                messages.push(describe(data, binary));
                ws.send(data, {binary}, error => { if (error) reject(error); });
            });
            ws.once('error', reject);
            ws.once('close', (code, reason) => {
                try {
                    assert.equal(code, 1000); assert.equal(reason.toString(), 'remote\0终');
                    assert.deepEqual(messages, expectedMessages(challenge));
                    resolve();
                } catch (error) { reject(error); }
            });
        }), `${name} exact exchange`);
    } finally { if (ws.readyState !== WebSocket.CLOSED) ws.terminate(); }
    if (resetOnClose) assert.equal(reset, true, 'RST followed the actual server Close frame');
    return messages;
}
async function rejectTLS(port, certs, wrongHost, resetDuringHandshake = false) {
    // Observe a real TLS certificate rejection, then send TCP RST from the
    // bridge after the client closes. This exercises server transport cleanup
    // without inventing a successful WebSocket session or cancelling a task.
    const sockets = new Set(), errors = [];
    let accepted = 0, received = 0, reset = false, finish;
    const resetClosed = new Promise(resolve => { finish = resolve; });
    const server = net.createServer(downstream => {
        accepted++;
        const upstream = net.connect(port, '127.0.0.1');
        for (const socket of [downstream, upstream]) {
            sockets.add(socket); socket.on('close', () => sockets.delete(socket));
            socket.on('error', error => errors.push(error.code));
        }
        upstream.on('data', chunk => {
            received += chunk.length;
            if (resetDuringHandshake && !reset) {
                reset = true;
                upstream.resetAndDestroy();
                downstream.destroy();
            }
        });
        downstream.pipe(upstream).pipe(downstream);
        downstream.once('close', () => {
            if (!reset) { reset = true; upstream.resetAndDestroy(); }
        });
        upstream.once('close', finish);
    });
    try {
        await new Promise((resolve, reject) => { server.once('error', reject); server.listen(0, '127.0.0.1', resolve); });
        const address = wrongHost ? '127.0.0.1' : 'localhost';
        const ws = new WebSocket(`wss://${address}:${server.address().port}/`, ['chat'],
            {ca: fs.readFileSync(certs.ca), perMessageDeflate: false});
        let opened = false, failure;
        ws.on('open', () => { opened = true; ws.close(); });
        ws.on('error', error => { failure = error; });
        await within(new Promise(resolve => ws.once('close', resolve)), 'TLS rejection');
        assert.equal(opened, false); assert.ok(failure, 'TLS verification rejected the actual certificate');
        assert.equal(failure.code, resetDuringHandshake ? 'ECONNRESET' :
            wrongHost ? 'ERR_TLS_CERT_ALTNAME_INVALID' : 'DEPTH_ZERO_SELF_SIGNED_CERT');
        await within(resetClosed, 'TCP reset after TLS rejection');
        assert.equal(accepted, 1); assert.ok(received > 0); assert.equal(reset, true);
        assert.deepEqual(errors, [], 'the bridge itself completed without an IO error');
    } finally {
        for (const socket of sockets) socket.destroy();
        if (server.listening) await new Promise(resolve => server.close(resolve));
    }
}

async function resetSession(port, name, challenge) {
    let transport;
    const ws = new WebSocket(`ws://127.0.0.1:${port}/echo?` + new URLSearchParams({case: name, challenge}), ['chat'], {
        perMessageDeflate: false,
        createConnection: () => { transport = net.connect(port, '127.0.0.1'); return transport; },
    });
    try {
        await within(new Promise((resolve, reject) => {
            ws.once('error', reject);
            ws.once('message', (data, binary) => {
                try {
                    assert.equal(binary, false); assert.equal(data.toString(), 'challenge:' + challenge);
                    transport.resetAndDestroy(); resolve();
                } catch (error) { reject(error); }
            });
        }), 'active WebSocket reset');
        const code = await within(new Promise(resolve => ws.once('close', resolve)), 'reset client close');
        assert.equal(code, 1006);
    } finally { if (ws.readyState !== WebSocket.CLOSED) ws.terminate(); }
}

let peer, stopped = false;
try {
    const certs = certificates();
    peer = startPeer(certs);
    const ready = await peer.next('peer ready');
    assert.equal(ready.type, 'ready');
    console.log(JSON.stringify({stage: 'ready', scenario, python: ready.identity, node: nodeIdentity}));
    const expected = [];
    if (scenario !== 'idle') {
        for (const [scheme, port] of [['ws', ready.ports.ws], ['wss', ready.ports.wss]]) {
            const name = scenario + '/' + scheme, challenge = randomUUID();
            const messages = await exchange(`${scheme}://localhost:${port}`, certs, name, challenge);
            expected.push({name, messages});
        }
        console.log(JSON.stringify({stage: 'healthy-exchanges', scenario, sessions: expected.length}));
    }
    if (scenario === 'tls-reset') {
        for (let i = 0; i < 8; i++) await rejectTLS(i % 2 ? ready.ports.wss : ready.ports.untrusted, certs, i % 2 !== 0);
        console.log(JSON.stringify({stage: 'rejected-tls-with-tcp-reset', attempts: 8}));
        for (let i = 0; i < 8; i++) await rejectTLS(ready.ports.wss, certs, false, true);
        console.log(JSON.stringify({stage: 'reset-during-tls-handshake', attempts: 8}));
    }
    if (scenario === 'reset-session') {
        const name = 'intentional-session-reset';
        await resetSession(ready.ports.ws, name, randomUUID());
        expected.push({name, failure: true});
        console.log(JSON.stringify({stage: 'active-session-reset'}));
    }
    if (scenario === 'close-reset') {
        for (let i = 0; i < 32; i++) {
            const name = 'close-reset/' + i, challenge = randomUUID();
            const messages = await exchange(`ws://127.0.0.1:${ready.ports.ws}`, certs, name, challenge, true);
            expected.push({name, messages, resetOnClose: true});
        }
        console.log(JSON.stringify({stage: 'tcp-reset-after-close', sessions: 32}));
    }
    peer.send({op: 'snapshot'});
    const snapshot = await peer.next('peer snapshot');
    assert.equal(snapshot.type, 'snapshot'); assert.equal(snapshot.active, 0);
    assert.equal(snapshot.records.length, expected.length);
    for (const [i, record] of snapshot.records.entries()) {
        assert.equal(record.case, expected[i].name); assert.equal(record.protocol, 'chat');
        assert.equal(record.extensions, 0);
        if (expected[i].failure) {
            assert.equal(record.failure?.name, 'ConnectionClosedError');
            assert.deepEqual(record.messages, []); assert.equal(record.pingPong, 0);
            assert.equal(record.close, undefined);
            continue;
        }
        assert.equal(record.pingPong, 2);
        assert.deepEqual(record.messages, expected[i].messages);
        if (expected[i].resetOnClose) {
            assert.equal(record.failure?.name, 'ConnectionClosedError', JSON.stringify(record));
            assert.match(record.failure.message, /no close frame received/, 'RST client sent no Close reply');
            assert.equal(record.close, undefined);
        } else {
            assert.deepEqual(record.close, {code: 1000, reason: 'remote\0终', clean: true});
            assert.equal(record.failure, undefined);
        }
    }
    console.log(JSON.stringify({stage: 'snapshot', active: snapshot.active, sessions: snapshot.records.length}));
    peer.send({op: 'stop'});
    const terminal = await peer.next('peer stopped');
    assert.deepEqual(terminal, {...snapshot, type: 'stopped'});
    const failedSessions = snapshot.records.filter(record => record.failure).length;
    assert.deepEqual(await within(peer.exited, 'normal peer exit'), {code: failedSessions ? 1 : 0, signal: null});
    if (failedSessions) {
        assert.match(peer.stderr(), /AssertionError: peer completes without active or failed sessions/);
        assert.doesNotMatch(peer.stderr(), /Exception in callback|ResourceWarning|unclosed transport/);
    } else assert.equal(peer.stderr(), '', 'peer teardown produced no unhandled callback or session errors');
    stopped = true;
    console.log(`PASS Python peer ${scenario}: normal stop, exact sessions, no callback errors`);
} catch (error) {
    console.error(error, peer?.stderr()); process.exitCode = 1;
} finally {
    if (peer && !stopped && peer.child.exitCode === null && peer.child.signalCode === null) {
        peer.child.kill('SIGKILL'); await peer.exited;
    }
    assert.equal(path.dirname(dir), path.resolve(os.tmpdir()));
    assert.ok(path.basename(dir).startsWith('capsid-python-peer-lifecycle-'));
    fs.rmSync(dir, {recursive: true, force: true});
}
