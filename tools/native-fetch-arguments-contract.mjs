import assert from 'node:assert/strict';
import {createHash} from 'node:crypto';
import {bindingArgumentRoutes, bindingArgumentViews, bindingArgumentMethods, bindingArgumentKinds,
    bindingArgumentPacket, bindingArgumentStateTemplate} from './binding-fetch-arguments-input.mjs';
import {loadBindingArgumentPrimaries} from './binding-fetch-arguments-contract.mjs';

export const nativeFetchArgumentRoutes = bindingArgumentRoutes;
export const nativeFetchArgumentViews = bindingArgumentViews;
export const nativeFetchArgumentMethods = bindingArgumentMethods;
export const nativeFetchArgumentKinds = bindingArgumentKinds;
export const nativeFetchArgumentPacket = bindingArgumentPacket;
export const nativeFetchArgumentInputPaths = Object.freeze({
    runner: 'tests/test_native_fetch_arguments.mjs', controls: 'tests/test_native_fetch_arguments_contract.mjs',
    contract: 'tools/native-fetch-arguments-contract.mjs', model: 'tools/binding-fetch-arguments-input.mjs',
    modelContract: 'tools/workerd-fetch-arguments-contract.mjs', primaryContract: 'tools/binding-fetch-arguments-contract.mjs',
    fixture: 'tests/fixtures/workerd-binding-fetch-arguments.mjs',
    primaryArchive: 'tests/fixtures/workerd-binding-fetch-arguments-report.json.gz',
    primaryMetadata: 'tests/fixtures/workerd-binding-fetch-arguments-report.json',
    bindingFixture: 'tests/fixtures/binding-fetch-arguments-package.mjs', bindingUser: 'tests/fixtures/binding-fetch-arguments-user.mjs',
    workerHelper: 'tests/test_native_fetch_response_limits.cc', exitObserver: 'tests/graceful_worker_exit.h',
    cleanupObserver: 'src/worker_exit_status.h',
    windowsLock: 'tests/fixtures/workerd-ws/reference-lock-windows.json', linuxLock: 'tests/fixtures/workerd-ws/reference-lock.json',
});
const sha = value => createHash('sha256').update(value).digest('hex');
const fields = (value, expected) => assert.deepEqual(Object.keys(value).sort(), [...expected].sort());
// This immutable primary input is validated from the actual archive at process
// startup. Every mutable product report is checked afresh against both sources.
const {archive, metadata} = loadBindingArgumentPrimaries();
const primaries = new Map(archive.reports.map(item => [item.platform, {
    sha256: item.originalReportSHA256, report: JSON.parse(item.originalTextUTF8),
}]));
for (const primary of primaries.values()) {
    for (const item of primary.report.cases) {
        assert.deepEqual(item.state, bindingArgumentStateTemplate(item.bodyKind, item.route, item.view, item.method));
    }
}
export function nativeFetchArgumentPrimary(platform, kind, route, view, method) {
    assert.ok(primaries.has(platform));
    const primary = primaries.get(platform);
    const found = primary.report.cases.find(item => item.bodyKind === kind && item.route === route && item.view === view && item.method === method);
    assert.ok(found, 'complete immutable primary selection');
    return {archiveSHA256: metadata.gzipSHA256, reportSHA256: primary.sha256,
        state: structuredClone(found.state)};
}
export function expectedNativeFetchArgumentState(kind, route, view, method) {
    return bindingArgumentStateTemplate(kind, route, view, method);
}
export function assertNativeFetchArgumentSources(records, count, port) {
    assert.equal(records.length, count);
    const sequences = [];
    for (const row of records) {
        fields(row, ['rawRequest', 'localAddress', 'remoteAddress', 'peerEnded', 'peerReset', 'fixtureEnded', 'fixtureDestroyed', 'events']);
        assert.ok(['127.0.0.1', '::1', '::ffff:127.0.0.1'].includes(row.localAddress));
        assert.ok(['127.0.0.1', '::1', '::ffff:127.0.0.1'].includes(row.remoteAddress));
        assert.equal(row.peerEnded, true); assert.equal(row.peerReset, false);
        assert.equal(row.fixtureEnded, true); assert.equal(row.fixtureDestroyed, false);
        const text = Buffer.from(row.rawRequest, 'base64').toString();
        assert.equal(Buffer.from(text).toString('base64'), row.rawRequest);
        assert.match(text, new RegExp('^GET /body HTTP/1\\.1\\r\\nHost: localhost:' + port + '\\r\\n', 'i'));
        assert.doesNotMatch(text, /\r\n(?:Upgrade|Sec-WebSocket[^:]*):/i);
        assert.deepEqual(row.events.map(event => event.name), ['request', 'source-complete', 'peer-ended']);
        for (const event of row.events) {
            fields(event, ['name', 'sequence']);
            assert.ok(Number.isSafeInteger(event.sequence) && event.sequence >= 0);
            sequences.push(event.sequence);
        }
        assert.ok(row.events[0].sequence < row.events[1].sequence && row.events[1].sequence < row.events[2].sequence);
    }
    assert.equal(new Set(sequences).size, sequences.length, 'actual source event identities');
}
export function assertNativeFetchArgumentCase(item, platform, kind, route, view, method) {
    fields(item, ['bodyKind', 'route', 'view', 'method', 'port', 'state', 'primary', 'packetSHA256',
        'sourcesBeforeStop', 'finalSources', 'child', 'workerExit', 'stdout', 'stderr']);
    assert.deepEqual([item.bodyKind, item.route, item.view, item.method], [kind, route, view, method]);
    assert.ok(nativeFetchArgumentKinds.includes(kind) && nativeFetchArgumentRoutes.includes(route));
    assert.ok(nativeFetchArgumentViews.includes(view) && nativeFetchArgumentMethods.includes(method));
    assert.ok(Number.isInteger(item.port) && item.port > 0 && item.port <= 65535);
    const expected = expectedNativeFetchArgumentState(kind, route, view, method);
    const primary = nativeFetchArgumentPrimary(platform, kind, route, view, method);
    assert.deepEqual(item.primary, {archiveSHA256: primary.archiveSHA256, reportSHA256: primary.reportSHA256});
    assert.deepEqual(item.state, expected, 'ordinary native view/coercion/detachment/retry/EOF state');
    assert.deepEqual(item.state, primary.state, 'ordinary native state and immutable actual default workerd capture');
    assert.equal(item.packetSHA256, sha(nativeFetchArgumentPacket(kind)));
    assertNativeFetchArgumentSources(item.sourcesBeforeStop, expected.rows.length, item.port);
    assert.deepEqual(item.finalSources, item.sourcesBeforeStop, 'host shutdown cannot supply source completion');
    assert.deepEqual(item.child, {code: 0, signal: null});
    assert.deepEqual(item.workerExit, {clean: true, processObserved: true, exitStatus: 0, forced: false, errorEvents: 0});
    assert.equal(typeof item.stdout, 'string'); assert.equal(typeof item.stderr, 'string');
    assert.match(item.stdout, /^LIMIT_PROBE /m); assert.match(item.stdout, /^LIMIT_EXIT /m);
    assert.doesNotMatch(item.stderr, /AddressSanitizer|runtime error:|FAIL:/);
    return expected.rows.length;
}
export function validateNativeFetchArgumentReport(report, expected) {
    fields(report, ['schema', 'complete', 'runId', 'bodyKind', 'route', 'referencePlatform', 'inputs', 'binaries', 'cases']);
    assert.equal(report.schema, 'capsid-native-fetch-arguments-v1'); assert.equal(report.complete, true);
    assert.equal(report.runId, expected.runId); assert.equal(report.bodyKind, expected.bodyKind); assert.equal(report.route, expected.route);
    assert.equal(report.referencePlatform, expected.referencePlatform);
    assert.deepEqual(report.inputs, expected.inputs); assert.deepEqual(report.binaries, expected.binaries);
    fields(report.inputs, Object.keys(nativeFetchArgumentInputPaths));
    for (const value of [...Object.values(report.inputs), ...Object.values(report.binaries)]) assert.match(value, /^[a-f0-9]{64}$/);
    assert.equal(report.cases.length, 26);
    let index = 0, observations = 0;
    for (const view of nativeFetchArgumentViews) for (const method of nativeFetchArgumentMethods) {
        observations += assertNativeFetchArgumentCase(report.cases[index++], report.referencePlatform,
            report.bodyKind, report.route, view, method);
    }
    assert.equal(observations, 1092);
    return {complete: true, groups: 26, nestedObservations: observations};
}
