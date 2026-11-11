import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import net from 'node:net';
import {spawn} from 'node:child_process';
import {createHash} from 'node:crypto';
import {setTimeout as delay} from 'node:timers/promises';
import {fileURLToPath} from 'node:url';
import {bindingFetchKinds, bindingFetchViewGroups, bindingFetchPacket, bindingFetchInputPaths} from '../tools/binding-fetch-body-input.mjs';
import {assertBindingFetchCase, validateBindingFetchReport} from '../tools/binding-fetch-body-contract.mjs';

const root = fileURLToPath(new URL('../', import.meta.url));
const hash = file => createHash('sha256').update(fs.readFileSync(file)).digest('hex');
const args = new Map();
for (let index = 2; index < process.argv.length; index += 2) {
    const name = process.argv[index], value = process.argv[index+1];
    assert.ok(['--helper', '--worker', '--kind'].includes(name) && value && !args.has(name)); args.set(name, value);
}
assert.equal(args.size, 3);
const helper = args.get('--helper'), worker = args.get('--worker'), kind = args.get('--kind'); assert.ok(bindingFetchKinds.includes(kind));
const binding = path.join(root, bindingFetchInputPaths.binding), user = path.join(root, bindingFetchInputPaths.user);
const expected = {runId: process.env.CAPSID_WS_RUN_ID ?? 'standalone-binding-fetch', kind,
    inputs: Object.fromEntries(Object.entries(bindingFetchInputPaths).map(([key, name]) => [key, hash(path.join(root, name))])),
    binaries: {helper: hash(helper), worker: hash(worker)}};
const report = {schema: 'capsid-binding-fetch-body-v1', complete: false, runId: expected.runId, kind, inputs: expected.inputs, binaries: expected.binaries, cases: []};
let destination;
if (process.env.CAPSID_WS_REPORT_DIR) {
    fs.mkdirSync(process.env.CAPSID_WS_REPORT_DIR, {recursive: true}); destination = path.join(process.env.CAPSID_WS_REPORT_DIR, 'binding-fetch-body-'+kind+'.json');
    assert.equal(fs.existsSync(destination), false, 'fresh actual observations only');
}
const save = () => {if (destination) fs.writeFileSync(destination, JSON.stringify(report, null, 2)+'\n');};
save();
for (const [group, views] of bindingFetchViewGroups.entries()) {
    const row = {group, views}; report.cases.push(row);
    const records = [], sockets = new Map(), packet = bindingFetchPacket(kind); let sequence = 0;
    const server = net.createServer({allowHalfOpen: true}, socket => {
        const item = {request: null, rawRequest: '', peerEnded: false, peerReset: false, fixtureEnded: false, fixtureDestroyed: false, events: []};
        records.push(item); sockets.set(socket, item); let input = Buffer.alloc(0);
        const event = name => item.events.push({name, sequence: sequence++});
        socket.on('error', error => {item.peerReset = error.code === 'ECONNRESET'; item.error = error.code;});
        socket.on('end', () => {item.peerEnded = true; event('peer-ended');}); socket.on('close', () => sockets.delete(socket));
        socket.on('data', chunk => {
            input = Buffer.concat([input, chunk]); item.rawRequest = input.toString('base64'); if (item.request !== null) return;
            const end = input.indexOf('\r\n\r\n'); if (end < 0) return;
            item.request = input.subarray(0, end).toString(); event('request'); item.sent = packet.toString('base64');
            item.fixtureEnded = true; event('server-complete'); socket.end(packet);
        });
    });
    await new Promise((resolve, reject) => {server.once('error', reject); server.listen(0, '127.0.0.1', resolve);});
    row.port = server.address().port;
    const child = spawn(helper, [worker, binding, user, 'http://127.0.0.1:'+row.port, views.join(',')], {stdio: ['pipe', 'pipe', 'pipe']});
    let stdout = '', stderr = '', pending = '', bodySeen = false, acknowledged = false, resolveBody, rejectBody, timer;
    const bodyReady = new Promise((resolve, reject) => {resolveBody = resolve; rejectBody = reject;});
    child.stdin.on('error', error => {row.controlError = error.code;});
    child.stdout.setEncoding('utf8'); child.stdout.on('data', chunk => {
        stdout += chunk; pending += chunk;
        for (;;) {
            const end = pending.indexOf('\n'); if (end < 0) break;
            const line = pending.slice(0, end).replace(/\r$/, ''); pending = pending.slice(end+1);
            if (line.startsWith('BINDING_BODY ')) {
                try {assert.equal(bodySeen, false); bodySeen = true; row.state = JSON.parse(line.slice(13)); resolveBody();} catch (error) {rejectBody(error);}
            } else if (line.startsWith('BINDING_EXIT ')) {
                try {assert.equal(row.workerExit, undefined); row.workerExit = JSON.parse(line.slice(13));} catch (error) {rejectBody(error);}
            }
        }
    });
    child.stderr.on('data', chunk => {stderr += chunk;});
    const closed = new Promise((resolve, reject) => {child.once('close', (code, signal) => resolve({code, signal})); child.once('error', reject);});
    const deadline = new Promise((_, reject) => {timer = setTimeout(() => reject(new Error('Binding helper original45s absolute deadline')), 45000);});
    let failure;
    try {
        await Promise.race([bodyReady, closed.then(() => {assert.ok(bodySeen, 'actual Binding result before helper exit');}), deadline]);
        const end = performance.now()+3000;
        while (!records.every(source => source.peerEnded) && performance.now() < end) await delay(5);
        row.sourcesBeforeFinish = structuredClone(records); row.sampleSequence = sequence++;
        child.stdin.end('finish\n'); acknowledged = true; row.child = await Promise.race([closed, deadline]);
    } catch (error) {
        failure = error; row.collectionFailure = String(error.stack ?? error);
        if (!acknowledged && !child.stdin.destroyed) child.stdin.end('finish\n');
        if (child.exitCode === null && child.signalCode === null) {row.helperForced = true; child.kill();}
        row.child ??= await Promise.race([closed.catch(() => null), delay(2000).then(() => null)]);
    } finally {
        clearTimeout(timer); row.stdout = stdout; row.stderr = stderr; row.finalSources = structuredClone(records);
        for (const [socket, source] of sockets) {source.fixtureDestroyed = true; socket.destroy();}
        await new Promise(resolve => server.close(resolve)); save();
        console.log('BINDING_FETCH_ROW '+JSON.stringify(row));
    }
    if (failure) throw failure;
    assertBindingFetchCase(row, kind, group);
}
assert.equal(hash(helper), expected.binaries.helper); assert.equal(hash(worker), expected.binaries.worker);
assert.deepEqual(Object.fromEntries(Object.entries(bindingFetchInputPaths).map(([key, name]) => [key, hash(path.join(root, name))])), expected.inputs);
report.complete = true; save(); validateBindingFetchReport(report, expected);
console.log(JSON.stringify({complete: true, kind, groups: 4, bodyViewInputs: 130, actualRowObservations: 264, producerEndedBeforeFinish: true, actualCleanExit: true}));
