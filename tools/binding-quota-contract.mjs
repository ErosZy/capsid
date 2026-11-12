import assert from 'node:assert/strict';
import {isDeepStrictEqual} from 'node:util';
import {bindingQuotaVariants, requestQuotaTemplate, workerQuotaTemplate, bindingQuotaExit,
    requestQuotaTranscript, workerQuotaTranscript} from './binding-quota-input.mjs';

const equal = (actual, expected) => assert.ok(isDeepStrictEqual(actual, expected), 'complete current typed quota value');
const fields = (value, names) => {
    assert.ok(value && typeof value==='object' && !Array.isArray(value)); equal(Object.keys(value).sort(), names.slice().sort());
};
const normalized = text => {assert.equal(typeof text, 'string'); return text.replace(/\r\n/g, '\n');};
export function assertRequestQuotaObservation(row, variant, platform) {
    fields(row, ['variant', 'stdout', 'stderr', 'request1', 'request2', 'workerExit', 'child']);
    assert.equal(row.variant, variant); const expected = requestQuotaTemplate(variant);
    equal(row.request1, expected.request1); equal(row.request2, expected.request2);
    equal(row.child, {code: 0, signal: null}); equal(row.workerExit, bindingQuotaExit()); assert.equal(row.stderr, '');
    equal(normalized(row.stdout), requestQuotaTranscript(row, platform)); return expected;
}
export function assertWorkerQuotaObservation(row, platform) {
    fields(row, ['stdout', 'stderr', 'responses', 'logs', 'events', 'workerExit', 'child']);
    const expected = workerQuotaTemplate();
    equal(row.responses, expected.responses); equal(row.logs, expected.logs); equal(row.events, expected.events);
    equal(row.child, {code: 0, signal: null}); equal(row.workerExit, bindingQuotaExit()); assert.equal(row.stderr, '');
    equal(normalized(row.stdout), workerQuotaTranscript(row, platform)); return expected;
}
export function validateBindingQuotaReport(report, expected) {
    fields(report, ['schema', 'complete', 'runId', 'kind', 'platform', 'inputs', 'binaries', 'observations']);
    assert.equal(report.schema, 'capsid-binding-quota-v1'); assert.equal(report.complete, true);
    assert.equal(report.runId, expected.runId); assert.match(report.runId, /^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$/);
    assert.equal(report.kind, expected.kind); assert.ok(['request', 'worker'].includes(report.kind));
    assert.equal(report.platform, expected.platform); assert.ok(['win32', 'linux', 'darwin'].includes(report.platform));
    equal(report.inputs, expected.inputs); equal(report.binaries, expected.binaries);
    assert.ok(Array.isArray(report.observations)); assert.equal(report.observations.length, report.kind==='request' ? 4 : 1);
    return report.kind==='request'
        ? report.observations.map((row, index) => assertRequestQuotaObservation(row, bindingQuotaVariants[index], expected.platform))
        : [assertWorkerQuotaObservation(report.observations[0], expected.platform)];
}
