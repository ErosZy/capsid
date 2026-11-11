import assert from 'node:assert/strict';
import fs from 'node:fs';
import {test} from 'node:test';
import {compareFrameReports, validateFrameReport, frameSizes} from '../tools/workerd-frame-contract.mjs';

function matrix() {
    const reference = JSON.parse(fs.readFileSync(new URL('./fixtures/websocket-frame-report.json', import.meta.url))).report;
    const expected = {runId: reference.runId, lock: structuredClone(reference.referenceLock),
        inputs: structuredClone(reference.inputs), host: 'a'.repeat(64), worker: 'b'.repeat(64)};
    // These copies test archive validation only. The required gate executes
    // all real backends, using fresh challenges and actual raw TCP frames.
    const candidates = ['single-worker', 'static-pool', 'managed'].map(mode => ({...structuredClone(reference), mode,
        binaries: {host: expected.host, worker: expected.worker}, shutdown: {code: 0, signal: null, forced: false}}));
    return {reference, candidates, expected};
}
const all = m => [m.reference, ...m.candidates];

test('all exact raw frame sizes and complete matching backend reports pass', () => {
    assert.deepEqual(frameSizes, [0, 1, 125, 126, 65535, 65536, 1048576]);
    const m = matrix();
    assert.deepEqual(compareFrameReports(m.reference, m.candidates, m.expected), {
        complete: true, backends: 4, connectionsPerBackend: 4, framesPerBackend: 56,
        sizes: [0, 1, 125, 126, 65535, 65536, 1048576], opcodes: [1, 2], singleFinFrames: true, reciprocalClose: 1000,
    });
});

for (const [name, mutate] of [
    ['missing backend', m => { m.candidates.pop(); }],
    ['duplicate backend', m => { m.candidates[1].mode = 'single-worker'; }],
    ['reference alias', m => { m.reference.mode = 'managed'; }],
    ['wrong schema', m => { m.reference.schema = 'capsid-websocket-frame-boundaries-v1'; }],
    ['incomplete report', m => { m.candidates[0].complete = false; }],
    ['stale run', m => { m.candidates[0].runId = 'previous-run'; }],
    ['unbounded run challenge', m => { m.expected.runId = m.reference.runId = 'x'.repeat(128); }],
    ['changed compatibility date', m => { m.reference.referenceLock.compatibility_date = '2000-01-01'; }],
    ['changed compatibility flags', m => { m.candidates[0].referenceLock.compatibility_flags = ['different']; }],
    ['changed Node', m => { m.reference.client.node = 'v0.0.0'; }],
    ['changed undici', m => { m.candidates[1].client.undici = '0.0.0'; }],
    ['wrong Host', m => { m.candidates[0].binaries.host = '0'.repeat(64); }],
    ['wrong Worker', m => { m.candidates[0].binaries.worker = '0'.repeat(64); }],
    ['wrong workerd', m => { m.reference.binaries.host = '0'.repeat(64); }],
    ['reference with Worker', m => { m.reference.binaries.worker = m.expected.worker; }],
    ...['runner', 'fixture', 'contract', 'hostBackend'].map(key => [`changed ${key}`, m => { m.candidates[0].inputs[key] = '0'.repeat(64); }]),
    ['omitted source identity', m => { delete m.reference.inputs.fixture; }],
    ['identical omitted one-megabyte cases', m => { for (const r of all(m)) r.observations = r.observations.filter(x => x.size !== 1048576); }],
    ['identical omitted binary cases', m => { for (const r of all(m)) r.observations = r.observations.filter(x => x.opcode !== 2); }],
    ['identical omitted connection', m => { for (const r of all(m)) r.observations = r.observations.filter(x => x.connection !== 3); }],
    ['fewer actual connections', m => { m.candidates[0].connections = 1; }],
    ['duplicate frame', m => { m.candidates[0].observations[1] = structuredClone(m.candidates[0].observations[0]); }],
    ['reordered frames', m => { m.reference.observations.reverse(); }],
    ['wrong opcode', m => { m.reference.observations[0].opcode = 2; }],
    ['wrong actual payload', m => { m.candidates[0].observations[0].sha256 = '0'.repeat(64); }],
    ['identical corrupt payloads on every backend', m => { for (const r of all(m)) for (const x of r.observations) x.sha256 = '0'.repeat(64); }],
    ['wrong FIN', m => { m.candidates[0].observations[0].fin = false; }],
    ['wrong mask', m => { m.candidates[0].observations[0].masked = true; }],
    ['unrequested extension', m => { m.candidates[0].observations[0].rsv = 64; }],
    ['noncanonical 16-bit length', m => { m.candidates[0].observations.find(x => x.size === 125).lengthTag = 126; }],
    ['noncanonical 64-bit length', m => { m.candidates[0].observations.find(x => x.size === 65535).lengthTag = 127; }],
    ['missing 64-bit boundary', m => { m.candidates[0].observations.find(x => x.size === 65536).lengthTag = 126; }],
    ['missing Close', m => { m.candidates[0].closes.pop(); }],
    ['abnormal Close', m => { m.candidates[0].closes[0].payloadHex = '03ee'; }],
    ['unfinished TCP close', m => { m.candidates[0].closes[0].tcpClosed = false; }],
    ['extra bytes after Close', m => { m.candidates[0].closes[0].extraBytes = 1; }],
    ['unhealthy backend', m => { m.candidates[0].healthy.status = 500; }],
    ['forced reference stop', m => { m.reference.shutdown.forced = true; }],
    ['abnormal Host exit', m => { m.candidates[0].shutdown.code = 1; }],
    ['signal Host exit', m => { m.candidates[0].shutdown = {code: null, signal: 'SIGTERM', forced: false}; }],
    ['sanitizer failure', m => { m.candidates[0].backendStderr = 'ERROR: AddressSanitizer: failure'; }],
]) test(`raw frame archive rejects ${name}`, () => {
    const m = matrix(); mutate(m);
    assert.throws(() => compareFrameReports(m.reference, m.candidates, m.expected));
});

test('each backend report can also be checked without a differential matrix', () => {
    const m = matrix();
    for (const report of all(m)) assert.equal(validateFrameReport(report, m.expected).observations.length, 56);
});
