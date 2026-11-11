import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { execFileSync } from 'node:child_process';
import { test } from 'node:test';
import { auditSource } from './audit-source.mjs';

test('a pinned HEAD cannot conceal changed WPT inputs', t => {
    const root = fs.mkdtempSync(path.join(os.tmpdir(), 'capsid-wpt-source-'));
    t.after(() => fs.rmSync(root, { recursive: true, force: true }));
    const git = args => execFileSync('git', ['-C', root, ...args], { encoding: 'utf8', stdio: ['ignore', 'pipe', 'pipe'] });
    git(['init', '-q']);
    fs.writeFileSync(path.join(root, 'test.js'), 'assert_true(false);\n');
    fs.writeFileSync(path.join(root, 'resource.json'), '{"value":1}\n');
    git(['add', '.']);
    git(['-c', 'user.name=Test', '-c', 'user.email=test@example.invalid', '-c', 'commit.gpgsign=false', 'commit', '-qm', 'fixture']);
    const revision = git(['rev-parse', 'HEAD']).trim();
    const inputs = ['test.js', 'resource.json'];
    assert.equal(auditSource(root, revision, inputs).files, 2);
    fs.writeFileSync(path.join(root, 'test.js'), 'assert_true(true);\n');
    // This is the old configure-time check: it stays green after an assertion changes.
    assert.equal(git(['rev-parse', 'HEAD']).trim(), revision);
    assert.throws(() => auditSource(root, revision, inputs), /differs from locked blob: test.js/);
    git(['add', 'test.js']);
    assert.throws(() => auditSource(root, revision, inputs), /differs from locked blob: test.js/);
    git(['reset', '-q', '--', 'test.js']);
    git(['update-index', '--assume-unchanged', 'test.js']);
    assert.throws(() => auditSource(root, revision, inputs), /differs from locked blob: test.js/);
    fs.writeFileSync(path.join(root, 'test.js'), 'assert_true(false);\n');
    fs.writeFileSync(path.join(root, 'resource.json'), '{"value":2}\n');
    assert.throws(() => auditSource(root, revision, inputs), /differs from locked blob: resource.json/);
    fs.rmSync(path.join(root, 'resource.json'));
    assert.throws(() => auditSource(root, revision, inputs), /ENOENT/);
    assert.throws(() => auditSource(root, '0'.repeat(40), inputs), /HEAD differs/);
    assert.throws(() => auditSource(root, revision, []), /must not be empty/);
    assert.throws(() => auditSource(root, revision, ['../outside']), /invalid WPT input/);
});
