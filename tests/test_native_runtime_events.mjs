import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import {resolve, dirname, join} from 'node:path';
import {fileURLToPath} from 'node:url';
import {spawnSync} from 'node:child_process';
import {compareRuntimeEventReport, loadRuntimeEventPrimaries, runtimeEventFiles,
    runtimeEventInputFiles, runtimeEventScenarios, runtimeEventSha256} from '../tools/native-runtime-events-contract.mjs';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const options = new Map();
const args = process.argv.slice(2);
assert.equal(args.length, 6, 'require --helper, --worker, --kind');
for (let i = 0; i < args.length; i += 2) {
    assert.ok(['--helper', '--worker', '--kind'].includes(args[i]) && !options.has(args[i]));
    options.set(args[i], args[i + 1]);
}
const helper = resolve(options.get('--helper'));
const worker = resolve(options.get('--worker'));
const kind = options.get('--kind');
assert.ok(kind === 'api' || kind === 'mutations');
const primaries = loadRuntimeEventPrimaries(root);
const measuredFiles = [...runtimeEventInputFiles.map(name => join(root, name)), helper, worker];
const snapshot = () => Object.fromEntries(measuredFiles.map(path => [path, runtimeEventSha256(readFileSync(path))]));
const before = snapshot();
const cases = [];
for (const scenario of kind === 'api' ? ['api'] : runtimeEventScenarios) {
    const program = join(root, runtimeEventFiles[kind]);
    const argv = [worker, program, 'http://localhost:1', '0', 'probe&scenario=' + scenario];
    const result = spawnSync(helper, argv, {input: 'finish\n', encoding: 'utf8', timeout: 15000, maxBuffer: 4 * 1024 * 1024});
    assert.ifError(result.error);
    assert.equal(result.signal, null);
    assert.equal(result.status, 0, scenario + ': ' + result.stderr);
    const lines = result.stdout.trim().split(/\r?\n/);
    const probes = lines.filter(line => line.startsWith('LIMIT_PROBE '));
    const exits = lines.filter(line => line.startsWith('LIMIT_EXIT '));
    assert.equal(probes.length, 1);
    assert.equal(exits.length, 1);
    assert.ok(lines.indexOf(exits[0]) > lines.indexOf(probes[0]));
    const report = JSON.parse(probes[0].slice(12));
    const nativeExit = JSON.parse(exits[0].slice(11));
    assert.deepEqual(nativeExit, {clean: true, processObserved: true, exitStatus: 0, forced: false, errorEvents: 0});
    const comparison = compareRuntimeEventReport(report, kind, scenario, primaries);
    cases.push({scenario, report, nativeExit, comparison,
        stdoutSha256: runtimeEventSha256(result.stdout), stderrSha256: runtimeEventSha256(result.stderr)});
}
assert.deepEqual(snapshot(), before, 'native binaries and program/model/archive source bytes must remain unchanged');
console.log('NATIVE_RUNTIME_EVENT_REPORT ' + JSON.stringify({complete: true, kind, cases, measuredInputs: before,
    profile: primaries.lock.profile, referenceBinarySha256: primaries.lock.referenceBinarySha256,
    actualNativeCAbiWorker: true, healthyReuseAndNaturalExitPerCase: true,
    nativeEventDeadlineMilliseconds: 5000, naturalExitDeadlineMilliseconds: 2500,
    rawStandardWptQualificationNotClaimed: true, wholeApiIntrospectionEquivalenceNotClaimed: true}));
