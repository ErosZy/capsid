import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawnSync } from 'node:child_process';

const args = new Map();
for (let index = 2; index < process.argv.length; index += 2) {
    args.set(process.argv[index], process.argv[index + 1]);
}

const collector = path.resolve(args.get('--collector') ?? '');
const qpsLane = path.resolve(args.get('--qps-lane') ?? '');
assert.ok(collector, '--collector is required');
assert.ok(qpsLane, '--qps-lane is required');

const work = fs.mkdtempSync(path.join(os.tmpdir(), 'capsid-benchmark-tools-'));
try {
    // The collector must ingest a zero-QPS round without crashing and must
    // carry the boundary through to the data point as a finite 0.0 cell.
    fs.mkdirSync(path.join(work, 'cold'));
    fs.mkdirSync(path.join(work, 'qps'));
    fs.writeFileSync(path.join(work, 'cold', 'cells.txt'),
        'capsid src 10k total_ms=1.0 ready_ms=0.5 warm_ms=0.1 pss_mb=1.0\n');
    fs.writeFileSync(path.join(work, 'qps', 'cells.txt'),
        'capsid matrix-json-16k   qps_med= 0 p50_ms=0 p95_ms=0\n' +
        'capsid                        serving_pss_mb=1.0\n' +
        'capsid                        serving_pss_detail=host=0.5;worker=0.5\n');

    const result = spawnSync('python3', [
        collector,
        '--cold-start', path.join(work, 'cold'),
        '--qps', path.join(work, 'qps'),
        '--version', 't',
        '--release', 't',
        '--data', path.join(work, 'data.js'),
    ], { encoding: 'utf8', cwd: work });
    assert.equal(result.status, 0,
        `zero-QPS input crashed the collector:\n${result.stderr}`);

    const data = fs.readFileSync(path.join(work, 'data.js'), 'utf8');
    assert.match(data, /"qps": 0\.0/,
        'zero-QPS round must survive as a finite 0.0 cell');
    assert.match(data, /"mb": 0\.5/,
        'serving PSS breakdown must be carried into the point');
    assert.match(data, /"totalMs": 1\.0/,
        'cold-start cells must be carried into the point');

    // actions/checkout uses native CRLF line endings on Windows. These are
    // source-shape assertions, so normalize line endings before matching the
    // same shell structure on every host platform.
    const source = fs.readFileSync(qpsLane, 'utf8').replace(/\r\n?/g, '\n');
    assert.match(source, /trap cleanup EXIT/,
        'the lane must clean up every started component on all exits');
    assert.match(source, /set -euo pipefail/,
        'the lane must abort on the first failed command or unbound variable');
    assert.match(source,
        /require_free_port 9102 18993 18994 18996 18997 18998/,
        'the lane must fail closed when a serving port is already taken');
    assert.match(source, /correctness FAILED/,
        'a wrong response body must fail the round');
    assert.match(source, /no sample/,
        'a round without measured samples must fail the lane');
    assert.match(source, /grep -q '"ok"\[\[:space:\]\]\*:\[\[:space:\]\]\*true'/,
        'the round verdict must be gated on the loadgen ok flag');
    assert.doesNotMatch(source, /\$LOADGEN[^\n]*\|\| true/,
        'load-generator failure must not be discarded');
} finally {
    fs.rmSync(work, { recursive: true, force: true });
}

console.log('benchmark tool boundaries: PASS');
