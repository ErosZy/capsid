import assert from 'node:assert/strict';
import {isDeepStrictEqual} from 'node:util';
import {unreadBindingCases, expectedUnreadBindingState, expectedUnreadBindingSources} from './binding-fetch-unread-input.mjs';

const equal = (actual, expected, label) => assert.ok(isDeepStrictEqual(actual, expected), label);
function fields(value, names, label) {
    assert.ok(value && typeof value === 'object' && !Array.isArray(value), label);
    equal(Object.keys(value).sort(), names.slice().sort(), label + ' fields');
}
export const unreadBindingExit = Object.freeze({reaped: true, channelClosed: true, code: 0});
export function assertUnreadBindingCase(row, plan) {
    fields(row, ['plan', 'collectionComplete', 'port', 'request1', 'sourceAfterRequest1', 'request2', 'sourceBeforeStop',
        'workerExit', 'helperExit', 'sourceTerminalAfterWorkerExit', 'sourceAfterWorkerExit', 'stdout', 'stderr',
        'sourceFinalBeforeFixtureCleanup'], 'complete actual unread case');
    equal(row.plan, plan, 'mandatory current plan');
    equal(row.collectionComplete, true, 'actual observation completed');
    assert.ok(Number.isSafeInteger(row.port) && row.port > 0 && row.port < 65536, 'actual loopback port');
    for (const request of [1, 2]) equal(row['request' + request], expectedUnreadBindingState(plan, request),
        'independent complete native state across User requests');
    const live = expectedUnreadBindingSources(plan, row.port);
    equal(row.sourceAfterRequest1, live, 'all native resources live after first completed User request');
    equal(row.sourceBeforeStop, live, 'all native resources still live after second completed User request');
    equal(row.sourceTerminalAfterWorkerExit, true, 'actual peer terminals observed after Worker exit');
    equal(row.sourceAfterWorkerExit.length, plan.count, 'every independent actual peer accounted');
    for (const [slot, source] of row.sourceAfterWorkerExit.entries()) {
        assert.equal(typeof source.peerEnded, 'boolean'); assert.equal(typeof source.peerReset, 'boolean');
        assert.ok(source.peerEnded || source.peerReset, 'actual FIN/reset before fixture cleanup');
        const expected = expectedUnreadBindingSources(plan, row.port, source.peerEnded)[slot];
        expected.peerReset = source.peerReset;
        equal(source, expected, 'complete raw HTTP, event order and no fixture EOF/destroy');
        for (const value of [source.rawRequest, ...source.sent])
            equal(Buffer.from(value, 'base64').toString('base64'), value, 'canonical complete bytes');
    }
    equal(row.sourceFinalBeforeFixtureCleanup, row.sourceAfterWorkerExit, 'fixture teardown supplies no terminal');
    equal(row.helperExit, {code: 0, signal: null}, 'actual helper OS exit');
    equal(row.workerExit, unreadBindingExit, 'actual reaped clean Worker OS exit');
    equal(row.stderr, '', 'no runtime diagnostics');
    assert.equal(typeof row.stdout, 'string');
    const features = process.platform === 'win32' ? 0 : 1, lines = row.stdout.trimEnd().split(/\r?\n/);
    equal(lines.length, 6, 'complete helper output');
    equal(lines[0], `memoryLimit=0 actualFeatures=${features} expectedFeatures=${features}`, 'existing helper resource defaults');
    equal([lines[1], lines[3]], ['BINDING_REQUEST 1', 'BINDING_REQUEST 2'], 'actual User request sequence');
    for (const [request, index] of [[1, 2], [2, 4]]) {
        assert.ok(lines[index].startsWith('BINDING_PHASE ' + request + ' '));
        equal(JSON.parse(lines[index].slice(16)), row['request' + request], 'full actual C ABI state, no substituted report');
    }
    assert.ok(lines[5].startsWith('BINDING_EXIT '));
    equal(JSON.parse(lines[5].slice(13)), row.workerExit, 'actual natural OS exit observation');
    return {requests: 2, rpcCalls: 2, sourceConnections: plan.count};
}
export function validateUnreadBindingReport(report, expected) {
    fields(report, ['schema', 'complete', 'kind', 'runId', 'inputs', 'binaries', 'cases'], 'unread Binding report');
    equal(report.schema, 'capsid-binding-fetch-unread-v1', 'report schema');
    equal(report.complete, true, 'actual report complete');
    for (const key of ['kind', 'runId', 'inputs', 'binaries']) equal(report[key], expected[key], 'actual identity ' + key);
    const plans = unreadBindingCases(report.kind);
    equal(report.cases.map(row => row.plan), plans, 'all mandatory ordered cases');
    return report.cases.map((row, index) => assertUnreadBindingCase(row, plans[index]));
}
