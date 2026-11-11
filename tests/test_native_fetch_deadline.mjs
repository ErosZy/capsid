import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import {spawn} from 'node:child_process';
import {createHash} from 'node:crypto';
import {setTimeout as delay} from 'node:timers/promises';
import {fileURLToPath} from 'node:url';
import {nativeFetchDeadlineInputPaths, nativeFetchDeadlineCasesForShard} from '../tools/native-fetch-deadline-input.mjs';
import {openNativeFetchDeadlineSource} from '../tools/native-fetch-deadline-peer.mjs';
import {assertNativeFetchDeadlineCase, validateNativeFetchDeadlineReport,
    nativeFetchDeadlinePrimaryArchive} from '../tools/native-fetch-deadline-contract.mjs';

const root = fileURLToPath(new URL('../', import.meta.url));
const hash = file => createHash('sha256').update(fs.readFileSync(file)).digest('hex');
const args = new Map();
for (let index = 2; index < process.argv.length; index += 2) {
    assert.ok(['--helper', '--worker', '--shard'].includes(process.argv[index]) && process.argv[index + 1] && !args.has(process.argv[index]));
    args.set(process.argv[index], process.argv[index + 1]);
}
assert.equal(args.size, 3);
nativeFetchDeadlinePrimaryArchive();
const helper = args.get('--helper'), worker = args.get('--worker'), shard = args.get('--shard');
const plans = nativeFetchDeadlineCasesForShard(shard), fixture = path.join(root, nativeFetchDeadlineInputPaths.fixture);
const inputs = () => Object.fromEntries(Object.entries(nativeFetchDeadlineInputPaths).map(([key, name]) => [key, hash(path.join(root, name))]));
const expected = {shard, runId: process.env.CAPSID_WS_RUN_ID ?? 'standalone-native-deadline', inputs: inputs(),
    binaries: {helper: hash(helper), worker: hash(worker)}};
const report = {schema: 'capsid-native-fetch-deadline-v1', complete: false, ...expected, cases: []};
let destination;
if (process.env.CAPSID_WS_REPORT_DIR) {
    fs.mkdirSync(process.env.CAPSID_WS_REPORT_DIR, {recursive: true});
    destination = path.join(process.env.CAPSID_WS_REPORT_DIR, `native-fetch-deadline-${shard}.json`);
    assert.equal(fs.existsSync(destination), false, 'fresh actual observations');
}
const save = () => { if (destination) fs.writeFileSync(destination, JSON.stringify(report, null, 2) + '\n'); };
save();
for (const plan of plans) {
    const start = performance.now(), source = await openNativeFetchDeadlineSource(plan);
    const row = {plan, collectionComplete: false, timings: {}};
    report.cases.push(row);
    const child = spawn(helper, [worker, fixture, source.target, plan.stage, plan.trigger, plan.link], {stdio: ['pipe', 'pipe', 'pipe']});
    let stdout = '', stderr = '', resolveState, timer, failure;
    const ready = new Promise(resolve => { resolveState = resolve; });
    child.stdout.on('data', value => {
        stdout += value;
        const line = stdout.split(/\r?\n/).find(value => value.startsWith('LIMIT_PROBE '));
        if (line) try { row.state = JSON.parse(line.slice(12)); resolveState(); } catch {}
    });
    child.stderr.on('data', value => { stderr += value; });
    child.stdin.on('error', () => {});
    const closed = new Promise((resolve, reject) => {
        child.once('close', (code, signal) => resolve({code, signal}));
        child.once('error', reject);
    });
    const bound = new Promise((_, reject) => {
        timer = setTimeout(() => reject(new Error('native deadline12s observation bound')), 12000);
    });
    try {
        await Promise.race([ready, closed.then(() => assert.ok(row.state, 'helper produced actual state')), bound]);
        row.timings.stateMs = performance.now() - start;
        row.sourceTerminalBeforeFinish = await source.terminal();
        row.timings.terminalMs = performance.now() - start;
        row.sourceBeforeFinish = structuredClone(source.data);
        assert.equal(row.sourceTerminalBeforeFinish, true, 'actual peers terminal before host finish');
        child.stdin.end('finish\n');
        row.shutdown = await Promise.race([closed, bound]);
        row.stdout = stdout;
        row.stderr = stderr;
        const exits = stdout.split(/\r?\n/).filter(value => value.startsWith('LIMIT_EXIT '));
        assert.equal(exits.length, 1, 'one actual worker exit');
        row.workerExit = JSON.parse(exits[0].slice(11));
        row.sourceAfterFinish = structuredClone(source.data);
        row.collectionComplete = true;
    } catch (error) {
        failure = error;
        row.collectionFailure = String(error.stack ?? error);
        row.sourceFailureSnapshot = structuredClone(source.data);
    } finally {
        clearTimeout(timer);
        if (child.exitCode === null && child.signalCode === null) {
            row.helperForced = true;
            child.kill();
            row.shutdown ??= await Promise.race([closed.catch(() => null), delay(2000).then(() => null)]);
        }
        row.stdout ??= stdout;
        row.stderr ??= stderr;
        await source.close();
        row.timings.completeMs = performance.now() - start;
        save();
        console.log('NATIVE_DEADLINE_ROW ' + JSON.stringify(row));
    }
    if (failure) throw failure;
    assertNativeFetchDeadlineCase(row, plan);
}
assert.deepEqual(inputs(), expected.inputs);
assert.deepEqual({helper: hash(helper), worker: hash(worker)}, expected.binaries);
report.complete = true;
save();
validateNativeFetchDeadlineReport(report, expected);
console.log(JSON.stringify({complete: true, shard, cases: plans.length,
    allSourceBodiesWithheld: true, allPeerTerminalsBeforeCleanup: true, allWorkerExitsNatural: true}));
