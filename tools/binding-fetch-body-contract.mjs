import assert from 'node:assert/strict';
import {isDeepStrictEqual} from 'node:util';
import fs from 'node:fs';
import {createHash} from 'node:crypto';
import {bindingFetchKinds, bindingFetchViewGroups, bindingFetchPacket, expectedBindingFetchRows, bindingFetchStateTemplate} from './binding-fetch-body-input.mjs';

const equal = (actual, expected, message) => assert.ok(isDeepStrictEqual(actual, expected), message ?? 'complete current typed value');
const hash = value => createHash('sha256').update(value).digest('hex');
const fields = (value, names) => {
    assert.ok(value && typeof value === 'object' && !Array.isArray(value));
    equal(Object.keys(value).sort(), names.slice().sort());
};
function sourceModel(sources, kind, port, sequence, count) {
    assert.ok(Number.isSafeInteger(port) && port > 0 && port < 65536);
    assert.equal(sequence, 3*count); assert.equal(sources.length, count);
    const request = 'GET /body HTTP/1.1\r\nHost: 127.0.0.1:'+port;
    const packet = bindingFetchPacket(kind).toString('base64'), all = [];
    for (const source of sources) {
        fields(source, ['request', 'rawRequest', 'peerEnded', 'peerReset', 'fixtureEnded', 'fixtureDestroyed', 'events', 'sent']);
        assert.equal(source.request, request); assert.equal(source.rawRequest, Buffer.from(request+'\r\n\r\n').toString('base64')); assert.equal(source.sent, packet);
        assert.equal(source.peerEnded, true); assert.equal(source.peerReset, false); assert.equal(source.fixtureEnded, true); assert.equal(source.fixtureDestroyed, false);
        equal(source.events.map(event => event.name), ['request', 'server-complete', 'peer-ended']);
        let previous = -1;
        for (const event of source.events) {
            fields(event, ['name', 'sequence']); assert.ok(Number.isSafeInteger(event.sequence) && event.sequence > previous && event.sequence < sequence);
            previous = event.sequence; all.push(event.sequence);
        }
    }
    equal(all.sort((a, b) => a-b), Array.from({length: sequence}, (_, index) => index));
}
export function assertBindingFetchReferenceArchive(archive, fixtureSHA256) {
    fields(archive, ['schema', 'reports']); assert.equal(archive.schema, 'capsid-binding-fetch-body-reference-v1'); assert.equal(archive.reports.length, 2);
    for (const [index, item] of archive.reports.entries()) {
        fields(item, ['platform', 'originalTextUTF8', 'originalReportSHA256']); assert.equal(item.platform, index ? 'linux' : 'native');
        assert.equal(hash(Buffer.from(item.originalTextUTF8)), item.originalReportSHA256);
        const report = JSON.parse(item.originalTextUTF8); assert.equal(report.complete, true); assert.equal(report.fixtureSHA256, fixtureSHA256); assert.equal(report.cases.length, 8);
        const lock = report.referenceLock;
        assert.equal(lock.release_tag, 'v1.20261010.1'); assert.equal(lock.source_commit, '8f311ae3e3cf3064983f56a6a1c8b7ee33d6b99f');
        assert.equal(lock.compatibility_date, '2026-08-20'); equal(lock.compatibility_flags, []);
        assert.equal(report.binarySHA256, index ? '50f31156a9a4c7d549f1b4753dd61eb527e1b6b0cd416e5ba3d02d64dfdee5dd' : 'c8799724c4221af61dde76ed071aa5996b25c9bd5d247da94c166718cabe612a');
        assert.equal(lock.binary_sha256, report.binarySHA256); equal(report.client, {node: 'v24.14.0', undici: '7.21.0'}); assert.equal(report.stderr, '');
        assert.ok(report.shutdown.signal === 'SIGTERM' || (report.shutdown.code === 0 && report.shutdown.signal === null));
        for (const [i, row] of report.cases.entries()) {
            fields(row, ['kind', 'group', 'views', 'sourcePort', 'rows', 'sourcesBeforeReferenceStop', 'sampleSequence']);
            const kind = i < 4 ? 'raw' : 'gzip', group = i%4, target = 'http://127.0.0.1:'+row.sourcePort+'/body';
            assert.equal(row.kind, kind); assert.equal(row.group, group); equal(row.views, bindingFetchViewGroups[group]);
            const expected = expectedBindingFetchRows(kind, group, target); equal(row.rows, expected);
            sourceModel(row.sourcesBeforeReferenceStop, kind, row.sourcePort, row.sampleSequence, expected.length);
        }
    }
}
const fixture = fs.readFileSync(new URL('../tests/fixtures/workerd-binding-fetch-body.mjs', import.meta.url));
const binding = fs.readFileSync(new URL('../tests/fixtures/binding-fetch-body-package.mjs', import.meta.url));
const prefix = value => value.toString('utf8').split('export default')[0];
assert.equal(prefix(fixture), prefix(binding), 'byte-identical matrix in actual primary and Binding fixture');
assertBindingFetchReferenceArchive(JSON.parse(fs.readFileSync(new URL('../tests/fixtures/workerd-binding-fetch-body-report.json', import.meta.url), 'utf8')), hash(fixture));
export function assertBindingFetchCase(row, kind, group) {
    fields(row, ['group', 'views', 'port', 'state', 'sourcesBeforeFinish', 'sampleSequence', 'finalSources', 'child', 'workerExit', 'stdout', 'stderr']);
    assert.equal(row.group, group); equal(row.views, bindingFetchViewGroups[group]);
    assert.ok(Number.isSafeInteger(row.port) && row.port > 0 && row.port < 65536);
    const target = 'http://127.0.0.1:'+row.port+'/body', expected = bindingFetchStateTemplate(kind, group, target);
    equal(row.state, expected, 'every typed row, byte/backing, metadata, RPC prototype/context, node policy and recovery');
    sourceModel(row.sourcesBeforeFinish, kind, row.port, row.sampleSequence, 2*expected.batch.length+31);
    equal(row.finalSources, row.sourcesBeforeFinish, 'shutdown/teardown must not manufacture producer completion');
    equal(row.child, {code: 0, signal: null}); equal(row.workerExit, {reaped: true, channelClosed: true, code: 0}); assert.equal(row.stderr, '');
    const lines = row.stdout.trim().split(/\r?\n/);
    assert.equal(lines.length, 3); assert.match(lines[0], /^memoryLimit=0 actualFeatures=(\d+) expectedFeatures=\1$/);
    assert.equal(lines[1], 'BINDING_BODY '+JSON.stringify(row.state)); assert.equal(lines[2], 'BINDING_EXIT '+JSON.stringify(row.workerExit));
    return row.state;
}
export function validateBindingFetchReport(report, expected) {
    fields(report, ['schema', 'complete', 'runId', 'kind', 'inputs', 'binaries', 'cases']); assert.equal(report.schema, 'capsid-binding-fetch-body-v1');
    assert.equal(report.complete, true); assert.equal(report.runId, expected.runId); assert.ok(bindingFetchKinds.includes(report.kind)); assert.equal(report.kind, expected.kind);
    equal(report.inputs, expected.inputs); equal(report.binaries, expected.binaries); assert.equal(report.cases.length, 4);
    return report.cases.map((row, group) => assertBindingFetchCase(row, report.kind, group));
}
