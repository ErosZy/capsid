import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import {execFileSync} from 'node:child_process';
import {createHash} from 'node:crypto';
import {parseArgs} from 'node:util';
import {fileURLToPath} from 'node:url';
import {root,readLock,fileDigest,fixtureIdentities} from './workerd-reference.mjs';
import {referencePlatform,referenceArchitecture,referenceBackendModes} from './workerd-platform.mjs';
import {validateGateRun} from './run-workerd-ws-gate.mjs';
import {checkWitness,readFullStandardEvidence,assertAuditOutput} from './workerd-wpt-profile.mjs';
import {messagePortInputPaths,assertMessagePortObservation} from './message-port-order-contract.mjs';
import {guardedRejectionTests} from '../tests/wpt/guard-rejection-events.mjs';

export const profileV2File='tests/wpt/workerd-default-profile-v2.json';
export const profileV2Paths=Object.freeze([
    'html/webappapis/scripting/processing-model-2/unhandled-promise-rejections/support/promise-rejection-events.js',
    'html/webappapis/scripting/processing-model-2/unhandled-promise-rejections/promise-rejection-event-constructor.html',
    'webmessaging/MessagePort_initial_disabled.any.js',
    'fetch/api/headers/headers-basic.any.js',
    'fetch/api/headers/headers-errors.any.js',
]);
const timeoutNames=Object.freeze([
    'delayed handling: a nested-task delay before attaching a handler causes unhandledrejection',
    'delayed handling: a nested-queueTask after promise creation/rejection, plus promise microtasks, is too late to attach a rejection handler',
    'delayed handling: a nested-queueTask before promise creation/rejection, plus many promise microtasks, is too late to attach a rejection handler',
    'delayed handling: a nested-queueTask after promise creation/rejection, plus many promise microtasks, is too late to attach a rejection handler',
]);
const referenceFields=['release_tag','source_commit','binary_sha256','compatibility_date','compatibility_flags'];
const read=file=>JSON.parse(fs.readFileSync(file,'utf8'));
const fields=(value,names,label)=>assert.deepEqual(Object.keys(value).sort(),names.slice().sort(),label);
const nameOf=file=>'worker_wpt_'+file.replace(/[^A-Za-z0-9]/g,'_');
const digest=bytes=>createHash('sha256').update(bytes).digest('hex');

export function adaptCompleteWptProgram(bytes) {
    const text=bytes.toString('utf8');
    assert.deepEqual(Buffer.from(text,'utf8'),bytes,'lossless UTF-8 bundle');
    const tail=/\nexport\s*\{\s*(\w+)\s+as\s+default\s*\};\s*$/.exec(text);
    assert.ok(tail,'one generated esbuild export tail');
    const prefix=Buffer.from(text.slice(0,tail.index),'utf8');
    const program=Buffer.concat([Buffer.from('export default { async fetch() {\n'),prefix,
        Buffer.from('\nreturn await '+tail[1]+'.fetch();\n}};\n')]);
    assert.deepEqual(program.subarray(Buffer.byteLength('export default { async fetch() {\n'),
        Buffer.byteLength('export default { async fetch() {\n')+prefix.length),prefix);
    return {program,bodyPrefixSHA256:digest(prefix),adaptedSHA256:digest(program)};
}

export function auditV2Metadata(profile,manifest,lock) {
    fields(profile,['schema','id','references','wpt','scheduling','standardsConformanceClaimed','conflicts'],'profile fields');
    assert.equal(profile.schema,'capsid-workerd-wpt-profile-v2');
    assert.equal(profile.id,`CAPSID-WORKERD-${lock.release_tag}-default-v2`);
    fields(profile.references,['linux','win32','darwin-x64','darwin-arm64'],'all supported native reference identities');
    const platform=referencePlatform(lock);
    const referenceKey=platform==='darwin' ? platform+'-'+referenceArchitecture(lock) : platform;
    assert.deepEqual(profile.references[referenceKey],Object.fromEntries(referenceFields.map(key=>[key,lock[key]])),
        'the selected OS and architecture must match its exact locked executable');
    assert.deepEqual(profile.wpt,{profile:manifest.profile,commit:manifest.commit,
        diagnosticMatch:'first-line-exact-with-one-explicit-engine-message-pair',standardFailuresRetained:true});
    assert.equal(profile.scheduling,'locked-workerd-promise-microtasks');
    assert.equal(profile.standardsConformanceClaimed,false);
    assert.deepEqual(profile.conflicts.map(c=>c.path),profileV2Paths);
    assert.deepEqual(profile.conflicts.map(c=>c.total),[36,1,1,23,18]);
    assert.deepEqual(profile.conflicts.map(c=>c.failures.length),[6,1,1,5,10]);
    assert.deepEqual(profile.conflicts[0].failures.slice(0,2).map(f=>f.name),guardedRejectionTests);
    assert.deepEqual(profile.conflicts[0].failures.slice(2).map(f=>f.name),timeoutNames);
    let enginePairs=0;
    for(const [index,conflict] of profile.conflicts.entries()) {
        fields(conflict,['path','upstreamSHA256','total','failures','missingExpectedFailures'],'conflict fields');
        assert.ok(manifest.executedProfile.includes(conflict.path),'retained upstream file');
        assert.match(conflict.upstreamSHA256,/^[a-f0-9]{64}$/);
        assert.equal(new Set(conflict.failures.map(f=>f.name)).size,conflict.failures.length);
        const historical=manifest.expectedFailures.filter(f=>f.path===conflict.path)
            .map(({subtest:name,path:ignored,...rest})=>({name,...rest}));
        assert.deepEqual(conflict.missingExpectedFailures,historical,'historical expectation stays visible');
        for(const [failureIndex,failure] of conflict.failures.entries()) {
            const paired=Object.hasOwn(failure,'referenceMessage');
            fields(failure,['name','status','errorName','phase','message',...(paired?['referenceMessage']:[])],'failure fields');
            assert.ok(typeof failure.name==='string' && failure.name.length>0);
            assert.ok(typeof failure.message==='string' && failure.message.length>0 && !/[\r\n]/.test(failure.message));
            if(index===0 && failureIndex>=2) {
                assert.deepEqual({status:failure.status,errorName:failure.errorName,phase:failure.phase,message:failure.message},
                    {status:'TIMEOUT',errorName:'TimeoutError',phase:'timeout',message:'test timed out'});
            } else {
                assert.equal(failure.status,'FAIL');assert.equal(failure.phase,'test');
                assert.ok(['AssertionError','TypeError'].includes(failure.errorName));
            }
            if(paired) {
                assert.equal(index,0);assert.equal(failureIndex,1);++enginePairs;
                assert.equal(failure.message,"cannot read property 'name' of undefined");
                assert.equal(failure.referenceMessage,"Cannot read properties of undefined (reading 'name')");
            }
        }
    }
    assert.equal(enginePairs,1,'only the observed V8/QuickJS TypeError diagnostic pair');
    return {profile:profile.id,files:5,totalSubtests:79,standardConflictSubtests:23,standardTimeouts:4};
}

export function auditV2Report(contract,manifest,report,mode) {
    fields(report,['commit','expectedFailures','failures','missingExpectedFailures','passed','profile','ranNothing','source','total'],'complete report fields');
    assert.equal(report.profile,manifest.profile);assert.equal(report.source,'web-platform-tests/wpt');
    assert.equal(report.commit,manifest.commit);assert.equal(report.ranNothing,false);
    assert.equal(report.passed,false,'original standard result remains a failure');
    assert.equal(report.total,contract.total,'entire upstream file executes');
    assert.deepEqual(report.expectedFailures,[],'no new WPT waiver');
    assert.deepEqual(report.missingExpectedFailures,contract.missingExpectedFailures);
    const expected=contract.failures.map(({referenceMessage,...failure})=>
        ({...failure,message:mode==='reference' && referenceMessage ? referenceMessage : failure.message}));
    assert.deepEqual(report.failures.map(f=>{
        fields(f,['name','status','errorName','phase','message'],'exact raw failure fields');
        assert.equal(typeof f.message,'string');return {...f,message:f.message.split('\n')[0]};
    }),expected,'exact names, statuses, phases, error classes and diagnostic lines');
}

export function auditV2Comparison(profile,manifest,lock,cases,witnesses) {
    const metadata=auditV2Metadata(profile,manifest,lock);
    const modes=['reference',...referenceBackendModes(lock)];
    assert.deepEqual(witnesses.map(w=>w.mode),modes,'actual independent API witnesses for all platform modes');
    for(const witness of witnesses) {
        checkWitness(witness.events,witness.mode);
        assertMessagePortObservation(witness.messagePort,witness.mode);
    }
    assert.deepEqual(cases.map(c=>c.path),profileV2Paths,'all five complete common-context files');
    for(const [index,row] of cases.entries()) {
        assert.deepEqual(row.roles.map(r=>r.mode),modes,'actual reference and every supported Host mode');
        for(const role of row.roles) auditV2Report(profile.conflicts[index],manifest,role.report,role.mode);
    }
    return {schema:'capsid-workerd-wpt-profile-audit-v2',status:'profile-matched-with-standard-failures',complete:true,
        ...metadata,standardsConformanceClaimed:false,standardCTestFilesPassed:false,witnessModes:modes,
        conflicts:cases.map(c=>({path:c.path,standardPassed:false,roles:c.roles}))};
}

function main() {
    const command=process.argv[2];
    assert.ok(['metadata','audit'].includes(command));
    const profile=read(path.join(root,profileV2File)),manifest=read(path.join(root,'tests/wpt/manifest.json')),lock=readLock();
    if(command==='metadata') return console.log(JSON.stringify(auditV2Metadata(profile,manifest,lock)));
    const names=['last-test','junit','registration','gate','build','wpt-root','context','output','python'];
    const {values}=parseArgs({args:process.argv.slice(3),options:Object.fromEntries(names.map(n=>[n,{type:'string'}]))});
    for(const name of names.filter(n=>n!=='python')) assert.ok(values[name],'required --'+name);
    const destination=path.resolve(values.output);
    assert.equal(fs.existsSync(destination),false,'fresh v2 audit output required');
    const ext=referencePlatform(lock)==='win32'?'.exe':'';
    const host=path.join(values.build,'capsid-host'+ext),worker=path.join(values.build,'capsid-worker'+ext);
    const protectedFiles=[values['last-test'],values.junit,values.registration,values.context,host,worker,path.join(root,profileV2File),path.join(root,'tests/wpt/manifest.json')];
    assertAuditOutput(destination,protectedFiles,values.gate,values.build);
    fs.mkdirSync(path.dirname(destination),{recursive:true});
    fs.writeFileSync(destination,JSON.stringify({schema:'capsid-workerd-wpt-profile-audit-v2',complete:false,status:'running'})+'\n');
    try {
        const identities=Object.fromEntries(protectedFiles.map(f=>[path.resolve(f),fileDigest(f)]));
        const summary=read(path.join(values.gate,'summary.json'));
        assert.equal(summary.status,'passed');assert.equal(summary.complete,true);
        const expected=read(path.join(summary.run,'expected-identities.json'));
        assert.deepEqual(expected.lock,lock);assert.equal(fileDigest(host),expected.host);assert.equal(fileDigest(worker),expected.worker);
        assert.deepEqual(expected.fixtures,fixtureIdentities(),'current independent API fixture bytes');
        assert.deepEqual(expected.messagePortInputs,Object.fromEntries(Object.entries(messagePortInputPaths).map(([key,file])=>[key,fileDigest(path.join(root,file))])));
        assert.equal(expected.messagePortNode,fileDigest(process.execPath));
        const gate=validateGateRun(summary.run,expected);assert.ok(gate.complete);
        const nativeModes=referenceBackendModes(lock),modes=['reference',...nativeModes];
        const witnesses=modes.map(mode=>{
            const api=read(path.join(summary.run,mode+'.json')).results.filter(r=>r.fixture==='api-compat.mjs');assert.equal(api.length,1);
            const port=read(path.join(summary.run,'message-port-order-'+(mode==='reference'?nativeModes[0]:mode)+'.json'));
            return {mode,events:api[0].observation,messagePort:port.roles[mode==='reference'?'reference':'native'].public};
        });
        const context=read(values.context);
        assert.equal(context.schema,'capsid-workerd-wpt-context-v2');assert.equal(context.complete,true);
        assert.deepEqual(context.referenceLock,lock);
        assert.deepEqual(context.binaries,{host:expected.host,worker:expected.worker,reference:lock.binary_sha256,node:fileDigest(process.execPath)});
        assert.equal(context.requestDeadlineMilliseconds,15000);assert.equal(context.wptInternalBudgetModified,false);
        assert.ok(Object.keys(context.measuredInputs).length>10);
        for(const [file,hash] of Object.entries(context.measuredInputs)) assert.equal(fileDigest(file),hash,'current context input: '+file);
        assert.equal(execFileSync('git',['-C',values['wpt-root'],'rev-parse','HEAD'],{encoding:'utf8'}).trim(),manifest.commit);
        for(const conflict of profile.conflicts) assert.equal(fileDigest(path.join(values['wpt-root'],conflict.path)),conflict.upstreamSHA256);
        for(const row of context.cases) for(const role of row.roles) {
            assert.equal(role.complete,true);assert.equal(role.httpStatus,200);
            assert.equal(fileDigest(role.bundle),row.adaptedSHA256,'same complete program in each real runtime');
            assert.equal(role.bundleSHA256,row.adaptedSHA256);
            assert.doesNotMatch(role.stderr,/AddressSanitizer|LeakSanitizer|runtime error:/);
            if(role.mode==='reference') {assert.equal(role.cleanupRequested,true);assert.equal(role.forced,false);}
            else assert.deepEqual(role.shutdown,{code:0,signal:null,forced:false});
        }
        const registration=read(values.registration);
        assert.deepEqual(registration,read(path.join(summary.run,'registration.json')),'same complete required-gate build inventory');
        const python=values.python??process.env.CAPSID_WS_INTEROP_PYTHON??'python3';
        const code='import json,sys,xml.etree.ElementTree as E\nr=E.parse(sys.argv[1]).getroot()\nprint(json.dumps([{"name":c.get("name"),"status":"failed" if c.find("failure") is not None or c.find("error") is not None else "skipped" if c.find("skipped") is not None else "passed"} for c in r.iter("testcase")]))';
        const junit=JSON.parse(execFileSync(python,['-I','-c',code,values.junit],{encoding:'utf8'}));
        const ordinary=readFullStandardEvidence(fs.readFileSync(values['last-test'],'utf8'),registration,junit,profile);
        for(const [index,row] of ordinary.entries()) {
            assert.equal(fs.realpathSync(row.command[1]),fs.realpathSync(worker));
            auditV2Report(profile.conflicts[index],manifest,row.report,'native');
            const captured=context.cases[index];
            assert.equal(fs.realpathSync(row.command[2]),fs.realpathSync(captured.originalBundle));
            assert.equal(fileDigest(row.command[2]),captured.originalSHA256,'same raw ordinary WPT program');
            const adapted=adaptCompleteWptProgram(fs.readFileSync(row.command[2]));
            assert.equal(captured.bodyPrefixSHA256,adapted.bodyPrefixSHA256,'every original body byte preserved');
            assert.equal(captured.adaptedSHA256,adapted.adaptedSHA256,'only the request wrapper and export tail change');
        }
        assert.deepEqual(junit.filter(c=>c.status==='failed').map(c=>c.name).sort(),profileV2Paths.map(nameOf).sort(),'no additional full-suite failures accepted');
        const audit=auditV2Comparison(profile,manifest,lock,context.cases,witnesses);
        for(const [file,hash] of Object.entries(identities)) assert.equal(fileDigest(file),hash);
        fs.writeFileSync(destination,JSON.stringify({...audit,fullSuite:{selected:junit.length,ordinarySuitePassed:false,skipped:junit.filter(c=>c.status==='skipped').map(c=>c.name)},gate:{run:summary.run,tests:gate.tests},inputSHA256:identities},null,2)+'\n');
        console.log(JSON.stringify({status:audit.status,files:5,standardConflictSubtests:23,standardTimeouts:4,ordinarySuitePassed:false}));
    } catch(error) {
        fs.writeFileSync(destination,JSON.stringify({schema:'capsid-workerd-wpt-profile-audit-v2',complete:false,status:'failed',error:String(error.stack??error)},null,2)+'\n');throw error;
    }
}
if(process.argv[1] && fileURLToPath(import.meta.url)===path.resolve(process.argv[1])) main();
