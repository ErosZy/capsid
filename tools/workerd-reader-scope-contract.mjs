import assert from 'node:assert/strict';
import {referenceBackendModes} from './workerd-platform.mjs';
import {exactFields as fields} from './workerd-native-fetch-contract.mjs';
import {assertFetchTransitionCertificates} from './workerd-fetch-transition-contract.mjs';
import {assertReaderLifecycleEnvelope} from './workerd-native-reader-lifecycle-contract.mjs';
import {readerScopeCases,expectedReaderScopeState,expectedReaderScopeActorState} from './workerd-reader-scope-state.mjs';
export {readerScopeCases,expectedReaderScopeState,expectedReaderScopeActorState};
export const readerScopeInputPaths=Object.freeze({
    runner:'tests/test_websocket_reader_scope.mjs',fixture:'tests/fixtures/workerd-reader-scope.mjs',
    upstream:'tests/helpers/ws_native_reader_lifecycle_peer.mjs',upstreamBase:'tests/helpers/ws_fetch_transition_peer.mjs',
    controlPeer:'tests/helpers/ws_response_client.mjs',hostBackend:'tests/helpers/ws_host_backend.mjs',
    contract:'tools/workerd-reader-scope-contract.mjs',stateModel:'tools/workerd-reader-scope-state.mjs',
    envelopeContract:'tools/workerd-native-reader-lifecycle-contract.mjs',envelopeStateModel:'tools/workerd-reader-lifecycle-state.mjs',
    certificateContract:'tools/workerd-fetch-transition-contract.mjs',fields:'tools/workerd-native-fetch-contract.mjs',
    platform:'tools/workerd-platform.mjs',reference:'tools/workerd-reference.mjs',
});
export function assertReaderScopeCase(value,mode){
    assert.ok(readerScopeCases.includes(value.kind));
    return assertReaderLifecycleEnvelope(value,mode,expectedReaderScopeState(value.kind,value.challenge),{
        messages:['started','primed','done'],actorState:expectedReaderScopeActorState(value.kind,value.challenge)});
}
export function validateReaderScopeReport(report,expected){
    fields(report,['schema','complete','mode','runId','referenceLock','client','inputs','binaries','certificateInputs','cases'],'reader scope report');
    assert.equal(report.schema,'capsid-reader-scope-v1');assert.equal(report.complete,true);
    assert.equal(report.runId,expected.runId);assert.match(report.runId,/^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$/);
    assert.ok(['reference',...referenceBackendModes(expected.lock)].includes(report.mode));
    assert.deepEqual(report.referenceLock,expected.lock);assert.deepEqual(report.inputs,expected.inputs);
    assert.deepEqual(Object.keys(report.inputs).sort(),Object.keys(readerScopeInputPaths).sort());
    for(const hash of Object.values(report.inputs))assert.match(hash,/^[a-f0-9]{64}$/);
    assert.deepEqual(report.client,{node:expected.lock.client.node_version,undici:expected.lock.client.undici_version});
    assert.deepEqual(report.binaries,{host:report.mode==='reference'?expected.lock.binary_sha256:expected.host,
        worker:report.mode==='reference'?null:expected.worker});
    assertFetchTransitionCertificates(report.certificateInputs);
    assert.deepEqual(report.cases.map(value=>value.kind),readerScopeCases);
    for(const values of [report.cases.map(value=>value.challenge),report.cases.map(value=>JSON.parse(value.initial.text).realm),
        report.cases.map(value=>value.headers.requestKey)])assert.equal(new Set(values).size,readerScopeCases.length);
    return report.cases.map(value=>assertReaderScopeCase(value,report.mode));
}
export function compareReaderScopeReports(reference,candidates,expected){
    assert.equal(reference.mode,'reference');assert.deepEqual(candidates.map(value=>value.mode).sort(),referenceBackendModes(expected.lock).sort());
    const baseline=validateReaderScopeReport(reference,expected);
    for(const candidate of candidates)assert.deepEqual(validateReaderScopeReport(candidate,expected),baseline);
    return {complete:true,casesPerBackend:readerScopeCases.length,backends:candidates.length};
}
