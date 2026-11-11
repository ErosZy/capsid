import assert from 'node:assert/strict';
import fs from 'node:fs';
import {test} from 'node:test';
import {auditV2Metadata,auditV2Comparison,adaptCompleteWptProgram} from '../tools/workerd-wpt-profile-v2.mjs';
import {checkOrdinaryMatrix,checkCompleteCompatibility,ordinaryWorkerdFailures} from '../tools/workerd-wpt-acceptance.mjs';
import {assertReleaseAcceptance} from '../tools/replay-workerd-release-acceptance.mjs';
import {requiredTestsFor} from '../tools/run-workerd-ws-gate.mjs';

const read=file=>JSON.parse(fs.readFileSync(new URL(file,import.meta.url),'utf8'));
const originalProfile=read('./wpt/workerd-default-profile-v2.json'),originalManifest=read('./wpt/manifest.json');
const linuxLock=read('./fixtures/workerd-ws/reference-lock.json'),windowsLock=read('./fixtures/workerd-ws/reference-lock-windows.json');
const portObservation=read('./fixtures/message-port-order-reference.json').public;
function fixture(windows=false) {
    const profile=structuredClone(originalProfile),manifest=structuredClone(originalManifest),lock=structuredClone(windows?windowsLock:linuxLock);
    const modes=['reference','single-worker','static-pool',...(windows?[]:['managed'])];
    const queue={beforeStart:['first','second'],immediatelyAfterStart:['first','second'],received:['first','second','third']};
    const events={runtimeEvents:{handled:{type:'rejectionhandled',trusted:true,phase:2,target:true,currentTarget:true,
        tag:'[object PromiseRejectionEvent]',promiseIdentity:true,reasonIdentity:false,reasonType:'undefined'},unhandled:{identity:true}},
        listenerEvents:{'MessagePort/capture-add':{ok:true,value:{errors:['TypeError','TypeError'],calls:0}}},
        messagePortQueue:{explicit:queue,handler:structuredClone(queue),nullHandler:structuredClone(queue)}};
    const cases=profile.conflicts.map(contract=>({path:contract.path,roles:modes.map(mode=>({mode,report:{
        profile:manifest.profile,source:'web-platform-tests/wpt',commit:manifest.commit,passed:false,ranNothing:false,total:contract.total,
        expectedFailures:[],missingExpectedFailures:structuredClone(contract.missingExpectedFailures),
        failures:contract.failures.map(({referenceMessage,...failure})=>({...failure,
            message:(mode==='reference'&&referenceMessage?referenceMessage:failure.message)+'\n    at original-upstream:1'})),
    }}))}));
    const witnesses=modes.map(mode=>({mode,events:structuredClone(events),messagePort:structuredClone(portObservation)}));
    return {profile,manifest,lock,cases,witnesses};
}
const audit=f=>auditV2Comparison(f.profile,f.manifest,f.lock,f.cases,f.witnesses);

for(const windows of [false,true]) test('complete '+(windows?'Windows':'Linux')+' profile comparison retains all23 standard failures and four timeouts',()=>{
    const f=fixture(windows),before=structuredClone(f),report=audit(f);
    assert.deepEqual(f,before,'the compatibility audit cannot rewrite raw reports');
    assert.equal(report.complete,true);assert.equal(report.files,5);assert.equal(report.totalSubtests,79);
    assert.equal(report.standardConflictSubtests,23);assert.equal(report.standardTimeouts,4);
    assert.equal(report.standardsConformanceClaimed,false);assert.equal(report.standardCTestFilesPassed,false);
    assert.ok(report.conflicts[0].roles[0].report.failures[2].message.includes('original-upstream'));
});

// Metadata and synthetic comparison controls do not qualify a real Darwin run.
// The archived native gate and complete common-context programs remain required.
for(const [architecture,expectedDigest] of [
    ['x64','9be36f0b9e867ffe081415beebf075f8fd2d5c27bc0a9e239ded1375bd09f27f'],
    ['arm64','2995714d79e9af6a0810eb7705a1f994b8ed977a266cb8976662027107761401'],
]) {
    const lock=read('./fixtures/workerd-ws/reference-lock-darwin-'+architecture+'.json');
    const key='darwin-'+architecture;
    test('Darwin '+architecture+' metadata binds the independently pinned executable',()=>{
        const before=structuredClone(originalProfile);
        assert.equal(lock.binary_sha256,expectedDigest);
        assert.equal(auditV2Metadata(originalProfile,originalManifest,lock).files,5);
        assert.deepEqual(originalProfile,before);
    });
    test('Darwin '+architecture+' comparison requires both supported host modes and retains standard failures',()=>{
        const f=fixture(true);f.lock=structuredClone(lock);
        const result=audit(f);
        assert.deepEqual(result.witnessModes,['reference','single-worker','static-pool']);
        assert.equal(result.standardConflictSubtests,23);assert.equal(result.standardTimeouts,4);
        assert.equal(result.standardsConformanceClaimed,false);assert.equal(result.standardCTestFilesPassed,false);
    });
    for(const [name,change] of [
        ['wrong binary',p=>{p.references[key].binary_sha256='0'.repeat(64);}],
        ['different source',p=>{p.references[key].source_commit='0'.repeat(40);}],
        ['different date',p=>{p.references[key].compatibility_date='2026-08-21';}],
        ['different flags',p=>{p.references[key].compatibility_flags.push('other_flag');}],
        ['missing architecture',p=>{delete p.references[key];}],
        ['substituted architecture',p=>{p.references[key]=structuredClone(p.references['darwin-'+(architecture==='x64'?'arm64':'x64')]);}],
        ['missing existing platform',p=>{delete p.references.linux;}],
        ['extra platform',p=>{p.references.other=structuredClone(p.references[key]);}],
    ]) test('Darwin '+architecture+' metadata rejects '+name,()=>{
        const profile=structuredClone(originalProfile);change(profile);
        assert.throws(()=>auditV2Metadata(profile,originalManifest,lock));
    });
}

const failures=f=>f.cases[0].roles[1].report.failures;
for(const [name,mutate] of [
    ['old profile schema',f=>{f.profile.schema='capsid-workerd-wpt-profile-v1';}],
    ['wrong reference digest',f=>{f.profile.references.linux.binary_sha256='0'.repeat(64);}],
    ['different compatibility flags',f=>{f.lock.compatibility_flags.push('new_flags');}],
    ['unknown profile file',f=>{f.profile.conflicts[4].path='fetch/unknown.any.js';}],
    ['extra declared failure',f=>{f.profile.conflicts[3].failures.push(structuredClone(f.profile.conflicts[3].failures[0]));}],
    ['unknown timeout name',f=>{f.profile.conflicts[0].failures[2].name+=' unknown';}],
    ['cleanup classified as scheduling timeout',f=>{f.profile.conflicts[0].failures[2].phase='cleanup';}],
    ['second engine diagnostic pair',f=>{f.profile.conflicts[1].failures[0].referenceMessage='another message';}],
    ['empty file checksum',f=>{f.profile.conflicts[2].upstreamSHA256='';}],
    ['waiver substituted for missing historical expectation',f=>{f.profile.conflicts[2].missingExpectedFailures=[];}],
    ['missing complete file report',f=>{f.cases.pop();}],
    ['missing native backend',f=>{f.cases[2].roles.pop();}],
    ['reference silently omitted',f=>{f.cases[1].roles.shift();}],
    ['mode order changed',f=>{f.cases[0].roles.reverse();}],
    ['extra failure',f=>{failures(f).push(structuredClone(failures(f)[0]));}],
    ['missing failure',f=>{failures(f).pop();}],
    ['raw standard failure relabeled passed',f=>{f.cases[0].roles[1].report.passed=true;}],
    ['empty execution',f=>{f.cases[0].roles[1].report.ranNothing=true;}],
    ['selected subset total',f=>{f.cases[0].roles[1].report.total=2;}],
    ['wrong upstream revision',f=>{f.cases[0].roles[1].report.commit='f'.repeat(40);}],
    ['added standard waiver',f=>{f.cases[0].roles[1].report.expectedFailures.push({name:'waived'});}],
    ['wrong native diagnostic',f=>{failures(f)[1].message='unreviewed TypeError';}],
    ['V8 diagnostic accepted as native QuickJS',f=>{failures(f)[1].message=f.profile.conflicts[0].failures[1].referenceMessage;}],
    ['QuickJS diagnostic accepted as V8',f=>{f.cases[0].roles[0].report.failures[1].message=f.profile.conflicts[0].failures[1].message;}],
    ['unexpected timeout',f=>{failures(f)[0].status='TIMEOUT';failures(f)[0].phase='timeout';}],
    ['known timeout changed to assertion failure',f=>{failures(f)[2].status='FAIL';failures(f)[2].phase='test';}],
    ['known timeout error class changed',f=>{failures(f)[2].errorName='Error';}],
    ['known timeout cleanup failure',f=>{failures(f)[2].phase='cleanup';}],
    ['unknown report field',f=>{f.cases[0].roles[1].report.filtered=true;}],
    ['missing independent scheduling witness',f=>{f.witnesses.pop();}],
    ['task scheduling substituted for microtasks',f=>{f.witnesses[2].messagePort.promiseOrder=['promise/before','promise/after','port','port/promise'];}],
    ['nested rejection events changed',f=>{f.witnesses[1].messagePort.nestedRejection.events.push({type:'unhandledrejection'});}],
    ['global Promise override affects delivery',f=>{f.witnesses[0].messagePort.intrinsic.error={name:'Error',message:'override'};}],
    ['missing handled event',f=>{f.witnesses[3].events.runtimeEvents.handled=null;}],
    ['capture listener invoked',f=>{f.witnesses[0].events.listenerEvents['MessagePort/capture-add'].value.calls=1;}],
    ['lost queue delivery',f=>{f.witnesses[1].events.messagePortQueue.explicit.received.pop();}],
]) test('v2 comparison rejects '+name,()=>{const f=fixture();mutate(f);assert.throws(()=>audit(f));});

test('only request scope and the generated export tail change in a complete WPT program',()=>{
    const body='// π\nconst exact = "all original assertions";\nthrow new Error(exact);\nconst application={fetch(){return 1;}};';
    const raw=Buffer.from(body+'\nexport { application as default };\n');
    const result=adaptCompleteWptProgram(raw);
    assert.equal(result.program.toString(),'export default { async fetch() {\n'+body+'\nreturn await application.fetch();\n}};\n');
    assert.throws(()=>adaptCompleteWptProgram(Buffer.from(body)));
    assert.throws(()=>adaptCompleteWptProgram(Buffer.from([255,254])));
    assert.equal(auditV2Metadata(originalProfile,originalManifest,windowsLock).files,5);
});

// Synthetic release-envelope controls only. A successful fixture never counts
// as a real ordinary full suite, reference gate or hosted release execution.
function acceptanceFixture() {
    const registration={tests:[...ordinaryWorkerdFailures,'native-resource-check','sandbox-root-probe'].map(name=>({name,properties:[]}))};
    const cases=registration.tests.map(({name})=>({name,status:ordinaryWorkerdFailures.includes(name)?'failed':'passed'}));
    const ordinary={exitText:'8\n',outcome:'failure',registration,cases};
    const raw=checkOrdinaryMatrix(ordinary);
    const report={...audit(fixture()),fullSuite:{selected:cases.length,ordinarySuitePassed:false,skipped:[]},
        gate:{run:'synthetic-gate-not-executed',tests:requiredTestsFor(linuxLock)},
        inputSHA256:Object.fromEntries(Array.from({length:8},(_,i)=>['synthetic-input-'+i,'a'.repeat(64)]))};
    return {ordinary,raw,report};
}

test('compatible release fixture preserves raw CTest failure and standard failures',()=>{
    const f=acceptanceFixture(),before=structuredClone(f);
    assert.equal(checkCompleteCompatibility(f.report,f.raw,f.ordinary.registration).compatibilityQualified,true);
    assert.deepEqual(f,before,'acceptance cannot rewrite raw cases or profile reports');
    assert.equal(f.raw.rawStepOutcome,'failure');assert.equal(f.raw.rawCTestExitCode,8);
    assert.equal(f.raw.ordinarySuitePassed,false);assert.equal(f.report.standardsConformanceClaimed,false);
});

for(const [name,change] of [
    ['success exit',f=>{f.ordinary.exitText='0\n';}],
    ['infrastructure exit',f=>{f.ordinary.exitText='1\n';}],
    ['timeout exit',f=>{f.ordinary.exitText='124\n';}],
    ['forged multiline exit',f=>{f.ordinary.exitText='8\n0\n';}],
    ['success step outcome',f=>{f.ordinary.outcome='success';}],
    ['skipped raw step',f=>{f.ordinary.outcome='skipped';}],
    ['cancelled raw step',f=>{f.ordinary.outcome='cancelled';}],
    ['empty registration',f=>{f.ordinary.registration.tests=[];}],
    ['missing actual case',f=>{f.ordinary.cases.pop();}],
    ['duplicate actual case',f=>{f.ordinary.cases.push(structuredClone(f.ordinary.cases[0]));}],
    ['duplicate registration',f=>{f.ordinary.registration.tests.push(structuredClone(f.ordinary.registration.tests[0]));}],
    ['unknown actual case',f=>{f.ordinary.cases[0].name='unexpected';}],
    ['additional native failure',f=>{f.ordinary.cases.find(r=>r.name==='native-resource-check').status='failed';}],
    ['missing known standard failure',f=>{f.ordinary.cases[0].status='passed';}],
    ['arbitrary native skip',f=>{f.ordinary.cases.find(r=>r.name==='native-resource-check').status='skipped';}],
    ['sandbox label without declared prerequisite skip',f=>{const t=f.ordinary.registration.tests.at(-1);t.properties=[{name:'LABELS',value:['sandbox']}];f.ordinary.cases.at(-1).status='skipped';}],
    ['sandbox regex skip',f=>{const t=f.ordinary.registration.tests.at(-1);t.properties=[{name:'LABELS',value:['sandbox']},{name:'SKIP_RETURN_CODE',value:77},{name:'SKIP_REGULAR_EXPRESSION',value:'anything'}];f.ordinary.cases.at(-1).status='skipped';}],
    ['disabled test',f=>{f.ordinary.registration.tests[0].properties=[{name:'DISABLED',value:true}];}],
    ['inverted failure',f=>{f.ordinary.registration.tests[0].properties=[{name:'WILL_FAIL',value:true}];}],
])test('release raw preflight rejects '+name,()=>{const f=acceptanceFixture();change(f);assert.throws(()=>checkOrdinaryMatrix(f.ordinary));});

test('ordinary declared sandbox prerequisite skip still requires privileged coverage',()=>{
    const f=acceptanceFixture();f.ordinary.registration.tests.at(-1).properties=[{name:'LABELS',value:['sandbox']},{name:'SKIP_RETURN_CODE',value:77}];
    f.ordinary.cases.at(-1).status='skipped';const raw=checkOrdinaryMatrix(f.ordinary);
    assert.deepEqual(raw.skipped,['sandbox-root-probe']);assert.equal(raw.requiresPrivilegedSandboxCoverage,true);
    f.report.fullSuite.skipped=raw.skipped;assert.equal(checkCompleteCompatibility(f.report,raw,f.ordinary.registration).compatibilityQualified,true);
});

for(const [name,change] of [
    ['incomplete profile',f=>{f.report.complete=false;}],
    ['running profile',f=>{f.report.status='running';}],
    ['wrong versioned profile',f=>{f.report.profile='older-profile';}],
    ['standards conformance claim',f=>{f.report.standardsConformanceClaimed=true;}],
    ['ordinary files marked passed',f=>{f.report.standardCTestFilesPassed=true;}],
    ['missing standard timeout',f=>{f.report.standardTimeouts=3;}],
    ['missing standard conflict',f=>{f.report.standardConflictSubtests=22;}],
    ['missing backend witness',f=>{f.report.witnessModes.pop();}],
    ['partial full suite',f=>{--f.report.fullSuite.selected;}],
    ['raw suite marked passed',f=>{f.report.fullSuite.ordinarySuitePassed=true;}],
    ['hidden sandbox skip',f=>{f.report.fullSuite.skipped=['sandbox-root-probe'];}],
    ['missing mandatory reference test',f=>{f.report.gate.tests.pop();}],
    ['replaced mandatory reference test',f=>{f.report.gate.tests[0]='unrelated-reference-test';}],
    ['missing input identities',f=>{f.report.inputSHA256={};}],
])test('release full-profile result rejects '+name,()=>{const f=acceptanceFixture();change(f);assert.throws(()=>checkCompleteCompatibility(f.report,f.raw,f.ordinary.registration));});

function replayReceiptFixture() {
    const f=acceptanceFixture();
    return {...f,record:{schema:'capsid-workerd-compatible-release-acceptance-v1',complete:true,
        status:'compatible-with-retained-standard-failures',compatibilityQualified:true,
        standardsConformanceClaimed:false,ordinarySuitePassed:false,rawCTestExitText:'8\n',rawStepOutcome:'failure',
        privilegedSandboxAndOtherHostedJobsStillMandatory:true,ordinary:structuredClone(f.raw),
        rawFullSuite:{selected:f.raw.selected,failed:[...f.raw.failed],skipped:[...f.raw.skipped]}}};
}
test('replay receipt must retain original failures and deferred privileged coverage',()=>{
    const f=replayReceiptFixture();assertReleaseAcceptance(f.record,f.raw,f.ordinary.registration,f.report);
    assert.deepEqual(f.record.rawFullSuite.failed,f.raw.failed);
});
for(const [name,change] of [
    ['wrong schema',r=>{r.schema='other';}],['incomplete',r=>{r.complete=false;}],
    ['running',r=>{r.status='running';}],['not qualified',r=>{r.compatibilityQualified=false;}],
    ['standard conformance claim',r=>{r.standardsConformanceClaimed=true;}],
    ['raw suite marked passed',r=>{r.ordinarySuitePassed=true;}],
    ['raw exit success',r=>{r.rawCTestExitText='0\n';}],['raw outcome success',r=>{r.rawStepOutcome='success';}],
    ['missing privileged requirement',r=>{r.privilegedSandboxAndOtherHostedJobsStillMandatory=false;}],
    ['partial ordinary inventory',r=>{--r.ordinary.selected;}],
    ['hidden ordinary failure',r=>{r.ordinary.failed.pop();}],
    ['hidden raw failure',r=>{r.rawFullSuite.failed.pop();}],
    ['invented raw skip',r=>{r.rawFullSuite.skipped.push('unexecuted-native');}],
])test('replay receipt rejects '+name,()=>{
    const f=replayReceiptFixture();change(f.record);
    assert.throws(()=>assertReleaseAcceptance(f.record,f.raw,f.ordinary.registration,f.report));
});
