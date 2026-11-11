import assert from 'node:assert/strict';
import fs from 'node:fs';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';
import {spawn} from 'node:child_process';
import {fileURLToPath} from 'node:url';
import {setTimeout as delay} from 'node:timers/promises';
import {nativeFetchIntrinsicPlans, nativeFetchIntrinsicPrimaryIdentities} from './native-fetch-intrinsic-input.mjs';
import {openNativeFetchIntrinsicSource} from './native-fetch-intrinsic-peer.mjs';
import {validateNativeFetchIntrinsicPrimary} from './native-fetch-intrinsic-contract.mjs';
import {readLock, verifyBinary, verifyClient, fileDigest} from './workerd-reference.mjs';

assert.equal(process.argv.length, 4); assert.equal(process.argv[2], '--output');
const output = process.argv[3]; assert.equal(fs.existsSync(output), false, 'fresh actual primary');
const binary = process.env.CAPSID_WORKERD_REF, lock = readLock(); verifyBinary(binary, lock); verifyClient(lock);
const fixture = fileURLToPath(new URL('../tests/fixtures/native-fetch-intrinsic.mjs', import.meta.url));
const identities = nativeFetchIntrinsicPrimaryIdentities();
const report = {schema: 'capsid-native-fetch-intrinsic-reference-v1', complete: false, ...identities,
    referenceLock: lock, binarySHA256: fileDigest(binary), client: {node: process.version, undici: process.versions.undici},
    cases: [], stderr: '', shutdown: null};
const save = () => fs.writeFileSync(output, JSON.stringify(report, null, 2) + '\n'); save();
const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'capsid-native-intrinsic-primary-'));
assert.ok(path.isAbsolute(directory) && path.basename(directory).startsWith('capsid-native-intrinsic-primary-'));
fs.copyFileSync(fixture, path.join(directory, 'main.mjs'));
const listener = net.createServer(); await new Promise(resolve => listener.listen(0, '127.0.0.1', resolve));
const port = listener.address().port; await new Promise(resolve => listener.close(resolve));
const config = path.join(directory, 'config.capnp');
fs.writeFileSync(config, `using Workerd = import "/workerd/workerd.capnp"; const config :Workerd.Config = (services=[(name="main",worker=(compatibilityDate=${JSON.stringify(lock.compatibility_date)},compatibilityFlags=${JSON.stringify(lock.compatibility_flags)},modules=[(name="main",esModule=embed "main.mjs")])),(name="internet",network=(allow=["local"]))],sockets=[(name="http",address="127.0.0.1:${port}",http=(),service="main")]);`);
const child = spawn(binary, ['serve', config], {stdio: ['ignore', 'pipe', 'pipe']}); child.stdout.resume();
child.stderr.on('data', value => { report.stderr += value; });
const closed = new Promise((resolve, reject) => { child.once('close', (code, signal) => resolve({code, signal})); child.once('error', reject); });
const held = [];
try {
    const end = performance.now() + 10000;
    for (;;) try {
        const response = await fetch(`http://127.0.0.1:${port}/healthy`, {signal: AbortSignal.timeout(500)});
        assert.equal(await response.text(), 'healthy'); break;
    } catch (error) {
        assert.ok(performance.now() < end && child.exitCode === null && child.signalCode === null, String(error) + report.stderr);
        await delay(20);
    }
    for (const plan of nativeFetchIntrinsicPlans) {
        const source = await openNativeFetchIntrinsicSource(plan), row = {plan, packetBase64: source.packet.toString('base64')};
        report.cases.push(row); held.push({source, row});
        try {
            const query = new URLSearchParams({target: source.target, api: plan.api});
            const response = await fetch(`http://127.0.0.1:${port}/probe?${query}`, {signal: AbortSignal.timeout(12000)});
            row.httpStatus = response.status; row.state = await response.json();
            row.sourceTerminalBeforeReferenceStop = await source.terminal(); row.sourceBeforeReferenceStop = structuredClone(source.data);
            row.collectionComplete = true;
        } catch (error) { row.collectionFailure = String(error.stack ?? error); row.sourceFailure = structuredClone(source.data); }
        finally { save(); }
    }
    report.complete = report.cases.every(row => row.collectionComplete);
} finally {
    child.kill('SIGTERM'); let timer;
    try { report.shutdown = await Promise.race([closed, new Promise((_, reject) => {
        timer = setTimeout(() => reject(new Error('reference stop deadline')), 2000);
    })]); }
    finally {
        clearTimeout(timer);
        for (const {source, row} of held) row.sourceAfterReferenceStop = structuredClone(source.data);
        save(); await Promise.all(held.map(({source}) => source.close()));
        fs.rmSync(directory, {recursive: true, force: true});
    }
}
assert.equal(fileDigest(binary), report.binarySHA256); assert.deepEqual(nativeFetchIntrinsicPrimaryIdentities(), identities);
validateNativeFetchIntrinsicPrimary(report, process.platform, identities);
console.log(JSON.stringify({complete: report.complete, cases: report.cases.length, output}));
