import assert from 'node:assert/strict';
import {spawn} from 'node:child_process';
import {once} from 'node:events';
import {createInterface} from 'node:readline';
import {fileURLToPath} from 'node:url';
import test from 'node:test';
import {parseDirectChildren, posixWorkerChildren} from './helpers/posix_worker_processes.mjs';

test('direct PPID selection preserves executable paths with spaces', () => {
    assert.deepEqual(parseDirectChildren('  11  2 /tmp/build with spaces/capsid-worker\n12 11 /tmp/grandchild\n13 2 /tmp/second worker\n', 2),
        [{pid: 11, executable: '/tmp/build with spaces/capsid-worker'}, {pid: 13, executable: '/tmp/second worker'}]);
    assert.deepEqual(parseDirectChildren('\n  12 11 /tmp/grandchild\n', 2), []);
});
test('incomplete or ambiguous process snapshots fail closed', () => {
    for (const table of ['11 2\n', 'PID PPID COMMAND\n', '11 2 /one\n11 2 /two\n', '9007199254740992 2 /one\n'])
        assert.throws(() => parseDirectChildren(table, 2));
    for (const pid of [0, -1, NaN, 1.5, '2']) assert.throws(() => parseDirectChildren('', pid));
});
if (process.platform !== 'win32') test('native ps finds only owned children and rejects an unrelated executable', async () => {
    const supervisor = spawn(process.execPath, ['-e', `
        const {spawn}=require('node:child_process');
        const child=spawn(process.execPath,['-e','process.stdin.resume()'],{stdio:['pipe','ignore','inherit']});
        child.once('spawn',()=>console.log(JSON.stringify({pid:child.pid})));
        process.stdin.resume();process.stdin.once('end',()=>child.stdin.end());
    `], {stdio: ['pipe', 'pipe', 'inherit']});
    const closed = once(supervisor, 'close');
    const lines = createInterface({input: supervisor.stdout});
    let forced = false;
    const timer = setTimeout(() => {forced = true; supervisor.stdin.end();}, 6000);
    try {
        const [line] = await once(lines, 'line');
        assert.deepEqual(posixWorkerChildren(supervisor.pid, process.execPath), [JSON.parse(line).pid]);
        assert.throws(() => posixWorkerChildren(supervisor.pid, fileURLToPath(import.meta.url)),
            /process observation targets the requested Worker executable/);
    } finally {
        supervisor.stdin.end();
        assert.deepEqual(await closed, [0, null], 'owned supervisor and child exit normally');
        clearTimeout(timer); lines.close();
    }
    assert.equal(forced, false, 'native observation finishes without deadline cleanup');
});
