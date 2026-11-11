import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import {spawn} from 'node:child_process';
import {createHash} from 'node:crypto';
import {setTimeout as delay} from 'node:timers/promises';
import {fileURLToPath} from 'node:url';
import {responseCreditPlans, responseCreditInputPaths, assertResponseCreditCase,
    validateResponseCreditReport} from '../tools/native-fetch-response-credit-contract.mjs';

const root = fileURLToPath(new URL('../', import.meta.url));
const hash = file => createHash('sha256').update(fs.readFileSync(file)).digest('hex');
const args = new Map();
for (let i = 2; i < process.argv.length; i += 2) {
    assert.ok(['--helper', '--worker'].includes(process.argv[i]) && process.argv[i+1] && !args.has(process.argv[i]));
    args.set(process.argv[i], process.argv[i+1]);
}
assert.equal(args.size, 2);
const helper = args.get('--helper'), worker = args.get('--worker');
const inputs = () => Object.fromEntries(Object.entries(responseCreditInputPaths).map(([key, name]) => [key, hash(path.join(root, name))]));
const binaries = () => ({helper: hash(helper), worker: hash(worker)});
const expected = {runId: process.env.CAPSID_WS_RUN_ID ?? 'standalone-response-credit', inputs: inputs(), binaries: binaries()};
const report = {schema: 'capsid-native-fetch-response-credit-v1', complete: false, ...expected, cases: []};
let destination;
if (process.env.CAPSID_WS_REPORT_DIR) {
    fs.mkdirSync(process.env.CAPSID_WS_REPORT_DIR, {recursive: true});
    destination = path.join(process.env.CAPSID_WS_REPORT_DIR, 'native-fetch-response-credit.json');
    assert.equal(fs.existsSync(destination), false, 'fresh response credit observations');
}
const save = () => {if (destination) fs.writeFileSync(destination, JSON.stringify(report, null, 2) + '\n');};
save();
for (const bytes of responseCreditPlans) {
    const row = {bytes, collectionComplete: false}; report.cases.push(row);
    const child = spawn(helper, [worker, path.join(root, responseCreditInputPaths.fixture), 'http://localhost/unused', '0', String(bytes)],
        {stdio: ['pipe', 'pipe', 'pipe']});
    let stdout = '', stderr = '', pending = '', resolveBody, rejectFailure, timer;
    const body = new Promise(resolve => {resolveBody = resolve;});
    const failure = new Promise((_, reject) => {rejectFailure = reject;});
    child.stdin.on('error', () => {}); child.stdout.setEncoding('utf8'); child.stderr.setEncoding('utf8');
    child.stdout.on('data', value => {
        stdout += value; pending += value;
        for (;;) {
            const at = pending.indexOf('\n'); if (at < 0) break;
            const line = pending.slice(0, at).replace(/\r$/, ''); pending = pending.slice(at + 1);
            try {
                if (line.startsWith('LIMIT_PROBE ')) resolveBody();
                else if (line.startsWith('LIMIT_EXIT ')) {
                    assert.equal(row.workerExit, undefined); row.workerExit = JSON.parse(line.slice(11));
                }
            } catch (error) {rejectFailure(error);}
        }
    });
    child.stderr.on('data', value => {stderr += value;});
    const closed = new Promise((resolve, reject) => {
        child.once('close', (code, signal) => resolve({code, signal})); child.once('error', reject);
    });
    const bound = new Promise((_, reject) => {timer = setTimeout(() => reject(new Error('response credit observation12s deadline')), 12000);});
    let error;
    try {
        await Promise.race([body, failure, bound, closed.then(() => {throw new Error('helper exited before complete response');})]);
        child.stdin.end('finish\n');
        row.helperExit = await Promise.race([closed, failure, bound]);
        row.collectionComplete = true;
    } catch (value) {error = value; row.collectionFailure = String(value.stack ?? value);}
    finally {
        clearTimeout(timer);
        if (child.exitCode === null && child.signalCode === null) {
            row.helperForced = true; child.kill();
            await Promise.race([closed.catch(() => null), delay(2000)]);
        }
        row.helperExit ??= await closed;
        row.stdout = stdout; row.stderr = stderr; save();
        console.log(JSON.stringify({bytes, collected: row.collectionComplete, helperExit: row.helperExit}));
    }
    if (error) throw error;
    assertResponseCreditCase(row, bytes);
}
assert.deepEqual(inputs(), expected.inputs); assert.deepEqual(binaries(), expected.binaries);
report.complete = true; save(); validateResponseCreditReport(report, expected);
console.log(JSON.stringify({complete: true, cases: responseCreditPlans.length, naturalExits: true}));
