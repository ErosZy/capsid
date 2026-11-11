import assert from 'node:assert/strict';
import {spawn} from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import {fileURLToPath} from 'node:url';

const args = new Map();
for (let i = 2; i < process.argv.length; i += 2) {
    assert.ok(['--host', '--worker', '--memory-mode'].includes(process.argv[i]) && process.argv[i + 1] && !args.has(process.argv[i]));
    args.set(process.argv[i], process.argv[i + 1]);
}
assert.equal(process.platform, 'linux');
assert.ok(args.get('--host') && args.get('--worker'));
const tool = fileURLToPath(new URL('../tools/run_managed_websocket_soak.mjs', import.meta.url));
const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'c78-soak-'));
const reports = [];
let complete = false;
try {
    for (const negative of [false, true]) {
        const label = negative ? 'retained-channel' : 'mixed-lifecycle';
        const output = path.join(directory, label);
        const command = [tool, '--host', args.get('--host'), '--worker', args.get('--worker'),
            '--runtime-dir', path.join(directory, negative ? 'n' : 'p'), '--output', output,
            '--seconds', '1', '--min-rounds', '12', '--memory-mode', args.get('--memory-mode') ?? 'production'];
        if (negative) command.push('--control-retain-channel', '1');
        const child = spawn(process.execPath, command, {stdio: ['ignore', 'pipe', 'pipe']});
        let stdout = '', stderr = '';
        child.stdout.on('data', chunk => { stdout += chunk; });
        child.stderr.on('data', chunk => { stderr += chunk; });
        const exit = await new Promise((resolve, reject) => {
            child.once('error', reject);
            child.once('close', (code, signal) => resolve({code, signal}));
        });
        assert.equal(exit.signal, null, stderr);
        const resultFile = path.join(output, 'result.json');
        assert.ok(fs.existsSync(resultFile), 'the actual runner must produce evidence: ' + stdout + stderr);
        const result = JSON.parse(fs.readFileSync(resultFile, 'utf8'));
        reports.push({label, command, exit, result, stdout, stderr});
        assert.equal(result.complete, true);
        assert.deepEqual(result.shutdown, {code: 0, signal: null, forced: false});
        assert.equal(result.cleanupError, undefined);
        if (negative) {
            assert.equal(exit.code, 1);
            assert.equal(result.status, 'failed');
            assert.equal(result.deliberateRetainedChannelControl, true);
            assert.match(result.error, /all channel resources return: \{"channels":1,"active":1/);
            assert.equal(result.rounds, 0, 'the real retained channel must reject the first idle check');
        } else {
            assert.equal(exit.code, 0, result.error ?? stderr);
            assert.equal(result.status, 'passed');
            assert.equal(result.rounds, 12);
            assert.equal(result.echoes, 16 * result.rounds);
            for (const name of ['outboundEchoes', 'pendingDialCancellations', 'queueRefusals', 'pausedReaders'])
                assert.equal(result[name], result.rounds, name);
            assert.equal(result.deployments, 6);
            assert.equal(result.workerReplacements, 6);
            assert.ok(result.elapsedSeconds >= 1);
            for (const name of ['channels', 'active', 'dials', 'queue', 'http']) assert.equal(result.finalResources[name], 0, name);
        }
    }
    complete = true;
} finally {
    if (process.env.CAPSID_WS_REPORT_DIR) {
        fs.mkdirSync(process.env.CAPSID_WS_REPORT_DIR, {recursive: true});
        const file = path.join(process.env.CAPSID_WS_REPORT_DIR, 'managed-websocket-soak-controls.json');
        assert.equal(fs.existsSync(file), false);
        fs.writeFileSync(file, JSON.stringify({complete, reports}, null, 2) + '\n');
    }
    fs.rmSync(directory, {recursive: true, force: true});
}
console.log(JSON.stringify({complete: true,positiveRounds: 12,actualRetainedChannelRejected: true}));
