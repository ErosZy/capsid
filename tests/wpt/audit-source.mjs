import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

export function auditSource(root, revision, inputs) {
    assert.match(revision, /^[0-9a-f]{40}$/, 'WPT revision must be a full commit');
    const git = args => execFileSync('git', ['-C', root, ...args], { maxBuffer: 64 * 1024 * 1024 });
    assert.equal(git(['rev-parse', 'HEAD']).toString().trim(), revision, 'WPT HEAD differs from lock');
    assert.ok(inputs.length > 0, 'WPT input list must not be empty');
    const files = [...new Set(inputs)].sort();
    for (const file of files) {
        assert.ok(file && !path.isAbsolute(file) && !file.includes('\\') &&
            !file.split('/').some(part => !part || part === '.' || part === '..'), `invalid WPT input: ${file}`);
        const actual = path.join(root, file);
        assert.ok(fs.lstatSync(actual).isFile(), `WPT input must be a regular file: ${file}`);
        const expected = git(['show', `${revision}:${file}`]);
        assert.ok(fs.readFileSync(actual).equals(expected), `WPT input differs from locked blob: ${file}`);
    }
    return { revision, files: files.length };
}

if (process.argv[1] && fs.realpathSync(process.argv[1]) === fs.realpathSync(fileURLToPath(import.meta.url))) {
    const [root, revision, list] = process.argv.slice(2);
    assert.ok(root && revision && list, 'usage: audit-source.mjs ROOT REVISION INPUT_LIST');
    const inputs = fs.readFileSync(list, 'utf8').trim().split(/\r?\n/).filter(Boolean);
    console.log(JSON.stringify(auditSource(root, revision, inputs)));
}
