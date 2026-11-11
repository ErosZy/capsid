import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { test } from 'node:test';

const root = fs.realpathSync(fileURLToPath(new URL('..', import.meta.url)));
const scripts = [
    'tests/wpt/audit-metadata.mjs',
    'tests/wpt/audit-source.mjs',
    'tools/workerd-reference.mjs',
    'tools/run-workerd-ws-gate.mjs',
];
for (const script of scripts) {
    for (const alias of [false, true]) {
        test(`${script}: ${alias ? 'symlink' : 'direct'} entry rejects missing arguments`, t => {
            const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'capsid-cli-entry-'));
            t.after(() => fs.rmSync(temporary, { recursive: true, force: true }));
            let directory = root;
            if (alias) {
                directory = path.join(temporary, 'checkout');
                fs.symlinkSync(root, directory, process.platform === 'win32' ? 'junction' : 'dir');
            }
            const result = spawnSync(process.execPath, [path.join(directory, script)],
                { encoding: 'utf8', timeout: 10000 });
            assert.equal(result.error, undefined);
            assert.equal(result.signal, null);
            assert.equal(result.status, 1, 'the CLI must execute its argument validation');
            assert.match(result.stderr, /usage:/, 'failure must come from the expected CLI contract');
        });
    }
}
