import assert from 'node:assert/strict';
import {test} from 'node:test';
import {responseLimitCases, expectedResponseLimitState, assertResponseLimitRow,
    assertResponseLimitExit, validateResponseLimitTranscript, gzipWireLength} from '../tools/fetch-response-limit-contract.mjs';

const cases = responseLimitCases();
const row = scenario => ({limit: scenario.limit, fragmented: scenario.fragmented,
    channel: scenario.channel, sent: scenario.encoding ? (scenario.action === 'full' ? gzipWireLength(scenario.length, scenario.fragmented) : 25) : scenario.action === 'full' ? scenario.length : 5,
    credit: scenario.action === 'cancel' ? (scenario.encoding ? 17 : 2) : scenario.action === 'pending' ? (scenario.encoding ? 25 : 5) : scenario.encoding && scenario.fragmented && scenario.length ? 25 : 0,
    cancels: scenario.action !== 'full' || scenario.limit && scenario.known && !scenario.encoding && scenario.length > scenario.limit ? 1 : 0,
    live: false, observation: expectedResponseLimitState(scenario)});
const lines = [];
const exitRow = limit => ({limit, shutdownResult: 0, flushResult: 0, eventResult: 0,
    channelClosed: true, errorEvents: 0, processObserved: true, exitStatus: 0,
    waitError: 0, destroyForced: false});
for (const limit of [8, 0]) {
    lines.push(...cases.filter(value => value.limit === limit).map(value => 'LIMIT_ROW ' + JSON.stringify(row(value))));
    lines.push('LIMIT_WORKER_EXIT ' + JSON.stringify(exitRow(limit)));
    lines.push(`LIMIT_WORKER_COMPLETE limit=${limit} rows=160 slots=1 naturalExit=true`);
}
const transcript = lines.join('\n') + '\n';

const encodedCases = responseLimitCases('gzip');
test('encoded Upgrade regression retains320 rows and160 distinct known-length header variants', () => {
    assert.equal(encodedCases.length, 320);
    assert.equal(encodedCases.filter(value => value.encoding === 'gzip').length, 160);
    assert.throws(() => responseLimitCases('deflate'));
    const encodedLines = [];
    for (const limit of [8, 0]) {
        encodedLines.push(...encodedCases.filter(value => value.limit === limit).map(value => 'LIMIT_ROW '+JSON.stringify(row(value))));
        encodedLines.push('LIMIT_WORKER_EXIT '+JSON.stringify(exitRow(limit)));
        encodedLines.push(`LIMIT_WORKER_COMPLETE limit=${limit} rows=160 slots=1 naturalExit=true`);
    }
    const stdout = encodedLines.join('\n')+'\n';
    assert.deepEqual(validateResponseLimitTranscript(stdout, {encoding: 'gzip'}),
        {complete: true, workers: 2, rows: 320, configuredLimits: [8, 0], slotsPerWorker: 1,
            knownLengthContentEncoding: 'gzip', encodedHeaderCases: 160});
    assert.throws(() => validateResponseLimitTranscript(stdout));
});
for (const scenario of encodedCases.filter(value => value.encoding)) test('reject missing actual encoded-header observation '+JSON.stringify(scenario), () => {
    const value = row(scenario);
    assertResponseLimitRow(value, scenario);
    delete value.observation.response.contentEncoding;
    assert.throws(() => assertResponseLimitRow(value, scenario));
});

for (const limit of [8, 0]) {
    test('independent successful exit observation for configured limit ' + limit,
        () => assertResponseLimitExit(exitRow(limit), limit));
    for (const field of Object.keys(exitRow(limit))) test('reject changed exit field ' + field + ' limit ' + limit, () => {
        const changed = exitRow(limit);
        changed[field] = typeof changed[field] === 'boolean' ? !changed[field] : changed[field] + 1;
        assert.throws(() => assertResponseLimitExit(changed, limit));
    });
    for (const field of Object.keys(exitRow(limit))) test('reject missing exit field ' + field + ' limit ' + limit, () => {
        const changed = exitRow(limit); delete changed[field];
        assert.throws(() => assertResponseLimitExit(changed, limit));
    });
    test('reject extra exit field limit ' + limit,
        () => assert.throws(() => assertResponseLimitExit({...exitRow(limit), assumedExit: true}, limit)));
}

test('independent inventory has 256 EOF and 64 consumer cancellation cases on real configured workers', () => {
    assert.equal(cases.length, 320);
    assert.equal(cases.filter(value => value.action === 'full').length, 256);
    assert.equal(cases.filter(value => value.action === 'pending').length, 32);
    assert.equal(new Set(cases.map(value => JSON.stringify(value))).size, 320);
    assert.deepEqual(validateResponseLimitTranscript(transcript),
        {complete: true, workers: 2, rows: 320, configuredLimits: [8, 0], slotsPerWorker: 1});
});
test('manual boundary model: unknown fragmented overflow preserves only the first five bytes', () => {
    const scenario = cases.find(value => value.limit === 8 && !value.known && value.fragmented &&
        value.method === 'least' && value.length === 9 && value.route === 'getReader');
    assert.deepEqual(expectedResponseLimitState(scenario).reads.map(value => value.bytes), [[1, 2], [3, 4, 5]]);
    assert.equal(expectedResponseLimitState({...scenario, method: 'high'}).reads.length, 0);
    assert.deepEqual(expectedResponseLimitState({...scenario, limit: 0, method: 'high'}).reads.map(value => value.bytes),
        [[1, 2, 3, 4, 5, 6, 7, 8, 9], []]);
});
for (const scenario of cases) {
    const value = row(scenario);
    const key = JSON.stringify(scenario);
    for (const [name, mutate] of [
        ['missing native brand', r => {r.observation.phase = 'getReader';}],
        ['wrong response metadata', r => {r.observation.response.contentLength = '999';}],
        ['leaked slot', r => {r.live = true;}],
        ['credit for unsent bytes', r => {r.credit = r.sent + 1;}],
        ['duplicate cancellation', r => {r.cancels = 2;}],
        ['not disturbed', r => {r.observation.bodyUsed = false;}],
        ['retained reader', r => {r.observation.locked = true;}],
        ['unsettled closed', r => {r.observation.closed = null;}],
        ['missing final state', r => {delete r.observation.errorIdentity;}],
        ['unknown extra observation', r => {r.extra = true;}],
    ]) test('reject ' + name + ' ' + key, () => {
        const changed = structuredClone(value); mutate(changed);
        assert.throws(() => assertResponseLimitRow(changed, scenario));
    });
    if (value.observation.pending) for (const [name, mutate] of [
        ['settled before cancellation', p => {p.beforeCancel.readSettled = true;}],
        ['lost cancellation reason', p => {p.result.tag = null;}],
        ['wrong reason identity', p => {p.result.sameReason = true;}],
        ['lost transferred buffer', p => {p.inputAfter = 32;}],
    ]) test('reject pending ' + name + ' ' + key, () => {
        const changed = structuredClone(value); mutate(changed.observation.pending);
        assert.throws(() => assertResponseLimitRow(changed, scenario));
    });
    for (let index = 0; index < value.observation.reads.length; index++) {
        test('reject changed body or EOF at read ' + index + ' ' + key, () => {
            const changed = structuredClone(value);
            changed.observation.reads[index].bytes = [255];
            assert.throws(() => assertResponseLimitRow(changed, scenario));
        });
    }
}
for (const [name, mutate] of [
    ['missing row', values => {values.splice(1, 1);}],
    ['duplicate row', values => {values.splice(1, 0, values[0]);}],
    ['reordered rows', values => {[values[0], values[1]] = [values[1], values[0]];}],
    ['missing worker completion', values => {values.pop();}],
    ['forced completion', values => {values[160] = values[160].replace('true', 'false');}],
    ['legacy completion without process evidence', values => {
        for (let index = values.length - 1; index >= 0; index--) if (values[index].startsWith('LIMIT_WORKER_EXIT ')) values.splice(index, 1);
    }],
    ['duplicate exit evidence', values => {values.splice(160, 0, values[160]);}],
    ['wrong worker exit evidence', values => {values[160] = values[322];}],
    ['exit fields hidden by duplicate JSON key', values => {
        values[160] = values[160].replace('"destroyForced":false', '"destroyForced":true,"destroyForced":false');
    }],
    ['unexpected output', values => {values.push('skipped');}],
]) test('transcript fails closed on ' + name, () => {
    const changed = [...lines]; mutate(changed);
    assert.throws(() => validateResponseLimitTranscript(changed.join('\n')));
});
