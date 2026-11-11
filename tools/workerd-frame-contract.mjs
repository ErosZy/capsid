import assert from 'node:assert/strict';
import {referenceBackendModes} from './workerd-platform.mjs';
import {createHash} from 'node:crypto';

export const frameInputPaths = Object.freeze({runner: 'tests/test_websocket_frame_boundaries.mjs',
    fixture: 'tests/fixtures/websocket-frame-boundaries.mjs', contract: 'tools/workerd-frame-contract.mjs',
    hostBackend: 'tests/helpers/ws_host_backend.mjs'});
export const frameSizes = Object.freeze([0, 1, 125, 126, 65535, 65536, 1048576]);
export const frameConnections = 4;
function keys(value, names) {
    assert.ok(value && typeof value === 'object');
    assert.deepEqual(Object.keys(value).sort(), [...names].sort());
}
function expectedPayloadDigest(runId, connection, opcode, size) {
    // Reconstruct the specified challenge, independently of the driver's
    // received bytes. Equal but corrupt hashes on all backends must fail too.
    let bytes;
    if (opcode === 1) bytes = Buffer.from(size >= 4 ? 'a'.repeat(size - 4) + '\u{1f600}' : 'a'.repeat(size));
    else {
        bytes = Buffer.alloc(size);
        for (let i = 0; i < size; i++) bytes[i] = (i * 17 + connection + 29) % 251;
    }
    if (size >= 64) Buffer.from(`${runId}:${connection}:`).copy(bytes);
    return createHash('sha256').update(bytes).digest('hex');
}

export function validateFrameReport(report, expected) {
    keys(report, ['schema', 'complete', 'runId', 'mode', 'referenceLock', 'client', 'inputs',
        'binaries', 'connections', 'observations', 'closes', 'healthy', 'shutdown', 'backendStderr']);
    assert.equal(report.schema, 'capsid-websocket-frame-boundaries-v2');
    assert.equal(report.complete, true);
    assert.equal(report.runId, expected.runId);
    assert.match(report.runId, /^[A-Za-z0-9_-]{1,64}$/, 'bounded run challenge');
    assert.ok(['reference', ...referenceBackendModes(expected.lock)].includes(report.mode), 'supported reference platform mode');
    assert.deepEqual(report.referenceLock, expected.lock);
    assert.deepEqual(report.client, {node: expected.lock.client.node_version, undici: expected.lock.client.undici_version});
    keys(report.inputs, Object.keys(frameInputPaths));
    assert.deepEqual(report.inputs, expected.inputs);
    for (const hash of Object.values(report.inputs)) assert.match(hash, /^[0-9a-f]{64}$/);
    assert.deepEqual(report.binaries, {host: report.mode === 'reference' ? expected.lock.binary_sha256 : expected.host,
        worker: report.mode === 'reference' ? null : expected.worker});
    assert.equal(report.connections, frameConnections);
    const inventory = Array.from({length: frameConnections}, (_, connection) =>
        [1, 2].flatMap(opcode => frameSizes.map(size => ({connection, opcode, size})))).flat();
    assert.equal(report.observations.length, inventory.length, 'all four connections execute every boundary');
    const normalized = [];
    for (const [index, actual] of report.observations.entries()) {
        keys(actual, ['connection', 'opcode', 'size', 'lengthTag', 'fin', 'masked', 'rsv', 'sha256']);
        const item = inventory[index];
        assert.deepEqual({connection: actual.connection, opcode: actual.opcode, size: actual.size}, item,
            'complete ordered raw frame inventory');
        assert.equal(actual.lengthTag, item.size < 126 ? item.size : item.size <= 65535 ? 126 : 127);
        assert.equal(actual.fin, true, 'single unfragmented echo frame');
        assert.equal(actual.masked, false, 'unmasked server frame');
        assert.equal(actual.rsv, 0, 'no unrequested extension bits');
        assert.equal(actual.sha256, expectedPayloadDigest(report.runId, item.connection, item.opcode, item.size),
            'actual whole payload matches this run challenge');
        normalized.push(structuredClone(actual));
    }
    assert.deepEqual(report.closes, Array.from({length: frameConnections}, (_, connection) =>
        ({connection, opcode: 8, fin: true, masked: false, rsv: 0, lengthTag: 2, payloadHex: '03e8',
            tcpClosed: true, extraBytes: 0})), 'all four reciprocal 1000 Close handshakes finish the actual transport');
    assert.deepEqual(report.healthy, {status: 200, text: 'frame-boundary-healthy'});
    assert.equal(typeof report.backendStderr, 'string');
    assert.doesNotMatch(report.backendStderr, /ERROR: (?:AddressSanitizer|LeakSanitizer)|runtime error:/);
    if (report.mode === 'reference') {
        keys(report.shutdown, ['code', 'signal', 'forced']);
        assert.equal(report.shutdown.forced, false);
        assert.ok(report.shutdown.code === 0 && report.shutdown.signal === null ||
            report.shutdown.code === null && report.shutdown.signal === 'SIGTERM', 'actual reference stop status');
    } else assert.deepEqual(report.shutdown, {code: 0, signal: null, forced: false});
    return {observations: normalized, closes: report.closes, healthy: report.healthy};
}

export function compareFrameReports(reference, candidates, expected) {
    assert.equal(reference.mode, 'reference');
    assert.deepEqual(candidates.map(r => r.mode).sort(), referenceBackendModes(expected.lock).sort());
    const baseline = validateFrameReport(reference, expected);
    for (const report of candidates) assert.deepEqual(validateFrameReport(report, expected), baseline,
        'raw frame results match the pinned workerd profile');
    return {complete: true, backends: 1 + candidates.length, connectionsPerBackend: frameConnections,
        framesPerBackend: frameConnections * 2 * frameSizes.length, sizes: [...frameSizes],
        opcodes: [1, 2], singleFinFrames: true, reciprocalClose: 1000};
}
