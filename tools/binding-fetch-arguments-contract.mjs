import assert from 'node:assert/strict';
import {isDeepStrictEqual} from 'node:util';
import fs from 'node:fs';
import {createHash} from 'node:crypto';
import {gunzipSync} from 'node:zlib';
import {bindingArgumentRoutes, bindingArgumentViews, bindingArgumentMethods, bindingArgumentKinds,
    bindingArgumentPacket, bindingArgumentStateTemplate, expectedBindingArgumentEnvelope} from './binding-fetch-arguments-input.mjs';

const equal = (actual, expected, message) => assert.ok(isDeepStrictEqual(actual, expected), message ?? 'complete current typed value');
const hash = value => createHash('sha256').update(value).digest('hex');
const fields = (value, names) => {assert.ok(value && typeof value === 'object' && !Array.isArray(value)); equal(Object.keys(value).sort(), names.slice().sort());};
function sources(actual, bodyKind, port, sequence, count) {
    assert.ok(Number.isSafeInteger(port) && port > 0 && port < 65536); assert.equal(sequence, count*3); assert.equal(actual.length, count);
    const request = 'GET /body HTTP/1.1\r\nHost: 127.0.0.1:'+port, raw = Buffer.from(request+'\r\n\r\n').toString('base64'), packet = bindingArgumentPacket(bodyKind).toString('base64'), all = [];
    for (const item of actual) {
        fields(item, ['request', 'rawRequest', 'peerEnded', 'peerReset', 'fixtureEnded', 'fixtureDestroyed', 'events', 'sent']);
        assert.equal(item.request, request); assert.equal(item.rawRequest, raw); assert.equal(item.sent, packet);
        assert.equal(item.peerEnded, true); assert.equal(item.peerReset, false); assert.equal(item.fixtureEnded, true); assert.equal(item.fixtureDestroyed, false);
        equal(item.events.map(event => event.name), ['request', 'server-complete', 'peer-ended']); let previous = -1;
        for (const event of item.events) {fields(event, ['name', 'sequence']); assert.ok(Number.isSafeInteger(event.sequence) && event.sequence > previous && event.sequence < sequence); previous = event.sequence; all.push(event.sequence);}
    }
    equal(all.sort((a, b) => a-b), Array.from({length: sequence}, (_, index) => index));
}
export function assertBindingArgumentCase(item, bodyKind, route, view, method) {
    fields(item, ['bodyKind', 'route', 'view', 'method', 'kind', 'port', 'state', 'calls', 'wireTypes', 'sourcesBeforeStop', 'sampleSequence', 'finalSources', 'child', 'workerExit', 'stdout', 'stderr']);
    equal([item.bodyKind, item.route, item.view, item.method], [bodyKind, route, view, method]);
    const expected = expectedBindingArgumentEnvelope(bodyKind, route, view, method); assert.equal(item.kind, expected.state.kind);
    equal({state: item.state, calls: item.calls, wireTypes: item.wireTypes}, expected, 'every argument, conversion, error, retry/backing, EOF and RPC value');
    sources(item.sourcesBeforeStop, bodyKind, item.port, item.sampleSequence, expected.state.rows.length);
    equal(item.finalSources, item.sourcesBeforeStop); equal(item.child, {code: 0, signal: null}); equal(item.workerExit, {reaped: true, channelClosed: true, code: 0}); assert.equal(item.stderr, '');
    const lines = item.stdout.trim().split(/\r?\n/); assert.equal(lines.length, 3); assert.match(lines[0], /^memoryLimit=0 actualFeatures=(\d+) expectedFeatures=\1$/);
    assert.equal(lines[1], 'BINDING_BODY '+JSON.stringify({state: item.state, calls: item.calls, wireTypes: item.wireTypes})); assert.equal(lines[2], 'BINDING_EXIT '+JSON.stringify(item.workerExit));
    return item.state;
}
export function validateBindingArgumentReport(report, expected) {
    fields(report, ['schema', 'complete', 'runId', 'bodyKind', 'route', 'inputs', 'binaries', 'cases']); assert.equal(report.schema, 'capsid-binding-fetch-arguments-v1');
    assert.equal(report.complete, true); assert.equal(report.runId, expected.runId); assert.match(report.runId, /^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$/);
    assert.equal(report.bodyKind, expected.bodyKind); assert.equal(report.route, expected.route); assert.ok(bindingArgumentKinds.includes(report.bodyKind) && bindingArgumentRoutes.includes(report.route));
    equal(report.inputs, expected.inputs); equal(report.binaries, expected.binaries); assert.equal(report.cases.length, 26);
    return report.cases.map((item, index) => assertBindingArgumentCase(item, report.bodyKind, report.route, bindingArgumentViews[Math.floor(index/2)], bindingArgumentMethods[index%2]));
}
const fixtures = Object.fromEntries([
    ['upgrade58bef-binding-arguments-package.mjs', '../tests/fixtures/binding-fetch-arguments-package.mjs'],
    ['upgrade58bef-binding-arguments-user.mjs', '../tests/fixtures/binding-fetch-arguments-user.mjs'],
    ['upgrade58bef-binding-arguments-reference.mjs', '../tests/fixtures/workerd-binding-fetch-arguments.mjs'],
].map(([key, path]) => [key, fs.readFileSync(new URL(path, import.meta.url))]));
equal(fixtures['upgrade58bef-binding-arguments-package.mjs'].toString('utf8').split('export default')[0], fixtures['upgrade58bef-binding-arguments-reference.mjs'].toString('utf8').split('export default')[0], 'byte-identical actual primary and Binding argument function');
const locks = Object.fromEntries(['native', 'linux'].map(platform => [platform, JSON.parse(fs.readFileSync(new URL('../tests/fixtures/workerd-ws/'+(platform === 'native' ? 'reference-lock-windows.json' : 'reference-lock.json'), import.meta.url), 'utf8'))]));
export function assertBindingArgumentReferenceReport(report, platform, captureRunnerSHA256) {
    assert.ok(['native', 'linux'].includes(platform));
    fields(report, ['complete', 'schema', 'mode', 'configuration', 'sourceManifest', 'inputs', 'binaries', 'referenceLock', 'client', 'cases', 'finalCombinedTestsExecuted', 'shutdown', 'stderr']);
    assert.equal(report.complete, true); assert.equal(report.schema, 'capsid-binding-arguments-observation-v1'); assert.equal(report.mode, 'reference'); assert.equal(report.configuration, platform);
    assert.equal(report.sourceManifest, 'upgrade58bec-post-document-inputs.json'); assert.equal(report.finalCombinedTestsExecuted, false); equal(report.referenceLock, locks[platform]);
    equal(report.binaries, {reference: locks[platform].binary_sha256}); equal(report.client, {node: 'v24.14.0', undici: '7.21.0'}); assert.equal(report.stderr, '');
    fields(report.shutdown, ['code', 'signal']); assert.ok((report.shutdown.signal === 'SIGTERM' && report.shutdown.code === null) || (report.shutdown.code === 0 && report.shutdown.signal === null));
    const inputMap = Object.fromEntries(Object.entries(report.inputs).map(([path, value]) => [path.replaceAll('\\', '/').split('/').at(-1), value])); assert.equal(Object.keys(report.inputs).length, 4);
    equal(inputMap, {...Object.fromEntries(Object.entries(fixtures).map(([name, content]) => [name, hash(content)])), 'upgrade58beg-binding-arguments-runner.mjs': captureRunnerSHA256});
    assert.equal(report.cases.length, 260); let index = 0, rows = 0;
    for (const bodyKind of bindingArgumentKinds) for (const route of bindingArgumentRoutes) for (const view of bindingArgumentViews) for (const method of bindingArgumentMethods) {
        const item = report.cases[index++]; fields(item, ['bodyKind', 'route', 'view', 'method', 'kind', 'port', 'state', 'sourcesBeforeStop', 'sampleSequence', 'validated', 'stdout', 'stderr', 'finalSources']);
        equal([item.bodyKind, item.route, item.view, item.method], [bodyKind, route, view, method]); const state = bindingArgumentStateTemplate(bodyKind, route, view, method);
        assert.equal(item.kind, state.kind); equal(item.state, state); assert.equal(item.validated, true); assert.equal(item.stdout, ''); assert.equal(item.stderr, '');
        sources(item.sourcesBeforeStop, bodyKind, item.port, item.sampleSequence, state.rows.length); equal(item.finalSources, item.sourcesBeforeStop); rows += state.rows.length;
    }
    assert.equal(rows, 10920); return rows;
}
export function assertBindingArgumentReferenceArchive(archive, metadata) {
    fields(archive, ['schema', 'reports']); assert.equal(archive.schema, 'capsid-binding-fetch-arguments-reference-v1'); assert.equal(archive.reports.length, 2); assert.equal(metadata.reports.length, 2);
    for (const [index, item] of archive.reports.entries()) {
        fields(item, ['platform', 'originalTextUTF8', 'originalReportSHA256']); assert.equal(item.platform, index ? 'linux' : 'native');
        assert.equal(hash(Buffer.from(item.originalTextUTF8)), item.originalReportSHA256);
        equal(metadata.reports[index], {platform: item.platform, sourceReportSHA256: item.originalReportSHA256, rawBytes: Buffer.byteLength(item.originalTextUTF8)});
        assertBindingArgumentReferenceReport(JSON.parse(item.originalTextUTF8), item.platform, metadata.captureRunnerSHA256);
    }
}
export function loadBindingArgumentPrimaries() {
    const metadata = JSON.parse(fs.readFileSync(new URL('../tests/fixtures/workerd-binding-fetch-arguments-report.json', import.meta.url), 'utf8'));
    fields(metadata, ['schema', 'rawBytes', 'rawSHA256', 'gzipSHA256', 'captureRunnerSHA256', 'reports']); assert.equal(metadata.schema, 'capsid-binding-fetch-arguments-reference-metadata-v1');
    assert.ok(Number.isSafeInteger(metadata.rawBytes) && metadata.rawBytes > 0 && metadata.rawBytes <= 128*1024*1024); assert.match(metadata.captureRunnerSHA256, /^[a-f0-9]{64}$/);
    const compressed = fs.readFileSync(new URL('../tests/fixtures/workerd-binding-fetch-arguments-report.json.gz', import.meta.url)); assert.equal(hash(compressed), metadata.gzipSHA256);
    const raw = gunzipSync(compressed, {maxOutputLength: metadata.rawBytes}); assert.equal(raw.length, metadata.rawBytes); assert.equal(hash(raw), metadata.rawSHA256);
    const archive = JSON.parse(raw); assertBindingArgumentReferenceArchive(archive, metadata); return {archive, metadata};
}
// Re-read and validate the actual immutable captures on each process invocation.
// Validators never memoize acceptance of a mutable report.
loadBindingArgumentPrimaries();
