import {referenceBackendModes} from '../tools/workerd-platform.mjs';
import assert from 'node:assert/strict';
import test from 'node:test';
import fs from 'node:fs';
import {ownerKinds, assertOwnerCase, validateOwnerReport, compareOwnerReports} from '../tools/workerd-owner-contract.mjs';

// Stored actual reference observations serve only as contract-unit inputs.
// The release gate always requires a fresh native reference process and hashes.
const stored = JSON.parse(fs.readFileSync(new URL('./fixtures/workerd-owner-report.json', import.meta.url), 'utf8'));
function matrix() {
    const reference = structuredClone(stored);
    const expected = {runId: reference.runId, lock: reference.referenceLock, inputs: reference.inputs,
        host: 'a'.repeat(64), worker: 'b'.repeat(64)};
    const candidates = referenceBackendModes(expected.lock).map(mode => ({...structuredClone(reference), mode,
        binaries: {host: expected.host, worker: expected.worker}, cases: reference.cases.map(value =>
            ({...structuredClone(value), shutdown: {code: 0, signal: null, forced: false}}))}));
    return {reference, candidates, expected};
}
test('literal seventeen-case owner inventory includes normal, closing and external-resolver lifetimes', () => {
    assert.deepEqual(ownerKinds, ['live-one-shot', 'live-interval', 'close-with-ack', 'close-delayed-ack',
        'error-timer-before', 'error-listener-timer', 'error-final-close-timer', 'http-await',
        'live-returned-then', 'live-detached-then', 'close-returned-await', 'close-detached-await',
        'close-external-returned', 'close-external-detached', 'cross-clear', 'same-clear', 'half-open-close-await']);
});
test('actual stored native reference satisfies the absolute owner contracts', () => {
    const {reference, candidates, expected} = matrix();
    assert.deepEqual(compareOwnerReports(reference, candidates, expected), {complete: true, casesPerBackend: 17, backends: 1 + candidates.length});
});
for (const [name, mutate, diagnostic] of [
    ['timer never fires on an open connection', value => {value.state.state.timerFired = 0;}, /complete lifecycle trace/],
    ['HTTP observation agrees with a corrupt timer count', value => {
        value.state.state.timerFired = 0; value.stateText = JSON.stringify(value.state);
    }, /actual HTTP state/],
    ['timer runs after Close ACK', value => {value.state.state.timerFired = 1;}, /complete lifecycle trace/],
    ['interval silently loses its second callback', value => {value.state.state.trace.pop();}, /complete lifecycle trace/],
    ['missing trusted error', value => {value.state.state.events.pop();}, /complete lifecycle trace/],
    ['an extra close callback after handler failure', value => {value.state.state.events.push({type: 'close', readyState: 3});}, /complete lifecycle trace/],
    ['wrong event ready state', value => {value.state.state.events[0].readyState = 3;}, /complete lifecycle trace/],
    ['closed external Promise reaction executes pure JavaScript', value => {value.state.state.trace.push('resumed');}, /complete lifecycle trace/],
    ['foreign clear cancels a connection timer', value => {value.state.state.timerFired = 0;}, /complete lifecycle trace/],
    ['same connection cannot clear its earlier timer', value => {value.state.state.timerFired = 1;}, /complete lifecycle trace/],
    ['half-open peer Close cancels the timer prematurely', value => {value.state.state.timerFired = 0;}, /complete lifecycle trace/],
    ['half-open close event reports CLOSED', value => {value.state.state.events[1].readyState = 3;}, /complete lifecycle trace/],
    ['state read uses a fresh unhealthy realm', value => {value.stateStatus = 503;}, /same realm remains healthy/],
    ['peer completion replaced by local disposal', value => {value.wire.completion = 'client-disposed';}, /no deadline or local disposal/],
    ['transport never actually closes', value => {value.wire.peerTerminated = false;}, /actual peer terminal/],
    ['truncated raw bytes despite matching decoded frames', value => {value.wire.rawTail = '';}, /exact raw bytes/],
    ['lost queued message', value => {value.wire.frames.shift();}, /exact complete frames/],
    ['forged Upgrade challenge', value => {value.wire.requestKey = 'a'.repeat(22) + '==';}, /Upgrade challenge/],
    ['timer canceled before delayed ACK', value => {
        const state = JSON.parse(value.beforeAckHTTP.body); state.state.timerFired = 0;
        value.beforeAckHTTP.body = JSON.stringify(state);
    }, /timer remains live while awaiting ACK/],
    ['transport ends before delayed ACK', value => {value.beforeManualAck.peerTerminated = true;}, /does not terminate before ACK/],
    ['external resolver was never invoked', value => {delete value.resumeResponse;}, /complete fields/],
    ['foreign clear happens only after timer fired', value => {value.clearResponse.text = '{"before":1}';}, /foreign clear attempted/],
    ['forced shutdown', value => {value.shutdown.forced = true;}, /cooperative shutdown/],
    ['sanitizer diagnostic', value => {value.stderr = 'ERROR: AddressSanitizer: heap-use-after-free';}, /no runtime diagnostics/],
]) test('owner contract rejects ' + name, () => {
    const kind = name.includes('half-open') ? 'half-open-close-await' :
        name.includes('delayed ACK') ? 'close-delayed-ack' : name.includes('Close ACK') ? 'close-with-ack' :
        name.includes('interval') ? 'live-interval' : name.includes('trusted error') || name.includes('handler failure') ? 'error-timer-before' :
        name.includes('external') ? 'close-external-returned' : name.includes('foreign clear') ? 'cross-clear' :
        name.includes('same connection') ? 'same-clear' : 'live-one-shot';
    const value = structuredClone(stored.cases.find(value => value.id === kind));
    mutate(value); assert.throws(() => assertOwnerCase(kind, value, 'reference'), diagnostic);
});
for (const [name, mutate, diagnostic] of [
    ['missing case on every backend', reports => reports.forEach(report => report.cases.pop()), /complete ordered connection owner inventory/],
    ['duplicate case on every backend', reports => reports.forEach(report => report.cases[1] = structuredClone(report.cases[0])), /complete ordered connection owner inventory/],
    ['same corrupt state on every backend', reports => reports.forEach(report => {
        report.cases[0].state.state.timerFired = 0; report.cases[0].stateText = JSON.stringify(report.cases[0].state);
    }), /actual HTTP state/],
    ['stale run', reports => {reports[1].runId = 'previous-run';}, /current connection owner run/],
    ['different fixture', reports => {reports[1].inputs.fixture = 'c'.repeat(64);}, /input provenance/],
    ['different Worker', reports => {reports[1].binaries.worker = 'c'.repeat(64);}, /binary provenance/],
    ['partial collection', reports => {reports[1].complete = false;}, /complete connection owner run/],
]) test('owner report rejects ' + name, () => {
    const {reference, candidates, expected} = matrix(); mutate([reference, ...candidates]);
    assert.throws(() => compareOwnerReports(reference, candidates, expected), diagnostic);
});
test('owner report requires the complete supported backend matrix', () => {
    const {reference, candidates, expected} = matrix();
    assert.throws(() => compareOwnerReports(reference, candidates.slice(0, 1), expected));
    assert.equal(validateOwnerReport(reference, expected).length, 17);
});

test('Linux validation rejects omission of managed from a verified healthy backend matrix', () => {
    const m=matrix();assert.ok(referenceBackendModes(m.expected.lock).includes('managed'));
    assert.doesNotThrow(()=>compareOwnerReports(m.reference,m.candidates,m.expected));
    assert.throws(()=>compareOwnerReports(m.reference,m.candidates.filter(r=>r.mode!=='managed'),m.expected));
});
