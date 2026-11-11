import assert from 'node:assert/strict';
import {createHash} from 'node:crypto';
import {referenceBackendModes} from './workerd-platform.mjs';
import {assertFetchTransitionCertificates} from './workerd-fetch-transition-contract.mjs';
import {assertNativeFetchEnvelope,exactFields} from './workerd-native-fetch-contract.mjs';

export const nativeReaderCases=Object.freeze([
    ...[0,1,6,4095,4096,4097,8192,16384,65536,65537,1048613].map(length=>`chunks-default-${length}-0`),
    'chunks-byob-1048613-131075','chunks-byob-1048613-1048613','chunks-least-1048613-131075']);
export const nativeReaderInputPaths=Object.freeze({runner:'tests/test_websocket_fetch_readers.mjs',
    fixture:'tests/fixtures/workerd-fetch-reader-shapes.mjs',upstream:'tests/helpers/ws_fetch_reader_peer.mjs',
    upstreamBase:'tests/helpers/ws_fetch_transition_peer.mjs',controlPeer:'tests/helpers/ws_response_client.mjs',
    hostBackend:'tests/helpers/ws_host_backend.mjs',contract:'tools/workerd-fetch-reader-contract.mjs',
    nativeContract:'tools/workerd-native-fetch-contract.mjs',certificateContract:'tools/workerd-fetch-transition-contract.mjs'});
const sha=bytes=>createHash('sha256').update(bytes).digest('hex');
const bodies=new Map(),models=new Map();
export function nativeReaderBody(length){
    if(!bodies.has(length)){const bytes=Buffer.alloc(length);for(let index=0;index<length;index++)bytes[index]=(index*29+17)%251;bodies.set(length,bytes);}
    return bodies.get(length);
}
function measured(done,offset,count,backing,content,byob){
    return {done,type:'Uint8Array',byteOffset:offset,byteLength:count,bufferBytes:backing.length,
        head:Array.from(content.subarray(0,16)),tail:Array.from(content.subarray(-16)),sha256:sha(content),backingSHA256:sha(backing),
        inputBytesAfterCall:byob?0:null,closedAfterRead:done?{fulfilled:true}:null};
}
export function expectedNativeReaderState(kind,challenge){
    assert.ok(nativeReaderCases.includes(kind));
    if(!models.has(kind)){
        const [,method,countText,minimumText]=kind.split('-'),length=Number(countText),minimum=Number(minimumText),byob=method!=='default';
        const body=nativeReaderBody(length),reads=[];
        // The controlled source sends one HTTP body write, split into16 KiB
        // TLS records. Native DefaultReader buffers are4 KiB; BYOB commits on
        // the first complete source fragment meeting its requested minimum.
        const quantum=byob?Math.ceil(minimum/16384)*16384:4096;
        for(let at=0;at<length;at+=quantum){
            const content=body.subarray(at,Math.min(length,at+quantum)),offset=byob?16:0;
            const backing=Buffer.alloc(byob?length+64:4096,byob?165:0);content.copy(backing,offset);
            reads.push(measured(false,offset,content.length,backing,content,byob));
        }
        reads.push(byob?measured(true,0,0,Buffer.alloc(length+64,165),Buffer.alloc(0),true):
            {done:true,type:null,byteOffset:null,byteLength:null,bufferBytes:null,head:null,tail:null,sha256:null,backingSHA256:null,
                inputBytesAfterCall:null,closedAfterRead:{fulfilled:true}});
        models.set(kind,{kind,unexpected:null,reads,total:length,invalidBytes:0,closed:{fulfilled:true},
            response:{status:200,contentLength:String(length)},sha256:sha(body),bodyUsed:true,lockedAfterRelease:false});
    }
    return {...models.get(kind),challenge};
}
export function assertNativeReaderCase(value,mode){
    assert.ok(nativeReaderCases.includes(value.kind));
    const state=JSON.parse(value.observed.text).state;
    assert.deepEqual(state,expectedNativeReaderState(value.kind,value.challenge),'exact source fragments, backing bytes, hashes, EOF and closed reaction');
    const length=Number(value.kind.split('-')[2]);
    assertNativeFetchEnvelope(value,{mode,requests:1,body:nativeReaderBody(length)});return {kind:value.kind};
}
export function validateNativeReaderReport(report,expected){
    exactFields(report,['schema','complete','mode','runId','referenceLock','client','inputs','binaries','certificateInputs','cases'],'reader report');
    assert.equal(report.schema,'capsid-fetch-readers-v1');assert.equal(report.complete,true);assert.equal(report.runId,expected.runId);
    assert.match(report.runId,/^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$/);assert.ok(['reference',...referenceBackendModes(expected.lock)].includes(report.mode));
    assert.deepEqual(report.referenceLock,expected.lock);assert.deepEqual(report.inputs,expected.inputs);
    assert.deepEqual(Object.keys(report.inputs).sort(),Object.keys(nativeReaderInputPaths).sort());
    for(const hash of Object.values(report.inputs))assert.match(hash,/^[a-f0-9]{64}$/);
    assert.deepEqual(report.client,{node:expected.lock.client.node_version,undici:expected.lock.client.undici_version});
    assert.deepEqual(report.binaries,{host:report.mode==='reference'?expected.lock.binary_sha256:expected.host,worker:report.mode==='reference'?null:expected.worker});
    assertFetchTransitionCertificates(report.certificateInputs);assert.deepEqual(report.cases.map(value=>value.kind),nativeReaderCases);
    for(const name of ['challenge','realm']){
        const ids=report.cases.map(value=>name==='challenge'?value.challenge:JSON.parse(value.initial.text).realm);
        assert.equal(new Set(ids).size,14);
    }
    return report.cases.map(value=>assertNativeReaderCase(value,report.mode));
}
export function compareNativeReaderReports(reference,candidates,expected){
    assert.equal(reference.mode,'reference');assert.deepEqual(candidates.map(value=>value.mode).sort(),referenceBackendModes(expected.lock).sort());
    const baseline=validateNativeReaderReport(reference,expected);
    for(const report of candidates)assert.deepEqual(validateNativeReaderReport(report,expected),baseline);
    return {complete:true,casesPerBackend:14,backends:candidates.length};
}
