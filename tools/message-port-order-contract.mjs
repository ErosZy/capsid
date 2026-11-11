import assert from 'node:assert/strict';

export const messagePortInputPaths = Object.freeze({
    fixture: 'tests/fixtures/message-port-order.mjs',
    runner: 'tests/test_message_port_order.mjs',
    helper: 'tests/helpers/ws_host_backend.mjs',
    contract: 'tools/message-port-order-contract.mjs',
});

// Public observations captured from the locked native workerd, rather than
// inferred from Capsid. The complete fixture runs in one HTTP request.
const expected = {
    promiseOrder: ['promise/before', 'port', 'promise/after', 'port/promise'],
    nestedOrder: ['first', 'after/post', 'second', 'first/promise'],
    intrinsic: {error: null, trace: ['delivered']},
    nestedRejection: {events: [], trace: ['first', 'second', 'catch/same']},
};

export function assertMessagePortObservation(value, label) {
    assert.deepEqual(value, expected, `exact ${label} MessagePort observation`);
}

export function assertMessagePortReport(report, {mode, runId, inputs, binaries, lock}) {
    assert.equal(report.schema, 'capsid-message-port-order-v1');
    assert.equal(report.complete, true);
    assert.equal(report.status, 'complete');
    assert.equal(report.mode, mode);
    assert.equal(report.runId, runId);
    assert.equal(report.requestDeadlineMilliseconds, 5000);
    assert.equal(report.functionalQualification, true);
    assert.equal(report.performanceQualification, false);
    assert.equal(report.workerOsExitNotInferredFromHostExit, true);
    assert.equal(report.measuredInputsUnchanged, true);
    assert.deepEqual(report.inputs, inputs);
    assert.deepEqual(report.binaries, binaries);
    assert.deepEqual(report.referenceLock, lock);
    for (const name of ['reference', 'native']) {
        const row = report.roles[name];
        assert.equal(row.httpStatus, 200);
        assertMessagePortObservation(row.public, name);
        assert.doesNotMatch(row.stderr, /AddressSanitizer|LeakSanitizer|runtime error:/);
    }
    assert.deepEqual(report.roles.native.shutdown, {code: 0, signal: null, forced: false});
    assert.equal(report.roles.reference.cleanupRequested, true);
    return {complete: true, mode, publicProperties: 4};
}
