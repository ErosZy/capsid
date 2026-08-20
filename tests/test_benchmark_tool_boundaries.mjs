import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawnSync } from 'node:child_process';

const args = new Map();
for (let index = 2; index < process.argv.length; index += 2) {
    args.set(process.argv[index], process.argv[index + 1]);
}

const summarizer = args.get('--summarizer');
const profiler = args.get('--profiler');
assert.ok(summarizer, '--summarizer is required');
assert.ok(profiler, '--profiler is required');

const work = fs.mkdtempSync(path.join(os.tmpdir(), 'capsid-benchmark-tools-'));
try {
    const sample = {
        phase: 'measured',
        qps: 0,
        p50_ms: 0,
        p95_ms: 0,
        p99_ms: 0,
        errors: 1,
        timeouts: 0,
    };
    fs.writeFileSync(
        path.join(work, 'samples.capsid.json16k.c64.r1.jsonl'),
        `${JSON.stringify(sample)}\n${JSON.stringify(sample)}\n`);

    const result = spawnSync('python3', [summarizer, work], {
        encoding: 'utf8',
    });
    assert.equal(result.status, 0,
        `zero-QPS input crashed the summarizer:\n${result.stderr}`);
    assert.match(result.stdout, /capsid\s+json16k\s+0\s+0\.0/,
        'zero-QPS input must report a finite 0.0 CV');

    // actions/checkout uses native CRLF line endings on Windows. These are
    // source-shape assertions, so normalize line endings before matching the
    // same shell structure on every host platform.
    const source = fs.readFileSync(profiler, 'utf8').replace(/\r\n?/g, '\n');
    assert.match(source, /trap cleanup_components EXIT INT TERM/,
        'profiling must clean up every started component on all exits');
    assert.match(source, /profile_loadgen[\s\S]*?>[^\n]*&\nLOADGEN_PID=\$!\n/,
        'load generation must run concurrently with profiling');
    assert.match(source, /wait "\$LOADGEN_PID"/,
        'load-generator failure must be observed');
    assert.doesNotMatch(source, /profile_loadgen[^\n]*\|\| true/,
        'load-generator failure must not be discarded');
} finally {
    fs.rmSync(work, { recursive: true, force: true });
}

console.log('benchmark tool boundaries: PASS');
