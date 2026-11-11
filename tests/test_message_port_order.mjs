import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import net from 'node:net';
import {spawn} from 'node:child_process';
import {parseArgs} from 'node:util';
import {fileURLToPath} from 'node:url';
import {setTimeout as delay} from 'node:timers/promises';
import {randomUUID} from 'node:crypto';
import {startCapsid, stopCapsid, withCleanup} from './helpers/ws_host_backend.mjs';
import {root, lockPath, readLock, verifyClient, verifyBinary, fileDigest} from '../tools/workerd-reference.mjs';
import {referenceBackendModes} from '../tools/workerd-platform.mjs';
import {messagePortInputPaths, assertMessagePortReport} from '../tools/message-port-order-contract.mjs';

const {values: args} = parseArgs({options: {
    host: {type: 'string'}, worker: {type: 'string'}, reference: {type: 'string'},
    mode: {type: 'string', default: 'single-worker'}, report: {type: 'string'},
}});
assert.ok(args.host && args.worker, '--host and --worker are required');
const lock = readLock();
verifyClient(lock);
const reference = args.reference ?? process.env.CAPSID_WORKERD_REF;
assert.ok(reference, '--reference or CAPSID_WORKERD_REF is required');
verifyBinary(reference, lock);
assert.ok(referenceBackendModes(lock).includes(args.mode), 'supported native Host mode');
const reportFile = path.resolve(args.report ?? path.join(process.env.CAPSID_WS_REPORT_DIR ?? process.cwd(),
    `message-port-order-${args.mode}.json`));
fs.mkdirSync(path.dirname(reportFile), {recursive: true});
const output = fs.mkdtempSync(reportFile + '.runs-');
// Managed state needs POSIX permissions and a short Unix socket path, even when
// the requested report archive is on a mounted drive or inside a deep checkout.
const nativeRuntime = process.platform === 'linux' && args.mode === 'managed'
    ? fs.mkdtempSync('/tmp/cpm-') : null;
const nativeDirectory = nativeRuntime ?? path.join(output, 'native');
const fixture = path.join(root, messagePortInputPaths.fixture);
const files = Object.fromEntries(Object.entries(messagePortInputPaths).map(([name, file]) => [name, path.join(root, file)]));
assert.equal(files.runner, fileURLToPath(import.meta.url));
const inputs = Object.fromEntries(Object.entries(files).map(([name, file]) => [name, fileDigest(file)]));
const measuredFiles = [...Object.values(files), lockPath, args.host, args.worker, reference, process.execPath];
const measured = Object.fromEntries(measuredFiles.map(file => [file, fileDigest(file)]));
const binaries = {host: fileDigest(args.host), worker: fileDigest(args.worker),
    reference: fileDigest(reference), node: fileDigest(process.execPath)};
const report = {schema: 'capsid-message-port-order-v1', complete: false, status: 'running',
    mode: args.mode, runId: process.env.CAPSID_WS_RUN_ID ?? randomUUID(), artifactDirectory: output,
    requestDeadlineMilliseconds: 5000, functionalQualification: true, performanceQualification: false,
    workerOsExitNotInferredFromHostExit: true, referenceLock: lock, inputs, binaries, roles: {}};
const save = () => fs.writeFileSync(reportFile, JSON.stringify(report, null, 2) + '\n');
save();

async function unusedPort() {
    const server = net.createServer();
    await new Promise((resolve, reject) => {server.once('error', reject); server.listen(0, '127.0.0.1', resolve);});
    const port = server.address().port;
    await new Promise(resolve => server.close(resolve));
    return port;
}
async function observe(target, row, directory) {
    const response = await fetch(target, {signal: AbortSignal.timeout(report.requestDeadlineMilliseconds)});
    row.httpStatus = response.status;
    const body = await response.text();
    fs.writeFileSync(path.join(directory, 'response.json'), body);
    assert.equal(response.status, 200);
    row.public = JSON.parse(body);
}

function archiveNativeRuntime() {
    if (!nativeRuntime) return;
    const archive = path.join(output, 'native'), files = [], omittedSpecialEntries = [];
    fs.cpSync(nativeDirectory, archive, {recursive: true, filter(source) {
        const info = fs.lstatSync(source), relative = path.relative(nativeDirectory, source);
        if (info.isDirectory()) return true;
        if (info.isFile()) {
            files.push({path: relative, sha256: fileDigest(source), mode: info.mode & 0o777});
            return true;
        }
        omittedSpecialEntries.push({path: relative,
            type: info.isSocket() ? 'socket' : info.isSymbolicLink() ? 'symlink' : 'special'});
        return false;
    }});
    for (const file of files) assert.equal(fileDigest(path.join(archive, file.path)), file.sha256);
    report.nativeRuntime = {directory: nativeDirectory, mode: fs.statSync(nativeDirectory).mode & 0o777,
        adminSocketPathBytes: Buffer.byteLength(path.join(nativeDirectory, 'run', 'admin.sock')),
        archivedDirectory: archive, files, omittedSpecialEntries};
}

let native, primary, primaryClosed, primaryStderr = '', primaryStdout = '';
try {
    await withCleanup(async () => {
        const directory = path.join(output, 'reference');
        fs.mkdirSync(directory);
        fs.copyFileSync(fixture, path.join(directory, 'main.mjs'));
        const port = await unusedPort(), config = path.join(directory, 'reference.capnp');
        fs.writeFileSync(config, 'using Workerd = import "/workerd/workerd.capnp";\n' +
            'const config :Workerd.Config = (services=[(name="main",worker=(compatibilityDate=' +
            JSON.stringify(lock.compatibility_date) + ',compatibilityFlags=' + JSON.stringify(lock.compatibility_flags) +
            ',modules=[(name="main",esModule=embed "main.mjs")]))],sockets=[(name="http",address="127.0.0.1:' +
            port + '",http=(),service="main")]);\n');
        primary = spawn(reference, ['serve', config], {stdio: ['ignore', 'pipe', 'pipe']});
        primaryClosed = new Promise(resolve => primary.once('close', resolve));
        primary.stdout.setEncoding('utf8'); primary.stdout.on('data', value => primaryStdout += value);
        primary.stderr.setEncoding('utf8'); primary.stderr.on('data', value => primaryStderr += value);
        report.roles.reference = {command: primary.spawnargs, pid: primary.pid};
        const deadline = Date.now() + 10000;
        for (;;) {
            assert.equal(primary.exitCode, null, primaryStderr);
            try {
                const socket = net.createConnection({host: '127.0.0.1', port});
                await new Promise((resolve, reject) => {socket.once('connect', resolve); socket.once('error', reject);});
                socket.destroy(); break;
            } catch (error) {assert.ok(Date.now() < deadline, String(error)); await delay(25);}
        }
        await observe(`http://127.0.0.1:${port}/`, report.roles.reference, directory);
        fs.mkdirSync(nativeDirectory, {recursive: true, mode: 0o700});
        if (nativeRuntime) {
            assert.equal(fs.statSync(nativeDirectory).mode & 0o077, 0, 'private Managed runtime');
            assert.ok(Buffer.byteLength(path.join(nativeDirectory, 'run', 'admin.sock')) < 108,
                'Managed admin socket fits sockaddr_un');
        }
        const program = path.join(nativeDirectory, 'main.mjs');
        fs.copyFileSync(fixture, program);
        assert.equal(fileDigest(program), inputs.fixture);
        native = await startCapsid(args.host, args.worker, program, nativeDirectory, args.mode);
        report.roles.native = {command: native.child.spawnargs, pid: native.child.pid};
        await observe(`http://127.0.0.1:${native.port}/@capsid/orders/`, report.roles.native, nativeDirectory);
    }, async () => {
        const cleanups = [];
        if (native) cleanups.push((async () => {
            report.roles.native.shutdown = await stopCapsid(native.child);
            report.roles.native.stderr = native.stderr();
            fs.writeFileSync(path.join(nativeDirectory, 'stderr.log'), native.stderr());
        })());
        if (primary) cleanups.push((async () => {
            if (primary.exitCode === null && primary.signalCode === null) primary.kill('SIGTERM');
            let timer;
            await Promise.race([primaryClosed, new Promise(resolve => {
                timer = setTimeout(() => {primary.kill('SIGKILL'); resolve();}, 5000);
            })]);
            clearTimeout(timer); await primaryClosed;
            Object.assign(report.roles.reference, {stderr: primaryStderr, cleanupRequested: true,
                cleanupExit: {code: primary.exitCode, signal: primary.signalCode}, naturalExitNotClaimed: true});
            fs.writeFileSync(path.join(output, 'reference', 'stderr.log'), primaryStderr);
            fs.writeFileSync(path.join(output, 'reference', 'stdout.log'), primaryStdout);
        })());
        const results = await Promise.allSettled(cleanups);
        archiveNativeRuntime();
        const errors = results.filter(value => value.status === 'rejected').map(value => value.reason);
        if (errors.length) throw new AggregateError(errors, 'MessagePort runtime cleanup failed');
    });
    for (const [file, digest] of Object.entries(measured)) assert.equal(fileDigest(file), digest, file);
    Object.assign(report, {complete: true, status: 'complete', measuredInputsUnchanged: true});
    report.contract = assertMessagePortReport(report, {mode: args.mode, runId: report.runId, inputs, binaries, lock});
    save();
    console.log(JSON.stringify(report.contract));
} catch (error) {
    Object.assign(report, {complete: true, status: 'failed', error: String(error.stack ?? error)});
    save(); throw error;
}
