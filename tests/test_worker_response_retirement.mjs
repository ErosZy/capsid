import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import {spawn} from 'node:child_process';
import {createHash} from 'node:crypto';
import {fileURLToPath} from 'node:url';
import {retirementActions, retirementPlans, retirementInputs, assertRetirementCase, validateRetirementReport} from '../tools/response-retirement-contract.mjs';

const root = fileURLToPath(new URL('../', import.meta.url));
const hash = file => createHash('sha256').update(fs.readFileSync(file)).digest('hex');
const args = new Map();
for (let index = 2; index < process.argv.length; index += 2) {
    const key = process.argv[index], value = process.argv[index + 1];
    assert.ok(['--helper', '--worker', '--action', '--process-memory', '--identity-header'].includes(key) && value && !args.has(key));
    args.set(key, value);
}
assert.equal(args.size, 5);
const helper = args.get('--helper'), worker = args.get('--worker'), action = args.get('--action');
assert.ok(retirementActions.includes(action));
const identity = fs.readFileSync(args.get('--identity-header'), 'utf8');
const compatibility = /#define CAPSID_BUILD_COMPATIBILITY_ID "(sha256:[a-f0-9]{64})"/.exec(identity)?.[1];
const manifest = /#define CAPSID_BUILD_CAPABILITY_MANIFEST_SHA256\s+(?:\\\s+)?"([a-f0-9]{64})"/.exec(identity)?.[1];
assert.ok(compatibility && manifest, 'generated identity must be complete');
const processMemory = Number(args.get('--process-memory'));
assert.ok(processMemory === 0 || processMemory === 268435456);
const inputs = () => Object.fromEntries(Object.entries(retirementInputs).map(([key, name]) => [key, hash(path.join(root, name))]));
const binaries = () => ({helper: hash(helper), worker: hash(worker), identity: hash(args.get('--identity-header'))});
const expected = {action, runId: process.env.CAPSID_WS_RUN_ID ?? 'standalone-response-retirement', inputs: inputs(), binaries: binaries(),
    processMemory, features: process.platform === 'win32' && !processMemory ? 0 : 1, compatibility, manifest};
const report = {schema: 'capsid-response-retirement-v1', complete: false, ...expected, cases: [], failures: []};
let destination;
if (process.env.CAPSID_WS_REPORT_DIR) {
    fs.mkdirSync(process.env.CAPSID_WS_REPORT_DIR, {recursive: true});
    destination = path.join(process.env.CAPSID_WS_REPORT_DIR, 'response-retirement-' + action + '.json');
    assert.equal(fs.existsSync(destination), false, 'fresh retirement observations');
}
const save = () => {if (destination) fs.writeFileSync(destination, JSON.stringify(report, null, 2) + '\n');};
save();
for (const plan of retirementPlans(action)) {
    const row = {plan, collectionComplete: false}; report.cases.push(row);
    const child = spawn(helper, [worker, path.join(root, retirementInputs.fixture), plan.kind, action, String(plan.bytes)], {stdio: ['ignore', 'pipe', 'pipe']});
    let stdout = '', stderr = '', timer;
    child.stdout.setEncoding('utf8'); child.stderr.setEncoding('utf8');
    child.stdout.on('data', data => {stdout += data;}); child.stderr.on('data', data => {stderr += data;});
    const closed = new Promise((resolve, reject) => {child.once('close', (code, signal) => resolve({code, signal})); child.once('error', reject);});
    try {
        row.helperExit = await Promise.race([closed, new Promise((_, reject) => {timer = setTimeout(() => reject(new Error('retirement observation deadline')), 12000);})]);
        row.collectionComplete = true;
    } catch (error) {row.collectionFailure = String(error.stack ?? error);}
    finally {
        clearTimeout(timer);
        if (child.exitCode === null && child.signalCode === null && child.pid) {row.helperForced = true; child.kill();}
        try {row.helperExit ??= await closed;} catch (error) {row.spawnFailure = String(error);}
        row.stdout = stdout; row.stderr = stderr;
    }
    try {assertRetirementCase(row, plan, expected);}
    catch (error) {report.failures.push({id: plan.id, reason: String(error.stack ?? error)});}
    save();
    console.log(JSON.stringify({id: plan.id, exit: row.helperExit, valid: !report.failures.some(failure => failure.id === plan.id)}));
}
assert.deepEqual(inputs(), expected.inputs); assert.deepEqual(binaries(), expected.binaries);
report.complete = report.failures.length === 0; save();
assert.equal(report.complete, true, JSON.stringify(report.failures));
console.log(JSON.stringify({complete: true, ...validateRetirementReport(report, expected)}));
