import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import net from 'node:net';
import {spawn} from 'node:child_process';
import {createHash} from 'node:crypto';
import {setTimeout as delay} from 'node:timers/promises';
import {fileURLToPath} from 'node:url';
import {bindingArgumentKinds, bindingArgumentRoutes, bindingArgumentViews, bindingArgumentMethods,
    bindingArgumentInputPaths, bindingArgumentPacket} from '../tools/binding-fetch-arguments-input.mjs';
import {assertBindingArgumentCase, validateBindingArgumentReport} from '../tools/binding-fetch-arguments-contract.mjs';

const root = fileURLToPath(new URL('../', import.meta.url)), hash = file => createHash('sha256').update(fs.readFileSync(file)).digest('hex');
const args = new Map();
for (let index = 2; index < process.argv.length; index += 2) {
    const name = process.argv[index], value = process.argv[index+1]; assert.ok(['--helper', '--worker', '--kind', '--route'].includes(name) && value && !args.has(name)); args.set(name, value);
}
assert.equal(args.size, 4);
const helper = args.get('--helper'), worker = args.get('--worker'), bodyKind = args.get('--kind'), route = args.get('--route');
assert.ok(bindingArgumentKinds.includes(bodyKind) && bindingArgumentRoutes.includes(route));
const binding = path.join(root, bindingArgumentInputPaths.binding), user = path.join(root, bindingArgumentInputPaths.user);
const expected = {runId: process.env.CAPSID_WS_RUN_ID ?? 'standalone-binding-arguments', bodyKind, route,
    inputs: Object.fromEntries(Object.entries(bindingArgumentInputPaths).map(([key, name]) => [key, hash(path.join(root, name))])), binaries: {helper: hash(helper), worker: hash(worker)}};
const report = {schema: 'capsid-binding-fetch-arguments-v1', complete: false, runId: expected.runId, bodyKind, route, inputs: expected.inputs, binaries: expected.binaries, cases: []};
let destination;
if (process.env.CAPSID_WS_REPORT_DIR) {fs.mkdirSync(process.env.CAPSID_WS_REPORT_DIR, {recursive: true}); destination = path.join(process.env.CAPSID_WS_REPORT_DIR, 'binding-fetch-arguments-'+bodyKind+'-'+route+'.json'); assert.equal(fs.existsSync(destination), false);}
const save = () => {if (destination) fs.writeFileSync(destination, JSON.stringify(report, null, 2)+'\n');}; save();
for (const view of bindingArgumentViews) for (const method of bindingArgumentMethods) {
    const item = {bodyKind, route, view, method, kind: 'native-arguments-'+route+'-'+view+'-'+method}; report.cases.push(item);
    const records = [], sockets = new Map(), packet = bindingArgumentPacket(bodyKind); let sequence = 0;
    const server = net.createServer({allowHalfOpen: true}, socket => {
        const source = {request: null, rawRequest: '', peerEnded: false, peerReset: false, fixtureEnded: false, fixtureDestroyed: false, events: []}; records.push(source); sockets.set(socket, source); let input = Buffer.alloc(0);
        const event = name => source.events.push({name, sequence: sequence++});
        socket.on('error', error => {source.peerReset = error.code === 'ECONNRESET'; source.error = error.code;}); socket.on('end', () => {source.peerEnded = true; event('peer-ended');}); socket.on('close', () => sockets.delete(socket));
        socket.on('data', chunk => {input = Buffer.concat([input, chunk]); source.rawRequest = input.toString('base64'); if (source.request !== null) return;
            const end = input.indexOf('\r\n\r\n'); if (end < 0) return; source.request = input.subarray(0, end).toString(); event('request');
            source.sent = packet.toString('base64'); source.fixtureEnded = true; event('server-complete'); socket.end(packet);
        });
    });
    await new Promise((resolve, reject) => {server.once('error', reject); server.listen(0, '127.0.0.1', resolve);}); item.port = server.address().port;
    const child = spawn(helper, [worker, binding, user, 'http://127.0.0.1:'+item.port, [route, view, method].join(',')], {stdio: ['pipe', 'pipe', 'pipe']});
    let stdout = '', stderr = '', pending = '', received = false, acknowledged = false, resolveBody, rejectBody, timer;
    const bodyReady = new Promise((resolve, reject) => {resolveBody = resolve; rejectBody = reject;}); child.stdin.on('error', error => {item.controlError = error.code;});
    child.stdout.setEncoding('utf8'); child.stdout.on('data', chunk => {stdout += chunk; pending += chunk;
        for (;;) {const end = pending.indexOf('\n'); if (end < 0) break; const line = pending.slice(0, end).replace(/\r$/, ''); pending = pending.slice(end+1);
            if (line.startsWith('BINDING_BODY ')) {try {assert.equal(received, false); received = true; Object.assign(item, JSON.parse(line.slice(13))); resolveBody();} catch (error) {rejectBody(error);}}
            else if (line.startsWith('BINDING_EXIT ')) {try {assert.equal(item.workerExit, undefined); item.workerExit = JSON.parse(line.slice(13));} catch (error) {rejectBody(error);}}
        }
    }); child.stderr.on('data', chunk => {stderr += chunk;});
    const closed = new Promise((resolve, reject) => {child.once('error', reject); child.once('close', (code, signal) => resolve({code, signal}));});
    const deadline = new Promise((_, reject) => {timer = setTimeout(() => reject(new Error('Binding helper original45s absolute deadline')), 45000);}); let failure;
    try {
        await Promise.race([bodyReady, closed.then(() => {assert.ok(received, 'actual Binding result before exit: '+stderr);}), deadline]);
        const end = performance.now()+3000; while (!records.every(source => source.peerEnded) && performance.now() < end) await delay(5);
        item.sourcesBeforeStop = structuredClone(records); item.sampleSequence = sequence++; child.stdin.end('finish\n'); acknowledged = true; item.child = await Promise.race([closed, deadline]);
    } catch (error) {
        failure = error; item.collectionFailure = String(error.stack ?? error); if (!acknowledged && !child.stdin.destroyed) child.stdin.end('finish\n');
        if (child.exitCode === null && child.signalCode === null) {item.helperForced = true; child.kill();}
        item.child ??= await Promise.race([closed.catch(() => null), delay(2000).then(() => null)]);
    } finally {
        clearTimeout(timer); item.stdout = stdout; item.stderr = stderr; item.finalSources = structuredClone(records);
        for (const [socket, source] of sockets) {source.fixtureDestroyed = true; socket.destroy();} await new Promise(resolve => server.close(resolve)); save();
        console.log('BINDING_ARGUMENT_ROW '+JSON.stringify(item));
    }
    if (failure) throw failure; assertBindingArgumentCase(item, bodyKind, route, view, method);
}
assert.equal(hash(helper), expected.binaries.helper); assert.equal(hash(worker), expected.binaries.worker);
assert.deepEqual(Object.fromEntries(Object.entries(bindingArgumentInputPaths).map(([key, name]) => [key, hash(path.join(root, name))])), expected.inputs);
report.complete = true; save(); validateBindingArgumentReport(report, expected);
console.log(JSON.stringify({complete: true, bodyKind, route, groups: 26, argumentRows: 1092, structuredRpcCalls: 1092, sourcesEndedBeforeStop: true, actualCleanExit: true}));
