import assert from 'node:assert/strict';
import {referenceBackendModes} from './workerd-platform.mjs';
import {exactFields as fields} from './workerd-native-fetch-contract.mjs';
import {assertFetchTransitionCertificates} from './workerd-fetch-transition-contract.mjs';
import {assertReaderLifecycleEnvelope} from './workerd-native-reader-lifecycle-contract.mjs';
import {standardReaderCases,expectedStandardReaderState} from './workerd-standard-reader-state.mjs';
export {standardReaderCases,expectedStandardReaderState};
export const standardReaderShardNames=Object.freeze(['shapes','ownership','transfers']);
export function standardReaderShardCases(shard) {
    assert.ok(standardReaderShardNames.includes(shard));
    return standardReaderCases.slice(shard==='shapes'?0:shard==='ownership'?80:130,
        shard==='shapes'?80:shard==='ownership'?130:230);
}
export const standardReaderInputPaths=Object.freeze({
    runner:'tests/test_websocket_standard_readers.mjs',fixture:'tests/fixtures/workerd-standard-readers.mjs',
    upstream:'tests/helpers/ws_native_reader_lifecycle_peer.mjs',upstreamBase:'tests/helpers/ws_fetch_transition_peer.mjs',
    controlPeer:'tests/helpers/ws_response_client.mjs',hostBackend:'tests/helpers/ws_host_backend.mjs',
    contract:'tools/workerd-standard-reader-contract.mjs',stateModel:'tools/workerd-standard-reader-state.mjs',
    envelopeContract:'tools/workerd-native-reader-lifecycle-contract.mjs',envelopeStateModel:'tools/workerd-reader-lifecycle-state.mjs',
    certificateContract:'tools/workerd-fetch-transition-contract.mjs',fields:'tools/workerd-native-fetch-contract.mjs',
    platform:'tools/workerd-platform.mjs',reference:'tools/workerd-reference.mjs',
    listenerControl:'tests/helpers/workerd_listener_control.mjs',listenerContract:'tools/workerd-fetch-byob-contract.mjs',
});
export function assertStandardReaderCase(value,mode) {
    assert.ok(standardReaderCases.includes(value.kind));
    return assertReaderLifecycleEnvelope(value,mode,expectedStandardReaderState(value.kind,value.challenge));
}
export function validateStandardReaderReport(report,expected) {
    const shard=expected.shard;
    fields(report,['schema','complete','mode','runId','referenceLock','client','inputs','binaries','certificateInputs','cases',
        ...(shard===undefined?[]:['shard'])],'standard reader report');
    if(shard!==undefined){assert.ok(standardReaderShardNames.includes(shard));assert.equal(report.shard,shard);}
    assert.equal(report.schema,'capsid-standard-readers-v1');assert.equal(report.complete,true);
    assert.equal(report.runId,expected.runId);assert.match(report.runId,/^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$/);
    assert.ok(['reference',...referenceBackendModes(expected.lock)].includes(report.mode));
    assert.deepEqual(report.referenceLock,expected.lock);assert.deepEqual(report.inputs,expected.inputs);
    assert.deepEqual(Object.keys(report.inputs).sort(),Object.keys(standardReaderInputPaths).sort());
    for(const hash of Object.values(report.inputs))assert.match(hash,/^[a-f0-9]{64}$/);
    assert.deepEqual(report.client,{node:expected.lock.client.node_version,undici:expected.lock.client.undici_version});
    assert.deepEqual(report.binaries,{host:report.mode==='reference'?expected.lock.binary_sha256:expected.host,
        worker:report.mode==='reference'?null:expected.worker});
    assertFetchTransitionCertificates(report.certificateInputs);
    const cases=shard===undefined?standardReaderCases:standardReaderShardCases(shard);
    assert.deepEqual(report.cases.map(value=>value.kind),cases);
    assert.equal(new Set(report.cases.map(value=>value.challenge)).size,cases.length);
    assert.equal(new Set(report.cases.map(value=>JSON.parse(value.initial.text).realm)).size,cases.length);
    return report.cases.map(value=>assertStandardReaderCase(value,report.mode));
}
export function compareStandardReaderReports(reference,candidates,expected) {
    assert.equal(reference.mode,'reference');assert.deepEqual(candidates.map(value=>value.mode).sort(),referenceBackendModes(expected.lock).sort());
    const baseline=validateStandardReaderReport(reference,expected);
    for(const candidate of candidates)assert.deepEqual(validateStandardReaderReport(candidate,expected),baseline);
    return {complete:true,casesPerBackend:standardReaderCases.length,backends:candidates.length};
}
export function compareStandardReaderShardReports(references,candidates,expected) {
    const modes=['reference',...referenceBackendModes(expected.lock)];
    assert.deepEqual(references.map(report=>report.shard),standardReaderShardNames);
    assert.ok(references.every(report=>report.mode==='reference'));
    assert.deepEqual(candidates.map(report=>report.mode+'|'+report.shard).sort(),
        referenceBackendModes(expected.lock).flatMap(mode=>standardReaderShardNames.map(shard=>mode+'|'+shard)).sort());
    const all=[...references,...candidates],states=new Map();
    for(const mode of modes) {
        const reports=standardReaderShardNames.map(shard=>{
            const matches=all.filter(report=>report.mode===mode&&report.shard===shard);assert.equal(matches.length,1);
            return matches[0];
        });
        const cases=reports.flatMap(report=>report.cases);assert.deepEqual(cases.map(value=>value.kind),standardReaderCases);
        for(const values of [cases.map(value=>value.challenge),cases.map(value=>JSON.parse(value.initial.text).realm),
            cases.map(value=>value.headers.requestKey)])assert.equal(new Set(values).size,standardReaderCases.length,'independent identities across shards');
        states.set(mode,reports.flatMap(report=>validateStandardReaderReport(report,{...expected,shard:report.shard})));
    }
    for(const mode of modes.slice(1))assert.deepEqual(states.get(mode),states.get('reference'));
    return {complete:true,casesPerBackend:standardReaderCases.length,shardsPerBackend:3,backends:modes.length-1};
}
