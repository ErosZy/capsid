import assert from 'node:assert/strict';

export const responseCreditPlans = Object.freeze([262143, 262144, 262145, 786469]);
export const responseCreditInputPaths = Object.freeze({
    fixture: 'tests/fixtures/native-fetch-response-credit.mjs',
    helper: 'tests/test_native_fetch_response_limits.cc',
    driver: 'tests/test_native_fetch_response_credit.mjs',
    contract: 'tools/native-fetch-response-credit-contract.mjs',
    controls: 'tests/test_native_fetch_response_credit_contract.mjs',
});
const exit = {clean: true, processObserved: true, exitStatus: 0, forced: false, errorEvents: 0};

// This byte model does not import the fixture's string-generation code.
export function expectedCreditBody(bytes) {
    const body = Buffer.alloc(bytes);
    for (let i = 0; i < bytes; i++) {
        const n = i % 36;
        body[i] = n < 26 ? 97 + n : 48 + n - 26;
    }
    return body.toString('ascii');
}
function fields(value, names) {
    assert.ok(value && typeof value === 'object' && !Array.isArray(value));
    assert.deepEqual(Object.keys(value).sort(), names.slice().sort());
}
export function assertResponseCreditCase(row, bytes) {
    fields(row, ['bytes', 'collectionComplete', 'stdout', 'stderr', 'helperExit', 'workerExit']);
    assert.equal(row.bytes, bytes);
    assert.equal(row.collectionComplete, true);
    assert.deepEqual(row.helperExit, {code: 0, signal: null});
    assert.deepEqual(row.workerExit, exit);
    assert.equal(row.stderr, '');
    const lines = row.stdout.trimEnd().split(/\r?\n/);
    assert.deepEqual(lines.filter(line => line.startsWith('LIMIT_PROBE ')), ['LIMIT_PROBE ' + expectedCreditBody(bytes)]);
    assert.deepEqual(lines.filter(line => line.startsWith('LIMIT_EXIT ')), ['LIMIT_EXIT ' + JSON.stringify(exit)]);
    assert.ok(lines.every(line => line.startsWith('LIMIT_PROBE ') || line.startsWith('LIMIT_EXIT ') ||
        /^LIMIT_AUX type=(?:6|10|11) request=\d+ status=\d+ payload=.*$/.test(line)));
}
export function validateResponseCreditReport(report, expected) {
    fields(report, ['schema', 'complete', 'runId', 'inputs', 'binaries', 'cases']);
    assert.equal(report.schema, 'capsid-native-fetch-response-credit-v1');
    assert.equal(report.complete, true);
    assert.equal(report.runId, expected.runId);
    assert.deepEqual(report.inputs, expected.inputs);
    assert.deepEqual(report.binaries, expected.binaries);
    assert.deepEqual(report.cases.map(row => row.bytes), responseCreditPlans);
    for (const [i, row] of report.cases.entries()) assertResponseCreditCase(row, responseCreditPlans[i]);
}
