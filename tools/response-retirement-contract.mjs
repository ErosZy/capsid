import assert from 'node:assert/strict';

export const retirementActions = ['timeout', 'cancel', 'grant', 'shutdown'];
export const retirementInputs = {
    helper: 'tests/test_worker_response_retirement.cc',
    fixture: 'tests/fixtures/response-retirement.mjs',
    collector: 'tests/test_worker_response_retirement.mjs',
    model: 'tools/response-retirement-contract.mjs',
    exit: 'tests/graceful_worker_exit.h',
    contract: 'tests/test_worker_response_retirement_contract.mjs'
};
export function retirementPlans(action) {
    assert.ok(retirementActions.includes(action));
    return ['snapshot', 'stream'].flatMap(kind => [33, 101].map(bytes => ({id: `${kind}-${action}-${bytes}`, kind, action, bytes})));
}
export function retirementBody(size) {
    return Buffer.from(Array.from({length: size}, (_, index) => 'abcdefghijklmnopqrstuvwxyz0123456789'[index % 36]).join(''));
}
const event = (type, request, flags, status, credit, bytes = Buffer.alloc(0)) =>
    `RETIREMENT_EVENT type=${type} request=${request} flags=${flags} status=${status} credit=${credit} payload=${bytes.toString('hex')}`;
const head = Buffer.from('000001000c00636f6e74656e742d7479706518000000746578742f706c61696e3b636861727365743d5554462d38', 'hex');
export const naturalRetirementExit = 'RETIREMENT_EXIT clean=1 shutdown=0 flush=0 event=0 closed=1 reaped=1 status=0 wait=0 forced=0 errors=0';
export function retirementConfig(expected) {
    return `RETIREMENT_CONFIG timeout=750 window=32 heap=67108864 process=${expected.processMemory} limit=0 ready=${expected.features} compatibility=${expected.compatibility} manifest=${expected.manifest}`;
}
export function retirementTranscript(plan, expected, milliseconds = 750) {
    const lines = [retirementConfig(expected), event(1, 0, expected.features, 0, 0, Buffer.from(expected.compatibility)),
        event(3, 1, 0, 200, 0, head), event(4, 1, 0, 0, 0, retirementBody(plan.bytes).subarray(0, 32))];
    if (plan.action === 'timeout') lines.push(event(9, 1, 1, 0, 0, Buffer.from('request deadline exceeded')));
    if (plan.action === 'grant') {
        let consumed = 32;
        for (let at = 32; at < plan.bytes; at += 32) {
            lines.push(`RETIREMENT_CREDIT request=1 bytes=${consumed}`);
            const bytes = retirementBody(plan.bytes).subarray(at, at + 32);
            lines.push(event(4, 1, 0, 0, 0, bytes)); consumed = bytes.length;
        }
        lines.push(`RETIREMENT_CREDIT request=1 bytes=${consumed}`, event(5, 1, 0, 0, 0));
    }
    lines.push(`RETIREMENT_ACTION kind=${plan.kind} action=${plan.action} bytes=${plan.bytes} received=${plan.action === 'grant' ? plan.bytes : 32} milliseconds=${milliseconds}`);
    if (plan.action !== 'shutdown') lines.push(event(3, 2, 1, 200, 7, head), event(4, 2, 0, 0, 0, Buffer.from('healthy')),
        'RETIREMENT_CREDIT request=2 bytes=7', event(5, 2, 0, 0, 0));
    lines.push(naturalRetirementExit);
    return lines.join('\n') + '\n';
}
export function assertRetirementCase(row, plan, expected) {
    assert.deepEqual(row.plan, plan, 'exact case identity');
    assert.equal(row.collectionComplete, true, 'complete collection');
    assert.deepEqual(row.helperExit, {code: 0, signal: null}, 'natural helper exit');
    assert.equal(row.helperForced ?? false, false, 'no forced helper termination');
    assert.equal(row.stderr, '', 'no runtime diagnostics');
    assert.equal(typeof row.stdout, 'string');
    const lines = row.stdout.replace(/\r\n/g, '\n').split('\n');
    assert.equal(lines.pop(), '', 'complete newline-terminated transcript');
    const marker = lines.find(line => line.startsWith('RETIREMENT_ACTION '));
    const match = /^RETIREMENT_ACTION kind=(snapshot|stream) action=(timeout|cancel|grant|shutdown) bytes=(33|101) received=(\d+) milliseconds=(\d+)$/.exec(marker ?? '');
    assert.ok(match, 'complete action and elapsed observation');
    const elapsed = Number(match[5]);
    assert.ok(Number.isSafeInteger(elapsed) && elapsed >= 0 && elapsed < 5000);
    if (plan.action === 'timeout') assert.ok(elapsed >= 750, 'never report timeout before the configured deadline');
    assert.equal(lines.join('\n') + '\n', retirementTranscript(plan, expected, elapsed),
        'exact metadata, credit, ordered bytes, terminal, healthy reuse and OS exit');
}
export function validateRetirementReport(report, expected) {
    assert.equal(report.schema, 'capsid-response-retirement-v1');
    assert.equal(report.complete, true);
    for (const key of ['action', 'runId', 'inputs', 'binaries', 'processMemory', 'features', 'compatibility', 'manifest'])
        assert.deepEqual(report[key], expected[key], `bound ${key}`);
    assert.deepEqual(report.failures, []);
    const plans = retirementPlans(expected.action);
    assert.equal(report.cases.length, plans.length);
    for (let index = 0; index < plans.length; ++index) assertRetirementCase(report.cases[index], plans[index], expected);
    return {cases: plans.length, timeoutMs: 750, window: 32};
}
