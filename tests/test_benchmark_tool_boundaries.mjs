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
  fs.writeFileSync(
    path.join(work, 'cold', 'cells.txt'),
    'capsid src 10k total_ms=1.0 ready_ms=0.5 warm_ms=0.1 pss_mb=1.0\n',
  );
  fs.writeFileSync(
    path.join(work, 'qps', 'cells.txt'),
    'capsid matrix-json-16k   qps_med= 0 p50_ms=0 p95_ms=0\n' +
      'capsid                        serving_pss_mb=1.0\n' +
      'capsid                        serving_pss_detail=host=0.5;worker=0.5\n',
  );

  const result = spawnSync(
    python,
    [
      '-B',
      '-c',
      [
        'import importlib.util, json, pathlib, sys',
        'sys.path.insert(0, str(pathlib.Path(sys.argv[1]).parent))',
        'spec = importlib.util.spec_from_file_location("collector", sys.argv[1])',
        'module = importlib.util.module_from_spec(spec)',
        'spec.loader.exec_module(module)',
        'print(json.dumps({"coldStart": module.parse_cold_start(sys.argv[2]), "qps": module.parse_qps(sys.argv[3])}, allow_nan=False))',
      ].join('\n'),
      collector,
      path.join(work, 'cold'),
      path.join(work, 'qps'),
    ],
    { encoding: 'utf8', cwd: work, timeout: 10000, windowsHide: true },
  );
  assert.ifError(result.error);
  assert.equal(
    result.status,
    0,
    `zero-QPS parser failed: code=${result.status}, signal=${result.signal}\n${result.stderr}`,
  );

  const data = result.stdout;
  assert.match(data, /"qps": 0\.0/, 'zero-QPS round must survive as a finite 0.0 cell');
  assert.match(data, /"mb": 0\.5/, 'serving PSS breakdown must be carried into the point');
  assert.match(data, /"totalMs": 1\.0/, 'cold-start cells must be carried into the point');
  const publication = spawnSync(
    python,
    [
      '-B',
      collector,
      '--cold-start',
      path.join(work, 'cold'),
      '--qps',
      path.join(work, 'qps'),
      '--version',
      't',
      '--release',
      't',
      '--repo',
      work,
      '--generated-at',
      '2026-10-20T00:00:00Z',
      '--data',
      path.join(work, 'data.js'),
    ],
    { encoding: 'utf8', cwd: work, timeout: 10000, windowsHide: true },
  );
  assert.ifError(publication.error);
  assert.equal(publication.status, 1, publication.stderr);
  assert.match(publication.stderr, /publication archives must live under bench\/results/);
  assert.ok(
    !fs.existsSync(path.join(work, 'data.js')),
    'parser-only fixtures must not produce a published data point',
  );

  // actions/checkout uses native CRLF line endings on Windows. These are
  // source-shape assertions, so normalize line endings before matching the
  // same shell structure on every host platform.
  const source = fs.readFileSync(qpsLane, 'utf8').replace(/\r\n?/g, '\n');
  assert.match(
    source,
    /set -euo pipefail/,
    'the lane must abort on the first failed command or unbound variable',
  );
  assert.match(
    source,
    /exec "\$\{CAPSID_BENCH_PYTHON:-python3\}" -B "\$SCRIPT_DIR\/qps-lane\.py" "\$@"/,
    'the public shell must pass status and signals to the owned controller',
  );
  const qpsPreflight = spawnSync(
    python,
    [
      '-B',
      path.join(path.dirname(qpsLane), 'qps-lane.py'),
      '--out',
      path.join(work, 'qps-must-not-launch'),
    ],
    {
      encoding: 'utf8',
      cwd: work,
      timeout: 10000,
      windowsHide: true,
      env: { ...process.env, CAPSID_BENCH_METADATA: '', STACKS: 'workerd' },
    },
  );
  assert.ifError(qpsPreflight.error);
  assert.equal(qpsPreflight.status, 2, qpsPreflight.stderr);
  assert.match(qpsPreflight.stderr, /formal QPS requires|requires Linux/);
  assert.ok(
    !fs.existsSync(path.join(work, 'qps-must-not-launch')),
    'incomplete formal protocol must fail before any output or process',
  );
  // Process/port ownership, failed rounds and cancellation are executed by
  // the actual Python controller/probe tests instead of shell source grep.

  // Execute the actual page script with its archived point. This checks
  // status and link behavior; browser layout is qualified separately.
  const siteRoot = path.resolve(path.dirname(collector), '..', 'perf-site');
  const html = fs.readFileSync(path.join(siteRoot, 'index.html'), 'utf8');
  const script = html.match(/<script>([\s\S]*?)<\/script>/)[1];
  const dataContext = { window: {} };
  for (const asset of ['data.js', 'status.js', 'archive-index.js']) {
    vm.runInNewContext(fs.readFileSync(path.join(siteRoot, asset), 'utf8'), dataContext);
  }
  function render(data) {
    const elements = new Map();
    const element = () => ({
      textContent: '',
      innerHTML: '',
      value: '',
      style: {},
      children: [],
      appendChild(child) {
        this.children.push(child);
        return child;
      },
      getContext: () => ({ measureText: (s) => ({ width: String(s).length * 8 }) }),
    });
    const context = {
      window: { ...dataContext.window, CAPSID_PERF_DATA: data },
      document: {
        body: {},
        getElementById(id) {
          if (!elements.has(id)) elements.set(id, element());
          return elements.get(id);
        },
        createElement: element,
      },
      getComputedStyle: () => ({ fontFamily: 'sans-serif' }),
    };
    vm.runInNewContext(script, context, { timeout: 5000 });
    return elements;
  }
  const historical = JSON.parse(JSON.stringify(dataContext.window.CAPSID_PERF_DATA));
  // Use the actual legacy record, not the newly appended accepted snapshot.
  const oldPoint = historical.points.find((point) => point.verification?.status !== 'verified');
  assert.ok(oldPoint, 'the retained historical observation is required');
  historical.points = [oldPoint];
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
  newPoint.density = {
    capsid: { 'cold@10k': { count: 2, pssMb: 1, unit: 'workers', limit: 'cap' } },
  };
  elements = render(complete);
  assert.doesNotMatch(elements.get('nav-meta').textContent, /unverified/);
  assert.match(
    elements.get('method-card').innerHTML,
    /href="evidence\/bench\/results\/synthetic%20fixture\/raw.jsonl"/,
  );
  assert.match(
    elements.get('verification-note').textContent,
    /Complete archive and protocol replay passed/,
  );
  assert.match(elements.get('budget-table').innerHTML, /cap reached/);
  assert.doesNotMatch(
    elements.get('budget-table').innerHTML,
    /largest pool|gap widens|never returns/,
  );
  newPoint.origins = { 'cold-start': { protocol: { httpClient: 'python-http-exact-v1' } } };
  elements = render(complete);
  assert.match(elements.get('warm-note').innerHTML, /C-ABI IPC/);
  assert.match(elements.get('warm-note').innerHTML, /loopback HTTP/);
  assert.doesNotMatch(elements.get('warm-chart').innerHTML, /curl floor|unresolved/);

  const v2 = JSON.parse(JSON.stringify(complete));
  const v2Point = v2.points[v2.points.length - 1];
  v2Point.verification = { status: 'verified', contract: 'capsid-perf-lane-v2' };
  v2Point.qps = {
    cells: [
      { stack: 'capsid', workload: 'matrix-json-1k', qps: 19000, p50Ms: 3.2, p95Ms: 4.8 },
      { stack: 'node', workload: 'matrix-json-1k', qps: 25000, p50Ms: 2.5, p95Ms: 3.9 },
      { stack: 'node-jitless', workload: 'matrix-json-1k', qps: 12000, p50Ms: 5.1, p95Ms: 7.4 },
      { stack: 'bun', workload: 'matrix-json-1k', qps: 60000, p50Ms: 1.0, p95Ms: 1.8 },
      { stack: 'workerd', workload: 'matrix-json-1k', qps: 45000, p50Ms: 1.4, p95Ms: 2.1 },
    ],
    servingPssMb: { capsid: 18.6, node: 55.0, 'node-jitless': 42.0, bun: 75.0, workerd: 220.0 },
    servingPss: {
      capsid: [
        { role: 'host', mb: 9.6 },
        { role: 'worker', mb: 9.0 },
      ],
      node: [{ role: 'process', mb: 55.0 }],
      'node-jitless': [{ role: 'process', mb: 42.0 }],
      bun: [{ role: 'process', mb: 75.0 }],
      workerd: [{ role: 'process', mb: 220.0 }],
    },
    lowLoadPssMb: { capsid: 14.8, node: 48.0, 'node-jitless': 38.0, bun: 58.0, workerd: 90.0 },
    lowLoadPss: {
      capsid: [
        { role: 'host', mb: 8.0 },
        { role: 'worker', mb: 6.8 },
      ],
      node: [{ role: 'process', mb: 48.0 }],
      'node-jitless': [{ role: 'process', mb: 38.0 }],
      bun: [{ role: 'process', mb: 58.0 }],
      workerd: [{ role: 'process', mb: 90.0 }],
    },
  };
  elements = render(v2);
  assert.doesNotMatch(elements.get('nav-meta').textContent, /unverified/);
  assert.match(elements.get('qps-table').innerHTML, /Node\.js \(--jitless\)/);
  assert.match(elements.get('qps-table').innerHTML, /Bun/);
  assert.match(elements.get('radar-chart').innerHTML, /<svg/);
  assert.match(elements.get('frontier-chart').innerHTML, /HIGH-EFFICIENCY ZONE/);
  assert.match(elements.get('scorecard-table').innerHTML, /OS Sandbox/);
  assert.match(elements.get('qps-eff-chart').innerHTML, /\/MiB/);
  assert.match(elements.get('qps-callout').innerHTML, /Node\.js \(--jitless\)/);

  const v3 = JSON.parse(JSON.stringify(v2));
  const v3Point = v3.points[v3.points.length - 1];
  v3Point.verification = { status: 'verified', contract: 'capsid-perf-lane-v3' };
  v3Point.qps.cells.push(
    { stack: 'deno', workload: 'matrix-json-1k', qps: 50000, p50Ms: 1.2, p95Ms: 2.3 },
    { stack: 'bun-jitless', workload: 'matrix-json-1k', qps: 30000, p50Ms: 2.1, p95Ms: 3.4 },
    { stack: 'capsid', workload: 'dynamic-api', qps: 12000, p50Ms: 5.0, p95Ms: 7.1 },
    { stack: 'workerd', workload: 'dynamic-api', qps: 19000, p50Ms: 3.1, p95Ms: 5.2 },
    { stack: 'deno', workload: 'dynamic-api', qps: 32000, p50Ms: 2.0, p95Ms: 3.3 },
    { stack: 'node-jitless', workload: 'dynamic-api', qps: 6000, p50Ms: 9.8, p95Ms: 12.1 },
    { stack: 'bun-jitless', workload: 'dynamic-api', qps: 16000, p50Ms: 3.8, p95Ms: 5.4 },
    { stack: 'node', workload: 'dynamic-api', qps: 42000, p50Ms: 1.5, p95Ms: 2.6 },
    { stack: 'bun', workload: 'dynamic-api', qps: 75000, p50Ms: 0.8, p95Ms: 1.4 },
  );
  v3Point.qps.servingPssMb.deno = 64.0;
  v3Point.qps.servingPssMb['bun-jitless'] = 34.0;
  v3Point.qps.servingPss.deno = [{ role: 'process', mb: 64.0 }];
  v3Point.qps.servingPss['bun-jitless'] = [{ role: 'process', mb: 34.0 }];
  v3Point.qps.lowLoadPssMb.deno = 35.0;
  v3Point.qps.lowLoadPssMb['bun-jitless'] = 24.0;
  v3Point.qps.lowLoadPss.deno = [{ role: 'process', mb: 35.0 }];
  v3Point.qps.lowLoadPss['bun-jitless'] = [{ role: 'process', mb: 24.0 }];
  elements = render(v3);
  assert.match(elements.get('scorecard-table').innerHTML, /Tier 1 · Isolated/);
  assert.match(elements.get('scorecard-table').innerHTML, /Permission Sandbox/);
  assert.match(elements.get('scorecard-table').innerHTML, /Dynamic API/);
  assert.match(elements.get('scenario-grid').innerHTML, /Choose Capsid/);
  assert.match(elements.get('qps-eff-tabs').innerHTML, /Fair Tier/);
  assert.match(elements.get('qps-table').innerHTML, /Bun \(--jitless\)/);
  assert.match(elements.get('qps-callout').innerHTML, /Dynamic API/);

  // Current packed evidence and user selection exercise the actual page,
  // using committed generated assets without inventing raw-file links.
  const current = JSON.parse(JSON.stringify(dataContext.window.CAPSID_PERF_DATA));
  const acceptedPoints = current.points
    .map((point, index) => ({ point, index }))
    .filter(({ point }) => point.verification?.status === 'verified');
  assert.ok(acceptedPoints.length >= 1);
  acceptedPoints.sort((a, b) =>
    a.point.date === b.point.date ? a.index - b.index : a.point.date.localeCompare(b.point.date),
  );
  const { point: acceptedPoint, index: acceptedIndex } = acceptedPoints[acceptedPoints.length - 1];
  const historicalIndex = current.points.findIndex(
    (point) => point.verification?.status !== 'verified',
  );
  assert.ok(acceptedIndex >= 0 && historicalIndex >= 0);
  elements = render(current);
  assert.match(elements.get('radar-chart').innerHTML, /<polygon/);
  assert.match(elements.get('density-chart').innerHTML, /<svg/);
  assert.match(elements.get('scorecard-table').innerHTML, /Capsid/);
  const selector = elements.get('record-select');
  assert.equal(selector.children.length, current.points.length);
  assert.equal(selector.value, String(acceptedIndex));
  assert.match(elements.get('nav-meta').textContent, /dev snapshot/);
  assert.match(
    elements.get('release-context').innerHTML,
    /no matching complete performance measurement/,
  );
  const packedLinks = () => elements.get('method-card').innerHTML;
  const activePrefixes = new Set(Object.values(acceptedPoint.verification?.archives || {}));
  const activeBundles = dataContext.window.CAPSID_PERF_ARCHIVES.bundles.filter(
    (bundle) => activePrefixes.size === 0 || activePrefixes.has(bundle.prefix),
  );
  const parts = activeBundles.flatMap((bundle) => bundle.parts);
  assert.equal((packedLinks().match(/class="archive-download"/g) || []).length, parts.length);
  for (const part of parts) {
    const href = part.path
      .replace(/^perf-site\//, '')
      .split('/')
      .map(encodeURIComponent)
      .join('/');
    assert.ok(packedLinks().includes(`href="${href}"`), part.path);
  }
  assert.match(packedLinks(), /href="evidence-index.json"/);
  assert.doesNotMatch(packedLinks(), /href="evidence\/bench\/results\//);
  selector.value = String(historicalIndex);
  selector.onchange();
  assert.match(elements.get('nav-meta').textContent, /unverified/);
  assert.doesNotMatch(packedLinks(), /archive-download|href="evidence\//);
  selector.value = String(acceptedIndex);
  selector.onchange();
  assert.equal((packedLinks().match(/class="archive-download"/g) || []).length, parts.length);
} finally {
  fs.rmSync(work, { recursive: true, force: true });
}

console.log('benchmark tool boundaries: PASS');
