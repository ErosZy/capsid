import assert from 'node:assert/strict';
import {execFileSync} from 'node:child_process';
import fs from 'node:fs';

export function parseDirectChildren(table, parentPid) {
    assert.ok(Number.isSafeInteger(parentPid) && parentPid > 0, 'actual parent PID required');
    const children = [], seen = new Set();
    for (const line of table.split('\n')) {
        if (!line.trim()) continue;
        const row = /^\s*(\d+)\s+(\d+)\s+(\S.*)$/.exec(line);
        assert.ok(row, 'complete ps PID, PPID and executable row required');
        const pid = Number(row[1]), parent = Number(row[2]);
        assert.ok(Number.isSafeInteger(pid) && pid > 0 && Number.isSafeInteger(parent), 'valid ps process IDs');
        assert.ok(!seen.has(pid), 'unique ps process ID');
        seen.add(pid);
        if (parent === parentPid) children.push({pid, executable: row[3]});
    }
    return children;
}

export function posixWorkerChildren(parentPid, worker) {
    // Darwin ps does not implement -P. Take one full, untruncated snapshot,
    // select direct children by PPID, then verify every executable path.
    const table = execFileSync('ps', ['-A', '-ww', '-o', 'pid=,ppid=,comm='],
        {encoding: 'utf8', timeout: 6000, maxBuffer: 8 * 1024 * 1024});
    const executable = fs.realpathSync(worker);
    return parseDirectChildren(table, parentPid).map(child => {
        // Linux comm is a short task name, while Darwin comm is a path.
        const actual = process.platform === 'linux' ? `/proc/${child.pid}/exe` : child.executable;
        assert.equal(fs.realpathSync(actual), executable,
            'process observation targets the requested Worker executable');
        return child.pid;
    });
}
