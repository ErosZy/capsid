import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawnSync } from 'node:child_process';
import vm from 'node:vm';

const args = new Map();
for (let index = 2; index < process.argv.length; index += 2) {
    args.set(process.argv[index], process.argv[index + 1]);
}

for (const name of ['--collector', '--qps-lane', '--python'])
    assert.ok(args.get(name), `${name} is required`);
const collector = path.resolve(args.get('--collector'));
const qpsLane = path.resolve(args.get('--qps-lane'));
const python = args.get('--python');
assert.ok(path.isAbsolute(python), '--python must name the configured interpreter');

const work = fs.mkdtempSync(path.join(os.tmpdir(), 'capsid-benchmark-tools-'));
try {
    // Test the parser independently. A zero-QPS syntax fixture is not a
    // complete measurement archive and must never qualify publication.
    fs.mkdirSync(path.join(work, 'cold'));
    fs.mkdirSync(path.join(work, 'qps'));
    fs.writeFileSync(path.join(work, 'cold', 'cells.txt'),
        'capsid src 10k total_ms=1.0 ready_ms=0.5 warm_ms=0.1 pss_mb=1.0\n');
    fs.writeFileSync(path.join(work, 'qps', 'cells.txt'),
        'capsid matrix-json-16k   qps_med= 0 p50_ms=0 p95_ms=0\n' +
        'capsid                        serving_pss_mb=1.0\n' +
        'capsid                        serving_pss_detail=host=0.5;worker=0.5\n');

    const result = spawnSync(python, ['-B', '-c', [
        'import importlib.util, json, pathlib, sys',
        'sys.path.insert(0, str(pathlib.Path(sys.argv[1]).parent))',
        'spec = importlib.util.spec_from_file_location("collector", sys.argv[1])',
        'module = importlib.util.module_from_spec(spec)',
        'spec.loader.exec_module(module)',
        'print(json.dumps({"coldStart": module.parse_cold_start(sys.argv[2]), "qps": module.parse_qps(sys.argv[3])}, allow_nan=False))',
    ].join('\n'), collector, path.join(work, 'cold'), path.join(work, 'qps')],
    { encoding: 'utf8', cwd: work, timeout: 10000, windowsHide: true });
    assert.ifError(result.error);
    assert.equal(result.status, 0,
        `zero-QPS parser failed: code=${result.status}, signal=${result.signal}\n${result.stderr}`);

    const data = result.stdout;
    assert.match(data, /"qps": 0\.0/,
        'zero-QPS round must survive as a finite 0.0 cell');
    assert.match(data, /"mb": 0\.5/,
        'serving PSS breakdown must be carried into the point');
    assert.match(data, /"totalMs": 1\.0/,
        'cold-start cells must be carried into the point');
    const publication = spawnSync(python, ['-B', collector,
        '--cold-start', path.join(work, 'cold'), '--qps', path.join(work, 'qps'),
        '--version', 't', '--release', 't', '--repo', work,
        '--generated-at', '2026-10-20T00:00:00Z', '--data', path.join(work, 'data.js'),
    ], { encoding: 'utf8', cwd: work, timeout: 10000, windowsHide: true });
    assert.ifError(publication.error);
    assert.equal(publication.status, 1, publication.stderr);
    assert.match(publication.stderr, /publication archives must live under bench\/results/);
    assert.ok(!fs.existsSync(path.join(work, 'data.js')),
        'parser-only fixtures must not produce a published data point');

    // actions/checkout uses native CRLF line endings on Windows. These are
    // source-shape assertions, so normalize line endings before matching the
    // same shell structure on every host platform.
    const source = fs.readFileSync(qpsLane, 'utf8').replace(/\r\n?/g, '\n');
    assert.match(source, /set -euo pipefail/,
        'the lane must abort on the first failed command or unbound variable');
    assert.match(source, /exec "\$\{CAPSID_BENCH_PYTHON:-python3\}" -B "\$SCRIPT_DIR\/qps-lane\.py" "\$@"/,
        'the public shell must pass status and signals to the owned controller');
    const qpsPreflight = spawnSync(python, ['-B', path.join(path.dirname(qpsLane), 'qps-lane.py'),
        '--out', path.join(work, 'qps-must-not-launch')],
    { encoding: 'utf8', cwd: work, timeout: 10000, windowsHide: true,
        env: { ...process.env, CAPSID_BENCH_METADATA: '', STACKS: 'workerd' } });
    assert.ifError(qpsPreflight.error);
    assert.equal(qpsPreflight.status, 2, qpsPreflight.stderr);
    assert.match(qpsPreflight.stderr, /formal QPS requires|requires Linux/);
    assert.ok(!fs.existsSync(path.join(work, 'qps-must-not-launch')),
        'incomplete formal protocol must fail before any output or process');
    // Process/port ownership, failed rounds and cancellation are executed by
    // the actual Python controller/probe tests instead of shell source grep.

    // Execute the actual page script with its archived point. This checks
    // status and link behavior; browser layout is qualified separately.
    const siteRoot = path.resolve(path.dirname(collector), '..', 'perf-site');
    const html = fs.readFileSync(path.join(siteRoot, 'index.html'), 'utf8');
    const script = html.match(/<script>([\s\S]*?)<\/script>/)[1];
    const dataContext = { window: {} };
    vm.runInNewContext(fs.readFileSync(path.join(siteRoot, 'data.js'), 'utf8'), dataContext);
    function render(data) {
        const elements = new Map();
        const context = {
            window: { CAPSID_PERF_DATA: data },
            document: {
                body: {},
                getElementById(id) {
                    if (!elements.has(id)) elements.set(id, { textContent: '', innerHTML: '', style: {} });
                    return elements.get(id);
                },
                createElement() {
                    return { getContext: () => ({ measureText: s => ({ width: String(s).length * 8 }) }) };
                },
            },
            getComputedStyle: () => ({ fontFamily: 'sans-serif' }),
        };
        vm.runInNewContext(script, context, { timeout: 5000 });
        return elements;
    }
    const historical = JSON.parse(JSON.stringify(dataContext.window.CAPSID_PERF_DATA));
    const oldPoint = historical.points[historical.points.length - 1];
    delete oldPoint.verification;
    let elements = render(historical);
    assert.match(elements.get('nav-meta').textContent, /unverified/);
    assert.match(elements.get('method-card').innerHTML, /checks have not passed/);
    assert.doesNotMatch(elements.get('method-card').innerHTML, /href="evidence\//);
    assert.match(elements.get('warm-note').innerHTML, /curl/);
    assert.match(elements.get('warm-chart').innerHTML, /floor \(unresolved\)/);
    const complete = JSON.parse(JSON.stringify(historical));
    const newPoint = complete.points[complete.points.length - 1];
    newPoint.verification = { status: 'verified', contract: 'capsid-perf-lane-v1' };
    newPoint.evidence = { 'bench/results/synthetic fixture/raw.jsonl': '1'.repeat(64) };
    newPoint.density = { capsid: { 'cold@10k': { count: 2, pssMb: 1, unit: 'workers', limit: 'cap' } } };
    elements = render(complete);
    assert.doesNotMatch(elements.get('nav-meta').textContent, /unverified/);
    assert.match(elements.get('method-card').innerHTML,
        /href="evidence\/bench\/results\/synthetic%20fixture\/raw.jsonl"/);
    assert.match(elements.get('verification-note').textContent, /checks passed/);
    assert.match(elements.get('budget-table').innerHTML, /cap reached/);
    assert.doesNotMatch(elements.get('budget-table').innerHTML, /largest pool|gap widens|never returns/);
    newPoint.origins = { 'cold-start': { protocol: { httpClient: 'python-http-exact-v1' } } };
    elements = render(complete);
    assert.match(elements.get('warm-note').innerHTML, /C-ABI IPC/);
    assert.match(elements.get('warm-note').innerHTML, /loopback HTTP/);
    assert.doesNotMatch(elements.get('warm-chart').innerHTML, /curl floor|unresolved/);
} finally {
    fs.rmSync(work, { recursive: true, force: true });
}

console.log('benchmark tool boundaries: PASS');
