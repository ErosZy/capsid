import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import {execFileSync} from 'node:child_process';
import {createHash} from 'node:crypto';
import {fileURLToPath} from 'node:url';
import {parseArgs} from 'node:util';
import {readLock, root} from './workerd-reference.mjs';
import {validateGateRun} from './run-workerd-ws-gate.mjs';
import {guardedRejectionTests} from '../tests/wpt/guard-rejection-events.mjs';

const profileFile = 'tests/wpt/workerd-default-profile.json';
const manifestFile = 'tests/wpt/manifest.json';
const rejectionPath = 'html/webappapis/scripting/processing-model-2/unhandled-promise-rejections/support/promise-rejection-events.js';
const portPath = 'webmessaging/MessagePort_initial_disabled.any.js';
const modes = ['reference', 'single-worker', 'static-pool', 'managed'];
const digest = file => createHash('sha256').update(fs.readFileSync(file)).digest('hex');
const testName = file => 'worker_wpt_' + file.replace(/[^A-Za-z0-9]/g, '_');

export function auditProfileMetadata(profile, manifest, lock) {
    assert.equal(profile.schema, 'capsid-workerd-wpt-profile-v1', 'versioned profile schema');
    assert.equal(profile.id, `CAPSID-WORKERD-${lock.release_tag}-default-v1`, 'versioned reference profile ID');
    assert.deepEqual(profile.reference, Object.fromEntries(
        ['release_tag', 'source_commit', 'binary_sha256', 'compatibility_date', 'compatibility_flags']
            .map(key => [key, lock[key]])), 'profile must bind the current complete reference identity');
    assert.deepEqual(profile.wpt, {profile: manifest.profile, commit: manifest.commit,
        diagnosticMatch: 'first-line-exact', standardFailuresRetained: true}, 'standard failures must remain visible');
    assert.deepEqual(profile.conflicts?.map(c => c.path), [rejectionPath, portPath], 'exact two-file profile scope');
    assert.deepEqual(profile.conflicts.map(c => c.total), [36, 1], 'complete upstream file subtest counts');
    assert.deepEqual(profile.conflicts[0].failures.map(f => f.name), guardedRejectionTests, 'both guarded rejection cases');
    assert.deepEqual(profile.conflicts[1].failures.map(f => f.name), ['Untitled test'], 'exact MessagePort case');
    for (const conflict of profile.conflicts) {
        assert.ok(manifest.executedProfile.includes(conflict.path), 'conflict remains in executed standard corpus');
        assert.match(conflict.upstreamSHA256, /^[a-f0-9]{64}$/, 'upstream file identity');
        assert.equal(conflict.deviation, 'CAPSID-D012', 'default event profile deviation');
        assert.equal(new Set(conflict.failures.map(f => f.name)).size, conflict.failures.length, 'no duplicate conflict cases');
        for (const failure of conflict.failures) {
            assert.deepEqual(Object.keys(failure).sort(), ['errorName', 'message', 'name', 'phase', 'status']);
            assert.equal(failure.status, 'FAIL');
            assert.equal(failure.phase, 'test', 'cleanup/timeout cannot represent the observed profile conflict');
            assert.ok(['AssertionError', 'TypeError'].includes(failure.errorName));
            assert.ok(typeof failure.message === 'string' && failure.message.length > 0 &&
                !/[\r\n]/.test(failure.message), 'exact single-line diagnostic');
        }
        const expected = manifest.expectedFailures.filter(f => f.path === conflict.path)
            .map(({subtest: name, path: ignored, ...rest}) => ({name, ...rest}));
        assert.deepEqual(conflict.missingExpectedFailures, expected,
            'the unreached historical expectation is recorded, not removed or accepted');
    }
    assert.deepEqual(manifest.sourceAugmentations[0].subtests, guardedRejectionTests, 'event-presence guards remain declared');
    assert.equal(manifest.sourceAugmentations[0].preservesUpstreamStatements, true, 'upstream reason assertions remain intact');
    return {profile: profile.id, files: 2, standardConflictSubtests: 3, unreachedHistoricalExpectations: 1};
}

export function checkWitness(observation, label) {
    assert.ok(observation && typeof observation === 'object', `${label}: actual API observations`);
    assert.deepEqual(observation.runtimeEvents?.handled, {
        type: 'rejectionhandled', trusted: true, phase: 2, target: true, currentTarget: true,
        tag: '[object PromiseRejectionEvent]', promiseIdentity: true, reasonIdentity: false, reasonType: 'undefined',
    }, `${label}: handled event must occur with the matching promise and omitted reason`);
    assert.equal(observation.runtimeEvents?.unhandled?.identity, true, `${label}: original unhandled reason identity`);
    assert.deepEqual(observation.listenerEvents?.['MessagePort/capture-add'],
        {ok: true, value: {errors: ['TypeError', 'TypeError'], calls: 0}}, `${label}: true capture options rejected at registration`);
    const queue = {beforeStart: ['first', 'second'], immediatelyAfterStart: ['first', 'second'],
        received: ['first', 'second', 'third']};
    assert.deepEqual(observation.messagePortQueue, {explicit: queue, handler: queue, nullHandler: queue},
        `${label}: independent queue observations, including the assertion upstream did not reach`);
}

export function auditProfileReports(profile, manifest, lock, standardReports, witnesses) {
    const metadata = auditProfileMetadata(profile, manifest, lock);
    assert.deepEqual(witnesses.map(w => w.mode), modes, 'reference and all three actual Host modes required');
    for (const witness of witnesses) checkWitness(witness.observation, witness.mode);
    assert.deepEqual(standardReports.map(r => r.path), profile.conflicts.map(c => c.path), 'both complete standard file reports');
    const conflicts = [];
    for (const [index, {path: file, report}] of standardReports.entries()) {
        const contract = profile.conflicts[index];
        assert.deepEqual(Object.keys(report).sort(), ['commit', 'expectedFailures', 'failures', 'missingExpectedFailures',
            'passed', 'profile', 'ranNothing', 'source', 'total'], `${file}: exact report fields`);
        assert.equal(report.profile, manifest.profile);
        assert.equal(report.source, 'web-platform-tests/wpt');
        assert.equal(report.commit, manifest.commit);
        assert.equal(report.passed, false, `${file}: preserve the actual standard failure`);
        assert.equal(report.ranNothing, false);
        assert.equal(report.total, contract.total, `${file}: complete file, not selected subtests`);
        assert.deepEqual(report.expectedFailures, [], `${file}: no standard waiver added`);
        assert.deepEqual(report.missingExpectedFailures, contract.missingExpectedFailures, `${file}: historical waiver state`);
        assert.deepEqual(report.failures.map(f => {
            assert.deepEqual(Object.keys(f).sort(), ['errorName', 'message', 'name', 'phase', 'status'], 'exact failure fields');
            assert.equal(typeof f.message, 'string');
            return {...f, message: f.message.split('\n')[0]};
        }), contract.failures, `${file}: exact name, phase, error class and first diagnostic line`);
        conflicts.push({path: file, standardPassed: false, total: report.total,
            failures: report.failures, missingExpectedFailures: report.missingExpectedFailures,
            deviation: contract.deviation});
    }
    return {schema: 'capsid-workerd-wpt-profile-audit-v1', status: 'profile-matched-with-standard-failures',
        complete: true, ...metadata, standardCTestFilesPassed: false, standardsConformanceClaimed: false,
        scope: 'Two standard WPT files and corresponding mandatory four-backend API observations; not full Web API conformance',
        witnessModes: modes, conflicts};
}

export function readFullStandardEvidence(lastTest, registration, junitCases, profile) {
    const raw = lastTest.replaceAll('\r\n', '\n');
    const marks = [...raw.matchAll(/^(\d+)\/(\d+) Testing: (\S+)\n/gm)];
    assert.ok(marks.length > 0, 'full LastTest sections are required');
    const endings = [...raw.matchAll(/^End testing: [^\n]+\n/gm)];
    assert.equal(endings.length, 1, 'complete LastTest footer');
    const tail = raw.slice(endings[0].index + endings[0][0].length);
    assert.ok(tail.split('\n').every(line => line.trim() === '' ||
        /^[^=]+ =\s+\d+(?:\.\d+)? sec\*proc$/.test(line)), 'only CTest label totals follow the footer');
    assert.deepEqual(marks.map(m => m[3]).sort(), registration.tests.map(t => t.name).sort(), 'full unfiltered registration and LastTest inventory');
    assert.deepEqual(junitCases.map(c => c.name).sort(), registration.tests.map(t => t.name).sort(), 'full unfiltered JUnit inventory');
    assert.equal(new Set(marks.map(m => m[3])).size, marks.length, 'no duplicate executed cases');
    assert.ok(marks.every(m => Number(m[2]) === registration.tests.length), 'full-suite denominator');
    const sections = new Map(marks.map((m, i) => [m[3], raw.slice(m.index + m[0].length, marks[i + 1]?.index ?? raw.length)]));
    return profile.conflicts.map(conflict => {
        const name = testName(conflict.path), section = sections.get(name);
        assert.ok(section, `${name}: original standard CTest executed`);
        assert.equal(junitCases.find(c => c.name === name)?.status, 'failed', `${name}: original CTest failure retained`);
        assert.match(section, /^Test Failed\.\s*$/m, `${name}: full output reports actual test failure`);
        const lines = section.split('\n').filter(l => l.startsWith('FAIL: invalid WPT execution evidence: '));
        assert.equal(lines.length, 1, `${name}: exactly one original failed report`);
        assert.ok(lines[0].startsWith('FAIL: invalid WPT execution evidence: unexpected or missing top-level WPT field: passed: '),
            `${name}: the independent C++ validator rejected the standard failure, not malformed evidence`);
        const jsonStart = lines[0].indexOf('{');
        assert.ok(jsonStart >= 0);
        const report = JSON.parse(lines[0].slice(jsonStart));
        const registered = registration.tests.find(t => t.name === name);
        assert.equal(registered.command[3], 'wpt', 'actual WPT integration driver mode');
        assert.equal(registered.command[4], 'https://wpt.local/' + conflict.path, 'exact upstream source URL');
        return {path: conflict.path, report, command: registered.command};
    });
}

function readJunit(file, python) {
    const code = 'import json,sys,xml.etree.ElementTree as ET\n' +
        'r=ET.parse(sys.argv[1]).getroot()\n' +
        'print(json.dumps([{"name":c.get("name"),"status":"failed" if c.find("failure") is not None or c.find("error") is not None else "skipped" if c.find("skipped") is not None else "passed"} for c in r.findall(".//testcase")]))';
    return JSON.parse(execFileSync(python, ['-I', '-c', code, file], {encoding: 'utf8', timeout: 10000}));
}

export function assertAuditOutput(destination, inputs, gate, build) {
    const canonical = file => fs.existsSync(file) ? fs.realpathSync(file) : path.resolve(file);
    const target = canonical(destination);
    for (const input of inputs) {
        assert.notEqual(target, canonical(input), 'audit output cannot overwrite input evidence or binaries');
        if (fs.existsSync(destination) && fs.existsSync(input)) {
            const a = fs.statSync(destination, {bigint: true}), b = fs.statSync(input, {bigint: true});
            assert.ok(a.dev !== b.dev || a.ino !== b.ino, 'audit output cannot alias an input file');
        }
    }
    for (const directory of [gate, path.join(build, 'Testing')]) {
        const relative = path.relative(canonical(directory), target);
        assert.ok(relative.startsWith('..' + path.sep) || path.isAbsolute(relative),
            'audit output must be outside archived gate and CTest state');
    }
    if (fs.existsSync(destination)) {
        assert.equal(JSON.parse(fs.readFileSync(destination, 'utf8')).schema, 'capsid-workerd-wpt-profile-audit-v1',
            'only a prior profile audit may be replaced');
    }
}

function main() {
    const command = process.argv[2];
    assert.ok(['metadata', 'audit'].includes(command), 'usage: workerd-wpt-profile.mjs metadata|audit [options]');
    const profile = JSON.parse(fs.readFileSync(path.join(root, profileFile), 'utf8'));
    const manifest = JSON.parse(fs.readFileSync(path.join(root, manifestFile), 'utf8'));
    // This retained profile binds the Linux reference and three Linux Host
    // backends. The native Windows reference has its own platform gate.
    const lock = readLock(path.join(root, 'tests/fixtures/workerd-ws/reference-lock.json'));
    if (command === 'metadata') return console.log(JSON.stringify(auditProfileMetadata(profile, manifest, lock)));
    const {values} = parseArgs({args: process.argv.slice(3), options: Object.fromEntries(
        ['last-test', 'junit', 'registration', 'gate', 'build', 'wpt-root', 'output', 'python'].map(n => [n, {type: 'string'}]))});
    for (const name of ['last-test', 'junit', 'registration', 'gate', 'build', 'wpt-root', 'output'])
        assert.ok(values[name], `required --${name}`);
    const destination = path.resolve(values.output);
    const protectedFiles = [values['last-test'], values.junit, values.registration,
        path.join(root, profileFile), path.join(root, manifestFile), path.join(values.build, 'capsid-host'),
        path.join(values.build, 'capsid-worker')].map(file => path.resolve(file));
    assertAuditOutput(destination, protectedFiles, values.gate, values.build);
    // Invalidate stale success before preflight. This report never rewrites
    // standard CTest/JUnit results, including when this audit itself fails.
    fs.mkdirSync(path.dirname(destination), {recursive: true});
    fs.writeFileSync(destination, JSON.stringify({schema: 'capsid-workerd-wpt-profile-audit-v1', complete: false, status: 'running'}) + '\n');
    try {
        const gateSummary = JSON.parse(fs.readFileSync(path.join(values.gate, 'summary.json'), 'utf8'));
        assert.equal(gateSummary.status, 'passed', 'complete required reference gate');
        assert.equal(gateSummary.complete, true);
        const run = gateSummary.run;
        const expected = JSON.parse(fs.readFileSync(path.join(run, 'expected-identities.json'), 'utf8'));
        assert.deepEqual(expected.lock, lock, 'same locked default reference');
        assert.equal(digest(path.join(values.build, 'capsid-host')), expected.host, 'same current Host binary');
        assert.equal(digest(path.join(values.build, 'capsid-worker')), expected.worker, 'same current Worker binary');
        const gate = validateGateRun(run, expected);
        assert.ok(gate.complete, 'independent full required-gate validation');
        const witnesses = modes.map(mode => {
            const report = JSON.parse(fs.readFileSync(path.join(run, mode + '.json'), 'utf8'));
            const api = report.results.filter(r => r.fixture === 'api-compat.mjs');
            assert.equal(api.length, 1);
            return {mode, observation: api[0].observation};
        });
        assert.equal(execFileSync('git', ['-C', values['wpt-root'], 'rev-parse', 'HEAD'], {encoding: 'utf8'}).trim(), manifest.commit);
        for (const conflict of profile.conflicts)
            assert.equal(digest(path.join(values['wpt-root'], conflict.path)), conflict.upstreamSHA256, 'pinned upstream source bytes');
        const registration = JSON.parse(fs.readFileSync(values.registration, 'utf8'));
        assert.deepEqual(registration, JSON.parse(fs.readFileSync(path.join(run, 'registration.json'), 'utf8')),
            'ordinary run retains the complete required-gate build inventory');
        const cases = readJunit(values.junit, values.python ?? process.env.CAPSID_WS_INTEROP_PYTHON ?? 'python3');
        const reports = readFullStandardEvidence(fs.readFileSync(values['last-test'], 'utf8'), registration, cases, profile);
        const runtimeInputs = {};
        for (const {command: argv} of reports) {
            assert.equal(fs.realpathSync(argv[1]), fs.realpathSync(path.join(values.build, 'capsid-worker')), 'full standard run uses the same Worker');
            for (const file of argv.slice(0, 3)) runtimeInputs[file] = digest(file);
        }
        const audit = auditProfileReports(profile, manifest, lock, reports, witnesses);
        const failed = cases.filter(c => c.status === 'failed').map(c => c.name);
        const expectedFailures = profile.conflicts.map(c => testName(c.path));
        const otherFailures = failed.filter(n => !expectedFailures.includes(n));
        assert.deepEqual(otherFailures, [], 'additional full-suite failures cannot be classified by this narrow profile');
        const inputs = Object.fromEntries([values['last-test'], values.junit, values.registration,
            path.join(values.gate, 'summary.json'), path.join(run, 'expected-identities.json'),
            path.join(root, profileFile), path.join(root, manifestFile)].map(file => [path.resolve(file), digest(file)]));
        fs.writeFileSync(destination, JSON.stringify({...audit, fullSuite: {selected: cases.length,
            failed, skipped: cases.filter(c => c.status === 'skipped').map(c => c.name), ordinarySuitePassed: false},
            gate: {run, tests: gate.tests, hostSHA256: expected.host, workerSHA256: expected.worker},
            inputSHA256: inputs, runtimeInputSHA256: runtimeInputs}, null, 2) + '\n');
        console.log(JSON.stringify({status: audit.status, profile: audit.profile, files: audit.files,
            standardConflictSubtests: audit.standardConflictSubtests, ordinarySuitePassed: false}));
    } catch (error) {
        fs.writeFileSync(destination, JSON.stringify({schema: 'capsid-workerd-wpt-profile-audit-v1', complete: false, status: 'failed', error: error.message}, null, 2) + '\n');
        throw error;
    }
}

if (process.argv[1] && fs.realpathSync(process.argv[1]) === fs.realpathSync(fileURLToPath(import.meta.url))) {
    try { main(); } catch (error) { console.error(error.message); process.exitCode = 1; }
}
