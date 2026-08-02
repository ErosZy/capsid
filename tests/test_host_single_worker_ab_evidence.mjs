// Frozen RED test for the M1B A/B benchmark runner
// (bench/run-ab.sh, see design review §15.7 M1-perf).
//
// The runner is validated against fake baseline/candidate/loadgen components
// before any real process is attached. The GREEN scenario asserts the fixed
// evidence layout is produced; every RED scenario asserts the runner refuses
// to report incomplete evidence:
//
//   - fewer than three rounds per side;
//   - A/B bundle/worker identity mismatch;
//   - correctness or negative-control failure;
//   - a missing gateway/host or worker profile;
//   - missing raw samples, commit, build arguments or file SHA-256;
//   - error responses counted as success.
//
// The test skips (exit 77) when perf is not usable, because the runner's
// profile runs require it; the evidence gate would otherwise reject
// everything for environmental reasons.

import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const testDir = path.dirname(fileURLToPath(import.meta.url));
const repoRoot = path.resolve(testDir, '..');

function parseArgs(argv) {
    const values = new Map();
    for (let index = 0; index < argv.length; index += 2) {
        const key = argv[index];
        const value = argv[index + 1];
        assert.ok(key?.startsWith('--') && value, `invalid argument: ${key}`);
        values.set(key.slice(2), value);
    }
    for (const required of [ 'runner', 'fake-dir', 'bundle', 'build-dir' ]) {
        assert.ok(values.has(required), `missing --${required}`);
    }
    return values;
}

function perfUsable() {
    return new Promise((resolve) => {
        const probe = spawn('perf', [ 'stat', '-e', 'task-clock', 'true' ],
            { stdio: 'ignore' });
        probe.on('exit', (code) => resolve(code === 0));
    });
}

// Runs the runner once with the given extra arguments and environment
// overrides, returning { code, stderr, out }. CAPSID_BENCH_TEST_MODE is
// set by default (the working tree is dirty during development); the
// dirty-tree RED scenario clears it.
async function runnerOutput(args, { extraArgs = [], env = {}, runnerPath = null } = {}) {
    env = { CAPSID_BENCH_TEST_MODE: '1', ...env };
    const out = fs.mkdtempSync(path.join(os.tmpdir(), 'capsid-ab-'));
    return new Promise((resolve) => {
        const child = spawn(runnerPath ?? args.get('runner'), [
            '--out', out,
            '--run-id', 'test',
            '--baseline', path.join(args.get('fake-dir'), 'fake-gateway.sh'),
            '--candidate', path.join(args.get('fake-dir'), 'fake-gateway.sh'),
            '--worker', path.join(args.get('fake-dir'), 'fake-worker.sh'),
            '--worker-name', 'fake-worker',
            '--loadgen', path.join(args.get('fake-dir'), 'fake-loadgen.sh'),
            '--bundle', args.get('bundle'),
            '--build-dir', args.get('build-dir'),
            '--duration', '1',
            '--warmup', '0',
            ...extraArgs,
        ], {
            env: { ...process.env, ...env },
            stdio: [ 'ignore', 'pipe', 'pipe' ],
        });
        let stderr = '';
        child.stderr.setEncoding('utf8');
        child.stderr.on('data', (chunk) => { stderr += chunk; });
        child.on('exit', (code) => resolve({ code, stderr, out }));
    });
}

const args = parseArgs(process.argv.slice(2));

if (!(await perfUsable())) {
    console.log('SKIP: perf is not usable in this environment');
    process.exit(77);
}

const requiredFiles = [
    'manifest.json',
    'samples.jsonl',
    'correctness.json',
    'baseline-gateway.pprof',
    'baseline-worker.perf.data',
    'candidate-host.perf.data',
    'candidate-worker.perf.data',
    'report.md',
];

// ---- GREEN: fake components produce complete evidence. The working tree
// is dirty during development, so the test exempts the dirty-tree check
// (production runs never set the exemption). ----
{
    const result = await runnerOutput(args, {
        env: { CAPSID_BENCH_TEST_MODE: '1' },
    });
    assert.equal(result.code, 0, `runner rejected complete evidence: ${result.stderr}`);
    for (const file of requiredFiles) {
        const full = path.join(result.out, file);
        assert.ok(fs.statSync(full).size > 0, `missing or empty ${file}`);
    }
    const statDir = path.join(result.out, 'perf-stat');
    assert.ok(fs.readdirSync(statDir).length > 0, 'perf-stat/ is empty');
    const manifest = JSON.parse(fs.readFileSync(path.join(result.out, 'manifest.json'), 'utf8'));
    // TEST_MODE=1 runs produce "diagnostic" status, not "complete".
    assert.equal(manifest.evidence_status, 'diagnostic');
    assert.notEqual(manifest.commit, 'unknown', 'commit not recorded');
    assert.notEqual(manifest.build_args, '{}', 'build arguments not recorded');
    assert.ok(manifest.files['samples.jsonl'], 'samples digest missing from manifest');
    // Exactly one headline measured sample per side and round (1..3); the
    // profile runs (round 0) carry their own samples and are not headline.
    const sampleLines = fs.readFileSync(path.join(result.out, 'samples.jsonl'), 'utf8')
        .trim().split('\n').filter((line) => line.includes('"phase":"measured"'));
    for (const side of [ 'baseline', 'candidate' ]) {
        for (let round = 1; round <= 3; round += 1) {
            const matches = sampleLines.filter((line) =>
                line.includes(`"side":"${side}"`) && line.includes(`"round":${round},`));
            assert.equal(matches.length, 1,
                `expected one measured sample for ${side} round ${round}`);
        }
    }
}

// ---- RED: fewer than three rounds. ----
{
    const result = await runnerOutput(args, { extraArgs: [ '--rounds', '2' ] });
    assert.notEqual(result.code, 0, 'fewer than three rounds must be rejected');
    assert.match(result.stderr, /rounds must be at least 3/);
}

// ---- RED: A/B bundle/worker identity mismatch. ----
{
    const result = await runnerOutput(args, {
        env: { CAPSID_BENCH_FAKE_BAD_IDENTITY: '1' },
    });
    assert.notEqual(result.code, 0, 'identity mismatch must be rejected');
    assert.match(result.stderr, /identity does not match/);
}

// ---- RED: missing identity report. ----
{
    const result = await runnerOutput(args, {
        env: { CAPSID_BENCH_FAKE_NO_IDENTITY: '1' },
    });
    assert.notEqual(result.code, 0, 'missing identity must be rejected');
    assert.match(result.stderr, /did not report its bundle\/worker identity/);
}

// ---- RED: correctness failure. ----
{
    const result = await runnerOutput(args, {
        env: { CAPSID_BENCH_FAKE_CORRECTNESS_FAIL: '1' },
    });
    assert.notEqual(result.code, 0, 'correctness failure must be rejected');
    assert.match(result.stderr, /correctness failed/);
}

// ---- RED: error responses counted as success. ----
{
    const result = await runnerOutput(args, {
        env: { CAPSID_BENCH_FAKE_ERRORS_AS_SUCCESS: '1' },
    });
    assert.notEqual(result.code, 0, 'errors-as-success must be rejected');
    assert.match(result.stderr, /errors counted as success/);
}

// ---- RED: missing profile. ----
{
    const result = await runnerOutput(args, {
        env: { CAPSID_BENCH_FAKE_NO_PROFILE: '1' },
    });
    assert.notEqual(result.code, 0, 'missing profile must be rejected');
    assert.match(result.stderr, /missing or empty profile/);
}

// ---- RED: missing raw samples. ----
{
    const result = await runnerOutput(args, {
        env: { CAPSID_BENCH_FAKE_FEWER_ROUNDS: '1' },
    });
    assert.notEqual(result.code, 0, 'missing measured samples must be rejected');
    assert.match(result.stderr, /missing measured sample/);
}

// ---- GREEN: frozen statistic + per-side env + IPC mechanism counters. The
// fakes emit metrics lines via CAPSID_BENCH_FAKE_METRICS; the runner must
// capture the profile-run window counters into the manifest. ----
{
    const result = await runnerOutput(args, {
        extraArgs: [
            '--statistic', 'median',
            '--baseline-env', 'CAPSID_BENCH_FAKE_METRICS=1',
            '--candidate-env', 'CAPSID_BENCH_FAKE_METRICS=1',
            '--require-ipc-counters',
        ],
    });
    assert.equal(result.code, 0,
        `runner rejected counter evidence: ${result.stderr}`);
    const manifest = JSON.parse(fs.readFileSync(
        path.join(result.out, 'manifest.json'), 'utf8'));
    assert.equal(manifest.params.statistic, 'median',
        'frozen statistic not recorded in manifest');
    assert.equal(manifest.params.baseline_env, 'CAPSID_BENCH_FAKE_METRICS=1',
        'baseline env not recorded in manifest');
    assert.equal(manifest.params.candidate_env, 'CAPSID_BENCH_FAKE_METRICS=1',
        'candidate env not recorded in manifest');
    assert.equal(manifest.params.require_ipc_counters, true,
        'require_ipc_counters not recorded');
    // Acceptance verdict is a separate field from evidence_status: complete
    // evidence must not be mistaken for a PASS.
    assert.ok([ 'pass', 'fail', 'n/a' ].includes(manifest.acceptance_verdict),
        'acceptance_verdict missing from manifest');
    for (const side of [ 'baseline', 'candidate' ]) {
        const counters = manifest.ipc_mechanism?.[side]?.counters ?? {};
        assert.ok(counters['host.commands_submitted'] > 0,
            `${side} IPC mechanism counters missing from manifest`);
    }
    const report = fs.readFileSync(path.join(result.out, 'report.md'), 'utf8');
    assert.match(report, /frozen statistic: median/,
        'report does not state the frozen statistic');
    assert.match(report, /IPC mechanism counters/,
        'report has no IPC mechanism counter table');
}

// ---- GREEN: --baseline-host-profile. The fake suppresses its own pprof
// (CAPSID_BENCH_FAKE_NO_PROFILE), so the baseline gateway profile can only
// come from the runner's perf record of the baseline process. ----
{
    const result = await runnerOutput(args, {
        extraArgs: [
            '--baseline-host-profile',
            '--candidate-env', 'CAPSID_BENCH_FAKE_METRICS=1',
        ],
        env: { CAPSID_BENCH_FAKE_NO_PROFILE: '1' },
    });
    assert.equal(result.code, 0,
        `runner rejected baseline-host-profile evidence: ${result.stderr}`);
    const baselinePerf = path.join(result.out, 'baseline-gateway.perf.data');
    assert.ok(fs.statSync(baselinePerf).size > 0,
        'baseline-gateway.perf.data missing or empty');
    const manifest = JSON.parse(fs.readFileSync(
        path.join(result.out, 'manifest.json'), 'utf8'));
    assert.equal(manifest.params.baseline_host_profile, true,
        'baseline_host_profile not recorded in manifest');
    assert.ok(manifest.files['baseline-gateway.perf.data'],
        'baseline-gateway.perf.data digest missing from manifest');
}

// ---- RED: --require-ipc-counters with no counters emitted. ----
{
    const result = await runnerOutput(args, {
        extraArgs: [ '--require-ipc-counters' ],
    });
    assert.notEqual(result.code, 0,
        'missing IPC mechanism counters must be rejected');
    assert.match(result.stderr, /no IPC mechanism counters/);
}

// ---- RED: dirty working tree (no exemption). The test dirties the tree
// itself (an untracked marker file at the repo root) so it does not depend
// on the checkout being dirty; the runner must refuse. ----
{
    const marker = path.join(repoRoot, '.dirty-marker-test');
    fs.writeFileSync(marker, 'dirty');
    try {
        const result = await runnerOutput(args, {
            env: { CAPSID_BENCH_TEST_MODE: '' },
        });
        assert.notEqual(result.code, 0, 'dirty working tree must be rejected');
        assert.match(result.stderr, /dirty working tree/);
    } finally {
        fs.rmSync(marker, { force: true });
    }
}

// ---- RED: build arguments not recorded. ----
{
    const result = await runnerOutput(args, {
        extraArgs: [],
        runnerPath: args.get('runner'), // no --build-dir
    });
    // runnerOutput always passes --build-dir; run without it directly.
    const out = fs.mkdtempSync(path.join(os.tmpdir(), 'capsid-ab-'));
    const noBuild = await new Promise((resolve) => {
        const child = spawn(args.get('runner'), [
            '--out', out,
            '--run-id', 'test',
            '--baseline', path.join(args.get('fake-dir'), 'fake-gateway.sh'),
            '--candidate', path.join(args.get('fake-dir'), 'fake-gateway.sh'),
            '--worker', path.join(args.get('fake-dir'), 'fake-worker.sh'),
            '--worker-name', 'fake-worker',
            '--loadgen', path.join(args.get('fake-dir'), 'fake-loadgen.sh'),
            '--bundle', args.get('bundle'),
            '--duration', '1',
            '--warmup', '0',
        ], {
            env: { ...process.env },
            stdio: [ 'ignore', 'pipe', 'pipe' ],
        });
        let stderr = '';
        child.stderr.setEncoding('utf8');
        child.stderr.on('data', (chunk) => { stderr += chunk; });
        child.on('exit', (code) => resolve({ code, stderr }));
    });
    assert.notEqual(noBuild.code, 0, 'missing build arguments must be rejected');
    assert.match(noBuild.stderr, /build arguments not recorded/);
}

// ---- RED: commit not resolvable. The runner is copied outside the git
// repository so `git rev-parse` fails. ----
{
    const strayDir = fs.mkdtempSync(path.join(os.tmpdir(), 'capsid-stray-'));
    const strayRunner = path.join(strayDir, 'run-ab.sh');
    fs.copyFileSync(args.get('runner'), strayRunner);
    fs.chmodSync(strayRunner, 0o755);
    // The evidence generator lives next to the runner.
    fs.copyFileSync(path.join(path.dirname(args.get('runner')), 'evidence.py'),
        path.join(strayDir, 'evidence.py'));
    const result = await runnerOutput(args, {
        runnerPath: strayRunner,
    });
    fs.rmSync(strayDir, { recursive: true, force: true });
    assert.notEqual(result.code, 0, 'unresolvable commit must be rejected');
    assert.match(result.stderr, /commit not resolvable/);
}

console.log('PASS');
