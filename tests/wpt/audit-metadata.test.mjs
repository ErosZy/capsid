import assert from 'node:assert/strict';
import fs from 'node:fs';
import { auditMetadata } from './audit-metadata.mjs';

const manifestPath = process.argv[2];
const deviationsPath = process.argv[3];
if (!manifestPath || !deviationsPath) {
    throw new Error('expected manifest and deviations paths');
}

const original = JSON.parse(fs.readFileSync(manifestPath, 'utf8'));
const deviations = fs.readFileSync(deviationsPath, 'utf8');
const clone = () => structuredClone(original);
const rejects = (mutate, pattern) => {
    const value = clone();
    mutate(value);
    assert.throws(() => auditMetadata(value, deviations), pattern);
};

const result = auditMetadata(original, deviations);
assert.ok(result.expectedFailures > 0);
assert.ok(result.notExecuted > 0);

rejects(
    value => {
        value.expectedFailures[0].deviation = 'CAPSID-D999';
    },
    /unknown deviation id/,
);
rejects(
    value => {
        value.expectedFailures.push(structuredClone(value.expectedFailures[0]));
    },
    /duplicate expected failure/,
);
rejects(
    value => {
        value.tests.find(test => test.notExecuted).notExecutedReason = '';
    },
    /notExecutedReason must be a non-empty string/,
);
rejects(
    value => {
        const group = value.tests.find(test => test.notExecuted);
        group.notExecuted[0] = value.executedProfile[0];
    },
    /both executed and notExecuted/,
);

console.log('PASS: WPT metadata positive and negative controls');
