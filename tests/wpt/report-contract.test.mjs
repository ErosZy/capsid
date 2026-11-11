import assert from 'node:assert/strict';
import { test } from 'node:test';
import app from './app.js';

const expected = {
    name: 'known deviation', deviation: 'CAPSID-D010',
    errorName: 'AssertionError', message: 'expected null, got {}',
};
const failure = {
    name: expected.name, status: 'FAIL', errorName: expected.errorName,
    phase: 'test', message: `${expected.message}\n    at upstream-test.js:10`,
};
async function report(tests, expectations = [expected]) {
    globalThis.__wptCompletion = Promise.resolve(tests);
    globalThis.__wptExpectedFailures = expectations;
    return (await app.fetch()).json();
}

test('a recognized failure retains its complete diagnostic', async () => {
    const result = await report([failure]);
    assert.equal(result.passed, true);
    assert.deepEqual(result.expectedFailures, [{ ...failure, deviation: expected.deviation }]);
});

for (const [name, change] of [
    ['a different assertion', { message: 'unrelated data was corrupted' }],
    ['an unexpected exception', { errorName: 'TypeError' }],
    ['a timeout', { status: 'TIMEOUT', phase: 'timeout' }],
    ['a cleanup failure', { phase: 'cleanup' }],
    ['a missing error class', { errorName: undefined }],
    ['an unknown status', { status: 'SKIP' }],
]) {
    test(`${name} cannot use the known test name as a waiver`, async () => {
        const changed = { ...failure, ...change };
        const result = await report([changed]);
        assert.equal(result.passed, false);
        assert.deepEqual(result.failures, [JSON.parse(JSON.stringify(changed))]);
        assert.equal(result.expectedFailures.length, 0);
        assert.equal(result.missingExpectedFailures.length, 1);
    });
}

test('a waiver without a diagnostic contract is rejected', async () => {
    const result = await report([failure], [{ name: expected.name, deviation: expected.deviation }]);
    assert.equal(result.passed, false);
});

test('unexpected success must remove the stale waiver', async () => {
    const result = await report([{ name: expected.name, status: 'PASS' }]);
    assert.equal(result.passed, false);
    assert.equal(result.missingExpectedFailures.length, 1);
});

test('an empty run and an unlisted failure never pass', async () => {
    assert.equal((await report([], [])).passed, false);
    assert.equal((await report([failure], [])).passed, false);
    assert.equal((await report([{ name: 'ordinary test', status: 'PASS' }], [])).passed, true);
});
