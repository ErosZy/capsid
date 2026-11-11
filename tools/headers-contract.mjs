import assert from 'node:assert/strict';
import fs from 'node:fs';
import {createHash} from 'node:crypto';
import {fileURLToPath} from 'node:url';
import {isDeepStrictEqual} from 'node:util';
const root=fileURLToPath(new URL('../',import.meta.url));
const equal=(a,b,message)=>assert.ok(isDeepStrictEqual(a,b),message??'complete current typed value');
const fields=(value,names)=>{assert.ok(value&&typeof value==='object'&&!Array.isArray(value));equal(Object.keys(value).sort(),names.slice().sort());};
export const headersDigest=value=>createHash('sha256').update(value).digest('hex');
export const headersInputPaths=Object.freeze({
    fixture:'tests/fixtures/workerd-headers.mjs',binding:'tests/fixtures/binding-headers-package.mjs',user:'tests/fixtures/binding-headers-user.mjs',
    samples:'tests/fixtures/workerd-headers-report.json',expectedModel:'tests/fixtures/headers-expected-input-model.json',modelSource:'tools/headers-input-model.py',
    contract:'tools/headers-contract.mjs',runner:'tests/test_headers_compatibility.mjs',controls:'tests/test_headers_contract.mjs',
    helperSource:'tests/test_binding_fetch_body.cc',exitObserver:'tests/graceful_worker_exit.h',runtime:'js/fetch.js',
    rpcPolicy:'src/binding_rpc.h',rpcBackend:'src/binding_rpc.cc',lockWindows:'tests/fixtures/workerd-ws/reference-lock-windows.json',lockLinux:'tests/fixtures/workerd-ws/reference-lock.json',
});
const freeze=value=>{if(value&&typeof value==='object'){for(const child of Object.values(value))freeze(child);Object.freeze(value);}return value;};
const rawModel=fs.readFileSync(root+headersInputPaths.expectedModel);
assert.ok(rawModel.byteLength<=128*1024);assert.equal(headersDigest(rawModel),'b07ce8f6548153f3f119a8bdaeefcd6a576d262157a2cffb2c1e91c0229369dc');
const model=freeze(JSON.parse(rawModel));fields(model,['schema','functionSHA256','caseCount','cases']);
assert.equal(model.schema,'capsid-headers-expected-inputs-v1');assert.equal(model.functionSHA256,'32ac93d22c97832d1b6c592d3e85f45af3c288b9c12aa6944b4368eb7898c87c');assert.equal(model.caseCount,99);assert.equal(model.cases.length,99);assert.equal(new Set(model.cases.map(row=>row.id)).size,99);
for(const key of ['fixture','binding','user']){
    const source=fs.readFileSync(root+headersInputPaths[key],'utf8'),start=source.indexOf('const headersCases=only=>{'),end=source.indexOf(key==='user'?'\nconst types=':'\nexport default',start);
    assert.ok(start>=0&&end>start);assert.equal(headersDigest(source.slice(start,end+1)),model.functionSHA256,'identical exercised99-case function in actual primary/User/Binding');
}
export const headersExpectedRows=()=>structuredClone(model.cases);
export function headersWireTypes(value){
    const result={objects:0,arrays:0,objectNullPrototypes:true,arrayPrototypes:true};
    const visit=item=>{if(!item||typeof item!=='object')return;if(Array.isArray(item))result.arrays++;else result.objects++;for(const child of Object.values(item))visit(child);};visit(value);return result;
}
export function assertHeadersRows(rows){
    assert.ok(Array.isArray(rows));assert.equal(rows.length,99);
    for(let index=0;index<99;index++)equal(rows[index],model.cases[index],model.cases[index].id+': every current value, conversion trace, error/sentinel and iterator property');
}
const sampleBytes=fs.readFileSync(root+headersInputPaths.samples);assert.ok(sampleBytes.byteLength<=1024*1024);assert.equal(headersDigest(sampleBytes),'c9b10ad722a5c29a91cc6dfa0854fb62f11899ee6e24fa4ccd141b76bfadbe3e');
const samples=freeze(JSON.parse(sampleBytes));
const baselineInputs={"upgrade58bgp-headers-common.mjs":"32ac93d22c97832d1b6c592d3e85f45af3c288b9c12aa6944b4368eb7898c87c","upgrade58bgp-headers-package.mjs":"94d7e68fe78698716b375d1ac42304d276b529b8bbc15f84e1eec4212875d559","upgrade58bgp-headers-user.mjs":"9803acfa534892a89b1783b31271d0c410651661e4c1bdc236cb40bf9786f73a","upgrade58bgp-headers-reference.mjs":"2ffb1a94ddda185cd1751989d20f5faf2bac8af3e36918be91e03a5e51f17ad8","upgrade58bgq-capture-headers.mjs":"317a303789453cb29ea3390abff46bb06bffd9cb7e82037f84691023e13b0588"};
const primaryFiles=[{"file":"upgrade58bgq-headers-native-reference.json","sha256":"d07301d8eadaf68193c6fc9a597a111767002a490de08c7634ae2f5e691c4cb0"},{"file":"upgrade58bgq-headers-linux-reference.json","sha256":"222f484e7f83d9175b48257b7f36c2820e0efac820c92c6cf001a8ef6c7a6400"}];
export const headersPrimarySamples=()=>structuredClone(samples);
export function validateHeadersPrimaries(archive){
    fields(archive,['schema','functionSHA256','captures']);assert.equal(archive.schema,'capsid-headers-primaries-v1');assert.equal(archive.functionSHA256,model.functionSHA256);assert.equal(archive.captures.length,2);
    for(let index=0;index<2;index++){
        const capture=archive.captures[index],platform=index===0?'win32':'linux';fields(capture,['platform','file','sha256','rawUtf8']);assert.equal(capture.platform,platform);assert.equal(capture.file,primaryFiles[index].file);assert.equal(capture.sha256,primaryFiles[index].sha256);assert.equal(typeof capture.rawUtf8,'string');assert.ok(Buffer.byteLength(capture.rawUtf8)<=512*1024);assert.equal(headersDigest(capture.rawUtf8),capture.sha256);
        const report=JSON.parse(capture.rawUtf8),lock=JSON.parse(fs.readFileSync(root+headersInputPaths[index===0?'lockWindows':'lockLinux'],'utf8'));
        assert.equal(lock.release_tag,'v1.20261010.1');assert.equal(lock.source_commit,'8f311ae3e3cf3064983f56a6a1c8b7ee33d6b99f');assert.equal(lock.compatibility_date,'2026-08-20');equal(lock.compatibility_flags,[]);
        assert.equal(report.complete,true);assert.equal(report.mode,'reference');equal(report.referenceLock,lock);equal(report.inputs,baselineInputs);equal(report.binaries,{binary:lock.binary_sha256,worker:null});
        equal(report.client,{node:'v24.14.0',undici:'7.21.0'});assert.equal(report.status,200);assert.equal(report.shutdownForced,false);assert.equal(report.rootIntegrated,false);assert.equal(report.finalCombinedTestsExecuted,false);
        equal(JSON.parse(report.response),report.observed);fields(report.observed,['rows']);assertHeadersRows(report.observed.rows);
    }
}
validateHeadersPrimaries(samples);
export function assertHeadersObservation(observation){
    fields(observation,['user','binding','types','rpcCalls']);assertHeadersRows(observation.user);assertHeadersRows(observation.binding);equal(observation.types,headersWireTypes(model.cases));assert.equal(observation.rpcCalls,99);
}
export function validateHeadersReport(report,expected){
    fields(report,['schema','complete','runId','inputs','binaries','observation','stdout','stderr','child','workerExit']);assert.equal(report.schema,'capsid-headers-compat-v1');assert.equal(report.complete,true);
    assert.equal(report.runId,expected.runId);assert.match(report.runId,/^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$/);equal(report.inputs,expected.inputs);equal(report.binaries,expected.binaries);assertHeadersObservation(report.observation);
    equal(report.child,{code:0,signal:null});equal(report.workerExit,{reaped:true,channelClosed:true,code:0});assert.equal(report.stderr,'');
    const lines=report.stdout.trim().split(/\r?\n/);assert.equal(lines.length,3);assert.match(lines[0],/^memoryLimit=0 actualFeatures=(\d+) expectedFeatures=\1$/);
    assert.equal(lines[1],'BINDING_BODY '+JSON.stringify(report.observation));assert.equal(lines[2],'BINDING_EXIT '+JSON.stringify(report.workerExit));
}
