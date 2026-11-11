import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import {spawn} from 'node:child_process';
import {createHash} from 'node:crypto';
import {fileURLToPath} from 'node:url';
import {helperCreditKinds, helperCreditPlans, helperCreditInputPaths, assertHelperCreditCase, validateHelperCreditReport} from '../tools/helper-response-credit-contract.mjs';

const root = fileURLToPath(new URL('../', import.meta.url)), hash = file => createHash('sha256').update(fs.readFileSync(file)).digest('hex');
const args = new Map();
for (let i = 2; i < process.argv.length; i += 2) {
    assert.ok(['--helper', '--worker', '--kind'].includes(process.argv[i]) && process.argv[i + 1] && !args.has(process.argv[i])); args.set(process.argv[i], process.argv[i + 1]);
}
assert.equal(args.size, 3);
const helper = args.get('--helper'), worker = args.get('--worker'), kind = args.get('--kind'); assert.ok(helperCreditKinds.includes(kind));
const inputs = () => Object.fromEntries(Object.entries(helperCreditInputPaths).map(([key, name]) => [key, hash(path.join(root, name))]));
const binaries = () => ({helper: hash(helper), worker: hash(worker)});
const expected = {kind, runId: process.env.CAPSID_WS_RUN_ID ?? 'standalone-helper-credit', inputs: inputs(), binaries: binaries()};
const report = {schema: 'capsid-helper-response-credit-v1', complete: false, ...expected, cases: [], failures: []};
let destination;
if (process.env.CAPSID_WS_REPORT_DIR) {
    fs.mkdirSync(process.env.CAPSID_WS_REPORT_DIR, {recursive: true}); destination = path.join(process.env.CAPSID_WS_REPORT_DIR, 'helper-response-credit-' + kind + '.json');
    assert.equal(fs.existsSync(destination), false, 'fresh helper response-window observations');
}
const save = () => {if (destination) fs.writeFileSync(destination, JSON.stringify(report, null, 2) + '\n');}; save();
for (const plan of helperCreditPlans(kind)) {
    const row = {plan, collectionComplete: false}; report.cases.push(row);
    let actual;
    if (kind === 'deadline') actual = [worker, path.join(root, helperCreditInputPaths.fixture), 'http://localhost/unused?bytes=' + plan.bytes, 'least', 'signal', 'timeout', '--small-response-window'];
    else if (kind.startsWith('limits')) actual = [worker, path.join(root, helperCreditInputPaths.limitsFixture), ...(kind === 'limits_encoded' ? ['--encoded-headers'] : []), '--small-response-window'];
    else actual = [worker, '--small-response-window'];
    const child = spawn(helper, actual, {stdio: ['pipe', 'pipe', 'pipe']}); let stdout = '', stderr = '', pending = '', acknowledged = false, timer;
    child.stdin.on('error', () => {}); child.stdout.setEncoding('utf8'); child.stderr.setEncoding('utf8');
    child.stdout.on('data', value => {
        stdout += value; pending += value;
        for (;;) {
            const at = pending.indexOf('\n'); if (at < 0) break;
            const line = pending.slice(0, at).replace(/\r$/, ''); pending = pending.slice(at + 1);
            if (kind === 'deadline' && line.startsWith('LIMIT_PROBE ') && !acknowledged) {acknowledged = true; child.stdin.end('finish\n');}
        }
    });
    child.stderr.on('data', value => {stderr += value;});
    const closed = new Promise((resolve, reject) => {child.once('close', (code, signal) => resolve({code, signal})); child.once('error', reject);});
    const bound = new Promise((_, reject) => {timer = setTimeout(() => reject(new Error('helper response-window observation deadline')), kind.startsWith('limits') ? 45000 : kind === 'deadline' ? 12000 : 30000);});
    try {row.helperExit = await Promise.race([closed, bound]); row.collectionComplete = row.helperExit.code === 0 && row.helperExit.signal === null;}
    catch (error) {row.collectionFailure = String(error.stack ?? error);}
    finally {
        clearTimeout(timer);
        if (child.exitCode === null && child.signalCode === null) {row.helperForced = true; child.kill();}
        row.helperExit ??= await closed; row.stdout = stdout; row.stderr = stderr;
    }
    try {assertHelperCreditCase(row, kind, plan);}
    catch (error) {report.failures.push({id: plan.id, reason: String(error.stack ?? error)});}
    save(); console.log(JSON.stringify({kind, id: plan.id, collected: row.collectionComplete, helperExit: row.helperExit, valid: !report.failures.some(f => f.id === plan.id)}));
}
assert.deepEqual(inputs(), expected.inputs); assert.deepEqual(binaries(), expected.binaries);
report.complete = report.failures.length === 0; save(); assert.equal(report.complete, true, 'all actual response-window observations must pass');
console.log(JSON.stringify({complete: true, kind, cases: report.cases.length, totals: validateHelperCreditReport(report, expected)}));
