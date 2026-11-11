import assert from 'node:assert/strict';
import crypto from 'node:crypto';
import fs from 'node:fs';
import path from 'node:path';
import { execFileSync } from 'node:child_process';
import { gunzipSync } from 'node:zlib';
import { fileURLToPath } from 'node:url';
import { assertResponseObservation } from './workerd-response-contract.mjs';
import { assertFanoutFixtureObservation } from './workerd-fanout-contract.mjs';
import { assertHandlerThrowObservation } from './workerd-throw-contract.mjs';
import { assertHandlerErrorObservations } from './workerd-handler-error-contract.mjs';
import { referencePlatform, referenceBackendModes } from './workerd-platform.mjs';
export { assertHandlerThrowCase } from './workerd-throw-contract.mjs';
export { assertResponseConstructors, assertResponseObservation } from './workerd-response-contract.mjs';
export { compareOutboundReports, assertOutboundSuccess } from './workerd-outbound-contract.mjs';

export const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
export const lockPath = path.join(root, 'tests/fixtures/workerd-ws',
    process.platform === 'win32' ? 'reference-lock-windows.json' : 'reference-lock.json');
export const digest = value => crypto.createHash('sha256').update(value).digest('hex');
export const fileDigest = file => digest(fs.readFileSync(file));

// Reviewed required matrix, independent of the files left on disk. Changing
// the product contract requires changing this list explicitly.
export const requiredFixtures = Object.freeze([
    'api-compat.mjs', 'close-boundary.mjs', 'disconnect-mid-stream.mjs',
    'double-accept.mjs', 'echo.mjs', 'fanout.mjs', 'fragmented.mjs',
    'invalid-utf8.mjs', 'lifecycle.mjs', 'ping-flood.mjs', 'response.mjs', 'semantics.mjs',
    'server-close.mjs', 'slow-consumer.mjs', 'transport.mjs',
]);
export function fixtureIdentities(directory = path.join(root, 'tests/fixtures/workerd-ws')) {
    assert.deepEqual(fs.readdirSync(directory).filter(f => f.endsWith('.mjs')).sort(),
        requiredFixtures, 'fixture directory must match the required manifest');
    return requiredFixtures.map(fixture => ({ fixture, sha256: fileDigest(path.join(directory, fixture)) }));
}

export function readLock(file = lockPath) {
    const lock = JSON.parse(fs.readFileSync(file, 'utf8'));
    for (const key of ['binary_sha256', 'release_asset_sha256'])
        assert.match(lock[key], /^[a-f0-9]{64}$/, `locked ${key}`);
    assert.match(lock.source_commit, /^[a-f0-9]{40}$/);
    assert.match(lock.release_tag, /^v[0-9.]+$/);
    referencePlatform(lock);
    assert.equal(lock.binary + '.gz', lock.release_asset, 'locked executable asset name');
    assert.ok(Array.isArray(lock.compatibility_flags));
    return lock;
}

export function verifyClient(lock) {
    assert.equal(process.version, lock.client.node_version, 'locked Node version');
    assert.equal(process.versions.undici, lock.client.undici_version, 'locked undici version');
}

export function verifyBinary(binary, lock) {
    assert.equal(process.platform, referencePlatform(lock), 'locked workerd executable platform');
    assert.equal(process.arch, 'x64', 'locked workerd executable architecture');
    assert.equal(fileDigest(binary), lock.binary_sha256, 'locked workerd executable digest');
    assert.equal(execFileSync(binary, ['--version'], { encoding: 'utf8', timeout: 10000 }).trim(),
        lock.binary_version_string, 'locked workerd version');
}

export async function installReference(output, lock, assetFile) {
    verifyClient(lock);
    assert.equal(process.platform, referencePlatform(lock), 'locked workerd platform');
    assert.equal(process.arch, 'x64', 'locked workerd architecture');
    if (fs.existsSync(output)) {
        verifyBinary(output, lock);
        return;
    }
    let asset;
    if (assetFile) asset = fs.readFileSync(assetFile);
    else {
        const response = await fetch(`https://github.com/cloudflare/workerd/releases/download/${lock.release_tag}/${lock.release_asset}`,
            { signal: AbortSignal.timeout(120000) });
        assert.ok(response.ok, `workerd download HTTP ${response.status}`);
        asset = Buffer.from(await response.arrayBuffer());
    }
    assert.equal(digest(asset), lock.release_asset_sha256, 'locked compressed workerd digest');
    const executable = gunzipSync(asset, { maxOutputLength: 256 * 1024 * 1024 });
    assert.equal(digest(executable), lock.binary_sha256, 'locked decompressed workerd digest');
    fs.mkdirSync(path.dirname(output), { recursive: true });
    const temporary = fs.mkdtempSync(path.join(path.dirname(output), '.workerd-'));
    try {
        const staged = path.join(temporary, process.platform === 'win32' ? 'workerd.exe' : 'workerd');
        fs.writeFileSync(staged, executable, { mode: 0o755 });
        verifyBinary(staged, lock);
        fs.renameSync(staged, output);
    } finally {
        assert.equal(path.dirname(temporary), path.dirname(output), 'owned staging directory parent');
        assert.ok(path.basename(temporary).startsWith('.workerd-'), 'owned staging directory name');
        fs.rmSync(temporary, { recursive: true, force: true });
    }
}

// Labels describe product differences; they never exempt a whole fixture.
export const deviations = Object.freeze({
    'close-boundary.mjs': 'strict received close-code validation (1002 instead of echoing 999)',
    'slow-consumer.mjs': 'finite 4 MiB send queue rejects synchronous 8 MiB burst',
});
export const observationExceptions = Object.freeze({
    'close-boundary.mjs': { invalidClose: {
        reference: { code: 999, reason: 'in' }, capsid: { code: 1002, reason: '' },
    } },
});

function comparableObservation(fixture, value, mode) {
    if (fixture === 'response.mjs') return assertResponseObservation(value);
    if (fixture === 'fanout.mjs') return assertFanoutFixtureObservation(value);
    const observed = structuredClone(value);
    assert.ok(observed && typeof observed === 'object' && Object.keys(observed).length > 0,
        `${mode}/${fixture}: nonempty observation required`);
    if (fixture === 'api-compat.mjs') {
        observed.handlerThrows = assertHandlerThrowObservation(observed.handlerThrows);
        observed.handlerErrors = assertHandlerErrorObservations(observed.handlerErrors);
    }
    if (fixture === 'ping-flood.mjs') {
        assert.ok(Array.isArray(observed.pongs) && observed.pongs.length > 0 && observed.pongs.length <= 50,
            `${mode}: bounded pong observations`);
        let previous = -1;
        for (const text of observed.pongs) {
            assert.match(text, /^ping-0[0-4][0-9]$/);
            const current = Number(text.slice(5));
            assert.ok(current > previous, `${mode}: pongs preserve order without duplicates`);
            previous = current;
        }
        // RFC control-frame coalescing permits a subset. Validate the actual
        // subset above; all data and terminal observations still compare.
        delete observed.pongs;
    }
    if (fixture === 'slow-consumer.mjs') {
        assert.ok(Number.isInteger(observed.frames));
        assert.ok(mode === 'reference' ? observed.frames === 128 : observed.frames > 0 && observed.frames < 128,
            `${mode}: documented send budget`);
        assert.equal(observed.bytes, observed.frames * 65536);
        assert.equal(observed.closeFrame, mode === 'reference' ? 1000 : null);
        assert.equal(observed.completionMessage, mode === 'reference' ? 'slow-burst-done' : null);
        assert.equal(observed.peerClosed, true, `${mode}: transport terminated`);
        delete observed.frames; delete observed.bytes; delete observed.closeFrame; delete observed.completionMessage;
    }
    return observed;
}

export function compareReports(reference, candidates, expected) {
    const { lock, runner, fixtures, host, worker } = expected;
    const names = fixtures.map(x => x.fixture).sort();
    assert.deepEqual(names, requiredFixtures, 'full required fixture manifest');
    assert.equal(new Set(names).size, names.length, 'unique expected fixtures');
    assert.ok(names.length > 0, 'nonempty fixture matrix');
    function validate(report, mode) {
        assert.equal(report.schema, mode === 'reference' ? 'capsid-workerd-ws-reference-v1' : 'capsid-ws-fixtures-v1',
            `${mode}: report schema`);
        assert.equal(report.complete, true, `${mode}: complete report required`);
        assert.equal(report.runner_sha256, runner, `${mode}: runner identity`);
        assert.deepEqual(report.client, { node: lock.client.node_version, undici: lock.client.undici_version });
        assert.deepEqual(report.fixtures, fixtures, `${mode}: all fixture identities`);
        assert.deepEqual(report.lock, lock, `${mode}: reference settings`);
        assert.equal(report.results.length, names.length, `${mode}: result count`);
        assert.deepEqual(report.results.map(x => x.fixture).sort(), names, `${mode}: every fixture exactly once`);
        for (const result of report.results) {
            assert.equal(result.passed, true, `${mode}/${result.fixture}: passed`);
            assert.ok(!result.skipped, `${mode}/${result.fixture}: no skip`);
            assert.ok(Number.isInteger(result.assertions) && result.assertions > 0, `${mode}/${result.fixture}: assertions executed`);
            assert.ok(Object.hasOwn(result, 'observation'), `${mode}/${result.fixture}: observation required`);
            assert.equal(result.shutdown?.forced, false, `${mode}/${result.fixture}: bounded shutdown`);
            if (mode === 'reference')
                assert.ok(result.shutdown.code === 0 || result.shutdown.signal === 'SIGTERM',
                    `${mode}/${result.fixture}: reference exit`);
            else assert.equal(result.shutdown.code, 0, `${mode}/${result.fixture}: normal Host exit`);
        }
        if (mode !== 'reference') assert.equal(report.reference_parity,
            !report.results.some(r => r.deviation || observationExceptions[r.fixture]),
            `${mode}: parity claim includes documented field differences`);
    }
    validate(reference, 'reference');
    assert.equal(reference.binary_sha256, lock.binary_sha256, 'reference binary identity');
    assert.deepEqual(candidates.map(x => x.mode).sort(), referenceBackendModes(lock).sort(),
        'every supported platform backend exactly once');
    for (const report of candidates) {
        validate(report, report.mode);
        assert.equal(report.host_sha256, host, 'Host identity');
        assert.equal(report.worker_sha256, worker, 'Worker identity');
        for (const result of report.results) {
            const exception = deviations[result.fixture] ?? null;
            assert.equal(result.deviation ?? null, exception, `${report.mode}/${result.fixture}: declared exception`);
            {
                const observed = comparableObservation(result.fixture, result.observation, report.mode);
                const baseline = comparableObservation(result.fixture,
                    reference.results.find(x => x.fixture === result.fixture).observation, 'reference');
                for (const [field, values] of Object.entries(observationExceptions[result.fixture] ?? {})) {
                    assert.deepEqual(baseline?.[field], values.reference, `reference/${field}: declared observation`);
                    assert.deepEqual(observed?.[field], values.capsid, `${report.mode}/${field}: declared observation`);
                    delete baseline[field]; delete observed[field];
                }
                assert.deepEqual(observed, baseline, `${report.mode}/${result.fixture}: observed behavior`);
            }
        }
    }
    return { complete: true, modes: candidates.map(x => x.mode), fixtures: names,
        exceptions: deviations, observation_exceptions: observationExceptions,
        host_sha256: host, worker_sha256: worker,
        assertions: Object.fromEntries([['reference', reference], ...candidates.map(r => [r.mode, r])]
            .map(([mode, report]) => [mode, report.results.reduce((sum, r) => sum + r.assertions, 0)])) };
}

if (process.argv[1] && fs.realpathSync(process.argv[1]) === fs.realpathSync(fileURLToPath(import.meta.url))) {
    try {
        assert.equal(process.argv[2], 'install', 'usage: workerd-reference.mjs install OUTPUT [ASSET]');
        assert.ok(process.argv[3], 'output path required');
        await installReference(path.resolve(process.argv[3]), readLock(), process.argv[4]);
        console.log(path.resolve(process.argv[3]));
    } catch (error) { console.error(error.message); process.exitCode = 1; }
}
