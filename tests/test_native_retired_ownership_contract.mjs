import assert from 'node:assert/strict';
import fs from 'node:fs';
import test from 'node:test';
import {validateRetiredCase, validateRetiredReport} from '../tools/native-retired-ownership-contract.mjs';

const samples = JSON.parse(fs.readFileSync(new URL('./fixtures/native-retired-ownership-contract-samples.json', import.meta.url)));
for (const [group, report] of Object.entries(samples.reports)) {
    test('actual qualified native and primary ' + group, () => validateRetiredReport(report, group));
}
for (const [index, sample] of samples.actualCounterexamples.entries()) {
    test('actual old native failure ' + index, () => assert.throws(() =>
        validateRetiredCase(sample.row, {bodyKind: sample.row.bodyKind, graph: sample.row.graph}, sample.group)));
}
const ordinary = 'promise_await-prelude';
const controls = [
    ['unfinished report', ordinary, r => {r.complete = false;}],
    ['missing case', ordinary, r => {r.cases.pop();}],
    ['duplicate case', ordinary, r => {r.cases[1] = structuredClone(r.cases[0]);}],
    ['reordered cases', ordinary, r => {r.cases.reverse();}],
    ['changed group', ordinary, r => {r.group = 'promise_direct';}],
    ['raised event budget', ordinary, r => {r.eventDeadlineMilliseconds = 5001;}],
    ['raised exit budget', ordinary, r => {r.naturalNativeExitDeadlineMilliseconds = 2501;}],
    ['changed input guard', ordinary, r => {r.measuredInputsUnchanged = false;}],
    ['missing decoded tail', ordinary, r => {r.cases[0].sourceDecodedBytes.pop();}],
    ['changed encoded prefix', ordinary, r => {r.cases[0].sourcePrefixSHA256 = '0'.repeat(64);}],
    ['changed encoded body', ordinary, r => {r.cases[0].sourcePacketSHA256 = '0'.repeat(64);}],
    ['different source URL', ordinary, r => {r.cases[0].targetUrl = 'http://localhost:1/raw-body?readerRoute=wrong';}],
    ['missing source', ordinary, r => {r.cases[0].native.sources = [];}],
    ['source still active', ordinary, r => {r.cases[0].native.sourceTerminalBeforeGuestRequest = false;}],
    ['fabricated fixture EOF', ordinary, r => {r.cases[0].native.sourceBeforeGuestRequest[0].fixtureEnded = true;}],
    ['fabricated fixture close', ordinary, r => {r.cases[0].native.sourceBeforeGuestRequest[0].fixtureDestroyed = true;}],
    ['suffix released early', ordinary, r => {r.cases[0].native.sourceBeforeGuestRequest[0].events.push('suffix-released');}],
    ['missing owner barrier', ordinary, r => {r.cases[0].native.stdout = '';}],
    ['parent forced helper', ordinary, r => {r.cases[0].native.helperForced = true;}],
    ['unobserved physical exit', ordinary, r => {r.cases[0].native.workerExit.processObserved = false;}],
    ['forced worker exit', ordinary, r => {r.cases[0].native.workerExit.forced = true;}],
    ['nonzero physical exit', ordinary, r => {r.cases[0].native.workerExit.exitStatus = 1;}],
    ['missing healthy reuse', ordinary, r => {r.cases[0].native.healthyReuse = false;}],
    ['wrong request owner', ordinary, r => {r.cases[0].native.protocol.replies[0].id = 2;}],
    ['duplicate response head', ordinary, r => {r.cases[0].native.protocol.replies[0].heads = 2;}],
    ['missing response end', ordinary, r => {r.cases[0].native.protocol.replies[0].ends = 0;}],
    ['wrong returned byte credit', ordinary, r => {++r.cases[0].native.protocol.replies[0].returnedCredit;}],
    ['sanitizer finding', ordinary, r => {r.cases[0].native.stderr += '\nruntime error: known bad';}],
    ['coherent false EOF', ordinary, r => {for (const runtime of ['reference', 'native']) for (const stage of ['owner', 'guest']) r.cases[0][runtime][stage].first.done = true;}],
    ['coherent false prefix', ordinary, r => {for (const runtime of ['reference', 'native']) for (const stage of ['owner', 'guest']) r.cases[0][runtime][stage].first.bytes[2] = 4;}],
    ['coherent early fulfillment', ordinary, r => {for (const runtime of ['reference', 'native']) for (const stage of ['owner', 'guest']) r.cases[0][runtime][stage].phase = 'fulfilled';}],
    ['coherent early unlock', ordinary, r => {for (const runtime of ['reference', 'native']) for (const stage of ['owner', 'guest']) r.cases[0][runtime][stage].bodyLocked = false;}],
    ['coherent capacity corruption', ordinary, r => {for (const runtime of ['reference', 'native']) for (const stage of ['owner', 'guest']) r.cases[0][runtime][stage].first.capacity = 1;}],
    ['coherent response metadata corruption', 'branches', r => {for (const runtime of ['reference', 'native']) for (const stage of ['owner', 'guest']) r.cases[0][runtime][stage].response.status = 200;}],
    ['coherent iterator cleanup', 'iterator_loop', r => {for (const runtime of ['reference', 'native']) for (const stage of ['owner', 'guest']) r.cases[0][runtime][stage].leaves[0].locked = false;}],
    ['ordinary live waiter accepted', 'controls', r => {r.cases[1].native.exit.code = 0;}],
    ['ordinary guard missing', 'controls', r => {r.cases[1].native.stderr = '';}],
    ['custom then gets twice', 'controls', r => {for (const runtime of ['reference', 'native']) for (const stage of ['owner', 'guest']) r.cases[3][runtime][stage].getterCalls = 2;}],
    ['ordinary guard forced by parent', 'controls', r => {r.cases[1].native.helperForced = true;}],
    ['completed timer falsely pending', 'controls', r => {for (const runtime of ['reference', 'native']) for (const stage of ['owner', 'guest']) r.cases[4][runtime][stage].phase = 'followup';}]
];
for (const [name, group, mutate] of controls) {
    test('rejects ' + name, () => {
        const value = structuredClone(samples.reports[group]);
        const before = JSON.stringify(value);
        mutate(value);
        assert.notEqual(JSON.stringify(value), before, 'the control changes its actual target');
        assert.throws(() => validateRetiredReport(value, group));
    });
}
