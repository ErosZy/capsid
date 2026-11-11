import assert from 'node:assert/strict';
import path from 'node:path';
import { pathToFileURL } from 'node:url';
import { root } from './workerd-reference.mjs';
import { assertHandlerThrowObservation } from './workerd-throw-contract.mjs';
import { assertHandlerErrorObservations } from './workerd-handler-error-contract.mjs';

// Independent of the producer's variants. Each entry names the assertion
// that must detect this fault, rather than accepting any nonzero process exit.
export const requiredMutationCases = Object.freeze([
    ['baseline', null],
    ['wire-code', /c03: actual wire close conversion/],
    ['wire-reason', /e-close: actual wire close conversion/],
    ['throw-send-dropped', /c17: throw wire contract exact bytes/],
    ['throw-close-corrupted', /c18: throw wire contract exact bytes/],
    ['handler-error-microtask-dropped', /c17\/Error: handler error contract exact microtask order/],
    ['handler-error-type-lost', /c17\/TypeError: handler error contract exact microtask order/],
    ['handler-error-custom-name-lost', /c17\/subclass: handler error contract exact microtask order/],
    ['handler-error-number-stringified', /c17\/number-message: handler error contract exact microtask order/],
    ['handler-error-message-getter-twice', /c17\/message-getter: handler error contract exact microtask order/],
    ['handler-error-replacement-timer-called', /c17\/override-timer: handler error contract exact microtask order/],
    ['handler-error-listener-write-dropped', /c17\/error-send: handler error contract exact accepted wire bytes/],
    ['handler-error-conversion-skipped', /c17\/error-send-object: handler error contract exact microtask order/],
    ['handler-error-getter-close-ignored', /c17\/getter-close: handler error contract exact microtask order/],
    ['handler-object-conversion-called', /c17\/object: handler error contract exact microtask order/],
    ['handler-object-shared-reference-lost', /c17\/object-cycle: handler error contract exact microtask order/],
    ['handler-object-error-prototype', /c17\/object-fake-error: handler error contract exact microtask order/],
    ['handler-dom-private-name-lost', /c17\/dom-exception: handler error contract exact microtask order/],
    ['handler-aggregate-alias-lost', /c17\/aggregate-data: handler error contract exact microtask order/],
    ['handler-error-getter-order-lost', /c17\/error-ordered-getters: handler error contract exact microtask order/],
    ['handler-error-original-stack-retained', /c17\/error-fixed-stack: handler error contract exact microtask order/],
    ['handler-error-diagnostic-unlinked', /c17\/name-getter-throws: handler error contract exact microtask order/],
    ['send-surrogate-normalized', /DOMString wire message 4/],
    ['close-surrogate-corrupted', /close reason uses USVString with the captured intrinsic/],
    ['getter-reads', /public event contract: CloseEvent\/reads/],
    ['error-payload', /refused dial carries the reference Error object/],
    ['event-phase', /public event contract: Event\/defaults/],
    ['runtime-trust', /refused dial carries the reference Error object/],
    ['handled-reason', /runtime event delivery matches the pinned default workerd profile/],
    ['message-source', /runtime event delivery matches the pinned default workerd profile/],
    ['cached-message-ports', /public event contract: MessageEvent\/ports-undefined/],
    ['listener-exception-swallowed', /listener contract: EventTarget\/exception/],
    ['listener-removal-ignored', /listener contract: EventTarget\/remove-next-once/],
    ['listener-option-reread', /listener contract: EventTarget\/option-read-order/],
    ['handler-order', /event handler contract: WebSocket\/open\/replace/],
    ['handler-clear-removes-listener', /event handler contract: MessagePort\/message\/clear/],
].map(([name, diagnostic]) => Object.freeze({name, diagnostic:diagnostic?.source ?? null})));

// Every fault has an independently pinned execution scope. The complete
// baseline still observes all161 handler-error cases and every API assertion.
const handlerMutationCases = Object.freeze({
    'handler-error-microtask-dropped':'c17/Error',
    'handler-error-type-lost':'c17/TypeError',
    'handler-error-custom-name-lost':'c17/subclass',
    'handler-error-number-stringified':'c17/number-message',
    'handler-error-message-getter-twice':'c17/message-getter',
    'handler-error-replacement-timer-called':'c17/override-timer',
    'handler-error-listener-write-dropped':'c17/error-send',
    'handler-error-conversion-skipped':'c17/error-send-object',
    'handler-error-getter-close-ignored':'c17/getter-close',
    'handler-object-conversion-called':'c17/object',
    'handler-object-shared-reference-lost':'c17/object-cycle',
    'handler-object-error-prototype':'c17/object-fake-error',
    'handler-dom-private-name-lost':'c17/dom-exception',
    'handler-aggregate-alias-lost':'c17/aggregate-data',
    'handler-error-getter-order-lost':'c17/error-ordered-getters',
    'handler-error-original-stack-retained':'c17/error-fixed-stack',
    'handler-error-diagnostic-unlinked':'c17/name-getter-throws',
});
export const mutationHandlerCase = name => {
    assert.ok(requiredMutationCases.some(value=>value.name===name), 'known mutation execution scope');
    return name === 'baseline' ? undefined : handlerMutationCases[name] ?? null;
};

// Only isolated generated programs receive the scope transformation. The
// ordinary runner has no environment/CLI switch that can omit its inventory.
export function prepareMutationRunner(source, name = 'baseline') {
    // Windows Git checkouts may use CRLF. Normalize only the isolated
    // generated program; fixture/source identities still hash original bytes.
    source = source.replace(/\r\n/g, '\n');
    const selected = mutationHandlerCase(name);
    if (typeof selected === 'string') {
        const imported = 'import { observeHandlerErrors }';
        assert.equal(source.split(imported).length, 2, 'single handler observer import');
        source = source.replace(imported, 'import { observeHandlerErrors, observeHandlerErrorCase }');
        const entry = "'api-compat.mjs': async (port, {server}) => {";
        assert.equal(source.split(entry).length, 2, 'single API scenario entry');
        source = source.replace(entry, entry + '\n        return {handlerErrors:{' + JSON.stringify(selected)
            + ':await observeHandlerErrorCase(port,routePrefix,assert,' + JSON.stringify(selected) + ')}};');
    } else if (selected === null) {
        const handlers = 'observation.handlerErrors = await observeHandlerErrors(port, routePrefix, assert);\n        assertHandlerErrorDiagnostics(observation.handlerErrors, server.stderr(), assert);';
        assert.equal(source.split(handlers).length, 2, 'single complete handler block');
        source = source.replace(handlers, '/* Dedicated non-handler fault: complete handler inventory is verified by the baseline. */');
    }
    for (const [relative, absolute] of [
        ['./helpers/ws_host_backend.mjs', 'tests/helpers/ws_host_backend.mjs'],
        ['./helpers/ws_response_client.mjs', 'tests/helpers/ws_response_client.mjs'],
        ['./helpers/ws_handler_errors.mjs', 'tests/helpers/ws_handler_errors.mjs'],
        ['../tools/workerd-handler-error-contract.mjs', 'tools/workerd-handler-error-contract.mjs'],
        ['../tools/workerd-reference.mjs', 'tools/workerd-reference.mjs'],
    ]) {
        assert.ok(source.includes(`'${relative}'`), `mutation driver import: ${relative}`);
        source = source.replaceAll(`'${relative}'`, JSON.stringify(pathToFileURL(path.join(root, absolute)).href));
    }
    const manifest = "path.join(here, '..', 'js', 'profile-manifest.js')";
    assert.equal(source.split(manifest).length, 2, 'mutation profile path is unique');
    return source.replace(manifest, JSON.stringify(path.join(root, 'js/profile-manifest.js')));
}

export function validateMutationReport(report, expected, apiObservation) {
    assert.ok(report && typeof report === 'object', 'mutation report required');
    assert.equal(report.schema, 'capsid-workerd-mutations-v1');
    assert.equal(report.complete, true, 'complete mutation report');
    assert.equal(report.runId, expected.runId, 'mutation report belongs to this gate run');
    assert.equal(report.mode, 'single-worker');
    assert.deepEqual(report.lock, expected.lock, 'mutation reference profile');
    const client = {node:expected.lock.client.node_version, undici:expected.lock.client.undici_version};
    assert.deepEqual(report.client, client, 'mutation client');
    assert.deepEqual(report.fixtures, expected.fixtures, 'mutation fixture identities');
    const fixture = expected.fixtures.find(x => x.fixture === 'api-compat.mjs');
    assert.ok(fixture, 'API fixture required');
    for (const [key, value] of Object.entries({hostSHA256:expected.host, workerSHA256:expected.worker,
        sourceRunnerSHA256:expected.runner, runnerSHA256:expected.mutationRunner,
        controllerSHA256:expected.mutationController, contractSHA256:expected.mutationContract,
        handlerObserverSHA256:expected.handlerErrorObserver,
        fixtureSHA256:fixture.sha256})) {
        assert.match(value, /^[a-f0-9]{64}$/, `expected ${key}`);
        assert.equal(report[key], value, `mutation ${key}`);
    }
    assert.ok(Array.isArray(report.results), 'mutation results required');
    assert.deepEqual(report.results.map(x=>x.name), requiredMutationCases.map(x=>x.name),
        'full ordered mutation inventory, with baseline first');
    assert.deepEqual(Object.keys(expected.mutationRunners).sort(), requiredMutationCases.map(x=>x.name).sort(),
        'complete expected per-fault runner identities');
    const digests = new Set();
    for (let i=0; i<requiredMutationCases.length; i++) {
        const result = report.results[i], {name, diagnostic} = requiredMutationCases[i];
        assert.equal(result.handlerErrorCase, mutationHandlerCase(name) ?? null, `${name}: fixed handler execution scope`);
        assert.match(expected.mutationRunners[name], /^[a-f0-9]{64}$/, `${name}: expected runner digest`);
        assert.equal(result.runnerSHA256, expected.mutationRunners[name], `${name}: exact executed runner`);
        assert.equal(result.signal, null, `${name}: no forced termination`);
        assert.equal(result.error, null, `${name}: no spawn error or timeout`);
        assert.equal(result.exit, diagnostic === null ? 0 : 1, `${name}: expected exit`);
        assert.equal(result.detected, diagnostic !== null, `${name}: detection result`);
        assert.equal(result.expectedDiagnostic, diagnostic, `${name}: fixed assertion diagnostic`);
        assert.equal(typeof result.stdout, 'string', `${name}: stdout preserved`);
        assert.equal(typeof result.stderr, 'string', `${name}: stderr preserved`);
        assert.match(result.fixtureSHA256, /^[a-f0-9]{64}$/, `${name}: fixture digest`);
        assert.ok(!digests.has(result.fixtureSHA256), `${name}: distinct mutation source`);
        digests.add(result.fixtureSHA256);
        if (diagnostic === null) assert.equal(result.fixtureSHA256, fixture.sha256, 'unmodified baseline');
        else assert.match(result.stdout + result.stderr, new RegExp(diagnostic), `${name}: intended assertion`);
    }
    const baseline = report.results[0].driverReport;
    assert.ok(baseline && typeof baseline === 'object', 'baseline API report required');
    assert.equal(baseline.schema, 'capsid-ws-fixtures-v1');
    // This deliberately selects one of the fifteen fixtures. Preserve the
    // driver's honest partial-matrix marker; verify its exact API scope below.
    assert.equal(baseline.complete, false, 'API-only baseline is not the full fixture matrix');
    assert.equal(baseline.reference_parity, true, 'API baseline has no remaining field exception');
    assert.equal(baseline.mode, 'single-worker');
    assert.equal(baseline.runner_sha256, expected.mutationRunner);
    assert.equal(baseline.host_sha256, expected.host);
    assert.equal(baseline.worker_sha256, expected.worker);
    assert.deepEqual(baseline.lock, expected.lock);
    assert.deepEqual(baseline.client, client);
    assert.deepEqual(baseline.fixtures, expected.fixtures);
    assert.equal(baseline.results.length, 1, 'only the API baseline is selected');
    const api = baseline.results[0];
    assert.equal(api.fixture, 'api-compat.mjs');
    assert.equal(api.passed, true);
    assert.ok(!api.skipped, 'API baseline did not skip');
    assert.equal(api.assertions, 3009, 'reviewed complete API baseline assertion count');
    assert.deepEqual(api.shutdown, {code:0, signal:null, forced:false});
    // Preserve raw traces; compare their validated wire projection because
    // undici event delivery can race peer transport disposal.
    const baselineObservation = structuredClone(api.observation);
    const gateObservation = structuredClone(apiObservation);
    baselineObservation.handlerThrows = assertHandlerThrowObservation(baselineObservation.handlerThrows);
    gateObservation.handlerThrows = assertHandlerThrowObservation(gateObservation.handlerThrows);
    baselineObservation.handlerErrors = assertHandlerErrorObservations(baselineObservation.handlerErrors);
    gateObservation.handlerErrors = assertHandlerErrorObservations(gateObservation.handlerErrors);
    assert.deepEqual(baselineObservation, gateObservation, 'baseline reproduces the gate API observations');
    return {complete:true, runId:report.runId, mode:report.mode,
        cases:report.results.map(x=>x.name), baselineAssertions:api.assertions,
        hostSHA256:report.hostSHA256, workerSHA256:report.workerSHA256,
        controllerSHA256:report.controllerSHA256, contractSHA256:report.contractSHA256};
}
