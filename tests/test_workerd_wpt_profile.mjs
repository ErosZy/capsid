import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import {test} from 'node:test';
import {auditProfileMetadata, auditProfileReports, readFullStandardEvidence, assertAuditOutput} from '../tools/workerd-wpt-profile.mjs';

const profile = JSON.parse(fs.readFileSync(new URL('./wpt/workerd-default-profile.json', import.meta.url)));
const manifest = JSON.parse(fs.readFileSync(new URL('./wpt/manifest.json', import.meta.url)));
const lock = JSON.parse(fs.readFileSync(new URL('./fixtures/workerd-ws/reference-lock.json', import.meta.url)));
const nameOf = file => 'worker_wpt_' + file.replace(/[^A-Za-z0-9]/g, '_');
const clone = value => structuredClone(value);

function fixture() {
    const queue = {beforeStart: ['first', 'second'], immediatelyAfterStart: ['first', 'second'],
        received: ['first', 'second', 'third']};
    const observation = {
        runtimeEvents: {
            handled: {type: 'rejectionhandled', trusted: true, phase: 2, target: true, currentTarget: true,
                tag: '[object PromiseRejectionEvent]', promiseIdentity: true, reasonIdentity: false, reasonType: 'undefined'},
            unhandled: {identity: true},
        },
        listenerEvents: {'MessagePort/capture-add': {ok: true, value: {errors: ['TypeError', 'TypeError'], calls: 0}}},
        messagePortQueue: {explicit: clone(queue), handler: clone(queue), nullHandler: clone(queue)},
    };
    return {
        profile: clone(profile), manifest: clone(manifest), lock: clone(lock),
        reports: profile.conflicts.map(c => ({path: c.path, report: {
            profile: manifest.profile, source: 'web-platform-tests/wpt', commit: manifest.commit,
            passed: false, ranNothing: false, total: c.total,
            failures: c.failures.map(f => ({...f, message: f.message + '\n    at original-upstream.js:10'})),
            expectedFailures: [], missingExpectedFailures: clone(c.missingExpectedFailures),
        }})),
        witnesses: ['reference', 'single-worker', 'static-pool', 'managed'].map(mode => ({mode, observation: clone(observation)})),
    };
}
function audit(f) { return auditProfileReports(f.profile, f.manifest, f.lock, f.reports, f.witnesses); }

test('profile matching preserves complete standard failures and the unreached historical expectation', () => {
    const f = fixture(), before = clone(f), r = audit(f);
    assert.deepEqual(f, before, 'audit cannot rewrite its inputs');
    assert.equal(r.status, 'profile-matched-with-standard-failures');
    assert.equal(r.standardCTestFilesPassed, false);
    assert.equal(r.standardsConformanceClaimed, false);
    assert.equal(r.standardConflictSubtests, 3);
    assert.equal(r.unreachedHistoricalExpectations, 1);
    assert.equal(r.conflicts[1].missingExpectedFailures[0].deviation, 'CAPSID-D010');
    assert.ok(r.conflicts[0].failures[0].message.includes('original-upstream.js'), 'full diagnostics retained');
});

for (const [name, change] of [
    ['profile version', f => { f.profile.id = 'unversioned'; }],
    ['reference date', f => { f.profile.reference.compatibility_date = '2026-01-01'; }],
    ['reference binary', f => { f.profile.reference.binary_sha256 = '0'.repeat(64); }],
    ['reference flags', f => { f.profile.reference.compatibility_flags.push('different_profile'); }],
    ['WPT commit', f => { f.profile.wpt.commit = '0'.repeat(40); }],
    ['standards claim', f => { f.profile.wpt.standardFailuresRetained = false; }],
    ['broad diagnostic matcher', f => { f.profile.wpt.diagnosticMatch = 'name-only'; }],
    ['removed standard file', f => { f.manifest.executedProfile = f.manifest.executedProfile.filter(p => p !== f.profile.conflicts[0].path); }],
    ['stale waiver silently removed', f => { f.manifest.expectedFailures = f.manifest.expectedFailures.filter(x => x.deviation !== 'CAPSID-D010'); }],
    ['missing source augmentation', f => { f.manifest.sourceAugmentations[0].subtests.pop(); }],
    ['partial upstream selection', f => { f.reports[0].report.total = 2; }],
    ['missing report', f => { f.reports.pop(); }],
    ['duplicate report', f => { f.reports[1] = clone(f.reports[0]); }],
    ['wrong source', f => { f.reports[0].report.source = 'project-owned'; }],
    ['wrong report profile', f => { f.reports[0].report.profile = 'other'; }],
    ['empty realm', f => { f.reports[0].report.ranNothing = true; }],
    ['invented standard success', f => { f.reports[0].report.passed = true; }],
    ['waived standard failure', f => { f.reports[0].report.expectedFailures.push(f.reports[0].report.failures.pop()); }],
    ['additional unrelated failure', f => { f.reports[0].report.failures.push({...f.reports[0].report.failures[0], name: 'unrelated'}); }],
    ['missing failure', f => { f.reports[0].report.failures.pop(); }],
    ['different diagnostic', f => { f.reports[0].report.failures[0].message = 'other assertion failed'; }],
    ['different error class', f => { f.reports[0].report.failures[0].errorName = 'TypeError'; }],
    ['missing handled event caught by cleanup', f => { f.reports[0].report.failures[0].phase = 'cleanup'; }],
    ['timeout', f => { f.reports[0].report.failures[0].status = 'TIMEOUT'; }],
    ['failure hidden in an extra field', f => { f.reports[0].report.extraFailure = {}; }],
    ['old queue failure substituted for registration error', f => {
        f.reports[1].report.failures[0].errorName = 'AssertionError';
        f.reports[1].report.failures[0].message = 'reached unreachable code';
    }],
    ['unreached queue assertion claimed executed', f => { f.reports[1].report.missingExpectedFailures = []; }],
    ['missing reference witness', f => { f.witnesses.shift(); }],
    ['missing candidate witness', f => { f.witnesses.pop(); }],
    ['duplicate candidate mode', f => { f.witnesses[2].mode = 'single-worker'; }],
    ['matching wrong reason on all backends', f => { for (const w of f.witnesses) w.observation.runtimeEvents.handled.reasonType = 'object'; }],
    ['matching missing handled event on all backends', f => { for (const w of f.witnesses) delete w.observation.runtimeEvents.handled; }],
    ['wrong promise', f => { f.witnesses[1].observation.runtimeEvents.handled.promiseIdentity = false; }],
    ['lost original unhandled reason', f => { f.witnesses[0].observation.runtimeEvents.unhandled.identity = false; }],
    ['capture listener called', f => { f.witnesses[2].observation.listenerEvents['MessagePort/capture-add'].value.calls = 1; }],
    ['matching different capture exception on all backends', f => {
        for (const w of f.witnesses) w.observation.listenerEvents['MessagePort/capture-add'].value.errors = ['Error', 'Error'];
    }],
    ['queue assertion never independently observed', f => { delete f.witnesses[0].observation.messagePortQueue; }],
    ['messages dropped after start', f => { f.witnesses[3].observation.messagePortQueue.explicit.received.pop(); }],
]) test(`reject ${name}`, () => {
    const f = fixture(); change(f); assert.throws(() => audit(f));
});

function fullFixture() {
    const f = fixture();
    const registration = {tests: f.reports.map(({path}) => ({name: nameOf(path),
        command: ['/build/test-worker-integration', '/build/capsid-worker', '/build/generated/wpt.js', 'wpt', 'https://wpt.local/' + path]}))};
    const cases = registration.tests.map(t => ({name: t.name, status: 'failed'}));
    const last = registration.tests.map((t, i) => `${i + 1}/2 Testing: ${t.name}\n` +
        `FAIL: invalid WPT execution evidence: unexpected or missing top-level WPT field: passed: ${JSON.stringify(f.reports[i].report)}\nTest Failed.\n`)
        .join('') + '\nEnd testing: Oct 17 14:00 CST\n';
    return {profile: f.profile, registration, cases, last};
}
function extract(f) { return readFullStandardEvidence(f.last, f.registration, f.cases, f.profile); }

test('full evidence binds actual standard CTests and their original file URLs', () => {
    assert.equal(extract(fullFixture()).length, 2);
});
test('complete parallel CTest output can finish in a different order', () => {
    const f = fullFixture(), at = f.last.indexOf('2/2 Testing:'), end = f.last.indexOf('\nEnd testing:');
    f.last = f.last.slice(at, end) + f.last.slice(0, at) + f.last.slice(end);
    f.cases.reverse();
    assert.equal(extract(f).length, 2);
});
test('CTest label totals may follow its complete footer', () => {
    const f = fullFixture(); f.last += '\nconformance =   0.91 sec*proc\n\nworkerd-profile =   1.25 sec*proc\n';
    assert.equal(extract(f).length, 2);
});
for (const [name, change] of [
    ['truncated footer', f => { f.last = f.last.split('End testing:')[0]; }],
    ['unrecognized trailing output', f => { f.last += 'partial next run\n'; }],
    ['partial LastTest', f => { f.last = f.last.slice(f.last.indexOf('2/2 Testing:')); }],
    ['partial registration', f => { f.registration.tests.pop(); }],
    ['partial JUnit', f => { f.cases.pop(); }],
    ['JUnit reports pass', f => { f.cases[0].status = 'passed'; }],
    ['JUnit reports skip', f => { f.cases[0].status = 'skipped'; }],
    ['wrong denominator', f => { f.last = f.last.replace('1/2 Testing:', '1/200 Testing:'); }],
    ['LastTest reports pass', f => { f.last = f.last.replace('Test Failed.', 'Test Passed.'); }],
    ['duplicate failed report', f => { f.last = f.last.replace('Test Failed.', f.last.split('\n')[1] + '\nTest Failed.'); }],
    ['malformed evidence rejection', f => { f.last = f.last.replace('unexpected or missing top-level WPT field: passed:', 'duplicate top-level WPT key:'); }],
    ['project fixture substituted for WPT', f => { f.registration.tests[0].command[3] = 'plain'; }],
    ['different source URL', f => { f.registration.tests[0].command[4] += '-other'; }],
]) test(`full evidence rejects ${name}`, () => {
    const f = fullFixture(); change(f); assert.throws(() => extract(f));
});

test('metadata can be inspected without a runtime or WPT checkout', () => {
    assert.equal(auditProfileMetadata(profile, manifest, lock).files, 2);
});

test('output validation protects original evidence, hardlink aliases and CTest state', () => {
    const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'capsid-profile-output-'));
    assert.equal(path.dirname(path.resolve(directory)), path.resolve(os.tmpdir()));
    assert.ok(path.basename(directory).startsWith('capsid-profile-output-'));
    try {
        const input = path.join(directory, 'LastTest.log'), alias = path.join(directory, 'alias.log');
        fs.writeFileSync(input, 'original standard evidence'); fs.linkSync(input, alias);
        const gate = path.join(directory, 'gate'), build = path.join(directory, 'build');
        const check = output => assertAuditOutput(output, [input], gate, build);
        assert.throws(() => check(input)); assert.throws(() => check(alias));
        assert.throws(() => check(path.join(gate, 'summary.json')));
        assert.throws(() => check(path.join(build, 'Testing/Temporary/LastTest.log')));
        assert.equal(fs.readFileSync(input, 'utf8'), 'original standard evidence');
        const output = path.join(directory, 'audit.json');
        fs.writeFileSync(output, '{"schema":"other-evidence"}'); assert.throws(() => check(output));
        fs.writeFileSync(output, '{"schema":"capsid-workerd-wpt-profile-audit-v1","status":"passed"}');
        assert.doesNotThrow(() => check(output));
        assert.doesNotThrow(() => check(path.join(directory, 'new-audit.json')));
    } finally {
        fs.rmSync(directory, {recursive: true, force: true});
    }
});
