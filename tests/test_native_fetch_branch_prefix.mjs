import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import {createHash} from 'node:crypto';
import {spawn} from 'node:child_process';
import {branchPrefixCases, branchPrefixInputPaths} from '../tools/native-fetch-branch-prefix-input.mjs';
import {openLargeSource} from '../tools/native-fetch-branch-prefix-peer.mjs';
import {assertBranchPrefixCase, validateBranchPrefixReport, readBranchPrefixPrimary} from '../tools/native-fetch-branch-prefix-contract.mjs';

const root = fileURLToPath(new URL('../', import.meta.url)), args = new Map();
for (let index = 2; index < process.argv.length; index += 2) {
    assert.ok(['--helper', '--worker', '--method'].includes(process.argv[index]) && process.argv[index + 1] && !args.has(process.argv[index]));
    args.set(process.argv[index], process.argv[index + 1]);
}
assert.equal(args.size, 3);
const helper = args.get('--helper'), worker = args.get('--worker'), method = args.get('--method');
const plans = branchPrefixCases(method), hash = file => createHash('sha256').update(fs.readFileSync(file)).digest('hex');
const inputs = () => Object.fromEntries(Object.entries(branchPrefixInputPaths).map(([name, file]) => [name, hash(path.join(root, file))]));
const expected = {method, runId: process.env.CAPSID_WS_RUN_ID ?? 'standalone-native-branch-prefix', inputs: inputs(),
    binaries: {helper: hash(helper), worker: hash(worker)}};
const primary = readBranchPrefixPrimary();
const report = {schema: 'capsid-native-fetch-branch-prefix-v1', complete: false, ...expected, cases: []};
let destination;
if (process.env.CAPSID_WS_REPORT_DIR) {
    fs.mkdirSync(process.env.CAPSID_WS_REPORT_DIR, {recursive: true});
    destination = path.join(process.env.CAPSID_WS_REPORT_DIR, `native-fetch-branch-prefix-${method}.json`);
    assert.equal(fs.existsSync(destination), false, 'fresh actual branch prefix observations');
}
const save = () => { if (destination) fs.writeFileSync(destination, JSON.stringify(report, null, 2) + '\n'); }; save();
for (const plan of plans) {
    const source = await openLargeSource(plan);
    const row = {plan, collectionComplete: false, packetBase64: source.packet.toString('base64')}; report.cases.push(row);
    const child = spawn(helper, [worker, path.join(root, branchPrefixInputPaths.binding),
        path.join(root, branchPrefixInputPaths.user), source.base, `${plan.api},1`], {stdio: ['pipe', 'pipe', 'pipe']});
    let stdout = '', stderr = '', pending = '', firstResolve, secondResolve, rejectObserved, timer;
    const first = new Promise(resolve => { firstResolve = resolve; }), second = new Promise(resolve => { secondResolve = resolve; });
    const failure = new Promise((_, reject) => { rejectObserved = reject; });
    child.stdin.on('error', () => {}); child.stdout.setEncoding('utf8');
    child.stdout.on('data', value => {
        stdout += value; pending += value;
        for (;;) {
            const at = pending.indexOf('\n'); if (at < 0) break;
            const line = pending.slice(0, at).replace(/\r$/, ''); pending = pending.slice(at + 1);
            try {
                if (line.startsWith('BINDING_PHASE 1 ')) { assert.equal(row.request1, undefined); row.request1 = JSON.parse(line.slice(16)); firstResolve(); }
                else if (line.startsWith('BINDING_PHASE 2 ')) { assert.equal(row.request2, undefined); row.request2 = JSON.parse(line.slice(16)); secondResolve(); }
                else if (line.startsWith('BINDING_EXIT ')) { assert.equal(row.workerExit, undefined); row.workerExit = JSON.parse(line.slice(13)); }
            } catch (error) { rejectObserved(error); }
        }
    });
    child.stderr.on('data', value => { stderr += value; });
    const closed = new Promise((resolve, reject) => { child.once('close', (code, signal) => resolve({code, signal})); child.once('error', reject); });
    const bound = new Promise((_, reject) => { timer = setTimeout(() => reject(new Error('branch prefix observation15s bound')), 15000); });
    try {
        await Promise.race([first, failure, closed.then(() => assert.ok(row.request1, 'actual first response')), bound]);
        row.sourceTerminalBeforeContinue = await source.terminal(); row.sourceBeforeContinue = structuredClone(source.data);
        child.stdin.write('continue\n');
        await Promise.race([second, failure, closed.then(() => assert.ok(row.request2, 'actual second response')), bound]);
        row.sourceBeforeStop = structuredClone(source.data); child.stdin.end('finish\n');
        row.helperExit = await Promise.race([closed, failure, bound]);
        row.sourceAfterExit = structuredClone(source.data); row.collectionComplete = true;
    } catch (error) { row.collectionFailure = String(error.stack ?? error); row.sourceFailure = structuredClone(source.data); }
    finally {
        clearTimeout(timer);
        if (child.exitCode === null && child.signalCode === null) { row.helperForced = true; child.kill(); row.helperExit = await closed; }
        row.stdout = stdout; row.stderr = stderr; row.sourceFinalBeforeCleanup = structuredClone(source.data);
        await source.close(); save(); console.log('NATIVE_FETCH_BRANCH_PREFIX_ROW ' + JSON.stringify(row));
    }
    assertBranchPrefixCase(row, plan);
    for (const platform of ['win32', 'linux']) assert.deepEqual(row.request1.summary,
        primary.platforms[platform].cases.find(value => value.plan.id === plan.id).state, 'complete actual pinned public state');
}
assert.equal(hash(helper), report.binaries.helper); assert.equal(hash(worker), report.binaries.worker);
assert.deepEqual(inputs(), expected.inputs); report.complete = true; save(); validateBranchPrefixReport(report, expected);
console.log(JSON.stringify({complete: true, method, cases: plans.length, requests: plans.length * 2, rpcCalls: plans.length * 2,
    naturalWorkerExits: plans.length, sourcesClosedBeforeSecondRequest: true, originalHelperBudgets: true}));
