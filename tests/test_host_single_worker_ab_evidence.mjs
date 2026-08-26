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
        // A missing perf binary fires 'error' (ENOENT), not 'exit'; without
        // this the promise never settles and the gate times out instead of
        // skipping (77).
        probe.on('error', () => resolve(false));
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

async function assertNoProcessContains(marker) {
    await new Promise((resolve) => setTimeout(resolve, 150));
    const output = await new Promise((resolve, reject) => {
        const child = spawn('ps', [ '-eo', 'args=' ],
            { stdio: [ 'ignore', 'pipe', 'pipe' ] });
        let stdout = '';
        child.stdout.setEncoding('utf8');
        child.stdout.on('data', (chunk) => { stdout += chunk; });
        child.on('error', reject);
        child.on('exit', (code) => code === 0
            ? resolve(stdout)
            : reject(new Error(`ps exited ${code}`)));
    });
    assert.ok(!output.includes(marker),
        `benchmark runner left a marked component process alive: ${marker}`);
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

// ---- GREEN: the documented quick-run flag skips every perf artifact while
// preserving the headline/correctness evidence contract. ----
{
    const result = await runnerOutput(args, {
        extraArgs: [ '--no-profile' ],
        env: { CAPSID_BENCH_TEST_MODE: '1' },
    });
    assert.equal(result.code, 0,
        `runner rejected --no-profile evidence: ${result.stderr}`);
    for (const file of [ 'manifest.json', 'samples.jsonl',
        'correctness.json', 'report.md' ]) {
        const full = path.join(result.out, file);
        assert.ok(fs.statSync(full).size > 0, `missing or empty ${file}`);
    }
    for (const file of [ 'baseline-gateway.pprof',
        'baseline-worker.perf.data', 'candidate-host.perf.data',
        'candidate-worker.perf.data' ]) {
        assert.ok(!fs.existsSync(path.join(result.out, file)),
            `--no-profile unexpectedly created ${file}`);
    }
}

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
    for (const side of [ 'baseline', 'candidate' ]) {
        const loadgenStat = path.join(statDir, `${side}-loadgen.stat`);
        assert.ok(fs.existsSync(loadgenStat) && fs.statSync(loadgenStat).size > 0,
            `missing ${side} loadgen CPU evidence`);
    }
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

// ---- GREEN: allocator/runtime A/Bs may use a different Worker binary per
// side. The runner must pass the correct path to each gateway and retain both
// identities instead of collapsing them into the legacy shared worker.
{
    const workerDir = fs.mkdtempSync(path.join(os.tmpdir(), 'capsid-workers-'));
    const baselineWorker = path.join(workerDir, 'baseline-worker');
    const candidateWorker = path.join(workerDir, 'candidate-worker');
    const fakeWorker = path.join(args.get('fake-dir'), 'fake-worker.sh');
    fs.copyFileSync(fakeWorker, baselineWorker);
    fs.copyFileSync(fakeWorker, candidateWorker);
    fs.appendFileSync(baselineWorker, '\n# baseline identity\n');
    fs.appendFileSync(candidateWorker, '\n# candidate identity\n');
    fs.chmodSync(baselineWorker, 0o755);
    fs.chmodSync(candidateWorker, 0o755);
    const result = await runnerOutput(args, {
        extraArgs: [
            '--baseline-worker', baselineWorker,
            '--candidate-worker', candidateWorker,
            '--no-profile',
        ],
    });
    assert.equal(result.code, 0,
        `runner rejected split Worker identities: ${result.stderr}`);
    const manifest = JSON.parse(fs.readFileSync(
        path.join(result.out, 'manifest.json'), 'utf8'));
    assert.equal(manifest.components.baseline_worker.cmd, baselineWorker);
    assert.equal(manifest.components.candidate_worker.cmd, candidateWorker);
    assert.notEqual(manifest.components.baseline_worker.sha256,
        manifest.components.candidate_worker.sha256,
        'the two distinct Worker builds collapsed to one identity');
    fs.rmSync(workerDir, { recursive: true, force: true });
}

// ---- RED: fewer than three rounds. ----
{
    const result = await runnerOutput(args, { extraArgs: [ '--rounds', '2' ] });
    assert.notEqual(result.code, 0, 'fewer than three rounds must be rejected');
    assert.match(result.stderr, /rounds must be at least 3/);
}

// ---- RED: A/B bundle/worker identity mismatch. ----
{
    const marker = `capsid-bench-child-${process.pid}-${Date.now()}`;
    const result = await runnerOutput(args, {
        env: {
            CAPSID_BENCH_FAKE_BAD_IDENTITY: '1',
            CAPSID_BENCH_FAKE_MARKER: marker,
        },
    });
    assert.notEqual(result.code, 0, 'identity mismatch must be rejected');
    assert.match(result.stderr, /identity does not match/);
    await assertNoProcessContains(marker);
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

// ---- RED: missing profile. Evidence is incomplete while the measured
// samples are complete — the acceptance verdict must be n/a in both the
// manifest and the report, never a pass/fail on partial evidence. ----
{
    const result = await runnerOutput(args, {
        env: { CAPSID_BENCH_FAKE_NO_PROFILE: '1' },
    });
    assert.notEqual(result.code, 0, 'missing profile must be rejected');
    assert.match(result.stderr, /missing or empty profile/);
    const manifest = JSON.parse(fs.readFileSync(
        path.join(result.out, 'manifest.json'), 'utf8'));
    assert.equal(manifest.evidence_status, 'incomplete',
        'missing profile must leave evidence incomplete');
    assert.equal(manifest.acceptance_verdict, 'n/a',
        'incomplete evidence must not carry a pass/fail verdict');
    const report = fs.readFileSync(path.join(result.out, 'report.md'), 'utf8');
    assert.match(report, /verdict: n\/a/i,
        'report must show n/a for incomplete evidence');
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

// ---- GREEN: multi-worker evidence is complete only when every configured
// shard has its own profile, perf-stat and resource row. This is the M2
// saturation-curve evidence contract; omitting worker.N resources from the
// derived report makes a pool profile impossible to interpret. ----
{
    const result = await runnerOutput(args, {
        extraArgs: [
            '--baseline-workers', '2',
            '--candidate-workers', '4',
            '--baseline-host-profile',
        ],
    });
    assert.equal(result.code, 0,
        `runner rejected complete pool evidence: ${result.stderr}`);
    const manifest = JSON.parse(fs.readFileSync(
        path.join(result.out, 'manifest.json'), 'utf8'));
    assert.equal(manifest.params.baseline_workers, 2);
    assert.equal(manifest.params.candidate_workers, 4);
    for (const [ side, workers ] of [
        [ 'baseline', 2 ],
        [ 'candidate', 4 ],
    ]) {
        for (let index = 1; index <= workers; index += 1) {
            const profile = `${side}-worker.${index}.perf.data`;
            const stat = path.join(
                result.out, 'perf-stat', `${side}-worker.${index}.stat`);
            assert.ok(fs.statSync(path.join(result.out, profile)).size > 0,
                `missing pool profile ${profile}`);
            assert.ok(fs.statSync(stat).size > 0,
                `missing pool perf-stat ${path.basename(stat)}`);
            assert.ok(manifest.files[profile],
                `pool profile digest missing for ${profile}`);
            assert.ok(manifest.resource[`${side}_worker.${index}`],
                `pool resource evidence missing for ${side} worker.${index}`);
        }
    }
    const report = fs.readFileSync(path.join(result.out, 'report.md'), 'utf8');
    const resourceSection = report.split(
        '## CPU/response and resources (profile runs)')[1]?.split(
        '## perf-stat summary')[0] ?? '';
    assert.match(resourceSection, /\| baseline \| worker\.1 \|/,
        'report omits baseline pool-worker resources');
    assert.match(resourceSection, /\| baseline \| worker\.2 \|/,
        'report omits the final baseline pool-worker resources');
    assert.match(resourceSection, /\| candidate \| worker\.1 \|/,
        'report omits candidate pool-worker resources');
    assert.match(resourceSection, /\| candidate \| worker\.4 \|/,
        'report omits the final candidate pool-worker resources');
    const perfStatSection = report.split('## perf-stat summary')[1]?.split(
        '## Dominant stacks')[0] ?? '';
    assert.match(perfStatSection, /\| baseline \| loadgen \|/,
        'report omits baseline loadgen CPU');
    assert.match(perfStatSection, /\| candidate \| loadgen \|/,
        'report omits candidate loadgen CPU');
}

// ---- RED: the profile process tree must contain exactly the configured
// number of direct worker children. Accepting an extra child and profiling
// only the first N makes CPU/PSS evidence incomplete and can hide a leaked
// or accidentally active shard. ----
{
    const result = await runnerOutput(args, {
        extraArgs: [
            '--baseline-workers', '2',
            '--candidate-workers', '2',
        ],
        env: { CAPSID_BENCH_FAKE_EXTRA_WORKER: '1' },
    });
    assert.notEqual(result.code, 0,
        'an extra direct worker child must invalidate pool evidence');
    assert.match(result.stderr,
        /expected 2 worker processes .* found 3/,
        'runner did not report the exact worker-count mismatch');
}

// ---- GREEN: an optimization A/B may compare two Host builds, so the
// runner must bind each side to its actual Host binary and record both
// identities independently. A single shared --host-bin identity cannot
// prove which implementation produced either side's samples. ----
{
    const baselineHost = path.join(args.get('build-dir'), 'capsid-host');
    const candidateHost = path.join(args.get('build-dir'), 'capsid-worker');
    assert.ok(fs.existsSync(baselineHost) && fs.existsSync(candidateHost),
        'host identity fixtures are missing from the build directory');
    const result = await runnerOutput(args, {
        extraArgs: [
            '--baseline-host-bin', baselineHost,
            '--candidate-host-bin', candidateHost,
            '--baseline-host-profile',
        ],
    });
    assert.equal(result.code, 0,
        `runner rejected distinct Host build identities: ${result.stderr}`);
    const manifest = JSON.parse(fs.readFileSync(
        path.join(result.out, 'manifest.json'), 'utf8'));
    const baselineIdentity = manifest.components.baseline_host_bin;
    const candidateIdentity = manifest.components.candidate_host_bin;
    assert.equal(baselineIdentity?.cmd, baselineHost,
        'baseline Host path is not bound to its side');
    assert.equal(candidateIdentity?.cmd, candidateHost,
        'candidate Host path is not bound to its side');
    assert.match(baselineIdentity?.sha256 ?? '', /^[0-9a-f]{64}$/,
        'baseline Host digest is missing');
    assert.match(candidateIdentity?.sha256 ?? '', /^[0-9a-f]{64}$/,
        'candidate Host digest is missing');
    assert.notEqual(baselineIdentity.sha256, candidateIdentity.sha256,
        'the two distinct Host builds collapsed to one identity');
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
