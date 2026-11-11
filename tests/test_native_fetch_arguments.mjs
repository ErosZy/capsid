import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import net from 'node:net';
import {spawn} from 'node:child_process';
import {createHash} from 'node:crypto';
import {fileURLToPath} from 'node:url';
import {setTimeout as delay} from 'node:timers/promises';
import {nativeFetchArgumentKinds, nativeFetchArgumentRoutes, nativeFetchArgumentViews, nativeFetchArgumentMethods,
    nativeFetchArgumentInputPaths, nativeFetchArgumentPacket, nativeFetchArgumentPrimary,
    assertNativeFetchArgumentCase, validateNativeFetchArgumentReport} from '../tools/native-fetch-arguments-contract.mjs';

const root = fileURLToPath(new URL('../', import.meta.url));
const sha = file => createHash('sha256').update(fs.readFileSync(file)).digest('hex');
const args = new Map();
for (let index = 2; index < process.argv.length; index += 2) {
    const name = process.argv[index], value = process.argv[index + 1];
    assert.ok(['--helper', '--worker', '--kind', '--route'].includes(name) && value && !args.has(name));
    args.set(name, value);
}
assert.equal(args.size, 4);
const helper = args.get('--helper'), worker = args.get('--worker');
const bodyKind = args.get('--kind'), route = args.get('--route');
assert.ok(nativeFetchArgumentKinds.includes(bodyKind) && nativeFetchArgumentRoutes.includes(route));
const fixture = path.join(root, nativeFetchArgumentInputPaths.fixture);
const inputs = () => Object.fromEntries(Object.entries(nativeFetchArgumentInputPaths).map(([key, name]) => [key, sha(path.join(root, name))]));
const expected = {runId: process.env.CAPSID_WS_RUN_ID ?? 'standalone-native-fetch-arguments', bodyKind, route,
    referencePlatform: process.platform === 'win32' ? 'native' : 'linux', inputs: inputs(), binaries: {helper: sha(helper), worker: sha(worker)}};
const report = {schema: 'capsid-native-fetch-arguments-v1', complete: false, ...expected, cases: []};
let reportFile;
if (process.env.CAPSID_WS_REPORT_DIR) {
    fs.mkdirSync(process.env.CAPSID_WS_REPORT_DIR, {recursive: true});
    reportFile = path.join(process.env.CAPSID_WS_REPORT_DIR, 'native-fetch-arguments-' + bodyKind + '-' + route + '.json');
    assert.equal(fs.existsSync(reportFile), false);
}
const save = () => {if (reportFile) fs.writeFileSync(reportFile, JSON.stringify(report, null, 2) + '\n');};
save();
const routeStart = performance.now();
for (const view of nativeFetchArgumentViews) for (const method of nativeFetchArgumentMethods) {
    const bytes = nativeFetchArgumentPacket(bodyKind), records = [], sockets = new Map();
    let sequence = 0;
    const accept = socket => {
        const row = {rawRequest: null, localAddress: socket.localAddress, remoteAddress: socket.remoteAddress,
            peerEnded: false, peerReset: false, fixtureEnded: false, fixtureDestroyed: false, events: []};
        records.push(row); sockets.set(socket, row);
        let incoming = Buffer.alloc(0);
        const event = name => row.events.push({name, sequence: sequence++});
        socket.on('error', error => {row.peerReset = error.code === 'ECONNRESET'; row.error = error.code;});
        socket.on('end', () => {row.peerEnded = true; event('peer-ended');});
        socket.on('close', () => sockets.delete(socket));
        socket.on('data', chunk => {
            incoming = Buffer.concat([incoming, chunk]);
            if (row.rawRequest !== null || !incoming.includes('\r\n\r\n')) return;
            row.rawRequest = incoming.toString('base64'); event('request');
            row.fixtureEnded = true; event('source-complete'); socket.end(bytes);
        });
    };
    const ipv4 = net.createServer({allowHalfOpen: true}, accept), ipv6 = net.createServer({allowHalfOpen: true}, accept);
    await new Promise((resolve, reject) => {ipv4.once('error', reject); ipv4.listen(0, '127.0.0.1', resolve);});
    const port = ipv4.address().port;
    try {
        await new Promise((resolve, reject) => {ipv6.once('error', reject); ipv6.listen({host: '::1', port, ipv6Only: true}, resolve);});
    } catch (error) {await new Promise(resolve => ipv4.close(resolve)); throw error;}
    const primary = nativeFetchArgumentPrimary(expected.referencePlatform, bodyKind, route, view, method);
    const item = {bodyKind, route, view, method, port,
        primary: {archiveSHA256: primary.archiveSHA256, reportSHA256: primary.reportSHA256},
        packetSHA256: createHash('sha256').update(bytes).digest('hex')};
    report.cases.push(item);
    const child = spawn(helper, [worker, fixture, 'http://localhost:' + port + '/body', '0',
        'unused&selection=' + [route, view, method].join(',')], {stdio: ['pipe', 'pipe', 'pipe']});
    const closed = new Promise((resolve, reject) => {child.once('error', reject); child.once('close', (code, signal) => resolve({code, signal}));});
    let stdout = '', stderr = '', pending = '', resolved = false, resolveBody, rejectBody, timer, failure;
    const body = new Promise((resolve, reject) => {resolveBody = resolve; rejectBody = reject;});
    const deadline = new Promise((_, reject) => {timer = setTimeout(() => reject(new Error('original45s absolute helper collection ceiling')), 45000);});
    child.stdin.on('error', error => {item.controlError = String(error);});
    child.stdout.setEncoding('utf8'); child.stderr.setEncoding('utf8');
    child.stderr.on('data', chunk => {stderr += chunk;});
    child.stdout.on('data', chunk => {
        stdout += chunk; pending += chunk;
        for (;;) {
            const end = pending.indexOf('\n'); if (end < 0) break;
            const line = pending.slice(0, end).replace(/\r$/, ''); pending = pending.slice(end + 1);
            try {
                if (line.startsWith('LIMIT_PROBE ')) {assert.equal(resolved, false); resolved = true; item.state = JSON.parse(line.slice(12)); resolveBody();}
                else if (line.startsWith('LIMIT_EXIT ')) {assert.equal(item.workerExit, undefined); item.workerExit = JSON.parse(line.slice(11));}
            } catch (error) {rejectBody(error);}
        }
    });
    try {
        await Promise.race([body, closed.then(() => {assert.ok(resolved, 'native body report before exit: ' + stderr);}), deadline]);
        const end = performance.now() + 3000;
        while (!records.every(row => row.peerEnded) && performance.now() < end) await delay(5);
        item.sourcesBeforeStop = structuredClone(records);
        assert.ok(records.length > 0 && records.every(row => row.peerEnded), 'actual sources must end before native helper stop');
        child.stdin.end('finish\n'); item.child = await Promise.race([closed, deadline]);
        assert.deepEqual(item.child, {code: 0, signal: null}, stderr);
    } catch (error) {failure = error; item.collectionFailure = String(error.stack ?? error);}
    finally {
        clearTimeout(timer);
        if (child.exitCode === null && child.signalCode === null) {item.helperForced = true; child.kill(); await closed;}
        item.stdout = stdout; item.stderr = stderr; item.finalSources = structuredClone(records); save();
        for (const [socket, row] of sockets) {row.fixtureDestroyed = true; socket.destroy();}
        await Promise.all([ipv4, ipv6].map(server => new Promise(resolve => server.close(resolve))));
    }
    if (failure) throw failure;
    assertNativeFetchArgumentCase(item, expected.referencePlatform, bodyKind, route, view, method);
    assert.ok(performance.now() - routeStart < 120000, 'original120s complete route budget');
}
assert.deepEqual(inputs(), expected.inputs);
assert.deepEqual({helper: sha(helper), worker: sha(worker)}, expected.binaries);
report.complete = true; save();
const checked = validateNativeFetchArgumentReport(report, expected);
console.log(JSON.stringify({...checked, bodyKind, route, sourcesEndedBeforeStop: true, actualCleanExit: true}));
